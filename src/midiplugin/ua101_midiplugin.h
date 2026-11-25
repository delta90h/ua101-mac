/*
 * UA101MIDI.plugin, the CoreMIDI driver plug-in: see
 * ua101_midiplugin.c. Its only exported symbol is the factory, UA101MIDI_Create, named in its
 * Info.plist. The rest of this header is for tests/test_midiplugin.c.
 */
#ifndef UA101_MIDIPLUGIN_H
#define UA101_MIDIPLUGIN_H

#include <CoreFoundation/CoreFoundation.h>
#include <stdint.h>

#define UA101_MIDIPLUGIN_BUNDLE_ID "io.github.delta90h.ua101.midi"
/* The factory's UUID, also in src/midiplugin/Info.plist. */
#define UA101_MIDIPLUGIN_FACTORY "B1DA83CA-F973-4575-BC96-E170D83C112F"
#define UA101_MIDIPLUGIN_DEVICE_NAME "UA-101"
/* What apps show for MIDI IN and OUT: the agent's port name. The fixed IDs come from it. */
#define UA101_MIDIPLUGIN_PORT_NAME "UA-101 MIDI"
/* The entity's and both endpoints' own name. macOS shows a device's port as the device's name,
   a space and the endpoint's name (the IAC Driver's "Bus 1" shows as "IAC Driver Bus 1"; in the
   trial "UA-101 MIDI" showed as "UA-101 UA-101 MIDI"), so "MIDI" shows as "UA-101 MIDI". */
#define UA101_MIDIPLUGIN_ENDPOINT_NAME "MIDI"

/* The factory: the MIDI server calls it with kMIDIDriverTypeID. */
void *UA101MIDI_Create(CFAllocatorRef allocator, CFUUIDRef type);

typedef struct {
    int running;             /* between Start and Stop */
    int attached;            /* the helper holds the unit for us */
    int version;             /* the driver interface asked for */
    uint32_t refs;
    uint64_t in_bytes;       /* MIDI bytes from the unit */
    uint64_t in_messages;    /* messages given to the server */
    uint64_t in_errors;      /* messages the server didn't take */
    uint64_t out_bytes;      /* MIDI bytes from apps handed to the helper */
    uint64_t out_refused;    /* packets from apps that MIDI 1.0 can't carry */
    uint64_t out_unattached; /* bytes from apps that came while not attached: dropped */
    uint64_t attaches;       /* answers to an attach, of any kind */
} ua101_midiplugin_counts;

#ifdef UA101_MIDIPLUGIN_TESTING
void ua101_midiplugin_get_counts(void *driver, ua101_midiplugin_counts *out);
/* Each line the plug-in logs, also handed here. */
extern void (*ua101_midiplugin_test_note)(const char *text);
#endif

#endif
