/*
 * The CoreMIDI side of the per-user agent (src/ua101midiagent.c): the unit's MIDI IN and OUT
 * sockets as a virtual source and destination, "UA-101 MIDI", that every MIDI app sees like
 * any other MIDI port. It holds no USB code at all: it is a MIDI client of the helper
 * (src/ua101_midilink.h), which owns the unit and decides what may reach it
 * (src/ua101_midigate.h).
 *
 * The ports exist while the helper says the unit is there: made when an attach succeeds, and
 * removed when the unit goes, is switched off, or the helper can't be reached. They carry
 * fixed IDs (ua101_midi_port_id), so an app that remembers a port finds the same one when the
 * unit comes back. Nothing here keeps a timer running while it is attached, or while the
 * helper has promised to say when a unit appears.
 *
 * It makes only its own two ports. It never opens, lists or connects to any other MIDI source
 * or destination, so other MIDI devices on the Mac are not touched.
 */
#ifndef UA101_MIDIPORTS_H
#define UA101_MIDIPORTS_H

#include <dispatch/dispatch.h>
#include <stdint.h>
#include <stdio.h>

#define UA101_MIDIPORTS_NAME "UA-101 MIDI"

typedef struct {
    const char *name; /* both ports' name; NULL for UA101_MIDIPORTS_NAME */
    /* For a test: the ports are hidden from other apps and get no fixed IDs. */
    int private_ports;
    FILE *log; /* NULL for none */
    /* CoreMIDI stopped working under the ports (its server went away). Nothing can be put
       right from inside: the agent exits and launchd starts it afresh. */
    void (*broken)(void *context, const char *why);
    void *context;
} ua101_midiports_options;

typedef struct {
    int attached;            /* the helper holds the unit for us */
    int ports_up;            /* the source and destination exist */
    uint32_t source, destination; /* their CoreMIDI references, 0 when they don't */
    uint64_t in_bytes;       /* MIDI bytes from the unit */
    uint64_t in_messages;    /* messages given to apps */
    uint64_t in_errors;      /* messages CoreMIDI didn't take */
    uint64_t out_bytes;      /* MIDI bytes from apps handed to the helper */
    uint64_t out_refused;    /* packets from apps that MIDI 1.0 can't carry */
    uint64_t out_unattached; /* bytes from apps that came while not attached: dropped */
    uint64_t attaches;       /* answers to an attach, of any kind */
} ua101_midiports_counts;

typedef struct ua101_midiports ua101_midiports;

/* Starts on `queue`, a serial queue: asks the helper for the unit, and from then on follows
   what it says. Returns NULL only when out of memory. */
ua101_midiports *ua101_midiports_start(dispatch_queue_t queue,
                                       const ua101_midiports_options *options);

/* Removes the ports and lets go of the helper. Call it on the queue; nothing of it is used
   after the queue has run what was already on its way. */
void ua101_midiports_stop(ua101_midiports *a);

/* Call it on the queue. */
void ua101_midiports_get_counts(ua101_midiports *a, ua101_midiports_counts *out);

#endif
