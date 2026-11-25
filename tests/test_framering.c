/* Tests for the frame ring: timekeeping, silence for what's missing, and two threads at once. */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../src/ua101_framering.h"

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

/* A frame says which frame it is, twice, the second time inverted: a stale frame has the wrong
   number, and a torn one (half from each of two writes) has halves that don't match. */
typedef struct {
    uint64_t number_plus_one;
    uint64_t inverted;
} frame;

static void make_frames(frame *f, uint64_t first, uint64_t n)
{
    uint64_t i;

    for (i = 0; i < n; i++) {
        f[i].number_plus_one = first + i + 1;
        f[i].inverted = ~(first + i + 1);
    }
}

static int is_frame(const frame *f, uint64_t number)
{
    return f->number_plus_one == number + 1 && f->inverted == ~(number + 1);
}

static int is_silence(const frame *f)
{
    return f->number_plus_one == 0 && f->inverted == 0;
}

static void test_in_order(void)
{
    frame store[64], in[40], out[50];
    ua101_frame_ring r;
    ua101_frame_ring_miss miss;
    uint64_t i;
    int ok = 1;

    EXPECT(ua101_frame_ring_init(&r, (uint8_t *)store, 64, sizeof(frame)));
    memset(out, 0xAB, sizeof(out));
    EXPECT(ua101_frame_ring_read(&r, 0, out, 10, &miss) == 0);
    EXPECT(miss.not_yet == 10 && is_silence(&out[0]) && is_silence(&out[9]));
    EXPECT(ua101_frame_ring_end(&r) == 0);

    /* Starts at frame 1000, as a stream that began some time after the bus did. */
    make_frames(in, 1000, 40);
    ua101_frame_ring_write(&r, 1000, in, 15);
    ua101_frame_ring_write(&r, 1015, in + 15, 25);
    EXPECT(ua101_frame_ring_end(&r) == 1040);
    EXPECT(ua101_frame_ring_read(&r, 995, out, 50, &miss) == 40);
    for (i = 0; i < 50; i++)
        if (i < 5 || i >= 45 ? !is_silence(&out[i]) : !is_frame(&out[i], 995 + i))
            ok = 0;
    EXPECT(ok);
    EXPECT(miss.not_yet == 10 && miss.overwritten == 0);
    EXPECT(r.lost == 0 && r.repeated == 0);
}

/* A stall: frames that never came read as silence in their own place, and later ones stay put. */
static void test_gap_keeps_time(void)
{
    frame store[256], in[100], out[200];
    ua101_frame_ring r;
    uint64_t i;
    int ok = 1;

    EXPECT(ua101_frame_ring_init(&r, (uint8_t *)store, 256, sizeof(frame)));
    memset(store, 0xCD, sizeof(store)); /* whatever was there before must not show through */
    make_frames(in, 0, 100);
    ua101_frame_ring_write(&r, 0, in, 100);
    make_frames(in, 150, 50);
    ua101_frame_ring_write(&r, 150, in, 50);
    EXPECT(r.lost == 50);
    EXPECT(ua101_frame_ring_read(&r, 0, out, 200, NULL) == 200);
    for (i = 0; i < 200; i++)
        if (i >= 100 && i < 150 ? !is_silence(&out[i]) : !is_frame(&out[i], i))
            ok = 0;
    EXPECT(ok);
}

/* Frames given twice: the first copy stays, and only the new part is written. */
static void test_repeats_ignored(void)
{
    frame store[256], in[120], other[40], out[120];
    ua101_frame_ring r;
    uint64_t i;
    int ok = 1;

    EXPECT(ua101_frame_ring_init(&r, (uint8_t *)store, 256, sizeof(frame)));
    make_frames(in, 0, 120);
    ua101_frame_ring_write(&r, 0, in, 100);
    memcpy(other, in + 80, sizeof(other));
    for (i = 0; i < 20; i++)
        other[i].inverted ^= 1; /* marks the repeated part, which must not be used */
    ua101_frame_ring_write(&r, 80, other, 40);
    ua101_frame_ring_write(&r, 10, in + 10, 5);
    EXPECT(r.repeated == 25 && r.lost == 0);
    EXPECT(ua101_frame_ring_end(&r) == 120);
    EXPECT(ua101_frame_ring_read(&r, 0, out, 120, NULL) == 120);
    for (i = 0; i < 120; i++)
        if (!is_frame(&out[i], i))
            ok = 0;
    EXPECT(ok);
}

/* Round and round: only the newest whole ring is there, and older frames read as silence. */
static void test_wraps_and_overwrites(void)
{
    frame store[64], in[7], out[100];
    ua101_frame_ring r;
    ua101_frame_ring_miss miss;
    uint64_t at = 0, i;
    int ok = 1;

    EXPECT(ua101_frame_ring_init(&r, (uint8_t *)store, 64, sizeof(frame)));
    while (at < 1000) {
        make_frames(in, at, 7);
        ua101_frame_ring_write(&r, at, in, 7);
        at += 7;
    }
    EXPECT(ua101_frame_ring_end(&r) == at);
    EXPECT(ua101_frame_ring_read(&r, at - 64, out, 64, &miss) == 64);
    for (i = 0; i < 64; i++)
        if (!is_frame(&out[i], at - 64 + i))
            ok = 0;
    EXPECT(ok);
    EXPECT(miss.not_yet == 0 && miss.overwritten == 0);

    EXPECT(ua101_frame_ring_read(&r, at - 100, out, 100, &miss) == 64);
    EXPECT(miss.overwritten == 36 && miss.not_yet == 0);
    EXPECT(is_silence(&out[35]) && is_frame(&out[36], at - 64));
    EXPECT(ua101_frame_ring_read(&r, 0, out, 10, &miss) == 0);
    EXPECT(miss.overwritten == 10 && is_silence(&out[0]));
}

/* A gap longer than the ring: the ring is all silence, then the new frames. */
static void test_long_gap(void)
{
    frame store[64], in[10], out[64];
    ua101_frame_ring r;
    uint64_t i;
    int ok = 1;

    EXPECT(ua101_frame_ring_init(&r, (uint8_t *)store, 64, sizeof(frame)));
    make_frames(in, 0, 10);
    ua101_frame_ring_write(&r, 0, in, 10);
    make_frames(in, 1000, 10);
    ua101_frame_ring_write(&r, 1000, in, 10);
    EXPECT(r.lost == 990);
    EXPECT(ua101_frame_ring_read(&r, 1010 - 64, out, 64, NULL) == 64);
    for (i = 0; i < 64; i++)
        if (i < 54 ? !is_silence(&out[i]) : !is_frame(&out[i], 946 + i))
            ok = 0;
    EXPECT(ok);
}

static void test_refusals(void)
{
    frame store[64];
    ua101_frame_ring r;

    EXPECT(!ua101_frame_ring_init(&r, (uint8_t *)store, 48, sizeof(frame)));
    EXPECT(!ua101_frame_ring_init(&r, (uint8_t *)store, 0, sizeof(frame)));
    EXPECT(!ua101_frame_ring_init(&r, (uint8_t *)store, 64, 0));
    EXPECT(!ua101_frame_ring_init(&r, NULL, 64, sizeof(frame)));
    EXPECT(ua101_frame_ring_init(&r, (uint8_t *)store, 1, sizeof(frame)));
}

/* A new start counts from 0 again, with nothing of the old stream left to read. */
static void test_reset(void)
{
    frame store[64], in[8], out[8];
    ua101_frame_ring r;
    ua101_frame_ring_miss miss;

    EXPECT(ua101_frame_ring_init(&r, (uint8_t *)store, 64, sizeof(frame)));
    make_frames(in, 5000, 8);
    ua101_frame_ring_write(&r, 5000, in, 8);
    EXPECT(ua101_frame_ring_end(&r) == 5008);
    ua101_frame_ring_reset(&r);
    EXPECT(ua101_frame_ring_end(&r) == 0 && r.lost == 0 && r.repeated == 0);
    EXPECT(ua101_frame_ring_read(&r, 5000, out, 8, &miss) == 0 && miss.not_yet == 8);
    make_frames(in, 0, 8);
    ua101_frame_ring_write(&r, 0, in, 8); /* not taken as a repeat of older frames */
    EXPECT(ua101_frame_ring_read(&r, 0, out, 8, &miss) == 8 && is_frame(&out[0], 0) &&
           is_frame(&out[7], 7) && r.repeated == 0);
}

/*
 * Two threads, flat out, with a small ring so the reader is often a whole ring behind and
 * frames are overwritten while it copies them, and with gaps now and then. Every frame read must
 * be either the right frame or silence, never a stale or torn one, and the counts must add up.
 * With the check after the copy removed (2026-11-09), this caught about 1.4 million bad frames a
 * run, so it does reach the moment a frame is overwritten during a copy.
 */
enum { STRESS_RING = 256, STRESS_FRAMES = 100000000 };

typedef struct {
    ua101_frame_ring ring;
    frame store[STRESS_RING];
    _Atomic int done;
} stress;

static void *stress_writer(void *arg)
{
    stress *s = arg;
    frame in[97];
    uint64_t at = 0, seed = 12345;

    while (at < STRESS_FRAMES) {
        uint64_t n;

        seed = seed * 6364136223846793005u + 1442695040888963407u;
        n = 1 + (seed >> 33) % 97;
        if ((seed >> 20) % 1000 == 0)
            at += 1 + (seed >> 40) % 500; /* a stall */
        make_frames(in, at, n);
        ua101_frame_ring_write(&s->ring, at, in, n);
        at += n;
    }
    atomic_store(&s->done, 1);
    return NULL;
}

static void test_two_threads(void)
{
    static stress s;
    static frame out[300];
    pthread_t writer;
    uint64_t reads = 0, real = 0, missing = 0, bad = 0, gaps_seen = 0, seed = 999;
    struct timespec t0, t1;

    EXPECT(ua101_frame_ring_init(&s.ring, (uint8_t *)s.store, STRESS_RING, sizeof(frame)));
    atomic_store(&s.done, 0);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    pthread_create(&writer, NULL, stress_writer, &s);
    while (!atomic_load(&s.done)) {
        uint64_t end = ua101_frame_ring_end(&s.ring), first, n, got, i;
        ua101_frame_ring_miss miss;

        seed = seed * 6364136223846793005u + 1442695040888963407u;
        n = 1 + (seed >> 33) % 300;
        /* Sometimes close behind the writer, sometimes further back than the ring. */
        first = end > n + (seed >> 50) % 400 ? end - n - (seed >> 50) % 400 : 0;
        got = ua101_frame_ring_read(&s.ring, first, out, n, &miss);
        reads++;
        real += got;
        missing += miss.not_yet + miss.overwritten;
        if (got + miss.not_yet + miss.overwritten != n)
            bad++;
        for (i = 0; i < n; i++) {
            if (is_silence(&out[i]))
                gaps_seen++;
            else if (!is_frame(&out[i], first + i))
                bad++;
        }
    }
    pthread_join(writer, NULL);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    printf("  two threads, %.1f s: %llu reads, %llu frames real, %llu missing, %llu silent "
           "frames seen, %llu bad\n",
           (double)(t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) * 1e-9,
           (unsigned long long)reads, (unsigned long long)real, (unsigned long long)missing,
           (unsigned long long)gaps_seen, (unsigned long long)bad);
    EXPECT(bad == 0);
    EXPECT(reads > 1000 && real > 0 && missing > 0);
    EXPECT(s.ring.lost > 0);
}

int main(void)
{
    test_in_order();
    test_gap_keeps_time();
    test_repeats_ignored();
    test_wraps_and_overwrites();
    test_long_gap();
    test_refusals();
    test_reset();
    test_two_threads();

    printf("%d of %d checks passed\n", g_tests - g_failures, g_tests);
    return g_failures == 0 ? 0 : 1;
}
