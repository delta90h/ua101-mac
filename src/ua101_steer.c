#include "ua101_steer.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* A slope further than this from one frame per frame isn't a clock: it starts over. */
#define MAX_RATIO_OFF 0.01

void ua101_steer_init(ua101_steer *s, double rate, double lost_frames)
{
    memset(s, 0, sizeof(*s));
    s->rate = rate;
    s->lost = lost_frames;
    s->ratio = 1.0;
}

void ua101_steer_restart(ua101_steer *s)
{
    s->on = 0;
}

/* Keeps the remainder between 0 and 1, moving whole frames into the count. */
static void set_point(ua101_steer *s, int64_t time, int64_t frame, double part)
{
    double whole = floor(part);

    s->time = time;
    s->frame = frame + (int64_t)whole;
    s->part = part - whole;
}

static void take(ua101_steer *s, int64_t now, int64_t frame, double part, double ratio)
{
    set_point(s, now, frame, part);
    s->ratio = ratio;
    s->seconds = 0.0;
    s->offset = 0.0;
    s->on = 1;
    s->starts++;
}

int ua101_steer_update(ua101_steer *s, int64_t now, int64_t frame, double part, double ratio)
{
    double dt, predicted, error, hz, keep;

    if (!(ratio > 1.0 - MAX_RATIO_OFF && ratio < 1.0 + MAX_RATIO_OFF) || !isfinite(part))
        return 0; /* not a line: keep what we have */
    if (!s->on) {
        take(s, now, frame, part, ratio);
        return 1;
    }
    if (now <= s->time)
        return 0;
    dt = (double)(now - s->time);
    predicted = s->part + dt * s->ratio;
    error = (double)(frame - s->frame) + part - predicted;
    if (!(fabs(error) <= s->lost)) {
        take(s, now, frame, part, ratio);
        return 1;
    }

    /* A critically damped second-order loop, with its gains worked out for this reading's
       interval, so the buffer size doesn't change how it behaves. */
    hz = UA101_STEER_START_HZ / (1.0 + s->seconds / UA101_STEER_NARROW_SECONDS);
    if (hz < UA101_STEER_HZ)
        hz = UA101_STEER_HZ;
    keep = exp(-2.0 * M_PI * hz * dt / s->rate);
    s->ratio += (1.0 - keep) * (1.0 - keep) * error / dt;
    if (!(s->ratio > 1.0 - MAX_RATIO_OFF && s->ratio < 1.0 + MAX_RATIO_OFF)) {
        take(s, now, frame, part, ratio);
        return 1;
    }
    set_point(s, now, s->frame, predicted + (1.0 - keep * keep) * error);
    s->seconds += dt / s->rate;
    s->offset = error;
    return 0;
}

double ua101_steer_frame(const ua101_steer *s, int64_t t, int64_t base)
{
    return (double)(s->frame - base) + (s->part + (double)(t - s->time) * s->ratio);
}

double ua101_steer_time(const ua101_steer *s, int64_t unit, int64_t base)
{
    return (double)(s->time - base) + ((double)(unit - s->frame) - s->part) / s->ratio;
}
