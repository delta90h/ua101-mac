/*
 * Tests for the UA-101 control protocol's read side. Runs on any computer, with
 * no hardware needed. The settings blocks are built here to match the layout in
 * docs/control-protocol.md. They aren't recordings from a real unit.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../src/ua101_control.h"

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

static void test_checksum(void)
{
    /* These five checksums are hardcoded in Roland's driver. */
    const uint8_t startup[] = {2, 0, 0, 0, 0, 0, 2, 0};
    const uint8_t read72[] = {1, 0, 0, 0, 0, 0, 0, 0x48};
    const uint8_t reset[] = {0, 0, 0, 2, 0};
    const uint8_t resume[] = {0, 0, 0, 0, 1};
    const uint8_t suspend[] = {0, 0, 0, 0, 0};

    EXPECT(ua101_ctl_checksum(startup, sizeof(startup)) == 0x7C);
    EXPECT(ua101_ctl_checksum(read72, sizeof(read72)) == 0x37);
    EXPECT(ua101_ctl_checksum(reset, sizeof(reset)) == 0x7E);
    EXPECT(ua101_ctl_checksum(resume, sizeof(resume)) == 0x7F);
    EXPECT(ua101_ctl_checksum(suspend, sizeof(suspend)) == 0x00);
}

static void test_read_request(void)
{
    const uint8_t expected[UA101_CTL_READ_REQUEST_BYTES] = {0xF0, 0x41, 0x10, 0x00, 0x69, 0x11,
                                                            0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
                                                            0x02, 0x00, 0x7C, 0xF7};
    const uint8_t expected_packets[UA101_CTL_READ_PACKET_BYTES] = {
        0x14, 0xF0, 0x41, 0x10, 0x14, 0x00, 0x69, 0x11, 0x14, 0x02, 0x00, 0x00,
        0x14, 0x00, 0x00, 0x00, 0x14, 0x02, 0x00, 0x7C, 0x15, 0xF7, 0x00, 0x00};
    uint8_t request[UA101_CTL_READ_REQUEST_BYTES];
    uint8_t packets[UA101_CTL_READ_PACKET_BYTES];

    ua101_ctl_read_request(request);
    EXPECT(memcmp(request, expected, sizeof(expected)) == 0);
    EXPECT(ua101_ctl_is_request_only(request, sizeof(request)));

    EXPECT(ua101_ctl_pack_sysex(request, sizeof(request), UA101_CTL_CABLE, packets,
                                sizeof(packets)) == sizeof(packets));
    EXPECT(memcmp(packets, expected_packets, sizeof(packets)) == 0);

    /* It won't overrun a buffer that's too small. */
    EXPECT(ua101_ctl_pack_sysex(request, sizeof(request), UA101_CTL_CABLE, packets,
                                sizeof(packets) - 1) == 0);
}

/* The guard in front of the only send in the tool has to refuse every write. */
static void test_request_guard(void)
{
    /* Reset all, a real destructive message. */
    const uint8_t reset_all[] = {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00,
                                 0x00, 0x00, 0x02, 0x00, 0x7E, 0xF7};
    uint8_t msg[UA101_CTL_READ_REQUEST_BYTES];

    EXPECT(!ua101_ctl_is_request_only(reset_all, sizeof(reset_all)));

    /* A set with a request's length, and an otherwise valid checksum. */
    ua101_ctl_read_request(msg);
    msg[5] = 0x12;
    EXPECT(!ua101_ctl_is_request_only(msg, sizeof(msg)));

    ua101_ctl_read_request(msg);
    msg[14] ^= 1;
    EXPECT(!ua101_ctl_is_request_only(msg, sizeof(msg)));

    ua101_ctl_read_request(msg);
    msg[4] = 0x6A; /* another model */
    EXPECT(!ua101_ctl_is_request_only(msg, sizeof(msg)));

    ua101_ctl_read_request(msg);
    EXPECT(!ua101_ctl_is_request_only(msg, sizeof(msg) - 1));

    /* The helper's check is stricter: the one request, byte for byte. */
    ua101_ctl_read_request(msg);
    EXPECT(ua101_ctl_is_read_request(msg, sizeof(msg)));
    EXPECT(!ua101_ctl_is_read_request(msg, sizeof(msg) - 1));
    EXPECT(!ua101_ctl_is_read_request(NULL, sizeof(msg)));
    EXPECT(!ua101_ctl_is_read_request(reset_all, sizeof(reset_all)));
    {
        size_t i;
        unsigned bit, let_through = 0;

        /* Any single bit changed anywhere, with or without a mended checksum, is refused. */
        for (i = 0; i < sizeof(msg); i++)
            for (bit = 0; bit < 8; bit++) {
                ua101_ctl_read_request(msg);
                msg[i] ^= (uint8_t)(1u << bit);
                let_through += ua101_ctl_is_read_request(msg, sizeof(msg));
                if (i == 14)
                    continue; /* mending the checksum itself gives the request back */
                msg[14] = ua101_ctl_checksum(msg + 6, 8);
                let_through += ua101_ctl_is_read_request(msg, sizeof(msg));
            }
        EXPECT(let_through == 0);
    }
    /* Requests the notes list under "Commands to avoid": well-formed, asking only, refused. */
    ua101_ctl_read_request(msg);
    msg[7] = 0x01; /* 02 01 00 00 */
    msg[14] = ua101_ctl_checksum(msg + 6, 8);
    EXPECT(ua101_ctl_is_request_only(msg, sizeof(msg)));
    EXPECT(!ua101_ctl_is_read_request(msg, sizeof(msg)));
    ua101_ctl_read_request(msg);
    msg[6] = 0x01; /* 01 00 00 00 */
    msg[14] = ua101_ctl_checksum(msg + 6, 8);
    EXPECT(ua101_ctl_is_request_only(msg, sizeof(msg)));
    EXPECT(!ua101_ctl_is_read_request(msg, sizeof(msg)));
}

static void put_nibbles(uint8_t *out, uint32_t value, unsigned count)
{
    unsigned i;

    for (i = 0; i < count; i++)
        out[i] = (uint8_t)((value >> (4 * (count - 1 - i))) & 0x0F);
}

/* A block with a different, recognizable value in every field. */
static void make_block(uint8_t block[UA101_CTL_BLOCK_BYTES])
{
    unsigned c;

    memset(block, 0, UA101_CTL_BLOCK_BYTES);
    block[0] = block[1] = block[2] = block[3] = 1;
    block[4] = 0x09; /* rate code 1, bit 3 set */
    block[5] = 0x0A; /* clock source 2, status set */
    block[6] = 0x04; /* Soft Control on, Mono off */
    for (c = 0; c < 15; c++)
        block[7 + c] = (uint8_t)(c + 1);
    block[0x16] = 3;
    block[0x17] = 5;
    block[0x18] = 0x01;
    block[0x19] = 0x05; /* group 1 pairs 0 and 2 */
    block[0x1A] = 0x01; /* group 1 pair 4 */
    block[0x1B] = 0x02; /* group 2 pair 1 */
    block[0x1D] = 0x02; /* group 0 channel 1 */
    block[0x1E] = 0x01; /* group 1 channel 0 */
    block[0x20] = 0x02; /* group 1 channel 9 */
    block[0x23] = 0x01; /* group 2 channel 8 */
    for (c = 0; c < 10; c++) {
        put_nibbles(block + 0x24 + c * 4, 0x1000u + c, 4);
        put_nibbles(block + 0x4C + c * 4, 0x2000u + c, 4);
        put_nibbles(block + 0x80 + c * 6, 0x100000u + c, 6);
        put_nibbles(block + 0xBC + c * 6, 0x200000u + c, 6);
    }
    put_nibbles(block + 0x74, 0x7FFFFFu, 6);
    put_nibbles(block + 0x7A, 0x123456u, 6);
}

static void test_settings_parse(void)
{
    uint8_t block[UA101_CTL_BLOCK_BYTES];
    ua101_settings s;

    make_block(block);
    ua101_settings_parse(block, &s);

    EXPECT(s.section_flags[0] == 1 && s.section_flags[3] == 1);
    EXPECT(s.rate_code == 1);
    EXPECT(s.d4_bit3 == 1);
    EXPECT(s.clock_source == 2);
    EXPECT(s.clock_status == 1);
    EXPECT(s.d6_bit0 == 0);
    EXPECT(s.mono == 0);
    EXPECT(s.soft_control == 1);

    /* The UA-101's dialog uses row 1, at bytes 12 to 16. */
    EXPECT(s.patch[0][0] == 1);
    EXPECT(s.patch[1][0] == 6);
    EXPECT(s.patch[1][4] == 10);
    EXPECT(s.patch[2][4] == 15);

    EXPECT(s.solo[0] == 3 && s.solo[1] == 5);

    EXPECT(s.link[0][0] == 1);
    EXPECT(s.link[1][0] == 1 && s.link[1][1] == 0 && s.link[1][2] == 1);
    EXPECT(s.link[1][4] == 1); /* crosses into the second byte */
    EXPECT(s.link[2][0] == 0 && s.link[2][1] == 1);

    EXPECT(s.channel[0][0].on == 0 && s.channel[0][1].on == 1);
    EXPECT(s.channel[1][0].on == 1 && s.channel[1][1].on == 0);
    EXPECT(s.channel[1][9].on == 1); /* third byte, bit 1 */
    EXPECT(s.channel[2][8].on == 1);

    /* Group 0 has no pan of its own. */
    EXPECT(s.channel[0][0].pan == 0x4000 && s.channel[0][1].pan == 0x4000);
    EXPECT(s.channel[1][0].pan == 0x1000 && s.channel[1][9].pan == 0x1009);
    EXPECT(s.channel[2][0].pan == 0x2000 && s.channel[2][9].pan == 0x2009);

    EXPECT(s.channel[0][0].volume == 0x7FFFFF);
    EXPECT(s.channel[0][1].volume == 0x123456);
    EXPECT(s.channel[1][0].volume == 0x100000 && s.channel[1][9].volume == 0x100009);
    EXPECT(s.channel[2][0].volume == 0x200000 && s.channel[2][9].volume == 0x200009);
    EXPECT(s.bad_nibbles == 0);

    /* A byte too big for a nibble is counted, and can't spill into its neighbor. */
    block[0x24] = 0x7F;
    ua101_settings_parse(block, &s);
    EXPECT(s.bad_nibbles == 1);
    EXPECT(s.channel[1][0].pan == 0xF000);
}

static void test_values(void)
{
    EXPECT(ua101_ctl_rate_hz(0) == 44100);
    EXPECT(ua101_ctl_rate_hz(1) == 48000);
    EXPECT(ua101_ctl_rate_hz(4) == 192000);
    EXPECT(ua101_ctl_rate_hz(5) == 0);

    EXPECT(ua101_ctl_patch_source(0) == NULL);
    EXPECT(strcmp(ua101_ctl_patch_source(1), "MONITOR OUT") == 0);
    EXPECT(strcmp(ua101_ctl_patch_source(2), "INPUT 1-2") == 0);
    EXPECT(strcmp(ua101_ctl_patch_source(7), "WAVE OUT 1-2") == 0);
    EXPECT(strcmp(ua101_ctl_patch_source(11), "WAVE OUT 9-10") == 0);
    EXPECT(ua101_ctl_patch_source(12) == NULL);

    EXPECT(fabs(ua101_ctl_volume_db(UA101_CTL_UNITY_VOLUME)) < 1e-9);
    EXPECT(fabs(ua101_ctl_volume_db(1) + 126.4) < 0.05); /* the app's lowest label */
    EXPECT(fabs(ua101_ctl_volume_db(0x7FFFFF) - 12.04) < 0.01);
    EXPECT(isinf(ua101_ctl_volume_db(0)));
}

/* Wraps a block in a message, like the one the unit should send. */
static size_t make_block_message(uint8_t *out)
{
    uint8_t block[UA101_CTL_BLOCK_BYTES];
    const uint8_t head[] = {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x02, 0x00, 0x00, 0x00};

    make_block(block);
    memcpy(out, head, sizeof(head));
    memcpy(out + sizeof(head), block, sizeof(block));
    out[sizeof(head) + sizeof(block)] = ua101_ctl_checksum(out + 6, 4 + sizeof(block));
    out[sizeof(head) + sizeof(block) + 1] = 0xF7;
    return sizeof(head) + sizeof(block) + 2;
}

static void test_classify(void)
{
    uint8_t msg[UA101_CTL_BLOCK_MESSAGE_BYTES];
    uint8_t change[] = {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00,
                        0x01, 0x00, 0x05, 0x01, 0x00, 0xF7};
    const uint8_t other[] = {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00,
                             0x00, 0x00, 0x02, 0x00, 0x7E, 0xF7};
    ua101_ctl_view view;
    size_t len = make_block_message(msg);

    EXPECT(len == UA101_CTL_BLOCK_MESSAGE_BYTES);
    EXPECT(ua101_ctl_classify(msg, len, &view) == UA101_CTL_SETTINGS);
    EXPECT(view.data == msg + 10);
    EXPECT(view.data_len == UA101_CTL_BLOCK_BYTES);

    /* One changed byte anywhere is caught by the checksum. */
    msg[100] ^= 1;
    EXPECT(ua101_ctl_classify(msg, len, &view) == UA101_CTL_BAD_CHECKSUM);
    msg[100] ^= 1;

    /* A cut-off message isn't a settings block, whatever else it is. */
    msg[len - 3] = 0xF7;
    EXPECT(ua101_ctl_classify(msg, len - 2, &view) != UA101_CTL_SETTINGS);

    change[11] = ua101_ctl_checksum(change + 6, 5);
    EXPECT(ua101_ctl_classify(change, sizeof(change), &view) == UA101_CTL_CHANGE);
    EXPECT(view.address[3] == 5);
    EXPECT(view.data[0] == 1);

    EXPECT(ua101_ctl_classify(other, sizeof(other), &view) == UA101_CTL_OTHER);

    /* Not this model, or not even long enough to hold an address. */
    change[4] = 0x6A;
    EXPECT(ua101_ctl_classify(change, sizeof(change), &view) == UA101_CTL_BAD_FORMAT);
    EXPECT(ua101_ctl_classify(other, 4, &view) == UA101_CTL_BAD_FORMAT);
    EXPECT(ua101_ctl_classify(other, 0, &view) == UA101_CTL_BAD_FORMAT);
}

static int feed_all(ua101_sysex_rx *rx, const uint8_t *packets, size_t bytes, const uint8_t **msg,
                    size_t *len)
{
    size_t i;
    int complete = 0;

    for (i = 0; i + 4 <= bytes; i += 4)
        if (ua101_sysex_rx_feed(rx, packets + i, msg, len))
            complete++;
    return complete;
}

static void test_reassembly(void)
{
    uint8_t message[UA101_CTL_BLOCK_MESSAGE_BYTES];
    uint8_t packets[UA101_CTL_BLOCK_MESSAGE_BYTES * 2];
    uint8_t mixed[UA101_CTL_BLOCK_MESSAGE_BYTES * 4];
    ua101_sysex_rx rx;
    const uint8_t *got = NULL;
    size_t got_len = 0, message_len, packed, i, n = 0;

    message_len = make_block_message(message);
    packed = ua101_ctl_pack_sysex(message, message_len, UA101_CTL_CABLE, packets, sizeof(packets));
    EXPECT(packed == (message_len + 2) / 3 * 4);

    /* A whole message comes back byte for byte, in pieces of any size. */
    ua101_sysex_rx_init(&rx);
    EXPECT(feed_all(&rx, packets, packed, &got, &got_len) == 1);
    EXPECT(got_len == message_len);
    EXPECT(got != NULL && memcmp(got, message, message_len) == 0);

    /* Normal MIDI on cable 0 and padding are skipped, without disturbing the message. */
    for (i = 0; i + 4 <= packed; i += 4) {
        const uint8_t note[4] = {0x09, 0x90, 0x3C, 0x7F};
        const uint8_t pad[4] = {0, 0, 0, 0};

        memcpy(mixed + n, packets + i, 4);
        n += 4;
        memcpy(mixed + n, note, 4);
        n += 4;
        if (i % 40 == 0 && n + 4 <= sizeof(mixed)) {
            memcpy(mixed + n, pad, 4);
            n += 4;
        }
    }
    ua101_sysex_rx_init(&rx);
    EXPECT(feed_all(&rx, mixed, n, &got, &got_len) == 1);
    EXPECT(got_len == message_len && memcmp(got, message, message_len) == 0);
    EXPECT(rx.other_cable_packets > 0);

    /* A SysEx message on cable 0 belongs to a MIDI app, and is never treated as control. */
    {
        uint8_t cable0[sizeof(packets)];

        packed = ua101_ctl_pack_sysex(message, message_len, 0, cable0, sizeof(cable0));
        ua101_sysex_rx_init(&rx);
        EXPECT(feed_all(&rx, cable0, packed, &got, &got_len) == 0);
    }

    /* Real-time bytes inside a message don't damage it. */
    {
        const uint8_t with_clock[] = {0x14, 0xF0, 0xF8, 0x41, 0x15, 0xF7, 0, 0};
        const uint8_t expect[] = {0xF0, 0x41, 0xF7};

        ua101_sysex_rx_init(&rx);
        EXPECT(feed_all(&rx, with_clock, sizeof(with_clock), &got, &got_len) == 1);
        EXPECT(got_len == 3 && memcmp(got, expect, 3) == 0);
    }

    /* Bytes before a start, a status in the middle, and a message that never ends. */
    {
        const uint8_t stray[] = {0x14, 0x01, 0x02, 0x03, 0x15, 0xF7, 0, 0};
        const uint8_t cut[] = {0x14, 0xF0, 0x41, 0x90, 0x15, 0xF7, 0, 0};
        uint8_t endless[4 * 200];

        ua101_sysex_rx_init(&rx);
        EXPECT(feed_all(&rx, stray, sizeof(stray), &got, &got_len) == 0);
        ua101_sysex_rx_init(&rx);
        EXPECT(feed_all(&rx, cut, sizeof(cut), &got, &got_len) == 0);

        ua101_sysex_rx_init(&rx);
        endless[0] = 0x14;
        endless[1] = 0xF0;
        endless[2] = endless[3] = 0x01;
        for (i = 4; i < sizeof(endless); i += 4) {
            endless[i] = 0x14;
            endless[i + 1] = endless[i + 2] = endless[i + 3] = 0x01;
        }
        EXPECT(feed_all(&rx, endless, sizeof(endless), &got, &got_len) == 0);
        EXPECT(rx.overflows == 1);
    }
}

/* The developer's read-only look at the other blocks (ua101settings --all-blocks). */
static void test_block_requests(void)
{
    const uint8_t memory1[UA101_CTL_READ_REQUEST_BYTES] = {0xF0, 0x41, 0x10, 0x00, 0x69, 0x11,
                                                           0x02, 0x03, 0x00, 0x00, 0x00, 0x00,
                                                           0x02, 0x00, 0x79, 0xF7};
    uint8_t msg[UA101_CTL_READ_REQUEST_BYTES], zero[UA101_CTL_READ_REQUEST_BYTES];
    uint8_t reply[UA101_CTL_BLOCK_MESSAGE_BYTES];
    ua101_ctl_view view;
    unsigned b, block = 99;

    ua101_ctl_read_request(zero);
    EXPECT(ua101_ctl_block_request(0, msg) && memcmp(msg, zero, sizeof(msg)) == 0);
    EXPECT(ua101_ctl_block_request(3, msg) && memcmp(msg, memory1, sizeof(msg)) == 0);
    for (b = 0; b < UA101_CTL_BLOCKS; b++) {
        EXPECT(ua101_ctl_block_request(b, msg));
        EXPECT(ua101_ctl_is_request_only(msg, sizeof(msg)));
        EXPECT(msg[5] == 0x11 && msg[6] == 0x02 && msg[7] == b);
        /* Block 0 is the settings request; 3 to 7 are the five memory reads, and no other. */
        EXPECT(ua101_ctl_is_read_request(msg, sizeof(msg)) == (b == 0));
        EXPECT(ua101_ctl_memory_read_request(msg, sizeof(msg)) ==
               (b >= 3 && b <= 7 ? (int)b - 3 : -1));
    }
    memset(msg, 0xAA, sizeof(msg));
    EXPECT(!ua101_ctl_block_request(UA101_CTL_BLOCKS, msg));
    EXPECT(!ua101_ctl_block_request(0x7F, msg));
    EXPECT(msg[0] == 0xAA && msg[15] == 0xAA);

    /* A reply from 02 05 00 00 is block 5; the settings block is still classified as before. */
    memcpy(reply, (const uint8_t[]){0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x02, 0x05, 0x00, 0x00}, 10);
    memset(reply + 10, 0, UA101_CTL_BLOCK_BYTES);
    reply[sizeof(reply) - 2] = ua101_ctl_checksum(reply + 6, 4 + UA101_CTL_BLOCK_BYTES);
    reply[sizeof(reply) - 1] = 0xF7;
    EXPECT(ua101_ctl_classify(reply, sizeof(reply), &view) == UA101_CTL_OTHER);
    EXPECT(ua101_ctl_block_reply(&view, &block) && block == 5);
    reply[7] = 0x00;
    reply[sizeof(reply) - 2] = ua101_ctl_checksum(reply + 6, 4 + UA101_CTL_BLOCK_BYTES);
    EXPECT(ua101_ctl_classify(reply, sizeof(reply), &view) == UA101_CTL_SETTINGS);
    EXPECT(ua101_ctl_block_reply(&view, &block) && block == 0);
    reply[7] = 0x10;
    reply[sizeof(reply) - 2] = ua101_ctl_checksum(reply + 6, 4 + UA101_CTL_BLOCK_BYTES);
    EXPECT(ua101_ctl_classify(reply, sizeof(reply), &view) == UA101_CTL_OTHER);
    EXPECT(!ua101_ctl_block_reply(&view, &block));
}

/* The five memory reads (link version 20): exactly those bytes, nothing near them. */
static void test_memory_requests(void)
{
    /* Written out here, from docs/control-protocol.md: 02 03 00 00 to 02 07 00 00, size
       00 00 02 00, checksums 79 to 75. */
    static const uint8_t k_reads[5][UA101_CTL_READ_REQUEST_BYTES] = {
        {0xF0, 0x41, 0x10, 0x00, 0x69, 0x11, 0x02, 0x03, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x79, 0xF7},
        {0xF0, 0x41, 0x10, 0x00, 0x69, 0x11, 0x02, 0x04, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x78, 0xF7},
        {0xF0, 0x41, 0x10, 0x00, 0x69, 0x11, 0x02, 0x05, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x77, 0xF7},
        {0xF0, 0x41, 0x10, 0x00, 0x69, 0x11, 0x02, 0x06, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x76, 0xF7},
        {0xF0, 0x41, 0x10, 0x00, 0x69, 0x11, 0x02, 0x07, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x75, 0xF7}};
    uint8_t msg[UA101_CTL_READ_REQUEST_BYTES], reply[UA101_CTL_BLOCK_MESSAGE_BYTES];
    ua101_ctl_view view;
    unsigned m, i, b;

    for (m = 0; m < 5; m++) {
        EXPECT(ua101_ctl_memory_request(m, msg) && memcmp(msg, k_reads[m], sizeof(msg)) == 0);
        EXPECT(ua101_ctl_memory_read_request(k_reads[m], sizeof(msg)) == (int)m);
        EXPECT(!ua101_ctl_is_read_request(k_reads[m], sizeof(msg)));
        EXPECT(ua101_ctl_memory_read_request(k_reads[m], sizeof(msg) - 1) == -1);
        /* Any one byte changed, to anything: no longer a memory read (another memory's request
           differs in two bytes, the block and the checksum). */
        for (i = 0; i < sizeof(msg); i++) {
            for (b = 0; b < 256; b++) {
                if (b == k_reads[m][i])
                    continue;
                memcpy(msg, k_reads[m], sizeof(msg));
                msg[i] = (uint8_t)b;
                EXPECT(ua101_ctl_memory_read_request(msg, sizeof(msg)) == -1);
            }
        }
    }
    memset(msg, 0xAA, sizeof(msg));
    EXPECT(!ua101_ctl_memory_request(5, msg) && !ua101_ctl_memory_request(0xFFFFFFFFu, msg));
    EXPECT(msg[0] == 0xAA && msg[15] == 0xAA);
    ua101_ctl_read_request(msg);
    EXPECT(ua101_ctl_memory_read_request(msg, sizeof(msg)) == -1);
    EXPECT(ua101_ctl_memory_read_request(NULL, sizeof(msg)) == -1);
    for (b = 0; b < UA101_CTL_BLOCKS; b++) {
        EXPECT(ua101_ctl_block_request(b, msg));
        EXPECT((ua101_ctl_memory_read_request(msg, sizeof(msg)) >= 0) == (b >= 3 && b <= 7));
    }

    /* A reply from each block: only 02 03 to 02 07 is a memory, 0 to 4. */
    memcpy(reply, (const uint8_t[]){0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x02, 0x00, 0x00, 0x00}, 10);
    memset(reply + 10, 0, UA101_CTL_BLOCK_BYTES);
    reply[sizeof(reply) - 1] = 0xF7;
    for (b = 0; b < UA101_CTL_BLOCKS + 1; b++) {
        reply[7] = (uint8_t)b;
        reply[sizeof(reply) - 2] = ua101_ctl_checksum(reply + 6, 4 + UA101_CTL_BLOCK_BYTES);
        EXPECT(ua101_ctl_classify(reply, sizeof(reply), &view) ==
               (b == 0 ? UA101_CTL_SETTINGS : UA101_CTL_OTHER));
        EXPECT(ua101_ctl_memory_reply(&view) == (b >= 3 && b <= 7 ? (int)b - 3 : -1));
    }
    /* A short reply from 02 03 is no memory. */
    reply[7] = 3;
    view.data_len = UA101_CTL_BLOCK_BYTES - 1;
    EXPECT(ua101_ctl_memory_reply(&view) == -1);
}

int main(void)
{
    test_checksum();
    test_read_request();
    test_block_requests();
    test_memory_requests();
    test_request_guard();
    test_settings_parse();
    test_values();
    test_classify();
    test_reassembly();

    printf("%d of %d checks passed\n", g_tests - g_failures, g_tests);
    return g_failures == 0 ? 0 : 1;
}
