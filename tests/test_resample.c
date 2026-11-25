/*
 * Tests for the rate correction. Runs on any computer, with no hardware needed.
 *
 * The idea behind most of these: make a tone as a clock running slow would have
 * sampled it, correct it, and compare with the same tone sampled on a perfect clock.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef UA101_RESAMPLE_TESTING
#define UA101_RESAMPLE_TESTING
#endif
#include "../src/ua101_resample.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

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

/* The unit's measured 48 kHz clock, and its measured 44.1 kHz clock. */
#define SLOW_HZ 47994.35
#define FAST_HZ 44103.32

static int32_t round_24(double value)
{
    return (int32_t)(round(value * 8388608.0) * 256.0);
}

/* Residual in dB against the ideal tone, away from the ends, where silence intrudes. */
static double tone_residual_db(double input_hz, double output_hz, double tone_hz, double level)
{
    size_t in_frames = (size_t)(input_hz * 2.0), n, count, margin = 1024;
    int32_t *in = malloc(in_frames * sizeof(int32_t));
    int32_t *out;
    ua101_resampler r;
    double signal = 0.0, error = 0.0;

    for (n = 0; n < in_frames; n++)
        in[n] = round_24(level * sin(2.0 * M_PI * tone_hz * (double)n / input_hz));
    EXPECT(ua101_resampler_init(&r, input_hz, output_hz));
    count = (size_t)ua101_resampler_output_frames(&r, in_frames);
    out = malloc(count * sizeof(int32_t));
    ua101_resampler_run(&r, in, in_frames, 1, 0, count, out);
    for (n = margin; n + margin < count; n++) {
        double ideal = level * sin(2.0 * M_PI * tone_hz * (double)n / output_hz) * 2147483648.0;

        signal += ideal * ideal;
        error += (out[n] - ideal) * (out[n] - ideal);
    }
    ua101_resampler_free(&r);
    free(out);
    free(in);
    return 10.0 * log10(error / signal);
}

static void test_tones_come_out_true(void)
{
    /* Limited by 24-bit rounding of the input and the output, so this is the real floor. */
    EXPECT(tone_residual_db(SLOW_HZ, 48000.0, 1000.0, 0.5) < -130.0);
    EXPECT(tone_residual_db(SLOW_HZ, 48000.0, 10000.0, 0.5) < -130.0);
    EXPECT(tone_residual_db(SLOW_HZ, 48000.0, 20000.0, 0.5) < -130.0);
    EXPECT(tone_residual_db(SLOW_HZ, 48000.0, 22000.0, 0.5) < -120.0);
    EXPECT(tone_residual_db(FAST_HZ, 44100.0, 1000.0, 0.5) < -130.0);
    EXPECT(tone_residual_db(FAST_HZ, 44100.0, 20000.0, 0.5) < -120.0);
}

static void test_length_matches_real_time(void)
{
    ua101_resampler r;
    uint64_t in = (uint64_t)(SLOW_HZ * 600.0);
    uint64_t out;

    EXPECT(ua101_resampler_init(&r, SLOW_HZ, 48000.0));
    out = ua101_resampler_output_frames(&r, in);
    /* Ten real minutes at 48 kHz, to within a frame. */
    EXPECT(llabs((long long)out - 28800000LL) <= 1);
    ua101_resampler_free(&r);
}

static void test_dc_and_silence(void)
{
    enum { FRAMES = 4096 };
    static int32_t in[FRAMES], out[FRAMES];
    ua101_resampler r;
    int n, bad = 0;

    EXPECT(ua101_resampler_init(&r, SLOW_HZ, 48000.0));
    ua101_resampler_run(&r, in, FRAMES, 1, 0, FRAMES - 1, out);
    for (n = 0; n < FRAMES - 1; n++)
        bad += out[n] != 0;
    EXPECT(bad == 0);

    for (n = 0; n < FRAMES; n++)
        in[n] = 1000000 * 256;
    ua101_resampler_run(&r, in, FRAMES, 1, 512, 2048, out);
    bad = 0;
    for (n = 0; n < 2048; n++)
        bad += out[n] != 1000000 * 256;
    EXPECT(bad == 0);
    ua101_resampler_free(&r);
}

static void test_full_scale_and_24_bits(void)
{
    enum { FRAMES = 8192 };
    static int32_t in[FRAMES], out[FRAMES];
    ua101_resampler r;
    int n, low_bits = 0, over = 0;

    for (n = 0; n < FRAMES; n++)
        in[n] = (n & 64) ? 2147483392 : INT32_MIN;
    EXPECT(ua101_resampler_init(&r, SLOW_HZ, 48000.0));
    ua101_resampler_run(&r, in, FRAMES, 1, 0, FRAMES - 16, out);
    for (n = 0; n < FRAMES - 16; n++) {
        low_bits += (out[n] & 0xFF) != 0;
        over += out[n] > 2147483392;
    }
    EXPECT(low_bits == 0);
    EXPECT(over == 0);
    ua101_resampler_free(&r);
}

static void test_channels_stay_apart(void)
{
    enum { FRAMES = 16384, CHANNELS = 3 };
    int32_t *in = calloc(FRAMES * CHANNELS, sizeof(int32_t));
    int32_t *out = calloc(FRAMES * CHANNELS, sizeof(int32_t));
    ua101_resampler r;
    int n, leak = 0;

    /* Tone on the middle channel only. */
    for (n = 0; n < FRAMES; n++)
        in[n * CHANNELS + 1] = round_24(0.5 * sin(2.0 * M_PI * 1000.0 * n / SLOW_HZ));
    EXPECT(ua101_resampler_init(&r, SLOW_HZ, 48000.0));
    ua101_resampler_run(&r, in, FRAMES, CHANNELS, 0, FRAMES - 16, out);
    for (n = 0; n < FRAMES - 16; n++)
        leak += out[n * CHANNELS] != 0 || out[n * CHANNELS + 2] != 0;
    EXPECT(leak == 0);
    ua101_resampler_free(&r);
    free(out);
    free(in);
}

static void test_rejects_above_the_new_nyquist(void)
{
    /* A big rate change, so that a tone falls above what the output can hold. */
    size_t in_frames = 96000, n, count, margin = 1024;
    int32_t *in = malloc(in_frames * sizeof(int32_t));
    int32_t *out;
    ua101_resampler r;
    double energy = 0.0;

    for (n = 0; n < in_frames; n++)
        in[n] = round_24(0.5 * sin(2.0 * M_PI * 23000.0 * (double)n / 48000.0));
    EXPECT(ua101_resampler_init(&r, 48000.0, 44100.0));
    count = (size_t)ua101_resampler_output_frames(&r, in_frames);
    out = malloc(count * sizeof(int32_t));
    ua101_resampler_run(&r, in, in_frames, 1, 0, count, out);
    for (n = margin; n + margin < count; n++)
        energy += (double)out[n] * (double)out[n];
    /* Only rounding noise should remain, near -146 dBFS. */
    EXPECT(10.0 * log10(energy / (double)(count - 2 * margin) / (2147483648.0 * 2147483648.0)) < -130.0);
    ua101_resampler_free(&r);
    free(out);
    free(in);
}

static void test_refuses_nonsense(void)
{
    ua101_resampler r;

    EXPECT(!ua101_resampler_init(&r, 0.0, 48000.0));
    EXPECT(!ua101_resampler_init(&r, 48000.0, -1.0));
    EXPECT(!ua101_resampler_init(&r, NAN, 48000.0));
    EXPECT(!ua101_resampler_init_live(&r, 48000.0, 0.0));
}

/* The driver's filter holds only the single-precision table, in one allocation, and the
   one-shot version, which needs the other table, leaves its output alone. */
static void test_live_only_filter(void)
{
    int32_t in[64] = {0}, out[8];
    unsigned long before = ua101_resample_allocations();
    ua101_resampler r;
    ua101_live l;
    size_t i;

    EXPECT(ua101_resampler_init_live(&r, 48000.0, 48000.0));
    EXPECT(ua101_resample_allocations() == before + 1);
    EXPECT(r.rows == NULL && r.rows_f != NULL);
    EXPECT(r.half_taps == 26);
    for (i = 0; i < 8; i++)
        out[i] = 12345;
    ua101_resampler_run(&r, in, 64, 1, 0, 8, out);
    for (i = 0; i < 8; i++)
        EXPECT(out[i] == 12345);
    EXPECT(ua101_live_init(&l, &r, 2, 1024));
    ua101_live_free(&l);
    ua101_resampler_free(&r);
    EXPECT(r.rows == NULL && r.rows_f == NULL);
    EXPECT(ua101_resampler_init(&r, 48000.0, 48000.0));
    EXPECT(r.half_taps == UA101_RESAMPLE_HALF_TAPS);
    ua101_resampler_free(&r);
}

static uint32_t g_seed = 20261108u;

static int32_t random_sample(void)
{
    g_seed = g_seed * 1664525u + 1013904223u;
    return (int32_t)(g_seed & 0xFFFFFF00u); /* 24-bit audio, full scale */
}

/*
 * The faster version must give exactly the same samples as the straightforward one: every
 * channel path (groups of 4, a pair, one left over), both ends of the input, calls spanning
 * several of its blocks, and rates in both directions.
 */
static void test_faster_matches_plain(void)
{
    static const unsigned channel_counts[] = {1, 2, 3, 4, 5, 6, 7, 10, 12, 13};
    static const double rates[][2] = {
        {SLOW_HZ, 48000.0}, {FAST_HZ, 44100.0}, {48000.0, 44100.0}, {44100.0, 48000.0}};
    enum { FRAMES = 6000 };
    size_t c, k;
    int all_same = 1;

    for (c = 0; c < sizeof(channel_counts) / sizeof(channel_counts[0]); c++) {
        unsigned channels = channel_counts[c];
        int32_t *in = malloc((size_t)FRAMES * channels * sizeof(int32_t));
        size_t out_cap = (size_t)FRAMES * 2 * channels;
        int32_t *fast = malloc(out_cap * sizeof(int32_t));
        int32_t *plain = malloc(out_cap * sizeof(int32_t));
        size_t i;

        for (i = 0; i < (size_t)FRAMES * channels; i++)
            in[i] = random_sample();
        for (k = 0; k < sizeof(rates) / sizeof(rates[0]); k++) {
            ua101_resampler r;
            uint64_t total;
            /* Whole, from the start, across the far end, and a short stretch in the middle. */
            uint64_t firsts[4], counts[4];
            size_t t;

            EXPECT(ua101_resampler_init(&r, rates[k][0], rates[k][1]));
            total = ua101_resampler_output_frames(&r, FRAMES);
            firsts[0] = 0, counts[0] = total;
            firsts[1] = 0, counts[1] = 300;
            firsts[2] = total - 700, counts[2] = 900; /* runs past the end, into silence */
            firsts[3] = 2047, counts[3] = 3;
            for (t = 0; t < 4; t++) {
                size_t bytes = (size_t)counts[t] * channels * sizeof(int32_t);

                ua101_resampler_run(&r, in, FRAMES, channels, firsts[t], counts[t], fast);
                ua101_resampler_run_plain(&r, in, FRAMES, channels, firsts[t], counts[t], plain);
                if (memcmp(fast, plain, bytes) != 0) {
                    all_same = 0;
                    printf("  differs: %u channels, rate pair %zu, part %zu\n", channels, k, t);
                }
            }
            ua101_resampler_free(&r);
        }
        free(in);
        free(fast);
        free(plain);
    }
    EXPECT(all_same);
}

static double seconds_now(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* Not a pass or fail: prints how much faster it is, for 12 channels at 48 kHz. */
static void report_speed(void)
{
    enum { FRAMES = 48000 * 2, CHANNELS = 12 };
    int32_t *in = malloc((size_t)FRAMES * CHANNELS * sizeof(int32_t));
    int32_t *out;
    ua101_resampler r;
    size_t i, count;
    double t0, plain_s, fast_s;

    for (i = 0; i < (size_t)FRAMES * CHANNELS; i++)
        in[i] = random_sample();
    EXPECT(ua101_resampler_init(&r, SLOW_HZ, 48000.0));
    /* A slow clock gives more output frames than input ones. */
    count = (size_t)ua101_resampler_output_frames(&r, FRAMES);
    out = malloc(count * CHANNELS * sizeof(int32_t));
    t0 = seconds_now();
    ua101_resampler_run_plain(&r, in, FRAMES, CHANNELS, 0, count, out);
    plain_s = seconds_now() - t0;
    t0 = seconds_now();
    ua101_resampler_run(&r, in, FRAMES, CHANNELS, 0, count, out);
    fast_s = seconds_now() - t0;
    printf("  2 s of 12 channels: plain %.3f s, faster %.3f s (%.1fx)\n", plain_s, fast_s,
           plain_s / fast_s);
    ua101_resampler_free(&r);
    free(in);
    free(out);
}

/* --- The live version --- */

#define FIXED_ONE 4294967296.0 /* 2^32: the live position's fixed point */

/* The step rounded to what the live version holds, so the one-shot version can use it too. */
static double fixed_step(double step)
{
    return (double)llround(step * FIXED_ONE) / FIXED_ONE;
}

/* Output frames the one-shot version can give from input_frames without reaching past them:
   frame k needs input up to floor(k * step) + UA101_RESAMPLE_HALF_TAPS. */
static uint64_t reachable_frames(double step, uint64_t input_frames)
{
    uint64_t s = (uint64_t)llround(step * FIXED_ONE);
    uint64_t limit = (input_frames - UA101_RESAMPLE_HALF_TAPS) << 32;

    return (limit + s - 1) / s;
}

/* Feeds input in pieces like USB transfers and takes output in pieces like the Mac's I/O
   cycles, both sizes uneven, then takes whatever is left. Returns the output frames, and 0 if
   any push was refused, which none should be at these sizes. */
static size_t live_run(ua101_live *l, const int32_t *in, size_t in_frames, unsigned channels,
                       int32_t *out, size_t out_cap)
{
    static const size_t push_sizes[] = {96, 95, 97, 96, 1, 300, 96};
    static const size_t pull_sizes[] = {512, 37, 1, 200, 96, 0, 1024};
    size_t fed = 0, made = 0, p = 0, q = 0;

    while (fed < in_frames) {
        size_t want = push_sizes[p++ % 7], took;

        if (want > in_frames - fed)
            want = in_frames - fed;
        took = ua101_live_push(l, in + fed * channels, want);
        if (took != want)
            return 0;
        fed += took;
        for (;;) {
            size_t ask = pull_sizes[q++ % 7], got;

            if (ask > out_cap - made)
                ask = out_cap - made;
            got = ua101_live_pull(l, out + made * channels, ask);
            made += got;
            if (got < ask || ask == 0)
                break;
        }
    }
    made += ua101_live_pull(l, out + made * channels, out_cap - made);
    return made;
}

/*
 * The live version works in single precision, so it can't match the one-shot version bit for
 * bit. It must stay within LIVE_ERROR of it on every sample, frame for frame, however the input
 * and output are cut up: every channel path, slow and fast clocks, and a big ratio. The input is
 * full-scale noise, the worst case for rounding. LIVE_ERROR (any one sample) and LIVE_NOISE
 * (rms) are in dB below full scale and sit far under the unit's own noise: idle inputs peak at
 * about -87 dBFS. Measured 2026-11-09: worst -119.4, rms -138.8. It must also give every frame
 * the input reaches, and nothing more.
 */
#define LIVE_ERROR (-115.0)
#define LIVE_NOISE (-133.0)

static void test_live_matches_one_shot(void)
{
    static const unsigned channel_counts[] = {1, 2, 3, 6, 10, 12, 13};
    static const double rates[][2] = {
        {SLOW_HZ, 48000.0}, {FAST_HZ, 44100.0}, {48000.0, 44100.0}, {44100.0, 48000.0}};
    enum { FRAMES = 6000 };
    size_t c, k;
    int all_same = 1, all_counted = 1;
    double worst = 0.0, squares = 0.0, values = 0.0;

    for (c = 0; c < sizeof(channel_counts) / sizeof(channel_counts[0]); c++) {
        unsigned channels = channel_counts[c];
        size_t out_cap = (size_t)FRAMES * 2;
        int32_t *in = malloc((size_t)FRAMES * channels * sizeof(int32_t));
        int32_t *live = malloc(out_cap * channels * sizeof(int32_t));
        int32_t *shot = malloc(out_cap * channels * sizeof(int32_t));
        size_t i;

        for (i = 0; i < (size_t)FRAMES * channels; i++)
            in[i] = random_sample();
        for (k = 0; k < sizeof(rates) / sizeof(rates[0]); k++) {
            ua101_resampler r;
            ua101_live l;
            size_t made;

            EXPECT(ua101_resampler_init(&r, rates[k][0], rates[k][1]));
            r.step = fixed_step(r.step);
            EXPECT(ua101_live_init(&l, &r, channels, 2048));
            made = live_run(&l, in, FRAMES, channels, live, out_cap);
            ua101_resampler_run(&r, in, FRAMES, channels, 0, made, shot);
            {
                double here = 0.0;

                for (i = 0; i < made * channels; i++) {
                    double e = fabs((double)live[i] - (double)shot[i]);

                    squares += e * e;
                    if (e > here)
                        here = e;
                }
                values += (double)(made * channels);
                if (here > worst)
                    worst = here;
                if (here > 0.0 && 20.0 * log10(here / 2147483648.0) >= LIVE_ERROR) {
                    all_same = 0;
                    printf("  live too far off: %u channels, rate pair %zu, %.1f dBFS\n", channels,
                           k, 20.0 * log10(here / 2147483648.0));
                }
            }
            if (made != reachable_frames(r.step, FRAMES) || l.pulled != made || l.pushed != FRAMES) {
                all_counted = 0;
                printf("  live gave %zu frames, expected %llu\n", made,
                       (unsigned long long)reachable_frames(r.step, FRAMES));
            }
            ua101_live_free(&l);
            ua101_resampler_free(&r);
        }
        free(in);
        free(live);
        free(shot);
    }
    /* One 24-bit step is 256 here, at -138.5 dBFS. */
    printf("  live vs one-shot, full-scale noise: worst %.0f steps (%.1f dBFS), rms %.1f dBFS\n",
           worst / 256.0, 20.0 * log10((worst > 0.0 ? worst : 1.0) / 2147483648.0),
           10.0 * log10((squares > 0.0 ? squares / values : 1.0) / (2147483648.0 * 2147483648.0)));
    EXPECT(10.0 * log10((squares > 0.0 ? squares / values : 1.0) / (2147483648.0 * 2147483648.0)) <
           LIVE_NOISE);
    EXPECT(all_same);
    EXPECT(all_counted);
}

/* None of the live calls may allocate, since a driver calls them on a real-time thread. */
static void test_live_never_allocates(void)
{
    enum { CHANNELS = 12, CHUNK = 96 };
    int32_t in[CHUNK * CHANNELS], out[1024 * CHANNELS];
    unsigned long before;
    ua101_resampler r;
    ua101_live l;
    size_t i;
    int round;

    for (i = 0; i < CHUNK * CHANNELS; i++)
        in[i] = random_sample();
    EXPECT(ua101_resampler_init(&r, SLOW_HZ, 48000.0));
    /* The counter itself works: the one-shot version allocates. */
    before = ua101_resample_allocations();
    ua101_resampler_run(&r, in, CHUNK, CHANNELS, 0, 10, out);
    EXPECT(ua101_resample_allocations() == before + 1);

    EXPECT(ua101_live_init(&l, &r, CHANNELS, 2048));
    before = ua101_resample_allocations();
    for (round = 0; round < 500; round++) {
        ua101_live_set_step(&l, round % 2 ? 1.0001 : 0.9999);
        if (round % 50 == 49)
            ua101_live_push_silence(&l, CHUNK);
        else
            ua101_live_push(&l, in, CHUNK);
        ua101_live_pull(&l, out, 1024);
    }
    EXPECT(ua101_resample_allocations() == before);
    ua101_live_free(&l);
    ua101_resampler_free(&r);
}

/*
 * The step changes several times while a tone plays, as a clock loop would change it, and the
 * output must still be the tone at exactly the input moments the steps add up to. A position
 * that jumped or slipped at a change would show here as a large error.
 */
static void test_live_step_changes_stay_in_place(void)
{
    static const double steps[] = {1.0, 1.0002, 0.9998, 1.000123, 0.99995};
    enum { IN_FRAMES = 48000, PIECE = 96 };
    const double tone_hz = 997.0, level = 0.5;
    int32_t *in = malloc(IN_FRAMES * sizeof(int32_t));
    int32_t *out = malloc(2 * IN_FRAMES * sizeof(int32_t));
    double *moment = malloc(2 * IN_FRAMES * sizeof(double));
    uint64_t at = 0; /* the next output frame's input moment, in 2^-32 frames */
    size_t fed = 0, made = 0, n;
    double signal = 0.0, error = 0.0, worst = 0.0;
    ua101_resampler r;
    ua101_live l;
    int changes = 0;

    for (n = 0; n < IN_FRAMES; n++)
        in[n] = round_24(level * sin(2.0 * M_PI * tone_hz * (double)n / 48000.0));
    EXPECT(ua101_resampler_init(&r, 48000.0, 48000.0));
    EXPECT(ua101_live_init(&l, &r, 1, 1024));
    while (fed < IN_FRAMES) {
        size_t got, i;
        uint64_t s;

        if (fed % 9600 == 0) /* a new step every 0.2 s */
            EXPECT(ua101_live_set_step(&l, steps[changes++ % 5]));
        ua101_live_push(&l, in + fed, PIECE);
        fed += PIECE;
        got = ua101_live_pull(&l, out + made, 2 * IN_FRAMES - made);
        s = (uint64_t)llround(ua101_live_get_step(&l) * FIXED_ONE);
        for (i = 0; i < got; i++, at += s)
            moment[made + i] = (double)at / FIXED_ONE;
        made += got;
    }
    /* Away from the silence before the input starts. */
    for (n = 1024; n < made; n++) {
        double ideal = level * sin(2.0 * M_PI * tone_hz * moment[n] / 48000.0) * 2147483648.0;
        double e = fabs(out[n] - ideal);

        signal += ideal * ideal;
        error += e * e;
        if (e > worst)
            worst = e;
    }
    printf("  live, 5 step changes: tone error %.1f dB, worst sample %.1f dB\n",
           10.0 * log10(error / signal), 20.0 * log10(worst / (level * 2147483648.0)));
    EXPECT(10.0 * log10(error / signal) < -130.0);
    EXPECT(20.0 * log10(worst / (level * 2147483648.0)) < -120.0);
    ua101_live_free(&l);
    ua101_resampler_free(&r);
    free(in);
    free(out);
    free(moment);
}

/*
 * One full-scale tone through the live version at a fixed step: the worst sample against the
 * exact sine at each output frame's own moment, in dB below full scale; the tone's gain in dB;
 * and the rms of what is left once the best-fitting tone is taken out, in dB below full scale.
 */
static void live_tone(const ua101_resampler *r, double rate, double tone_hz, double step,
                      double *worst_db, double *gain_db, double *rest_db)
{
    enum { FRAMES = 24000, PIECE = 96, SKIP = 1024 };
    static int32_t in[FRAMES + 2 * PIECE], out[FRAMES];
    static double moment[FRAMES];
    const double level = 0.99;
    double worst = 0, ss = 0, cc = 0, sc = 0, sy = 0, cy = 0, a, b, det, rest = 0;
    uint64_t fixed, at = 0;
    size_t made = 0, fed = 0, n;
    ua101_live l;

    for (n = 0; n < FRAMES + 2 * PIECE; n++)
        in[n] = round_24(level * sin(2.0 * M_PI * tone_hz * (double)n / rate));
    EXPECT(ua101_live_init(&l, r, 1, 1024));
    EXPECT(ua101_live_set_step(&l, step));
    fixed = (uint64_t)llround(ua101_live_get_step(&l) * FIXED_ONE);
    while (made < FRAMES && fed + PIECE <= FRAMES + 2 * PIECE) {
        size_t got, i;

        ua101_live_push(&l, in + fed, PIECE);
        fed += PIECE;
        got = ua101_live_pull(&l, out + made, FRAMES - made);
        for (i = 0; i < got; i++, at += fixed)
            moment[made + i] = (double)at / FIXED_ONE;
        made += got;
    }
    EXPECT(made > 2 * SKIP);
    for (n = SKIP; n < made; n++) {
        double phase = 2.0 * M_PI * tone_hz * moment[n] / rate, si = sin(phase), co = cos(phase);
        double y = (double)out[n] / 2147483648.0, e = fabs(y - level * si);

        if (e > worst)
            worst = e;
        ss += si * si;
        cc += co * co;
        sc += si * co;
        sy += si * y;
        cy += co * y;
    }
    det = ss * cc - sc * sc;
    a = (sy * cc - cy * sc) / det;
    b = (cy * ss - sy * sc) / det;
    for (n = SKIP; n < made; n++) {
        double phase = 2.0 * M_PI * tone_hz * moment[n] / rate;
        double e = (double)out[n] / 2147483648.0 - a * sin(phase) - b * cos(phase);

        rest += e * e;
    }
    *worst_db = 20.0 * log10(worst + 1e-30);
    *gain_db = 20.0 * log10(sqrt(a * a + b * b) / level);
    *rest_db = 10.0 * log10(rest / (double)(made - SKIP) + 1e-30);
    ua101_live_free(&l);
}

/*
 * The live design, the short filter a driver uses (measured 2026-11-10). Its length at each of
 * the unit's rates. Inside the band it keeps (20 kHz, or 40 kHz from 88.2 kHz up), with the
 * step 120 ppm either side of one frame per frame as on the unit: full-scale tones come out
 * true to LIVE_TONE_ERROR on every sample (measured -119.9 to -120.2 dBFS, which is single
 * precision's floor) and flat to LIVE_FLATNESS (measured 0.00001 dB). Above the band it rolls
 * off and isn't clean, which is the price of the short length: at 48 kHz a 22 kHz tone loses
 * 0.26 dB and leaves an image at -34 dBFS, and 23 kHz loses 1.8 dB.
 */
#define LIVE_TONE_ERROR (-116.0)
#define LIVE_FLATNESS 0.001

static void test_live_design(void)
{
    static const struct {
        double rate;
        unsigned half_taps;
        int tones;
    } designs[] = {{44100.0, 46, 40}, {48000.0, 26, 40}, {88200.0, 46, 12},
                   {96000.0, 26, 12}, {192000.0, 8, 12}};
    double worst, gain, rest;
    size_t k;
    int i, sign;

    for (k = 0; k < sizeof(designs) / sizeof(designs[0]); k++) {
        double rate = designs[k].rate, top = rate < 80000.0 ? 20000.0 : 40000.0;
        double worst_all = -999.0, low = 0.0, high = 0.0;
        ua101_resampler r;

        EXPECT(ua101_resampler_live_half_taps(rate, rate) == designs[k].half_taps);
        EXPECT(ua101_resampler_init_live(&r, rate, rate));
        EXPECT(r.half_taps == designs[k].half_taps);
        {
            /* Room for more than the filter's own length, as with the one-shot design. */
            ua101_live small;

            EXPECT(!ua101_live_init(&small, &r, 2, 2 * r.half_taps));
            EXPECT(ua101_live_init(&small, &r, 2, 2 * r.half_taps + 1));
            ua101_live_free(&small);
        }
        for (sign = -1; sign <= 1; sign += 2) {
            /* From 20 Hz to the top of the band, evenly by ratio, then the top octave evenly
               by frequency, where the filter works hardest. */
            for (i = 0; i <= 2 * designs[k].tones; i++) {
                double hz = i <= designs[k].tones
                                ? 20.0 * pow(top / 20.0, (double)i / designs[k].tones)
                                : top / 2.0 + top / 2.0 * (double)(i - designs[k].tones) /
                                                  designs[k].tones;

                live_tone(&r, rate, hz, 1.0 + sign * 120e-6, &worst, &gain, &rest);
                if (worst > worst_all)
                    worst_all = worst;
                if (gain < low)
                    low = gain;
                if (gain > high)
                    high = gain;
            }
        }
        printf("  live design at %.0f Hz: %u taps, %.3f ms; tones to %.0f kHz worst sample "
               "%.1f dBFS, gain %+.5f to %+.5f dB\n",
               rate, 2 * r.half_taps, r.half_taps / rate * 1e3, top / 1e3, worst_all, low, high);
        EXPECT(worst_all < LIVE_TONE_ERROR);
        EXPECT(low > -LIVE_FLATNESS && high < LIVE_FLATNESS);
        if (rate == 48000.0) {
            /* Above the band: where the unit's stated range at 48 kHz ends, and beyond. */
            EXPECT(r.half_taps / rate <= 0.0006); /* the delay it adds, in seconds */
            live_tone(&r, rate, 21000.0, 1.0 - 120e-6, &worst, &gain, &rest);
            printf("    21 kHz: %+.2f dB, what's left %.1f dBFS rms\n", gain, rest);
            EXPECT(gain > -0.05 && rest < -55.0);
            live_tone(&r, rate, 22000.0, 1.0 - 120e-6, &worst, &gain, &rest);
            printf("    22 kHz: %+.2f dB, what's left %.1f dBFS rms\n", gain, rest);
            EXPECT(gain > -0.4 && gain < -0.1 && rest < -30.0);
            live_tone(&r, rate, 23000.0, 1.0 - 120e-6, &worst, &gain, &rest);
            printf("    23 kHz: %+.2f dB, what's left %.1f dBFS rms\n", gain, rest);
            EXPECT(gain > -2.2 && gain < -1.4 && rest < -15.0);
        }
        ua101_resampler_free(&r);
    }
    /* Rates too low for the short design's slope get the one-shot design's length. */
    EXPECT(ua101_resampler_live_half_taps(32000.0, 32000.0) == UA101_RESAMPLE_HALF_TAPS);
    EXPECT(ua101_resampler_live_half_taps(0.0, 48000.0) == 0);
}

/* Silence for lost frames is the same as zeros pushed, it stops when full, and nonsense is
   refused without changing anything. */
static void test_live_silence_room_and_refusals(void)
{
    enum { CHANNELS = 2, FRAMES = 2000 };
    int32_t *in = calloc((size_t)FRAMES * CHANNELS, sizeof(int32_t));
    int32_t *a = malloc((size_t)FRAMES * 2 * CHANNELS * sizeof(int32_t));
    int32_t *b = malloc((size_t)FRAMES * 2 * CHANNELS * sizeof(int32_t));
    ua101_resampler r;
    ua101_live x, y;
    size_t i, made_x, made_y;

    for (i = 0; i < 500 * CHANNELS; i++)
        in[i] = random_sample(); /* then 1000 frames of zeros, then more sound */
    for (i = 1500 * CHANNELS; i < (size_t)FRAMES * CHANNELS; i++)
        in[i] = random_sample();
    EXPECT(ua101_resampler_init(&r, SLOW_HZ, 48000.0));
    EXPECT(ua101_live_init(&x, &r, CHANNELS, 4096));
    EXPECT(ua101_live_init(&y, &r, CHANNELS, 4096));
    ua101_live_push(&x, in, FRAMES);
    ua101_live_push(&y, in, 500);
    EXPECT(ua101_live_push_silence(&y, 1000) == 1000);
    ua101_live_push(&y, in + 1500 * CHANNELS, 500);
    made_x = ua101_live_pull(&x, a, FRAMES * 2);
    made_y = ua101_live_pull(&y, b, FRAMES * 2);
    EXPECT(made_x == made_y && made_x > 0);
    EXPECT(memcmp(a, b, made_x * CHANNELS * sizeof(int32_t)) == 0);
    EXPECT(x.pushed == FRAMES && y.pushed == FRAMES);

    /* Full: it takes what fits and says so. */
    EXPECT(ua101_live_room(&x) == 4096 - x.filled);
    EXPECT(ua101_live_push(&x, in, FRAMES) == FRAMES);
    i = ua101_live_room(&x);
    EXPECT(i < FRAMES);
    EXPECT(ua101_live_push(&x, in, FRAMES) == i);
    EXPECT(ua101_live_room(&x) == 0);
    EXPECT(ua101_live_push(&x, in, 10) == 0);
    EXPECT(ua101_live_push_silence(&x, 10) == 0);

    /* Steps out of range are refused, and the old one stays. */
    EXPECT(ua101_live_set_step(&y, 1.0001));
    EXPECT(!ua101_live_set_step(&y, 0.4));
    EXPECT(!ua101_live_set_step(&y, 2.5));
    EXPECT(!ua101_live_set_step(&y, NAN));
    EXPECT(fabs(ua101_live_get_step(&y) - 1.0001) < 1e-9);

    /* Bad set-ups are refused. */
    ua101_live_free(&y);
    EXPECT(!ua101_live_init(&y, &r, 0, 4096));
    EXPECT(!ua101_live_init(&y, &r, 65, 4096));
    EXPECT(!ua101_live_init(&y, &r, 2, 2 * UA101_RESAMPLE_HALF_TAPS));
    EXPECT(ua101_live_available(&y) == 0 && ua101_live_room(&y) == 0);

    ua101_live_free(&x);
    ua101_resampler_free(&r);
    free(in);
    free(a);
    free(b);
}

/* Not a pass or fail: how many times faster than real time the live version runs, for 12
   channels at 48 kHz in 2 ms pieces, as a driver would use it. */
/*
 * What a driver's clock loop leans on: ua101_live_needed says exactly how much input the next
 * pull takes (one frame fewer isn't enough), ua101_live_next_position follows the 32.32 steps
 * exactly, and ua101_live_reset starts again as a fresh one would, without allocating.
 */
static void test_live_needed_position_reset(int live_design)
{
    enum { CHANNELS = 3, ROUNDS = 400 };
    static int32_t in[600 * CHANNELS], out[600 * CHANNELS], again[600 * CHANNELS];
    static int32_t first_in[4000 * CHANNELS], first_out[1000 * CHANNELS];
    ua101_resampler r;
    ua101_live l, fresh;
    uint64_t expect = 0; /* the position, in 32.32 */
    unsigned long allocations;
    int bad_needed = 0, bad_position = 0, k;
    size_t i, made, fed;

    if (live_design) {
        EXPECT(ua101_resampler_init_live(&r, SLOW_HZ, 48000.0));
        EXPECT(r.half_taps == 26);
    } else {
        EXPECT(ua101_resampler_init(&r, SLOW_HZ, 48000.0));
        EXPECT(r.half_taps == UA101_RESAMPLE_HALF_TAPS);
    }
    EXPECT(ua101_live_init(&l, &r, CHANNELS, 1024));
    EXPECT(ua101_live_next_position(&l) == 0.0);
    EXPECT(ua101_live_needed(&l, 1) == r.half_taps + 1);
    EXPECT(ua101_live_needed(&l, 0) == 0);
    allocations = ua101_resample_allocations();
    for (k = 0; k < ROUNDS; k++) {
        double step = 1.0 + ((double)(random_sample() >> 8) / 8388608.0) * 0.01;
        size_t n = 1 + (size_t)((uint32_t)random_sample() >> 8) % 300, need;

        EXPECT(ua101_live_set_step(&l, step));
        need = ua101_live_needed(&l, n);
        for (i = 0; i < (size_t)need * CHANNELS; i++)
            in[i] = random_sample();
        if (need > 0) {
            ua101_live_push(&l, in, need - 1);
            bad_needed += ua101_live_available(&l) >= n;
            ua101_live_push(&l, in + (need - 1) * CHANNELS, 1);
        }
        bad_needed += ua101_live_available(&l) < n || ua101_live_needed(&l, n) != 0;
        made = ua101_live_pull(&l, out, n);
        bad_needed += made != n;
        expect += (uint64_t)n * (uint64_t)llround(step * FIXED_ONE);
        bad_position += ua101_live_next_position(&l) != (double)expect / FIXED_ONE;
    }
    EXPECT(bad_needed == 0);
    EXPECT(bad_position == 0);
    if (bad_needed != 0 || bad_position != 0)
        printf("  needed wrong %d times, position %d times\n", bad_needed, bad_position);

    /* Rebase: the count starts again at the newest input frame, by exactly the frames it
       says, and only once enough have been counted. Nothing else changes. */
    {
        double before = ua101_live_next_position(&l);
        uint64_t pushed = l.pushed, position = l.position, moved;

        EXPECT(ua101_live_rebase(&l, pushed + 1) == 0 &&
               ua101_live_next_position(&l) == before);
        moved = ua101_live_rebase(&l, pushed);
        EXPECT(moved == pushed && l.pushed == pushed && l.position == position);
        EXPECT(ua101_live_next_position(&l) == before - (double)moved);
        EXPECT(ua101_live_rebase(&l, 0) == 0); /* nothing new to move by */
    }

    /* Reset: the same output as a fresh one on the same input, and no allocations. */
    ua101_live_reset(&l);
    EXPECT(ua101_resample_allocations() == allocations);
    EXPECT(ua101_live_next_position(&l) == 0.0 && l.pushed == 0 && l.pulled == 0);
    EXPECT(ua101_live_get_step(&l) == fixed_step(r.step));
    EXPECT(ua101_live_needed(&l, 1) == r.half_taps + 1);
    EXPECT(ua101_live_init(&fresh, &r, CHANNELS, 1024));
    for (i = 0; i < (size_t)4000 * CHANNELS; i++)
        first_in[i] = random_sample();
    for (fed = 0, made = 0; fed < 4000; fed += 250) {
        ua101_live_push(&l, first_in + fed * CHANNELS, 250);
        ua101_live_push(&fresh, first_in + fed * CHANNELS, 250);
        i = ua101_live_pull(&l, first_out + made * CHANNELS, 1000 - made);
        EXPECT(ua101_live_pull(&fresh, again, 1000 - made) == i);
        EXPECT(memcmp(first_out + made * CHANNELS, again, i * CHANNELS * sizeof(int32_t)) == 0);
        made += i;
        if (made == 1000)
            break;
    }
    EXPECT(made == 1000);
    ua101_live_free(&fresh);
    ua101_live_free(&l);
    ua101_resampler_free(&r);
}

/*
 * What the live version cuts is counted. A wave can pass over full scale between samples that
 * are all within it: the plainest case is a tone at a quarter of the rate whose samples fall
 * 45 degrees either side of each peak, so every sample is at full scale and the wave between
 * them is 3.01 dB over. Resampled, the output lands on those peaks and is cut to 24 bits. The
 * count is checked against the same tone at half the size, where nothing is cut and each
 * output is half of what the full one would have been.
 */
static void test_live_counts_what_it_cuts(void)
{
    enum { FRAMES = 48000 };
    static int32_t in[FRAMES], half[FRAMES], out[FRAMES + 64], out_half[FRAMES + 64];
    const double step = fixed_step(1.0 / (1.0 - 115e-6)); /* the unit 115 ppm slow */
    ua101_resampler r;
    ua101_live l, h;
    size_t n, made, made_half, want = 0, differ = 0, rails = 0;
    double want_peak = 0.0;

    for (n = 0; n < FRAMES; n++) {
        in[n] = (n & 2) ? INT32_MIN + 256 : INT32_MAX - 255; /* +, +, -, -: full scale */
        half[n] = in[n] / 2 / 256 * 256;
    }
    EXPECT(ua101_resampler_init_live(&r, 48000.0, 48000.0));
    EXPECT(ua101_live_init(&l, &r, 1, 4096) && ua101_live_init(&h, &r, 1, 4096));
    EXPECT(l.cut == 0 && l.cut_peak == 0.0f);
    EXPECT(ua101_live_set_step(&l, step) && ua101_live_set_step(&h, step));
    made = live_run(&l, in, FRAMES, 1, out, FRAMES + 64);
    made_half = live_run(&h, half, FRAMES, 1, out_half, FRAMES + 64);
    EXPECT(made == made_half && made > FRAMES - 200);
    /* What should have been cut, from the half-size run: twice its value is beyond 24 bits.
       Its rounding leaves 2 units of doubt, so samples that close to the edge aren't asked
       about. */
    for (n = 0; n < made; n++) {
        double full = 2.0 * (double)(out_half[n] >> 8);
        int over = full > 8388607.0 + 2.0 || full < -8388608.0 - 2.0;
        int near = fabs(fabs(full) - 8388607.5) <= 2.5;
        int at_rail = out[n] == INT32_MAX - 255 || out[n] == INT32_MIN;

        want += over;
        rails += at_rail;
        differ += !near && over != at_rail;
        if (fabs(full) > want_peak)
            want_peak = fabs(full);
    }
    printf("  a full-scale 12 kHz tone with its peaks between the samples: %llu of %zu output "
           "samples cut, the largest %+.2f dB over (the half-size run says %zu, %+.2f dB)\n",
           (unsigned long long)l.cut, made, 20.0 * log10((double)l.cut_peak / 8388608.0), want,
           20.0 * log10(want_peak / 8388608.0));
    EXPECT(h.cut == 0 && h.cut_peak == 0.0f); /* half size: nothing cut */
    EXPECT(differ == 0);                      /* every output over is at the edge, no other is */
    /* Half of them: a sine is beyond 0.707 of its peak for half of each cycle. */
    EXPECT(l.cut > made / 2 - made / 100 && l.cut < made / 2 + made / 100 && l.cut <= rails);
    EXPECT(l.cut + 8 >= want && l.cut <= want + made / 100); /* the count is that count */
    EXPECT(fabs((double)l.cut_peak - want_peak) <= 4.0);
    EXPECT(fabs(20.0 * log10((double)l.cut_peak / 8388608.0) - 3.01) < 0.01);

    /* Starting again in place keeps the counts; only ua101_live_init clears them. */
    {
        uint64_t cut = l.cut;
        float peak = l.cut_peak;

        ua101_live_reset(&l);
        EXPECT(l.cut == cut && l.cut_peak == peak && cut != 0);
        made = live_run(&l, half, FRAMES, 1, out, FRAMES + 64);
        EXPECT(made > FRAMES - 200 && l.cut == cut && l.cut_peak == peak);
    }
    ua101_live_free(&l);
    ua101_live_free(&h);
    EXPECT(ua101_live_init(&l, &r, 1, 4096) && l.cut == 0 && l.cut_peak == 0.0f);

    /* Samples at the very edge that come out as they went in aren't cuts. At the filter's own
       step of exactly 1 each output frame is an input frame, so full-scale samples of both
       signs pass unchanged and nothing is counted. */
    for (n = 0; n < FRAMES; n++)
        in[n] = (n % 3 == 0) ? INT32_MAX - 255 : (n % 3 == 1) ? INT32_MIN : 0;
    made = live_run(&l, in, FRAMES, 1, out, FRAMES + 64);
    EXPECT(made > FRAMES - 200);
    for (n = 0, differ = 0; n < made; n++)
        differ += out[n] != in[n];
    EXPECT(differ == 0);
    EXPECT(l.cut == 0 && l.cut_peak == 0.0f);
    ua101_live_reset(&l);

    /* A full-scale tone whose peaks fall on the samples: at most the filter's ripple, a few
       units over, so "how far" says about 0 dB. */
    for (n = 0; n < FRAMES; n++)
        in[n] = round_24((1.0 - 1.0 / 8388608.0) * sin(2.0 * M_PI * 997.0 * (double)n / 48000.0));
    EXPECT(ua101_live_set_step(&l, step));
    made = live_run(&l, in, FRAMES, 1, out, FRAMES + 64);
    EXPECT(made > FRAMES - 200);
    EXPECT(l.cut < 200 && (l.cut == 0 || (double)l.cut_peak < 8388608.0 + 16.0));
    ua101_live_free(&l);
    ua101_resampler_free(&r);
}

static void report_live_speed(void)
{
    enum { FRAMES = 48000 * 2, CHANNELS = 12 };
    int32_t *in = malloc((size_t)FRAMES * CHANNELS * sizeof(int32_t));
    int32_t *out = malloc((size_t)FRAMES * 2 * CHANNELS * sizeof(int32_t));
    ua101_resampler r;
    ua101_live l;
    size_t i, made;
    double t0, took;

    for (i = 0; i < (size_t)FRAMES * CHANNELS; i++)
        in[i] = random_sample();
    EXPECT(ua101_resampler_init(&r, SLOW_HZ, 48000.0));
    EXPECT(ua101_live_init(&l, &r, CHANNELS, 4096));
    t0 = seconds_now();
    made = live_run(&l, in, FRAMES, CHANNELS, out, (size_t)FRAMES * 2);
    took = seconds_now() - t0;
    printf("  live, 2 s of 12 channels in 2 ms pieces: %.3f s, %.0fx real time\n", took,
           2.0 / took);
    EXPECT(made > FRAMES - 200);
    ua101_live_free(&l);
    ua101_resampler_free(&r);
    free(in);
    free(out);
}

int main(void)
{
    test_faster_matches_plain();
    report_speed();
    test_live_matches_one_shot();
    test_live_never_allocates();
    test_live_step_changes_stay_in_place();
    test_live_silence_room_and_refusals();
    test_live_needed_position_reset(0);
    test_live_needed_position_reset(1);
    test_live_design();
    test_live_counts_what_it_cuts();
    report_live_speed();
    test_tones_come_out_true();
    test_length_matches_real_time();
    test_dc_and_silence();
    test_full_scale_and_24_bits();
    test_channels_stay_apart();
    test_rejects_above_the_new_nyquist();
    test_refuses_nonsense();
    test_live_only_filter();

    printf("%d of %d checks passed\n", g_tests - g_failures, g_tests);
    return g_failures == 0 ? 0 : 1;
}
