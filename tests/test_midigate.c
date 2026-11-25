/*
 * The gate between MIDI clients and the unit's MIDI sockets (src/ua101_midigate.h), with no
 * unit and no Apple APIs: which cable goes where, what a client's bytes may become on the way
 * out, the two checks that keep SysEx for the UA-101 itself in, the last check of the cable,
 * the pacing and the full queue. The clock is a number the test counts up.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/ua101_control.h"
#include "../src/ua101_midi.h"
#include "../src/ua101_midigate.h"

static int failures, checks;

#define CHECK(cond, ...)                                                                           \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(cond)) {                                                                             \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                            \
            printf(__VA_ARGS__);                                                                   \
            printf("\n");                                                                          \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

/* The gate is big (its queue): one here, set up afresh by each case. */
static ua101_midigate G;

/* What left for the port, as MIDI bytes, in order. */
static uint8_t g_out[1 << 20];
static size_t g_out_len, g_out_packets;
static int g_out_other_cable, g_out_malformed;

static void out_reset(void)
{
    g_out_len = g_out_packets = 0;
    g_out_other_cable = g_out_malformed = 0;
}

/* One visit to the gate at `now`. Returns the MIDI bytes it gave out. */
static size_t pull(ua101_midigate *g, uint64_t now)
{
    uint8_t packets[4 * UA101_MIDIGATE_BURST_BYTES];
    size_t n = ua101_midigate_take(g, now, packets, sizeof(packets)), i, bytes = 0;

    for (i = 0; i < n; i += 4) {
        uint8_t cable, b[3];
        int malformed;
        size_t len = ua101_usbmidi_unpack(packets + i, &cable, b, &malformed);

        g_out_packets++;
        if ((packets[i] >> 4) != 0)
            g_out_other_cable++;
        if (malformed || len == 0)
            g_out_malformed++;
        if (g_out_len + len <= sizeof(g_out)) {
            memcpy(g_out + g_out_len, b, len);
            g_out_len += len;
        }
        bytes += len;
    }
    return bytes;
}

/* Visits every tick until nothing waits. Returns the microseconds it took. */
static uint64_t drain(ua101_midigate *g, uint64_t *now)
{
    uint64_t start = *now;

    pull(g, *now);
    while (ua101_midigate_pending(g)) {
        *now += UA101_MIDIGATE_TICK_US;
        pull(g, *now);
    }
    return *now - start;
}

static void fresh(ua101_midifeed *a, ua101_midifeed *b)
{
    ua101_midigate_init(&G);
    if (a != NULL)
        ua101_midifeed_init(a);
    if (b != NULL)
        ua101_midifeed_init(b);
    out_reset();
#ifdef UA101_MIDIGATE_TESTING
    ua101_midigate_test_skip = 0;
#endif
}

static int out_is(const uint8_t *want, size_t len)
{
    return g_out_len == len && memcmp(g_out, want, len) == 0;
}

/* Whether what left holds a SysEx that got as far as the UA-101's address: F0 41 <device>
   00 69, real-time bytes between them not counting. That is the thing that must never be. */
static int out_reaches_own_address(void)
{
    uint8_t pre[5];
    size_t i, seen = 0;
    int in = 0;

    for (i = 0; i < g_out_len; i++) {
        uint8_t b = g_out[i];

        if (b >= 0xF8)
            continue;
        if (b == 0xF0) {
            in = 1;
            seen = 1;
            pre[0] = b;
        } else if (b >= 0x80) {
            in = 0;
        } else if (in && seen < 5) {
            pre[seen++] = b;
            if (seen == 5 && pre[1] == 0x41 && pre[3] == 0x00 && pre[4] == 0x69)
                return 1;
        }
    }
    return 0;
}

/* --- From the unit. --- */

static void test_split(void)
{
    static const uint8_t read[] = {
        0x09, 0x90, 0x3C, 0x40, /* cable 0: note on */
        0x14, 0xF0, 0x41, 0x10, /* cable 1: the start of a control message */
        0x14, 0x00, 0x69, 0x12, /* cable 1 */
        0x0F, 0xF8, 0x00, 0x00, /* cable 0: clock */
        0x29, 0x90, 0x40, 0x40, /* cable 2: a note on a cable the unit doesn't have */
        0x04, 0xF0, 0x7D, 0x01, /* cable 0: SysEx starts */
        0x16, 0x01, 0xF7, 0x00, /* cable 1: ends */
        0x06, 0x02, 0xF7, 0x00, /* cable 0: SysEx ends */
        0x09, 0x3C, 0x90, 0x40, /* cable 0, but not what its code says */
        0x00, 0x00, 0x00, 0x00, /* padding */
        0xF8, 0x80, 0x3C, 0x00, /* cable 15 */
        0x08, 0x80, 0x3C, 0x00, /* cable 0: note off */
    };
    static const uint8_t want[] = {0x90, 0x3C, 0x40, 0xF8, 0xF0, 0x7D, 0x01,
                                   0x02, 0xF7, 0x80, 0x3C, 0x00};
    ua101_midisplit_counts c;
    uint8_t midi[64];
    size_t n;
    unsigned cable;

    memset(&c, 0, sizeof(c));
    n = ua101_midisplit(read, sizeof(read), midi, sizeof(midi), &c);
    CHECK(n == sizeof(want) && memcmp(midi, want, n) == 0,
          "one read with four cables in it: only cable 0's bytes come out, in order (%zu)", n);
    CHECK(c.midi_packets == 5 && c.midi_bytes == sizeof(want), "5 packets, 12 bytes for MIDI clients");
    CHECK(c.control_packets == 3, "the control cable's 3 packets are counted for control apps (%llu)",
          (unsigned long long)c.control_packets);
    CHECK(c.other_packets == 2, "2 packets of other cables go nowhere (%llu)",
          (unsigned long long)c.other_packets);
    CHECK(c.malformed == 1, "the damaged cable 0 packet goes nowhere (%llu)",
          (unsigned long long)c.malformed);

    /* Every cable there could be, with a note and with each kind of SysEx packet. */
    for (cable = 0; cable < 16; cable++) {
        static const uint8_t kinds[4][4] = {{0x09, 0x90, 0x3C, 0x40},
                                            {0x04, 0xF0, 0x41, 0x10},
                                            {0x07, 0x00, 0x69, 0xF7},
                                            {0x0F, 0xFE, 0x00, 0x00}};
        unsigned k, got = 0;

        for (k = 0; k < 4; k++) {
            uint8_t p[4];

            memcpy(p, kinds[k], 4);
            p[0] = (uint8_t)((cable << 4) | (p[0] & 0x0F));
            memset(&c, 0, sizeof(c));
            got += (unsigned)ua101_midisplit(p, 4, midi, sizeof(midi), &c);
        }
        CHECK((cable == 0) == (got != 0), "cable %u: %s for MIDI clients (%u bytes)", cable,
              cable == 0 ? "everything" : "nothing", got);
    }

    memset(&c, 0, sizeof(c));
    n = ua101_midisplit(read, sizeof(read), midi, 4, &c);
    CHECK(n <= 4 && c.no_room > 0, "too little room: nothing is written past it (%zu)", n);
    memset(&c, 0, sizeof(c));
    CHECK(ua101_midisplit(read, 3, midi, sizeof(midi), &c) == 0, "less than a packet: nothing");
}

/* --- To the unit: what passes. --- */

static void test_plain(void)
{
    static const uint8_t in[] = {0x90, 0x3C, 0x40, 0x3E, 0x41,       /* two notes, running status */
                                 0xB0, 0x07, 0x64, 0xC5, 0x03,       /* a control and a program */
                                 0xE1, 0x00, 0x40, 0xF8, 0xF2, 0x10, /* bend, clock, position */
                                 0x20, 0xF6, 0x80, 0x3C, 0x00};      /* tune request, note off */
    static const uint8_t want[] = {0x90, 0x3C, 0x40, 0x90, 0x3E, 0x41, 0xB0, 0x07, 0x64,
                                   0xC5, 0x03, 0xE1, 0x00, 0x40, 0xF8, 0xF2, 0x10, 0x20,
                                   0xF6, 0x80, 0x3C, 0x00};
    static const uint8_t junk[] = {0x40, 0x41, 0xF4, 0xF7, 0xFD, 0x90, 0x3C}; /* the last is cut off */
    ua101_midifeed f;
    uint64_t now = 1000;

    fresh(&f, NULL);
    ua101_midigate_feed(&G, &f, in, sizeof(in));
    drain(&G, &now);
    CHECK(out_is(want, sizeof(want)), "each kind of message passes, running status written out (%zu)",
          g_out_len);
    CHECK(g_out_other_cable == 0 && g_out_malformed == 0, "every packet is a sound one for cable 0");
    CHECK(G.counts.bytes_in == sizeof(in) && G.counts.bytes_sent == sizeof(want) &&
              G.counts.packets_sent == G.counts.packets_queued,
          "the counts agree");

    fresh(&f, NULL);
    ua101_midigate_feed(&G, &f, junk, sizeof(junk));
    drain(&G, &now);
    CHECK(g_out_len == 0 && G.counts.invalid_bytes == 5,
          "bytes MIDI 1.0 doesn't allow are dropped and counted (%llu)",
          (unsigned long long)G.counts.invalid_bytes);
}

static void test_sysex_passes(void)
{
    static const uint8_t other[] = {0xF0, 0x7D, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0xF7};
    static const uint8_t gs[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00,
                                 0x7F, 0x00, 0x41, 0xF7}; /* Roland, another model: GS reset */
    static const uint8_t near1[] = {0xF0, 0x41, 0x10, 0x00, 0x6A, 0x12, 0x00, 0xF7};
    static const uint8_t near2[] = {0xF0, 0x41, 0x10, 0x01, 0x69, 0x12, 0x00, 0xF7};
    static const uint8_t near3[] = {0xF0, 0x40, 0x10, 0x00, 0x69, 0x12, 0x00, 0xF7};
    static const uint8_t ident[] = {0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7};
    static const uint8_t tiny[4][5] = {{0xF0, 0xF7}, {0xF0, 0x41, 0xF7}, {0xF0, 0x41, 0x10, 0xF7},
                                       {0xF0, 0x41, 0x10, 0x00, 0xF7}};
    static const struct {
        const uint8_t *m;
        size_t len;
        const char *what;
    } pass[] = {{other, sizeof(other), "SysEx of another maker"},
                {gs, sizeof(gs), "Roland SysEx for another model (42)"},
                {near1, sizeof(near1), "Roland SysEx for model 00 6A"},
                {near2, sizeof(near2), "Roland SysEx for model 01 69"},
                {near3, sizeof(near3), "maker 40 with the UA-101's model bytes"},
                {ident, sizeof(ident), "the identity request"},
                {tiny[0], 2, "an empty SysEx"},
                {tiny[1], 3, "a SysEx that ends after the maker"},
                {tiny[2], 4, "a SysEx that ends after the device"},
                {tiny[3], 5, "a SysEx that ends inside the model"}};
    ua101_midifeed f;
    uint64_t now = 1000;
    size_t i, k;

    for (i = 0; i < sizeof(pass) / sizeof(pass[0]); i++) {
        fresh(&f, NULL);
        ua101_midigate_feed(&G, &f, pass[i].m, pass[i].len);
        drain(&G, &now);
        CHECK(out_is(pass[i].m, pass[i].len) && G.counts.own_sysex == 0 &&
                  G.counts.gate_own_sysex == 0,
              "%s passes whole", pass[i].what);
        /* The same a byte at a time, as an app may hand it over. */
        fresh(&f, NULL);
        for (k = 0; k < pass[i].len; k++)
            ua101_midigate_feed(&G, &f, pass[i].m + k, 1);
        drain(&G, &now);
        CHECK(out_is(pass[i].m, pass[i].len), "%s, a byte at a time, passes whole", pass[i].what);
    }

    /* A SysEx ended by another message instead of F7: nothing closes it on the way out. */
    {
        static const uint8_t in[] = {0xF0, 0x7D, 0x01, 0x02, 0x03, 0x90, 0x3C, 0x40};
        static const uint8_t want[] = {0xF0, 0x7D, 0x01, 0x90, 0x3C, 0x40};

        fresh(&f, NULL);
        ua101_midigate_feed(&G, &f, in, sizeof(in));
        drain(&G, &now);
        CHECK(out_is(want, sizeof(want)) && G.counts.sysex_cut == 1,
              "a cut SysEx leaves only whole packets and no F7, then the message that cut it");
    }
}

/* --- To the unit: SysEx for the UA-101 itself stays in. --- */

/* Messages a control app could send, as docs/control-protocol.md gives them. */
static const uint8_t k_pause_saving[] = {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00,
                                         0x00, 0x00, 0x00, 0x00, 0x00, 0xF7};
static const uint8_t k_reset_all[] = {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00,
                                      0x00, 0x00, 0x00, 0x02, 0x7E, 0xF7};
static const uint8_t k_patch[] = {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00,
                                  0x02, 0x01, 0x00, 0x03, 0x7A, 0xF7};
static const uint8_t k_volume[] = {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x04, 0x00,
                                   0x07, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x71, 0xF7};
static const uint8_t k_note[] = {0x90, 0x3C, 0x40};

static void test_own_sysex(void)
{
    uint8_t request[UA101_CTL_READ_REQUEST_BYTES];
    const struct {
        const uint8_t *m;
        size_t len;
        const char *what;
    } own[] = {{request, sizeof(request), "the request for the settings"},
               {k_pause_saving, sizeof(k_pause_saving), "\"pause saving\""},
               {k_reset_all, sizeof(k_reset_all), "\"reset all\""},
               {k_patch, sizeof(k_patch), "a patch bay change"},
               {k_volume, sizeof(k_volume), "a volume change"}};
    ua101_midifeed f;
    uint64_t now = 1000;
    unsigned dev, skip;
    size_t i, k;

    ua101_ctl_read_request(request);
    for (i = 0; i < sizeof(own) / sizeof(own[0]); i++) {
        fresh(&f, NULL);
        ua101_midigate_feed(&G, &f, own[i].m, own[i].len);
        ua101_midigate_feed(&G, &f, k_note, sizeof(k_note));
        drain(&G, &now);
        CHECK(out_is(k_note, sizeof(k_note)) && G.counts.own_sysex == 1,
              "%s from a MIDI client is dropped whole, and the note after it passes (%zu bytes "
              "left)",
              own[i].what, g_out_len);
        CHECK(G.counts.gate_own_sysex == 0, "the first check caught it: the last had nothing to do");

        /* A byte at a time, with a clock byte between every two. */
        fresh(&f, NULL);
        for (k = 0; k < own[i].len; k++) {
            static const uint8_t clock = 0xF8;

            ua101_midigate_feed(&G, &f, own[i].m + k, 1);
            ua101_midigate_feed(&G, &f, &clock, 1);
        }
        drain(&G, &now);
        CHECK(!out_reaches_own_address() && g_out_len == own[i].len && G.counts.own_sysex == 1,
              "%s, a byte at a time between clock bytes: only the %zu clock bytes pass (%zu)",
              own[i].what, own[i].len, g_out_len);
    }

    /* Whatever device number it names. */
    {
        unsigned kept = 0;

        for (dev = 0; dev < 0x80; dev++) {
            uint8_t m[sizeof(k_volume)];

            memcpy(m, k_volume, sizeof(m));
            m[2] = (uint8_t)dev;
            fresh(&f, NULL);
            ua101_midigate_feed(&G, &f, m, sizeof(m));
            drain(&G, &now);
            kept += g_out_len == 0 && G.counts.own_sysex == 1;
        }
        CHECK(kept == 0x80, "with each of the 128 device numbers it is dropped (%u)", kept);
    }

    /* One left unfinished, then cut by a note: nothing of it leaves either. */
    fresh(&f, NULL);
    ua101_midigate_feed(&G, &f, k_volume, 9);
    ua101_midigate_feed(&G, &f, k_note, sizeof(k_note));
    drain(&G, &now);
    CHECK(out_is(k_note, sizeof(k_note)), "an unfinished one cut by a note: only the note leaves");

#ifdef UA101_MIDIGATE_TESTING
    /* Each of the two checks keeps the rule alone; with neither the message leaves, which is
       how this test knows it would see one. 1: the first check off. 2: the last check off. */
    for (skip = 0; skip <= 3; skip++) {
        unsigned reached = 0, whole = 0;

        for (i = 0; i < sizeof(own) / sizeof(own[0]); i++) {
            fresh(&f, NULL);
            ua101_midigate_test_skip = skip;
            ua101_midigate_feed(&G, &f, own[i].m, own[i].len);
            ua101_midigate_feed(&G, &f, k_note, sizeof(k_note));
            drain(&G, &now);
            reached += (unsigned)out_reaches_own_address();
            whole += g_out_len == own[i].len + sizeof(k_note);
            if (skip == 1)
                CHECK(g_out_len == 3 + sizeof(k_note) && g_out[0] == 0xF0 && g_out[1] == 0x41 &&
                          memcmp(g_out + 3, k_note, sizeof(k_note)) == 0 &&
                          G.counts.gate_own_sysex == 1,
                      "the last check alone: %s is stopped at its address, so only \"F0 41 "
                      "<device>\" and the note leave (%zu bytes)",
                      own[i].what, g_out_len);
            if (skip == 2)
                CHECK(out_is(k_note, sizeof(k_note)) && G.counts.own_sysex == 1,
                      "the first check alone: nothing of %s leaves", own[i].what);
        }
        if (skip == 3)
            CHECK(reached == 5 && whole == 5,
                  "with both checks off all five leave whole: the test would see it (%u, %u)",
                  reached, whole);
        else
            CHECK(reached == 0, "checks left out: %u; none reached its address (%u did)", skip,
                  reached);
    }
    ua101_midigate_test_skip = 0;
#endif
}

/* --- To the unit: the last check of the cable, and of the packet. --- */

static void test_last_check(void)
{
#ifdef UA101_MIDIGATE_TESTING
    static const uint8_t control[4] = {0x14, 0xF0, 0x41, 0x10}; /* cable 1 */
    static const uint8_t cable2[4] = {0x29, 0x90, 0x3C, 0x40};
    static const uint8_t reserved[4] = {0x01, 0x90, 0x3C, 0x40};
    static const uint8_t data_alone[4] = {0x0F, 0x41, 0x00, 0x00};
    static const uint8_t good[4] = {0x09, 0x90, 0x3C, 0x40};
    ua101_midifeed f;
    uint64_t now = 1000;
    unsigned cable, stopped = 0;

    fresh(&f, NULL);
    CHECK(ua101_midigate_test_enqueue(&G, control) && ua101_midigate_test_enqueue(&G, good) &&
              ua101_midigate_test_enqueue(&G, cable2) && ua101_midigate_test_enqueue(&G, reserved) &&
              ua101_midigate_test_enqueue(&G, data_alone) && ua101_midigate_test_enqueue(&G, good),
          "six packets put straight into the queue, as no client can");
    drain(&G, &now);
    CHECK(g_out_packets == 2 && g_out_len == 6 && g_out_other_cable == 0,
          "only the two for cable 0 leave (%zu packets)", g_out_packets);
    CHECK(G.counts.gate_wrong_cable == 2 && G.counts.gate_malformed == 2,
          "the control cable's and cable 2's were stopped by the last check (%llu), the two "
          "unsound ones too (%llu)",
          (unsigned long long)G.counts.gate_wrong_cable,
          (unsigned long long)G.counts.gate_malformed);

    for (cable = 1; cable < 16; cable++) {
        uint8_t p[4] = {(uint8_t)((cable << 4) | 0x09), 0x90, 0x3C, 0x40};

        fresh(&f, NULL);
        ua101_midigate_test_enqueue(&G, p);
        drain(&G, &now);
        stopped += g_out_packets == 0 && G.counts.gate_wrong_cable == 1;
    }
    CHECK(stopped == 15, "a packet for each of the cables 1 to 15 is stopped (%u)", stopped);

    /* With the check left out it leaves: the test would see it. */
    fresh(&f, NULL);
    ua101_midigate_test_skip = 4;
    ua101_midigate_test_enqueue(&G, control);
    drain(&G, &now);
    CHECK(g_out_other_cable == 1, "with the last check of the cable off, the packet leaves");
    ua101_midigate_test_skip = 0;
#endif
}

/* --- Pacing. --- */

static void test_pacing(void)
{
    enum { LONG = 12000 };
    static uint8_t sysex[LONG];
    static uint16_t per_tick[4096];
    ua101_midifeed f;
    uint64_t now = 5000000, took;
    size_t i, ticks = 0, first, worst_second = 0, worst_tick = 0;
    uint8_t chord[30];

    sysex[0] = 0xF0;
    sysex[1] = 0x7D;
    for (i = 2; i < LONG - 1; i++)
        sysex[i] = (uint8_t)(i & 0x7F);
    sysex[LONG - 1] = 0xF7;

    fresh(&f, NULL);
    ua101_midigate_feed(&G, &f, sysex, LONG);
    CHECK(G.counts.packets_queued == LONG / 3, "a 12,000 byte SysEx is queued whole (%llu packets)",
          (unsigned long long)G.counts.packets_queued);
    first = pull(&G, now);
    per_tick[ticks++] = (uint16_t)first;
    CHECK(first <= UA101_MIDIGATE_BURST_BYTES && first >= UA101_MIDIGATE_BURST_BYTES - 2,
          "at once only the burst leaves: %zu bytes", first);
    while (ua101_midigate_pending(&G) && ticks < 4096) {
        now += UA101_MIDIGATE_TICK_US;
        per_tick[ticks++] = (uint16_t)pull(&G, now);
    }
    took = (uint64_t)(ticks - 1) * UA101_MIDIGATE_TICK_US;
    CHECK(out_is(sysex, LONG), "all of it arrives, in order");
    for (i = 0; i < ticks; i++) {
        size_t sum = 0, k;

        if (per_tick[i] > worst_tick)
            worst_tick = per_tick[i];
        for (k = i; k < ticks && k < i + 100; k++) /* 100 ticks: one second */
            sum += per_tick[k];
        if (sum > worst_second)
            worst_second = sum;
    }
    CHECK(worst_second <= 3125 + UA101_MIDIGATE_BURST_BYTES,
          "in no second more than 3,125 bytes and the burst left (%zu)", worst_second);
    CHECK(worst_tick <= UA101_MIDIGATE_BURST_BYTES, "no visit gave out more than the burst (%zu)",
          worst_tick);
    CHECK(took >= (uint64_t)(LONG - UA101_MIDIGATE_BURST_BYTES) * UA101_MIDIGATE_BYTE_US &&
              took <= (uint64_t)LONG * UA101_MIDIGATE_BYTE_US + 2 * UA101_MIDIGATE_TICK_US,
          "it took the wire's time: %.2f s for 12,000 bytes", (double)took / 1e6);

    /* After a pause a chord leaves at once. */
    now += 1000000;
    for (i = 0; i < sizeof(chord); i += 3) {
        chord[i] = 0x90;
        chord[i + 1] = (uint8_t)(0x30 + i);
        chord[i + 2] = 0x50;
    }
    out_reset();
    ua101_midigate_feed(&G, &f, chord, sizeof(chord));
    CHECK(pull(&G, now) == sizeof(chord) && !ua101_midigate_pending(&G),
          "after a pause ten notes at once leave in one go");

    /* A clock that stands still, or is asked twice, gives nothing extra. */
    fresh(&f, NULL);
    ua101_midigate_feed(&G, &f, sysex, LONG);
    first = pull(&G, now);
    CHECK(pull(&G, now) < 3 && pull(&G, now) == 0, "asking again at the same moment gives no more");
    CHECK(pull(&G, now - 1000) == 0, "nor does a clock that seems to go back");
}

/* --- The full queue. --- */

static void test_overflow(void)
{
    enum { BIG = 60000 };
    static uint8_t sysex[BIG];
    const uint32_t sysex_room = UA101_MIDIGATE_QUEUE_PACKETS - UA101_MIDIGATE_SHORT_RESERVE_PACKETS;
    static const uint8_t small[] = {0xF0, 0x7D, 0x55, 0xF7};
    static const uint8_t end_and_off[] = {0xF7, 0x80, 0x3C, 0x00};
    ua101_midifeed f;
    uint64_t now = 1000;
    size_t i, notes_at, f7 = 0;
    unsigned n;

    sysex[0] = 0xF0;
    sysex[1] = 0x7D;
    for (i = 2; i < BIG; i++)
        sysex[i] = 0x11;

    fresh(&f, NULL);
    ua101_midigate_feed(&G, &f, sysex, BIG); /* no F7 yet, and nobody takes anything */
    CHECK(G.count == sysex_room && G.counts.overflow_messages == 1,
          "a SysEx longer than the queue stops at the part kept for short messages (%u packets), "
          "counted once (%llu)",
          G.count, (unsigned long long)G.counts.overflow_messages);
    ua101_midigate_feed(&G, &f, sysex + 2, 5000); /* it goes on: all of that is dropped */
    CHECK(G.count == sysex_room && G.counts.overflow_messages == 1, "the rest of it is dropped");
    for (n = 0; n < UA101_MIDIGATE_SHORT_RESERVE_PACKETS; n++)
        ua101_midigate_feed(&G, &f, n == 0 ? end_and_off : k_note, n == 0 ? 4 : 3);
    CHECK(G.count == UA101_MIDIGATE_QUEUE_PACKETS && G.counts.overflow_messages == 1,
          "short messages still find room behind it: %u of them", n);
    ua101_midigate_feed(&G, &f, k_note, sizeof(k_note));
    CHECK(G.count == UA101_MIDIGATE_QUEUE_PACKETS && G.counts.overflow_messages == 2,
          "one more is dropped whole and counted");

    drain(&G, &now);
    notes_at = (size_t)sysex_room * 3;
    for (i = 0; i < g_out_len; i++)
        f7 += g_out[i] == 0xF7;
    CHECK(g_out_len == notes_at + 3 * UA101_MIDIGATE_SHORT_RESERVE_PACKETS && f7 == 0,
          "what leaves: the SysEx as far as it got, never closed with F7 (%zu), and the short "
          "messages (%zu bytes)",
          f7, g_out_len);
    CHECK(g_out[notes_at] == 0x80 && g_out[notes_at + 3] == 0x90 && g_out[g_out_len - 3] == 0x90,
          "each short message whole, right after the cut SysEx");
    CHECK(g_out_other_cable == 0 && g_out_malformed == 0, "and every packet sound");

    out_reset();
    ua101_midigate_feed(&G, &f, small, sizeof(small));
    drain(&G, &now);
    CHECK(out_is(small, sizeof(small)), "afterwards a new SysEx passes whole");
}

/* --- Two clients. --- */

static void test_two_clients(void)
{
    static const uint8_t a_start[] = {0xF0, 0x7D, 0x01, 0x02, 0x03, 0x04, 0x05};
    static const uint8_t a_end[] = {0x06, 0x07, 0xF7};
    static const uint8_t b_sysex[] = {0xF0, 0x7D, 0x41, 0x42, 0x43, 0x44, 0xF7};
    static const uint8_t clock = 0xF8;
    static const uint8_t want[] = {0xF0, 0x7D, 0x01, 0x02, 0x03, 0x04, 0xF8,
                                   0x05, 0x06, 0x07, 0xF7, 0x90, 0x3C, 0x40};
    ua101_midifeed a, b;
    uint64_t now = 1000;

    fresh(&a, &b);
    ua101_midigate_feed(&G, &a, a_start, sizeof(a_start));
    ua101_midigate_feed(&G, &b, k_note, sizeof(k_note));
    ua101_midigate_feed(&G, &b, b_sysex, sizeof(b_sysex));
    ua101_midigate_feed(&G, &b, &clock, 1);
    CHECK(G.counts.busy_messages == 2,
          "while one client's SysEx is on its way, the other's note and SysEx are dropped (%llu)",
          (unsigned long long)G.counts.busy_messages);
    ua101_midigate_feed(&G, &a, a_end, sizeof(a_end));
    ua101_midigate_feed(&G, &b, k_note, sizeof(k_note));
    drain(&G, &now);
    CHECK(out_is(want, sizeof(want)),
          "the first client's SysEx leaves unbroken, with the other's clock byte in it, then the "
          "other's next note (%zu)",
          g_out_len);

    /* The first client goes away in the middle. */
    fresh(&a, &b);
    ua101_midigate_feed(&G, &a, a_start, sizeof(a_start));
    ua101_midigate_release(&G, &a);
    ua101_midigate_feed(&G, &b, k_note, sizeof(k_note));
    drain(&G, &now);
    CHECK(G.counts.busy_messages == 0 && g_out_len == 6 + sizeof(k_note),
          "a client that left with a SysEx half sent no longer holds the other back");
}

/* --- The port closed, or didn't take a write. --- */

static void test_flush(void)
{
    static const uint8_t start[] = {0xF0, 0x7D, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    static const uint8_t rest[] = {0x07, 0x08, 0x09, 0x0A, 0x0B, 0xF7};
    ua101_midifeed f;
    uint64_t now = 1000;

    fresh(&f, NULL);
    ua101_midigate_feed(&G, &f, start, sizeof(start));
    CHECK(G.count == 2, "two packets of a SysEx wait");
    ua101_midigate_flush(&G);
    CHECK(G.count == 0 && G.counts.flushed_packets == 2 && !ua101_midigate_pending(&G),
          "flushed: nothing waits, and they are counted");
    ua101_midigate_feed(&G, &f, rest, sizeof(rest));
    drain(&G, &now);
    CHECK(g_out_len == 0,
          "the rest of that SysEx is dropped as it comes: no bytes without their start leave "
          "(%zu)",
          g_out_len);
    ua101_midigate_feed(&G, &f, k_note, sizeof(k_note));
    drain(&G, &now);
    CHECK(out_is(k_note, sizeof(k_note)), "the next message passes");
}

/* --- Anything at all. --- */

static uint32_t g_seed = 0x1234567u;

static uint32_t rnd(void)
{
    g_seed = g_seed * 1664525u + 1013904223u;
    return g_seed >> 8;
}

/* Random bytes from two clients, leaning towards the things that matter (SysEx starts, the
   UA-101's address, status bytes), with visits to the gate at random times. Whatever comes:
   every packet that leaves is a sound one for cable 0, no SysEx reaches the UA-101's address,
   and the queue never holds more than its size. */
static void fuzz(unsigned skip, int rounds)
{
    static const uint8_t own[] = {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12};
    ua101_midifeed f[2];
    uint64_t now = 1000;
    int round, reached = 0, over = 0;

    for (round = 0; round < rounds; round++) {
        int step;

        fresh(&f[0], &f[1]);
#ifdef UA101_MIDIGATE_TESTING
        ua101_midigate_test_skip = skip;
#else
        (void)skip;
#endif
        for (step = 0; step < 400; step++) {
            uint8_t bytes[24];
            size_t n = 1 + rnd() % sizeof(bytes), i;
            ua101_midifeed *who = &f[rnd() % 2];

            for (i = 0; i < n; i++) {
                uint32_t r = rnd() % 100;

                if (r < 8 && i + sizeof(own) <= n) {
                    memcpy(bytes + i, own, sizeof(own));
                    if (rnd() % 4 == 0)
                        bytes[i + 2] = (uint8_t)(rnd() & 0x7F);
                    i += sizeof(own) - 1;
                } else if (r < 14) {
                    bytes[i] = 0xF0;
                } else if (r < 20) {
                    bytes[i] = 0xF7;
                } else if (r < 24) {
                    bytes[i] = 0xF8;
                } else if (r < 34) {
                    bytes[i] = (uint8_t)(0x80 | (rnd() & 0x7F));
                } else if (r < 44) {
                    static const uint8_t pick[] = {0x41, 0x00, 0x69, 0x10};

                    bytes[i] = pick[rnd() % sizeof(pick)];
                } else {
                    bytes[i] = (uint8_t)(rnd() & 0x7F);
                }
            }
            ua101_midigate_feed(&G, who, bytes, n);
            over += G.count > UA101_MIDIGATE_QUEUE_PACKETS;
            if (rnd() % 3 == 0) {
                now += rnd() % 30000;
                pull(&G, now);
            }
            if (rnd() % 97 == 0)
                ua101_midigate_flush(&G);
            if (rnd() % 89 == 0)
                ua101_midigate_release(&G, who);
        }
        drain(&G, &now);
        reached += out_reaches_own_address();
        if (g_out_other_cable != 0 || g_out_malformed != 0)
            break;
    }
    CHECK(round == rounds && g_out_other_cable == 0 && g_out_malformed == 0,
          "%d rounds of random bytes from two clients%s: every packet that left was a sound one "
          "for cable 0",
          rounds, skip == 1 ? ", first check off" : skip == 2 ? ", last check off" : "");
    CHECK(reached == 0, "and no SysEx reached the UA-101's address (%d did)", reached);
    CHECK(over == 0, "and the queue never held more than its size");
#ifdef UA101_MIDIGATE_TESTING
    ua101_midigate_test_skip = 0;
#endif
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    test_split();
    test_plain();
    test_sysex_passes();
    test_own_sysex();
    test_last_check();
    test_pacing();
    test_overflow();
    test_two_clients();
    test_flush();
    fuzz(0, 300);
#ifdef UA101_MIDIGATE_TESTING
    fuzz(1, 300);
    fuzz(2, 300);
#endif
    printf("test_midigate: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
