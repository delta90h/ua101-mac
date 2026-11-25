/*
 * Tests for the UA-101 streaming logic and offline audio checks. Runs on any
 * computer, with no hardware needed.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/ua101_analysis.h"
#include "../src/ua101_stream.h"

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

static void test_scheduling(void)
{
    EXPECT(ua101_iso_bus_frames(8, 1) == 1);
    EXPECT(ua101_iso_bus_frames(16, 1) == 2);
    EXPECT(ua101_iso_bus_frames(4, 2) == 1);
    EXPECT(ua101_iso_bus_frames(12, 1) == 0); /* not a whole number of bus frames */
    EXPECT(ua101_iso_bus_frames(0, 1) == 0);
    EXPECT(ua101_iso_bus_frames(16, 0) == 0);
    EXPECT(ua101_iso_bus_frames(16, 5) == 0);
}

static void test_fifo(void)
{
    uint16_t slots[4];
    ua101_size_fifo fifo;
    uint16_t value = 0;
    uint16_t i;

    ua101_fifo_init(&fifo, slots, 4);
    EXPECT(ua101_fifo_pop(&fifo, &value) == 0);

    /* The same sequence comes out in the same order. */
    for (i = 5; i < 8; i++)
        ua101_fifo_push(&fifo, i);
    EXPECT(ua101_fifo_held_frames(&fifo) == 5 + 6 + 7);
    EXPECT(ua101_fifo_pop(&fifo, &value) == 1 && value == 5);
    EXPECT(ua101_fifo_pop(&fifo, &value) == 1 && value == 6);

    /* Filling past capacity drops the oldest, across the wrap. */
    ua101_fifo_push(&fifo, 1);
    ua101_fifo_push(&fifo, 2);
    ua101_fifo_push(&fifo, 3);
    EXPECT(fifo.count == 4);
    EXPECT(fifo.dropped_packets == 0);
    ua101_fifo_push(&fifo, 4);
    EXPECT(fifo.dropped_packets == 1);
    EXPECT(fifo.dropped_frames == 7);
    EXPECT(ua101_fifo_pop(&fifo, &value) == 1 && value == 1);

    /* Every frame is accounted for. */
    EXPECT(fifo.pushed_frames ==
           fifo.popped_frames + ua101_fifo_held_frames(&fifo) + fifo.dropped_frames);
}

/* Writes fake audio words, whose low byte is zero like the real format. */
static void put_audio(uint8_t *buf, size_t offset, size_t bytes)
{
    size_t i;

    for (i = 0; i < bytes; i += 4) {
        uint32_t word = (uint32_t)(0x00123400u + i * 0x100u);

        memcpy(buf + offset + i, &word, 4);
    }
}

static void test_layout(void)
{
    enum { SLOT = 336, PACKETS = 4 };
    static uint8_t buf[SLOT * PACKETS] __attribute__((aligned(4)));
    const uint16_t sizes[PACKETS] = {240, 288, 240, 288};
    const uint16_t full[PACKETS] = {SLOT, SLOT, SLOT, SLOT};
    const uint16_t none[PACKETS] = {0, 0, 0, 0};
    ua101_layout_stats stats;
    size_t offset;
    int p;

    /* Data at requested offsets, with gaps after short packets. */
    memset(&stats, 0, sizeof(stats));
    ua101_sentinel_fill(buf, sizeof(buf));
    for (p = 0; p < PACKETS; p++)
        put_audio(buf, (size_t)p * SLOT, sizes[p]);
    ua101_layout_check(buf, PACKETS, SLOT, sizes, &stats);
    EXPECT(stats.slot_bad_words == 0);
    EXPECT(stats.packed_bad_words > 0);
    EXPECT(stats.short_packets == 4);
    EXPECT(stats.data_bytes == 240 + 288 + 240 + 288);
    EXPECT(ua101_layout_verdict(&stats) == UA101_LAYOUT_SLOT);

    /* The same data run together. */
    memset(&stats, 0, sizeof(stats));
    ua101_sentinel_fill(buf, sizeof(buf));
    offset = 0;
    for (p = 0; p < PACKETS; p++) {
        put_audio(buf, offset, sizes[p]);
        offset += sizes[p];
    }
    ua101_layout_check(buf, PACKETS, SLOT, sizes, &stats);
    EXPECT(stats.packed_bad_words == 0);
    EXPECT(stats.slot_bad_words > 0);
    EXPECT(ua101_layout_verdict(&stats) == UA101_LAYOUT_PACKED);

    /* Full packets look the same in both layouts, so nothing can be concluded. */
    memset(&stats, 0, sizeof(stats));
    put_audio(buf, 0, sizeof(buf));
    ua101_layout_check(buf, PACKETS, SLOT, full, &stats);
    EXPECT(ua101_layout_verdict(&stats) == UA101_LAYOUT_UNKNOWN);

    /* So do empty ones. */
    memset(&stats, 0, sizeof(stats));
    ua101_sentinel_fill(buf, sizeof(buf));
    ua101_layout_check(buf, PACKETS, SLOT, none, &stats);
    EXPECT(ua101_layout_verdict(&stats) == UA101_LAYOUT_UNKNOWN);

    /* If the kernel zeroed the gaps, neither layout fits, which must not pass. */
    memset(&stats, 0, sizeof(stats));
    memset(buf, 0, sizeof(buf));
    ua101_layout_check(buf, PACKETS, SLOT, sizes, &stats);
    EXPECT(ua101_layout_verdict(&stats) == UA101_LAYOUT_UNKNOWN);
}

static void test_tone_limits(void)
{
    ua101_tone tone;
    double nan_value = nan("");

    EXPECT(ua101_tone_init(&tone, 44100.0, -60.0, 1.0, 1000.0, 1500.0, 0) == 0);
    EXPECT(ua101_tone_init(&tone, 44100.0, -40.0, 5.0, 1000.0, 1500.0, 0) == 0);

    EXPECT(ua101_tone_init(&tone, 44100.0, -39.9, 1.0, 1000.0, 1500.0, 0) == -1);
    EXPECT(ua101_tone_init(&tone, 44100.0, 0.0, 1.0, 1000.0, 1500.0, 0) == -1);
    EXPECT(ua101_tone_init(&tone, 44100.0, nan_value, 1.0, 1000.0, 1500.0, 0) == -1);
    EXPECT(ua101_tone_init(&tone, 44100.0, -60.0, 5.1, 1000.0, 1500.0, 0) == -1);
    EXPECT(ua101_tone_init(&tone, 44100.0, -60.0, 0.0, 1000.0, 1500.0, 0) == -1);
    EXPECT(ua101_tone_init(&tone, 44100.0, -60.0, nan_value, 1000.0, 1500.0, 0) == -1);
    EXPECT(ua101_tone_init(&tone, 44100.0, -60.0, 1.0, 30000.0, 1500.0, 0) == -1);
    EXPECT(ua101_tone_init(&tone, 44100.0, -60.0, 1.0, 1000.0, 0.0, 0) == -1);
}

static int32_t read_word(const uint8_t *buf, size_t frame, unsigned channels, unsigned ch)
{
    uint32_t word;

    memcpy(&word, buf + (frame * channels + ch) * 4, 4);
    return (int32_t)word;
}

static void test_tone_samples(void)
{
    enum { CHANNELS = 10, FRAMES = 44100 };
    uint8_t *buf = calloc(FRAMES, CHANNELS * 4);
    ua101_tone tone;
    double dbfs = -60.0;
    int64_t limit = (int64_t)(pow(10.0, dbfs / 20.0) * 2147483648.0) + 256;
    int64_t peak0 = 0, peak1 = 0;
    int wrong = 0, lowbyte = 0, others = 0;
    size_t f;
    uint32_t done;

    EXPECT(ua101_tone_init(&tone, 44100.0, dbfs, 0.5, 1000.0, 1500.0, 1000) == 0);
    EXPECT(tone.length == 22050);
    EXPECT(tone.fade == 882);
    EXPECT(ua101_tone_envelope(&tone, 0) < 0.01);
    EXPECT(ua101_tone_envelope(&tone, tone.length / 2) == 1.0);
    EXPECT(ua101_tone_envelope(&tone, tone.length - 1) < 0.01);
    EXPECT(ua101_tone_envelope(&tone, tone.length) == 0.0);

    /* Written in pieces like playback packets, which must not disturb the phase. */
    for (done = 0; done < FRAMES;) {
        uint32_t step = (done % 2) ? 5 : 6;

        if (step > FRAMES - done)
            step = FRAMES - done;
        ua101_tone_mix(&tone, buf + (size_t)done * CHANNELS * 4, step, CHANNELS, done);
        done += step;
    }

    for (f = 0; f < FRAMES; f++) {
        int32_t s0 = read_word(buf, f, CHANNELS, 0);
        int32_t s1 = read_word(buf, f, CHANNELS, 1);
        int64_t m0 = s0 < 0 ? -(int64_t)s0 : s0;
        int64_t m1 = s1 < 0 ? -(int64_t)s1 : s1;
        int ch;

        if (m0 > peak0)
            peak0 = m0;
        if (m1 > peak1)
            peak1 = m1;
        if ((s0 & 0xFF) || (s1 & 0xFF))
            lowbyte++;
        if ((f < 1000 || f >= 1000 + 22050) && (s0 != 0 || s1 != 0))
            wrong++;
        for (ch = 2; ch < CHANNELS; ch++)
            if (read_word(buf, f, CHANNELS, (unsigned)ch) != 0)
                others++;
    }
    EXPECT(wrong == 0);
    EXPECT(lowbyte == 0);
    EXPECT(others == 0);
    EXPECT(peak0 <= limit);
    EXPECT(peak1 <= limit);
    /* -60 dBFS must reach the stream at about that level, and not much less. */
    EXPECT(peak0 > limit / 2);
    EXPECT(peak1 > limit / 2);

    /* Checks one known sample's bytes: the sample at the 1 kHz quarter period. */
    {
        ua101_tone t;
        uint8_t frame[8] = {0};
        uint64_t n = 0;
        double value;
        int32_t s24;

        EXPECT(ua101_tone_init(&t, 48000.0, -60.0, 1.0, 1000.0, 1000.0, 0) == 0);
        n = 12000; /* steady part, at an exact multiple of the period */
        n += 12;   /* a quarter period later, at the positive peak */
        value = ua101_tone_value(&t, 0, n);
        s24 = (int32_t)lround(value * 8388607.0);
        /* 0.001 of full scale: 0.001 * 8388607 rounds to 8389, or 0x0020C5. */
        EXPECT(s24 == 8389);
        ua101_tone_mix(&t, frame, 1, 2, n);
        EXPECT(frame[0] == 0x00);
        EXPECT(frame[1] == 0xC5);
        EXPECT(frame[2] == 0x20);
        EXPECT(frame[3] == 0x00);
    }
    free(buf);
}

/* The jack map: each output gets exactly the tone a single tone at its frequency would. */
static void test_jack_samples(void)
{
    enum { CHANNELS = 12, FRAMES = 48000, START = 1000 };
    uint8_t *buf = calloc(FRAMES, CHANNELS * 4);
    uint8_t *small = calloc(FRAMES, 6 * 4);
    ua101_tone tone;
    double dbfs = -60.0;
    int64_t limit = (int64_t)(pow(10.0, dbfs / 20.0) * 2147483648.0) + 256;
    int wrong = 0, lowbyte = 0, beyond = 0, loud = 0;
    unsigned ch;
    uint32_t done;
    size_t f;

    for (ch = 0; ch < UA101_JACK_OUTPUTS; ch++)
        EXPECT(ua101_jack_freq(ch) > 0.0 && ua101_jack_freq(ch) < 22050.0 / 2.0);
    EXPECT(ua101_jack_freq(UA101_JACK_OUTPUTS) == 0.0);
    /* The limits are the same as for any tone, because the tone carries them. */
    EXPECT(ua101_tone_init(&tone, 48000.0, -39.0, 0.5, 1000.0, 1000.0, START) != 0);
    EXPECT(ua101_tone_init(&tone, 48000.0, dbfs, 0.5, 1000.0, 1500.0, START) == 0);

    for (done = 0; done < FRAMES;) {
        uint32_t step = (done % 2) ? 5 : 6;

        if (step > FRAMES - done)
            step = FRAMES - done;
        ua101_jack_mix(&tone, buf + (size_t)done * CHANNELS * 4, step, CHANNELS,
                       UA101_JACK_OUTPUTS, done);
        done += step;
    }
    for (ch = 0; ch < CHANNELS; ch++) {
        ua101_tone ref;

        if (ch < UA101_JACK_OUTPUTS)
            EXPECT(ua101_tone_init(&ref, 48000.0, dbfs, 0.5, ua101_jack_freq(ch),
                                   ua101_jack_freq(ch), START) == 0);
        for (f = 0; f < FRAMES; f++) {
            int32_t s = read_word(buf, f, CHANNELS, ch);
            int32_t want = 0;

            if (ch < UA101_JACK_OUTPUTS && f >= START && f < START + ref.length)
                want = ua101_tone_word(&ref, 0, f - START);
            if (ch >= UA101_JACK_OUTPUTS && s != 0)
                beyond++;
            else if (s != want)
                wrong++;
            if (s & 0xFF)
                lowbyte++;
            if ((s < 0 ? -(int64_t)s : s) > limit)
                loud++;
        }
    }
    EXPECT(wrong == 0);
    EXPECT(beyond == 0);
    EXPECT(lowbyte == 0);
    EXPECT(loud == 0);

    /* Asked for more outputs than the stream has, it writes only the stream's channels. */
    ua101_jack_mix(&tone, small, FRAMES, 6, UA101_JACK_OUTPUTS, 0);
    EXPECT(read_word(small, START + 12000, 6, 5) != 0);
    free(small);
    free(buf);
}

static void test_ring(void)
{
    uint8_t storage[8];
    ua101_ring ring;
    const uint8_t *ptr = NULL;
    const uint8_t data[6] = {1, 2, 3, 4, 5, 6};
    size_t n;

    ua101_ring_init(&ring, storage, sizeof(storage));
    EXPECT(ua101_ring_peek(&ring, &ptr) == 0);
    EXPECT(ua101_ring_write(&ring, data, 6) == 1);

    /* All or nothing. */
    EXPECT(ua101_ring_write(&ring, data, 3) == 0);
    n = ua101_ring_peek(&ring, &ptr);
    EXPECT(n == 6 && ptr[0] == 1 && ptr[5] == 6);
    ua101_ring_consume(&ring, 4);

    /* A write that wraps comes back in two reads, in order: 5 6 1 2, then 3 4 5 6. */
    EXPECT(ua101_ring_write(&ring, data, 6) == 1);
    n = ua101_ring_peek(&ring, &ptr);
    EXPECT(n == 4 && ptr[0] == 5 && ptr[1] == 6 && ptr[2] == 1 && ptr[3] == 2);
    ua101_ring_consume(&ring, n);
    n = ua101_ring_peek(&ring, &ptr);
    EXPECT(n == 4 && ptr[0] == 3 && ptr[1] == 4 && ptr[2] == 5 && ptr[3] == 6);
    ua101_ring_consume(&ring, n);
    EXPECT(ua101_ring_peek(&ring, &ptr) == 0);
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static void test_wav_header(void)
{
    uint8_t h[UA101_WAV_HEADER_BYTES];

    ua101_wav_header(h, 12, 44100, 32, 24, 4800);
    EXPECT(memcmp(h, "RIFF", 4) == 0);
    EXPECT(get_u32(h + 4) == 60 + 4800);
    EXPECT(memcmp(h + 8, "WAVEfmt ", 8) == 0);
    EXPECT(get_u32(h + 16) == 40);
    EXPECT(get_u16(h + 20) == 0xFFFE);
    EXPECT(get_u16(h + 22) == 12);
    EXPECT(get_u32(h + 24) == 44100);
    EXPECT(get_u32(h + 28) == 44100 * 48);
    EXPECT(get_u16(h + 32) == 48);
    EXPECT(get_u16(h + 34) == 32);
    EXPECT(get_u16(h + 38) == 24);
    EXPECT(get_u16(h + 44) == 1); /* PCM subformat */
    EXPECT(memcmp(h + 60, "data", 4) == 0);
    EXPECT(get_u32(h + 64) == 4800);

    /* A size too big for the format is capped, not wrapped around to a small one. */
    ua101_wav_header(h, 12, 44100, 32, 24, 0x200000000ull);
    EXPECT(get_u32(h + 64) == (uint32_t)UA101_WAV_MAX_DATA_BYTES);
}

/* Repeatable noise, so the tests give the same answer on every computer. */
static uint32_t g_noise = 0x12345678u;

static double noise_value(void)
{
    g_noise ^= g_noise << 13;
    g_noise ^= g_noise >> 17;
    g_noise ^= g_noise << 5;
    return ((double)g_noise / 4294967296.0) * 2.0 - 1.0;
}

/* Makes a recording with noise on every channel and the tone on channels 10 and 11. */
static int32_t *make_recording(size_t frames, unsigned channels, const ua101_tone *tone,
                               size_t where, size_t lose_at)
{
    int32_t *data = calloc(frames, channels * sizeof(int32_t));
    double noise = pow(10.0, -100.0 / 20.0);
    size_t f, n = 0;
    unsigned c;

    for (f = 0; f < frames; f++) {
        for (c = 0; c < channels; c++) {
            double v = noise * noise_value();

            if (tone != NULL && f >= where && n < tone->length) {
                if (c == 10)
                    v += ua101_tone_value(tone, 0, n);
                else if (c == 11)
                    v += ua101_tone_value(tone, 1, n);
            }
            data[f * channels + c] = (int32_t)lround(v * 8388607.0) * 256;
        }
        if (tone != NULL && f >= where) {
            n++;
            /* Skipping a tone sample here is what a lost sample looks like. */
            if (lose_at != 0 && n == lose_at)
                n++;
        }
    }
    return data;
}

static void test_analysis_clean(void)
{
    enum { CHANNELS = 12, FRAMES = 44100 * 3 };
    const unsigned chans[UA101_TONE_CHANNELS] = {10, 11};
    ua101_tone tone;
    ua101_channel_stats st;
    ua101_tone_fit fit;
    int32_t *data;
    int64_t found;
    size_t where = 44100 + 1234;

    EXPECT(ua101_tone_init(&tone, 44100.0, -60.0, 1.0, 1000.0, 1500.0, 0) == 0);
    data = make_recording(FRAMES, CHANNELS, &tone, where, 0);

    ua101_channel_analyze(data, FRAMES, CHANNELS, 3, 44100.0, &st);
    EXPECT(st.low_byte_nonzero == 0);
    EXPECT(st.clicks == 0);
    EXPECT(st.peak_dbfs < -99.0 && st.peak_dbfs > -101.0);

    /* The tone is seen on the channel that carries it and not on another one. */
    EXPECT(fabs(ua101_goertzel_dbfs(data, where + 4410, 22050, CHANNELS, 10, 1000.0, 44100.0) +
                60.0) < 0.2);
    EXPECT(fabs(ua101_goertzel_dbfs(data, where + 4410, 22050, CHANNELS, 11, 1500.0, 44100.0) +
                60.0) < 0.2);
    EXPECT(ua101_goertzel_dbfs(data, where + 4410, 22050, CHANNELS, 3, 1000.0, 44100.0) < -120.0);

    found = ua101_tone_locate(data, FRAMES, CHANNELS, &tone, chans, (int64_t)where + 200, 400);
    EXPECT(found == (int64_t)where);

    ua101_tone_measure(data, FRAMES, CHANNELS, 10, &tone, 0, where, &fit);
    EXPECT(fabs(fit.level_dbfs + 60.0) < 0.1);
    EXPECT(fabs(fit.delay_fraction) < 0.01);
    EXPECT(fit.max_phase_dev_deg < 1.0);
    EXPECT(fit.residual_db < -30.0);
    EXPECT(fit.clean);
    free(data);
}

/* Correlating every sample at every start: slow, but obviously right. */
static int64_t locate_slowly(const int32_t *data, size_t frames, unsigned channels,
                             const ua101_tone *tone, const unsigned chans[UA101_TONE_CHANNELS],
                             int64_t guess, int64_t radius)
{
    int64_t start, best_start = -1;
    double best = -1.0;
    size_t n;
    unsigned c;

    for (start = guess - radius; start <= guess + radius; start++) {
        double score = 0.0;

        if (start < 0 || (size_t)start + tone->length > frames)
            continue;
        for (c = 0; c < UA101_TONE_CHANNELS; c++)
            for (n = 0; n < tone->length; n++)
                score += (double)data[((size_t)start + n) * channels + chans[c]] *
                         ua101_tone_value(tone, c, n);
        if (score > best) {
            best = score;
            best_start = start;
        }
    }
    return best_start;
}

/*
 * The search must find the same frame as correlating every sample, at every
 * rate the unit has, and for tones longer than the old fixed table allowed.
 */
static void test_tone_locate_rates(void)
{
    static const struct {
        double rate, seconds;
        int64_t radius;
        int slow;
    } cases[] = {
        {44100.0, 1.0, 300, 1}, {48000.0, 0.03, 300, 1}, {96000.0, 0.5, 300, 1},
        {192000.0, 0.25, 200, 1}, {192000.0, 5.0, 40, 1}, {192000.0, 5.0, 16384, 0},
    };
    enum { CHANNELS = 12 };
    const unsigned chans[UA101_TONE_CHANNELS] = {10, 11};
    size_t i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        ua101_tone tone;
        size_t where = (size_t)(cases[i].rate * 0.3) + 17 * (i + 1);
        size_t frames = where + (size_t)(cases[i].rate * (cases[i].seconds + 0.5));
        int32_t *data;
        int64_t fast;

        EXPECT(ua101_tone_init(&tone, cases[i].rate, -60.0, cases[i].seconds, 997.0, 1499.0, 0) == 0);
        data = make_recording(frames, CHANNELS, &tone, where, 0);
        fast = ua101_tone_locate(data, frames, CHANNELS, &tone, chans,
                                 (int64_t)where + cases[i].radius / 3, cases[i].radius);
        EXPECT(fast == (int64_t)where);
        if (cases[i].slow)
            EXPECT(fast == locate_slowly(data, frames, CHANNELS, &tone, chans,
                                         (int64_t)where + cases[i].radius / 3, cases[i].radius));
        free(data);
    }
}

static void test_analysis_finds_lost_sample(void)
{
    enum { CHANNELS = 12, FRAMES = 44100 * 3 };
    ua101_tone tone;
    ua101_tone_fit fit;
    int32_t *data;
    size_t where = 44100;

    EXPECT(ua101_tone_init(&tone, 44100.0, -60.0, 1.0, 1000.0, 1500.0, 0) == 0);
    data = make_recording(FRAMES, CHANNELS, &tone, where, 22050);
    ua101_tone_measure(data, FRAMES, CHANNELS, 10, &tone, 0, where, &fit);
    EXPECT(fit.max_phase_dev_deg > fit.slip_limit_deg);
    EXPECT(!fit.clean);
    free(data);
}

static void test_analysis_finds_click(void)
{
    enum { CHANNELS = 12, FRAMES = 44100 * 3 };
    ua101_tone tone;
    ua101_tone_fit fit;
    ua101_channel_stats st;
    int32_t *data;
    size_t where = 44100;
    size_t click = where + 22050;

    EXPECT(ua101_tone_init(&tone, 44100.0, -60.0, 1.0, 1000.0, 1500.0, 0) == 0);
    data = make_recording(FRAMES, CHANNELS, &tone, where, 0);
    data[click * CHANNELS + 10] += (int32_t)(pow(10.0, -50.0 / 20.0) * 2147483648.0) & ~0xFF;

    ua101_tone_measure(data, FRAMES, CHANNELS, 10, &tone, 0, where, &fit);
    EXPECT(!fit.clean);

    ua101_channel_analyze(data, FRAMES, CHANNELS, 10, 44100.0, &st);
    EXPECT(st.clicks > 0);
    EXPECT(st.first_click >= click && st.first_click <= click + 2);

    /* A sample with its low byte set couldn't have come from a 24-bit converter. */
    data[5 * CHANNELS + 2] |= 0x01;
    ua101_channel_analyze(data, FRAMES, CHANNELS, 2, 44100.0, &st);
    EXPECT(st.low_byte_nonzero == 1);
    free(data);
}

/* Builds a recording the way the hardware does: the mix channels are exact sums. */
static int32_t *make_mixed_recording(size_t frames, const ua101_tone *tone, size_t where)
{
    int32_t *data = calloc(frames, UA101_MIX_CHANNELS * sizeof(int32_t));
    double noise = pow(10.0, -80.0 / 20.0);
    size_t f;
    unsigned c;

    for (f = 0; f < frames; f++) {
        int32_t *frame = data + f * UA101_MIX_CHANNELS;

        for (c = 0; c < 8; c++)
            frame[c] = (int32_t)lround(noise * noise_value() * 8388607.0) * 256;
        /* Inputs 9 and 10 are the optical input, which is silent here. */
        for (c = 0; c < 10; c++)
            frame[10 + c % 2] += frame[c];
        if (tone != NULL && f >= where && f - where < tone->length) {
            frame[10] += ua101_tone_word(tone, 0, f - where);
            frame[11] += ua101_tone_word(tone, 1, f - where);
        }
    }
    return data;
}

static void test_mix_identity(void)
{
    enum { FRAMES = 44100 * 2 };
    const unsigned outputs[UA101_TONE_CHANNELS] = {0, 1};
    size_t where = 20000;
    ua101_tone tone;
    ua101_exact_check check;
    ua101_tone_fit fit;
    int32_t *data, *rec;
    int64_t found;

    /* At this level the inputs would hide the tone without the subtraction. */
    EXPECT(ua101_tone_init(&tone, 44100.0, -90.0, 0.5, 1000.0, 1500.0, 0) == 0);
    data = make_mixed_recording(FRAMES, &tone, where);
    rec = calloc(FRAMES, UA101_MIX_OUTPUTS * sizeof(int32_t));
    ua101_mix_recover(data, FRAMES, rec);

    found = ua101_tone_locate(rec, FRAMES, UA101_MIX_OUTPUTS, &tone, outputs, (int64_t)where + 50,
                              400);
    EXPECT(found == (int64_t)where);

    ua101_tone_compare(rec, FRAMES, 0, &tone, where, &check);
    EXPECT(check.compared == tone.length);
    EXPECT(check.different == 0);
    ua101_tone_compare(rec, FRAMES, 1, &tone, where, &check);
    EXPECT(check.different == 0);

    /* The wrong position is noticed, even by one frame. */
    ua101_tone_compare(rec, FRAMES, 0, &tone, where + 1, &check);
    EXPECT(check.different > tone.length / 2);

    /* Silence outside the tone, adding across the two sides of it. */
    memset(&check, 0, sizeof(check));
    ua101_silence_check(rec, 0, where, 0, &check);
    ua101_silence_check(rec, where + (size_t)tone.length, FRAMES, 0, &check);
    EXPECT(check.different == 0);
    EXPECT(check.compared == FRAMES - tone.length);

    /* And the recovered tone measures clean, despite the noise on the inputs. */
    ua101_tone_measure(rec, FRAMES, UA101_MIX_OUTPUTS, 0, &tone, 0, where, &fit);
    EXPECT(fabs(fit.level_dbfs + 90.0) < 0.1);
    EXPECT(fit.max_phase_dev_deg < fit.slip_limit_deg);

    /* One damaged sample on an input, one in the tone: each is found and placed. */
    data[100 * UA101_MIX_CHANNELS + 2] += 256;
    data[(where + 500) * UA101_MIX_CHANNELS + 11] -= 512;
    ua101_mix_recover(data, FRAMES, rec);
    memset(&check, 0, sizeof(check));
    ua101_silence_check(rec, 0, where, 0, &check);
    EXPECT(check.different == 1);
    EXPECT(check.first_different == 100);
    EXPECT(check.largest == 256);
    ua101_tone_compare(rec, FRAMES, 1, &tone, where, &check);
    EXPECT(check.different == 1);
    EXPECT(check.first_different == 500);
    EXPECT(check.largest == 512);

    /* A tone cut off by the end of the recording isn't a match. */
    ua101_mix_recover(data, FRAMES, rec);
    ua101_tone_compare(rec, 21000, 0, &tone, where, &check);
    EXPECT(check.compared == 1000);
    EXPECT(check.different == tone.length - 1000);

    free(rec);
    free(data);
}

static void test_rate_check(void)
{
    /* The margin depends only on the time measured. These match real runs. */
    EXPECT(fabs(ua101_rate_margin_ppm(600.0) - 0.4167) < 0.001);
    EXPECT(fabs(ua101_rate_margin_ppm(119.0) - 2.101) < 0.001);

    /* The real readings: 48 kHz is outside the limit, and 44.1 kHz is inside. */
    EXPECT(ua101_rate_verdict(-120.1, 0.4, 100.0) == UA101_RATE_OUTSIDE);
    EXPECT(ua101_rate_verdict(75.4, 0.4, 100.0) == UA101_RATE_INSIDE);

    /* Real 30 s readings, which are long enough to decide. */
    EXPECT(ua101_rate_verdict(-120.6, 8.6, 100.0) == UA101_RATE_OUTSIDE);
    EXPECT(ua101_rate_verdict(74.6, 8.6, 100.0) == UA101_RATE_INSIDE);

    /* No audio at all, as with the digital input switch on and no source. */
    EXPECT(ua101_rate_verdict(-1000000.0, ua101_rate_margin_ppm(29.0), 100.0) == UA101_RATE_OUTSIDE);
    EXPECT(ua101_rate_verdict(-1000000.0, ua101_rate_margin_ppm(1.0), 100.0) == UA101_RATE_OUTSIDE);

    /* The real 8 s reading that used to print PASS. It could be on either side. */
    EXPECT(ua101_rate_verdict(-124.7, 35.6, 100.0) == UA101_RATE_UNDECIDED);
    EXPECT(ua101_rate_verdict(-300.0, ua101_rate_margin_ppm(1.0), 100.0) == UA101_RATE_UNDECIDED);
    EXPECT(ua101_rate_verdict(1.0, ua101_rate_margin_ppm(1.0), 100.0) == UA101_RATE_UNDECIDED);

    /* Exactly on the limit, with no margin, is inside. */
    EXPECT(ua101_rate_verdict(100.0, 0.0, 100.0) == UA101_RATE_INSIDE);
    EXPECT(ua101_rate_verdict(100.1, 0.0, 100.0) == UA101_RATE_OUTSIDE);

    /* A measurement that couldn't be made doesn't pass. */
    EXPECT(ua101_rate_verdict(-120.0, ua101_rate_margin_ppm(0.0), 100.0) == UA101_RATE_OUTSIDE);
    EXPECT(ua101_rate_verdict(NAN, 1.0, 100.0) == UA101_RATE_OUTSIDE);
    EXPECT(ua101_rate_verdict(0.0, INFINITY, 100.0) == UA101_RATE_OUTSIDE);

    /* The host margin must cover the worst errors seen in the 10-minute clock log. */
    EXPECT(ua101_host_margin_ppm(5.0) >= 198.0);
    EXPECT(ua101_host_margin_ppm(10.0) >= 99.4);
    EXPECT(ua101_host_margin_ppm(30.0) >= 33.5);
    EXPECT(ua101_host_margin_ppm(60.0) >= 16.8);
    EXPECT(ua101_host_margin_ppm(120.0) >= 8.4);
    EXPECT(ua101_host_margin_ppm(300.0) >= 3.6);
    EXPECT(ua101_host_margin_ppm(590.0) >= 1.3);

    /* And the real 29 s reading of +40.4 ppm, against +1.8 ppm from a long run. */
    EXPECT(fabs(40.4 - 1.8) <= ua101_host_margin_ppm(29.0));
    EXPECT(!isfinite(ua101_host_margin_ppm(0.0)));
}

int main(void)
{
    test_rate_check();
    test_mix_identity();
    test_scheduling();
    test_fifo();
    test_layout();
    test_tone_limits();
    test_tone_samples();
    test_jack_samples();
    test_ring();
    test_wav_header();
    test_analysis_clean();
    test_tone_locate_rates();
    test_analysis_finds_lost_sample();
    test_analysis_finds_click();

    printf("%d of %d checks passed\n", g_tests - g_failures, g_tests);
    return g_failures == 0 ? 0 : 1;
}
