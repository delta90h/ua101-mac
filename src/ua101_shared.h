/*
 * The memory the helper (ua101d) and the audio plug-in share, in one block: a header, the
 * clock, a ring of capture frames and a ring of playback frames. The helper writes capture and
 * the clock and reads playback; the plug-in does the opposite. Neither ever waits for the other.
 *
 * Both processes map the block at different addresses, so it holds no pointers. Contains no
 * Apple APIs; host times are in mach_absolute_time() ticks, passed in by the caller.
 *
 * Sample time is the capture timeline's frame number (src/ua101_timeline.h): it keeps counting
 * through a stall, so the plug-in's timestamps don't jump when the USB side recovers. The clock
 * is a straight line from sample time to host time, which the helper republishes as its clock
 * loops follow the unit's drift. The plug-in turns it into the zero timestamps Core Audio asks
 * for.
 */
#ifndef UA101_SHARED_H
#define UA101_SHARED_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "ua101_framering.h"

#define UA101_SHARED_MAGIC 0x31303155u /* "U101" in memory */
/* 2 since 2026-11-19: the helper's counts grew by playback's spare room and late frames. */
#define UA101_SHARED_VERSION 2

/* What the helper is doing, for the plug-in and, later, the control app. */
enum {
    UA101_STATE_STOPPED = 0,
    UA101_STATE_STARTING = 1, /* streaming, but the clock isn't published yet */
    UA101_STATE_RUNNING = 2,
    UA101_STATE_FAILED = 3,
};

/*
 * The clock, published with a sequence lock: the writer makes seq odd, changes the line and
 * makes it even again, and a reader retries if seq was odd or changed while it read. Every
 * field is an atomic, so a reader that races the writer reads a stale value, never a torn one.
 */
typedef struct {
    _Atomic uint32_t seq;
    _Atomic uint32_t seed;      /* changes whenever sample time starts again from 0 */
    _Atomic uint64_t sample;    /* a sample time on the line */
    _Atomic uint64_t host_bits; /* its host time, in ticks, as the bits of a double */
    _Atomic uint64_t ticks_per_sample_bits; /* the line's slope, as the bits of a double */
} ua101_shared_clock;

/* A copy of the line, as one read gave it. */
typedef struct {
    uint32_t seed;
    uint64_t sample;
    double host;
    double ticks_per_sample;
} ua101_clock_line;

/* Counted by the helper, for reports. Only the helper writes them. */
typedef struct {
    _Atomic uint64_t capture_frames;   /* written to the capture ring */
    _Atomic uint64_t capture_gaps;     /* times the timeline found frames missing */
    _Atomic uint64_t capture_gap_frames;
    _Atomic uint64_t playback_frames;  /* sent */
    _Atomic uint64_t playback_missing; /* sent as silence: not written by the plug-in in time */
    _Atomic uint64_t playback_gaps;
    _Atomic uint64_t resyncs;
    _Atomic uint64_t errors; /* transfer, packet and submit errors, both ways */
    _Atomic int64_t playback_lead_min; /* frames ahead of sample time when playback was read */
    _Atomic int64_t playback_lead_max;
    /* The least the playback ring held beyond a transfer's last frame, in frames, in this run
       (src/ua101_drvset.h, "spare room"); INT64_MAX while nothing is measured. And the frames
       sent as silence between the first and the last transfer the plug-in filled completely:
       the run's "late" frames so far (src/ua101_engine.h). */
    _Atomic int64_t playback_spare_min;
    _Atomic uint64_t playback_late;
} ua101_shared_stats;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint64_t bytes; /* the whole block */
    double rate_hz;
    uint32_t capture_channels, playback_channels; /* 4-byte samples, 24 bits valid */
    uint32_t capture_frame_bytes, playback_frame_bytes;
    uint64_t ring_frames; /* each ring's capacity, a power of two */

    _Atomic uint32_t state;
    _Atomic uint32_t heartbeat; /* bumped by the helper about every 10 ms while it streams */

    ua101_shared_clock clock;
    ua101_shared_stats stats;

    /* Each ring finds its frames by their distance from itself, so these work in any mapping. */
    ua101_frame_ring capture;  /* helper writes, plug-in reads */
    ua101_frame_ring playback; /* plug-in writes, helper reads */
} ua101_shared;

/* Each ring's capacity in frames at a rate: the smallest power of two holding 0.5 s. */
uint64_t ua101_shared_ring_frames(double rate_hz);

/* Bytes needed for a block, with each ring's frames page-aligned after the header. */
size_t ua101_shared_bytes(uint32_t capture_frame_bytes, uint32_t playback_frame_bytes,
                          uint64_t ring_frames, size_t page_bytes);

/* Sets up a zeroed block of `bytes`. Returns 0 if the sizes don't fit or aren't valid. */
int ua101_shared_init(ua101_shared *s, size_t bytes, double rate_hz, uint32_t capture_channels,
                      uint32_t playback_channels, uint64_t ring_frames, size_t page_bytes);

/* For the side that maps a block it didn't set up: whether it looks right and fits in `bytes`.
   Returns 0 if not, so nothing in it is used. */
int ua101_shared_check(const ua101_shared *s, size_t bytes);

/*
 * For streaming: each side's own views of the two rings (src/ua101_framering.h), checked
 * against what that side knows for itself: the block's real size, and the channel counts it
 * was told or read from the unit. Every field is read once, so the other process can't change
 * one between the check and its use. A channel count of 0 leaves that ring out. Returns 0, and
 * nothing may be used, if anything doesn't fit.
 */
int ua101_shared_views(ua101_shared *s, size_t bytes, uint32_t capture_channels,
                       uint32_t playback_channels, ua101_frame_view *capture,
                       ua101_frame_view *playback);

/*
 * The safety offsets Core Audio is given, as times, so they hold at every rate. What decides
 * them is time: a transfer is 2 ms long at every rate, so capture's newest frame is up to about
 * 2.2 ms behind (measured at 48 kHz: 105 frames), and the helper reads playback about 6 ms
 * ahead of sample time (measured at 48 kHz: 291 frames at most, with a queue of 4).
 * Both are 8 ms since 1.1 (2026-11-27, the owner's choice after the tests at the box on
 * 2026-11-20, src/ua101_drvset.h): 384 frames at 48 kHz. Until 1.1 they were 5 ms and
 * 21.3 ms (what 1024 frames were at 48 kHz). They are the defaults of the driver's settings
 * too: the plug-in uses them while the user has set nothing.
 */
#define UA101_INPUT_OFFSET_SECONDS 0.008
#define UA101_OUTPUT_OFFSET_SECONDS 0.008

/* A time as frames at a rate, rounded up: 8 ms is 384 frames at 48 kHz, 353 at 44.1 kHz and
   1536 at 192 kHz. */
uint32_t ua101_offset_frames(double seconds, double rate_hz);

/* Writer: publishes a new line. The first one, and every one after ua101_shared_new_seed,
   carries the new seed. */
void ua101_shared_publish(ua101_shared_clock *c, uint64_t sample, double host,
                          double ticks_per_sample);
/* Writer: sample time starts again from 0, as on each start. Clears the line until the next
   publish. */
void ua101_shared_new_seed(ua101_shared_clock *c);

/* Reader: the latest line. Returns 0 if there's none yet, or if the writer kept changing it
   (it's tried a few times, never waiting). */
int ua101_shared_read(const ua101_shared_clock *c, ua101_clock_line *line);

/* On a line: the host time of a sample time, and the sample time at a host time. */
double ua101_line_host(const ua101_clock_line *l, double sample);
double ua101_line_sample(const ua101_clock_line *l, double host);

/*
 * For Core Audio's GetZeroTimeStamp: the latest sample time at or before host time `now` that
 * is a whole number of periods, and its host time. Returns 0 if there's no line yet.
 */
int ua101_zero_timestamp(const ua101_clock_line *l, uint64_t now, uint32_t period,
                         double *sample_out, uint64_t *host_out);

#endif
