/*
 * Tests for the shared block: its layout, the checks the plug-in makes before using one, the
 * clock's sequence lock under two threads, the zero timestamps, and the rings working through
 * two mappings of the same memory at different addresses, as in two processes.
 */
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "../src/ua101_shared.h"

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

#define PAGE 16384
#define RING_FRAMES 65536
#define CAP_CHANNELS 12
#define PLAY_CHANNELS 10

static void test_layout(void)
{
    size_t bytes = ua101_shared_bytes(CAP_CHANNELS * 4, PLAY_CHANNELS * 4, RING_FRAMES, PAGE);
    ua101_shared *s;

    printf("Layout\n");
    EXPECT(bytes == PAGE + RING_FRAMES * 48 + RING_FRAMES * 40);
    EXPECT(ua101_shared_bytes(48, 40, 65535, PAGE) == 0);   /* not a power of two */
    EXPECT(ua101_shared_bytes(48, 40, RING_FRAMES, 0) == 0);
    EXPECT(ua101_shared_bytes(0, 40, RING_FRAMES, PAGE) == 0);

    if (posix_memalign((void **)&s, PAGE, bytes) != 0)
        return;
    memset(s, 0, bytes);
    EXPECT(ua101_shared_init(s, bytes, 48000.0, CAP_CHANNELS, PLAY_CHANNELS, RING_FRAMES, PAGE));
    EXPECT(ua101_shared_check(s, bytes));
    EXPECT(!ua101_shared_check(s, bytes - 1)); /* the mapping is smaller than the block */
    EXPECT(s->capture.frame_bytes == 48 && s->playback.frame_bytes == 40);
    EXPECT(atomic_load(&s->state) == UA101_STATE_STOPPED);
    EXPECT(atomic_load(&s->stats.playback_lead_min) == INT64_MAX);
    /* Frames are page-aligned and inside the block. */
    EXPECT(((uintptr_t)&s->capture + (uintptr_t)s->capture.data_offset) % PAGE == 0);
    EXPECT((intptr_t)&s->playback + s->playback.data_offset - (intptr_t)s ==
           PAGE + RING_FRAMES * 48);

    /* What a plug-in must refuse. */
    s->magic ^= 1;
    EXPECT(!ua101_shared_check(s, bytes));
    s->magic ^= 1;
    s->version++;
    EXPECT(!ua101_shared_check(s, bytes));
    s->version--;
    s->capture.data_offset += 8; /* the capture frames would run past the block */
    s->capture.data_offset += (int64_t)(RING_FRAMES * 40);
    EXPECT(!ua101_shared_check(s, bytes));
    s->capture.data_offset -= 8 + (int64_t)(RING_FRAMES * 40);
    s->playback.data_offset -= (int64_t)(RING_FRAMES * 40); /* would overlap capture */
    EXPECT(!ua101_shared_check(s, bytes));
    s->playback.data_offset += (int64_t)(RING_FRAMES * 40);
    s->capture.capacity = 1024;
    EXPECT(!ua101_shared_check(s, bytes));
    s->capture.capacity = RING_FRAMES;
    EXPECT(ua101_shared_check(s, bytes));

    EXPECT(!ua101_shared_init(s, bytes - 1, 48000.0, CAP_CHANNELS, PLAY_CHANNELS, RING_FRAMES,
                              PAGE));
    EXPECT(!ua101_shared_init(s, bytes, 0.0, CAP_CHANNELS, PLAY_CHANNELS, RING_FRAMES, PAGE));
    free(s);
}

/* Each side's own views of the rings: what is checked once is what is used, whatever the other
   process writes into the block afterwards. */
static void test_views(void)
{
    size_t bytes = ua101_shared_bytes(CAP_CHANNELS * 4, PLAY_CHANNELS * 4, RING_FRAMES, PAGE);
    ua101_shared *s;
    ua101_frame_view cap, play, bad_cap, bad_play;
    static int32_t in[512 * CAP_CHANNELS], out[512 * CAP_CHANNELS];
    ua101_frame_ring_miss miss;
    size_t i;
    int wrong = 0;

    printf("Views\n");
    if (posix_memalign((void **)&s, PAGE, bytes) != 0)
        return;
    memset(s, 0, bytes);
    EXPECT(ua101_shared_init(s, bytes, 48000.0, CAP_CHANNELS, PLAY_CHANNELS, RING_FRAMES, PAGE));
    EXPECT(ua101_shared_views(s, bytes, CAP_CHANNELS, PLAY_CHANNELS, &cap, &play));
    EXPECT(cap.ring == &s->capture && cap.capacity == RING_FRAMES && cap.frame_bytes == 48 &&
           cap.data == (uint8_t *)s + PAGE);
    EXPECT(play.ring == &s->playback && play.frame_bytes == 40 &&
           play.data == (uint8_t *)s + PAGE + RING_FRAMES * 48);
    /* One ring only, as the helper's capture-only test uses it. */
    EXPECT(ua101_shared_views(s, bytes, CAP_CHANNELS, 0, &bad_cap, &bad_play) &&
           bad_cap.data == cap.data && bad_play.data == NULL && bad_play.frame_bytes == 0);

    /* Refused: channel counts that aren't the block's, a mapping smaller than the block, and a
       ring whose size or place was changed. */
    EXPECT(!ua101_shared_views(s, bytes, CAP_CHANNELS + 1, PLAY_CHANNELS, &bad_cap, &bad_play));
    EXPECT(!ua101_shared_views(s, bytes, CAP_CHANNELS, PLAY_CHANNELS - 1, &bad_cap, &bad_play));
    EXPECT(!ua101_shared_views(s, bytes - 1, CAP_CHANNELS, PLAY_CHANNELS, &bad_cap, &bad_play));
    EXPECT(!ua101_shared_views(s, sizeof(*s) - 1, CAP_CHANNELS, PLAY_CHANNELS, &bad_cap, &bad_play));
    s->capture.capacity = RING_FRAMES * 2;
    EXPECT(!ua101_shared_views(s, bytes, CAP_CHANNELS, PLAY_CHANNELS, &bad_cap, &bad_play));
    s->capture.capacity = RING_FRAMES;
    s->playback.frame_bytes = 44;
    EXPECT(!ua101_shared_views(s, bytes, CAP_CHANNELS, PLAY_CHANNELS, &bad_cap, &bad_play));
    s->playback.frame_bytes = 40;
    s->playback.data_offset += PAGE; /* its last page would be past the end */
    EXPECT(!ua101_shared_views(s, bytes, CAP_CHANNELS, PLAY_CHANNELS, &bad_cap, &bad_play));
    s->playback.data_offset = s->capture.data_offset - 4096; /* over capture's frames */
    EXPECT(!ua101_shared_views(s, bytes, CAP_CHANNELS, PLAY_CHANNELS, &bad_cap, &bad_play));
    s->playback.data_offset = INT64_MIN;
    EXPECT(!ua101_shared_views(s, bytes, CAP_CHANNELS, PLAY_CHANNELS, &bad_cap, &bad_play));
    s->playback.data_offset = (int64_t)(play.data - (uint8_t *)&s->playback);
    s->ring_frames = RING_FRAMES - 1;
    EXPECT(!ua101_shared_views(s, bytes, CAP_CHANNELS, PLAY_CHANNELS, &bad_cap, &bad_play));
    s->ring_frames = RING_FRAMES;
    EXPECT(ua101_shared_views(s, bytes, CAP_CHANNELS, PLAY_CHANNELS, &bad_cap, &bad_play));

    /* Through a view, the other side's later changes to the ring's size and place don't
       matter, and neither do nonsense counters: every copy stays inside the frames and the
       caller's buffer (the address checker would stop this test otherwise). */
    for (i = 0; i < 512 * CAP_CHANNELS; i++)
        in[i] = (int32_t)(i * 2654435761u);
    ua101_frame_view_write(&cap, 1000, in, 512);
    s->capture.capacity = UINT64_C(1) << 40;
    s->capture.frame_bytes = 4096;
    s->capture.data_offset += (int64_t)bytes * 16;
    EXPECT(ua101_frame_view_read(&cap, 1000, out, 512, &miss) == 512 && miss.not_yet == 0);
    for (i = 0; i < 512 * CAP_CHANNELS; i++)
        wrong += out[i] != in[i];
    EXPECT(wrong == 0);
    ua101_frame_view_write(&cap, 1512, in, 512);
    EXPECT(ua101_frame_view_end(&cap) == 2024);
    {
        static const uint64_t values[] = {0, 1, 1511, 1512, 2024, RING_FRAMES, RING_FRAMES + 7,
                                          UINT64_C(1) << 40, UINT64_MAX - 100, UINT64_MAX};
        size_t a, b, c;

        for (a = 0; a < sizeof(values) / sizeof(values[0]); a++)
            for (b = 0; b < sizeof(values) / sizeof(values[0]); b++)
                for (c = 0; c < sizeof(values) / sizeof(values[0]); c++) {
                    atomic_store(&s->capture.start, values[a]);
                    atomic_store(&s->capture.end, values[b]);
                    atomic_store(&s->capture.writing_to, values[c]);
                    ua101_frame_view_read(&cap, 1400, out, 512, &miss);
                    ua101_frame_view_read(&cap, values[a], out, 512, NULL);
                    ua101_frame_view_write(&cap, values[c] / 2, in, 512);
                    ua101_frame_view_write(&cap, 3000, in, 512);
                }
        g_tests++; /* it got here */
    }
    free(s);
}

static void test_offsets(void)
{
    printf("Offsets\n");
    /* 8 ms at every rate, rounded up to whole frames. */
    EXPECT(ua101_offset_frames(UA101_INPUT_OFFSET_SECONDS, 48000.0) == 384);
    EXPECT(ua101_offset_frames(UA101_OUTPUT_OFFSET_SECONDS, 48000.0) == 384);
    EXPECT(ua101_offset_frames(UA101_INPUT_OFFSET_SECONDS, 44100.0) == 353);
    EXPECT(ua101_offset_frames(UA101_OUTPUT_OFFSET_SECONDS, 44100.0) == 353);
    EXPECT(ua101_offset_frames(UA101_INPUT_OFFSET_SECONDS, 88200.0) == 706);
    EXPECT(ua101_offset_frames(UA101_OUTPUT_OFFSET_SECONDS, 88200.0) == 706);
    EXPECT(ua101_offset_frames(UA101_INPUT_OFFSET_SECONDS, 96000.0) == 768);
    EXPECT(ua101_offset_frames(UA101_OUTPUT_OFFSET_SECONDS, 96000.0) == 768);
    EXPECT(ua101_offset_frames(UA101_INPUT_OFFSET_SECONDS, 192000.0) == 1536);
    EXPECT(ua101_offset_frames(UA101_OUTPUT_OFFSET_SECONDS, 192000.0) == 1536);
    /* The times as before 1.1, through the same rounding. */
    EXPECT(ua101_offset_frames(0.005, 48000.0) == 240);
    EXPECT(ua101_offset_frames(1024.0 / 48000.0, 48000.0) == 1024);
    EXPECT(ua101_offset_frames(0.005, 44100.0) == 221);
    EXPECT(ua101_offset_frames(1024.0 / 48000.0, 44100.0) == 941);
    EXPECT(ua101_offset_frames(1024.0 / 48000.0, 192000.0) == 4096);
    EXPECT(ua101_offset_frames(0.0, 48000.0) == 0 && ua101_offset_frames(-1.0, 48000.0) == 0 &&
           ua101_offset_frames(NAN, 48000.0) == 0 && ua101_offset_frames(1e9, 48000.0) == 1000000);
}

static void test_clock_lines(void)
{
    ua101_shared_clock c;
    ua101_clock_line l;
    double sample;
    uint64_t host, now;
    const double tps = 24e6 / 48000.0; /* M1 ticks per sample at 48 kHz */
    int i, ok = 1;

    printf("Clock lines and zero timestamps\n");
    memset(&c, 0, sizeof(c));
    EXPECT(!ua101_shared_read(&c, &l)); /* nothing published */
    ua101_shared_new_seed(&c);
    EXPECT(!ua101_shared_read(&c, &l));
    ua101_shared_publish(&c, 0, 1e9, tps);
    EXPECT(ua101_shared_read(&c, &l) && l.seed == 1 && l.sample == 0 && l.host == 1e9 &&
           l.ticks_per_sample == tps);
    ua101_shared_publish(&c, 0, 1e9, 0.0); /* refused: no slope */
    ua101_shared_publish(&c, 0, NAN, tps); /* refused */
    EXPECT(ua101_shared_read(&c, &l) && l.ticks_per_sample == tps && l.host == 1e9);

    EXPECT(fabs(ua101_line_host(&l, 1000.0) - (1e9 + 1000.0 * tps)) < 1e-6);
    EXPECT(fabs(ua101_line_sample(&l, 1e9 + 1000.5 * tps) - 1000.5) < 1e-9);

    now = (uint64_t)(1e9 + 1000.5 * tps);
    EXPECT(ua101_zero_timestamp(&l, now, 512, &sample, &host) && sample == 512.0 &&
           host == (uint64_t)(1e9 + 512.0 * tps));
    EXPECT(!ua101_zero_timestamp(&l, (uint64_t)(1e9 - 10.0 * tps), 512, &sample, &host));
    EXPECT(!ua101_zero_timestamp(&l, now, 0, &sample, &host));

    /* Always the latest whole period at or before now, and never after it. */
    for (i = 0; i < 100000; i++) {
        uint64_t t = (uint64_t)1e9 + (uint64_t)i * 977u;

        if (!ua101_zero_timestamp(&l, t, 512, &sample, &host) || host > t ||
            fmod(sample, 512.0) != 0.0 || ua101_line_sample(&l, (double)t) - sample >= 512.0) {
            ok = 0;
            break;
        }
    }
    EXPECT(ok);

    /* A new start: the line is cleared and the seed moves on. */
    ua101_shared_new_seed(&c);
    EXPECT(!ua101_shared_read(&c, &l));
    ua101_shared_publish(&c, 0, 2e9, tps);
    EXPECT(ua101_shared_read(&c, &l) && l.seed == 2 && l.host == 2e9);
}

/* --- The sequence lock, raced by two threads. --- */

#define LINES 2000000

static ua101_shared_clock g_clock;
static atomic_int g_writer_done;

/* Each line's three values come from one number k, so a line read half old and half new
   doesn't add up. */
static double host_of(uint64_t k) { return 1e9 + (double)k * 7.25; }
static double slope_of(uint64_t k) { return 500.0 + (double)k * 1e-7; }

static void *clock_writer(void *arg)
{
    uint64_t k;

    (void)arg;
    for (k = 1; k <= LINES; k++)
        ua101_shared_publish(&g_clock, k * 1000, host_of(k), slope_of(k));
    atomic_store(&g_writer_done, 1);
    return NULL;
}

static void test_clock_race(void)
{
    pthread_t writer;
    uint64_t reads = 0, gave_up = 0, torn = 0, backwards = 0, last = 0;

    printf("Clock lines, two threads at once\n");
    memset(&g_clock, 0, sizeof(g_clock));
    ua101_shared_new_seed(&g_clock);
    atomic_store(&g_writer_done, 0);
    pthread_create(&writer, NULL, clock_writer, NULL);
    while (!atomic_load(&g_writer_done)) {
        ua101_clock_line l;

        if (!ua101_shared_read(&g_clock, &l)) {
            gave_up++;
            continue;
        }
        reads++;
        if (l.sample % 1000 != 0 || l.host != host_of(l.sample / 1000) ||
            l.ticks_per_sample != slope_of(l.sample / 1000) || l.seed != 1)
            torn++;
        if (l.sample < last)
            backwards++;
        last = l.sample;
    }
    pthread_join(writer, NULL);
    printf("  %llu reads while %d lines were published, %llu torn, %llu gave up "
           "(no line yet, or the writer kept changing it)\n",
           (unsigned long long)reads, LINES, (unsigned long long)torn,
           (unsigned long long)gave_up);
    EXPECT(reads > 1000);
    EXPECT(torn == 0);
    EXPECT(backwards == 0);
}

/* --- The rings through two mappings, as in two processes. --- */

#define TOTAL_FRAMES 3000000ull
#define GAP_EVERY 10000ull
#define GAP_AT 5000ull
#define GAP_FRAMES 100ull

/* Like the frame ring test: each frame says which it is, twice, the second time inverted. */
static void make_frame(uint8_t *f, uint64_t n, uint32_t frame_bytes)
{
    uint64_t a = n + 1, b = ~(n + 1);
    uint32_t i;

    memcpy(f, &a, 8);
    memcpy(f + 8, &b, 8);
    for (i = 16; i < frame_bytes; i++)
        f[i] = (uint8_t)(n + i);
}

static int frame_is(const uint8_t *f, uint64_t n, uint32_t frame_bytes)
{
    uint8_t want[256];

    make_frame(want, n, frame_bytes);
    return memcmp(f, want, frame_bytes) == 0;
}

static int frame_is_silent(const uint8_t *f, uint32_t frame_bytes)
{
    uint32_t i;

    for (i = 0; i < frame_bytes; i++)
        if (f[i] != 0)
            return 0;
    return 1;
}

static int in_gap(uint64_t n)
{
    return n % GAP_EVERY >= GAP_AT && n % GAP_EVERY < GAP_AT + GAP_FRAMES;
}

typedef struct {
    ua101_shared *view; /* the writer's mapping */
    _Atomic uint64_t *read_to; /* how far the reader has got, so the writer stays in reach */
} writer_args;

static void *ring_writer(void *arg)
{
    writer_args *w = arg;
    ua101_frame_ring *r = &w->view->capture;
    uint8_t chunk[96 * 48];
    uint64_t n = 0;
    unsigned size = 95;

    while (n < TOTAL_FRAMES) {
        uint64_t i, real = 0, first = n;

        while (n + size > atomic_load(w->read_to) + RING_FRAMES / 2)
            sched_yield();
        /* A transfer's worth, with the gap's frames never written. */
        for (i = 0; i < size; i++) {
            if (in_gap(n + i)) {
                if (real > 0)
                    ua101_frame_ring_write(r, first, chunk, real);
                real = 0;
                first = n + i + 1;
                continue;
            }
            make_frame(chunk + real * 48, n + i, 48);
            real++;
        }
        if (real > 0)
            ua101_frame_ring_write(r, first, chunk, real);
        n += size;
        size = size == 95 ? 96 : 95;
    }
    return NULL;
}

static void test_two_mappings(void)
{
    size_t bytes = ua101_shared_bytes(CAP_CHANNELS * 4, PLAY_CHANNELS * 4, RING_FRAMES, PAGE);
    char path[] = "/tmp/ua101-test-shared-XXXXXX";
    int fd = mkstemp(path);
    ua101_shared *a, *b;
    _Atomic uint64_t read_to = 0;
    writer_args w;
    pthread_t writer;
    uint8_t buf[256 * 48];
    uint64_t n = 0, wrong = 0, missing_outside_gaps = 0, silent = 0, real_total = 0;
    ua101_frame_ring_miss miss;

    printf("Rings through two mappings at different addresses\n");
    EXPECT(fd >= 0 && ftruncate(fd, (off_t)bytes) == 0);
    a = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    b = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    unlink(path);
    EXPECT(a != MAP_FAILED && b != MAP_FAILED && a != b);
    if (a == MAP_FAILED || b == MAP_FAILED)
        return;

    EXPECT(ua101_shared_init(a, bytes, 48000.0, CAP_CHANNELS, PLAY_CHANNELS, RING_FRAMES, PAGE));
    EXPECT(ua101_shared_check(b, bytes)); /* the other side sees a good block */

    /* One frame, written through one mapping and read through the other. */
    {
        uint8_t f[48], g[48];

        make_frame(f, 0, 40);
        ua101_frame_ring_write(&a->playback, 0, f, 1);
        EXPECT(ua101_frame_ring_read(&b->playback, 0, g, 1, NULL) == 1 && frame_is(g, 0, 40));
    }

    w.view = a;
    w.read_to = &read_to;
    pthread_create(&writer, NULL, ring_writer, &w);
    while (n < TOTAL_FRAMES) {
        uint64_t want = TOTAL_FRAMES - n < 256 ? TOTAL_FRAMES - n : 256, i;

        /* Waits until the writer is past these frames. It writes a little past the total. */
        while (ua101_frame_ring_end(&b->capture) < n + want)
            sched_yield();
        real_total += ua101_frame_ring_read(&b->capture, n, buf, want, &miss);
        for (i = 0; i < want; i++) {
            const uint8_t *f = buf + i * 48;

            if (frame_is_silent(f, 48)) {
                silent++;
                if (!in_gap(n + i))
                    missing_outside_gaps++;
            } else if (!frame_is(f, n + i, 48)) {
                wrong++;
            }
        }
        n += want;
        atomic_store(&read_to, n);
    }
    pthread_join(writer, NULL);
    printf("  %llu frames: %llu given by the ring, %llu silent (%llu in gaps), %llu wrong, %llu missing "
           "outside the gaps\n",
           (unsigned long long)n, (unsigned long long)real_total, (unsigned long long)silent,
           (unsigned long long)(TOTAL_FRAMES / GAP_EVERY * GAP_FRAMES),
           (unsigned long long)wrong, (unsigned long long)missing_outside_gaps);
    EXPECT(wrong == 0);
    EXPECT(missing_outside_gaps == 0);
    EXPECT(silent == TOTAL_FRAMES / GAP_EVERY * GAP_FRAMES);
    EXPECT(a->capture.lost == TOTAL_FRAMES / GAP_EVERY * GAP_FRAMES);
    munmap(a, bytes);
    munmap(b, bytes);
}

static void test_read_speed(void)
{
    ua101_shared_clock c;
    ua101_clock_line l;
    struct timespec t0, t1;
    double sum = 0.0, ns;
    int i, n = 1000000;

    memset(&c, 0, sizeof(c));
    ua101_shared_new_seed(&c);
    ua101_shared_publish(&c, 123, 4.5e9, 500.0);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (i = 0; i < n; i++) {
        double sample;
        uint64_t host;

        ua101_shared_read(&c, &l);
        ua101_zero_timestamp(&l, (uint64_t)4.6e9 + (uint64_t)i, 512, &sample, &host);
        sum += sample;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    ns = ((double)(t1.tv_sec - t0.tv_sec) * 1e9 + (double)(t1.tv_nsec - t0.tv_nsec)) / n;
    printf("Speed: %.0f ns to read the line and make a zero timestamp (checksum %.0f)\n", ns,
           sum / n);
    EXPECT(ns < 2000.0);
}

int main(void)
{
    test_layout();
    test_views();
    test_offsets();
    test_clock_lines();
    test_clock_race();
    test_two_mappings();
    test_read_speed();

    printf("%d of %d checks passed\n", g_tests - g_failures, g_tests);
    return g_failures == 0 ? 0 : 1;
}
