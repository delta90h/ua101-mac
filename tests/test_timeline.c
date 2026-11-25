/* Tests for placing transfers in time after a stall, against a made-up unit whose frames are
   known exactly, with the rate and drift measured in Stage 2. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../src/ua101_framering.h"
#include "../src/ua101_timeline.h"

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

#define BUS_FRAME 0.001
#define SPAN 2 /* bus frames per 2 ms transfer */
#define B0 987654321ull /* where the bus count happens to be */
#define UNIT_RATE (48000.0 * (1.0 - 122.7e-6))

static uint32_t g_seed = 20261109u;

static double uniform01(void)
{
    g_seed = g_seed * 1664525u + 1013904223u;
    return (double)g_seed / 4294967296.0;
}

/* The unit: whole frames, as many as its clock has made, which drifts as heat changed it. */
typedef struct {
    double rate;
    double drift; /* relative change per second */
    double phase; /* 0 to 1 */
} unit;

static uint64_t unit_count(const unit *u, uint64_t bus_from_start)
{
    double t = (double)bus_from_start * BUS_FRAME;

    return (uint64_t)floor(u->rate * (t + 0.5 * u->drift * t * t) + u->phase);
}

typedef struct {
    int64_t first_error; /* placed minus true, for the first transfer after the gap */
    int64_t worst_error; /* the furthest off of all of them */
    uint64_t lost;       /* frames really in the gap */
} gap_result;

/* For collecting transfers around the gap, for the offset check. */
typedef struct {
    uint64_t bus[4000];
    uint64_t frame[4000];
    size_t count, before;
} samples;

/*
 * 2 ms transfers for `seconds`, leaving out gap_length bus frames from the first transfer at or
 * after gap_at (none if gap_length is 0). With s, collects the 2 s either side of the gap.
 */
static gap_result run_stream(const unit *u, double seconds, uint64_t gap_at, uint64_t gap_length,
                             ua101_timeline *t, samples *s)
{
    gap_result r = {0, 0, 0};
    uint64_t b = 0, end = (uint64_t)(seconds / BUS_FRAME), n0 = unit_count(u, 0);
    int gap_done = 0, first_pending = 0;

    ua101_timeline_init(t, 48000.0, BUS_FRAME);
    if (s != NULL)
        s->count = s->before = 0;
    while (b < end) {
        uint64_t frames, truth, got;
        int64_t error;

        if (gap_length > 0 && !gap_done && b >= gap_at) {
            r.lost = unit_count(u, b + gap_length) - unit_count(u, b);
            b += gap_length;
            gap_done = first_pending = 1;
            if (s != NULL)
                s->before = s->count;
        }
        frames = unit_count(u, b + SPAN) - unit_count(u, b);
        truth = unit_count(u, b) - n0;
        got = ua101_timeline_place(t, B0 + b, SPAN, (uint32_t)frames);
        error = (int64_t)got - (int64_t)truth;
        if (first_pending) {
            r.first_error = error;
            first_pending = 0;
        }
        if (llabs(error) > llabs(r.worst_error))
            r.worst_error = error;
        if (s != NULL && b + 2000 >= gap_at && (!gap_done || s->count < s->before + 1000) &&
            s->count < sizeof(s->bus) / sizeof(s->bus[0])) {
            s->bus[s->count] = B0 + b;
            s->frame[s->count] = got;
            s->count++;
        }
        b += SPAN;
    }
    return r;
}

static void test_steady_stream_counts_exactly(void)
{
    unit u = {UNIT_RATE, 1e-7, 0.37};
    ua101_timeline t;
    gap_result r = run_stream(&u, 120.0, 0, 0, &t, NULL);

    EXPECT(r.worst_error == 0);
    EXPECT(t.gaps == 0 && t.overlaps == 0 && t.early_gaps == 0);
}

/* Many gaps at random clock phases and random places in the transfer grid, after `at_s`. */
static void gap_trials(const char *label, double at_s, double drift, const uint64_t *lengths,
                       int length_count, int trials, int64_t *worst, double *exact_share)
{
    int i, k, exact = 0, total = 0;

    *worst = 0;
    for (i = 0; i < length_count; i++) {
        for (k = 0; k < trials; k++) {
            unit u = {UNIT_RATE, drift, uniform01()};
            uint64_t at = (uint64_t)(at_s / BUS_FRAME) + (uint64_t)(uniform01() * 50.0);
            ua101_timeline t;
            gap_result r = run_stream(&u, at_s + 1.0, at, lengths[i], &t, NULL);

            /* Counting after the gap is exact, so the first one's error is everyone's. */
            EXPECT(r.worst_error == r.first_error);
            EXPECT(t.gaps == 1);
            if (llabs(r.first_error) > llabs(*worst))
                *worst = r.first_error;
            exact += r.first_error == 0;
            total++;
        }
    }
    *exact_share = (double)exact / (double)total;
    printf("  %s: %d gaps, %.1f%% placed exactly, the worst %lld frames off\n", label, total,
           100.0 * *exact_share, (long long)*worst);
}

static void test_gaps_once_settled(void)
{
    /* From one failed transfer to 10 s. Stalls seen in Stage 2 reached 9.6 ms. */
    static const uint64_t lengths[] = {1, 2, 3, 7, 18, 38, 100, 1000, 10000};
    int64_t worst;
    double exact;

    gap_trials("gaps 30 s in, steady clock", 30.0, 0.0, lengths, 9, 30, &worst, &exact);
    EXPECT(llabs(worst) <= 1);
    EXPECT(exact > 0.9);
    gap_trials("gaps 30 s in, drifting 0.1 ppm/s", 30.0, 1e-7, lengths, 9, 30, &worst, &exact);
    EXPECT(llabs(worst) <= 1);
    EXPECT(exact > 0.9);
}

static void test_gaps_early(void)
{
    static const uint64_t lengths[] = {2, 18, 38, 100};
    int64_t worst;
    double exact;

    gap_trials("gaps 0.1 s in", 0.1, 0.0, lengths, 4, 30, &worst, &exact);
    EXPECT(llabs(worst) <= 1);
    gap_trials("gaps 1 s in", 1.0, 0.0, lengths, 4, 30, &worst, &exact);
    EXPECT(llabs(worst) <= 1);
}

static void test_gap_after_the_first_transfer(void)
{
    unit u = {UNIT_RATE, 0.0, 0.81};
    ua101_timeline t;
    gap_result r = run_stream(&u, 2.0, SPAN, 38, &t, NULL);

    /* No line yet, so it's the nominal rate: 38 ms at -122.7 ppm is 0.2 frames off. */
    EXPECT(t.early_gaps == 1);
    EXPECT(llabs(r.first_error) <= 1);
}

static void test_never_goes_back(void)
{
    ua101_timeline t;
    uint64_t a, b, c;

    ua101_timeline_init(&t, 48000.0, BUS_FRAME);
    a = ua101_timeline_place(&t, 1000, SPAN, 96);
    b = ua101_timeline_place(&t, 1001, SPAN, 96); /* starts inside the last one */
    c = ua101_timeline_place(&t, 1003, SPAN, 96); /* follows on from that */
    EXPECT(a == 0 && b == 96 && c == 192);
    EXPECT(t.overlaps == 1 && t.gaps == 0);
}

/* The check used on the unit, against cases whose answer is known. */
static void test_gap_offset_check(void)
{
    static samples s;
    unit u = {UNIT_RATE, 1e-7, 0.52};
    ua101_timeline t;
    gap_result r = run_stream(&u, 13.0, 10000, 38, &t, &s);
    double right, closed, shifted;
    size_t i;

    EXPECT(s.before == 1000 && s.count == 2000);
    right = ua101_timeline_gap_offset(s.bus, s.frame, s.before, s.count);
    /* As the test program counted before: frames after the gap moved earlier by all it lost. */
    for (i = s.before; i < s.count; i++)
        s.frame[i] -= t.gap_frames;
    closed = ua101_timeline_gap_offset(s.bus, s.frame, s.before, s.count);
    for (i = s.before; i < s.count; i++)
        s.frame[i] += t.gap_frames + 3;
    shifted = ua101_timeline_gap_offset(s.bus, s.frame, s.before, s.count);
    printf("  the check: placed %+.3f frames, closed up %+.1f (lost %llu), placed 3 late %+.3f\n",
           right, closed, (unsigned long long)r.lost, shifted);
    EXPECT(fabs(right - (double)r.first_error) < 0.1);
    EXPECT(fabs(closed + (double)r.lost - (double)r.first_error) < 0.1);
    EXPECT(fabs(shifted - 3.0 - (double)r.first_error) < 0.1);
    EXPECT(isnan(ua101_timeline_gap_offset(s.bus, s.frame, 1, s.count)));
}

/*
 * The whole path: transfers placed by the timeline and written to the frame ring, then read back
 * by number. Each frame carries its true number plus one, so silence (0) is a missing frame.
 */
#define RING_FRAMES 65536
#define WINDOW 60000 /* the last 1.25 s, which takes in the gap */

static void test_ring_keeps_the_gap_in_place(void)
{
    static uint64_t ring_data[RING_FRAMES], buf[200], out[WINDOW];
    unit u = {UNIT_RATE, 0.0, 0.29};
    ua101_frame_ring ring;
    ua101_timeline t;
    uint64_t b = 0, n0 = unit_count(&u, 0), gap_bus = 2000, gap_length = 38, gap_first = 0;
    uint64_t lost = 0, end, read_from, real, i, silent = 0, wrong = 0, first_silent = UINT64_MAX;
    ua101_frame_ring_miss miss = {0, 0};

    EXPECT(ua101_frame_ring_init(&ring, (uint8_t *)ring_data, RING_FRAMES, 8));
    ua101_timeline_init(&t, 48000.0, BUS_FRAME);
    while (b < 3000) {
        uint64_t frames, first, f;

        if (b == gap_bus) {
            gap_first = unit_count(&u, b) - n0;
            lost = unit_count(&u, b + gap_length) - unit_count(&u, b);
            b += gap_length;
        }
        frames = unit_count(&u, b + SPAN) - unit_count(&u, b);
        for (f = 0; f < frames; f++)
            buf[f] = unit_count(&u, b) - n0 + f + 1;
        first = ua101_timeline_place(&t, B0 + b, SPAN, (uint32_t)frames);
        ua101_frame_ring_write(&ring, first, buf, frames);
        b += SPAN;
    }

    end = ua101_frame_ring_end(&ring);
    read_from = end - WINDOW;
    real = ua101_frame_ring_read(&ring, read_from, out, WINDOW, &miss);
    for (i = 0; i < WINDOW; i++) {
        if (out[i] == 0) {
            if (first_silent == UINT64_MAX)
                first_silent = read_from + i;
            silent++;
        } else if (out[i] - 1 != read_from + i) {
            wrong++;
        }
    }
    printf("  through the frame ring: %llu frames of silence from frame %llu, where %llu were "
           "lost from frame %llu; %llu frames out of place\n",
           (unsigned long long)silent, (unsigned long long)first_silent, (unsigned long long)lost,
           (unsigned long long)gap_first, (unsigned long long)wrong);
    EXPECT(wrong == 0);
    EXPECT(silent == lost && first_silent == gap_first);
    /* The writer stored the gap as silence, so the reader got every frame it asked for. */
    EXPECT(real == WINDOW);
    EXPECT(ring.lost == lost && miss.not_yet == 0 && miss.overwritten == 0);
}

/*
 * The unit's real packet pattern: 60 s of capture at 48 kHz from tests/data. Gaps are cut out of
 * it here, so the frames that were really in them are known. This checks the clock's estimate
 * alone; a real stall also has the unit's own behaviour as it starts again.
 */
#define REAL_MAX 40000

static void test_real_pattern(const char *path)
{
    static uint64_t bus[REAL_MAX], first[REAL_MAX];
    static const uint64_t lengths[] = {1, 5, 45, 195, 500}; /* transfers: 2 ms to 1 s */
    FILE *f = fopen(path, "r");
    char line[256];
    size_t n = 1, s;
    int i;

    EXPECT(f != NULL);
    if (f == NULL) {
        printf("  couldn't open %s\n", path);
        return;
    }
    bus[0] = B0;
    first[0] = 0;
    while (fgets(line, sizeof(line), f) != NULL) {
        unsigned frames, repeat, k;

        if (line[0] == '#' || sscanf(line, "%u %u", &frames, &repeat) != 2)
            continue;
        for (k = 0; k < repeat && n < REAL_MAX; k++, n++) {
            bus[n] = bus[n - 1] + SPAN;
            first[n] = first[n - 1] + frames;
        }
    }
    fclose(f);
    EXPECT(n == 30023 && first[n - 1] == 2881758);

    for (i = 0; i < 5; i++) {
        ua101_timeline t;
        uint64_t k = lengths[i];
        long trials = 0, exact = 0, worst = 0;

        ua101_timeline_init(&t, 48000.0, BUS_FRAME);
        for (s = 0; s + k + 1 < n; s++) {
            /* From 10 s in, where the stall tests on the unit held capture up. */
            if (s >= 5000 && s % 7 == 0) {
                ua101_timeline c = t;
                uint64_t got = ua101_timeline_place(&c, bus[s + k], SPAN,
                                                    (uint32_t)(first[s + k + 1] - first[s + k]));
                long error = (long)((int64_t)got - (int64_t)first[s + k]);

                trials++;
                exact += error == 0;
                if (labs(error) > labs(worst))
                    worst = error;
            }
            ua101_timeline_place(&t, bus[s], SPAN, (uint32_t)(first[s + 1] - first[s]));
        }
        if (i == 0 || i == 4)
            printf("  the unit's real pattern, gaps of %llu ms: %ld gaps, %.1f%% placed exactly, "
                   "the worst %ld frames off\n",
                   (unsigned long long)(k * SPAN), trials, 100.0 * (double)exact / (double)trials,
                   worst);
        EXPECT(labs(worst) <= 1);
        EXPECT((double)exact > 0.97 * (double)trials);
    }
}

static void report_speed(void)
{
    ua101_timeline t;
    clock_t start;
    uint64_t i, sum = 0;
    const uint64_t n = 10000000;
    double seconds;

    ua101_timeline_init(&t, 48000.0, BUS_FRAME);
    start = clock();
    for (i = 0; i < n; i++)
        sum += ua101_timeline_place(&t, B0 + SPAN * i + (i % 1000 == 999 ? 5 : 0), SPAN,
                                    i % 7 == 0 ? 95 : 96);
    seconds = (double)(clock() - start) / CLOCKS_PER_SEC;
    printf("  speed: %.0f ns a transfer (checksum %llu)\n", seconds / (double)n * 1e9,
           (unsigned long long)(sum & 0xFFFF));
}

int main(int argc, char **argv)
{
    test_steady_stream_counts_exactly();
    test_gaps_once_settled();
    test_gaps_early();
    test_gap_after_the_first_transfer();
    test_never_goes_back();
    test_gap_offset_check();
    test_ring_keeps_the_gap_in_place();
    test_real_pattern(argc > 1 ? argv[1] : "tests/data/ua101-48k-capture-transfers.txt");
    report_speed();

    printf("%d of %d checks passed\n", g_tests - g_failures, g_tests);
    return g_failures == 0 ? 0 : 1;
}
