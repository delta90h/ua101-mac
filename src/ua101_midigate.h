/*
 * The gate between MIDI clients and the unit's MIDI IN and OUT sockets, for the helper
 * (src/ua101_service.c). A MIDI client is a program of the Mac's user that publishes the unit's
 * MIDI port to apps: the per-user agent (src/ua101midiagent.c). No Apple APIs here, so all of
 * it is tested without a unit (tests/test_midigate.c).
 *
 * The unit's MIDI interface carries two things on one pair of USB pipes (src/ua101_midi.h):
 *   cable 0  the MIDI IN and OUT sockets;
 *   cable 1  Roland's control messages, which change the unit's mixer and stored settings.
 * The rules kept here:
 *
 * FROM THE UNIT (ua101_midisplit). Only the bytes of cable 0's packets are given out for MIDI
 * clients. Cable 1 is for control apps alone (the service puts those messages together with
 * src/ua101_control.h, which itself keeps cable 1 only), and any other cable goes nowhere.
 *
 * TO THE UNIT. A MIDI client hands over MIDI bytes, never USB packets, so it can't name a
 * cable at all. Each client's bytes go through a parser of its own and through
 * src/ua101_midi.c's packer, which can only write cable 0 (ua101_midigate_feed). What may then
 * leave is decided twice, by two checks that share no code:
 *   - Roland SysEx addressed to the UA-101 itself (F0 41 <device> 00 69 ...) is dropped whole.
 *     It is not known whether the unit acts on such a message when it arrives on cable 0, so no
 *     MIDI app may change the unit's settings by that route; changes go through the control
 *     app and the helper's rules (send_to_unit) alone. The first check reads the client's
 *     bytes as the parser gives them; the last check, just before the port, reads the packets
 *     and stops such a message at its address, so at most "F0 41 <device>" could ever leave.
 *   - Every packet's cable is looked at again just before the port: anything but cable 0 is
 *     not sent.
 * Everything else passes, as on any MIDI port.
 *
 * PACING. MIDI OUT drains at 3,125 bytes a second. Packets wait in a queue with a fixed size
 * and are let out at that speed (ua101_midigate_take), a few at a time, so the unit's USB pipe
 * never fills and a write to it never waits: a long SysEx can't hold up the helper's service
 * queue or the control app's messages, which use the same pipe. When the queue is full, a
 * short message is dropped whole and a SysEx is cut where it stands (its remaining bytes are
 * dropped up to its end, and it is never closed with F7, so a receiver throws it away); both
 * are counted. The last part of the queue is kept for short messages, so a note off still
 * finds room behind a SysEx that filled the rest.
 *
 * MORE THAN ONE CLIENT. While one client's SysEx is on its way, another client's messages
 * (but for single real-time bytes) are dropped and counted: put between the SysEx's packets
 * they would cut it at the receiver.
 */
#ifndef UA101_MIDIGATE_H
#define UA101_MIDIGATE_H

#include <stddef.h>
#include <stdint.h>

#include "ua101_midi.h"

/* --- From the unit. --- */

typedef struct {
    uint64_t midi_packets;    /* cable 0: their bytes were given out for MIDI clients */
    uint64_t midi_bytes;
    uint64_t control_packets; /* cable 1: for control apps only */
    uint64_t other_packets;   /* any other cable: nowhere */
    uint64_t malformed;       /* cable 0 packets that are neither valid nor padding: nowhere */
    uint64_t no_room;         /* cable 0 packets left out because `midi` was too small */
} ua101_midisplit_counts;

/*
 * Sorts one read from the unit, `bytes` of 4-byte USB-MIDI packets of any cable. Copies the
 * MIDI bytes of cable 0's packets, in order, to `midi` and returns how many: 3 bytes of room
 * for every packet is always enough. Nothing of any other cable is copied.
 */
size_t ua101_midisplit(const uint8_t *packets, size_t bytes, uint8_t *midi, size_t capacity,
                       ua101_midisplit_counts *counts);

/* --- To the unit. --- */

/* One MIDI byte on the wire: 10 bits at 31,250 bits a second. */
#define UA101_MIDIGATE_BYTE_US 320u
/* What may leave at once after a pause: about 15 ms of the wire. */
#define UA101_MIDIGATE_BURST_BYTES 48u
/* While packets wait, the caller comes back this often. */
#define UA101_MIDIGATE_TICK_US 10000u
/* The queue: 64 KB of packets, about 15 s of SysEx at the wire's speed. */
#define UA101_MIDIGATE_QUEUE_PACKETS 16384u
/* Of those, kept free for short messages. */
#define UA101_MIDIGATE_SHORT_RESERVE_PACKETS 1024u

typedef struct {
    uint64_t bytes_in;          /* MIDI bytes handed in by clients */
    uint64_t invalid_bytes;     /* of those, bytes MIDI 1.0 doesn't allow: dropped */
    uint64_t sysex_cut;         /* SysEx a client ended without F7 */
    uint64_t own_sysex;         /* SysEx addressed to the UA-101 itself: dropped whole */
    uint64_t overflow_messages; /* with the queue full: short messages dropped, SysEx cut */
    uint64_t busy_messages;     /* dropped because another client's SysEx was on its way */
    uint64_t packets_queued;
    uint64_t packets_sent;      /* given out for the port */
    uint64_t bytes_sent;        /* the MIDI bytes in them */
    uint64_t gate_wrong_cable;  /* the last check: packets not for cable 0, not given out */
    uint64_t gate_own_sysex;    /* the last check: SysEx for the UA-101 itself, stopped */
    uint64_t gate_malformed;    /* the last check: packets with a reserved code, not given out */
    uint64_t flushed_packets;   /* thrown away when the port closed or didn't take a write */
} ua101_midigate_counts;

/* The queue, the pacing and the last check: one for the unit's MIDI OUT socket. */
typedef struct {
    uint8_t ring[UA101_MIDIGATE_QUEUE_PACKETS * UA101_MIDI_PACKET_BYTES];
    uint32_t head, count;    /* in packets */
    uint64_t credit_us;      /* wire time saved up, at most the burst */
    uint64_t last_us;
    int primed;
    const void *wire_owner;  /* the feed whose SysEx is on its way, or NULL: only it may
                                queue SysEx packets */
    /* The last check's own view of the SysEx that is leaving. */
    uint8_t out_pre[5];
    uint8_t out_seen, out_in_sysex, out_blocked;
    ua101_midigate_counts counts;
} ua101_midigate;

/* One per MIDI client: where its byte stream stands. */
typedef struct {
    ua101_midi_parser parser;
    ua101_usbmidi_packer packer;
    uint8_t held[5];   /* a SysEx's first bytes, until it is known whom it is for */
    uint8_t held_count;
    uint8_t mode;
} ua101_midifeed;

void ua101_midigate_init(ua101_midigate *g);
void ua101_midifeed_init(ua101_midifeed *f);

/* A client's MIDI bytes, as many or as few as it likes at a time: what passes is queued. */
void ua101_midigate_feed(ua101_midigate *g, ua101_midifeed *f, const uint8_t *bytes, size_t len);

/* The client is gone: a SysEx it left half sent no longer holds the others back. */
void ua101_midigate_release(ua101_midigate *g, ua101_midifeed *f);

/*
 * What may go to the port now, `now_us` being a clock in microseconds that never goes back:
 * copies whole packets to `out` and returns their bytes (0: nothing now). Each was checked
 * again as described above. Room for UA101_MIDIGATE_BURST_BYTES packets is always enough.
 */
size_t ua101_midigate_take(ua101_midigate *g, uint64_t now_us, uint8_t *out, size_t capacity);

/* Packets are waiting: come back in UA101_MIDIGATE_TICK_US. */
int ua101_midigate_pending(const ua101_midigate *g);

/* The port closed, or didn't take a write: everything waiting is thrown away, and every SysEx
   on its way is over (the rest of it is dropped as its client hands it in). */
void ua101_midigate_flush(ua101_midigate *g);

#ifdef UA101_MIDIGATE_TESTING
/* For tests/test_midigate.c only, never built into the helper. Leaves out one check, to show
   that the other keeps the rule alone: 1 the first check for SysEx addressed to the UA-101,
   2 the last check for the same, 4 the last check of the cable. */
extern unsigned ua101_midigate_test_skip;
/* Puts a packet into the queue as it is, which no client can: for the last check of the
   cable, since the packer can't make a packet for another one. Returns 1 if there was room. */
int ua101_midigate_test_enqueue(ua101_midigate *g, const uint8_t packet[UA101_MIDI_PACKET_BYTES]);
#endif

#endif
