/*
 * ua101midi: the UA-101's MIDI IN and OUT ports as "UA-101 MIDI" in CoreMIDI, for Stage 1.
 *
 * It opens only interface 2, the MIDI interface, and passes MIDI between it and a virtual
 * source and destination that other apps can use while it runs.
 *
 * What it sends to the unit: only what apps send to the "UA-101 MIDI" destination, packed by
 * ua101_midi.c, which can only use cable 0, the MIDI OUT port. Each transfer is checked again
 * before it goes, and refused if any packet names another cable. Cable 1, Roland's control
 * channel, is never written. The unit's own messages on cable 1 (the panel switches) are
 * counted and not passed to apps. With --receive-only there's no destination, so nothing at
 * all is sent. --self-test checks the CoreMIDI side without opening the unit.
 *
 * It installs nothing and changes no system setting. Run it without sudo, with no other
 * program using the UA-101's MIDI interface.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>

#include <mach/mach_error.h>
#include <mach/mach_time.h>
#include <os/lock.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ua101_desc.h"
#include "ua101_midi.h"

typedef IOUSBDeviceInterface650 **device_ref;
typedef IOUSBInterfaceInterface650 **interface_ref;

#define PORT_NAME "UA-101 MIDI"
#define INTERFACE_WAIT_TRIES 20
#define INTERFACE_WAIT_US 100000
/* USB-MIDI packets waiting for MIDI OUT. MIDI runs at 3,125 bytes a second, so this holds about
   15 s of SysEx; more than that is dropped and counted. */
#define OUT_RING_BYTES (64 * 1024)
#define TX_CHUNK_PACKETS 256
#define MAX_SHOWN 200
#define SELF_TEST_WAIT_SECONDS 2.0
#define MAX_SELF_TEST_WORDS 256

typedef struct {
    /* USB, used on the main thread only */
    interface_ref intf;
    UInt8 pipe_in;
    UInt8 pipe_out;
    UInt16 in_bytes;
    UInt16 out_bytes;
    uint8_t *in_buf;
    uint8_t *out_buf;
    int read_pending;
    int write_pending;
    int stopping;
    IOReturn usb_error;
    const char *fatal;
    CFRunLoopRef loop;
    CFRunLoopSourceRef wake;

    /* CoreMIDI */
    MIDIClientRef client;
    MIDIEndpointRef source;
    MIDIEndpointRef dest;
    int loopback; /* --self-test: packets go straight back in, with no USB */

    /* App to unit, on CoreMIDI's receive thread */
    ua101_midi_parser tx_parser;
    ua101_usbmidi_packer tx_packer;
    os_unfair_lock ring_lock;
    uint8_t ring[OUT_RING_BYTES];
    size_t ring_head;
    size_t ring_count;
    atomic_ullong tx_packets;
    atomic_ullong tx_refused;
    atomic_ullong tx_ring_full;
    atomic_ullong tx_wrong_cable;
    atomic_ullong usb_bytes_out;

    /* Unit to app: the main thread, or CoreMIDI's thread in --self-test */
    ua101_midi_parser rx_parser;
    ua101_ump_packer rx_packer;
    uint64_t rx_messages;
    uint64_t rx_malformed;
    uint64_t rx_control;
    uint64_t rx_other_cable;
    uint64_t rx_send_errors;
    uint64_t usb_bytes_in;
    int show;
    unsigned shown;
} session;

static session g_session;
static volatile sig_atomic_t g_signal;

static void on_signal(int number)
{
    g_signal = number;
}

static const char *describe(IOReturn result)
{
    static char text[128];

    snprintf(text, sizeof(text), "0x%08x, %s", (unsigned)result, mach_error_string(result));
    return text;
}

static int all_cable0(const uint8_t *packets, size_t bytes)
{
    size_t i;

    for (i = 0; i < bytes; i += UA101_MIDI_PACKET_BYTES)
        if ((packets[i] >> 4) != UA101_MIDI_CABLE)
            return 0;
    return 1;
}

/* ---- Unit to apps ---- */

/* One message per event list: SysEx must travel in packets of its own. */
static void deliver(session *s, const uint32_t *words, size_t count)
{
    MIDIEventList list;
    MIDIEventPacket *packet = MIDIEventListInit(&list, kMIDIProtocol_1_0);

    if (count == 0)
        return;
    packet = MIDIEventListAdd(&list, sizeof(list), packet, mach_absolute_time(), count, words);
    if (packet == NULL || MIDIReceivedEventList(s->source, &list) != noErr) {
        s->rx_send_errors++;
        return;
    }
    s->rx_messages++;
    if (s->show && s->shown < MAX_SHOWN) {
        s->shown++;
        printf("  in: %08x%s", words[0], count > 1 ? " " : "\n");
        if (count > 1)
            printf("%08x\n", words[1]);
    }
}

static void from_unit(session *s, const uint8_t *packets, size_t bytes)
{
    static const ua101_midi_event cut_event = {UA101_MIDI_SYSEX_CUT, {0}, 0};
    size_t i, j;

    for (i = 0; i + UA101_MIDI_PACKET_BYTES <= bytes; i += UA101_MIDI_PACKET_BYTES) {
        uint8_t cable, midi[3];
        int malformed;
        size_t len = ua101_usbmidi_unpack(packets + i, &cable, midi, &malformed);

        s->rx_malformed += (uint64_t)malformed;
        if (len == 0)
            continue;
        switch (ua101_midi_route_for(cable)) {
        case UA101_ROUTE_CONTROL:
            s->rx_control++;
            continue;
        case UA101_ROUTE_DROP:
            s->rx_other_cable++;
            continue;
        case UA101_ROUTE_APPS:
            break;
        }
        for (j = 0; j < len; j++) {
            ua101_midi_event ev;
            uint32_t words[2];
            int cut;

            ua101_midi_parse(&s->rx_parser, midi[j], &ev, &cut);
            if (cut)
                deliver(s, words, ua101_ump_pack(&s->rx_packer, &cut_event, words));
            deliver(s, words, ua101_ump_pack(&s->rx_packer, &ev, words));
        }
    }
}

/* ---- Apps to unit ---- */

/* Queues packets for the main thread to write, or loops them back in --self-test. */
static void to_unit(session *s, const uint8_t *packets, size_t bytes)
{
    if (bytes == 0)
        return;
    if (!all_cable0(packets, bytes)) {
        s->tx_wrong_cable++;
        return;
    }
    s->tx_packets += bytes / UA101_MIDI_PACKET_BYTES;
    if (s->loopback) {
        from_unit(s, packets, bytes);
        return;
    }
    os_unfair_lock_lock(&s->ring_lock);
    if (s->ring_count + bytes > OUT_RING_BYTES) {
        s->tx_ring_full += bytes / UA101_MIDI_PACKET_BYTES;
    } else {
        size_t tail = (s->ring_head + s->ring_count) % OUT_RING_BYTES;
        size_t first = OUT_RING_BYTES - tail < bytes ? OUT_RING_BYTES - tail : bytes;

        memcpy(s->ring + tail, packets, first);
        memcpy(s->ring, packets + first, bytes - first);
        s->ring_count += bytes;
    }
    os_unfair_lock_unlock(&s->ring_lock);
    CFRunLoopSourceSignal(s->wake);
    CFRunLoopWakeUp(s->loop);
}

/* Runs on CoreMIDI's receive thread for everything apps send to the destination. */
static void from_apps(session *s, const MIDIEventList *list)
{
    static const ua101_midi_event cut_event = {UA101_MIDI_SYSEX_CUT, {0}, 0};
    uint8_t packets[TX_CHUNK_PACKETS * UA101_MIDI_PACKET_BYTES];
    const MIDIEventPacket *packet = &list->packet[0];
    size_t used = 0;
    UInt32 p;

    if (list->protocol != kMIDIProtocol_1_0) {
        s->tx_refused += list->numPackets;
        return;
    }
    for (p = 0; p < list->numPackets; p++) {
        UInt32 w = 0;

        while (w < packet->wordCount) {
            size_t count = ua101_ump_words(packet->words[w]), n, i;
            uint8_t bytes[8];
            int refused;

            if (w + count > packet->wordCount) {
                s->tx_refused++;
                break;
            }
            n = ua101_ump_to_bytes(&packet->words[w], count, bytes, &refused);
            s->tx_refused += (uint64_t)refused;
            for (i = 0; i < n; i++) {
                ua101_midi_event ev;
                int cut;

                ua101_midi_parse(&s->tx_parser, bytes[i], &ev, &cut);
                if (cut)
                    ua101_usbmidi_pack(&s->tx_packer, &cut_event, packets + used);
                if (ua101_usbmidi_pack(&s->tx_packer, &ev, packets + used))
                    used += UA101_MIDI_PACKET_BYTES;
                if (used == sizeof(packets)) {
                    to_unit(s, packets, used);
                    used = 0;
                }
            }
            w += (UInt32)count;
        }
        packet = MIDIEventPacketNext(packet);
    }
    to_unit(s, packets, used);
}

/* ---- USB ---- */

static void start_write(session *s);

static void write_done(void *refcon, IOReturn result, void *arg0)
{
    session *s = refcon;

    s->write_pending = 0;
    if (s->stopping || result == kIOReturnAborted)
        return;
    if (result != kIOReturnSuccess) {
        s->usb_error = result;
        return;
    }
    s->usb_bytes_out += (uint64_t)(uintptr_t)arg0;
    start_write(s);
}

/* Writes one transfer of queued packets at a time, so MIDI OUT's pace sets the flow. */
static void start_write(session *s)
{
    size_t bytes, first;
    IOReturn result;

    if (s->write_pending || s->stopping || s->usb_error != 0 || s->fatal != NULL)
        return;
    os_unfair_lock_lock(&s->ring_lock);
    bytes = s->ring_count < s->out_bytes ? s->ring_count : s->out_bytes;
    bytes -= bytes % UA101_MIDI_PACKET_BYTES;
    first = OUT_RING_BYTES - s->ring_head < bytes ? OUT_RING_BYTES - s->ring_head : bytes;
    memcpy(s->out_buf, s->ring + s->ring_head, first);
    memcpy(s->out_buf + first, s->ring, bytes - first);
    s->ring_head = (s->ring_head + bytes) % OUT_RING_BYTES;
    s->ring_count -= bytes;
    os_unfair_lock_unlock(&s->ring_lock);
    if (bytes == 0)
        return;
    /* The last check before anything leaves: cable 0 only. */
    if (!all_cable0(s->out_buf, bytes)) {
        s->fatal = "a packet for another cable reached the write, so nothing was sent";
        return;
    }
    result = (*s->intf)->WritePipeAsync(s->intf, s->pipe_out, s->out_buf, (UInt32)bytes,
                                        write_done, s);
    if (result == kIOReturnSuccess)
        s->write_pending = 1;
    else
        s->usb_error = result;
}

static void wake_perform(void *info)
{
    start_write(info);
}

static void start_read(session *s);

static void read_done(void *refcon, IOReturn result, void *arg0)
{
    session *s = refcon;
    size_t bytes = (size_t)(uintptr_t)arg0;

    s->read_pending = 0;
    if (s->stopping || result == kIOReturnAborted)
        return;
    if (result != kIOReturnSuccess && result != kIOReturnUnderrun) {
        s->usb_error = result;
        return;
    }
    if (bytes > s->in_bytes)
        bytes = s->in_bytes;
    s->usb_bytes_in += bytes;
    from_unit(s, s->in_buf, bytes);
    start_read(s);
}

static void start_read(session *s)
{
    IOReturn result =
        (*s->intf)->ReadPipeAsync(s->intf, s->pipe_in, s->in_buf, s->in_bytes, read_done, s);

    if (result == kIOReturnSuccess)
        s->read_pending = 1;
    else
        s->usb_error = result;
}

static void add_number(CFMutableDictionaryRef dict, CFStringRef key, int32_t value)
{
    CFNumberRef number = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &value);

    if (number != NULL) {
        CFDictionarySetValue(dict, key, number);
        CFRelease(number);
    }
}

static io_service_t find_device(void)
{
    CFMutableDictionaryRef match = IOServiceMatching("IOUSBHostDevice");

    if (match == NULL)
        return IO_OBJECT_NULL;
    add_number(match, CFSTR("idVendor"), UA101_VENDOR_ID);
    add_number(match, CFSTR("idProduct"), UA101_PRODUCT_HIGH_SPEED);
    return IOServiceGetMatchingService(kIOMainPortDefault, match);
}

static device_ref open_device_object(io_service_t service)
{
    IOCFPlugInInterface **plugin = NULL;
    device_ref device = NULL;
    SInt32 score = 0;

    if (IOCreatePlugInInterfaceForService(service, kIOUSBDeviceUserClientTypeID,
                                          kIOCFPlugInInterfaceID, &plugin, &score) != KERN_SUCCESS ||
        plugin == NULL)
        return NULL;
    (*plugin)->QueryInterface(plugin, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID650),
                              (LPVOID *)&device);
    IODestroyPlugInInterface(plugin);
    return device;
}

static interface_ref open_interface_object(io_service_t service)
{
    IOCFPlugInInterface **plugin = NULL;
    interface_ref intf = NULL;
    SInt32 score = 0;

    if (IOCreatePlugInInterfaceForService(service, kIOUSBInterfaceUserClientTypeID,
                                          kIOCFPlugInInterfaceID, &plugin, &score) != KERN_SUCCESS ||
        plugin == NULL)
        return NULL;
    (*plugin)->QueryInterface(plugin, CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID650),
                              (LPVOID *)&intf);
    IODestroyPlugInInterface(plugin);
    return intf;
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
                    interface_ref intf = open_interface_object(service);
                    UInt8 number = 0xFF;

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

/* Reads the descriptors, sets the configuration if nothing has yet, and lets the device go. */
static int read_config(device_ref device, ua101_config *config)
{
    IOUSBConfigurationDescriptorPtr descriptor = NULL;
    IOReturn result;
    UInt8 current = 0;
    int ok = 0;

    result = (*device)->USBDeviceOpen(device);
    if (result != kIOReturnSuccess) {
        printf("Couldn't open the device: %s\n", describe(result));
        if (result == kIOReturnExclusiveAccess)
            printf("Another program has it open. Quit audio or MIDI apps and any UA-101 test.\n");
        return 0;
    }
    (*device)->GetConfiguration(device, &current);
    if (current != 1 && (result = (*device)->SetConfiguration(device, 1)) != kIOReturnSuccess) {
        printf("Couldn't set the USB configuration: %s\n", describe(result));
        goto done;
    }
    result = (*device)->GetConfigurationDescriptorPtr(device, 0, &descriptor);
    if (result != kIOReturnSuccess || descriptor == NULL) {
        printf("Couldn't read the configuration descriptor: %s\n", describe(result));
        goto done;
    }
    if (ua101_parse_config((const uint8_t *)descriptor, descriptor->wTotalLength, config) !=
        UA101_PARSE_OK) {
        printf("Couldn't parse the configuration descriptor.\n");
        goto done;
    }
    ok = 1;
done:
    (*device)->USBDeviceClose(device);
    return ok;
}

static int find_pipes(session *s, const ua101_alt *alt)
{
    UInt8 count = 0, pipe;
    uint8_t e;

    (*s->intf)->GetNumEndpoints(s->intf, &count);
    for (pipe = 1; pipe <= count; pipe++) {
        UInt8 direction = 0, number = 0, type = 0, interval = 0;
        UInt16 max_packet = 0;

        if ((*s->intf)->GetPipeProperties(s->intf, pipe, &direction, &number, &type, &max_packet,
                                          &interval) != kIOReturnSuccess)
            continue;
        for (e = 0; e < alt->num_endpoints; e++) {
            const ua101_endpoint *ep = &alt->endpoints[e];

            if ((ep->address & 0x0F) != number || type != kUSBBulk)
                continue;
            if (direction == kUSBIn && ua101_endpoint_is_in(ep)) {
                s->pipe_in = pipe;
                s->in_bytes = max_packet;
            } else if (direction == kUSBOut && !ua101_endpoint_is_in(ep)) {
                s->pipe_out = pipe;
                s->out_bytes = max_packet;
            }
        }
    }
    if (s->pipe_in == 0 || s->pipe_out == 0 || s->in_bytes < 4 || s->out_bytes < 4) {
        printf("  Couldn't find both bulk pipes on the MIDI interface.\n");
        return 0;
    }
    printf("  MIDI interface %u: bulk in %u bytes, bulk out %u bytes\n", UA101_IF_MIDI, s->in_bytes,
           s->out_bytes);
    return 1;
}

/* ---- CoreMIDI ---- */

/* Gives a published port its fixed ID, so apps remember it from one run to the next. If the
   ID is taken, it tries a few others; failing all, the port keeps CoreMIDI's random one. */
static void set_port_id(MIDIEndpointRef endpoint, const char *name, int is_destination)
{
    unsigned attempt;

    for (attempt = 0; attempt < 8; attempt++) {
        int32_t id = ua101_midi_port_id(name, is_destination, attempt);
        OSStatus status = MIDIObjectSetIntegerProperty(endpoint, kMIDIPropertyUniqueID, id);

        if (status == noErr) {
            printf("  %s ID %d%s\n", is_destination ? "Destination" : "Source", (int)id,
                   attempt == 0 ? "" : " (the usual one was taken)");
            return;
        }
        if (status != kMIDIIDNotUnique) {
            printf("  Couldn't set the %s ID: %d, so it keeps a random one\n",
                   is_destination ? "destination" : "source", (int)status);
            return;
        }
    }
    printf("  Every %s ID tried was taken, so it keeps a random one\n",
           is_destination ? "destination" : "source");
}

static int make_ports(session *s, const char *name, int with_dest, int private_ports)
{
    CFStringRef cf_name = CFStringCreateWithCString(NULL, name, kCFStringEncodingUTF8);
    OSStatus status;
    int ok = 0;

    status = MIDIClientCreateWithBlock(CFSTR("ua101midi"), &s->client, NULL);
    if (status != noErr) {
        printf("  Couldn't create a CoreMIDI client: %d\n", (int)status);
        goto done;
    }
    status = MIDISourceCreateWithProtocol(s->client, cf_name, kMIDIProtocol_1_0, &s->source);
    if (status != noErr) {
        printf("  Couldn't create the source: %d\n", (int)status);
        goto done;
    }
    if (private_ports)
        MIDIObjectSetIntegerProperty(s->source, kMIDIPropertyPrivate, 1);
    else
        set_port_id(s->source, name, 0);
    if (with_dest) {
        status = MIDIDestinationCreateWithProtocol(
            s->client, cf_name, kMIDIProtocol_1_0, &s->dest,
            ^(const MIDIEventList *list, void *ref) {
                (void)ref;
                from_apps(s, list);
            });
        if (status != noErr) {
            printf("  Couldn't create the destination: %d\n", (int)status);
            goto done;
        }
        if (private_ports)
            MIDIObjectSetIntegerProperty(s->dest, kMIDIPropertyPrivate, 1);
        else
            set_port_id(s->dest, name, 1);
    }
    ok = 1;
done:
    if (cf_name != NULL)
        CFRelease(cf_name);
    return ok;
}

static void dispose_ports(session *s)
{
    /* The destination goes first, so nothing new can arrive while the rest shuts down. */
    if (s->dest != 0)
        MIDIEndpointDispose(s->dest);
    s->dest = 0;
    if (s->source != 0)
        MIDIEndpointDispose(s->source);
    s->source = 0;
    if (s->client != 0)
        MIDIClientDispose(s->client);
    s->client = 0;
}

static void print_counts(const session *s)
{
    printf("\nTo the unit (MIDI OUT): %llu packets queued, %llu bytes written; refused %llu UMP "
           "packets, %llu packets dropped with the queue full, %llu for another cable\n",
           (unsigned long long)s->tx_packets, (unsigned long long)s->usb_bytes_out,
           (unsigned long long)s->tx_refused, (unsigned long long)s->tx_ring_full,
           (unsigned long long)s->tx_wrong_cable);
    printf("  Dropped: %llu bytes MIDI 1.0 doesn't allow, %llu SysEx cut short (%llu bytes)\n",
           (unsigned long long)s->tx_parser.dropped, (unsigned long long)s->tx_parser.sysex_cut,
           (unsigned long long)s->tx_packer.dropped);
    printf("From the unit (MIDI IN): %llu bytes read, %llu messages to apps; %llu control "
           "packets kept back (cable 1), %llu on other cables, %llu malformed, %llu not "
           "delivered\n",
           (unsigned long long)s->usb_bytes_in, (unsigned long long)s->rx_messages,
           (unsigned long long)s->rx_control, (unsigned long long)s->rx_other_cable,
           (unsigned long long)s->rx_malformed, (unsigned long long)s->rx_send_errors);
    printf("  Dropped: %llu bytes MIDI 1.0 doesn't allow, %llu SysEx cut short\n",
           (unsigned long long)s->rx_parser.dropped, (unsigned long long)s->rx_parser.sysex_cut);
}

/* ---- Message tests ---- */

/*
 * For --self-test, which never reaches a real port: each kind of message, and a SysEx shaped
 * like Roland's control messages, which must still be packed for cable 0.
 */
static const uint32_t k_self_test_words[] = {
    0x20903C64,             /* note on */
    0x20B00740,             /* control change */
    0x20C50300,             /* program change */
    0x20E10040,             /* pitch bend */
    0x10F80000,             /* clock */
    0x10F21020,             /* song position */
    0x10F60000,             /* tune request */
    0x30164110, 0x00691202, /* 14-byte SysEx: start, */
    0x30260000, 0x00000000, /* continue, */
    0x30320000, 0x00000000, /* end */
    0x20803C00,             /* note off */
};

/*
 * For --cable-test, which goes out of MIDI OUT for real. These would do nothing on an
 * instrument plugged in by mistake: note offs, a centred pitch bend, clock, and SysEx with
 * manufacturer ID 7D, which is set aside for non-commercial use.
 */
static const uint32_t k_cable_test_words[] = {
    0x20803C00,             /* note off */
    0x20903C00,             /* note on at velocity 0, which also means note off */
    0x20A03C00,             /* key pressure 0 */
    0x20E00040,             /* pitch bend, centred */
    0x10F80000,             /* clock */
    0x10F20000,             /* song position 0 */
    0x30167D01, 0x02030405, /* 14-byte SysEx: start, */
    0x30260607, 0x08090A0B, /* continue, */
    0x30320C0D, 0x00000000, /* end */
    0x20803C00,             /* note off */
};

typedef struct {
    os_unfair_lock lock;
    uint32_t words[MAX_SELF_TEST_WORDS];
    size_t count;
    uint64_t active_sensing;
    int overflow;
} collected;

static collected g_got;

/* Waits, running the main thread's USB callbacks, until enough words arrive or time is up. */
static void wait_for_words(size_t want, double seconds)
{
    CFAbsoluteTime deadline = CFAbsoluteTimeGetCurrent() + seconds;

    for (;;) {
        size_t count;

        os_unfair_lock_lock(&g_got.lock);
        count = g_got.count;
        os_unfair_lock_unlock(&g_got.lock);
        if (count >= want || CFAbsoluteTimeGetCurrent() > deadline || g_signal)
            return;
        if (CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, false) == kCFRunLoopRunFinished)
            usleep(10000);
    }
}

/*
 * Sends each message to a private destination, one per event list, and collects what arrives
 * at the private source: through USB and a MIDI cable for --cable-test, or straight back for
 * --self-test. What arrives must match what was sent. Active sensing is set aside, in case the
 * unit adds it on its own.
 */
static int message_test(session *s, const char *name, const uint32_t *words, size_t total)
{
    MIDIPortRef in_port = 0, out_port = 0;
    size_t i, count, messages = 0;
    OSStatus status;
    int pass = 0;

    g_got.lock = OS_UNFAIR_LOCK_INIT;
    if (!make_ports(s, name, 1, 1))
        goto done;
    status = MIDIInputPortCreateWithProtocol(
        s->client, CFSTR("test in"), kMIDIProtocol_1_0, &in_port,
        ^(const MIDIEventList *list, void *ref) {
            const MIDIEventPacket *packet = &list->packet[0];
            UInt32 p, w;

            (void)ref;
            os_unfair_lock_lock(&g_got.lock);
            for (p = 0; p < list->numPackets; p++) {
                for (w = 0; w < packet->wordCount; w += (UInt32)ua101_ump_words(packet->words[w])) {
                    size_t n = ua101_ump_words(packet->words[w]);

                    if ((packet->words[w] & 0xF0FF0000u) == 0x10FE0000u) {
                        g_got.active_sensing++;
                    } else if (w + n > packet->wordCount || g_got.count + n > MAX_SELF_TEST_WORDS) {
                        g_got.overflow = 1;
                        break;
                    } else {
                        memcpy(g_got.words + g_got.count, &packet->words[w], n * sizeof(uint32_t));
                        g_got.count += n;
                    }
                }
                packet = MIDIEventPacketNext(packet);
            }
            os_unfair_lock_unlock(&g_got.lock);
        });
    if (status != noErr || MIDIPortConnectSource(in_port, s->source, NULL) != noErr) {
        printf("  Couldn't listen to the source: %d\n", (int)status);
        goto done;
    }
    if (MIDIOutputPortCreate(s->client, CFSTR("test out"), &out_port) != noErr) {
        printf("  Couldn't create an output port.\n");
        goto done;
    }

    for (i = 0; i < total; i += count) {
        MIDIEventList list;
        MIDIEventPacket *packet = MIDIEventListInit(&list, kMIDIProtocol_1_0);

        count = ua101_ump_words(words[i]);
        packet = MIDIEventListAdd(&list, sizeof(list), packet, 0, count, words + i);
        if (packet == NULL || MIDISendEventList(out_port, s->dest, &list) != noErr) {
            printf("  Couldn't send message %zu.\n", messages + 1);
            goto done;
        }
        messages++;
    }
    wait_for_words(total, SELF_TEST_WAIT_SECONDS);
    wait_for_words(SIZE_MAX, 0.1); /* anything extra would arrive now */

    os_unfair_lock_lock(&g_got.lock);
    printf("  Sent %zu messages, %zu words; received %zu words", messages, total, g_got.count);
    if (g_got.active_sensing != 0)
        printf(", and %llu active sensing set aside", (unsigned long long)g_got.active_sensing);
    printf("\n");
    pass = !g_got.overflow && g_got.count == total &&
           memcmp(g_got.words, words, total * sizeof(uint32_t)) == 0;
    if (!pass) {
        for (i = 0; i < total || i < g_got.count; i++)
            printf("  word %2zu: sent %08x, received %08x%s\n", i, i < total ? words[i] : 0,
                   i < g_got.count ? g_got.words[i] : 0,
                   i < total && i < g_got.count && words[i] == g_got.words[i] ? "" : "  <--");
    }
    os_unfair_lock_unlock(&g_got.lock);
    pass = pass && s->tx_wrong_cable == 0 && s->tx_refused == 0 && s->tx_ring_full == 0 &&
           s->rx_malformed == 0 && s->rx_send_errors == 0 && s->usb_error == 0 &&
           s->fatal == NULL;

done:
    if (in_port != 0)
        MIDIPortDispose(in_port);
    if (out_port != 0)
        MIDIPortDispose(out_port);
    dispose_ports(s);
    return pass;
}

/* ---- Opening and closing the unit ---- */

typedef struct {
    device_ref device;
    CFRunLoopSourceRef usb_source;
    int opened;
} unit;

/* Opens interface 2 and starts listening. Nothing is sent. */
static int open_unit(session *s, unit *u)
{
    CFRunLoopSourceContext wake_context;
    ua101_config config;
    const ua101_alt *midi_alt;
    io_service_t service;
    IOReturn result;

    service = find_device();
    if (service == IO_OBJECT_NULL) {
        printf("No UA-101 in Hi-Speed mode was found. Run ua101probe for details.\n");
        return 0;
    }
    u->device = open_device_object(service);
    IOObjectRelease(service);
    if (u->device == NULL) {
        printf("Couldn't get a device object.\n");
        return 0;
    }
    if (!read_config(u->device, &config))
        return 0;
    midi_alt = ua101_find_alt(&config, UA101_IF_MIDI, 0);
    if (midi_alt == NULL) {
        printf("The descriptors don't have the MIDI interface.\n");
        return 0;
    }
    s->intf = find_interface(u->device, UA101_IF_MIDI);
    if (s->intf == NULL) {
        printf("Couldn't find the MIDI interface.\n");
        return 0;
    }
    result = (*s->intf)->USBInterfaceOpen(s->intf);
    if (result != kIOReturnSuccess) {
        printf("Couldn't open the MIDI interface: %s\n", describe(result));
        if (result == kIOReturnExclusiveAccess)
            printf("Another program is using it. Quit MIDI apps and any UA-101 test.\n");
        return 0;
    }
    u->opened = 1;
    if (!find_pipes(s, midi_alt))
        return 0;
    s->in_buf = calloc(1, s->in_bytes);
    s->out_buf = calloc(1, s->out_bytes);
    if (s->in_buf == NULL || s->out_buf == NULL) {
        printf("Out of memory.\n");
        return 0;
    }
    result = (*s->intf)->CreateInterfaceAsyncEventSource(s->intf, &u->usb_source);
    if (result != kIOReturnSuccess) {
        printf("Couldn't create the USB event source: %s\n", describe(result));
        return 0;
    }
    s->loop = CFRunLoopGetCurrent();
    CFRunLoopAddSource(s->loop, u->usb_source, kCFRunLoopDefaultMode);
    memset(&wake_context, 0, sizeof(wake_context));
    wake_context.info = s;
    wake_context.perform = wake_perform;
    s->wake = CFRunLoopSourceCreate(NULL, 0, &wake_context);
    if (s->wake == NULL) {
        printf("Couldn't create the wake-up source.\n");
        return 0;
    }
    CFRunLoopAddSource(s->loop, s->wake, kCFRunLoopDefaultMode);

    /* Listening starts before any port appears, so nothing from MIDI IN is missed. */
    start_read(s);
    if (s->usb_error != 0) {
        printf("Couldn't start reading: %s\n", describe(s->usb_error));
        return 0;
    }
    return 1;
}

/* Lets the pipes finish. Call after dispose_ports, so nothing new arrives from apps. */
static void stop_unit(session *s)
{
    CFAbsoluteTime until = CFAbsoluteTimeGetCurrent() + 1.0;

    s->stopping = 1;
    if (s->read_pending)
        (*s->intf)->AbortPipe(s->intf, s->pipe_in);
    if (s->write_pending)
        (*s->intf)->AbortPipe(s->intf, s->pipe_out);
    while ((s->read_pending || s->write_pending) && CFAbsoluteTimeGetCurrent() < until)
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, false);
}

static void close_unit(session *s, unit *u)
{
    if (u->usb_source != NULL)
        CFRunLoopRemoveSource(s->loop, u->usb_source, kCFRunLoopDefaultMode);
    if (s->wake != NULL) {
        CFRunLoopRemoveSource(s->loop, s->wake, kCFRunLoopDefaultMode);
        CFRelease(s->wake);
        s->wake = NULL;
    }
    if (s->intf != NULL) {
        if (u->opened)
            (*s->intf)->USBInterfaceClose(s->intf);
        (*s->intf)->Release(s->intf);
        s->intf = NULL;
    }
    free(s->in_buf);
    free(s->out_buf);
    s->in_buf = s->out_buf = NULL;
    if (u->device != NULL)
        (*u->device)->Release(u->device);
    u->device = NULL;
}

/* ---- main ---- */

static int usage(void)
{
    printf("Usage: ua101midi [--seconds N] [--receive-only] [--show]\n"
           "       ua101midi --self-test\n"
           "       ua101midi --cable-test\n"
           "  Publishes the unit's MIDI IN and OUT as \"" PORT_NAME "\" until Ctrl-C, or for N\n"
           "  seconds.\n"
           "  --receive-only  MIDI IN only: there's no destination, so nothing is sent\n"
           "  --show          print the first %d messages from MIDI IN\n"
           "  --self-test     check the CoreMIDI side with private ports, without the unit\n"
           "  --cable-test    with a MIDI cable from MIDI OUT to MIDI IN and nothing else\n"
           "                  connected, send a fixed set of messages and check they come back\n",
           MAX_SHOWN);
    return 2;
}

int main(int argc, char **argv)
{
    session *s = &g_session;
    double seconds = 0.0;
    int receive_only = 0, self_test = 0, cable_test = 0, i, exit_code = 1, pass;
    struct sigaction action;
    unit u;
    CFAbsoluteTime deadline;

    setvbuf(stdout, NULL, _IOLBF, 0);
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc)
            seconds = atof(argv[++i]);
        else if (strcmp(argv[i], "--receive-only") == 0)
            receive_only = 1;
        else if (strcmp(argv[i], "--show") == 0)
            s->show = 1;
        else if (strcmp(argv[i], "--self-test") == 0)
            self_test = 1;
        else if (strcmp(argv[i], "--cable-test") == 0)
            cable_test = 1;
        else
            return usage();
    }
    if (!(seconds >= 0.0) || self_test + cable_test + receive_only > 1)
        return usage();

    memset(&action, 0, sizeof(action));
    action.sa_handler = on_signal;
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    memset(&u, 0, sizeof(u));
    s->ring_lock = OS_UNFAIR_LOCK_INIT;
    ua101_midi_parser_init(&s->tx_parser);
    ua101_usbmidi_packer_init(&s->tx_packer);
    ua101_midi_parser_init(&s->rx_parser);
    ua101_ump_packer_init(&s->rx_packer);

    if (self_test) {
        printf("=== UA-101 MIDI self-test (no USB) ===\n");
        s->loopback = 1;
        pass = message_test(s, PORT_NAME " self-test", k_self_test_words,
                            sizeof(k_self_test_words) / sizeof(k_self_test_words[0]));
        pass = pass && s->rx_control == 0;
        print_counts(s);
        printf("\n=== MIDI self-test: %s ===\n", pass ? "PASS" : "FAIL");
        return pass ? 0 : 1;
    }

    printf("=== UA-101 MIDI (%s) ===\n", cable_test     ? "cable test"
                                        : receive_only ? "MIDI IN only"
                                                       : "MIDI IN and OUT");
    if (!open_unit(s, &u))
        goto done;

    if (cable_test) {
        printf("Sending %zu words out of MIDI OUT. Only a cable back to MIDI IN should be "
               "connected.\n",
               sizeof(k_cable_test_words) / sizeof(k_cable_test_words[0]));
        pass = message_test(s, PORT_NAME " cable test", k_cable_test_words,
                            sizeof(k_cable_test_words) / sizeof(k_cable_test_words[0]));
        stop_unit(s);
        print_counts(s);
        printf("\n=== MIDI cable test: %s ===\n", pass ? "PASS" : "FAIL");
        exit_code = pass ? 0 : 1;
        goto done;
    }

    if (!make_ports(s, PORT_NAME, !receive_only, 0))
        goto stop;
    if (seconds > 0.0)
        printf("Running for %.0f s. Ctrl-C stops it sooner.\n", seconds);
    else
        printf("Running. Ctrl-C stops it.\n");
    deadline = CFAbsoluteTimeGetCurrent() + seconds;
    while (!g_signal && s->usb_error == 0 && s->fatal == NULL &&
           (seconds == 0.0 || CFAbsoluteTimeGetCurrent() < deadline))
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, false);
    if (s->usb_error != 0)
        printf("USB error: %s\n", describe(s->usb_error));
    if (s->fatal != NULL)
        printf("Stopped: %s.\n", s->fatal);
    exit_code = (s->usb_error == 0 && s->fatal == NULL) ? 0 : 1;

stop:
    /* No new MIDI from apps, then let the pipes finish. */
    dispose_ports(s);
    stop_unit(s);
    print_counts(s);
    printf("\n=== UA-101 MIDI: %s ===\n", exit_code == 0 ? "stopped cleanly" : "FAILED");

done:
    dispose_ports(s);
    close_unit(s, &u);
    return exit_code;
}
