#include "ua101_timeline.h"

#include <math.h>
#include <string.h>

void ua101_timeline_init(ua101_timeline *t, double nominal_rate_hz, double bus_frame_seconds)
{
    memset(t, 0, sizeof(*t));
    t->nominal_rate_hz = nominal_rate_hz;
    t->bus_frame_seconds = bus_frame_seconds;
    ua101_dll_init(&t->clock, 1.0 / nominal_rate_hz, UA101_TIMELINE_BANDWIDTH_HZ, 0.0);
}

/*
 * The frames the unit made from the expected start to this one. The unit makes whole frames, so
 * a transfer at time y begins at frame floor(rate * y + phase). The loop's line runs through the
 * middle of those steps, half a frame below, so rounding its estimate gives the frame itself.
 */
static uint64_t frames_in_gap(ua101_timeline *t, uint64_t bus_frame)
{
    double extra;

    if (ua101_dll_locked(&t->clock)) {
        double y = (double)(bus_frame - t->first_bus) * t->bus_frame_seconds;

        extra = (y - ua101_dll_time(&t->clock, t->next_frame)) / ua101_dll_period(&t->clock);
    } else {
        extra = (double)(bus_frame - t->next_bus) * t->bus_frame_seconds * t->nominal_rate_hz;
        t->early_gaps++;
    }
    t->last_gap_estimate = extra;
    return extra > 0.0 ? (uint64_t)llround(extra) : 0;
}

uint64_t ua101_timeline_place(ua101_timeline *t, uint64_t bus_frame, uint32_t bus_frames,
                              uint32_t frames)
{
    uint64_t first;

    if (!t->started) {
        t->started = 1;
        t->first_bus = bus_frame;
        first = 0;
    } else if (bus_frame == t->next_bus) {
        first = t->next_frame;
    } else if (bus_frame > t->next_bus) {
        uint64_t lost = frames_in_gap(t, bus_frame);

        t->gaps++;
        t->gap_bus_frames += bus_frame - t->next_bus;
        t->gap_frames += lost;
        first = t->next_frame + lost;
    } else {
        /* Bus frames can't be used twice, so this is a caller's mistake. Never go back, and
           leave the clock loop alone. */
        t->overlaps++;
        first = t->next_frame;
        t->next_frame += frames;
        if (bus_frame + bus_frames > t->next_bus)
            t->next_bus = bus_frame + bus_frames;
        return first;
    }

    ua101_dll_update(&t->clock, first, (double)(bus_frame - t->first_bus) * t->bus_frame_seconds);
    t->next_frame = first + frames;
    t->next_bus = bus_frame + bus_frames;
    return first;
}

double ua101_timeline_gap_offset(const uint64_t *bus, const uint64_t *frame, size_t before,
                                 size_t count)
{
    double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0, slope, intercept, n, sum = 0.0;
    size_t i;

    if (before < 2 || count <= before)
        return NAN;
    /* Relative to the first one, so the sums keep their precision. */
    for (i = 0; i < before; i++) {
        double x = (double)(bus[i] - bus[0]);
        double y = (double)(frame[i] - frame[0]);

        sx += x;
        sy += y;
        sxx += x * x;
        sxy += x * y;
    }
    n = (double)before;
    slope = (n * sxy - sx * sy) / (n * sxx - sx * sx);
    intercept = (sy - slope * sx) / n;
    for (i = before; i < count; i++)
        sum += (double)(frame[i] - frame[0]) - (intercept + slope * (double)(bus[i] - bus[0]));
    return sum / (double)(count - before);
}
