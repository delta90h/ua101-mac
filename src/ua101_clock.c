#include "ua101_clock.h"

#include <limits.h>
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* How fast the bandwidth narrows: start / (1 + elapsed / NARROW_SECONDS), down to the target. */
#define NARROW_SECONDS 1.0
/* Readings before rejection starts, so a first guess that's off can't lock it out. */
#define WARMUP_UPDATES 200
/* Keeps each step's loop gain small, as the per-reading formulas assume. */
#define MAX_OMEGA 0.5

void ua101_dll_init(ua101_dll *d, double nominal_period, double bandwidth_hz,
                    double reject_seconds)
{
    memset(d, 0, sizeof(*d));
    d->bandwidth_hz = bandwidth_hz;
    d->start_bandwidth_hz = bandwidth_hz * 20.0;
    d->nominal_period = nominal_period;
    d->reject_seconds = reject_seconds;
    d->restart_after = 50;
    d->period = nominal_period;
}

static void restart(ua101_dll *d, uint64_t x, double y)
{
    d->x_ref = x;
    d->y_ref = y;
    d->period = d->nominal_period;
    d->elapsed = 0.0;
    d->updates = 1;
    d->in_a_row = 0;
}

int ua101_dll_update(ua101_dll *d, uint64_t x, double y)
{
    double dx, predicted, error, bandwidth, omega, b, c;

    if (d->updates == 0) {
        restart(d, x, y);
        return 1;
    }
    if (x <= d->x_ref)
        return 0;
    dx = (double)(x - d->x_ref);
    predicted = d->y_ref + dx * d->period;
    error = y - predicted;

    if (d->reject_seconds > 0.0 && d->updates > WARMUP_UPDATES &&
        fabs(error) > d->reject_seconds) {
        d->rejected++;
        if (++d->in_a_row >= d->restart_after) {
            d->restarts++;
            restart(d, x, y);
            return 1;
        }
        return 0;
    }
    d->in_a_row = 0;

    /* A second-order loop, with its gains worked out for this reading's interval. */
    bandwidth = d->start_bandwidth_hz / (1.0 + d->elapsed / NARROW_SECONDS);
    if (bandwidth < d->bandwidth_hz)
        bandwidth = d->bandwidth_hz;
    omega = 2.0 * M_PI * bandwidth * dx * d->period;
    if (omega > MAX_OMEGA)
        omega = MAX_OMEGA;
    b = sqrt(2.0) * omega;
    c = omega * omega;

    d->x_ref = x;
    d->y_ref = predicted + b * error;
    d->period += c * error / dx;
    d->elapsed += dx * d->period;
    d->updates++;
    return 1;
}

int ua101_dll_locked(const ua101_dll *d)
{
    return d->updates >= 2;
}

double ua101_dll_time(const ua101_dll *d, uint64_t x)
{
    return d->y_ref + ((double)x - (double)d->x_ref) * d->period;
}

double ua101_dll_period(const ua101_dll *d)
{
    return d->period;
}

/* Stamps rejected in a row, after which the line is found again from a new batch. */
#define BUS_RESTART_AFTER 50
/* How fast each bound on the tick relaxes, in frames per second: 20 us a second, faster than the
   bus drifts from its nominal rate (1.3 ppm measured, so 1.3 us a second), slower than stamps
   near the tick come on each side (several a second). */
#define TICK_RELAX_PER_SECOND 0.02
/* Once published, the tick in use moves at most this far per stamp, in frames: 0.2 us, so the
   timestamps never jump. */
#define TICK_SLEW 0.0002
/* How far a stamp may sit outside the tick in use before it counts as disagreeing: the stamp
   pairs its two readings to within a few us, and the line wanders by a few more. */
#define TICK_TOLERANCE 0.01

void ua101_bus_clock_init(ua101_bus_clock *c, double frame_seconds, double bandwidth_hz,
                          double reject_seconds)
{
    memset(c, 0, sizeof(*c));
    ua101_dll_init(&c->dll, frame_seconds, bandwidth_hz, reject_seconds);
    /* It starts again itself, from a batch, rather than from whichever stamp came last. */
    c->dll.restart_after = UINT_MAX;
}

/*
 * Where the line goes through the batch, as a time against its first stamp. The stamps a frame
 * behind sit one frame from the rest, so folded onto one frame they all agree, and their mean
 * there gives the line's place in the frame with nothing half-way between. Of the whole frames
 * that place could then mean, it takes the one most stamps are in, the earliest if two tie.
 */
static double batch_line(const double *r, unsigned n, double frame)
{
    double sx = 0.0, sy = 0.0, phase, best = 0.0;
    unsigned i, best_count = 0;
    int m;

    for (i = 0; i < n; i++) {
        double a = 2.0 * M_PI * r[i] / frame;

        sx += cos(a);
        sy += sin(a);
    }
    phase = atan2(sy, sx) / (2.0 * M_PI) * frame;
    for (m = -3; m <= 2; m++) {
        double candidate = phase + m * frame;
        unsigned count = 0;

        for (i = 0; i < n; i++)
            count += fabs(r[i] - candidate) < 0.5 * frame;
        if (count > best_count) {
            best_count = count;
            best = candidate;
        }
    }
    return best;
}

static void forget_tick(ua101_bus_clock *c)
{
    c->tick_started = 0;
    c->tick_found = 0;
    c->tick_wait = 0;
    c->tick = 0.0;
    memset(&c->ticks, 0, sizeof(c->ticks));
}

/*
 * One stamp, as taken: bus frame `frame` was current at `time`. So that frame's tick came at or
 * before it, and the next tick after it: time - period < tick <= time. The bounds keep the
 * tightest of these, as times of the newest frame's tick, carried from frame to frame at the
 * nominal rate and each relaxing slowly. They don't use the line, so they hold however it moves
 * while it settles. A stamp that contradicts an older bound wins halfway: the bus may have
 * drifted since.
 */
static void follow_tick(ua101_bus_clock *c, uint64_t frame, double time)
{
    double period = c->dll.nominal_period, hi, lo, target;

    if (!c->tick_started) {
        c->tick_frame = frame;
        hi = time;
        lo = time - period;
    } else {
        double at;

        if (frame > c->tick_frame) {
            double ahead = (double)(frame - c->tick_frame), move = ahead * period;
            double relax = TICK_RELAX_PER_SECOND * ahead * period * period;

            c->tick_hi += move + relax;
            c->tick_lo += move - relax;
            c->tick += move;
            c->tick_frame = frame;
        }
        at = time + (double)(c->tick_frame - frame) * period; /* the stamp, at the newest frame */
        hi = fmin(c->tick_hi, at);
        lo = fmax(c->tick_lo, at - period);
        if (lo > hi)
            lo = hi = 0.5 * (lo + hi);
    }
    c->tick_lo = lo;
    c->tick_hi = hi;

    if (hi - lo <= UA101_BUS_CLOCK_TICK_FOUND * period) {
        target = 0.5 * (lo + hi);
        c->tick_found = 1;
    } else {
        /* Not pinned down now: keep the place found before, inside what the stamps allow. If
           it was never found, that's just before the earliest stamp. */
        target = c->tick_started ? fmin(fmax(c->tick, lo), hi) : hi;
    }
    if (c->settled)
        c->tick += fmin(fmax(target - c->tick, -TICK_SLEW * period), TICK_SLEW * period);
    else
        c->tick = target;
    c->tick_started = 1;
}

/* Moves the line a whole frame later (+1) or earlier (-1); the tick doesn't depend on it. */
static void move_line(ua101_bus_clock *c, int frames)
{
    c->dll.y_ref += frames * c->dll.period;
    c->moved++;
}

int ua101_bus_clock_update(ua101_bus_clock *c, uint64_t frame, double time)
{
    ua101_dll *d = &c->dll;
    double off;
    long long k;
    int used;

    if (d->updates == 0) {
        if (c->batch_count == 0) {
            c->batch_frame = frame;
            c->batch_time = time;
        } else if (frame <= c->batch_frame) {
            return 0;
        }
        c->batch[c->batch_count++] =
            time - c->batch_time - (double)(frame - c->batch_frame) * d->nominal_period;
        if (c->batch_count < UA101_BUS_CLOCK_BATCH)
            return 0;
        c->batch_count = 0;
        c->starts++;
        return ua101_dll_update(d, c->batch_frame,
                                c->batch_time + batch_line(c->batch, UA101_BUS_CLOCK_BATCH,
                                                           d->nominal_period));
    }

    /* Whether the clock's ticks, as they stand, explain this stamp: its time at or after the
       tick of the frame it names, and before the next. */
    if (c->settled) {
        double at = (time - ua101_bus_clock_time(c, (double)frame)) / ua101_bus_clock_period(c);

        if (at < -TICK_TOLERANCE || at >= 1.0 + TICK_TOLERANCE)
            c->tick_disagree++;
    }

    /* More than half a frame off the line: the neighbouring frame, as the counter lags. */
    follow_tick(c, frame, time);
    off = (time - ua101_dll_time(d, frame)) / d->period;
    k = llround(off);
    if (k != 0 && k >= -2 && k <= 2 && (k > 0 || frame >= (uint64_t)(-k))) {
        frame = (uint64_t)((long long)frame + k);
        c->corrected++;
    }
    used = ua101_dll_update(d, frame, time);
    if (c->checked < UA101_BUS_CLOCK_CHECK && used) {
        c->balance += k >= -2 && k <= 2 ? k : 0;
        if (++c->checked == UA101_BUS_CLOCK_CHECK) {
            if (2 * c->balance <= -(long)UA101_BUS_CLOCK_CHECK)
                move_line(c, -1); /* the line was a frame late */
            else if (2 * c->balance >= (long)UA101_BUS_CLOCK_CHECK)
                move_line(c, 1);
        }
    }
    if (!c->settled && c->checked >= UA101_BUS_CLOCK_CHECK &&
        (c->tick_found || ++c->tick_wait >= UA101_BUS_CLOCK_TICK_WAIT)) {
        c->settled = 1;
        /* From the nominal rate, not the line's: that swings while the line settles. */
        ua101_dll_init(&c->ticks, d->nominal_period, d->bandwidth_hz, 0.0);
    }
    if (c->settled)
        ua101_dll_update(&c->ticks, c->tick_frame, c->tick);
    if (d->in_a_row >= BUS_RESTART_AFTER) {
        double nominal = d->nominal_period, bandwidth = d->bandwidth_hz;
        double reject = d->reject_seconds;
        unsigned long rejected = d->rejected, restarts = d->restarts + 1;

        ua101_dll_init(d, nominal, bandwidth, reject);
        d->restart_after = UINT_MAX;
        d->rejected = rejected;
        d->restarts = restarts;
        c->checked = 0;
        c->balance = 0;
        c->settled = 0;
        forget_tick(c);
    }
    return used;
}

int ua101_bus_clock_settled(const ua101_bus_clock *c)
{
    return c->settled;
}

double ua101_bus_clock_time(const ua101_bus_clock *c, double frame)
{
    const ua101_dll *d = &c->dll;

    if (ua101_dll_locked(&c->ticks))
        return c->ticks.y_ref + (frame - (double)c->ticks.x_ref) * c->ticks.period;
    if (c->tick_started)
        return c->tick + (frame - (double)c->tick_frame) * d->nominal_period;
    return d->y_ref + (frame - (double)d->x_ref) * d->period;
}

double ua101_bus_clock_period(const ua101_bus_clock *c)
{
    return ua101_dll_locked(&c->ticks) ? c->ticks.period : c->dll.period;
}
