/*
 * Steering for the plug-in's rate correction: a smoothed copy of the helper's clock line.
 * Contains no Apple APIs, and nothing here allocates, locks or waits.
 *
 * The helper's line says which of the unit's frames belongs at each moment of our own
 * timeline. It is rewritten every millisecond and carries a small ripple (about 65 ns rms from
 * one I/O cycle to the next, measured): the unit's frames counted against USB's milliseconds
 * make a staircase that repeats 5.8 times a second at 48 kHz, and the helper's clock loop lets
 * a little of it through. Followed exactly, the ripple becomes pitch wobble: 6.3 parts per
 * million. So the resamplers aim at this copy instead: a line of its own, pulled toward the
 * helper's by a slow, critically damped loop that takes out most of the ripple (0.5 ppm is
 * left, measured on the unit) and still keeps up with real changes: the unit's rate against
 * the Mac's clock moves by several ppm within a minute when the Mac warms or cools. Capture and
 * playback both aim at the same copy, so they stay locked to each other.
 *
 * Its point is kept as whole numbers plus a small remainder, so nothing is lost to rounding
 * however long it runs: a millionth of a frame matters at 15 kHz.
 */
#ifndef UA101_STEER_H
#define UA101_STEER_H

#include <stdint.h>

/* The loop's working frequency. It starts UA101_STEER_START_HZ wide, while the helper's own
   clock is still settling, and narrows as start / (1 + seconds / UA101_STEER_NARROW_SECONDS). */
#define UA101_STEER_HZ 0.2
#define UA101_STEER_START_HZ 2.0
#define UA101_STEER_NARROW_SECONDS 1.0

typedef struct {
    double rate;   /* our frames per second, for the loop's timing */
    double lost;   /* further off the helper's line than this, in frames, it starts over */
    int on;        /* whether it has a line */
    int64_t time;  /* its point: a sample time of ours */
    int64_t frame; /* the unit frame there, whole part */
    double part;   /* and the rest, 0 up to 1 */
    double ratio;  /* unit frames per frame of ours */
    double seconds; /* followed since it last started */
    /* For reports. */
    double offset;   /* the helper's line less this one at the last reading, in unit frames */
    uint64_t starts; /* times it took the helper's line as it stood, the first included */
} ua101_steer;

/* Without a line: the first reading gives it one. */
void ua101_steer_init(ua101_steer *s, double rate, double lost_frames);

/*
 * One reading of the helper's line, at our sample time `now`: the unit frame there is
 * frame + part (part may be any size; the caller keeps frame near it so nothing is rounded
 * away), and the line's slope is `ratio` unit frames per frame of ours. Readings at or before
 * the last one's time change nothing, so capture and playback can both call it each cycle.
 * Returns 1 if it took the helper's line as it stood: the first reading, one more than `lost`
 * frames off, or after ua101_steer_restart. The resamplers then start again in place.
 */
int ua101_steer_update(ua101_steer *s, int64_t now, int64_t frame, double part, double ratio);

/* The next reading is taken as it stands: for a new seed, when sample time starts again. */
void ua101_steer_restart(ua101_steer *s);

/* The unit frame at our sample time `t`, less `base`; and our sample time at unit frame
   `unit`, less `base`. The bases keep the results small enough to be exact. */
double ua101_steer_frame(const ua101_steer *s, int64_t t, int64_t base);
double ua101_steer_time(const ua101_steer *s, int64_t unit, int64_t base);

#endif
