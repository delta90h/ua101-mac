/*
 * ua101settings: reads the UA-101's mixer, patch bay and switch settings.
 *
 * It sends one fixed message, the read request Roland's driver sends at startup,
 * and listens for the reply. It can't change a setting: nothing in this program
 * or its parser builds a message that sets anything, and the only send goes
 * through a check that refuses anything but a request. See docs/control-protocol.md.
 *
 * With --all-blocks (a developer's test, 2026-11-27) it also asks for the other 15 blocks at
 * 02 01 00 00 to 02 0F 00 00, one at a time, and the settings block again at the end, to see
 * whether the unit's memories can be read without loading them. Those are asks too, built by
 * ua101_ctl_block_request and passed through the same check; still nothing sets anything.
 *
 * It installs nothing and changes no system setting. Run it without sudo, with
 * no other program using the UA-101's MIDI interface.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>

#include <mach/mach_error.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ua101_control.h"
#include "ua101_desc.h"

typedef IOUSBDeviceInterface650 **device_ref;
typedef IOUSBInterfaceInterface650 **interface_ref;

#define REPLY_WAIT_SECONDS 2.0
#define MAX_WATCH_SECONDS 600.0
#define WRITE_TIMEOUT_MS 1000
#define INTERFACE_WAIT_TRIES 20
#define INTERFACE_WAIT_US 100000
#define MAX_PAN 0x8000u
#define MAX_VOLUME 0x7FFFFFu
#define BLOCK_PAUSE_US 100000

typedef struct {
    interface_ref intf;
    UInt8 pipe_out;
    UInt8 pipe_in;
    UInt16 in_packet_bytes;
    uint8_t *in_buf;
    int read_pending;
    int stopping;
    int watching;
    IOReturn read_error;
    CFAbsoluteTime start_time;
    ua101_sysex_rx rx;

    int have_settings;
    uint8_t block[UA101_CTL_BLOCK_BYTES];
    unsigned checksum_failures;
    unsigned format_failures;
    unsigned other_messages;
    unsigned change_messages;
    uint64_t bytes_received;

    /* --all-blocks */
    int all_blocks;
    int have_block[UA101_CTL_BLOCKS];
    uint8_t blocks[UA101_CTL_BLOCKS][UA101_CTL_BLOCK_BYTES];
    int have_again; /* the settings block, asked again at the end */
    uint8_t again[UA101_CTL_BLOCK_BYTES];
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

static void print_hex(const uint8_t *bytes, size_t count)
{
    size_t i;

    for (i = 0; i < count; i++)
        printf("%s%02x", i ? " " : "", bytes[i]);
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
    kern_return_t result;

    result = IOCreatePlugInInterfaceForService(service, kIOUSBDeviceUserClientTypeID,
                                               kIOCFPlugInInterfaceID, &plugin, &score);
    if (result != KERN_SUCCESS || plugin == NULL)
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
    kern_return_t result;

    result = IOCreatePlugInInterfaceForService(service, kIOUSBInterfaceUserClientTypeID,
                                               kIOCFPlugInInterfaceID, &plugin, &score);
    if (result != KERN_SUCCESS || plugin == NULL)
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

/* Reads the descriptors, and sets the configuration if nothing has yet. */
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
    if (current != 1) {
        result = (*device)->SetConfiguration(device, 1);
        if (result != kIOReturnSuccess) {
            printf("Couldn't set the USB configuration: %s\n", describe(result));
            goto done;
        }
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
    /* Stage 1 needs interface 2 to work without holding the whole device. */
    (*device)->USBDeviceClose(device);
    return ok;
}

static int find_pipes(session *s, const ua101_alt *alt)
{
    UInt8 count = 0, pipe, setting = 0xFF;
    uint8_t e;

    (*s->intf)->GetAlternateSetting(s->intf, &setting);
    (*s->intf)->GetNumEndpoints(s->intf, &count);
    printf("  Interface %u, alternate setting %u, %u pipes\n", UA101_IF_MIDI, setting, count);

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
                s->in_packet_bytes = max_packet;
            } else if (direction == kUSBOut && !ua101_endpoint_is_in(ep)) {
                s->pipe_out = pipe;
            }
        }
    }
    if (s->pipe_in == 0 || s->pipe_out == 0 || s->in_packet_bytes == 0) {
        printf("  Couldn't find both bulk pipes in this setting.\n");
        return 0;
    }
    return 1;
}

static void handle_message(session *s, const uint8_t *msg, size_t len)
{
    ua101_ctl_view view;

    switch (ua101_ctl_classify(msg, len, &view)) {
    case UA101_CTL_SETTINGS:
        if (!s->have_settings) {
            memcpy(s->block, view.data, sizeof(s->block));
            s->have_settings = 1;
        } else if (s->all_blocks && !s->have_again) {
            memcpy(s->again, view.data, sizeof(s->again));
            s->have_again = 1;
        } else {
            printf("  Another settings block arrived, which isn't shown.\n");
        }
        break;
    case UA101_CTL_CHANGE:
        s->change_messages++;
        printf("  %8.3f s  Value change: address 00 01 00 %02x, value 0x%02x\n",
               CFAbsoluteTimeGetCurrent() - s->start_time, view.address[3], view.data[0]);
        break;
    case UA101_CTL_OTHER: {
        unsigned block = 0;

        if (s->all_blocks && ua101_ctl_block_reply(&view, &block) && block != 0) {
            if (!s->have_block[block]) {
                memcpy(s->blocks[block], view.data, UA101_CTL_BLOCK_BYTES);
                s->have_block[block] = 1;
            } else {
                printf("  Block 02 %02x arrived again; the first one is kept.\n", block);
            }
            break;
        }
        s->other_messages++;
        printf("  Other message (%zu bytes): ", len);
        print_hex(msg, len < 32 ? len : 32);
        printf("%s\n", len > 32 ? " ..." : "");
        break;
    }
    case UA101_CTL_BAD_CHECKSUM:
        s->checksum_failures++;
        printf("  Message with a bad checksum (%zu bytes): ", len);
        print_hex(msg, len < 32 ? len : 32);
        printf("%s\n", len > 32 ? " ..." : "");
        break;
    case UA101_CTL_BAD_FORMAT:
        s->format_failures++;
        printf("  Unrecognized message (%zu bytes): ", len);
        print_hex(msg, len < 32 ? len : 32);
        printf("%s\n", len > 32 ? " ..." : "");
        break;
    }
}

static void start_read(session *s);

static void read_done(void *refcon, IOReturn result, void *arg0)
{
    session *s = refcon;
    size_t bytes = (size_t)(uintptr_t)arg0;
    size_t i;

    s->read_pending = 0;
    if (s->stopping || result == kIOReturnAborted)
        return;
    if (result != kIOReturnSuccess && result != kIOReturnUnderrun) {
        s->read_error = result;
        return;
    }
    if (bytes > s->in_packet_bytes)
        bytes = s->in_packet_bytes;
    s->bytes_received += bytes;
    for (i = 0; i + 4 <= bytes; i += 4) {
        const uint8_t *msg = NULL;
        size_t len = 0;

        if (ua101_sysex_rx_feed(&s->rx, s->in_buf + i, &msg, &len))
            handle_message(s, msg, len);
    }
    start_read(s);
}

static void start_read(session *s)
{
    IOReturn result =
        (*s->intf)->ReadPipeAsync(s->intf, s->pipe_in, s->in_buf, s->in_packet_bytes, read_done, s);

    if (result == kIOReturnSuccess)
        s->read_pending = 1;
    else
        s->read_error = result;
}

/*
 * Builds the ask for one block (0 is the settings block) and checks it. Returns 0, and
 * says why, if it isn't an ask that sets nothing.
 */
static int build_block_request(unsigned block, uint8_t request[UA101_CTL_READ_REQUEST_BYTES])
{
    if (!ua101_ctl_block_request(block, request)) {
        printf("  There is no block %u to ask for, so nothing was sent.\n", block);
        return 0;
    }
    if (!ua101_ctl_is_request_only(request, UA101_CTL_READ_REQUEST_BYTES)) {
        printf("  The check refused the message, so nothing was sent.\n");
        return 0;
    }
    return 1;
}

/*
 * The only send in this program. The message is built here from a block number, and
 * has to pass the check before it can go anywhere.
 */
static IOReturn send_block_request(session *s, unsigned block)
{
    uint8_t request[UA101_CTL_READ_REQUEST_BYTES];
    uint8_t packets[UA101_CTL_READ_PACKET_BYTES];
    size_t packed;

    if (!build_block_request(block, request)) {
        printf("  The check refused the message, so nothing was sent.\n");
        return kIOReturnBadArgument;
    }
    packed =
        ua101_ctl_pack_sysex(request, sizeof(request), UA101_CTL_CABLE, packets, sizeof(packets));
    if (packed != sizeof(packets)) {
        printf("  The message didn't pack as expected, so nothing was sent.\n");
        return kIOReturnBadArgument;
    }
    printf("  Sending on cable 1: ");
    print_hex(request, sizeof(request));
    printf("\n");
    return (*s->intf)->WritePipeTO(s->intf, s->pipe_out, packets, (UInt32)packed, WRITE_TIMEOUT_MS,
                                   WRITE_TIMEOUT_MS);
}

/* Runs callbacks until the time is up, *flag turns 1, or something stops it. */
static void run_loop_until_flag(session *s, double seconds, const int *flag)
{
    CFAbsoluteTime deadline = CFAbsoluteTimeGetCurrent() + seconds;

    while (CFAbsoluteTimeGetCurrent() < deadline && !g_signal && s->read_error == 0 && !*flag)
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, false);
}

/* Runs callbacks until the time is up, the settings arrive (if asked), or something stops it. */
static void run_loop_until(session *s, double seconds, int stop_when_settings)
{
    CFAbsoluteTime deadline = CFAbsoluteTimeGetCurrent() + seconds;

    while (CFAbsoluteTimeGetCurrent() < deadline && !g_signal && s->read_error == 0 &&
           !(stop_when_settings && s->have_settings))
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, false);
}

static const char *on_off(uint8_t value)
{
    return value ? "on" : "off";
}

static void print_group(const ua101_settings *st, unsigned group, const char *name)
{
    unsigned channels = ua101_ctl_group_channels(group);
    unsigned c;

    printf("\n  %s\n", name);
    printf("    ch  on-bit  link  pan      volume     level\n");
    for (c = 0; c < channels; c++) {
        const ua101_ctl_channel *ch = &st->channel[group][c];
        double db = ua101_ctl_volume_db(ch->volume);
        char level[32];

        if (isinf(db))
            snprintf(level, sizeof(level), "silent");
        else
            snprintf(level, sizeof(level), "%+.1f dB", db);
        printf("    %2u  %u       %u     0x%04x   0x%06x   %s\n", c + 1, ch->on,
               st->link[group][c / 2], ch->pan, ch->volume, level);
    }
}

/* Returns the number of values that don't fit the protocol notes. */
static unsigned print_settings(const session *s, uint32_t descriptor_rate)
{
    ua101_settings st;
    uint32_t rate;
    unsigned problems = 0, row, output, group, c;

    ua101_settings_parse(s->block, &st);
    rate = ua101_ctl_rate_hz(st.rate_code);

    printf("\nSwitches:\n");
    printf("  Section flags: %u %u %u %u (switches, patch row 0, patch rows 1-2, mixer)\n",
           st.section_flags[0], st.section_flags[1], st.section_flags[2], st.section_flags[3]);
    printf("  Sample rate:   code %u, which should mean %u Hz. The USB descriptors say %u Hz: ",
           st.rate_code, (unsigned)rate, (unsigned)descriptor_rate);
    if (rate == descriptor_rate) {
        printf("match\n");
    } else {
        printf("DIFFERENT\n");
        problems++;
    }
    printf("  Soft Control:  %s\n", on_off(st.soft_control));
    printf("  Mono:          %s\n", on_off(st.mono));
    printf("  Clock:         source %u, status %u\n", st.clock_source, st.clock_status);
    printf("  Unknown bits:  d[4] bit 3 = %u, d[6] bit 0 = %u\n", st.d4_bit3, st.d6_bit0);

    printf("\nPatch bay (row 1, the one the UA-101's dialog uses):\n");
    for (output = 0; output < UA101_CTL_PATCH_OUTPUTS; output++) {
        const char *source = ua101_ctl_patch_source(st.patch[1][output]);

        printf("  OUTPUT %u-%u:  %s (%u)\n", output * 2 + 1, output * 2 + 2,
               source != NULL ? source : "UNKNOWN VALUE", st.patch[1][output]);
        if (source == NULL)
            problems++;
    }
    for (row = 0; row < UA101_CTL_PATCH_ROWS; row += 2) {
        printf("  Row %u, raw:", row);
        for (output = 0; output < UA101_CTL_PATCH_OUTPUTS; output++)
            printf(" %u", st.patch[row][output]);
        printf("\n");
    }

    printf("\nDirect monitor mixer (ignored by the unit while Soft Control is off):\n");
    printf("  Solo: input group %u, output group %u (0: no solo; 1 to 10: that channel alone)\n",
           st.solo[0], st.solo[1]);
    print_group(&st, 0, "MONITOR OUT");
    print_group(&st, 1, "INPUT");
    print_group(&st, 2, "OUTPUT from the computer");

    for (group = 0; group < UA101_CTL_GROUPS; group++) {
        for (c = 0; c < ua101_ctl_group_channels(group); c++) {
            if (st.channel[group][c].pan > MAX_PAN || st.channel[group][c].volume > MAX_VOLUME)
                problems++;
        }
    }
    if (st.bad_nibbles != 0) {
        printf("\n  %u bytes in value fields were above 0x0F.\n", st.bad_nibbles);
        problems++;
    }

    printf("\nRaw settings block:\n");
    for (c = 0; c < UA101_CTL_BLOCK_BYTES; c += 16) {
        printf("  %02x: ", c);
        print_hex(s->block + c, 16);
        printf("\n");
    }
    return problems;
}

static void print_block(const uint8_t *block)
{
    unsigned c;

    for (c = 0; c < UA101_CTL_BLOCK_BYTES; c += 16) {
        printf("    %02x: ", c);
        print_hex(block + c, 16);
        printf("\n");
    }
}

/* Prints which bytes differ from the settings block in use, as ranges. */
static unsigned print_differences(const uint8_t *block, const uint8_t *in_use)
{
    unsigned i = 0, count = 0, ranges = 0;

    printf("    Differs from 02 00 in bytes:");
    while (i < UA101_CTL_BLOCK_BYTES) {
        unsigned start;

        if (block[i] == in_use[i]) {
            i++;
            continue;
        }
        start = i;
        while (i < UA101_CTL_BLOCK_BYTES && block[i] != in_use[i])
            i++;
        count += i - start;
        if (ranges++ < 24)
            printf(i - start == 1 ? " %u" : " %u-%u", start, i - 1);
    }
    printf(count == 0 ? " none (the same)\n" : (ranges > 24 ? " ... (%u bytes)\n" : " (%u bytes)\n"),
           count);
    return count;
}

/* --all-blocks --dry-run: what would be sent, with no unit. */
static int dry_run(void)
{
    uint8_t request[UA101_CTL_READ_REQUEST_BYTES];
    unsigned b, refused = 0;

    printf("=== UA-101 all blocks, dry run: nothing is opened or sent ===\n");
    printf("In this order, one at a time, each after the last one's answer (or %.0f s):\n",
           REPLY_WAIT_SECONDS);
    for (b = 0; b <= UA101_CTL_BLOCKS; b++) {
        unsigned block = b % UA101_CTL_BLOCKS;

        if (!build_block_request(block, request)) {
            refused++;
            continue;
        }
        printf("  02 %02x%s: ", block, b == UA101_CTL_BLOCKS ? " again" : "      ");
        print_hex(request, sizeof(request));
        printf("   ask only: %s; the helper would pass it: %s\n",
               ua101_ctl_is_request_only(request, sizeof(request)) ? "yes" : "NO",
               ua101_ctl_is_read_request(request, sizeof(request)) ? "yes" : "no");
    }
    printf("=== Dry run: %s ===\n", refused == 0 ? "17 asks, all pass the check" : "REFUSED");
    return refused == 0 ? 0 : 1;
}

/* After the settings block: the other 15, then the settings block again. Returns 1 if all went. */
static int read_other_blocks(session *s)
{
    unsigned b, answered = 0, same;

    printf("\nThe other blocks, one at a time:\n");
    for (b = 1; b <= UA101_CTL_BLOCKS && !g_signal && s->read_error == 0; b++) {
        int *flag = b < UA101_CTL_BLOCKS ? &s->have_block[b] : &s->have_again;

        usleep(BLOCK_PAUSE_US);
        if (send_block_request(s, b % UA101_CTL_BLOCKS) != kIOReturnSuccess) {
            printf("  Sending failed, so nothing more is sent.\n");
            return 0;
        }
        run_loop_until_flag(s, REPLY_WAIT_SECONDS, flag);
        if (b < UA101_CTL_BLOCKS)
            answered += *flag != 0;
        printf("  02 %02x%s: %s\n", b % UA101_CTL_BLOCKS, b < UA101_CTL_BLOCKS ? "" : " again",
               *flag ? "answered" : "NO REPLY");
    }
    if (g_signal || s->read_error != 0)
        return 0;

    printf("\nBlocks (02 03 to 02 07 are probably Memory 1 to 5):\n");
    for (b = 1; b < UA101_CTL_BLOCKS; b++) {
        printf("\n  Block 02 %02x:%s\n", b, s->have_block[b] ? "" : " no reply");
        if (!s->have_block[b])
            continue;
        print_differences(s->blocks[b], s->block);
        print_block(s->blocks[b]);
    }
    same = s->have_again && memcmp(s->again, s->block, sizeof(s->block)) == 0;
    printf("\n%u of 15 other blocks answered. The settings in use, read again at the end: %s\n",
           answered, !s->have_again ? "NO REPLY" : same ? "the same as at the start" : "CHANGED");
    if (s->have_again && !same)
        print_differences(s->again, s->block);
    return s->have_again && same;
}

int main(int argc, char **argv)
{
    session *s = &g_session;
    double watch_seconds = 0.0;
    struct sigaction action;
    ua101_config config;
    const ua101_alt *midi_alt, *audio_alt;
    io_service_t service;
    device_ref device = NULL;
    CFRunLoopSourceRef source = NULL;
    IOReturn result;
    int exit_code = 1, source_added = 0, opened = 0, i;
    uint32_t descriptor_rate = 0;
    unsigned problems = 0;
    int dry = 0, blocks_ok = 1;

    setvbuf(stdout, NULL, _IOLBF, 0);
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--watch") == 0 && i + 1 < argc) {
            watch_seconds = atof(argv[++i]);
        } else if (strcmp(argv[i], "--all-blocks") == 0) {
            s->all_blocks = 1;
        } else if (strcmp(argv[i], "--dry-run") == 0) {
            dry = 1;
        } else {
            printf("Usage: ua101settings [--watch SECONDS] [--all-blocks [--dry-run]]\n"
                   "  Reads the unit's settings. It can't change them.\n"
                   "  --watch SECONDS  then keeps listening for changes, such as a front panel\n"
                   "                   switch being moved, up to %.0f s\n"
                   "  --all-blocks     also asks for blocks 02 01 to 02 0F (developer's test)\n"
                   "  --dry-run        with --all-blocks: only prints what it would send\n",
                   MAX_WATCH_SECONDS);
            return 2;
        }
    }
    if (dry && !s->all_blocks) {
        printf("--dry-run goes with --all-blocks.\n");
        return 2;
    }
    if (dry)
        return dry_run();
    if (!(watch_seconds >= 0.0) || watch_seconds > MAX_WATCH_SECONDS) {
        printf("--watch must be between 0 and %.0f seconds.\n", MAX_WATCH_SECONDS);
        return 2;
    }

    memset(&action, 0, sizeof(action));
    action.sa_handler = on_signal;
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    ua101_sysex_rx_init(&s->rx);

    printf("=== UA-101 settings (read-only) ===\n");

    service = find_device();
    if (service == IO_OBJECT_NULL) {
        printf("No UA-101 in Hi-Speed mode was found. Run ua101probe for details.\n");
        return 1;
    }
    device = open_device_object(service);
    IOObjectRelease(service);
    if (device == NULL) {
        printf("Couldn't get a device object.\n");
        return 1;
    }
    if (!read_config(device, &config))
        goto done;

    midi_alt = ua101_find_alt(&config, UA101_IF_MIDI, 0);
    audio_alt = ua101_find_alt(&config, UA101_IF_CAPTURE, 1);
    if (midi_alt == NULL || audio_alt == NULL || !audio_alt->has_format) {
        printf("The descriptors don't have the expected interfaces.\n");
        goto done;
    }
    descriptor_rate = audio_alt->rate_hz;

    printf("\nConnection:\n");
    s->intf = find_interface(device, UA101_IF_MIDI);
    if (s->intf == NULL) {
        printf("  Couldn't find the MIDI interface.\n");
        goto done;
    }
    result = (*s->intf)->USBInterfaceOpen(s->intf);
    if (result != kIOReturnSuccess) {
        printf("  Couldn't open the MIDI interface: %s\n", describe(result));
        if (result == kIOReturnExclusiveAccess)
            printf("  Another program is using it. Quit MIDI apps and any UA-101 test.\n");
        goto done;
    }
    opened = 1;
    if (!find_pipes(s, midi_alt))
        goto done;

    result = (*s->intf)->CreateInterfaceAsyncEventSource(s->intf, &source);
    if (result != kIOReturnSuccess) {
        printf("  Couldn't create the event source: %s\n", describe(result));
        goto done;
    }
    CFRunLoopAddSource(CFRunLoopGetCurrent(), source, kCFRunLoopDefaultMode);
    source_added = 1;

    s->in_buf = calloc(1, s->in_packet_bytes);
    if (s->in_buf == NULL) {
        printf("  Out of memory.\n");
        goto done;
    }

    /* Listening starts first, so a quick reply can't be missed. */
    s->start_time = CFAbsoluteTimeGetCurrent();
    start_read(s);
    if (s->read_error != 0) {
        printf("  Couldn't start reading: %s\n", describe(s->read_error));
        goto stop;
    }
    result = send_block_request(s, 0);
    if (result != kIOReturnSuccess) {
        printf("  Sending failed: %s\n", describe(result));
        goto stop;
    }

    run_loop_until(s, REPLY_WAIT_SECONDS, 1);

    printf("  Received %llu bytes; %llu packets on other cables; %llu other packets on this one\n",
           (unsigned long long)s->bytes_received, (unsigned long long)s->rx.other_cable_packets,
           (unsigned long long)s->rx.ignored_packets);
    if (s->read_error != 0)
        printf("  Reading failed: %s\n", describe(s->read_error));

    if (s->have_settings) {
        printf("  Reply: a settings block with a valid checksum\n");
        problems = print_settings(s, descriptor_rate);
        if (s->all_blocks)
            blocks_ok = read_other_blocks(s);
    } else {
        printf("\nNo settings block arrived within %.0f s. Nothing was changed, and nothing "
               "more was sent.\n",
               REPLY_WAIT_SECONDS);
    }

    if (watch_seconds > 0.0 && s->read_error == 0 && !g_signal) {
        printf("\nListening for %.0f s. Move the Mono or Soft Control switch now.\n", watch_seconds);
        s->watching = 1;
        run_loop_until(s, watch_seconds, 0);
        printf("Received %u value changes.\n", s->change_messages);
    }

    if (s->checksum_failures || s->format_failures || s->other_messages)
        printf("\nAlso received: %u with a bad checksum, %u unrecognized, %u others.\n",
               s->checksum_failures, s->format_failures, s->other_messages);

    exit_code = (s->have_settings && problems == 0 && s->checksum_failures == 0 && blocks_ok) ? 0 : 1;
    printf("\n=== Settings read: %s ===\n",
           exit_code == 0        ? "OK"
           : s->have_settings    ? "DONE, but the values raise questions (see above)"
                                 : "NO REPLY");

stop:
    s->stopping = 1;
    if (s->read_pending) {
        CFAbsoluteTime deadline = CFAbsoluteTimeGetCurrent() + 1.0;

        (*s->intf)->AbortPipe(s->intf, s->pipe_in);
        while (s->read_pending && CFAbsoluteTimeGetCurrent() < deadline)
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, false);
    }

done:
    if (source_added)
        CFRunLoopRemoveSource(CFRunLoopGetCurrent(), source, kCFRunLoopDefaultMode);
    if (s->intf != NULL) {
        if (opened)
            (*s->intf)->USBInterfaceClose(s->intf);
        (*s->intf)->Release(s->intf);
    }
    free(s->in_buf);
    if (device != NULL)
        (*device)->Release(device);
    return exit_code;
}
