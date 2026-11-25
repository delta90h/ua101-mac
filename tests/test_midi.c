/*
 * Tests for MIDI framing: MIDI bytes to and from USB-MIDI packets, and to and from UMP words.
 * Runs on any computer, with no hardware needed. The expected packets follow the USB-MIDI 1.0
 * and UMP 1.1 specifications. They aren't recordings from a real unit.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../src/ua101_control.h"
#include "../src/ua101_midi.h"

static int g_failures;
static int g_tests;

#define EXPECT(cond)                                                     \
    do {                                                                 \
        g_tests++;                                                       \
        if (!(cond)) {                                                   \
            g_failures++;                                                \
            printf("FAILED line %d: %s\n", __LINE__, #cond);             \
        }                                                                \
    } while (0)

#define MAX_BYTES 65536

/* App to unit: MIDI bytes through the parser and the USB-MIDI packer. */
typedef struct {
    ua101_midi_parser parser;
    ua101_usbmidi_packer packer;
    int overflow;
} tx_state;

static void tx_init(tx_state *t)
{
    ua101_midi_parser_init(&t->parser);
    ua101_usbmidi_packer_init(&t->packer);
    t->overflow = 0;
}

static size_t tx_run(tx_state *t, const uint8_t *in, size_t n, uint8_t *out, size_t cap)
{
    static const ua101_midi_event cut_event = {UA101_MIDI_SYSEX_CUT, {0}, 0};
    size_t i, used = 0;

    for (i = 0; i < n; i++) {
        ua101_midi_event ev;
        uint8_t packet[UA101_MIDI_PACKET_BYTES];
        int cut;

        ua101_midi_parse(&t->parser, in[i], &ev, &cut);
        if (cut)
            ua101_usbmidi_pack(&t->packer, &cut_event, packet);
        if (ua101_usbmidi_pack(&t->packer, &ev, packet)) {
            if (used + UA101_MIDI_PACKET_BYTES > cap) {
                t->overflow = 1;
                continue;
            }
            memcpy(out + used, packet, sizeof(packet));
            used += sizeof(packet);
        }
    }
    return used;
}

static size_t tx(const uint8_t *in, size_t n, uint8_t *out, size_t cap)
{
    tx_state t;

    tx_init(&t);
    return tx_run(&t, in, n, out, cap);
}

/* Unit to app: USB-MIDI packets, cable 0 only, through the parser and the UMP packer. */
typedef struct {
    ua101_midi_parser parser;
    ua101_ump_packer packer;
    uint64_t malformed;
    uint64_t control;
    uint64_t dropped;
    int overflow;
} rx_state;

static void rx_init(rx_state *r)
{
    memset(r, 0, sizeof(*r));
    ua101_midi_parser_init(&r->parser);
    ua101_ump_packer_init(&r->packer);
}

static size_t rx_run(rx_state *r, const uint8_t *packets, size_t n, uint32_t *words, size_t cap)
{
    static const ua101_midi_event cut_event = {UA101_MIDI_SYSEX_CUT, {0}, 0};
    size_t i, used = 0;

    for (i = 0; i + UA101_MIDI_PACKET_BYTES <= n; i += UA101_MIDI_PACKET_BYTES) {
        uint8_t cable, bytes[3];
        int malformed;
        size_t len = ua101_usbmidi_unpack(packets + i, &cable, bytes, &malformed), j;

        r->malformed += (uint64_t)malformed;
        if (len == 0)
            continue;
        switch (ua101_midi_route_for(cable)) {
        case UA101_ROUTE_CONTROL:
            r->control++;
            continue;
        case UA101_ROUTE_DROP:
            r->dropped++;
            continue;
        case UA101_ROUTE_APPS:
            break;
        }
        for (j = 0; j < len; j++) {
            ua101_midi_event ev;
            uint32_t w[2];
            size_t k, got;
            int cut;

            ua101_midi_parse(&r->parser, bytes[j], &ev, &cut);
            for (k = 0; k < 2; k++) {
                if (k == 0 && !cut)
                    continue;
                got = ua101_ump_pack(&r->packer, k == 0 ? &cut_event : &ev, w);
                if (used + got > cap) {
                    r->overflow = 1;
                    continue;
                }
                memcpy(words + used, w, got * sizeof(uint32_t));
                used += got;
            }
        }
    }
    return used;
}

static size_t rx(const uint8_t *packets, size_t n, uint32_t *words, size_t cap)
{
    rx_state r;

    rx_init(&r);
    return rx_run(&r, packets, n, words, cap);
}

/* UMP words from an app back to MIDI bytes. Counts refused packets in *refused. */
static size_t ump_bytes(const uint32_t *words, size_t n, uint8_t *out, size_t cap,
                        unsigned *refused)
{
    size_t i = 0, used = 0;

    *refused = 0;
    while (i < n) {
        size_t count = ua101_ump_words(words[i]);
        uint8_t bytes[8];
        size_t len;
        int bad;

        if (i + count > n)
            break;
        len = ua101_ump_to_bytes(words + i, count, bytes, &bad);
        *refused += (unsigned)bad;
        if (used + len <= cap) {
            memcpy(out + used, bytes, len);
            used += len;
        }
        i += count;
    }
    return used;
}

static int same(const uint8_t *a, size_t alen, const uint8_t *b, size_t blen)
{
    return alen == blen && memcmp(a, b, alen) == 0;
}

static int same_words(const uint32_t *a, size_t alen, const uint32_t *b, size_t blen)
{
    return alen == blen && memcmp(a, b, alen * sizeof(uint32_t)) == 0;
}

#define TX_EXPECT(in, ...)                                                          \
    do {                                                                            \
        const uint8_t in_[] = in;                                                   \
        const uint8_t want_[] = __VA_ARGS__;                                        \
        uint8_t out_[64];                                                           \
        size_t got_ = tx(in_, sizeof(in_), out_, sizeof(out_));                     \
        EXPECT(same(out_, got_, want_, sizeof(want_)));                             \
    } while (0)

#define B(...) {__VA_ARGS__}

static void test_data_bytes(void)
{
    EXPECT(ua101_midi_data_bytes(0x90) == 2);
    EXPECT(ua101_midi_data_bytes(0x8F) == 2);
    EXPECT(ua101_midi_data_bytes(0xA0) == 2);
    EXPECT(ua101_midi_data_bytes(0xB3) == 2);
    EXPECT(ua101_midi_data_bytes(0xC0) == 1);
    EXPECT(ua101_midi_data_bytes(0xDF) == 1);
    EXPECT(ua101_midi_data_bytes(0xE0) == 2);
    EXPECT(ua101_midi_data_bytes(0xF1) == 1);
    EXPECT(ua101_midi_data_bytes(0xF2) == 2);
    EXPECT(ua101_midi_data_bytes(0xF3) == 1);
    EXPECT(ua101_midi_data_bytes(0xF6) == 0);
    EXPECT(ua101_midi_data_bytes(0xF8) == 0);
    EXPECT(ua101_midi_data_bytes(0xFF) == 0);
    EXPECT(ua101_midi_data_bytes(0x7F) == -1);
    EXPECT(ua101_midi_data_bytes(0xF0) == -1);
    EXPECT(ua101_midi_data_bytes(0xF7) == -1);
    EXPECT(ua101_midi_data_bytes(0xF4) == -1);
    EXPECT(ua101_midi_data_bytes(0xF5) == -1);
    EXPECT(ua101_midi_data_bytes(0xF9) == -1);
    EXPECT(ua101_midi_data_bytes(0xFD) == -1);
}

static void test_tx_short(void)
{
    /* Channel messages: the code repeats the status byte's top half. */
    TX_EXPECT(B(0x90, 0x3C, 0x64), B(0x09, 0x90, 0x3C, 0x64));
    TX_EXPECT(B(0x85, 0x3C, 0x00), B(0x08, 0x85, 0x3C, 0x00));
    TX_EXPECT(B(0xA1, 0x40, 0x20), B(0x0A, 0xA1, 0x40, 0x20));
    TX_EXPECT(B(0xB0, 0x07, 0x7F), B(0x0B, 0xB0, 0x07, 0x7F));
    TX_EXPECT(B(0xC9, 0x05), B(0x0C, 0xC9, 0x05, 0x00));
    TX_EXPECT(B(0xD2, 0x33), B(0x0D, 0xD2, 0x33, 0x00));
    TX_EXPECT(B(0xEF, 0x00, 0x40), B(0x0E, 0xEF, 0x00, 0x40));
    /* Running status is expanded into whole messages. */
    TX_EXPECT(B(0x90, 0x3C, 0x64, 0x3E, 0x64, 0x40, 0x00),
              B(0x09, 0x90, 0x3C, 0x64, 0x09, 0x90, 0x3E, 0x64, 0x09, 0x90, 0x40, 0x00));
    TX_EXPECT(B(0xC0, 0x01, 0x02), B(0x0C, 0xC0, 0x01, 0x00, 0x0C, 0xC0, 0x02, 0x00));
    /* Real time goes out at once and leaves the message around it whole. */
    TX_EXPECT(B(0x90, 0xF8, 0x3C, 0xFE, 0x64),
              B(0x0F, 0xF8, 0x00, 0x00, 0x0F, 0xFE, 0x00, 0x00, 0x09, 0x90, 0x3C, 0x64));
    TX_EXPECT(B(0xFA, 0xFB, 0xFC, 0xFF),
              B(0x0F, 0xFA, 0, 0, 0x0F, 0xFB, 0, 0, 0x0F, 0xFC, 0, 0, 0x0F, 0xFF, 0, 0));
    /* System common. */
    TX_EXPECT(B(0xF1, 0x23), B(0x02, 0xF1, 0x23, 0x00));
    TX_EXPECT(B(0xF2, 0x10, 0x20), B(0x03, 0xF2, 0x10, 0x20));
    TX_EXPECT(B(0xF3, 0x05), B(0x02, 0xF3, 0x05, 0x00));
    TX_EXPECT(B(0xF6), B(0x05, 0xF6, 0x00, 0x00));
    /* System common ends running status, so the data after it is dropped. */
    {
        const uint8_t in[] = {0x90, 0x3C, 0x64, 0xF1, 0x01, 0x3C, 0x64};
        const uint8_t want[] = {0x09, 0x90, 0x3C, 0x64, 0x02, 0xF1, 0x01, 0x00};
        uint8_t out[64];
        tx_state t;
        size_t got;

        tx_init(&t);
        got = tx_run(&t, in, sizeof(in), out, sizeof(out));
        EXPECT(same(out, got, want, sizeof(want)));
        EXPECT(t.parser.dropped == 2);
    }
    /* Real time doesn't end running status. */
    TX_EXPECT(B(0x90, 0x3C, 0x64, 0xF8, 0x3E, 0x64),
              B(0x09, 0x90, 0x3C, 0x64, 0x0F, 0xF8, 0x00, 0x00, 0x09, 0x90, 0x3E, 0x64));
}

static void test_tx_dropped(void)
{
    /* Data with no status, undefined status bytes, a lone F7, and a message cut short. */
    const uint8_t in[] = {0x40, 0x41, 0xF4, 0xF5, 0xF9, 0xFD, 0xF7, 0x90, 0x3C, 0xB0, 0x07, 0x50};
    const uint8_t want[] = {0x0B, 0xB0, 0x07, 0x50};
    uint8_t out[64];
    tx_state t;
    size_t got;

    tx_init(&t);
    got = tx_run(&t, in, sizeof(in), out, sizeof(out));
    EXPECT(same(out, got, want, sizeof(want)));
    /* 2 stray data bytes, 4 undefined bytes, the lone F7, and 90 3C. */
    EXPECT(t.parser.dropped == 9);
    EXPECT(t.parser.sysex_cut == 0);

    /* An undefined status byte also ends running status. */
    TX_EXPECT(B(0x90, 0x3C, 0x64, 0xF4, 0x3E, 0x64), B(0x09, 0x90, 0x3C, 0x64));
}

static void test_tx_sysex(void)
{
    /* The end code says how many bytes the last packet holds. */
    TX_EXPECT(B(0xF0, 0xF7), B(0x06, 0xF0, 0xF7, 0x00));
    TX_EXPECT(B(0xF0, 0x01, 0xF7), B(0x07, 0xF0, 0x01, 0xF7));
    TX_EXPECT(B(0xF0, 0x01, 0x02, 0xF7), B(0x04, 0xF0, 0x01, 0x02, 0x05, 0xF7, 0x00, 0x00));
    TX_EXPECT(B(0xF0, 0x01, 0x02, 0x03, 0xF7), B(0x04, 0xF0, 0x01, 0x02, 0x06, 0x03, 0xF7, 0x00));
    TX_EXPECT(B(0xF0, 0x01, 0x02, 0x03, 0x04, 0xF7),
              B(0x04, 0xF0, 0x01, 0x02, 0x07, 0x03, 0x04, 0xF7));
    /* Real time inside SysEx goes out at once, and the SysEx carries on. */
    TX_EXPECT(B(0xF0, 0x01, 0xF8, 0x02, 0x03, 0xF7),
              B(0x0F, 0xF8, 0x00, 0x00, 0x04, 0xF0, 0x01, 0x02, 0x06, 0x03, 0xF7, 0x00));

    /* A status byte cuts SysEx short. The bytes still held are dropped, as Linux does. */
    {
        const uint8_t in[] = {0xF0, 0x01, 0x02, 0x03, 0x04, 0x90, 0x3C, 0x64};
        const uint8_t want[] = {0x04, 0xF0, 0x01, 0x02, 0x09, 0x90, 0x3C, 0x64};
        uint8_t out[64];
        tx_state t;
        size_t got;

        tx_init(&t);
        got = tx_run(&t, in, sizeof(in), out, sizeof(out));
        EXPECT(same(out, got, want, sizeof(want)));
        EXPECT(t.parser.sysex_cut == 1);
        EXPECT(t.packer.dropped == 2);
    }
    /* The status byte that cuts it can be a whole message, or a new SysEx. */
    TX_EXPECT(B(0xF0, 0x01, 0xF6), B(0x05, 0xF6, 0x00, 0x00));
    TX_EXPECT(B(0xF0, 0x01, 0xF0, 0x02, 0xF7), B(0x07, 0xF0, 0x02, 0xF7));
}

/* An app's SysEx shaped like Roland's control messages must still go out on cable 0, the
   MIDI OUT port, never on cable 1, where it would reach the unit's own settings. */
static void test_tx_cable_guard(void)
{
    uint8_t request[UA101_CTL_READ_REQUEST_BYTES];
    uint8_t out[64];
    size_t got, i;
    int all_cable0 = 1;

    EXPECT(UA101_MIDI_CABLE != UA101_CTL_CABLE);
    EXPECT(ua101_midi_route_for(UA101_CTL_CABLE) == UA101_ROUTE_CONTROL);
    EXPECT(ua101_midi_route_for(UA101_MIDI_CABLE) == UA101_ROUTE_APPS);
    for (i = 2; i < 16; i++)
        EXPECT(ua101_midi_route_for((uint8_t)i) == UA101_ROUTE_DROP);

    ua101_ctl_read_request(request);
    got = tx(request, sizeof(request), out, sizeof(out));
    EXPECT(got == 24);
    for (i = 0; i < got; i += 4)
        all_cable0 &= (out[i] >> 4) == 0;
    EXPECT(all_cable0);
}

static uint32_t g_seed = 20261108u;

static uint32_t rnd(void)
{
    g_seed = g_seed * 1664525u + 1013904223u;
    return g_seed >> 8;
}

/* Any bytes at all, valid or not, give only cable 0 packets that read back as valid. */
static void test_tx_fuzz(void)
{
    static uint8_t in[MAX_BYTES], out[MAX_BYTES * 8];
    size_t i, got;
    int all_cable0 = 1, all_valid = 1;
    tx_state t;

    for (i = 0; i < sizeof(in); i++)
        in[i] = (uint8_t)rnd();
    tx_init(&t);
    got = tx_run(&t, in, sizeof(in), out, sizeof(out));
    EXPECT(!t.overflow);
    EXPECT(got > 0);
    for (i = 0; i < got; i += 4) {
        uint8_t cable, bytes[3];
        int malformed;

        all_cable0 &= (out[i] >> 4) == 0;
        all_valid &= ua101_usbmidi_unpack(out + i, &cable, bytes, &malformed) > 0 && !malformed;
    }
    EXPECT(all_cable0);
    EXPECT(all_valid);
}

static void test_unpack(void)
{
    uint8_t cable, bytes[3];
    int malformed;

    {
        const uint8_t p[] = {0x09, 0x90, 0x3C, 0x64};

        EXPECT(ua101_usbmidi_unpack(p, &cable, bytes, &malformed) == 3);
        EXPECT(cable == 0 && !malformed && bytes[0] == 0x90 && bytes[2] == 0x64);
    }
    {
        const uint8_t p[] = {0x1C, 0xC0, 0x05, 0x00};

        EXPECT(ua101_usbmidi_unpack(p, &cable, bytes, &malformed) == 2);
        EXPECT(cable == 1 && !malformed);
    }
    {
        /* The zero padding after the last packet of a transfer. */
        const uint8_t p[] = {0x00, 0x00, 0x00, 0x00};

        EXPECT(ua101_usbmidi_unpack(p, &cable, bytes, &malformed) == 0);
        EXPECT(!malformed);
    }
    {
        /* Reserved codes, and codes that don't match their bytes. */
        const uint8_t bad[][4] = {
            {0x01, 0x90, 0x3C, 0x64}, /* reserved code 1 */
            {0x10, 0x01, 0x00, 0x00}, /* reserved code 0, not padding */
            {0x09, 0x80, 0x3C, 0x64}, /* note off under the note on code */
            {0x09, 0x90, 0x9C, 0x64}, /* status byte where data should be */
            {0x0C, 0xC0, 0x85, 0x00}, /* program change with a status byte for data */
            {0x02, 0xF2, 0x01, 0x00}, /* song position under the 2-byte code */
            {0x03, 0xF1, 0x01, 0x02}, /* time code under the 3-byte code */
            {0x05, 0x40, 0x00, 0x00}, /* single byte that is neither F6 nor F7 */
            {0x06, 0x01, 0x02, 0x00}, /* SysEx end with no F7 */
            {0x07, 0x01, 0xF7, 0x02}, /* F7 in the wrong place */
            {0x04, 0x01, 0xF7, 0x02}, /* F7 inside a continuing packet */
        };
        size_t i;
        int all = 1;

        for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
            all &= ua101_usbmidi_unpack(bad[i], &cable, bytes, &malformed) == 0 && malformed;
        EXPECT(all);
    }
}

static void test_rx_ump(void)
{
    uint32_t w[64];
    size_t n;

    {
        const uint8_t p[] = {0x09, 0x90, 0x3C, 0x64, 0x0C, 0xC3, 0x05, 0x00, 0x0F, 0xF8, 0, 0,
                             0x03, 0xF2, 0x10, 0x20, 0x05, 0xF6, 0, 0};
        const uint32_t want[] = {0x20903C64, 0x20C30500, 0x10F80000, 0x10F21020, 0x10F60000};

        n = rx(p, sizeof(p), w, 64);
        EXPECT(same_words(w, n, want, 5));
    }
    {
        /* F0 41 10 F7: 2 data bytes, in one complete packet. */
        const uint8_t p[] = {0x04, 0xF0, 0x41, 0x10, 0x05, 0xF7, 0x00, 0x00};
        const uint32_t want[] = {0x30024110, 0x00000000};

        n = rx(p, sizeof(p), w, 64);
        EXPECT(same_words(w, n, want, 2));
    }
    {
        /* 6 data bytes still fit one complete packet. */
        const uint8_t p[] = {0x04, 0xF0, 1, 2, 0x04, 3, 4, 5, 0x06, 6, 0xF7, 0};
        const uint32_t want[] = {0x30060102, 0x03040506};

        n = rx(p, sizeof(p), w, 64);
        EXPECT(same_words(w, n, want, 2));
    }
    {
        /* 7 data bytes: a start packet of 6, then an end packet of 1. */
        const uint8_t p[] = {0x04, 0xF0, 1, 2, 0x04, 3, 4, 5, 0x07, 6, 7, 0xF7};
        const uint32_t want[] = {0x30160102, 0x03040506, 0x30310700, 0x00000000};

        n = rx(p, sizeof(p), w, 64);
        EXPECT(same_words(w, n, want, 4));
    }
    {
        /* 13 data bytes: start, continue, end. */
        const uint8_t p[] = {0x04, 0xF0, 1, 2, 0x04, 3, 4, 5, 0x04, 6, 7, 8, 0x04, 9, 10, 11,
                             0x07, 12, 13, 0xF7};
        const uint32_t want[] = {0x30160102, 0x03040506, 0x30260708, 0x090A0B0C,
                                 0x30310D00, 0x00000000};

        n = rx(p, sizeof(p), w, 64);
        EXPECT(same_words(w, n, want, 6));
    }
    {
        /* Cable 1 is the unit's control channel, and other cables don't exist: neither reaches
           apps. */
        const uint8_t p[] = {0x14, 0xF0, 0x41, 0x10, 0x15, 0xF7, 0, 0, 0x29, 0x90, 0x3C, 0x64,
                             0x09, 0x90, 0x3C, 0x64};
        const uint32_t want[] = {0x20903C64};
        rx_state r;

        rx_init(&r);
        n = rx_run(&r, p, sizeof(p), w, 64);
        EXPECT(same_words(w, n, want, 1));
        EXPECT(r.control == 2);
        EXPECT(r.dropped == 1);
    }
    {
        /* Bytes sent one at a time, with running status, as a raw MIDI stream can be. */
        const uint8_t p[] = {0x0F, 0x90, 0, 0, 0x0F, 0x3C, 0, 0, 0x0F, 0x64, 0, 0,
                             0x0F, 0x3E, 0, 0, 0x0F, 0x64, 0, 0};
        const uint32_t want[] = {0x20903C64, 0x20903E64};

        n = rx(p, sizeof(p), w, 64);
        EXPECT(same_words(w, n, want, 2));
    }
    {
        /* A cut SysEx that has sent its start is closed with an end packet. */
        const uint8_t p[] = {0x04, 0xF0, 1, 2, 0x04, 3, 4, 5, 0x04, 6, 7, 8, 0x09, 0x90, 0x3C,
                             0x64};
        const uint32_t want[] = {0x30160102, 0x03040506, 0x30320708, 0x00000000, 0x20903C64};
        rx_state r;

        rx_init(&r);
        n = rx_run(&r, p, sizeof(p), w, 64);
        EXPECT(same_words(w, n, want, 5));
        EXPECT(r.packer.cut == 1);
    }
    {
        /* One that hasn't is dropped, so it isn't passed on as a complete message. */
        const uint8_t p[] = {0x04, 0xF0, 1, 2, 0x09, 0x90, 0x3C, 0x64};
        const uint32_t want[] = {0x20903C64};
        rx_state r;

        rx_init(&r);
        n = rx_run(&r, p, sizeof(p), w, 64);
        EXPECT(same_words(w, n, want, 1));
        EXPECT(r.packer.dropped == 2);
    }
}

static void test_ump_to_bytes(void)
{
    uint8_t b[8];
    int refused;

    {
        const uint32_t w[] = {0x20903C64};

        EXPECT(ua101_ump_to_bytes(w, 1, b, &refused) == 3 && !refused);
        EXPECT(b[0] == 0x90 && b[1] == 0x3C && b[2] == 0x64);
    }
    {
        /* Any group: the unit has one MIDI port. */
        const uint32_t w[] = {0x2BC30500};

        EXPECT(ua101_ump_to_bytes(w, 1, b, &refused) == 2 && !refused);
        EXPECT(b[0] == 0xC3 && b[1] == 0x05);
    }
    {
        const uint32_t w[] = {0x10F80000};

        EXPECT(ua101_ump_to_bytes(w, 1, b, &refused) == 1 && b[0] == 0xF8 && !refused);
    }
    {
        const uint32_t w[] = {0x10F21020};

        EXPECT(ua101_ump_to_bytes(w, 1, b, &refused) == 3 && b[2] == 0x20 && !refused);
    }
    {
        const uint32_t complete[] = {0x30024110, 0};
        const uint32_t start[] = {0x30160102, 0x03040506};
        const uint32_t cont[] = {0x30260708, 0x090A0B0C};
        const uint32_t end[] = {0x30310D00, 0};
        const uint8_t want_complete[] = {0xF0, 0x41, 0x10, 0xF7};
        const uint8_t want_start[] = {0xF0, 1, 2, 3, 4, 5, 6};
        const uint8_t want_cont[] = {7, 8, 9, 10, 11, 12};
        const uint8_t want_end[] = {13, 0xF7};
        size_t n;

        n = ua101_ump_to_bytes(complete, 2, b, &refused);
        EXPECT(same(b, n, want_complete, sizeof(want_complete)) && !refused);
        n = ua101_ump_to_bytes(start, 2, b, &refused);
        EXPECT(same(b, n, want_start, sizeof(want_start)) && !refused);
        n = ua101_ump_to_bytes(cont, 2, b, &refused);
        EXPECT(same(b, n, want_cont, sizeof(want_cont)) && !refused);
        n = ua101_ump_to_bytes(end, 2, b, &refused);
        EXPECT(same(b, n, want_end, sizeof(want_end)) && !refused);
    }
    {
        /* A 6-byte complete SysEx packet is the longest output: 8 bytes. */
        const uint32_t w[] = {0x30060102, 0x03040506};

        EXPECT(ua101_ump_to_bytes(w, 2, b, &refused) == 8 && b[7] == 0xF7);
    }
    {
        /* Utility messages carry no MIDI and aren't refused. */
        const uint32_t w[] = {0x00200123};

        EXPECT(ua101_ump_to_bytes(w, 1, b, &refused) == 0 && !refused);
    }
    {
        const uint32_t bad[][2] = {
            {0x40903C00, 0x80000000}, /* MIDI 2.0 note on */
            {0x20F80000, 0},          /* real time under the channel voice type */
            {0x10903C64, 0},          /* note on under the system type */
            {0x20F00000, 0},          /* SysEx start as a channel message */
            {0x10F40000, 0},          /* undefined status */
            {0x20908064, 0},          /* data byte with its top bit set */
            {0x30070102, 0x03040506}, /* 7 bytes in a SysEx packet */
            {0x30420102, 0},          /* SysEx form 4 */
            {0x30028110, 0},          /* SysEx byte with its top bit set */
        };
        const size_t counts[] = {2, 1, 1, 1, 1, 1, 2, 2, 2};
        size_t i;
        int all = 1;

        for (i = 0; i < sizeof(counts) / sizeof(counts[0]); i++)
            all &= ua101_ump_to_bytes(bad[i], counts[i], b, &refused) == 0 && refused;
        EXPECT(all);
    }
    {
        /* A word count that doesn't match the type is refused. */
        const uint32_t w[] = {0x30024110, 0};

        EXPECT(ua101_ump_to_bytes(w, 1, b, &refused) == 0 && refused);
    }
    EXPECT(ua101_ump_words(0x00000000) == 1);
    EXPECT(ua101_ump_words(0x20000000) == 1);
    EXPECT(ua101_ump_words(0x30000000) == 2);
    EXPECT(ua101_ump_words(0x40000000) == 2);
    EXPECT(ua101_ump_words(0x50000000) == 4);
    EXPECT(ua101_ump_words(0xF0000000) == 4);
}

/* Random well-formed MIDI, with real time scattered anywhere, including inside SysEx. */
static size_t make_stream(uint8_t *out, size_t cap)
{
    static const uint8_t realtime[] = {0xF8, 0xFA, 0xFB, 0xFC, 0xFE, 0xFF};
    uint8_t msg[64];
    size_t used = 0;

    while (used + 2 * sizeof(msg) < cap) {
        size_t len = 0, i;
        uint32_t pick = rnd() % 10;

        if (pick < 6) {
            uint8_t status = (uint8_t)(0x80 | ((rnd() % 7) << 4) | (rnd() % 16));
            int need = ua101_midi_data_bytes(status);

            msg[len++] = status;
            while (need-- > 0)
                msg[len++] = (uint8_t)(rnd() & 0x7F);
        } else if (pick < 8) {
            static const uint8_t common[] = {0xF1, 0xF2, 0xF3, 0xF6};
            uint8_t status = common[rnd() % 4];
            int need = ua101_midi_data_bytes(status);

            msg[len++] = status;
            while (need-- > 0)
                msg[len++] = (uint8_t)(rnd() & 0x7F);
        } else {
            size_t n = rnd() % 41;

            msg[len++] = 0xF0;
            while (n-- > 0)
                msg[len++] = (uint8_t)(rnd() & 0x7F);
            msg[len++] = 0xF7;
        }
        for (i = 0; i < len; i++) {
            if (rnd() % 10 == 0)
                out[used++] = realtime[rnd() % sizeof(realtime)];
            out[used++] = msg[i];
        }
    }
    return used;
}

static size_t keep(const uint8_t *in, size_t n, int realtime, uint8_t *out)
{
    size_t i, used = 0;

    for (i = 0; i < n; i++)
        if ((in[i] >= 0xF8) == realtime)
            out[used++] = in[i];
    return used;
}

/* Bytes -> USB-MIDI -> UMP -> bytes keeps every message, and every real-time byte in order.
   Real time inside SysEx may move a few bytes, which MIDI allows. */
static void test_round_trip(void)
{
    static uint8_t stream[MAX_BYTES], packets[MAX_BYTES * 4], back[MAX_BYTES * 2];
    static uint8_t a[MAX_BYTES * 2], b[MAX_BYTES * 2];
    static uint32_t words[MAX_BYTES * 2];
    size_t n, np, nw, nb, na, nbb;
    unsigned refused;
    tx_state t;
    rx_state r;

    n = make_stream(stream, sizeof(stream));
    tx_init(&t);
    np = tx_run(&t, stream, n, packets, sizeof(packets));
    EXPECT(!t.overflow);
    EXPECT(t.parser.dropped == 0 && t.parser.sysex_cut == 0 && t.packer.dropped == 0);

    rx_init(&r);
    nw = rx_run(&r, packets, np, words, sizeof(words) / sizeof(words[0]));
    EXPECT(!r.overflow);
    EXPECT(r.malformed == 0 && r.control == 0 && r.dropped == 0);
    EXPECT(r.parser.dropped == 0 && r.parser.sysex_cut == 0);
    EXPECT(r.packer.dropped == 0 && r.packer.cut == 0);

    nb = ump_bytes(words, nw, back, sizeof(back), &refused);
    EXPECT(refused == 0);

    na = keep(stream, n, 0, a);
    nbb = keep(back, nb, 0, b);
    EXPECT(same(a, na, b, nbb));
    na = keep(stream, n, 1, a);
    nbb = keep(back, nb, 1, b);
    EXPECT(same(a, na, b, nbb));
    EXPECT(na > 1000);
}

/*
 * The port IDs must never change between versions, or apps would lose track of the port, so
 * they're pinned here to values worked out separately (FNV-1a, in Python). They must also
 * differ by direction and by attempt, and never be 0.
 */
static void test_port_ids(void)
{
    int32_t seen[64];
    unsigned i, j, n = 0;
    int all_positive = 1, all_different = 1;

    EXPECT(ua101_midi_port_id("UA-101 MIDI", 0, 0) == 102697888);
    EXPECT(ua101_midi_port_id("UA-101 MIDI", 0, 1) == 1447774897);
    EXPECT(ua101_midi_port_id("UA-101 MIDI", 1, 0) == 1596358181);
    EXPECT(ua101_midi_port_id("UA-101 MIDI", 1, 1) == 251281172);
    EXPECT(ua101_midi_port_id("UA-101 MIDI", 0, 0) == ua101_midi_port_id("UA-101 MIDI", 0, 0));
    EXPECT(ua101_midi_port_id("UA-101 MIDI", 0, 0) != ua101_midi_port_id("UA-101 MIDI 2", 0, 0));
    for (i = 0; i < 32; i++) {
        seen[n++] = ua101_midi_port_id("UA-101 MIDI", 0, i);
        seen[n++] = ua101_midi_port_id("UA-101 MIDI", 1, i);
    }
    for (i = 0; i < n; i++) {
        if (seen[i] <= 0)
            all_positive = 0;
        for (j = i + 1; j < n; j++)
            if (seen[i] == seen[j])
                all_different = 0;
    }
    EXPECT(all_positive);
    EXPECT(all_different);
    EXPECT(ua101_midi_port_id("", 0, 0) > 0);
}

int main(void)
{
    test_port_ids();
    test_data_bytes();
    test_tx_short();
    test_tx_dropped();
    test_tx_sysex();
    test_tx_cable_guard();
    test_tx_fuzz();
    test_unpack();
    test_rx_ump();
    test_ump_to_bytes();
    test_round_trip();

    printf("%d of %d checks passed\n", g_tests - g_failures, g_tests);
    return g_failures == 0 ? 0 : 1;
}
