/*
 * Where each transfer's audio belongs in time, for the driver. A transfer starts at a USB bus
 * frame, and in a steady stream its first audio frame follows on from the last transfer's. After
 * a stall the stream starts again at a later bus frame, and the audio the unit made in between
 * was never asked for. Here those frames are counted from the clock loop (frames against bus
 * time), so the audio after the gap keeps its place in time instead of moving earlier: written to
 * the frame ring at these numbers, the gap reads as silence where it really was.
 *
 * One per pipe, used from one thread. Contains no Apple APIs and allocates nothing.
 */
#ifndef UA101_TIMELINE_H
#define UA101_TIMELINE_H

#include <stddef.h>
#include <stdint.h>

#include "ua101_clock.h"

/* The clock loop's working bandwidth, as tested in tests/test_clock.c. */
#define UA101_TIMELINE_BANDWIDTH_HZ 0.1

typedef struct {
    ua101_dll clock; /* frame numbers against bus time, in seconds from the first transfer */
    double bus_frame_seconds;
    double nominal_rate_hz;
    int started;
    uint64_t first_bus;
    uint64_t next_frame; /* where the next transfer's first frame goes, if it follows on */
    uint64_t next_bus;   /* the bus frame it would start at */

    /* Counted. */
    uint64_t gaps;            /* transfers that started later than the last one ended */
    uint64_t gap_bus_frames;  /* bus frames in those gaps */
    uint64_t gap_frames;      /* audio frames put in them, estimated */
    uint64_t early_gaps;      /* estimated from the nominal rate, before the loop had a line */
    uint64_t overlaps;        /* transfers that started before the last one ended: a bug */
    double last_gap_estimate; /* the last gap's frames before rounding, for checking */
} ua101_timeline;

/* bus_frame_seconds is 0.001 for the bus frame numbers macOS gives, even on high-speed USB. */
void ua101_timeline_init(ua101_timeline *t, double nominal_rate_hz, double bus_frame_seconds);

/*
 * One transfer, in the order they ran: the bus frame it started at, how many bus frames it
 * spans, and how many audio frames it carried. Returns the frame number of its first frame; the
 * first transfer's is 0. Leave out a transfer that failed, or that had any damaged packet, and
 * the next one finds the gap: that keeps time, where counting its frames as missing wouldn't.
 */
uint64_t ua101_timeline_place(ua101_timeline *t, uint64_t bus_frame, uint32_t bus_frames,
                              uint32_t frames);

/*
 * For checking a gap afterwards, with numbers that don't come from the clock loop: transfers'
 * bus frames and first frame numbers, the first `before` of them before one gap and the rest
 * after it. Fits a straight line to the ones before and returns how far, in frames, the ones
 * after sit from it on average. Near 0 means the gap was placed right; a gap that closed up
 * shows as minus the frames that were lost.
 */
double ua101_timeline_gap_offset(const uint64_t *bus, const uint64_t *frame, size_t before,
                                 size_t count);

#endif
