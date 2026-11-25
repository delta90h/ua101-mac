/*
 * ua101probe: Stage 0 hardware check for the Edirol/Roland UA-101.
 *
 * It finds the device, sets its USB configuration if needed, prints its
 * descriptors, opens each interface and briefly selects the streaming
 * settings. It never transfers any audio or MIDI, and it doesn't install
 * anything or change any system setting. Run it without sudo.
 *
 * With --read-only, and always for the Full-Speed product (0x008D), it only
 * reads what macOS already holds about the device (I/O Registry, cached
 * descriptors): no open, no configuration, no alternate setting, no request.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>

#include <mach/mach_error.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include "ua101_desc.h"

typedef IOUSBDeviceInterface650 **device_ref;
typedef IOUSBInterfaceInterface650 **interface_ref;

#define INTERFACE_WAIT_TRIES 20
#define INTERFACE_WAIT_US 100000

static int g_checks;
static int g_failures;

#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 2, 3)))
#endif
static int check(int ok, const char *fmt, ...)
{
    va_list args;

    g_checks++;
    if (!ok)
        g_failures++;
    printf("  [%s] ", ok ? " OK " : "FAIL");
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    putchar('\n');
    return ok;
}

static void report_check(void *ctx, int ok, const char *message)
{
    (void)ctx;
    check(ok, "%s", message);
}

static const char *describe(IOReturn result)
{
    static char text[128];

    snprintf(text, sizeof(text), "0x%08x, %s", (unsigned)result,
             mach_error_string(result));
    return text;
}

static void print_system_info(void)
{
    char version[64] = "unknown";
    char machine[64] = "unknown";
    int translated = 0;
    size_t size;

    size = sizeof(version);
    sysctlbyname("kern.osproductversion", version, &size, NULL, 0);
    size = sizeof(machine);
    sysctlbyname("hw.machine", machine, &size, NULL, 0);
    size = sizeof(translated);
    if (sysctlbyname("sysctl.proc_translated", &translated, &size, NULL, 0) != 0)
        translated = 0;

    printf("macOS %s, %s%s\n", version, machine,
           translated ? " (running under Rosetta)" : "");
}

static void add_number(CFMutableDictionaryRef dict, CFStringRef key, int32_t value)
{
    CFNumberRef number =
        CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &value);

    if (number != NULL) {
        CFDictionarySetValue(dict, key, number);
        CFRelease(number);
    }
}

static CFMutableDictionaryRef roland_matching(int32_t product)
{
    CFMutableDictionaryRef match = IOServiceMatching("IOUSBHostDevice");

    if (match != NULL) {
        add_number(match, CFSTR("idVendor"), UA101_VENDOR_ID);
        if (product >= 0)
            add_number(match, CFSTR("idProduct"), product);
    }
    return match;
}

static io_service_t find_device(uint16_t product)
{
    CFMutableDictionaryRef match = roland_matching(product);

    if (match == NULL)
        return IO_OBJECT_NULL;
    /* This call consumes the matching dictionary. */
    return IOServiceGetMatchingService(kIOMainPortDefault, match);
}

/* Helps diagnose a missing device: is any Roland USB device visible at all? */
static void list_roland_devices(void)
{
    CFMutableDictionaryRef match = roland_matching(-1);
    io_iterator_t iterator = IO_OBJECT_NULL;
    io_service_t service;
    int count = 0;

    if (match == NULL ||
        IOServiceGetMatchingServices(kIOMainPortDefault, match, &iterator) !=
            KERN_SUCCESS)
        return;

    while ((service = IOIteratorNext(iterator)) != IO_OBJECT_NULL) {
        io_name_t name = "unnamed";
        int32_t product = 0;
        CFTypeRef value;

        IORegistryEntryGetName(service, name);
        value = IORegistryEntryCreateCFProperty(service, CFSTR("idProduct"),
                                                kCFAllocatorDefault, 0);
        if (value != NULL) {
            CFNumberGetValue((CFNumberRef)value, kCFNumberSInt32Type, &product);
            CFRelease(value);
        }
        printf("  Roland USB device seen: %s, product 0x%04x\n", name,
               (unsigned)product);
        count++;
        IOObjectRelease(service);
    }
    IOObjectRelease(iterator);
    if (count == 0)
        printf("  No Roland USB devices are visible to this Mac.\n");
}

static device_ref open_device_interface(io_service_t service)
{
    IOCFPlugInInterface **plugin = NULL;
    device_ref device = NULL;
    SInt32 score = 0;
    kern_return_t result;
    HRESULT query;

    result = IOCreatePlugInInterfaceForService(
        service, kIOUSBDeviceUserClientTypeID, kIOCFPlugInInterfaceID, &plugin,
        &score);
    if (result != KERN_SUCCESS || plugin == NULL) {
        printf("  Could not create the device plug-in: %s\n", describe(result));
        return NULL;
    }
    query = (*plugin)->QueryInterface(
        plugin, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID650), (LPVOID *)&device);
    IODestroyPlugInInterface(plugin);
    if (query != 0 || device == NULL) {
        printf("  Could not get the device interface (0x%08x)\n", (unsigned)query);
        return NULL;
    }
    return device;
}

static interface_ref open_interface_interface(io_service_t service)
{
    IOCFPlugInInterface **plugin = NULL;
    interface_ref intf = NULL;
    SInt32 score = 0;
    kern_return_t result;
    HRESULT query;

    result = IOCreatePlugInInterfaceForService(
        service, kIOUSBInterfaceUserClientTypeID, kIOCFPlugInInterfaceID, &plugin,
        &score);
    if (result != KERN_SUCCESS || plugin == NULL) {
        printf("  Could not create the interface plug-in: %s\n", describe(result));
        return NULL;
    }
    query = (*plugin)->QueryInterface(
        plugin, CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID650), (LPVOID *)&intf);
    IODestroyPlugInInterface(plugin);
    if (query != 0 || intf == NULL) {
        printf("  Could not get the interface object (0x%08x)\n", (unsigned)query);
        return NULL;
    }
    return intf;
}

static void print_device_identity(device_ref device)
{
    UInt16 vendor = 0, product = 0, release = 0;
    UInt8 device_class = 0, subclass = 0, protocol = 0, speed = 0, configs = 0;
    UInt32 location = 0;
    const char *speed_name;

    (*device)->GetDeviceVendor(device, &vendor);
    (*device)->GetDeviceProduct(device, &product);
    (*device)->GetDeviceReleaseNumber(device, &release);
    (*device)->GetDeviceClass(device, &device_class);
    (*device)->GetDeviceSubClass(device, &subclass);
    (*device)->GetDeviceProtocol(device, &protocol);
    (*device)->GetDeviceSpeed(device, &speed);
    (*device)->GetNumberOfConfigurations(device, &configs);
    (*device)->GetLocationID(device, &location);

    switch (speed) {
    case kUSBDeviceSpeedLow:
        speed_name = "Low";
        break;
    case kUSBDeviceSpeedFull:
        speed_name = "Full";
        break;
    case kUSBDeviceSpeedHigh:
        speed_name = "High";
        break;
    default:
        speed_name = "SuperSpeed or other";
        break;
    }

    printf("  Vendor 0x%04x, product 0x%04x, release 0x%04x, location 0x%08x\n",
           vendor, product, release, (unsigned)location);
    printf("  Class %u, subclass %u, protocol %u, configurations %u, %s speed\n",
           device_class, subclass, protocol, configs, speed_name);

    check(speed == kUSBDeviceSpeedHigh,
          "Device is connected at High speed (needed for all channels)");
}

/*
 * Nothing on macOS configures this vendor-specific device, so we do it here.
 * It's safe because no audio can be streaming yet. Returns 1 when the device
 * was left open and the caller must close it.
 */
static int ensure_configured(device_ref device)
{
    IOReturn result;
    UInt8 current = 0;
    int opened = 0;

    result = (*device)->USBDeviceOpen(device);
    if (result == kIOReturnSuccess) {
        opened = 1;
        check(1, "Device opened");
    } else if (result == kIOReturnExclusiveAccess) {
        printf("  Another program has the device open. Quit audio or MIDI apps,\n"
               "  and remove any older Roland driver if one is installed.\n");
        check(0, "Device opened (%s)", describe(result));
    } else {
        check(0, "Device opened (%s)", describe(result));
    }

    result = (*device)->GetConfiguration(device, &current);
    if (result != kIOReturnSuccess)
        printf("  Could not read the current configuration: %s\n", describe(result));
    printf("  Current configuration value: %u\n", current);

    if (current != 1 && opened) {
        result = (*device)->SetConfiguration(device, 1);
        check(result == kIOReturnSuccess, "Set configuration 1 (%s)",
              describe(result));
        result = (*device)->GetConfiguration(device, &current);
        if (result != kIOReturnSuccess)
            printf("  Could not re-read the configuration: %s\n", describe(result));
    }
    check(current == 1, "Device is using configuration 1");
    return opened;
}

static int read_and_check_descriptors(device_ref device, ua101_config *config)
{
    IOUSBConfigurationDescriptorPtr descriptor = NULL;
    const uint8_t *bytes;
    ua101_parse_result parsed;
    IOReturn result;
    size_t total, i;
    uint8_t n;

    result = (*device)->GetConfigurationDescriptorPtr(device, 0, &descriptor);
    if (!check(result == kIOReturnSuccess && descriptor != NULL,
               "Read configuration descriptor (%s)", describe(result)))
        return 0;

    bytes = (const uint8_t *)descriptor;
    total = (size_t)bytes[2] | ((size_t)bytes[3] << 8);

    printf("\n  Raw configuration descriptor (%zu bytes):\n", total);
    for (i = 0; i < total; i++)
        printf("%s%02x", (i % 16 == 0) ? "\n    " : " ", bytes[i]);
    printf("\n\n");

    parsed = ua101_parse_config(bytes, total, config);
    if (!check(parsed == UA101_PARSE_OK, "Descriptor parsed (%s)",
               ua101_parse_result_string(parsed)))
        return 0;

    for (n = 0; n < config->num_alts; n++) {
        const ua101_alt *alt = &config->alts[n];
        uint8_t e;

        printf("  Interface %u alt %u: class %u/%u/%u, %u endpoint(s)", alt->number,
               alt->alt, alt->iface_class, alt->iface_subclass, alt->iface_protocol,
               alt->num_endpoints);
        if (alt->has_format)
            printf(", %u ch, %u-byte samples, %u-bit, %u Hz", alt->channels,
                   alt->subframe_bytes, alt->bit_resolution, (unsigned)alt->rate_hz);
        putchar('\n');
        for (e = 0; e < alt->num_endpoints; e++) {
            const ua101_endpoint *ep = &alt->endpoints[e];
            printf("    Endpoint 0x%02x, %s, %s, max packet %u, interval %u\n",
                   ep->address, ua101_endpoint_is_in(ep) ? "IN" : "OUT",
                   ua101_endpoint_is_isochronous(ep) ? "isochronous"
                   : ua101_endpoint_is_bulk(ep)      ? "bulk"
                   : (ep->attributes & 0x03) == 3    ? "interrupt"
                                                     : "control",
                   ep->max_packet, ep->interval);
        }
    }
    putchar('\n');

    ua101_check_config(config, report_check, NULL);
    return 1;
}

static void check_pipe(interface_ref intf, const char *label, UInt8 pipe,
                       const ua101_endpoint *expected)
{
    UInt8 direction = 0, number = 0, type = 0, interval = 0;
    UInt16 max_packet = 0;
    IOReturn result;

    result = (*intf)->GetPipeProperties(intf, pipe, &direction, &number, &type,
                                        &max_packet, &interval);
    if (!check(result == kIOReturnSuccess, "%s: read pipe %u (%s)", label, pipe,
               describe(result)))
        return;

    printf("    Pipe %u: endpoint %u, %s, type %u, max packet %u, interval %u\n", pipe,
           number, direction == kUSBIn ? "IN" : "OUT", type, max_packet, interval);

    if (expected != NULL) {
        int want_in = ua101_endpoint_is_in(expected);

        check(number == (expected->address & 0x0F) &&
                  (direction == kUSBIn) == want_in,
              "%s: pipe matches endpoint 0x%02x", label, expected->address);
        if (max_packet != expected->max_packet)
            printf("    Note: macOS reports max packet %u; the descriptor says %u\n",
                   max_packet, expected->max_packet);
    }
}

/* Selects the streaming setting and returns to idle. No audio is transferred. */
static void test_audio_interface(interface_ref intf, const ua101_config *config,
                                 UInt8 number, const char *label)
{
    const ua101_alt *alt = ua101_find_alt(config, number, 1);
    IOReturn result;
    UInt8 endpoints = 0;

    result = (*intf)->SetAlternateInterface(intf, 1);
    if (!check(result == kIOReturnSuccess, "%s: selected streaming setting (%s)",
               label, describe(result))) {
        if (result == kIOReturnNoBandwidth)
            printf("    Not enough USB bandwidth. Plug the UA-101 directly into\n"
                   "    the Mac, without a hub, and disconnect other USB audio.\n");
        return;
    }

    (*intf)->GetNumEndpoints(intf, &endpoints);
    if (check(endpoints == 1, "%s: streaming setting has one pipe (found %u)", label,
              endpoints))
        check_pipe(intf, label, 1,
                   (alt != NULL && alt->num_endpoints == 1) ? &alt->endpoints[0]
                                                            : NULL);

    result = (*intf)->SetAlternateInterface(intf, 0);
    check(result == kIOReturnSuccess, "%s: returned to idle setting (%s)", label,
          describe(result));
}

static void test_midi_interface(interface_ref intf, const ua101_config *config)
{
    const ua101_alt *alt = ua101_find_alt(config, UA101_IF_MIDI, 0);
    UInt8 endpoints = 0, pipe;

    (*intf)->GetNumEndpoints(intf, &endpoints);
    check(endpoints == 2, "MIDI: idle setting has two pipes (found %u)", endpoints);
    for (pipe = 1; pipe <= endpoints; pipe++) {
        const ua101_endpoint *expected = NULL;
        UInt8 direction = 0, number = 0, type = 0, interval = 0;
        UInt16 max_packet = 0;

        if ((*intf)->GetPipeProperties(intf, pipe, &direction, &number, &type,
                                       &max_packet, &interval) == kIOReturnSuccess)
            check(type == kUSBBulk, "MIDI: pipe %u is bulk", pipe);

        /* macOS may number pipes differently, so match on the endpoint. */
        if (alt != NULL) {
            uint8_t e;

            for (e = 0; e < alt->num_endpoints; e++) {
                const ua101_endpoint *ep = &alt->endpoints[e];

                if ((ep->address & 0x0F) == number &&
                    ua101_endpoint_is_in(ep) == (direction == kUSBIn))
                    expected = ep;
            }
        }
        check_pipe(intf, "MIDI", pipe, expected);
    }
}

static void test_interface(interface_ref intf, const ua101_config *config,
                           UInt8 number)
{
    static const char *const labels[UA101_NUM_INTERFACES] = {"Playback", "Capture",
                                                             "MIDI"};
    const char *label = number < UA101_NUM_INTERFACES ? labels[number] : "Unknown";
    IOReturn result;

    printf("\n  Interface %u (%s):\n", number, label);
    result = (*intf)->USBInterfaceOpen(intf);
    if (!check(result == kIOReturnSuccess, "%s: opened without root (%s)", label,
               describe(result))) {
        if (result == kIOReturnExclusiveAccess)
            printf("    Another program or driver is using this interface.\n");
        return;
    }

    if (number == UA101_IF_PLAYBACK || number == UA101_IF_CAPTURE)
        test_audio_interface(intf, config, number, label);
    else if (number == UA101_IF_MIDI)
        test_midi_interface(intf, config);

    result = (*intf)->USBInterfaceClose(intf);
    check(result == kIOReturnSuccess, "%s: closed (%s)", label, describe(result));
}

static kern_return_t interface_iterator(device_ref device, io_iterator_t *iterator)
{
    IOUSBFindInterfaceRequest request;

    request.bInterfaceClass = kIOUSBFindInterfaceDontCare;
    request.bInterfaceSubClass = kIOUSBFindInterfaceDontCare;
    request.bInterfaceProtocol = kIOUSBFindInterfaceDontCare;
    request.bAlternateSetting = kIOUSBFindInterfaceDontCare;
    return (*device)->CreateInterfaceIterator(device, &request, iterator);
}

static int count_interfaces(device_ref device)
{
    io_iterator_t iterator = IO_OBJECT_NULL;
    io_service_t service;
    int count = 0;

    if (interface_iterator(device, &iterator) != kIOReturnSuccess)
        return 0;
    while ((service = IOIteratorNext(iterator)) != IO_OBJECT_NULL) {
        count++;
        IOObjectRelease(service);
    }
    IOObjectRelease(iterator);
    return count;
}

static void test_all_interfaces(device_ref device, const ua101_config *config)
{
    io_iterator_t iterator = IO_OBJECT_NULL;
    io_service_t service;
    IOReturn result;
    int tries, found, tested = 0;

    /* macOS publishes interfaces shortly after the configuration is set. */
    found = count_interfaces(device);
    for (tries = 0; found < UA101_NUM_INTERFACES && tries < INTERFACE_WAIT_TRIES;
         tries++) {
        usleep(INTERFACE_WAIT_US);
        found = count_interfaces(device);
    }
    check(found == UA101_NUM_INTERFACES, "macOS lists %d interfaces (found %d)",
          UA101_NUM_INTERFACES, found);

    result = interface_iterator(device, &iterator);
    if (!check(result == kIOReturnSuccess, "Listed interfaces (%s)", describe(result)))
        return;

    while ((service = IOIteratorNext(iterator)) != IO_OBJECT_NULL) {
        interface_ref intf = open_interface_interface(service);

        IOObjectRelease(service);
        if (check(intf != NULL, "Got an interface object")) {
            UInt8 number = 0;

            (*intf)->GetInterfaceNumber(intf, &number);
            test_interface(intf, config, number);
            (*intf)->Release(intf);
            tested++;
        }
    }
    IOObjectRelease(iterator);
    check(tested == UA101_NUM_INTERFACES, "Tested %d interfaces (tested %d)",
          UA101_NUM_INTERFACES, tested);
}

/*
 * Read-only report, used for the Full-Speed product (0x008D) and for
 * --read-only. It never opens the device or an interface, never sets a
 * configuration or an alternate setting and sends no request of its own:
 * everything comes from what macOS read when the unit was plugged in (the
 * I/O Registry and the cached configuration descriptors).
 */
static void print_hex_line(const char *indent, const uint8_t *d, size_t len)
{
    size_t i;

    printf("%s", indent);
    for (i = 0; i < len; i++)
        printf("%s%02x", i ? " " : "", d[i]);
    putchar('\n');
}

static uint32_t read24(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

static uint16_t read16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

/* Which kind of class-specific descriptors follow an interface descriptor. */
enum ro_kind { RO_OTHER, RO_CONTROL, RO_STREAMING, RO_MIDI };

static enum ro_kind interface_kind(uint8_t cls, uint8_t sub, uint8_t proto)
{
    if (cls == 1)
        return sub == 1 ? RO_CONTROL : sub == 2 ? RO_STREAMING : sub == 3 ? RO_MIDI
                                                                           : RO_OTHER;
    /* The vendor layout seen in Hi-Speed: class 255, subclass 1, protocol 2 for
     * audio streaming and protocol 3 for MIDI. */
    if (cls == 0xFF)
        return proto == 2 ? RO_STREAMING : proto == 3 ? RO_MIDI : RO_OTHER;
    return RO_OTHER;
}

static void decode_cs_interface(enum ro_kind kind, const uint8_t *d, uint8_t len)
{
    uint8_t sub = len >= 3 ? d[2] : 0;

    if (kind == RO_STREAMING && sub == 1 && len >= 7) {
        printf("      AS general: terminal link %u, delay %u, format tag 0x%04x%s\n", d[3],
               d[4], read16(d + 5), read16(d + 5) == 1 ? " (PCM)" : "");
    } else if (kind == RO_STREAMING && sub == 2 && len >= 8 && d[3] == 1) {
        uint8_t n = d[7], i;

        printf("      Format type I: %u channel(s), %u-byte samples, %u-bit, ", d[4], d[5],
               d[6]);
        if (n == 0 && len >= 14) {
            printf("continuous %u to %u Hz\n", (unsigned)read24(d + 8),
                   (unsigned)read24(d + 11));
        } else {
            printf("%u rate(s):", n);
            for (i = 0; i < n && 8u + 3u * i + 3u <= len; i++)
                printf(" %u", (unsigned)read24(d + 8 + 3 * i));
            printf(" Hz\n");
        }
    } else if (kind == RO_STREAMING && sub == 2 && len >= 4) {
        printf("      Format type %u (not decoded)\n", d[3]);
    } else if (kind == RO_CONTROL && sub == 1 && len >= 5) {
        printf("      AC header: audio class %x.%02x\n", d[4], d[3]);
    } else if (kind == RO_CONTROL && sub == 2 && len >= 8) {
        printf("      Input terminal %u: type 0x%04x, %u channel(s)\n", d[3],
               read16(d + 4), d[7]);
    } else if (kind == RO_CONTROL && sub == 3 && len >= 8) {
        printf("      Output terminal %u: type 0x%04x, source %u\n", d[3], read16(d + 4),
               d[7]);
    } else if (kind == RO_CONTROL && sub == 6 && len >= 5) {
        printf("      Feature unit %u: source %u\n", d[3], d[4]);
    } else if (kind == RO_MIDI && sub == 1 && len >= 5) {
        printf("      MIDI header: MIDI class %x.%02x\n", d[4], d[3]);
    } else if (kind == RO_MIDI && sub == 2 && len >= 5) {
        printf("      MIDI IN jack %u (%s)\n", d[4],
               d[3] == 1 ? "embedded" : d[3] == 2 ? "external" : "other");
    } else if (kind == RO_MIDI && sub == 3 && len >= 6) {
        printf("      MIDI OUT jack %u (%s), %u input pin(s)\n", d[4],
               d[3] == 1 ? "embedded" : d[3] == 2 ? "external" : "other", d[5]);
    } else {
        printf("      Class-specific descriptor, subtype %u (not decoded)\n", sub);
    }
}

static void decode_cs_endpoint(enum ro_kind kind, const uint8_t *d, uint8_t len)
{
    if (kind == RO_MIDI && len >= 4 && d[2] == 1) {
        uint8_t i;

        printf("      MIDI endpoint: %u embedded jack(s):", d[3]);
        for (i = 0; i < d[3] && 4u + i < len; i++)
            printf(" %u", d[4 + i]);
        putchar('\n');
    } else if (kind == RO_STREAMING && len >= 7 && d[2] == 1) {
        printf("      Audio endpoint: attributes 0x%02x (rate control %s, pitch %s), "
               "lock delay %u unit(s) %u\n",
               d[3], (d[3] & 1) ? "yes" : "no", (d[3] & 2) ? "yes" : "no", d[4],
               read16(d + 5));
    } else {
        printf("      Class-specific endpoint descriptor (not decoded)\n");
    }
}

static void decode_endpoint(const uint8_t *d, uint8_t len)
{
    static const char *const types[4] = {"control", "isochronous", "bulk", "interrupt"};
    static const char *const syncs[4] = {"no sync", "asynchronous", "adaptive",
                                         "synchronous"};
    static const char *const usages[4] = {"data", "feedback", "implicit feedback",
                                          "reserved"};
    uint16_t wmax = read16(d + 4);
    uint8_t type = d[3] & 3;

    printf("    Endpoint 0x%02x %s, %s", d[2], (d[2] & 0x80) ? "IN" : "OUT", types[type]);
    if (type == 1)
        printf(" (%s, %s)", syncs[(d[3] >> 2) & 3], usages[(d[3] >> 4) & 3]);
    printf(", max packet %u", wmax & 0x7FF);
    if (wmax >> 11)
        printf(" x %u per microframe", (wmax >> 11) + 1);
    printf(", interval %u", d[6]);
    if (len >= 9)
        printf(", refresh %u, sync endpoint 0x%02x", d[7], d[8]);
    putchar('\n');
}

static void decode_configuration(const uint8_t *b, size_t total)
{
    enum ro_kind kind = RO_OTHER;
    size_t off = 0;

    while (off + 2 <= total) {
        const uint8_t *d = b + off;
        uint8_t len = d[0], type = d[1];

        if (len < 2 || off + len > total) {
            printf("    (descriptor with length %u at offset %zu; stopping here)\n", len,
                   off);
            return;
        }
        if (type == 0x02 && len >= 9) {
            printf("  Configuration %u: %u interface(s), attributes 0x%02x (%s), "
                   "up to %u mA\n",
                   d[5], d[4], d[7], (d[7] & 0x40) ? "self-powered" : "bus-powered",
                   d[8] * 2u);
        } else if (type == 0x04 && len >= 9) {
            kind = interface_kind(d[5], d[6], d[7]);
            printf("  Interface %u alt %u: class %u/%u/%u, %u endpoint(s)\n", d[2], d[3],
                   d[5], d[6], d[7], d[4]);
        } else if (type == 0x05 && len >= 7) {
            decode_endpoint(d, len);
        } else if (type == 0x24) {
            print_hex_line("      bytes: ", d, len);
            decode_cs_interface(kind, d, len);
        } else if (type == 0x25) {
            print_hex_line("      bytes: ", d, len);
            decode_cs_endpoint(kind, d, len);
        } else {
            printf("    Descriptor type 0x%02x:", type);
            print_hex_line(" ", d, len);
        }
        off += len;
    }
}

static void print_registry_string(io_service_t service, CFStringRef key,
                                  const char *label)
{
    CFTypeRef value =
        IORegistryEntryCreateCFProperty(service, key, kCFAllocatorDefault, 0);
    char text[256];

    if (value == NULL)
        return;
    if (CFGetTypeID(value) == CFStringGetTypeID() &&
        CFStringGetCString((CFStringRef)value, text, sizeof(text), kCFStringEncodingUTF8))
        printf("  %s: %s\n", label, text);
    CFRelease(value);
}

/* What macOS attached to the device: interface nubs and any driver on them. */
static void print_registry_children(io_registry_entry_t entry, int depth)
{
    io_iterator_t iterator = IO_OBJECT_NULL;
    io_registry_entry_t child;

    if (depth > 3 ||
        IORegistryEntryGetChildIterator(entry, kIOServicePlane, &iterator) != KERN_SUCCESS)
        return;
    while ((child = IOIteratorNext(iterator)) != IO_OBJECT_NULL) {
        io_name_t name = "unnamed", cls = "unknown";

        IORegistryEntryGetName(child, name);
        IOObjectGetClass(child, cls);
        printf("  %*s%s (%s)\n", depth * 2, "", name, cls);
        print_registry_children(child, depth + 1);
        IOObjectRelease(child);
    }
    IOObjectRelease(iterator);
}

static void read_only_report(io_service_t service)
{
    device_ref device;
    ua101_config config;
    UInt8 configs = 0, i;

    printf("\nRead-only report: the device is not opened, no configuration or\n"
           "alternate setting is selected and no request is sent.\n");

    printf("\nI/O Registry:\n");
    print_registry_string(service, CFSTR("USB Vendor Name"), "Vendor name");
    print_registry_string(service, CFSTR("USB Product Name"), "Product name");
    printf("  What macOS attached to it:\n");
    print_registry_children(service, 1);

    device = open_device_interface(service);
    if (!check(device != NULL, "Got a device object"))
        return;

    printf("\nDevice:\n");
    print_device_identity(device);
    (*device)->GetNumberOfConfigurations(device, &configs);

    for (i = 0; i < configs; i++) {
        IOUSBConfigurationDescriptorPtr descriptor = NULL;
        const uint8_t *bytes;
        size_t total, k;
        IOReturn result;

        result = (*device)->GetConfigurationDescriptorPtr(device, i, &descriptor);
        if (!check(result == kIOReturnSuccess && descriptor != NULL,
                   "Read cached configuration descriptor %u (%s)", i, describe(result)))
            continue;
        bytes = (const uint8_t *)descriptor;
        total = (size_t)bytes[2] | ((size_t)bytes[3] << 8);

        printf("\n  Raw configuration descriptor %u (%zu bytes):\n", i, total);
        for (k = 0; k < total; k++)
            printf("%s%02x", (k % 16 == 0) ? "\n    " : " ", bytes[k]);
        printf("\n\n");
        decode_configuration(bytes, total);
        printf("\n  The Hi-Speed parser says: %s\n",
               ua101_parse_result_string(ua101_parse_config(bytes, total, &config)));
    }
    (*device)->Release(device);
}

static int finish(void)
{
    printf("\n=== Stage 0 result: %s (%d of %d checks passed) ===\n",
           g_failures == 0 ? "PASS" : "FAIL", g_checks - g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}

int main(int argc, char **argv)
{
    ua101_config config;
    io_service_t service;
    device_ref device;
    int device_open;
    int read_only = 0;

    /* Keep output in order if the program stops early or is piped to a file. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    if (argc == 2 && strcmp(argv[1], "--read-only") == 0) {
        read_only = 1;
    } else if (argc != 1) {
        fprintf(stderr, "usage: %s [--read-only]\n", argv[0]);
        return 2;
    }

    printf("=== UA-101 Stage 0 probe%s ===\n", read_only ? " (read-only)" : "");
    print_system_info();
    check(geteuid() != 0, "Running without root");

    printf("\nLooking for the UA-101:\n");
    service = find_device(UA101_PRODUCT_HIGH_SPEED);
    if (service != IO_OBJECT_NULL) {
        check(1, "Found UA-101 in High speed mode (product 0x%04x)",
              UA101_PRODUCT_HIGH_SPEED);
        if (read_only) {
            read_only_report(service);
            IOObjectRelease(service);
            return finish();
        }
    } else {
        service = find_device(UA101_PRODUCT_FULL_SPEED);
        if (service != IO_OBJECT_NULL) {
            check(0, "UA-101 found, but in Full speed mode (product 0x%04x)",
                  UA101_PRODUCT_FULL_SPEED);
            printf("  Turn the rear Hi-Speed USB switch on, power-cycle the unit\n"
                   "  and run this again. In Full speed this probe only reads.\n");
            read_only_report(service);
            IOObjectRelease(service);
            return finish();
        } else {
            check(0, "Found UA-101");
            list_roland_devices();
            printf("  Check the power, the USB cable and any adapter, and connect it\n"
                   "  directly to the Mac. Use a data cable, not a charge-only one.\n");
            return finish();
        }
    }

    device = open_device_interface(service);
    IOObjectRelease(service);
    if (!check(device != NULL, "Got a device object"))
        return finish();

    printf("\nDevice:\n");
    print_device_identity(device);

    printf("\nConfiguration:\n");
    device_open = ensure_configured(device);

    printf("\nDescriptors:\n");
    if (read_and_check_descriptors(device, &config)) {
        printf("\nInterfaces:\n");
        test_all_interfaces(device, &config);
    }

    if (device_open)
        (*device)->USBDeviceClose(device);
    (*device)->Release(device);

    return finish();
}
