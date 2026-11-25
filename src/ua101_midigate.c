#include "ua101_midigate.h"

#include <string.h>

/* Where a client's SysEx stands. */
enum {
    FEED_IDLE = 0,
    FEED_DECIDING, /* its first bytes are held: is it for the UA-101 itself? */
    FEED_PASSING,  /* it is on its way to the queue, and holds the wire */
    FEED_DROPPING  /* the rest of it is thrown away, up to its end */
};

/* Roland SysEx for this model: F0 41 <device> 00 69. The device byte can be anything. */
#define ROLAND_ID 0x41
#define MODEL_ID_HIGH 0x00
#define MODEL_ID_LOW 0x69

#ifdef UA101_MIDIGATE_TESTING
unsigned ua101_midigate_test_skip;
#define SKIPPED(bit) (ua101_midigate_test_skip & (bit))
#else
#define SKIPPED(bit) 0
#endif

/* --- From the unit. --- */

size_t ua101_midisplit(const uint8_t *packets, size_t bytes, uint8_t *midi, size_t capacity,
                       ua101_midisplit_counts *counts)
{
    size_t used = 0, i;

    for (i = 0; i + UA101_MIDI_PACKET_BYTES <= bytes; i += UA101_MIDI_PACKET_BYTES) {
        const uint8_t *p = packets + i;
        uint8_t cable, message[3];
        int malformed;
        size_t n;

        if ((p[0] | p[1] | p[2] | p[3]) == 0) /* the padding at the end of a read */
            continue;
        /* The cable decides first, whatever else the packet holds. */
        switch (ua101_midi_route_for(p[0] >> 4)) {
        case UA101_ROUTE_CONTROL:
            counts->control_packets++;
            continue;
        case UA101_ROUTE_DROP:
            counts->other_packets++;
            continue;
        case UA101_ROUTE_APPS:
            break;
        }
        n = ua101_usbmidi_unpack(p, &cable, message, &malformed);
        if (malformed || n == 0 || cable != UA101_MIDI_CABLE) {
            counts->malformed++;
            continue;
        }
        if (used + n > capacity) {
            counts->no_room++;
            continue;
        }
        memcpy(midi + used, message, n);
        used += n;
        counts->midi_packets++;
        counts->midi_bytes += n;
    }
    return used;
}

/* --- To the unit: from a client into the queue. --- */

void ua101_midigate_init(ua101_midigate *g)
{
    memset(g, 0, sizeof(*g));
}

void ua101_midifeed_init(ua101_midifeed *f)
{
    memset(f, 0, sizeof(*f));
    ua101_midi_parser_init(&f->parser);
    ua101_usbmidi_packer_init(&f->packer);
    f->mode = FEED_IDLE;
}

/* SysEx stops short of the last part of the queue, which is kept for short messages. */
static int enqueue(ua101_midigate *g, const uint8_t packet[UA101_MIDI_PACKET_BYTES], int sysex)
{
    uint32_t limit = sysex ? UA101_MIDIGATE_QUEUE_PACKETS - UA101_MIDIGATE_SHORT_RESERVE_PACKETS
                           : UA101_MIDIGATE_QUEUE_PACKETS;
    uint32_t tail;

    if (g->count >= limit)
        return 0;
    tail = (g->head + g->count) % UA101_MIDIGATE_QUEUE_PACKETS;
    memcpy(g->ring + (size_t)tail * UA101_MIDI_PACKET_BYTES, packet, UA101_MIDI_PACKET_BYTES);
    g->count++;
    g->counts.packets_queued++;
    return 1;
}

#ifdef UA101_MIDIGATE_TESTING
int ua101_midigate_test_enqueue(ua101_midigate *g, const uint8_t packet[UA101_MIDI_PACKET_BYTES])
{
    return enqueue(g, packet, 0);
}
#endif

/* This client's SysEx is over before its end: what it still hands in of it is thrown away,
   and whatever the packer held back of it never leaves. */
static void stop_sysex(ua101_midigate *g, ua101_midifeed *f)
{
    f->mode = FEED_DROPPING;
    ua101_usbmidi_packer_init(&f->packer);
    if (g->wire_owner == f)
        g->wire_owner = NULL;
}

/* One event of a SysEx that is passing: packed, and queued when it fills a packet. */
static void sysex_event(ua101_midigate *g, ua101_midifeed *f, const ua101_midi_event *ev)
{
    uint8_t packet[UA101_MIDI_PACKET_BYTES];

    /* Only the SysEx that holds the wire may add to the queue. One that lost it (the queue
       was flushed under it, or its client was let go) is over: its start is gone. */
    if (g->wire_owner != f) {
        stop_sysex(g, f);
        return;
    }
    if (!ua101_usbmidi_pack(&f->packer, ev, packet))
        return;
    if (!enqueue(g, packet, 1)) {
        g->counts.overflow_messages++;
        stop_sysex(g, f);
    }
}

/* The held first bytes aren't the UA-101's address: the SysEx takes the wire and they go. */
static void begin_passing(ua101_midigate *g, ua101_midifeed *f)
{
    static const ua101_midi_event start = {UA101_MIDI_SYSEX_START, {0xF0, 0, 0}, 1};
    uint8_t i;

    if (g->wire_owner != NULL && g->wire_owner != f) {
        g->counts.busy_messages++;
        f->mode = FEED_DROPPING;
        return;
    }
    g->wire_owner = f;
    f->mode = FEED_PASSING;
    ua101_usbmidi_packer_init(&f->packer);
    sysex_event(g, f, &start);
    for (i = 1; i < f->held_count && f->mode == FEED_PASSING; i++) {
        ua101_midi_event ev = {UA101_MIDI_SYSEX_BYTE, {f->held[i], 0, 0}, 1};

        sysex_event(g, f, &ev);
    }
}

static void feed_event(ua101_midigate *g, ua101_midifeed *f, const ua101_midi_event *ev)
{
    uint8_t packet[UA101_MIDI_PACKET_BYTES];

    switch (ev->kind) {
    case UA101_MIDI_SHORT:
        /* Another client's SysEx is on its way: only a single real-time byte may go between
           its packets without cutting it. */
        if (ev->bytes[0] < 0xF8 && g->wire_owner != NULL && g->wire_owner != f) {
            g->counts.busy_messages++;
            return;
        }
        if (ua101_usbmidi_pack(&f->packer, ev, packet) && !enqueue(g, packet, 0))
            g->counts.overflow_messages++;
        return;
    case UA101_MIDI_SYSEX_START:
        f->held[0] = 0xF0;
        f->held_count = 1;
        f->mode = FEED_DECIDING;
        return;
    case UA101_MIDI_SYSEX_BYTE:
        if (f->mode == FEED_PASSING) {
            sysex_event(g, f, ev);
        } else if (f->mode == FEED_DECIDING) {
            uint8_t b = ev->bytes[0], n;

            f->held[f->held_count++] = b;
            n = f->held_count; /* 2: the maker, 3: the device, 4 and 5: the model */
            if ((n == 2 && b != ROLAND_ID) || (n == 4 && b != MODEL_ID_HIGH) ||
                (n == 5 && b != MODEL_ID_LOW)) {
                begin_passing(g, f);
            } else if (n == 5) {
                /* F0 41 <device> 00 69: addressed to the UA-101 itself. The first check. */
                if (SKIPPED(1u)) {
                    begin_passing(g, f);
                } else {
                    g->counts.own_sysex++;
                    f->mode = FEED_DROPPING;
                }
            }
        }
        return;
    case UA101_MIDI_SYSEX_END:
        if (f->mode == FEED_DECIDING) /* too short to be addressed to anything of ours */
            begin_passing(g, f);
        if (f->mode == FEED_PASSING)
            sysex_event(g, f, ev);
        if (g->wire_owner == f)
            g->wire_owner = NULL;
        f->mode = FEED_IDLE;
        return;
    case UA101_MIDI_SYSEX_CUT:
        /* Ended without F7. If nothing of it has left yet, nothing does; otherwise what the
           packer held back is dropped and the message stays open, for the receiver to drop. */
        if (f->mode == FEED_PASSING)
            sysex_event(g, f, ev);
        ua101_usbmidi_packer_init(&f->packer);
        if (g->wire_owner == f)
            g->wire_owner = NULL;
        f->mode = FEED_IDLE;
        return;
    default:
        return;
    }
}

void ua101_midigate_feed(ua101_midigate *g, ua101_midifeed *f, const uint8_t *bytes, size_t len)
{
    static const ua101_midi_event cut_event = {UA101_MIDI_SYSEX_CUT, {0}, 0};
    uint64_t dropped = f->parser.dropped, cuts = f->parser.sysex_cut;
    size_t i;

    for (i = 0; i < len; i++) {
        ua101_midi_event ev;
        int cut;

        ua101_midi_parse(&f->parser, bytes[i], &ev, &cut);
        if (cut)
            feed_event(g, f, &cut_event);
        if (ev.kind != UA101_MIDI_NONE)
            feed_event(g, f, &ev);
    }
    g->counts.bytes_in += len;
    g->counts.invalid_bytes += f->parser.dropped - dropped;
    g->counts.sysex_cut += f->parser.sysex_cut - cuts;
}

void ua101_midigate_release(ua101_midigate *g, ua101_midifeed *f)
{
    if (g->wire_owner == f)
        g->wire_owner = NULL;
}

/* --- To the unit: from the queue to the port. --- */

/* USB-MIDI 1.0, table 4-1: the MIDI bytes in a packet of each code. 0 and 1 are reserved. */
static const uint8_t k_code_bytes[16] = {0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1};

/*
 * The last check, on the packet itself, sharing nothing with the checks a client's bytes went
 * through on their way in. Returns 1 if the packet may leave.
 */
static int last_check(ua101_midigate *g, const uint8_t p[UA101_MIDI_PACKET_BYTES])
{
    uint8_t code = p[0] & 0x0F, len = k_code_bytes[code], i;
    int sysex_data, ends = 0, leave;

    if (!SKIPPED(4u) && (p[0] >> 4) != UA101_MIDI_CABLE) {
        g->counts.gate_wrong_cable++;
        return 0;
    }
    if (len == 0 || (code == 0xF && p[1] < 0xF8)) {
        g->counts.gate_malformed++;
        return 0;
    }
    sysex_data = code == 0x4 || code == 0x6 || code == 0x7 || (code == 0x5 && p[1] == 0xF7);
    if (!sysex_data) {
        /* A message of its own ends an open SysEx, as cut; a real-time byte doesn't. */
        if (code != 0xF) {
            g->out_in_sysex = 0;
            g->out_blocked = 0;
        }
        return 1;
    }
    for (i = 0; i < len; i++) {
        uint8_t b = p[1 + i];

        if (b == 0xF0) {
            g->out_in_sysex = 1;
            g->out_blocked = 0;
            g->out_pre[0] = b;
            g->out_seen = 1;
        } else if (b == 0xF7) {
            ends = 1;
        } else if (g->out_in_sysex && g->out_seen < sizeof(g->out_pre)) {
            g->out_pre[g->out_seen++] = b;
            if (g->out_seen == sizeof(g->out_pre) && g->out_pre[1] == ROLAND_ID &&
                g->out_pre[3] == MODEL_ID_HIGH && g->out_pre[4] == MODEL_ID_LOW && !SKIPPED(2u)) {
                /* For the UA-101 itself: this packet, with the model in it, and the rest of
                   the message stay here. */
                g->out_blocked = 1;
                g->counts.gate_own_sysex++;
            }
        }
    }
    leave = !g->out_blocked;
    if (ends) {
        g->out_in_sysex = 0;
        g->out_blocked = 0;
    }
    return leave;
}

size_t ua101_midigate_take(ua101_midigate *g, uint64_t now_us, uint8_t *out, size_t capacity)
{
    const uint64_t most = (uint64_t)UA101_MIDIGATE_BURST_BYTES * UA101_MIDIGATE_BYTE_US;
    size_t used = 0;

    if (!g->primed) {
        g->primed = 1;
        g->credit_us = most;
    } else if (now_us > g->last_us) {
        uint64_t gained = now_us - g->last_us;

        g->credit_us = gained >= most || g->credit_us + gained >= most ? most
                                                                       : g->credit_us + gained;
    }
    if (now_us > g->last_us || g->last_us == 0)
        g->last_us = now_us;

    while (g->count != 0 && used + UA101_MIDI_PACKET_BYTES <= capacity) {
        const uint8_t *p = g->ring + (size_t)g->head * UA101_MIDI_PACKET_BYTES;
        uint64_t cost = (uint64_t)k_code_bytes[p[0] & 0x0F] * UA101_MIDIGATE_BYTE_US;

        if (g->credit_us < cost)
            break;
        if (last_check(g, p)) {
            memcpy(out + used, p, UA101_MIDI_PACKET_BYTES);
            used += UA101_MIDI_PACKET_BYTES;
            g->credit_us -= cost;
            g->counts.packets_sent++;
            g->counts.bytes_sent += k_code_bytes[p[0] & 0x0F];
        }
        g->head = (g->head + 1) % UA101_MIDIGATE_QUEUE_PACKETS;
        g->count--;
    }
    return used;
}

int ua101_midigate_pending(const ua101_midigate *g)
{
    return g->count != 0;
}

void ua101_midigate_flush(ua101_midigate *g)
{
    g->counts.flushed_packets += g->count;
    g->head = 0;
    g->count = 0;
    g->wire_owner = NULL;
    g->out_in_sysex = 0;
    g->out_blocked = 0;
    g->out_seen = 0;
}
