/*
 * The unit's MIDI interface, with IOKit. See src/ua101_midiport.h.
 *
 * The USB calls and their order are ua101settings', which read the unit's settings many times,
 * also during a stream: the device is opened only to make sure the configuration is set and to
 * read the descriptor, then closed again; interface 2 is opened as it stands (alternate setting
 * 0, where IN is a bulk pipe); one read is always waiting on the IN pipe.
 *
 * The reads run on a thread of their own and are handed to the caller's queue, each with the
 * time it completed (on the unit since 2026-11-11, for the control app).
 */
#include "ua101_midiport.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>

#include <mach/mach_error.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ua101_desc.h"

typedef IOUSBDeviceInterface650 **device_ref;
typedef IOUSBInterfaceInterface650 **interface_ref;

#define WRITE_TIMEOUT_MS 250
#define INTERFACE_WAIT_TRIES 20
#define INTERFACE_WAIT_US 100000
#define READY_WAIT_SECONDS 2

struct ua101_midiport {
    dispatch_queue_t queue;
    ua101_midiport_events events;
    void *context;

    interface_ref intf;
    UInt8 pipe_in, pipe_out;
    UInt16 in_bytes;
    uint8_t *in_buf;
    CFRunLoopSourceRef source;

    pthread_t thread;
    int thread_running;
    CFRunLoopRef loop; /* the thread's, set before `ready` */
    dispatch_semaphore_t ready;
    atomic_int stopping;
    int closed;        /* only on `queue`: close was called, so no event may be passed on */
    _Atomic int refs;  /* the user, the thread, and each block on its way to `queue` */
};

static void note(FILE *log, const char *format, ...)
{
    va_list args;

    if (log == NULL)
        return;
    va_start(args, format);
    vfprintf(log, format, args);
    va_end(args);
    fflush(log);
}

static const char *describe(IOReturn result, char *text, size_t bytes)
{
    snprintf(text, bytes, "0x%08x, %s", (unsigned)result, mach_error_string(result));
    return text;
}

static void port_release(ua101_midiport *p)
{
    if (atomic_fetch_sub(&p->refs, 1) != 1)
        return;
    free(p->in_buf);
    dispatch_release(p->ready);
    dispatch_release(p->queue);
    free(p);
}

static void add_number(CFMutableDictionaryRef dict, CFStringRef key, int32_t value)
{
    CFNumberRef number = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &value);

    if (number != NULL) {
        CFDictionarySetValue(dict, key, number);
        CFRelease(number);
    }
}

static device_ref find_device(void)
{
    CFMutableDictionaryRef match = IOServiceMatching("IOUSBHostDevice");
    IOCFPlugInInterface **plugin = NULL;
    device_ref device = NULL;
    io_service_t service;
    SInt32 score = 0;

    if (match == NULL)
        return NULL;
    add_number(match, CFSTR("idVendor"), UA101_VENDOR_ID);
    add_number(match, CFSTR("idProduct"), UA101_PRODUCT_HIGH_SPEED);
    service = IOServiceGetMatchingService(kIOMainPortDefault, match);
    if (service == IO_OBJECT_NULL)
        return NULL;
    if (IOCreatePlugInInterfaceForService(service, kIOUSBDeviceUserClientTypeID,
                                          kIOCFPlugInInterfaceID, &plugin, &score) == KERN_SUCCESS &&
        plugin != NULL) {
        (*plugin)->QueryInterface(plugin, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID650),
                                  (LPVOID *)&device);
        IODestroyPlugInInterface(plugin);
    }
    IOObjectRelease(service);
    return device;
}

static interface_ref find_interface(device_ref device, UInt8 wanted)
{
    interface_ref found = NULL;
    int tries;

    /* macOS publishes interfaces shortly after the configuration is set. */
    for (tries = 0; found == NULL && tries < INTERFACE_WAIT_TRIES; tries++) {
        IOUSBFindInterfaceRequest request;
        io_iterator_t iterator = IO_OBJECT_NULL;
        io_service_t service;

        request.bInterfaceClass = kIOUSBFindInterfaceDontCare;
        request.bInterfaceSubClass = kIOUSBFindInterfaceDontCare;
        request.bInterfaceProtocol = kIOUSBFindInterfaceDontCare;
        request.bAlternateSetting = kIOUSBFindInterfaceDontCare;
        if ((*device)->CreateInterfaceIterator(device, &request, &iterator) == kIOReturnSuccess) {
            while ((service = IOIteratorNext(iterator)) != IO_OBJECT_NULL) {
                if (found == NULL) {
                    IOCFPlugInInterface **plugin = NULL;
                    interface_ref intf = NULL;
                    SInt32 score = 0;
                    UInt8 number = 0xFF;

                    if (IOCreatePlugInInterfaceForService(service, kIOUSBInterfaceUserClientTypeID,
                                                          kIOCFPlugInInterfaceID, &plugin,
                                                          &score) == KERN_SUCCESS &&
                        plugin != NULL) {
                        (*plugin)->QueryInterface(plugin,
                                                  CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID650),
                                                  (LPVOID *)&intf);
                        IODestroyPlugInInterface(plugin);
                    }
                    if (intf != NULL) {
                        (*intf)->GetInterfaceNumber(intf, &number);
                        if (number == wanted)
                            found = intf;
                        else
                            (*intf)->Release(intf);
                    }
                }
                IOObjectRelease(service);
            }
            IOObjectRelease(iterator);
        }
        if (found == NULL)
            usleep(INTERFACE_WAIT_US);
    }
    return found;
}

/* Reads the descriptors, and sets the configuration if nothing has yet. The device is closed
   again at once: only interface 2 is held. */
static int read_config(device_ref device, ua101_config *config, char *why, size_t why_bytes)
{
    IOUSBConfigurationDescriptorPtr descriptor = NULL;
    char text[128];
    IOReturn result;
    UInt8 current = 0;
    int ok = 0;

    result = (*device)->USBDeviceOpen(device);
    if (result != kIOReturnSuccess) {
        snprintf(why, why_bytes, "couldn't open the device (%s)%s",
                 describe(result, text, sizeof(text)),
                 result == kIOReturnExclusiveAccess ? ": another program has it open" : "");
        return 0;
    }
    (*device)->GetConfiguration(device, &current);
    if (current != 1) {
        result = (*device)->SetConfiguration(device, 1);
        if (result != kIOReturnSuccess) {
            snprintf(why, why_bytes, "couldn't set the configuration (%s)",
                     describe(result, text, sizeof(text)));
            goto done;
        }
    }
    result = (*device)->GetConfigurationDescriptorPtr(device, 0, &descriptor);
    if (result != kIOReturnSuccess || descriptor == NULL ||
        ua101_parse_config((const uint8_t *)descriptor, descriptor->wTotalLength, config) !=
            UA101_PARSE_OK) {
        snprintf(why, why_bytes, "couldn't read the configuration descriptor");
        goto done;
    }
    ok = 1;
done:
    (*device)->USBDeviceClose(device);
    return ok;
}

static int find_pipes(ua101_midiport *p, const ua101_alt *alt)
{
    UInt8 count = 0, pipe;
    uint8_t e;

    (*p->intf)->GetNumEndpoints(p->intf, &count);
    for (pipe = 1; pipe <= count; pipe++) {
        UInt8 direction = 0, number = 0, type = 0, interval = 0;
        UInt16 max_packet = 0;

        if ((*p->intf)->GetPipeProperties(p->intf, pipe, &direction, &number, &type, &max_packet,
                                          &interval) != kIOReturnSuccess)
            continue;
        for (e = 0; e < alt->num_endpoints; e++) {
            const ua101_endpoint *ep = &alt->endpoints[e];

            if ((ep->address & 0x0F) != number || type != kUSBBulk)
                continue;
            if (direction == kUSBIn && ua101_endpoint_is_in(ep)) {
                p->pipe_in = pipe;
                p->in_bytes = max_packet;
            } else if (direction == kUSBOut && !ua101_endpoint_is_in(ep)) {
                p->pipe_out = pipe;
            }
        }
    }
    return p->pipe_in != 0 && p->pipe_out != 0 && p->in_bytes >= 4;
}

/* --- The reading thread. --- */

static void start_read(ua101_midiport *p);

static void report_lost(ua101_midiport *p, IOReturn result)
{
    atomic_fetch_add(&p->refs, 1);
    dispatch_async(p->queue, ^{
      char text[128];

      if (!p->closed && p->events.lost != NULL)
          p->events.lost(p->context, describe(result, text, sizeof(text)));
      port_release(p);
    });
}

static void read_done(void *refcon, IOReturn result, void *arg0)
{
    ua101_midiport *p = refcon;
    size_t bytes = (size_t)(uintptr_t)arg0;
    /* The time stamp MIDI IN's bytes travel with: taken here, on the thread the read
       completes on, not when the service's queue gets to them. */
    uint64_t when = mach_absolute_time();

    if (atomic_load(&p->stopping) || result == kIOReturnAborted)
        return;
    if (result != kIOReturnSuccess && result != kIOReturnUnderrun) {
        report_lost(p, result);
        return;
    }
    if (bytes > p->in_bytes)
        bytes = p->in_bytes;
    bytes -= bytes % 4;
    if (bytes != 0) {
        uint8_t *copy = malloc(bytes);

        if (copy != NULL) {
            memcpy(copy, p->in_buf, bytes);
            atomic_fetch_add(&p->refs, 1);
            dispatch_async(p->queue, ^{
              if (!p->closed && p->events.packets != NULL)
                  p->events.packets(p->context, copy, bytes, when);
              free(copy);
              port_release(p);
            });
        }
    }
    start_read(p);
}

static void start_read(ua101_midiport *p)
{
    IOReturn result =
        (*p->intf)->ReadPipeAsync(p->intf, p->pipe_in, p->in_buf, p->in_bytes, read_done, p);

    if (result != kIOReturnSuccess && !atomic_load(&p->stopping))
        report_lost(p, result);
}

static void *port_thread(void *arg)
{
    ua101_midiport *p = arg;

    pthread_setname_np("ua101d midi");
    p->loop = CFRunLoopGetCurrent();
    CFRunLoopAddSource(p->loop, p->source, kCFRunLoopDefaultMode);
    /* Listening starts before anything can be sent, so a quick reply can't be missed. */
    start_read(p);
    dispatch_semaphore_signal(p->ready);
    while (!atomic_load(&p->stopping))
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1.0e9, false);
    CFRunLoopRemoveSource(p->loop, p->source, kCFRunLoopDefaultMode);
    port_release(p);
    return NULL;
}

/* --- Opening and closing. --- */

ua101_midiport *ua101_midiport_open(dispatch_queue_t queue, const ua101_midiport_events *events,
                                    void *context, FILE *log, char *why, size_t why_bytes)
{
    ua101_midiport *p;
    ua101_config config;
    const ua101_alt *alt;
    device_ref device;
    char text[128];
    IOReturn result;
    int opened = 0;

    if (why_bytes > 0)
        why[0] = '\0';
    device = find_device();
    if (device == NULL) {
        snprintf(why, why_bytes, "no UA-101 in Hi-Speed mode was found");
        return NULL;
    }
    p = calloc(1, sizeof(*p));
    if (p == NULL) {
        snprintf(why, why_bytes, "out of memory");
        (*device)->Release(device);
        return NULL;
    }
    p->queue = queue;
    dispatch_retain(queue);
    p->events = *events;
    p->context = context;
    p->ready = dispatch_semaphore_create(0);
    atomic_store(&p->refs, 1);

    if (!read_config(device, &config, why, why_bytes))
        goto failed;
    alt = ua101_find_alt(&config, UA101_IF_MIDI, 0);
    if (alt == NULL) {
        snprintf(why, why_bytes, "the descriptors have no MIDI interface");
        goto failed;
    }
    p->intf = find_interface(device, UA101_IF_MIDI);
    if (p->intf == NULL) {
        snprintf(why, why_bytes, "couldn't find the MIDI interface");
        goto failed;
    }
    result = (*p->intf)->USBInterfaceOpen(p->intf);
    if (result != kIOReturnSuccess) {
        snprintf(why, why_bytes, "couldn't open the MIDI interface (%s)%s",
                 describe(result, text, sizeof(text)),
                 result == kIOReturnExclusiveAccess
                     ? ": another program is using it, so quit MIDI apps and UA-101 test tools"
                     : "");
        goto failed;
    }
    opened = 1;
    if (!find_pipes(p, alt)) {
        snprintf(why, why_bytes, "the MIDI interface doesn't have both bulk pipes");
        goto failed;
    }
    p->in_buf = calloc(1, p->in_bytes);
    result = (*p->intf)->CreateInterfaceAsyncEventSource(p->intf, &p->source);
    if (p->in_buf == NULL || result != kIOReturnSuccess) {
        snprintf(why, why_bytes, "couldn't create the event source (%s)",
                 describe(result, text, sizeof(text)));
        goto failed;
    }
    atomic_fetch_add(&p->refs, 1); /* the thread's */
    if (pthread_create(&p->thread, NULL, port_thread, p) != 0) {
        atomic_fetch_sub(&p->refs, 1);
        snprintf(why, why_bytes, "couldn't start the MIDI thread");
        goto failed;
    }
    p->thread_running = 1;
    dispatch_semaphore_wait(p->ready,
                            dispatch_time(DISPATCH_TIME_NOW, READY_WAIT_SECONDS * NSEC_PER_SEC));
    (*device)->Release(device);
    note(log, "  MIDI interface open: pipes in %u and out %u, %u bytes a read\n", p->pipe_in,
         p->pipe_out, p->in_bytes);
    return p;

failed:
    if (p->intf != NULL) {
        if (opened)
            (*p->intf)->USBInterfaceClose(p->intf);
        (*p->intf)->Release(p->intf);
    }
    (*device)->Release(device);
    port_release(p);
    return NULL;
}

int ua101_midiport_send(ua101_midiport *p, const uint8_t *packets, size_t bytes)
{
    if (p == NULL || p->closed || bytes == 0 || bytes % 4 != 0 || bytes > 4096)
        return 0;
    return (*p->intf)->WritePipeTO(p->intf, p->pipe_out, (void *)packets, (UInt32)bytes,
                                   WRITE_TIMEOUT_MS, WRITE_TIMEOUT_MS) == kIOReturnSuccess;
}

void ua101_midiport_close(ua101_midiport *p)
{
    if (p == NULL || p->closed)
        return;
    p->closed = 1;
    atomic_store(&p->stopping, 1);
    if (p->thread_running) {
        (*p->intf)->AbortPipe(p->intf, p->pipe_in);
        CFRunLoopStop(p->loop);
        pthread_join(p->thread, NULL);
        p->thread_running = 0;
    }
    (*p->intf)->USBInterfaceClose(p->intf);
    (*p->intf)->Release(p->intf);
    p->intf = NULL;
    port_release(p);
}
