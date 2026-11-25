/*
 * Tests for the UA-101 descriptor parser. Runs on any computer, with no
 * hardware needed. The fixture comes from real lsusb dumps at 48 kHz in Linux
 * kernel bug 212477.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../src/ua101_desc.h"

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

/* Byte offsets into the fixture, used to build faulty variations. */
#define OFF_PLAYBACK_FORMAT 34
#define OFF_PLAYBACK_EP 45
#define OFF_CAPTURE_FORMAT 84
#define OFF_CAPTURE_EP 95

/*
 * lsusb doesn't print class-specific endpoint descriptors on vendor-class
 * interfaces, but wTotalLength is 14 bytes longer than what it shows. The two
 * 07 25 entries below are an assumption that accounts for those bytes.
 */
static const uint8_t k_real_48k[155] = {
    /* configuration */
    0x09, 0x02, 0x9b, 0x00, 0x03, 0x01, 0x00, 0xc0, 0x00,
    /* interface 0 alt 0: playback idle */
    0x09, 0x04, 0x00, 0x00, 0x00, 0xff, 0x01, 0x02, 0x00,
    /* interface 0 alt 1: playback streaming */
    0x09, 0x04, 0x00, 0x01, 0x01, 0xff, 0x01, 0x02, 0x00,
    0x07, 0x24, 0x01, 0x01, 0x00, 0x01, 0x00,
    0x0b, 0x24, 0x02, 0x01, 0x0a, 0x04, 0x18, 0x01, 0x80, 0xbb, 0x00,
    0x07, 0x05, 0x02, 0x05, 0x18, 0x01, 0x01,
    0x07, 0x25, 0x01, 0x00, 0x00, 0x00, 0x00,
    /* interface 1 alt 0: capture idle */
    0x09, 0x04, 0x01, 0x00, 0x00, 0xff, 0x01, 0x02, 0x00,
    /* interface 1 alt 1: capture streaming */
    0x09, 0x04, 0x01, 0x01, 0x01, 0xff, 0x01, 0x02, 0x00,
    0x07, 0x24, 0x01, 0x07, 0x00, 0x01, 0x00,
    0x0b, 0x24, 0x02, 0x01, 0x0c, 0x04, 0x18, 0x01, 0x80, 0xbb, 0x00,
    0x07, 0x05, 0x81, 0x25, 0x50, 0x01, 0x01,
    0x07, 0x25, 0x01, 0x00, 0x00, 0x00, 0x00,
    /* interface 2 alt 0: MIDI with bulk endpoints */
    0x09, 0x04, 0x02, 0x00, 0x02, 0xff, 0x01, 0x03, 0x00,
    0x07, 0x05, 0x04, 0x02, 0x00, 0x02, 0x01,
    0x07, 0x05, 0x83, 0x02, 0x00, 0x02, 0x01,
    /* interface 2 alt 1: MIDI with an interrupt IN endpoint */
    0x09, 0x04, 0x02, 0x01, 0x02, 0xff, 0x01, 0x03, 0x00,
    0x07, 0x05, 0x04, 0x02, 0x00, 0x02, 0x01,
    0x07, 0x05, 0x83, 0x03, 0x00, 0x02, 0x01,
};

static void test_real_descriptor(void)
{
    ua101_config c;
    const ua101_alt *play, *cap, *midi;

    EXPECT(ua101_parse_config(k_real_48k, sizeof(k_real_48k), &c) == UA101_PARSE_OK);
    EXPECT(c.total_length == 155);
    EXPECT(c.num_interfaces == 3);
    EXPECT(c.config_value == 1);
    EXPECT(c.attributes == 0xc0);
    EXPECT(c.truncated == 0);
    EXPECT(c.num_alts == 6);

    play = ua101_find_alt(&c, UA101_IF_PLAYBACK, 1);
    EXPECT(play != NULL);
    if (play != NULL) {
        EXPECT(play->iface_class == 0xff);
        EXPECT(play->iface_subclass == 1);
        EXPECT(play->iface_protocol == 2);
        EXPECT(play->has_format);
        EXPECT(play->channels == 10);
        EXPECT(play->subframe_bytes == 4);
        EXPECT(play->bit_resolution == 24);
        EXPECT(play->rate_hz == 48000);
        EXPECT(play->num_endpoints == 1);
        EXPECT(play->endpoints[0].address == 0x02);
        EXPECT(play->endpoints[0].max_packet == 280);
        EXPECT(play->endpoints[0].interval == 1);
        EXPECT(ua101_endpoint_is_isochronous(&play->endpoints[0]));
        EXPECT(!ua101_endpoint_is_in(&play->endpoints[0]));
    }

    cap = ua101_find_alt(&c, UA101_IF_CAPTURE, 1);
    EXPECT(cap != NULL);
    if (cap != NULL) {
        EXPECT(cap->channels == 12);
        EXPECT(cap->subframe_bytes == 4);
        EXPECT(cap->rate_hz == 48000);
        EXPECT(cap->endpoints[0].address == 0x81);
        EXPECT(cap->endpoints[0].attributes == 37);
        EXPECT(cap->endpoints[0].max_packet == 336);
        EXPECT(ua101_endpoint_is_in(&cap->endpoints[0]));
    }

    midi = ua101_find_alt(&c, UA101_IF_MIDI, 0);
    EXPECT(midi != NULL);
    if (midi != NULL) {
        EXPECT(midi->iface_protocol == 3);
        EXPECT(midi->num_endpoints == 2);
        EXPECT(ua101_endpoint_is_bulk(&midi->endpoints[0]));
        EXPECT(ua101_endpoint_is_bulk(&midi->endpoints[1]));
        EXPECT(midi->endpoints[0].max_packet == 512);
    }

    EXPECT(ua101_find_alt(&c, UA101_IF_MIDI, 7) == NULL);
    EXPECT(ua101_find_alt(&c, 9, 0) == NULL);
    EXPECT(ua101_check_config(&c, NULL, NULL) == 0);
}

static int failures_for(const uint8_t *data, size_t len)
{
    ua101_config c;

    if (ua101_parse_config(data, len, &c) != UA101_PARSE_OK)
        return -1;
    return ua101_check_config(&c, NULL, NULL);
}

static void test_fixture_offsets(void)
{
    /* The faulty variations below are only valid if these offsets are right. */
    EXPECT(k_real_48k[OFF_PLAYBACK_FORMAT] == 0x0b);
    EXPECT(k_real_48k[OFF_PLAYBACK_FORMAT + 4] == 0x0a);
    EXPECT(k_real_48k[OFF_PLAYBACK_EP + 1] == 0x05);
    EXPECT(k_real_48k[OFF_PLAYBACK_EP + 2] == 0x02);
    EXPECT(k_real_48k[OFF_CAPTURE_FORMAT] == 0x0b);
    EXPECT(k_real_48k[OFF_CAPTURE_FORMAT + 4] == 0x0c);
    EXPECT(k_real_48k[OFF_CAPTURE_EP + 1] == 0x05);
    EXPECT(k_real_48k[OFF_CAPTURE_EP + 2] == 0x81);
}

static void test_detects_problems(void)
{
    uint8_t d[sizeof(k_real_48k)];

    /* Capture rate differs from playback rate. */
    memcpy(d, k_real_48k, sizeof(d));
    d[OFF_CAPTURE_FORMAT + 8] = 0x44; /* 44100 is 0x00ac44 */
    d[OFF_CAPTURE_FORMAT + 9] = 0xac;
    EXPECT(failures_for(d, sizeof(d)) > 0);

    /* Format descriptor missing. */
    memcpy(d, k_real_48k, sizeof(d));
    d[OFF_PLAYBACK_FORMAT + 2] = 0x7f;
    EXPECT(failures_for(d, sizeof(d)) > 0);

    /* Capture endpoint pointing the wrong way. */
    memcpy(d, k_real_48k, sizeof(d));
    d[OFF_CAPTURE_EP + 2] = 0x01;
    EXPECT(failures_for(d, sizeof(d)) > 0);

    /* Capture endpoint isn't isochronous. */
    memcpy(d, k_real_48k, sizeof(d));
    d[OFF_CAPTURE_EP + 3] = 0x02;
    EXPECT(failures_for(d, sizeof(d)) > 0);

    /* Playback packet too small for the largest capture packet. */
    memcpy(d, k_real_48k, sizeof(d));
    d[OFF_PLAYBACK_EP + 4] = 100;
    d[OFF_PLAYBACK_EP + 5] = 0;
    EXPECT(failures_for(d, sizeof(d)) > 0);

    /* Unexpected bit depth. */
    memcpy(d, k_real_48k, sizeof(d));
    d[OFF_PLAYBACK_FORMAT + 6] = 16;
    EXPECT(failures_for(d, sizeof(d)) > 0);

    /* Zero channels must not cause a division by zero. */
    memcpy(d, k_real_48k, sizeof(d));
    d[OFF_CAPTURE_FORMAT + 4] = 0;
    EXPECT(failures_for(d, sizeof(d)) > 0);

    /* Wrong interface count in the header. */
    memcpy(d, k_real_48k, sizeof(d));
    d[4] = 2;
    EXPECT(failures_for(d, sizeof(d)) > 0);
}

static void test_other_valid_formats(void)
{
    uint8_t d[sizeof(k_real_48k)];
    ua101_config c;
    const ua101_alt *alt;

    /* 3-byte samples still fit in the same packets. */
    memcpy(d, k_real_48k, sizeof(d));
    d[OFF_PLAYBACK_FORMAT + 5] = 3;
    d[OFF_CAPTURE_FORMAT + 5] = 3;
    EXPECT(failures_for(d, sizeof(d)) == 0);

    /*
     * Assumed 192 kHz layout: 6 channels each way, using the 624-byte packet
     * size the Linux driver mentions. No real 192 kHz dump was available.
     */
    memcpy(d, k_real_48k, sizeof(d));
    d[OFF_PLAYBACK_FORMAT + 4] = 6;
    d[OFF_CAPTURE_FORMAT + 4] = 6;
    d[OFF_PLAYBACK_FORMAT + 8] = 0x00; /* 192000 is 0x02ee00 */
    d[OFF_PLAYBACK_FORMAT + 9] = 0xee;
    d[OFF_PLAYBACK_FORMAT + 10] = 0x02;
    d[OFF_CAPTURE_FORMAT + 8] = 0x00;
    d[OFF_CAPTURE_FORMAT + 9] = 0xee;
    d[OFF_CAPTURE_FORMAT + 10] = 0x02;
    d[OFF_PLAYBACK_EP + 4] = 0x70; /* 624 is 0x0270 */
    d[OFF_PLAYBACK_EP + 5] = 0x02;
    d[OFF_CAPTURE_EP + 4] = 0x70;
    d[OFF_CAPTURE_EP + 5] = 0x02;
    EXPECT(ua101_parse_config(d, sizeof(d), &c) == UA101_PARSE_OK);
    alt = ua101_find_alt(&c, UA101_IF_CAPTURE, 1);
    EXPECT(alt != NULL && alt->rate_hz == 192000 && alt->channels == 6);
    EXPECT(ua101_check_config(&c, NULL, NULL) == 0);

    /* The multiplier bits must not be counted as packet size. */
    memcpy(d, k_real_48k, sizeof(d));
    d[OFF_CAPTURE_EP + 5] = 0x09; /* 0x0950: multiplier bit set, size 336 */
    EXPECT(ua101_parse_config(d, sizeof(d), &c) == UA101_PARSE_OK);
    alt = ua101_find_alt(&c, UA101_IF_CAPTURE, 1);
    EXPECT(alt != NULL && alt->endpoints[0].max_packet == 336);
}

static void test_malformed_input(void)
{
    uint8_t d[sizeof(k_real_48k)];
    ua101_config c;
    uint8_t many[9 + 9 * (UA101_MAX_ALTS + 1)];
    size_t i;

    EXPECT(ua101_parse_config(NULL, 0, &c) == UA101_PARSE_BAD_HEADER);
    EXPECT(ua101_parse_config(k_real_48k, 0, &c) == UA101_PARSE_BAD_HEADER);
    EXPECT(ua101_parse_config(k_real_48k, 8, &c) == UA101_PARSE_BAD_HEADER);

    /* Wrong descriptor type at the start. */
    memcpy(d, k_real_48k, sizeof(d));
    d[1] = 0x01;
    EXPECT(ua101_parse_config(d, sizeof(d), &c) == UA101_PARSE_BAD_HEADER);

    /* Header length that runs past the end. */
    memcpy(d, k_real_48k, sizeof(d));
    d[0] = 200;
    EXPECT(ua101_parse_config(d, sizeof(d), &c) == UA101_PARSE_BAD_HEADER);

    /* A zero-length descriptor would otherwise loop forever. */
    memcpy(d, k_real_48k, sizeof(d));
    d[9] = 0;
    EXPECT(ua101_parse_config(d, sizeof(d), &c) == UA101_PARSE_BAD_DESCRIPTOR);

    /* A descriptor that claims to run past the end. */
    memcpy(d, k_real_48k, sizeof(d));
    d[148] = 0xff;
    EXPECT(ua101_parse_config(d, sizeof(d), &c) == UA101_PARSE_BAD_DESCRIPTOR);

    /* An interface descriptor that is too short. */
    memcpy(d, k_real_48k, sizeof(d));
    d[9] = 8;
    EXPECT(ua101_parse_config(d, sizeof(d), &c) == UA101_PARSE_BAD_DESCRIPTOR);

    /* An endpoint before any interface. */
    {
        static const uint8_t early[] = {0x09, 0x02, 0x10, 0x00, 0x00, 0x01, 0x00,
                                        0xc0, 0x00, 0x07, 0x05, 0x81, 0x01, 0x10,
                                        0x00, 0x01};
        EXPECT(ua101_parse_config(early, sizeof(early), &c) ==
               UA101_PARSE_BAD_DESCRIPTOR);
    }

    /* A single stray byte after the last descriptor. */
    {
        static const uint8_t stray[] = {0x09, 0x02, 0x0a, 0x00, 0x00, 0x01,
                                        0x00, 0xc0, 0x00, 0x07};
        EXPECT(ua101_parse_config(stray, sizeof(stray), &c) ==
               UA101_PARSE_BAD_DESCRIPTOR);
    }

    /* Fewer bytes supplied than the header promises. */
    EXPECT(ua101_parse_config(k_real_48k, 100, &c) != UA101_PARSE_OK || c.truncated);
    EXPECT(failures_for(k_real_48k, 100) != 0);

    /* More interfaces than the parser has room for. */
    memset(many, 0, sizeof(many));
    many[0] = 9;
    many[1] = 0x02;
    many[2] = (uint8_t)(sizeof(many) & 0xff);
    many[3] = (uint8_t)(sizeof(many) >> 8);
    for (i = 0; i <= UA101_MAX_ALTS; i++) {
        many[9 + 9 * i] = 9;
        many[9 + 9 * i + 1] = 0x04;
        many[9 + 9 * i + 2] = (uint8_t)i;
    }
    EXPECT(ua101_parse_config(many, sizeof(many), &c) == UA101_PARSE_TOO_MANY);
}

/* Every prefix and many random corruptions must parse safely, whatever they do. */
static void test_robustness(void)
{
    uint8_t d[sizeof(k_real_48k)];
    ua101_config c;
    uint32_t state = 0x1234abcd;
    size_t len;
    int round;

    for (len = 0; len <= sizeof(k_real_48k); len++) {
        ua101_parse_result r = ua101_parse_config(k_real_48k, len, &c);
        if (r == UA101_PARSE_OK)
            (void)ua101_check_config(&c, NULL, NULL);
    }

    for (round = 0; round < 300000; round++) {
        int flips, i;

        memcpy(d, k_real_48k, sizeof(d));
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        flips = 1 + (int)(state % 8);
        for (i = 0; i < flips; i++) {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            d[state % sizeof(d)] = (uint8_t)(state >> 24);
        }
        if (ua101_parse_config(d, sizeof(d), &c) == UA101_PARSE_OK) {
            (void)ua101_check_config(&c, NULL, NULL);
            if (c.num_alts > UA101_MAX_ALTS) {
                EXPECT(0);
                return;
            }
        }
    }
    EXPECT(1);
}

static int g_reported;

static void count_reports(void *ctx, int ok, const char *message)
{
    (void)ctx;
    (void)ok;
    if (message != NULL && message[0] != '\0')
        g_reported++;
}

static void test_reporting(void)
{
    ua101_config c;

    EXPECT(ua101_parse_config(k_real_48k, sizeof(k_real_48k), &c) == UA101_PARSE_OK);
    g_reported = 0;
    EXPECT(ua101_check_config(&c, count_reports, NULL) == 0);
    EXPECT(g_reported >= 20);
    EXPECT(strcmp(ua101_parse_result_string(UA101_PARSE_OK), "ok") == 0);
}

int main(void)
{
    test_fixture_offsets();
    test_real_descriptor();
    test_detects_problems();
    test_other_valid_formats();
    test_malformed_input();
    test_robustness();
    test_reporting();

    printf("%d of %d checks passed\n", g_tests - g_failures, g_tests);
    return g_failures == 0 ? 0 : 1;
}
