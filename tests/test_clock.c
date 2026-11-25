/* Tests for the clock loop, against made-up clocks with known answers, using the drift, steps
   and glitches measured in Stage 2. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../src/ua101_clock.h"

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

#define BANDWIDTH_HZ 0.1
/*
 * The worst rate error allowed once settled. The loop's worst moments are 0.13 to 0.32 ppm
 * (2026-11-09; a first guess of 0.05 was tighter than it gives). That is far below hearing
 * (0.5 ppm is under a thousandth of a cent) and doesn't add up over time, because the time
 * error, checked separately, stays within a few microseconds. It's 200 times tighter than
 * the 100 ppm limit.
 */
#define RATE_LIMIT_PPM 0.5
#define MICROFRAME 125e-6
#define TRANSFER_MICROFRAMES 16 /* 2 ms */

static uint32_t g_seed = 20261109u;

static double uniform(void) /* -1 to 1 */
{
    g_seed = g_seed * 1664525u + 1013904223u;
    return (double)g_seed / 2147483648.0 - 1.0;
}

/*
 * Frames against bus time. The unit sends whole frames each 125 us microframe, as many as its
 * own clock has made, so the count steps by one frame at a time. relative_drift_per_s changes
 * its rate over time, as heat did in Stage 2. Returns the worst rate error in ppm and the worst
 * time error in us, both after settle_s.
 */
static void frames_against_bus(double rate_hz, double relative_drift_per_s, double seconds,
                               double settle_s, double *worst_ppm, double *worst_us,
                               double *lock_s)
{
    ua101_dll d;
    uint64_t m, end = (uint64_t)(seconds / MICROFRAME);
    double phase = 0.37;

    ua101_dll_init(&d, 1.0 / 48000.0, BANDWIDTH_HZ, 0.0);
    *worst_ppm = *worst_us = 0.0;
    *lock_s = -1.0;
    for (m = TRANSFER_MICROFRAMES; m <= end; m += TRANSFER_MICROFRAMES) {
        double t = (double)m * MICROFRAME;
        /* Frames made by time t: the integral of a rate that drifts linearly. */
        double made = rate_hz * (t + 0.5 * relative_drift_per_s * t * t) + phase;
        uint64_t frames = (uint64_t)floor(made);
        double true_rate = rate_hz * (1.0 + relative_drift_per_s * t);
        double ppm, us;

        ua101_dll_update(&d, frames, t);
        if (!ua101_dll_locked(&d))
            continue;
        ppm = fabs((1.0 / ua101_dll_period(&d)) / true_rate - 1.0) * 1e6;
        /* Where frame number `frames` really was: the middle of its step. */
        {
            double tau = t - ((made - (double)frames) - 0.5) / true_rate;

            us = fabs(ua101_dll_time(&d, frames) - tau) * 1e6;
        }
        if (*lock_s < 0.0 && ppm < 1.0 && t > 0.1)
            *lock_s = t;
        if (ppm >= 1.0 && t <= settle_s)
            *lock_s = -1.0;
        if (t > settle_s) {
            if (ppm > *worst_ppm)
                *worst_ppm = ppm;
            if (us > *worst_us)
                *worst_us = us;
        }
    }
}

static void test_locks_to_the_unit(void)
{
    double ppm, us, lock;

    /* The unit's 48 kHz clock: about -122.7 ppm against the bus. */
    frames_against_bus(48000.0 * (1.0 - 122.7e-6), 0.0, 120.0, 30.0, &ppm, &us, &lock);
    printf("  frames vs bus, steady: within 1 ppm from %.1f s; after 30 s worst %.3f ppm, "
           "%.2f us\n", lock, ppm, us);
    EXPECT(lock > 0.0 && lock < 10.0);
    EXPECT(ppm < RATE_LIMIT_PPM);
    EXPECT(us < 3.0);
}

static void test_follows_heat_drift(void)
{
    double ppm, us, lock;

    /* 0.1 ppm per second, as when the Mac heated up: 30 ppm over 5 minutes. */
    frames_against_bus(48000.0 * (1.0 - 122.7e-6), 1e-7, 300.0, 30.0, &ppm, &us, &lock);
    printf("  frames vs bus, drifting 0.1 ppm/s: after 30 s worst %.3f ppm, %.2f us\n", ppm, us);
    EXPECT(ppm < RATE_LIMIT_PPM);
    EXPECT(us < 3.0);
}

/*
 * Bus frame numbers (1 ms) against host time, with timestamp jitter and with 1 reading in 5 one
 * bus frame early, as in Stage 2. Returns the worst errors after settle_s against the true line.
 */
static void bus_against_host(double reject_s, double jump_at_s, double *worst_ppm,
                             double *worst_us, ua101_dll *out)
{
    const double relative = 7e-6; /* host against bus */
    ua101_dll d;
    uint64_t k;
    double offset = 1234.5;

    ua101_dll_init(&d, 0.001, BANDWIDTH_HZ, reject_s);
    *worst_ppm = *worst_us = 0.0;
    for (k = 2; k <= 240000; k += 2) { /* 240 s */
        double t = (double)k * 0.001;
        double host = offset + (double)k * 0.001 * (1.0 + relative) + 5e-6 * uniform();
        uint64_t reported = k;

        if (jump_at_s > 0.0 && t >= jump_at_s)
            host += 0.005; /* as after a sleep: the line itself moved */
        if (uniform() < -0.6) /* about 1 in 5 */
            reported = k - 1;
        ua101_dll_update(&d, reported, host);
        if (t > 60.0 && (jump_at_s <= 0.0 || t > jump_at_s + 60.0)) {
            double true_host = offset + (double)k * 0.001 * (1.0 + relative) +
                               (jump_at_s > 0.0 && t >= jump_at_s ? 0.005 : 0.0);
            double ppm = fabs(ua101_dll_period(&d) / (0.001 * (1.0 + relative)) - 1.0) * 1e6;
            double us = fabs(ua101_dll_time(&d, k) - true_host) * 1e6;

            if (ppm > *worst_ppm)
                *worst_ppm = ppm;
            if (us > *worst_us)
                *worst_us = us;
        }
    }
    *out = d;
}

static void test_rejects_early_bus_stamps(void)
{
    ua101_dll d;
    double ppm, us, ppm_all, us_all;

    bus_against_host(0.0003, 0.0, &ppm, &us, &d);
    printf("  bus vs host, 1 in 5 stamps 1 ms early: worst %.3f ppm, %.1f us, %.0f%% rejected\n",
           ppm, us, 100.0 * (double)d.rejected / (double)(d.rejected + d.updates));
    EXPECT(ppm < RATE_LIMIT_PPM);
    EXPECT(us < 5.0);
    EXPECT(d.rejected > 0.15 * (d.rejected + d.updates) && d.rejected < 0.25 * (d.rejected + d.updates));
    EXPECT(d.restarts == 0);

    /* Without rejecting them, it's off by a fifth of a bus frame: they must be rejected. */
    bus_against_host(0.0, 0.0, &ppm_all, &us_all, &d);
    printf("  the same without rejection: worst %.3f ppm, %.1f us\n", ppm_all, us_all);
    EXPECT(us_all > 100.0);
}

static void test_starts_again_after_a_jump(void)
{
    ua101_dll d;
    double ppm, us;

    bus_against_host(0.0003, 100.0, &ppm, &us, &d);
    printf("  bus vs host, host time jumps 5 ms at 100 s: %lu restart, then worst %.3f ppm, "
           "%.1f us\n", d.restarts, ppm, us);
    EXPECT(d.restarts == 1);
    EXPECT(ppm < RATE_LIMIT_PPM);
    EXPECT(us < 5.0);
}

static void test_ignores_counter_not_moving(void)
{
    ua101_dll d;

    ua101_dll_init(&d, 0.001, BANDWIDTH_HZ, 0.0);
    EXPECT(!ua101_dll_locked(&d));
    EXPECT(ua101_dll_update(&d, 100, 1.0));
    EXPECT(!ua101_dll_update(&d, 100, 1.1));
    EXPECT(!ua101_dll_update(&d, 99, 1.2));
    EXPECT(!ua101_dll_locked(&d));
    EXPECT(ua101_dll_update(&d, 102, 1.002));
    EXPECT(ua101_dll_locked(&d));
    EXPECT(fabs(ua101_dll_time(&d, 102) - 1.002) < 1e-4);
}

/* --- The bus clock as the unit really stamps it (ua101_bus_clock). --- */

/*
 * Stamps as measured on the unit (2026-11-09): the bus
 * counter's tick is sharp, and a stamp is the frame current when it's taken, with that time, so
 * it's never wrong about its frame. The capture callbacks, one each 2 ms, cluster around a tick:
 * `center_us` from it on average (negative: before it), spread evenly over +-60 us. At 60 s the
 * cluster jumps by `jump_us`, as it did in the 10-minute run. The first `early` stamps come 150
 * to 250 us before the tick, a bad start for the batch. The bus runs `ppm` against the host:
 * -1.3 measured. Everything is measured after 10 s, against the true tick.
 */
typedef struct {
    double worst_ppm;              /* the rate the clock gives */
    double plain_ppm;              /* the plain line's, which follows the stamps' cluster */
    double line_lo_us, line_hi_us; /* the plain line, through most stamps */
    double tick_lo_us, tick_hi_us; /* the tick the clock gives */
    double worst_step_us;          /* the most one stamp moved the time it gives, once settled */
    double settled_s;
    ua101_bus_clock c;
} bus_result;

static void bus_stamps_sharp(double center_us, double jump_us, int early, double ppm,
                             bus_result *r)
{
    const double offset = 1234.5, period = 0.001 * (1.0 + ppm * 1e-6);
    ua101_bus_clock *c = &r->c;
    uint64_t k;
    int i = 0;

    ua101_bus_clock_init(c, 0.001, UA101_BUS_CLOCK_BANDWIDTH_HZ, 0.0003);
    r->worst_ppm = r->plain_ppm = r->worst_step_us = 0.0;
    r->line_lo_us = r->tick_lo_us = INFINITY;
    r->line_hi_us = r->tick_hi_us = -INFINITY;
    r->settled_s = -1.0;
    for (k = 2; k <= 240000; k += 2, i++) {
        double t = (double)k * 0.001, tick = offset + (double)k * period;
        double phase_us = center_us + (t >= 60.0 ? jump_us : 0.0) + 60.0 * uniform();
        double host, before = 0.0;
        int was_settled = c->settled;

        if (i < early)
            phase_us = -200.0 + 50.0 * uniform();
        host = tick + phase_us * 1e-6;
        if (was_settled)
            before = ua101_bus_clock_time(c, (double)k);
        ua101_bus_clock_update(c, (uint64_t)floor((host - offset) / period), host);
        if (c->settled && r->settled_s < 0.0)
            r->settled_s = t;
        if (was_settled) {
            double step = fabs(ua101_bus_clock_time(c, (double)k) - before) * 1e6;

            if (step > r->worst_step_us)
                r->worst_step_us = step;
        }
        if (t > 10.0) {
            double ppm = fabs(ua101_bus_clock_period(c) / period - 1.0) * 1e6;
            double plain = fabs(ua101_dll_period(&c->dll) / period - 1.0) * 1e6;
            double line = (ua101_dll_time(&c->dll, k) - tick) * 1e6;
            double given = (ua101_bus_clock_time(c, (double)k) - tick) * 1e6;

            r->worst_ppm = fmax(r->worst_ppm, ppm);
            r->plain_ppm = fmax(r->plain_ppm, plain);
            r->line_lo_us = fmin(r->line_lo_us, line);
            r->line_hi_us = fmax(r->line_hi_us, line);
            r->tick_lo_us = fmin(r->tick_lo_us, given);
            r->tick_hi_us = fmax(r->tick_hi_us, given);
        }
    }
}

static void print_bus_result(const char *what, const bus_result *r)
{
    printf("  %s: settled at %.2f s; the plain line %+.0f to %+.0f us from the tick, worst "
           "%.3f ppm; the clock's tick %+.1f to %+.1f us from it, worst %.3f ppm, moving at most "
           "%.2f us a stamp; tick %s, %lu disagreed; moved a frame %lu times\n",
           what, r->settled_s, r->line_lo_us, r->line_hi_us, r->plain_ppm, r->tick_lo_us,
           r->tick_hi_us, r->worst_ppm, r->worst_step_us,
           r->c.tick_found ? "found" : "not found", r->c.tick_disagree, r->c.moved);
}

/* The most one stamp may move the time the clock gives, once settled: the loop, still narrowing
   in the first seconds, moves it up to about 0.5 us. */
#define STEP_LIMIT_US 1.0

/* The tick the clock gives is within this of the true one, wherever the stamps cluster. */
#define TICK_LIMIT_US 10.0

static void test_bus_clock_as_measured(void)
{
    bus_result r;

    printf("Bus clock, with stamps as the unit gives them\n");

    /* Most just after the tick, as in the capture-only runs: the plain line is at the start of
       each frame, by the cluster. */
    bus_stamps_sharp(30.0, 0.0, 0, -1.3, &r);
    print_bus_result("most stamps 30 us after the tick", &r);
    EXPECT(r.worst_ppm < RATE_LIMIT_PPM);
    EXPECT(r.line_lo_us > 0.0 && r.line_hi_us < 60.0);
    EXPECT(r.tick_lo_us > -TICK_LIMIT_US && r.tick_hi_us < TICK_LIMIT_US);
    EXPECT(r.c.tick_found && r.c.tick_disagree == 0 && r.worst_step_us < STEP_LIMIT_US);
    EXPECT(r.c.starts == 1 && r.c.dll.restarts == 0 && r.settled_s < 1.0);

    /* Most just before it, as in the 60 s run: the plain line is at the end of each frame,
       nearly 1 ms late. The clock's tick is where it was in the case before. */
    bus_stamps_sharp(-30.0, 0.0, 0, -1.3, &r);
    print_bus_result("most stamps 30 us before the tick", &r);
    EXPECT(r.worst_ppm < RATE_LIMIT_PPM);
    EXPECT(r.line_lo_us > 940.0 && r.line_hi_us < 1000.0);
    EXPECT(r.tick_lo_us > -TICK_LIMIT_US && r.tick_hi_us < TICK_LIMIT_US);
    EXPECT(r.c.tick_found && r.c.tick_disagree == 0 && r.worst_step_us < STEP_LIMIT_US);
    EXPECT(r.c.starts == 1 && r.c.dll.restarts == 0 && r.settled_s < 1.0);

    /* The cluster jumps from after the tick to before it, as at 250 s in the 10-minute run.
       The plain line follows the cluster; the tick doesn't move. */
    bus_stamps_sharp(40.0, -80.0, 0, -1.3, &r);
    print_bus_result("the cluster jumping 80 us earlier at 60 s", &r);
    EXPECT(r.worst_ppm < RATE_LIMIT_PPM);
    EXPECT(r.line_lo_us < -30.0 && r.line_hi_us > 30.0);
    EXPECT(r.tick_lo_us > -TICK_LIMIT_US && r.tick_hi_us < TICK_LIMIT_US);
    EXPECT(r.c.tick_found && r.c.tick_disagree == 0 && r.worst_step_us < STEP_LIMIT_US);

    /* Bad starts: the batch mostly before the tick, then the rest after. With 12 of 16, the
       check moves the line a frame; the tick is right either way. */
    bus_stamps_sharp(30.0, 0.0, 5, -1.3, &r);
    print_bus_result("starting with 5 stamps 200 us before the tick", &r);
    EXPECT(r.worst_ppm < RATE_LIMIT_PPM);
    EXPECT(r.tick_lo_us > -TICK_LIMIT_US && r.tick_hi_us < TICK_LIMIT_US);
    EXPECT(r.c.starts == 1 && r.c.dll.restarts == 0 && r.c.tick_disagree == 0);
    bus_stamps_sharp(30.0, 0.0, 12, -1.3, &r);
    print_bus_result("starting with 12 of 16 that way", &r);
    EXPECT(r.worst_ppm < RATE_LIMIT_PPM);
    EXPECT(r.tick_lo_us > -TICK_LIMIT_US && r.tick_hi_us < TICK_LIMIT_US);
    EXPECT(r.c.starts == 1 && r.c.dll.restarts == 0 && r.c.moved == 1 && r.c.tick_disagree == 0);

    /* None near the tick: all 240 to 360 us after it. The tick can't be found, so after a wait
       it's taken just before the earliest stamp, which every stamp still agrees with. */
    bus_stamps_sharp(300.0, 0.0, 0, -1.3, &r);
    print_bus_result("all stamps 240 to 360 us after the tick", &r);
    EXPECT(!r.c.tick_found && r.c.settled && r.settled_s > 1.0 && r.settled_s < 3.0);
    EXPECT(r.tick_lo_us > 200.0 && r.tick_hi_us < 250.0);
    EXPECT(r.c.tick_disagree == 0 && r.worst_step_us < STEP_LIMIT_US);

    /* The bus 15 ppm off its nominal rate either way, more than ten times what was measured:
       the bounds on the tick are carried at the nominal rate and relax at 20 us a second. */
    bus_stamps_sharp(30.0, 0.0, 0, 15.0, &r);
    print_bus_result("the bus 15 ppm fast", &r);
    EXPECT(r.worst_ppm < RATE_LIMIT_PPM);
    EXPECT(r.tick_lo_us > -TICK_LIMIT_US && r.tick_hi_us < TICK_LIMIT_US);
    EXPECT(r.c.tick_found && r.c.tick_disagree == 0 && r.worst_step_us < STEP_LIMIT_US);
    bus_stamps_sharp(-30.0, 0.0, 0, -15.0, &r);
    print_bus_result("the bus 15 ppm slow", &r);
    EXPECT(r.worst_ppm < RATE_LIMIT_PPM);
    EXPECT(r.tick_lo_us > -TICK_LIMIT_US && r.tick_hi_us < TICK_LIMIT_US);
    EXPECT(r.c.tick_found && r.c.tick_disagree == 0 && r.worst_step_us < STEP_LIMIT_US);
}

/*
 * The stamps the unit really gave. The plain line is compared with a straight line fitted
 * through all of them once each is put in its right frame. The clock's tick can't be compared
 * with a true one, but every stamp names its own frame, so it can be checked against them all:
 * a stamp before the tick it gives, or after the next, disagrees with it.
 */
typedef struct {
    size_t n;
    double line_spread_us; /* how far the plain line strays from the fitted one, after 5 s */
    double ppm;            /* its rate against the fitted one's, at the end */
    double corrected;      /* the share of stamps the plain line had to move a frame */
    double tick_us;        /* the clock's tick against the plain line, at the end */
    double worst_step_us;  /* the most one stamp moved the time it gives, once settled */
    ua101_bus_clock c;
} real_result;

static int replay_real_stamps(const char *path, real_result *r)
{
    static uint64_t frame[20000];
    static double time[20000];
    FILE *f = fopen(path, "r");
    char line[128];
    uint64_t b = 0, t = 0;
    size_t n = 0, i;
    ua101_bus_clock *c = &r->c;
    double mx, my, sxx, sxy, slope, base, lo = INFINITY, hi = -INFINITY;
    int pass;

    printf("Bus clock, the unit's own stamps (%s)\n", path);
    if (f == NULL)
        return 0;
    while (n < 20000 && fgets(line, sizeof(line), f) != NULL) {
        unsigned long long db, dt;

        if (line[0] == '#' || sscanf(line, "%llu %llu", &db, &dt) != 2)
            continue;
        b += db;
        t += dt;
        frame[n] = b;
        time[n] = (double)t / 24e6;
        n++;
    }
    fclose(f);
    r->n = n;

    ua101_bus_clock_init(c, 0.001, UA101_BUS_CLOCK_BANDWIDTH_HZ, 0.0003);
    for (i = 0; i < n; i++)
        ua101_bus_clock_update(c, frame[i], time[i]);

    /* The reference: each stamp in its frame by the final line, then a least-squares line,
       twice over. */
    slope = ua101_dll_period(&c->dll);
    base = ua101_dll_time(&c->dll, 0);
    for (pass = 0; pass < 2; pass++) {
        double *x = malloc(n * sizeof(double));

        mx = my = sxx = sxy = 0.0;
        for (i = 0; i < n; i++) {
            x[i] = (double)frame[i] + round((time[i] - base - (double)frame[i] * slope) / slope);
            mx += x[i];
            my += time[i];
        }
        mx /= (double)n;
        my /= (double)n;
        for (i = 0; i < n; i++) {
            sxx += (x[i] - mx) * (x[i] - mx);
            sxy += (x[i] - mx) * (time[i] - my);
        }
        slope = sxy / sxx;
        base = my - slope * mx;
        free(x);
    }

    /* Replays again, comparing the line as it goes with the reference, after 5 s, and
       watching how much each stamp moves the time the clock gives. */
    ua101_bus_clock_init(c, 0.001, UA101_BUS_CLOCK_BANDWIDTH_HZ, 0.0003);
    r->worst_step_us = 0.0;
    for (i = 0; i < n; i++) {
        int was_settled = c->settled;
        double before = was_settled ? ua101_bus_clock_time(c, (double)frame[i]) : 0.0;

        ua101_bus_clock_update(c, frame[i], time[i]);
        if (was_settled)
            r->worst_step_us = fmax(r->worst_step_us,
                                    fabs(ua101_bus_clock_time(c, (double)frame[i]) - before) * 1e6);
        if (time[i] - time[0] > 5.0) {
            double d = ua101_dll_time(&c->dll, frame[i]) - (base + (double)frame[i] * slope);

            if (d < lo)
                lo = d;
            if (d > hi)
                hi = d;
        }
    }
    r->line_spread_us = (hi - lo) * 1e6;
    r->ppm = (ua101_dll_period(&c->dll) / slope - 1.0) * 1e6;
    r->corrected = (double)c->corrected / (double)n;
    r->tick_us = (c->tick - ua101_dll_time(&c->dll, c->tick_frame)) * 1e6;
    printf("  %zu stamps; bus against host %+.2f ppm by the reference; the plain line, after "
           "5 s, within %.1f to %.1f us of it, rate %+.3f ppm off at the end; %.1f%% taken as "
           "the next frame; %lu start, %lu restarts, %lu rejected\n",
           n, (slope / 0.001 - 1.0) * 1e6, lo * 1e6, hi * 1e6, r->ppm, 100.0 * r->corrected,
           c->starts, c->dll.restarts, c->dll.rejected);
    printf("  the clock's tick: %s, %+.0f us from the plain line at the end, bounds %.1f us "
           "apart; %lu stamps disagreed with it (%.2f%%); it moved at most %.2f us a stamp\n",
           c->tick_found ? "found" : "not found", r->tick_us,
           (c->tick_hi - c->tick_lo) * 1e6, c->tick_disagree,
           100.0 * (double)c->tick_disagree / (double)n, r->worst_step_us);
    return 1;
}

/* Disagreeing stamps allowed, as a share of all: the plain line disagrees with 2 to 21%. */
#define TICK_DISAGREE_LIMIT 0.002

static void test_bus_clock_real_stamps(void)
{
    real_result r;

    /* Capture only: most stamps just after the tick, so the plain line is right by the tick. */
    EXPECT(replay_real_stamps("tests/data/ua101-bus-stamps-20s.txt", &r));
    EXPECT(r.n == 10035);
    EXPECT(r.c.starts == 1 && r.c.dll.restarts == 0);
    EXPECT(r.line_spread_us < 30.0 && fabs(r.ppm) < 1.0);
    EXPECT(r.corrected > 0.04 && r.corrected < 0.11);
    EXPECT(r.c.tick_found && r.c.tick_disagree <= TICK_DISAGREE_LIMIT * r.n);
    EXPECT(r.tick_us > -100.0 && r.tick_us < 0.0);
    EXPECT(r.worst_step_us < STEP_LIMIT_US);

    /* Capture and playback, most stamps just before the tick: the plain line sits at the end of
       each frame, and the tick is nearly a whole frame before it. */
    EXPECT(replay_real_stamps("tests/data/ua101-bus-stamps-duplex-20s.txt", &r));
    EXPECT(r.n == 10000);
    EXPECT(r.c.starts == 1 && r.c.dll.restarts == 0);
    EXPECT(r.line_spread_us < 30.0 && fabs(r.ppm) < 1.0);
    EXPECT(r.corrected > 0.10);
    EXPECT(r.c.tick_found && r.c.tick_disagree <= TICK_DISAGREE_LIMIT * r.n);
    EXPECT(r.tick_us > -1000.0 && r.tick_us < -900.0);
    EXPECT(r.worst_step_us < STEP_LIMIT_US);

    /* The cluster crossing the tick, with two late completions: the tick moves smoothly and
       still explains every stamp. */
    EXPECT(replay_real_stamps("tests/data/ua101-bus-stamps-crossing-40s.txt", &r));
    EXPECT(r.n == 20000);
    EXPECT(r.c.starts == 1 && r.c.dll.restarts == 0);
    EXPECT(r.c.tick_found && r.c.tick_disagree <= TICK_DISAGREE_LIMIT * r.n);
    EXPECT(r.worst_step_us < STEP_LIMIT_US);
}

int main(void)
{
    test_locks_to_the_unit();
    test_follows_heat_drift();
    test_rejects_early_bus_stamps();
    test_starts_again_after_a_jump();
    test_ignores_counter_not_moving();
    test_bus_clock_as_measured();
    test_bus_clock_real_stamps();

    printf("%d of %d checks passed\n", g_tests - g_failures, g_tests);
    return g_failures == 0 ? 0 : 1;
}
