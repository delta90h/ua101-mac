/*
 * The helper's USB engine: streams capture and playback with the unit, on a real-time thread,
 * through the shared block (src/ua101_shared.h). Capture is written into the capture ring at its
 * timeline numbers, which are the sample times, and the clock line is republished as it goes.
 * Playback is read from the playback ring by sample time, so a frame the plug-in hasn't written
 * goes out as silence, never as stale audio.
 *
 * Built from ua101stream's submit, resync, size FIFO and layout code, which ran for days on the
 * unit at all five rates, without the test instrumentation. ua101stream stays the test tool.
 *
 * Opening it opens the capture and playback interfaces only, so another program can own MIDI.
 */
#ifndef UA101_ENGINE_H
#define UA101_ENGINE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "ua101_shared.h"

/* A design choice for capture (2026-11-09). Playback's depth is still to be chosen. */
#define UA101_ENGINE_CAPTURE_QUEUE 8
#define UA101_ENGINE_PLAYBACK_QUEUE 4

typedef struct {
    unsigned capture_queue;  /* transfers of 2 ms queued; 0 for the default */
    unsigned playback_queue;
    int capture_only;        /* never opens the playback interface */
} ua101_engine_options;

/* What the unit streams at its current rate, for sizing the shared block. */
typedef struct {
    double rate_hz;
    uint32_t capture_channels;
    uint32_t playback_channels; /* 0 when capture_only */
} ua101_engine_format;

typedef struct ua101_engine ua101_engine;

/* Finds the unit, opens its streaming interfaces and checks the bus. Returns NULL with the
   reason in `why` if it can't. Prints a line for each step to `log`, which may be NULL. */
ua101_engine *ua101_engine_open(const ua101_engine_options *opt, ua101_engine_format *format,
                                FILE *log, char *why, size_t why_bytes);

/* Starts streaming into `shared`, a block of `shared_bytes` set up for this format. Sample time
   starts again from 0 with a new seed. Returns 0 if the block doesn't fit the format (the
   plug-in can write to it too, so it is checked again here) or the thread couldn't start. */
int ua101_engine_start(ua101_engine *e, ua101_shared *shared, size_t shared_bytes);

/* Why streaming stopped by itself, or NULL while it's fine. */
const char *ua101_engine_failure(const ua101_engine *e);

/* Stops streaming and waits for the thread. Safe to call more than once. */
void ua101_engine_stop(ua101_engine *e);

/* Prints the detailed counts from the last run: errors, gaps, timing. */
void ua101_engine_report(const ua101_engine *e, FILE *out);

/* For checking the bus clock: the raw stamps from the first transfers of the next run, each the
   bus frame and host time the USB stack paired, and the host time it was asked. Set before
   ua101_engine_start. */
typedef struct {
    uint64_t bus, stamp, asked;
} ua101_bus_stamp;

int ua101_engine_keep_stamps(ua101_engine *e, uint32_t capacity);
const ua101_bus_stamp *ua101_engine_stamps(const ua101_engine *e, uint32_t *count);

/*
 * The last run in a few numbers, for a verdict in the log: what a real app's session lost.
 * Playback frames sent as silence count as late only between the first and the last transfer
 * the app's frames filled completely: before the first the app hadn't started writing, and
 * after the last it had stopped. The clock is the unit's rate against the Mac's clock as an
 * average over the run, from the first line published after the loops settled to the last;
 * clock_seconds is 0 when the run was too short for one. Playback's spare room is the least the
 * playback ring held beyond a transfer's last frame while the app was writing
 * (src/ua101_drvset.h); negative when frames were late, and not known when the app never wrote.
 */
typedef struct {
    double seconds;
    uint64_t capture_frames, capture_lost_frames, capture_gaps;
    uint64_t playback_frames, playback_late_frames;
    int playback_spare_known;
    int64_t playback_spare_frames;
    double rate_hz;
    uint64_t errors, resyncs;
    double clock_seconds, clock_ppm; /* against the Mac's clock */
} ua101_engine_summary;

void ua101_engine_summarize(const ua101_engine *e, ua101_engine_summary *out);

/* Errors of any kind in the last run, for a pass or fail. Resyncs aren't errors. */
uint64_t ua101_engine_errors(const ua101_engine *e);

/* Stops if needed, returns the interfaces to their idle setting and closes them. */
void ua101_engine_close(ua101_engine *e);

#endif
