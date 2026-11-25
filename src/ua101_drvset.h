/*
 * The driver's own settings (2026-11-19): the two delays that trade speed against
 * safety, and how the room they leave is measured. No Apple APIs, so it is tested by itself
 * (tests/test_drvset.c). The plug-in (src/plugin/ua101_plugin.c) holds the settings, as device
 * properties kept in Core Audio's storage; the control app and build/ua101drv set them through
 * Core Audio (src/ua101_drvclient.c). Nothing here goes to the unit.
 *
 * THE TWO DELAYS are the safety offsets Core Audio is given (src/ua101_shared.h), as times:
 *   Recording margin: how far behind the unit a cycle reads recording. A USB transfer is 2 ms
 *     long and arrives in one piece at its end, so the newest frame is up to about 2.2 ms old
 *     (measured at 48 kHz), more when a transfer is late. Under that, recording has holes.
 *   Playback delay: how far ahead of the unit a cycle must write playback. The helper fills a
 *     transfer whose first frame is 5.4 to 7.0 ms ahead of sample time (measured at 48 kHz, a
 *     queue of 4) and whose last is 2 ms later, so it needs about 9 ms; the rest is spare for a
 *     cycle that wakes late.
 * Only the values in the two lists below are ever used: the plug-in refuses any other.
 *
 * BELOW THE RECOMMENDED VALUES (2026-11-20, a design choice: the whole scale, down to where
 * the sound breaks, to try by ear). Each list starts with values under what the two figures
 * above allow. The control app and build/ua101drv give every value a plain note of what it
 * needs or does, not a warning.
 *   Recording margin 1 and 2 ms: the newest frame is often older than that, so those cycles
 *     get silence for the frames that haven't arrived. What is recorded then has gaps.
 *   Playback delay 2 to 8 ms: too little for the helper on its own. Sound only arrives in
 *     time when the app finishes its buffer early enough to make up the difference (the room
 *     is the delay less about 8 ms, plus the unused part of the app's buffer); otherwise the
 *     helper sends silence for the late frames. So these move safety from the driver to the
 *     app's buffer size; they don't take the helper's need away. Each is offered with the
 *     shortest buffer that keeps it clean (below).
 * Nothing is harmed by them but the sound: missing and late frames are silence, and counted.
 *
 * MEASURED AT THE BOX (2026-11-20, link 16, 48 kHz, Logic Pro, the user playing): at Logic's
 * buffer of 128 frames the least room was the delay less 5.5 ms: 10 ms +4.5 ms, 8 ms +2.5 ms,
 * 6 ms +0.5 ms, 4 ms -1.4 ms (late frames, heard as distortion). At 512 frames 4 ms left
 * +5.4 ms and 2 ms +3.2 ms. While the Mac built the project, 8 ms at 128 fell from +2.5 to
 * +1.5 ms (nothing late). So room = delay + buffer - need, where the need was 8.2 ms at 128
 * frames and 9.4 ms at 512 (the app takes longer over a bigger buffer), taken as a straight
 * line between and beyond. So each playback delay under 10 ms is
 * offered with the shortest buffer (32 to 1024 frames, as music apps offer them) that leaves
 * at least 2 ms by that line: 1 ms, and the 1.1 ms a busy Mac took away. 10 ms and over
 * leave that at any buffer. 8 ms at 64 frames would leave 1.4 ms: not offered, and not
 * measured. Recording: 3 ms left +0.8 ms at its worst, 2 ms missed 5 frames in 4 minutes,
 * 1 ms missed 2% of a take; the buffer doesn't help there.
 * Zero is not offered: the plug-in and the link use 0 for "not set".
 *
 * SPARE ROOM is what was left of a delay at the worst moment of a run, in frames:
 *   recording: the capture ring's newest frame, less the last frame a cycle wanted;
 *   playback: the playback ring's newest frame, less the last frame a transfer wanted.
 * Zero or more means everything was there. Negative means frames were missed, by that many.
 */
#ifndef UA101_DRVSET_H
#define UA101_DRVSET_H

#include <stdint.h>

#include "ua101_shared.h"

/* The device the plug-in publishes, and its settings as properties of that device (global
   scope, main element). Each margin is a CFNumber, milliseconds; the statistics are a
   CFDictionary that can only be read. Rate correction ('UArc', a CFBoolean) is older. */
#define UA101_DEVICE_UID "io.github.delta90h.ua101:UA-101"
#define UA101_PROPERTY_RATE_CORRECTION 0x55417263u /* 'UArc' */
#define UA101_PROPERTY_INPUT_MARGIN 0x5541696Du    /* 'UAim' */
#define UA101_PROPERTY_OUTPUT_DELAY 0x55416F64u    /* 'UAod' */
#define UA101_PROPERTY_STATS 0x55417374u           /* 'UAst' */

/* Where Core Audio keeps the two for the plug-in, as numbers of milliseconds. */
#define UA101_INPUT_MARGIN_KEY "recording margin ms"
#define UA101_OUTPUT_DELAY_KEY "playback delay ms"

/* The statistics dictionary's keys. Frames are frames at "rate". The two spares are left out
   when nothing was measured; everything but "running" is left out before the first run. */
#define UA101_STAT_RUNNING "running"               /* bool: I/O runs now; else the last run's */
#define UA101_STAT_RATE "rate"                     /* number, Hz */
#define UA101_STAT_SECONDS "seconds"               /* number: how long the run has lasted, or did */
#define UA101_STAT_INPUT_OFFSET "input offset"     /* number, frames: the run's safety offsets */
#define UA101_STAT_OUTPUT_OFFSET "output offset"
#define UA101_STAT_CAPTURE_SPARE "capture spare"   /* number, frames; negative: missed */
#define UA101_STAT_CAPTURE_MISSING "capture missing" /* number, frames a cycle wanted and didn't get */
#define UA101_STAT_PLAYBACK_SPARE "playback spare" /* number, frames; negative: late */
#define UA101_STAT_PLAYBACK_LATE "playback late"   /* number, frames sent as silence instead */

/* The values offered, in seconds, shortest first. The defaults are the ones in use since
   2026-11-10: 5 ms (a design choice that day) and what 1024 frames were at 48 kHz.
   RECOMMENDED is the place of the shortest value that is recommended whatever the music app's
   buffer: the ones before it are offered with a warning (see above). For playback the warning
   names the buffer each needs (ua101_output_min_buffers); for recording no buffer helps. */
#define UA101_INPUT_MARGIN_COUNT 6
#define UA101_OUTPUT_DELAY_COUNT 9
#define UA101_INPUT_MARGIN_DEFAULT 3
#define UA101_OUTPUT_DELAY_DEFAULT 8
#define UA101_INPUT_MARGIN_RECOMMENDED 2
#define UA101_OUTPUT_DELAY_RECOMMENDED 4

static const double ua101_input_margins[UA101_INPUT_MARGIN_COUNT] = {
    0.001, 0.002, 0.003, UA101_INPUT_OFFSET_SECONDS, 0.008, 0.010};
static const double ua101_output_delays[UA101_OUTPUT_DELAY_COUNT] = {
    0.002, 0.004, 0.006, 0.008, 0.010, 0.012, 0.014, 0.016, UA101_OUTPUT_OFFSET_SECONDS};

/* The shortest buffer a music app needs for each playback delay (see "Measured at the box"):
   512, 512, 256 and 128 frames at 48 kHz, kept as times so that another rate needs the same
   time. 0: any buffer will do. */
static const double ua101_output_min_buffers[UA101_OUTPUT_DELAY_COUNT] = {
    512.0 / 48000.0, 512.0 / 48000.0, 256.0 / 48000.0, 128.0 / 48000.0, 0.0, 0.0, 0.0, 0.0, 0.0};

/* That buffer in frames at `rate`, as music apps offer it: the shortest power of two from 32
   frames up that is at least as long (to within half a frame). 0 when any buffer will do, or
   for a place or a rate that isn't one. */
static inline unsigned ua101_output_min_buffer_frames(int index, double rate)
{
    double need;
    unsigned frames = 32;

    if (index < 0 || index >= UA101_OUTPUT_DELAY_COUNT || !(rate > 0.0) ||
        !(ua101_output_min_buffers[index] > 0.0))
        return 0;
    need = ua101_output_min_buffers[index] * rate - 0.5;
    while ((double)frames < need && frames < 8192)
        frames *= 2;
    return frames;
}

/* Which value of a list a number of milliseconds names: within 0.05 ms of it, so 21.3 and
   21.33 both name the last playback delay. -1 if none, or if `ms` isn't a number. */
static inline int ua101_drvset_index(const double *list, int count, double ms)
{
    int i;

    for (i = 0; i < count; i++) {
        double off = ms - list[i] * 1000.0;

        if (off > -0.05 && off < 0.05)
            return i;
    }
    return -1;
}

static inline int ua101_input_margin_index(double ms)
{
    return ua101_drvset_index(ua101_input_margins, UA101_INPUT_MARGIN_COUNT, ms);
}

static inline int ua101_output_delay_index(double ms)
{
    return ua101_drvset_index(ua101_output_delays, UA101_OUTPUT_DELAY_COUNT, ms);
}

/*
 * One read's spare room. `end` is the ring's end (one past its newest frame) as read BEFORE the
 * copy, `need_end` one past the last frame wanted, and `missed` what the copy then couldn't
 * give. The writer may finish a write between the two, so they are made to agree: a read that
 * got everything has no negative spare, and one that missed frames has no spare left.
 */
static inline int64_t ua101_spare_of(uint64_t end, uint64_t need_end, uint64_t missed)
{
    int64_t spare = (int64_t)end - (int64_t)need_end;

    if (missed == 0 && spare < 0)
        return 0;
    if (missed != 0 && spare >= 0)
        return -(int64_t)(missed > (uint64_t)INT64_MAX ? (uint64_t)INT64_MAX : missed);
    return spare;
}

/*
 * The smallest spare room of a run, for a reader that goes on reading after its writer has
 * stopped: the helper sends playback for a moment after the app's last cycle, and runs the
 * ring dry, which is no fault. So a read's spare counts only once the writer has written
 * again: `waiting` is the smallest since the ring's end last moved, and joins `least` when the
 * end moves. What is still waiting when the run ends was read after the last write, and is
 * dropped. Nothing counts before the first read that got every frame either: until then the
 * writer hasn't reached the reader's place (the app's first cycle writes well ahead of where
 * the helper is reading), which is the same rule as for "late" frames (src/ua101_engine.h).
 */
typedef struct {
    uint64_t last_end;
    int begun; /* a read has got every frame */
    int have_waiting, have_least;
    int64_t waiting, least;
} ua101_spare;

static inline void ua101_spare_reset(ua101_spare *s)
{
    s->last_end = 0;
    s->begun = 0;
    s->have_waiting = s->have_least = 0;
    s->waiting = s->least = 0;
}

/* Returns 1 when `least` changed. */
static inline int ua101_spare_note(ua101_spare *s, uint64_t end, uint64_t need_end, uint64_t missed)
{
    int64_t spare;
    int changed = 0;

    if (!s->begun) {
        if (missed != 0 || end == 0)
            return 0;
        s->begun = 1;
    }
    if (end != s->last_end) {
        if (s->have_waiting && (!s->have_least || s->waiting < s->least)) {
            s->least = s->waiting;
            s->have_least = 1;
            changed = 1;
        }
        s->have_waiting = 0;
        s->last_end = end;
    }
    spare = ua101_spare_of(end, need_end, missed);
    if (!s->have_waiting || spare < s->waiting) {
        s->waiting = spare;
        s->have_waiting = 1;
    }
    return changed;
}

/* "Nothing measured" where a spare travels as one number (the shared block, an atomic). */
#define UA101_SPARE_NONE INT64_MAX

#endif
