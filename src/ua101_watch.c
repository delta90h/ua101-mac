#include "ua101_watch.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOMessage.h>
#include <IOKit/pwr_mgt/IOPMLib.h>

#include "ua101_desc.h"

static struct {
    ua101_watch_events events;
    IONotificationPortRef bus_port, power_port;
    io_iterator_t arrived, left;
    io_connect_t root; /* for answering the system's sleep messages */
    io_object_t power_notifier;
} W;

/* The same match the engine opens the unit by (src/ua101_engine.c). */
static CFMutableDictionaryRef unit_match(void)
{
    CFMutableDictionaryRef match = IOServiceMatching("IOUSBHostDevice");
    int32_t vendor = UA101_VENDOR_ID, product = UA101_PRODUCT_HIGH_SPEED;
    CFNumberRef v, p;

    if (match == NULL)
        return NULL;
    v = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &vendor);
    p = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &product);
    if (v != NULL)
        CFDictionarySetValue(match, CFSTR("idVendor"), v);
    if (p != NULL)
        CFDictionarySetValue(match, CFSTR("idProduct"), p);
    if (v != NULL)
        CFRelease(v);
    if (p != NULL)
        CFRelease(p);
    return match;
}

/* An iterator must be emptied before it reports anything more. Returns how many it held. */
static int drain(io_iterator_t it)
{
    io_object_t o;
    int n = 0;

    while ((o = IOIteratorNext(it)) != IO_OBJECT_NULL) {
        IOObjectRelease(o);
        n++;
    }
    return n;
}

static void on_arrived(void *refcon, io_iterator_t it)
{
    int n = drain(it);

    (void)refcon;
    while (n-- > 0 && W.events.unit_arrived != NULL)
        W.events.unit_arrived();
}

static void on_left(void *refcon, io_iterator_t it)
{
    int n = drain(it);

    (void)refcon;
    while (n-- > 0 && W.events.unit_left != NULL)
        W.events.unit_left();
}

static void on_power(void *refcon, io_service_t service, natural_t type, void *argument)
{
    (void)refcon;
    (void)service;
    switch (type) {
    case kIOMessageCanSystemSleep:
        /* An idle sleep is being considered. Core Audio keeps the Mac awake itself while audio
           runs, so the helper never objects. */
        IOAllowPowerChange(W.root, (long)argument);
        break;
    case kIOMessageSystemWillSleep:
        if (W.events.will_sleep != NULL)
            W.events.will_sleep();
        IOAllowPowerChange(W.root, (long)argument);
        break;
    case kIOMessageSystemHasPoweredOn:
        if (W.events.did_wake != NULL)
            W.events.did_wake();
        break;
    default:
        break;
    }
}

int ua101_watch_start(dispatch_queue_t queue, const ua101_watch_events *events, int *units)
{
    CFMutableDictionaryRef match;
    int bus_ok = 0;

    W.events = *events;
    *units = 0;

    W.root = IORegisterForSystemPower(NULL, &W.power_port, on_power, &W.power_notifier);
    if (W.root != MACH_PORT_NULL)
        IONotificationPortSetDispatchQueue(W.power_port, queue);

    W.bus_port = IONotificationPortCreate(kIOMainPortDefault);
    if (W.bus_port == NULL)
        return 0;
    IONotificationPortSetDispatchQueue(W.bus_port, queue);
    /* Each call takes its match dictionary for itself. */
    if ((match = unit_match()) != NULL &&
        IOServiceAddMatchingNotification(W.bus_port, kIOFirstMatchNotification, match, on_arrived,
                                         NULL, &W.arrived) == KERN_SUCCESS &&
        (match = unit_match()) != NULL &&
        IOServiceAddMatchingNotification(W.bus_port, kIOTerminatedNotification, match, on_left,
                                         NULL, &W.left) == KERN_SUCCESS) {
        *units = drain(W.arrived); /* the ones already there: counted, not announced */
        drain(W.left);
        bus_ok = 1;
    }
    return bus_ok;
}
