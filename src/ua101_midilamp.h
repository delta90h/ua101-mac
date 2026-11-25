/*
 * The control app's MIDI IN and MIDI OUT lamps, the helper's side (link version 15, the user's
 * option B of 2026-11-17). When MIDI passes one of the unit's MIDI sockets, the helper tells
 * each attached control app with a short event, "midi_lamps", carrying two flags:
 *   in   MIDI bytes arrived at the unit's MIDI IN socket (cable 0, ua101_midisplit);
 *   out  MIDI bytes really left through the unit's MIDI OUT socket: packets the gate let out
 *        (ua101_midigate_take) and the unit took (ua101_midiport_send). Bytes a client handed
 *        over that the gate dropped, or that the unit didn't take, never light it.
 * The control app's own messages (cable 1) light neither: they are not MIDI.
 *
 * Design choices (2026-11-17): at most 10 events a second (UA101_MIDILAMP_GAP_US apart);
 * active sensing (0xFE) alone lights nothing, so an idle keyboard leaves the lamp dark; every
 * other byte counts, MIDI clock too. The app keeps a lamp lit 0.15 s after each event.
 *
 * NO TIMER WHILE MIDI IS QUIET. The first MIDI after a gap is told at once. MIDI within the gap
 * is remembered, and the caller sets ONE one-shot wake-up for the end of the gap; when it comes
 * the remembered flags are told and nothing more is booked. So after the last MIDI at most one
 * wake-up follows, within 0.1 s, and then nothing runs until MIDI passes again.
 *
 * No Apple APIs and no clock of its own: time is handed in, so all of it is tested with time
 * moved by hand (tests/test_midilamp.c). Header only, so every program that builds the helper's
 * service gets it without a change to its list of files.
 */
#ifndef UA101_MIDILAMP_H
#define UA101_MIDILAMP_H

#include <stddef.h>
#include <stdint.h>

/* The two flags. */
#define UA101_MIDILAMP_IN 1u
#define UA101_MIDILAMP_OUT 2u
/* Events are at least this far apart: 10 a second at most. */
#define UA101_MIDILAMP_GAP_US 100000u
/* Active sensing: a keyboard's "still here", about 3 times a second while nobody plays. */
#define UA101_MIDILAMP_ACTIVE_SENSING 0xFEu

typedef struct {
    uint64_t last_us; /* when the last event was told */
    int told_any;     /* an event was told since init */
    unsigned pending; /* flags seen within the gap, not told yet */
} ua101_midilamp;

static inline void ua101_midilamp_init(ua101_midilamp *m)
{
    m->last_us = 0;
    m->told_any = 0;
    m->pending = 0;
}

/*
 * MIDI passed now (`flags`, 0 for "the wake-up came"). Returns the flags to tell now, 0 for
 * nothing. `*wake_at_us` is set to when the caller must come back with flags 0, or 0 for never:
 * then no wake-up may stay booked.
 */
static inline unsigned ua101_midilamp_step(ua101_midilamp *m, unsigned flags, uint64_t now_us,
                                           uint64_t *wake_at_us)
{
    m->pending |= flags & (UA101_MIDILAMP_IN | UA101_MIDILAMP_OUT);
    *wake_at_us = 0;
    if (m->pending == 0)
        return 0;
    if (!m->told_any || now_us < m->last_us || now_us - m->last_us >= UA101_MIDILAMP_GAP_US) {
        unsigned tell = m->pending;

        m->pending = 0;
        m->last_us = now_us;
        m->told_any = 1;
        return tell;
    }
    *wake_at_us = m->last_us + UA101_MIDILAMP_GAP_US;
    return 0;
}

/* Whether plain MIDI bytes (from ua101_midisplit) light the IN lamp: any byte but active
   sensing. */
static inline int ua101_midilamp_bytes_count(const uint8_t *bytes, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++)
        if (bytes[i] != UA101_MIDILAMP_ACTIVE_SENSING)
            return 1;
    return 0;
}

/* Whether USB-MIDI packets the unit took for MIDI OUT light the OUT lamp: any packet with MIDI
   bytes in it (code 2 to 15) but one that is active sensing alone. The gate gives out cable 0
   only; a packet of another cable lights nothing here either. */
static inline int ua101_midilamp_packets_count(const uint8_t *packets, size_t bytes)
{
    size_t i;

    for (i = 0; i + 4 <= bytes; i += 4) {
        unsigned cable = packets[i] >> 4, code = packets[i] & 0x0Fu;

        if (cable != 0 || code < 2)
            continue;
        if (code == 0xF && packets[i + 1] == UA101_MIDILAMP_ACTIVE_SENSING)
            continue;
        return 1;
    }
    return 0;
}

#endif
