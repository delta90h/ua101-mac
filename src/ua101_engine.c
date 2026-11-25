#include "ua101_engine.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>

#include <mach/mach.h>
#include <mach/mach_error.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "ua101_clock.h"
#include "ua101_desc.h"
#include "ua101_drvset.h"
#include "ua101_stream.h"
#include "ua101_timeline.h"

typedef IOUSBDeviceInterface650 **device_ref;
typedef IOUSBInterfaceInterface650 **interface_ref;

#define TRANSFER_MS 2
#define PACKETS (TRANSFER_MS * 8) /* 125 us each */
#define MAX_QUEUE 16
#define HIGH_SPEED_US_PER_FRAME 125
#define BUS_FRAME_SECONDS 0.001
/* As in ua101stream, where each was measured or taken from libusb. */
#define START_MARGIN_FRAMES 4
#define MIN_HEADROOM_FRAMES 2
#define PLAYBACK_DELAY_SECONDS 0.1
#define LAYOUT_PROBE_TRANSFERS 25
#define SIZE_FIFO_SLOTS 1024
#define INTERFACE_WAIT_TRIES 20
#define INTERFACE_WAIT_US 100000
/* With no capture completing for this long, the stream has stopped. */
#define STALL_SECONDS 1.0
/* The bus clock against the host clock (ua101_bus_clock, tests/test_clock.c): stamps still off
   by this much, once each is in its right frame, are left out. */
#define BUS_CLOCK_REJECT_SECONDS 0.0003
#define HEARTBEAT_TRANSFERS 5
#define GAP_HIST_BINS 8
/* The run's own clock reading leaves out the first seconds, while the loops narrow, and needs
   this much after them. */
#define SUMMARY_SETTLE_SECONDS 5.0
#define SUMMARY_MIN_SECONDS 10.0

typedef struct ua101_engine engine;

typedef struct {
    engine *owner;
    uint32_t index;
    uint8_t *buf;
    size_t buf_bytes;
    uint64_t start_frame;
    IOUSBIsocFrame frames[PACKETS];
} transfer;

typedef struct {
    const char *name;
    interface_ref intf;
    UInt8 pipe;
    UInt8 interval;
    CFRunLoopSourceRef source;
    uint32_t max_packet;
    uint32_t frame_bytes;
    uint32_t channels;
    uint32_t queue;
    transfer transfers[MAX_QUEUE];
    uint32_t next_index;   /* next transfer to submit */
    uint32_t expect_index; /* next transfer that should complete */
    uint32_t idle;         /* transfers waiting to be submitted */
    uint64_t next_frame;   /* bus frame for the next submit */
    unsigned inflight;
    ua101_timeline timeline; /* where each transfer's audio belongs in time */

    /* Counted on the USB thread, read after it has stopped. */
    uint64_t completed, frames_total;
    uint64_t transfer_errors, packet_errors, submit_errors, order_errors;
    uint64_t resyncs, skipped_frames, short_writes;
    IOReturn last_error, last_packet_error;
    uint64_t last_done_host, max_gap;
    uint64_t gap_hist[GAP_HIST_BINS];
    int64_t min_headroom;
} pipe_state;

struct ua101_engine {
    ua101_engine_options opt;
    ua101_engine_format format;
    double abs_to_seconds;
    uint32_t bus_frames_per_transfer;
    device_ref device;
    pipe_state cap;
    pipe_state play;
    int play_enabled;

    ua101_shared *shared;
    /* The rings, through this process's own copy of where their frames are. */
    ua101_frame_view cap_ring, play_ring;
    pthread_t thread;
    int thread_running;
    atomic_int stop_request;
    const char *_Atomic failure;
    int gave_up;
    uint64_t started_host, stopped_host;

    /* Buffer layout, checked on the first transfers before any capture is used. */
    ua101_layout layout;
    ua101_layout_stats layout_stats;
    uint32_t layout_transfers;
    uint64_t misaligned_packets, oversize_packets, damaged_transfers, low_byte_nonzero;

    /* Implicit feedback: capture's packet sizes, replayed by playback. */
    ua101_size_fifo fifo;
    uint16_t fifo_slots[SIZE_FIFO_SLOTS];
    uint64_t play_open_host;
    int fifo_open;
    int play_started;
    int64_t play_offset; /* the sample time of playback's frame 0 */

    /* The clock: capture's timeline holds frames against the bus, and this the bus against the
       host, in seconds since host_base. */
    ua101_bus_clock bus_clock;
    uint64_t host_base;
    uint64_t published;
    uint64_t bus_reads_failed;
    ua101_bus_stamp *stamps; /* raw stamps, for checking */
    uint32_t stamp_count, stamp_capacity;

    /* Playback against sample time. */
    uint64_t playback_missing, playback_overwritten;
    int64_t drift_min, drift_max; /* playback numbers against capture's, at the same bus frame */
    int64_t lead_min, lead_max;   /* playback numbers against sample time when read */
    ua101_spare spare;            /* the least the playback ring held beyond a transfer */
    /* For the summary: playback_missing when a transfer was first filled completely, and when
       one last was. */
    int play_filled;
    uint64_t late_base, late_at_last_full;
    /* The first line published after settling, and the last: sample and host seconds. */
    int line_kept;
    uint64_t line_first_sample, line_last_sample;
    double line_first_host, line_last_host;
};

static void note(FILE *log, const char *format, ...) __attribute__((format(printf, 2, 3)));

static void note(FILE *log, const char *format, ...)
{
    va_list args;

    if (log == NULL)
        return;
    va_start(args, format);
    vfprintf(log, format, args);
    va_end(args);
}

static const char *describe(IOReturn result)
{
    static char text[128];

    snprintf(text, sizeof(text), "0x%08x, %s", (unsigned)result, mach_error_string(result));
    return text;
}

static void fail(engine *e, const char *reason)
{
    const char *expected = NULL;

    atomic_compare_exchange_strong(&e->failure, &expected, reason);
    atomic_store(&e->stop_request, 1);
}

static double host_seconds(const engine *e, uint64_t ticks)
{
    return (double)ticks * e->abs_to_seconds;
}

static uint64_t seconds_to_host(const engine *e, double seconds)
{
    return (uint64_t)(seconds / e->abs_to_seconds);
}

static int status_ok(IOReturn status)
{
    return status == kIOReturnSuccess || status == kIOReturnUnderrun;
}

/* --- Streaming, on the USB thread. --- */

static void note_completion(engine *e, pipe_state *p, transfer *x, IOReturn result, void *arg0,
                            uint64_t now)
{
    if (arg0 != (void *)x->frames)
        p->order_errors++;
    if (x->index != p->expect_index)
        p->order_errors++;
    p->expect_index = (x->index + 1) % p->queue;

    if (p->last_done_host != 0) {
        uint64_t gap = now - p->last_done_host;
        double ms = host_seconds(e, gap) * 1000.0;
        unsigned bin = ms < 1.0    ? 0
                       : ms < 2.0  ? 1
                       : ms < 3.0  ? 2
                       : ms < 4.0  ? 3
                       : ms < 6.0  ? 4
                       : ms < 10.0 ? 5
                       : ms < 20.0 ? 6
                                   : 7;

        p->gap_hist[bin]++;
        if (gap > p->max_gap)
            p->max_gap = gap;
    }
    p->last_done_host = now;

    if (!status_ok(result) &&
        !(result == kIOReturnAborted && atomic_load(&e->stop_request))) {
        p->transfer_errors++;
        p->last_error = result;
    }
    p->inflight--;
    p->completed++;
}

/* Skips ahead to a bus frame that can still be reached. The timeline finds the gap. */
static void resync(engine *e, pipe_state *p, UInt64 bus)
{
    uint64_t target = bus + START_MARGIN_FRAMES;

    p->resyncs++;
    atomic_fetch_add_explicit(&e->shared->stats.resyncs, 1, memory_order_relaxed);
    if (target > p->next_frame)
        p->skipped_frames += target - p->next_frame;
    p->next_frame = target;
}

/* Before a submit: it's too late for the next bus frame in the sequence, so skip ahead. */
static void make_room(engine *e, pipe_state *p)
{
    UInt64 bus = 0;
    AbsoluteTime at;

    if ((*p->intf)->GetBusFrameNumber(p->intf, &bus, &at) == kIOReturnSuccess) {
        int64_t headroom = (int64_t)p->next_frame - (int64_t)bus;

        if (headroom < p->min_headroom)
            p->min_headroom = headroom;
        if (headroom < MIN_HEADROOM_FRAMES)
            resync(e, p, bus);
    }
}

static void submitted(engine *e, pipe_state *p)
{
    p->next_frame += e->bus_frames_per_transfer;
    p->inflight++;
}

static void submit_failed(engine *e, pipe_state *p, IOReturn result)
{
    p->submit_errors++;
    p->last_error = result;
    atomic_fetch_add_explicit(&e->shared->stats.errors, 1, memory_order_relaxed);
}

static void capture_done(void *refcon, IOReturn result, void *arg0);
static void playback_done(void *refcon, IOReturn result, void *arg0);

static IOReturn submit_capture(engine *e, transfer *x)
{
    pipe_state *p = &e->cap;
    IOReturn result = kIOReturnError;
    UInt64 bus = 0;
    AbsoluteTime at;
    uint32_t i;
    int attempt;

    /* Marks what the hardware didn't write. See ua101_stream.h. */
    ua101_sentinel_fill(x->buf, (size_t)PACKETS * p->max_packet);
    for (i = 0; i < PACKETS; i++) {
        x->frames[i].frStatus = kIOReturnSuccess;
        x->frames[i].frReqCount = (UInt16)p->max_packet;
        x->frames[i].frActCount = 0;
    }
    make_room(e, p);
    /* The bus keeps moving, so a request can still arrive too late: skip ahead once more. */
    for (attempt = 0;; attempt++) {
        x->start_frame = p->next_frame;
        result = (*p->intf)->ReadIsochPipeAsync(p->intf, p->pipe, x->buf, x->start_frame, PACKETS,
                                                x->frames, capture_done, x);
        if (result != kIOReturnIsoTooOld || attempt > 0 ||
            (*p->intf)->GetBusFrameNumber(p->intf, &bus, &at) != kIOReturnSuccess)
            break;
        resync(e, p, bus);
    }
    if (result != kIOReturnSuccess) {
        submit_failed(e, p, result);
        return result;
    }
    submitted(e, p);
    return kIOReturnSuccess;
}

/* Capture's sample time at a bus frame, from capture's own timeline. */
static double capture_sample_at_bus(const engine *e, double bus)
{
    const ua101_timeline *t = &e->cap.timeline;
    double y = (bus - (double)t->first_bus) * t->bus_frame_seconds;

    return (double)t->clock.x_ref + (y - t->clock.y_ref) / t->clock.period;
}

static void update_extremes(int64_t value, int64_t *lo, int64_t *hi)
{
    if (value < *lo)
        *lo = value;
    if (value > *hi)
        *hi = value;
}

/*
 * Fills a playback transfer from the playback ring. Its frames' sample times come from where
 * the transfer was placed, so this happens after placing and before submitting. Frames the
 * plug-in hasn't written come back as silence.
 */
static void fill_playback(engine *e, transfer *x, uint64_t first, uint32_t frames)
{
    pipe_state *p = &e->play;
    ua101_shared *sh = e->shared;
    int64_t number = e->play_offset + (int64_t)first;
    ua101_frame_ring_miss miss = {0, 0};
    ua101_clock_line line;
    uint64_t ring_end = 0;

    if (frames == 0)
        return;
    update_extremes(number - llround(capture_sample_at_bus(e, (double)x->start_frame)),
                    &e->drift_min, &e->drift_max);
    if (ua101_shared_read(&sh->clock, &line)) {
        int64_t lead = number - llround(ua101_line_sample(&line, (double)mach_absolute_time()));

        update_extremes(lead, &e->lead_min, &e->lead_max);
        atomic_store_explicit(&sh->stats.playback_lead_min, e->lead_min, memory_order_relaxed);
        atomic_store_explicit(&sh->stats.playback_lead_max, e->lead_max, memory_order_relaxed);
    }
    if (number < 0) {
        memset(x->buf, 0, (size_t)frames * p->frame_bytes);
        miss.not_yet = frames;
    } else {
        /* The ring's end as it is before the copy: what was there in time (ua101_spare_of). */
        ring_end = ua101_frame_view_end(&e->play_ring);
        ua101_frame_view_read(&e->play_ring, (uint64_t)number, x->buf, frames, &miss);
    }
    e->playback_missing += miss.not_yet;
    e->playback_overwritten += miss.overwritten;
    /* Counting only: how much room the plug-in's writes left, for the choice of the
       playback delay (src/ua101_drvset.h). It changes nothing that is sent. */
    if (number >= 0 &&
        ua101_spare_note(&e->spare, ring_end, (uint64_t)number + frames,
                         miss.not_yet + miss.overwritten))
        atomic_store_explicit(&sh->stats.playback_spare_min, e->spare.least, memory_order_relaxed);
    if (miss.not_yet + miss.overwritten == 0 && number >= 0) {
        if (!e->play_filled) {
            e->play_filled = 1;
            e->late_base = e->playback_missing + e->playback_overwritten;
        }
        e->late_at_last_full = e->playback_missing + e->playback_overwritten;
        atomic_store_explicit(&sh->stats.playback_late, e->late_at_last_full - e->late_base,
                              memory_order_relaxed);
    }
    atomic_fetch_add_explicit(&sh->stats.playback_frames, frames, memory_order_relaxed);
    atomic_fetch_add_explicit(&sh->stats.playback_missing, miss.not_yet + miss.overwritten,
                              memory_order_relaxed);
}

/* Places, fills and submits one playback transfer of `frames` frames. */
static IOReturn submit_playback(engine *e, transfer *x, uint32_t frames)
{
    pipe_state *p = &e->play;
    ua101_timeline before = p->timeline;
    IOReturn result = kIOReturnError;
    UInt64 bus = 0;
    AbsoluteTime at;
    int attempt;

    make_room(e, p);
    for (attempt = 0;; attempt++) {
        uint64_t first;

        x->start_frame = p->next_frame;
        first = ua101_timeline_place(&p->timeline, x->start_frame, e->bus_frames_per_transfer,
                                     frames);
        fill_playback(e, x, first, frames);
        result = (*p->intf)->WriteIsochPipeAsync(p->intf, p->pipe, x->buf, x->start_frame,
                                                 PACKETS, x->frames, playback_done, x);
        if (result == kIOReturnSuccess)
            break;
        /* It never went, so it never happened: take the placing back. */
        p->timeline = before;
        if (result != kIOReturnIsoTooOld || attempt > 0 ||
            (*p->intf)->GetBusFrameNumber(p->intf, &bus, &at) != kIOReturnSuccess)
            break;
        resync(e, p, bus);
    }
    if (result != kIOReturnSuccess) {
        submit_failed(e, p, result);
        return result;
    }
    atomic_store_explicit(&e->shared->stats.playback_gaps, p->timeline.gaps,
                          memory_order_relaxed);
    submitted(e, p);
    return kIOReturnSuccess;
}

/* Sends every idle playback transfer for which capture's sizes are known. */
static void pump_playback(engine *e)
{
    pipe_state *p = &e->play;

    if (!e->play_enabled || atomic_load(&e->stop_request))
        return;

    if (!e->play_started) {
        UInt64 bus = 0;
        AbsoluteTime at;

        /* Waits for one queue's worth, because the first ones all go at once, and for capture's
           clock, which gives playback its sample times. */
        if (e->fifo.count < p->queue * PACKETS || !ua101_dll_locked(&e->cap.timeline.clock))
            return;
        if ((*p->intf)->GetBusFrameNumber(p->intf, &bus, &at) != kIOReturnSuccess) {
            fail(e, "couldn't read the bus frame number to start playback");
            return;
        }
        p->next_frame = bus + START_MARGIN_FRAMES;
        e->play_offset = llround(capture_sample_at_bus(e, (double)p->next_frame));
        e->play_started = 1;
    }

    while (p->idle > 0 && e->fifo.count >= PACKETS) {
        transfer *x = &p->transfers[p->next_index];
        uint32_t frames = 0, i;

        for (i = 0; i < PACKETS; i++) {
            uint16_t size = 0;

            ua101_fifo_pop(&e->fifo, &size);
            frames += size;
            x->frames[i].frStatus = kIOReturnSuccess;
            x->frames[i].frReqCount = (UInt16)(size * p->frame_bytes);
            x->frames[i].frActCount = 0;
        }
        if (submit_playback(e, x, frames) != kIOReturnSuccess) {
            fail(e, "a playback transfer couldn't be submitted");
            return;
        }
        p->frames_total += frames;
        p->next_index = (p->next_index + 1) % p->queue;
        p->idle--;
    }
}

static void playback_done(void *refcon, IOReturn result, void *arg0)
{
    transfer *x = refcon;
    engine *e = x->owner;
    pipe_state *p = &e->play;
    uint32_t i;

    note_completion(e, p, x, result, arg0, mach_absolute_time());
    if (status_ok(result)) {
        for (i = 0; i < PACKETS; i++) {
            if (!status_ok(x->frames[i].frStatus)) {
                p->packet_errors++;
                p->last_packet_error = x->frames[i].frStatus;
                atomic_fetch_add_explicit(&e->shared->stats.errors, 1, memory_order_relaxed);
            } else if (x->frames[i].frActCount != x->frames[i].frReqCount) {
                p->short_writes++;
            }
        }
    } else if (!(result == kIOReturnAborted && atomic_load(&e->stop_request))) {
        atomic_fetch_add_explicit(&e->shared->stats.errors, 1, memory_order_relaxed);
    }
    p->idle++;
    pump_playback(e);
}

/* The bus clock against the host clock, from a stamp the USB stack pairs itself. */
static void follow_bus_clock(engine *e, pipe_state *p)
{
    UInt64 frame = 0;
    AbsoluteTime time;
    uint64_t host;

    if ((*p->intf)->GetBusFrameNumberWithTime(p->intf, &frame, &time) != kIOReturnSuccess) {
        e->bus_reads_failed++;
        return;
    }
    host = ((uint64_t)time.hi << 32) | time.lo;
    if (e->stamps != NULL && e->stamp_count < e->stamp_capacity) {
        ua101_bus_stamp *s = &e->stamps[e->stamp_count++];

        s->bus = frame;
        s->stamp = host;
        s->asked = mach_absolute_time();
    }
    if (host < e->host_base)
        return;
    ua101_bus_clock_update(&e->bus_clock, frame, host_seconds(e, host - e->host_base));
}

/*
 * The line from sample time to host time: capture's frames against the bus, then the bus
 * against the host. Published at the end of the newest transfer placed.
 */
static void publish_clock(engine *e)
{
    const ua101_timeline *t = &e->cap.timeline;
    double bus, host_seconds_at, ticks_per_sample;
    uint64_t sample;

    if (!t->started || !ua101_dll_locked(&t->clock) || !ua101_bus_clock_settled(&e->bus_clock))
        return;
    sample = t->next_frame;
    bus = (double)t->first_bus + ua101_dll_time(&t->clock, sample) / t->bus_frame_seconds;
    /* At the counter's tick, not where most stamps happen to fall (src/ua101_clock.h). */
    host_seconds_at = ua101_bus_clock_time(&e->bus_clock, bus);
    ticks_per_sample = ua101_dll_period(&t->clock) / t->bus_frame_seconds *
                       ua101_bus_clock_period(&e->bus_clock) / e->abs_to_seconds;
    ua101_shared_publish(&e->shared->clock, sample,
                         (double)e->host_base + host_seconds_at / e->abs_to_seconds,
                         ticks_per_sample);
    if (e->published++ == 0)
        atomic_store(&e->shared->state, UA101_STATE_RUNNING);
    if (host_seconds_at >= SUMMARY_SETTLE_SECONDS) {
        if (!e->line_kept) {
            e->line_kept = 1;
            e->line_first_sample = sample;
            e->line_first_host = host_seconds_at;
        }
        e->line_last_sample = sample;
        e->line_last_host = host_seconds_at;
    }
}

static void decide_layout(engine *e)
{
    ua101_layout verdict;

    if (e->layout != UA101_LAYOUT_UNKNOWN || ++e->layout_transfers < LAYOUT_PROBE_TRANSFERS)
        return;
    verdict = ua101_layout_verdict(&e->layout_stats);
    if (verdict == UA101_LAYOUT_UNKNOWN) {
        fail(e, "the capture buffer layout is neither of the expected ones");
        return;
    }
    e->layout = verdict;
}

static uint64_t count_low_bytes(const uint8_t *data, uint32_t bytes)
{
    uint64_t bad = 0;
    uint32_t i;

    for (i = 0; i + 4 <= bytes; i += 4)
        bad += data[i] != 0;
    return bad;
}

static void process_capture(engine *e, transfer *x)
{
    pipe_state *p = &e->cap;
    ua101_shared *sh = e->shared;
    uint16_t actual[PACKETS];
    uint32_t i, packed = 0, frames = 0, damaged = 0;
    uint64_t first, at;

    for (i = 0; i < PACKETS; i++) {
        IOReturn status = x->frames[i].frStatus;
        uint32_t act = x->frames[i].frActCount;

        if (!status_ok(status)) {
            /* A damaged packet counts as no frames, which keeps playback in step. */
            p->packet_errors++;
            p->last_packet_error = status;
            act = 0;
            damaged++;
        } else if (act > p->max_packet) {
            e->oversize_packets++;
            act = 0;
            damaged++;
        } else if (act % p->frame_bytes != 0) {
            e->misaligned_packets++;
            act -= act % p->frame_bytes;
            damaged++;
        }
        actual[i] = (uint16_t)act;
        frames += act / p->frame_bytes;
        if (e->fifo_open)
            ua101_fifo_push(&e->fifo, (uint16_t)(act / p->frame_bytes));
    }
    if (damaged != 0)
        atomic_fetch_add_explicit(&sh->stats.errors, damaged, memory_order_relaxed);
    if (e->layout == UA101_LAYOUT_UNKNOWN)
        ua101_layout_check(x->buf, PACKETS, p->max_packet, actual, &e->layout_stats);
    p->frames_total += frames;

    /* One with a damaged packet is left out, so the next one finds the gap and time is kept. */
    if (damaged != 0) {
        e->damaged_transfers++;
    } else {
        first = ua101_timeline_place(&p->timeline, x->start_frame, e->bus_frames_per_transfer,
                                     frames);
        if (e->layout != UA101_LAYOUT_UNKNOWN) {
            at = first;
            for (i = 0; i < PACKETS; i++) {
                uint32_t n = actual[i] / p->frame_bytes;
                const uint8_t *data = x->buf + (e->layout == UA101_LAYOUT_SLOT
                                                    ? (size_t)i * p->max_packet
                                                    : (size_t)packed);

                if (n != 0) {
                    e->low_byte_nonzero += count_low_bytes(data, actual[i]);
                    ua101_frame_view_write(&e->cap_ring, at, data, n);
                }
                at += n;
                packed += actual[i];
            }
            atomic_fetch_add_explicit(&sh->stats.capture_frames, frames, memory_order_relaxed);
        }
        atomic_store_explicit(&sh->stats.capture_gaps, p->timeline.gaps, memory_order_relaxed);
        atomic_store_explicit(&sh->stats.capture_gap_frames, p->timeline.gap_frames,
                              memory_order_relaxed);
        follow_bus_clock(e, p);
        publish_clock(e);
    }
    decide_layout(e);
}

static void capture_done(void *refcon, IOReturn result, void *arg0)
{
    transfer *x = refcon;
    engine *e = x->owner;
    pipe_state *p = &e->cap;
    uint64_t now = mach_absolute_time();

    note_completion(e, p, x, result, arg0, now);

    /* A failed transfer has no usable frame list. Keep the sizes in step anyway. */
    if (status_ok(result)) {
        process_capture(e, x);
    } else if (!(result == kIOReturnAborted && atomic_load(&e->stop_request))) {
        uint32_t i;

        atomic_fetch_add_explicit(&e->shared->stats.errors, 1, memory_order_relaxed);
        if (e->fifo_open)
            for (i = 0; i < PACKETS; i++)
                ua101_fifo_push(&e->fifo, 0);
    }
    if (p->completed % HEARTBEAT_TRANSFERS == 0)
        atomic_fetch_add_explicit(&e->shared->heartbeat, 1, memory_order_relaxed);

    if (e->play_enabled && !e->fifo_open && now >= e->play_open_host)
        e->fifo_open = 1;

    if (!atomic_load(&e->stop_request) && submit_capture(e, x) != kIOReturnSuccess)
        fail(e, "a capture transfer couldn't be resubmitted");
    pump_playback(e);
}

static int set_realtime(engine *e)
{
    thread_time_constraint_policy_data_t policy;
    double period = TRANSFER_MS / 1000.0;

    policy.period = (uint32_t)seconds_to_host(e, period);
    policy.computation = (uint32_t)seconds_to_host(e, 0.0005);
    policy.constraint = (uint32_t)seconds_to_host(e, period);
    policy.preemptible = TRUE;
    return thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
                             (thread_policy_t)&policy,
                             THREAD_TIME_CONSTRAINT_POLICY_COUNT) == KERN_SUCCESS;
}

static void *usb_thread(void *arg)
{
    engine *e = arg;
    CFRunLoopRef loop = CFRunLoopGetCurrent();
    uint64_t stop_seen = 0, last_progress, last_completed = 0;
    int aborted = 0;
    UInt64 bus = 0;
    AbsoluteTime at;
    uint32_t i;

    pthread_setname_np("ua101 usb");
    if (!set_realtime(e))
        fail(e, "couldn't get a real-time thread");

    CFRunLoopAddSource(loop, e->cap.source, kCFRunLoopDefaultMode);
    if (e->play_enabled)
        CFRunLoopAddSource(loop, e->play.source, kCFRunLoopDefaultMode);

    if (atomic_load(&e->stop_request)) {
        /* Failed before starting. */
    } else if ((*e->cap.intf)->GetBusFrameNumber(e->cap.intf, &bus, &at) != kIOReturnSuccess) {
        fail(e, "couldn't read the bus frame number to start capture");
    } else {
        e->cap.next_frame = bus + START_MARGIN_FRAMES;
        e->play_open_host = mach_absolute_time() + seconds_to_host(e, PLAYBACK_DELAY_SECONDS);
        for (i = 0; i < e->cap.queue && !atomic_load(&e->stop_request); i++) {
            transfer *x = &e->cap.transfers[i];

            if (submit_capture(e, x) != kIOReturnSuccess)
                fail(e, "a capture transfer couldn't be submitted");
        }
    }
    last_progress = mach_absolute_time();

    for (;;) {
        uint64_t now;

        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, false);
        now = mach_absolute_time();
        if (!atomic_load(&e->stop_request)) {
            if (e->cap.completed != last_completed) {
                last_completed = e->cap.completed;
                last_progress = now;
            } else if (host_seconds(e, now - last_progress) > STALL_SECONDS) {
                fail(e, "no capture transfer has completed for over a second");
            }
            continue;
        }

        if (e->cap.inflight == 0 && e->play.inflight == 0)
            break;
        if (stop_seen == 0) {
            stop_seen = now;
        } else if (host_seconds(e, now - stop_seen) > 2.0) {
            e->gave_up = 1;
            break;
        } else if (!aborted && host_seconds(e, now - stop_seen) > 0.25) {
            aborted = 1;
            (*e->cap.intf)->AbortPipe(e->cap.intf, e->cap.pipe);
            if (e->play_enabled)
                (*e->play.intf)->AbortPipe(e->play.intf, e->play.pipe);
        }
    }

    CFRunLoopRemoveSource(loop, e->cap.source, kCFRunLoopDefaultMode);
    if (e->play_enabled)
        CFRunLoopRemoveSource(loop, e->play.source, kCFRunLoopDefaultMode);
    return NULL;
}

/* --- Opening the unit. --- */

static void add_number(CFMutableDictionaryRef dict, CFStringRef key, int32_t value)
{
    CFNumberRef number = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &value);

    if (number != NULL) {
        CFDictionarySetValue(dict, key, number);
        CFRelease(number);
    }
}

static io_service_t find_device(void)
{
    CFMutableDictionaryRef match = IOServiceMatching("IOUSBHostDevice");

    if (match == NULL)
        return IO_OBJECT_NULL;
    add_number(match, CFSTR("idVendor"), UA101_VENDOR_ID);
    add_number(match, CFSTR("idProduct"), UA101_PRODUCT_HIGH_SPEED);
    return IOServiceGetMatchingService(kIOMainPortDefault, match);
}

static device_ref open_device_object(io_service_t service)
{
    IOCFPlugInInterface **plugin = NULL;
    device_ref device = NULL;
    SInt32 score = 0;

    if (IOCreatePlugInInterfaceForService(service, kIOUSBDeviceUserClientTypeID,
                                          kIOCFPlugInInterfaceID, &plugin, &score) != KERN_SUCCESS ||
        plugin == NULL)
        return NULL;
    (*plugin)->QueryInterface(plugin, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID650),
                              (LPVOID *)&device);
    IODestroyPlugInInterface(plugin);
    return device;
}

static interface_ref open_interface_object(io_service_t service)
{
    IOCFPlugInInterface **plugin = NULL;
    interface_ref intf = NULL;
    SInt32 score = 0;

    if (IOCreatePlugInInterfaceForService(service, kIOUSBInterfaceUserClientTypeID,
                                          kIOCFPlugInInterfaceID, &plugin, &score) != KERN_SUCCESS ||
        plugin == NULL)
        return NULL;
    (*plugin)->QueryInterface(plugin, CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID650),
                              (LPVOID *)&intf);
    IODestroyPlugInInterface(plugin);
    return intf;
}

static interface_ref find_interface(device_ref device, UInt8 wanted)
{
    interface_ref found = NULL;
    int tries;

    /* macOS publishes interfaces shortly after the configuration is set. */
    for (tries = 0; found == NULL && tries < INTERFACE_WAIT_TRIES; tries++) {
        IOUSBFindInterfaceRequest request;
        io_iterator_t iterator = IO_OBJECT_NULL;
        io_service_t service;

        request.bInterfaceClass = kIOUSBFindInterfaceDontCare;
        request.bInterfaceSubClass = kIOUSBFindInterfaceDontCare;
        request.bInterfaceProtocol = kIOUSBFindInterfaceDontCare;
        request.bAlternateSetting = kIOUSBFindInterfaceDontCare;
        if ((*device)->CreateInterfaceIterator(device, &request, &iterator) == kIOReturnSuccess) {
            while ((service = IOIteratorNext(iterator)) != IO_OBJECT_NULL) {
                if (found == NULL) {
                    interface_ref intf = open_interface_object(service);
                    UInt8 number = 0xFF;

                    if (intf != NULL) {
                        (*intf)->GetInterfaceNumber(intf, &number);
                        if (number == wanted)
                            found = intf;
                        else
                            (*intf)->Release(intf);
                    }
                }
                IOObjectRelease(service);
            }
            IOObjectRelease(iterator);
        }
        if (found == NULL)
            usleep(INTERFACE_WAIT_US);
    }
    return found;
}

static int configure_device(device_ref device, FILE *log, char *why, size_t why_bytes)
{
    IOReturn result;
    UInt8 current = 0;

    result = (*device)->USBDeviceOpen(device);
    if (result != kIOReturnSuccess) {
        snprintf(why, why_bytes, "couldn't open the device (%s)%s", describe(result),
                 result == kIOReturnExclusiveAccess
                     ? ": another program has it open, so quit audio or MIDI apps"
                     : "");
        return 0;
    }
    (*device)->GetConfiguration(device, &current);
    if (current != 1) {
        result = (*device)->SetConfiguration(device, 1);
        if (result != kIOReturnSuccess) {
            snprintf(why, why_bytes, "couldn't set the configuration (%s)", describe(result));
            (*device)->USBDeviceClose(device);
            return 0;
        }
        note(log, "  Set configuration 1 (it was %u)\n", current);
    }
    return 1;
}

/* Opens an interface, selects its streaming setting and finds the pipe. */
static int prepare_pipe(engine *e, pipe_state *p, device_ref device, const ua101_config *config,
                        UInt8 number, const char *name, FILE *log, char *why, size_t why_bytes)
{
    const ua101_alt *alt = ua101_find_alt(config, number, 1);
    IOReturn result;
    UInt8 count = 0, pipe;

    p->name = name;
    if (alt == NULL || alt->num_endpoints != 1 || !alt->has_format) {
        snprintf(why, why_bytes, "%s: the descriptor doesn't describe one streaming endpoint",
                 name);
        return 0;
    }
    if (alt->subframe_bytes != 4) {
        snprintf(why, why_bytes, "%s: %u-byte samples aren't handled; only 4-byte ones were seen",
                 name, alt->subframe_bytes);
        return 0;
    }
    p->max_packet = alt->endpoints[0].max_packet;
    p->channels = alt->channels;
    p->frame_bytes = (uint32_t)alt->channels * alt->subframe_bytes;
    if (e->format.rate_hz == 0.0) {
        e->format.rate_hz = alt->rate_hz;
    } else if (e->format.rate_hz != alt->rate_hz) {
        snprintf(why, why_bytes, "playback and capture have different rates");
        return 0;
    }

    p->intf = find_interface(device, number);
    if (p->intf == NULL) {
        snprintf(why, why_bytes, "%s: couldn't find interface %u", name, number);
        return 0;
    }
    result = (*p->intf)->USBInterfaceOpen(p->intf);
    if (result != kIOReturnSuccess) {
        snprintf(why, why_bytes, "%s: couldn't open the interface (%s)", name, describe(result));
        (*p->intf)->Release(p->intf);
        p->intf = NULL;
        return 0;
    }
    result = (*p->intf)->CreateInterfaceAsyncEventSource(p->intf, &p->source);
    if (result != kIOReturnSuccess) {
        snprintf(why, why_bytes, "%s: couldn't create the event source (%s)", name,
                 describe(result));
        return 0;
    }
    result = (*p->intf)->SetAlternateInterface(p->intf, 1);
    if (result != kIOReturnSuccess) {
        snprintf(why, why_bytes, "%s: couldn't select the streaming setting (%s)", name,
                 describe(result));
        return 0;
    }

    (*p->intf)->GetNumEndpoints(p->intf, &count);
    for (pipe = 1; pipe <= count; pipe++) {
        UInt8 direction = 0, endpoint = 0, type = 0, interval = 0;
        UInt16 max_packet = 0;

        if ((*p->intf)->GetPipeProperties(p->intf, pipe, &direction, &endpoint, &type, &max_packet,
                                          &interval) != kIOReturnSuccess)
            continue;
        if (endpoint == (alt->endpoints[0].address & 0x0F) && type == kUSBIsoc &&
            (direction == kUSBIn) == ua101_endpoint_is_in(&alt->endpoints[0])) {
            p->pipe = pipe;
            p->interval = interval;
            note(log, "  %s: pipe %u, endpoint 0x%02x, max packet %u, interval %u, %u channels\n",
                 name, pipe, alt->endpoints[0].address, max_packet, interval, alt->channels);
        }
    }
    if (p->pipe == 0) {
        snprintf(why, why_bytes, "%s: no pipe matches the descriptor", name);
        return 0;
    }
    return 1;
}

static void release_pipe(pipe_state *p)
{
    if (p->intf == NULL)
        return;
    (*p->intf)->SetAlternateInterface(p->intf, 0);
    (*p->intf)->USBInterfaceClose(p->intf);
    (*p->intf)->Release(p->intf);
    p->intf = NULL;
}

static int allocate_transfers(engine *e, pipe_state *p, unsigned queue)
{
    size_t page = (size_t)getpagesize();
    size_t bytes = ((size_t)PACKETS * p->max_packet + page - 1) / page * page;
    unsigned i;

    p->queue = queue;
    for (i = 0; i < queue; i++) {
        transfer *x = &p->transfers[i];

        if (posix_memalign((void **)&x->buf, page, bytes) != 0) {
            x->buf = NULL;
            return 0;
        }
        memset(x->buf, 0, bytes);
        mlock(x->buf, bytes);
        x->buf_bytes = bytes;
        x->owner = e;
        x->index = i;
    }
    return 1;
}

static void free_transfers(pipe_state *p)
{
    unsigned i;

    for (i = 0; i < MAX_QUEUE; i++) {
        if (p->transfers[i].buf != NULL) {
            munlock(p->transfers[i].buf, p->transfers[i].buf_bytes);
            free(p->transfers[i].buf);
            p->transfers[i].buf = NULL;
        }
    }
}

/* Reads the bus clock twice, so a wrong assumption stops it before streaming starts. */
static int check_bus_timing(engine *e, pipe_state *p, FILE *log, char *why, size_t why_bytes)
{
    UInt32 microseconds = 0;
    UInt64 bus0 = 0, bus1 = 0;
    AbsoluteTime t0, t1;
    double host_ms, per_ms;

    if ((*p->intf)->GetFrameListTime(p->intf, &microseconds) != kIOReturnSuccess ||
        microseconds != HIGH_SPEED_US_PER_FRAME) {
        snprintf(why, why_bytes, "the bus isn't Hi-Speed (frame list time %u us)",
                 (unsigned)microseconds);
        return 0;
    }
    if ((*p->intf)->GetBusFrameNumberWithTime(p->intf, &bus0, &t0) != kIOReturnSuccess)
        goto unreadable;
    usleep(100000);
    if ((*p->intf)->GetBusFrameNumberWithTime(p->intf, &bus1, &t1) != kIOReturnSuccess)
        goto unreadable;
    host_ms = host_seconds(e, (((uint64_t)t1.hi << 32) | t1.lo) - (((uint64_t)t0.hi << 32) | t0.lo)) *
              1000.0;
    per_ms = host_ms > 0.0 ? (double)(bus1 - bus0) / host_ms : 0.0;
    note(log, "  Bus frame %llu, %.3f per ms\n", (unsigned long long)bus1, per_ms);
    if (fabs(per_ms - 1.0) > 0.02) {
        snprintf(why, why_bytes, "bus frames aren't counting in milliseconds (%.3f per ms)",
                 per_ms);
        return 0;
    }
    return 1;

unreadable:
    snprintf(why, why_bytes, "couldn't read the bus frame number");
    return 0;
}

static void close_all(engine *e)
{
    release_pipe(&e->play);
    release_pipe(&e->cap);
    free_transfers(&e->play);
    free_transfers(&e->cap);
    if (e->device != NULL)
        (*e->device)->Release(e->device);
    e->device = NULL;
}

ua101_engine *ua101_engine_open(const ua101_engine_options *opt, ua101_engine_format *format,
                                FILE *log, char *why, size_t why_bytes)
{
    mach_timebase_info_data_t timebase;
    IOUSBConfigurationDescriptorPtr descriptor = NULL;
    ua101_config config;
    io_service_t service;
    engine *e;
    int device_open = 0;

    if (why_bytes > 0)
        why[0] = '\0';
    e = calloc(1, sizeof(*e));
    if (e == NULL) {
        snprintf(why, why_bytes, "out of memory");
        return NULL;
    }
    e->opt = *opt;
    if (e->opt.capture_queue == 0)
        e->opt.capture_queue = UA101_ENGINE_CAPTURE_QUEUE;
    if (e->opt.playback_queue == 0)
        e->opt.playback_queue = UA101_ENGINE_PLAYBACK_QUEUE;
    if (e->opt.capture_queue < 2 || e->opt.capture_queue > MAX_QUEUE ||
        e->opt.playback_queue < 2 || e->opt.playback_queue > MAX_QUEUE) {
        snprintf(why, why_bytes, "queues must be 2 to %d transfers", MAX_QUEUE);
        free(e);
        return NULL;
    }
    e->play_enabled = !e->opt.capture_only;
    mach_timebase_info(&timebase);
    e->abs_to_seconds = (double)timebase.numer / (double)timebase.denom / 1e9;

    service = find_device();
    if (service == IO_OBJECT_NULL) {
        snprintf(why, why_bytes, "no UA-101 in Hi-Speed mode was found");
        free(e);
        return NULL;
    }
    e->device = open_device_object(service);
    IOObjectRelease(service);
    if (e->device == NULL) {
        snprintf(why, why_bytes, "couldn't get a device object");
        free(e);
        return NULL;
    }
    if (!configure_device(e->device, log, why, why_bytes))
        goto failed;
    device_open = 1;
    if ((*e->device)->GetConfigurationDescriptorPtr(e->device, 0, &descriptor) !=
            kIOReturnSuccess ||
        descriptor == NULL ||
        ua101_parse_config((const uint8_t *)descriptor, descriptor->wTotalLength, &config) !=
            UA101_PARSE_OK) {
        snprintf(why, why_bytes, "couldn't read the configuration descriptor");
        goto failed;
    }

    /* Capture first, as playback needs it running. */
    if (!prepare_pipe(e, &e->cap, e->device, &config, UA101_IF_CAPTURE, "Capture", log, why,
                      why_bytes))
        goto failed;
    if (e->play_enabled && !prepare_pipe(e, &e->play, e->device, &config, UA101_IF_PLAYBACK,
                                         "Playback", log, why, why_bytes))
        goto failed;

    /* Only the streaming interfaces stay open, so another program can own MIDI. */
    (*e->device)->USBDeviceClose(e->device);
    device_open = 0;

    e->bus_frames_per_transfer = ua101_iso_bus_frames(PACKETS, e->cap.interval);
    if (e->bus_frames_per_transfer == 0 ||
        (e->play_enabled &&
         ua101_iso_bus_frames(PACKETS, e->play.interval) != e->bus_frames_per_transfer)) {
        snprintf(why, why_bytes, "transfers of %d packets don't fit the endpoint intervals",
                 PACKETS);
        goto failed;
    }
    /* Playback must carry any packet size capture can give. */
    if (e->play_enabled &&
        e->cap.max_packet / e->cap.frame_bytes > e->play.max_packet / e->play.frame_bytes) {
        snprintf(why, why_bytes, "a capture packet can hold more frames than a playback packet");
        goto failed;
    }
    if (!check_bus_timing(e, &e->cap, log, why, why_bytes))
        goto failed;
    if (!allocate_transfers(e, &e->cap, e->opt.capture_queue) ||
        (e->play_enabled && !allocate_transfers(e, &e->play, e->opt.playback_queue))) {
        snprintf(why, why_bytes, "out of memory");
        goto failed;
    }

    e->format.capture_channels = e->cap.channels;
    e->format.playback_channels = e->play_enabled ? e->play.channels : 0;
    *format = e->format;
    return e;

failed:
    if (device_open)
        (*e->device)->USBDeviceClose(e->device);
    close_all(e);
    free(e);
    return NULL;
}

static void reset_pipe(pipe_state *p, double rate_hz)
{
    p->next_index = p->expect_index = 0;
    p->idle = p->queue;
    p->next_frame = 0;
    p->inflight = 0;
    ua101_timeline_init(&p->timeline, rate_hz, BUS_FRAME_SECONDS);
    p->completed = p->frames_total = 0;
    p->transfer_errors = p->packet_errors = p->submit_errors = p->order_errors = 0;
    p->resyncs = p->skipped_frames = p->short_writes = 0;
    p->last_error = p->last_packet_error = kIOReturnSuccess;
    p->last_done_host = p->max_gap = 0;
    memset(p->gap_hist, 0, sizeof(p->gap_hist));
    p->min_headroom = INT64_MAX;
}

static void reset_shared(ua101_shared *sh)
{
    ua101_shared_stats *st = &sh->stats;

    ua101_frame_ring_reset(&sh->capture);
    ua101_frame_ring_reset(&sh->playback);
    atomic_store(&st->capture_frames, 0);
    atomic_store(&st->capture_gaps, 0);
    atomic_store(&st->capture_gap_frames, 0);
    atomic_store(&st->playback_frames, 0);
    atomic_store(&st->playback_missing, 0);
    atomic_store(&st->playback_gaps, 0);
    atomic_store(&st->resyncs, 0);
    atomic_store(&st->errors, 0);
    atomic_store(&st->playback_lead_min, INT64_MAX);
    atomic_store(&st->playback_lead_max, INT64_MIN);
    atomic_store(&st->playback_spare_min, UA101_SPARE_NONE);
    atomic_store(&st->playback_late, 0);
    ua101_shared_new_seed(&sh->clock);
    atomic_store(&sh->state, UA101_STATE_STARTING);
}

int ua101_engine_start(ua101_engine *e, ua101_shared *shared, size_t shared_bytes)
{
    if (e->thread_running || shared == NULL || shared->rate_hz != e->format.rate_hz ||
        !ua101_shared_views(shared, shared_bytes, e->cap.channels,
                            e->play_enabled ? e->play.channels : 0, &e->cap_ring, &e->play_ring))
        return 0;
    e->shared = shared;
    atomic_store(&e->stop_request, 0);
    atomic_store(&e->failure, NULL);
    e->gave_up = 0;
    reset_pipe(&e->cap, e->format.rate_hz);
    reset_pipe(&e->play, e->format.rate_hz);
    e->layout = UA101_LAYOUT_UNKNOWN;
    memset(&e->layout_stats, 0, sizeof(e->layout_stats));
    e->layout_transfers = 0;
    e->misaligned_packets = e->oversize_packets = e->damaged_transfers = 0;
    e->low_byte_nonzero = 0;
    ua101_fifo_init(&e->fifo, e->fifo_slots, SIZE_FIFO_SLOTS);
    e->fifo_open = e->play_started = 0;
    e->play_offset = 0;
    e->host_base = mach_absolute_time();
    ua101_bus_clock_init(&e->bus_clock, BUS_FRAME_SECONDS, UA101_BUS_CLOCK_BANDWIDTH_HZ,
                         BUS_CLOCK_REJECT_SECONDS);
    e->published = e->bus_reads_failed = 0;
    e->stamp_count = 0;
    e->playback_missing = e->playback_overwritten = 0;
    e->play_filled = e->line_kept = 0;
    e->late_base = e->late_at_last_full = 0;
    e->drift_min = e->lead_min = INT64_MAX;
    e->drift_max = e->lead_max = INT64_MIN;
    ua101_spare_reset(&e->spare);
    reset_shared(shared);

    e->started_host = mach_absolute_time();
    if (pthread_create(&e->thread, NULL, usb_thread, e) != 0) {
        atomic_store(&shared->state, UA101_STATE_FAILED);
        return 0;
    }
    e->thread_running = 1;
    return 1;
}

const char *ua101_engine_failure(const ua101_engine *e)
{
    return atomic_load(&((engine *)e)->failure);
}

void ua101_engine_stop(ua101_engine *e)
{
    if (!e->thread_running)
        return;
    atomic_store(&e->stop_request, 1);
    pthread_join(e->thread, NULL);
    e->thread_running = 0;
    e->stopped_host = mach_absolute_time();
    atomic_store(&e->shared->state,
                 ua101_engine_failure(e) != NULL ? UA101_STATE_FAILED : UA101_STATE_STOPPED);
}

static void print_pipe(const engine *e, const pipe_state *p, FILE *out)
{
    static const char *bins[GAP_HIST_BINS] = {"<1", "1-2", "2-3", "3-4", "4-6", "6-10", "10-20",
                                              ">=20"};
    const ua101_timeline *t = &p->timeline;
    unsigned b;

    fprintf(out, "  %s: %llu transfers, %llu frames; errors: %llu transfer, %llu packet, "
                 "%llu submit, %llu order\n",
            p->name, (unsigned long long)p->completed, (unsigned long long)p->frames_total,
            (unsigned long long)p->transfer_errors, (unsigned long long)p->packet_errors,
            (unsigned long long)p->submit_errors, (unsigned long long)p->order_errors);
    if (p->last_error != kIOReturnSuccess)
        fprintf(out, "    last error %s\n", describe(p->last_error));
    if (p->last_packet_error != kIOReturnSuccess)
        fprintf(out, "    last packet error %s\n", describe(p->last_packet_error));
    fprintf(out, "    gaps between completions (ms):");
    for (b = 0; b < GAP_HIST_BINS; b++)
        if (p->gap_hist[b] != 0)
            fprintf(out, " %s: %llu", bins[b], (unsigned long long)p->gap_hist[b]);
    fprintf(out, "; longest %.2f ms\n", host_seconds(e, p->max_gap) * 1000.0);
    fprintf(out, "    least headroom %lld bus frames; %llu resyncs, %llu bus frames skipped",
            p->min_headroom == INT64_MAX ? 0LL : (long long)p->min_headroom,
            (unsigned long long)p->resyncs, (unsigned long long)p->skipped_frames);
    if (p->short_writes != 0)
        fprintf(out, "; %llu short writes", (unsigned long long)p->short_writes);
    fprintf(out, "\n    timeline: %llu gaps, %llu frames worked out for them, %llu overlaps\n",
            (unsigned long long)t->gaps, (unsigned long long)t->gap_frames,
            (unsigned long long)t->overlaps);
}

void ua101_engine_report(const ua101_engine *e, FILE *out)
{
    const ua101_timeline *t = &e->cap.timeline;
    const ua101_bus_clock *b = &e->bus_clock;
    double seconds = host_seconds(e, (e->stopped_host > e->started_host ? e->stopped_host
                                                                         : mach_absolute_time()) -
                                         e->started_host);

    fprintf(out, "Engine, %.1f s at %.0f Hz:\n", seconds, e->format.rate_hz);
    print_pipe(e, &e->cap, out);
    fprintf(out, "    layout %s; %llu damaged transfers left out; %llu misaligned, %llu oversize "
                 "packets; %llu samples with a nonzero low byte\n",
            ua101_layout_string(e->layout), (unsigned long long)e->damaged_transfers,
            (unsigned long long)e->misaligned_packets, (unsigned long long)e->oversize_packets,
            (unsigned long long)e->low_byte_nonzero);
    if (e->play_enabled) {
        print_pipe(e, &e->play, out);
        fprintf(out, "    from the ring: %llu frames not written in time, %llu overwritten\n",
                (unsigned long long)e->playback_missing,
                (unsigned long long)e->playback_overwritten);
        if (e->drift_min <= e->drift_max)
            fprintf(out, "    playback against capture at the same bus frame: %+lld to %+lld "
                         "frames\n",
                    (long long)e->drift_min, (long long)e->drift_max);
        if (e->lead_min <= e->lead_max)
            fprintf(out, "    read ahead of sample time: %lld to %lld frames (%.2f to %.2f ms)\n",
                    (long long)e->lead_min, (long long)e->lead_max,
                    (double)e->lead_min * 1000.0 / e->format.rate_hz,
                    (double)e->lead_max * 1000.0 / e->format.rate_hz);
        if (e->spare.have_least)
            fprintf(out, "    least in the ring beyond a transfer: %lld frames (%.2f ms)\n",
                    (long long)e->spare.least, (double)e->spare.least * 1000.0 / e->format.rate_hz);
        fprintf(out, "    size FIFO: %llu pushed, %llu popped, %llu dropped frames\n",
                (unsigned long long)e->fifo.pushed_frames,
                (unsigned long long)e->fifo.popped_frames,
                (unsigned long long)e->fifo.dropped_frames);
    }
    fprintf(out, "  Clock: %llu lines published; unit %.3f Hz against the bus (%+.1f ppm)\n",
            (unsigned long long)e->published,
            ua101_dll_locked(&t->clock) ? 1.0 / ua101_dll_period(&t->clock) : 0.0,
            ua101_dll_locked(&t->clock)
                ? (1.0 / ua101_dll_period(&t->clock) / e->format.rate_hz - 1.0) * 1e6
                : 0.0);
    fprintf(out, "    bus against host %+.2f ppm; of %lu stamps, %lu taken as the next frame, "
                 "%lu left out; %lu start, %lu restarts, moved a frame %lu times; %llu "
                 "unreadable\n",
            ua101_dll_locked(&b->dll)
                ? (ua101_bus_clock_period(b) / BUS_FRAME_SECONDS - 1.0) * 1e6
                : 0.0,
            b->dll.updates + b->dll.rejected, b->corrected, b->dll.rejected, b->starts,
            b->dll.restarts, b->moved, (unsigned long long)e->bus_reads_failed);
    if (b->tick_started)
        fprintf(out, "    the counter's tick: %s, %+.0f us from the line through most stamps; its "
                     "bounds %.1f us apart at the end; %lu stamps since publishing it disagreed "
                     "with it\n",
                b->tick_found ? "found between stamps either side"
                              : "never found, so taken just before the earliest stamp",
                (b->tick - ua101_dll_time(&b->dll, b->tick_frame)) * 1e6,
                (b->tick_hi - b->tick_lo) * 1e6, b->tick_disagree);
    if (e->gave_up)
        fprintf(out, "  Gave up waiting for transfers to finish after stopping.\n");
    if (ua101_engine_failure(e) != NULL)
        fprintf(out, "  Stopped because %s.\n", ua101_engine_failure(e));
}

int ua101_engine_keep_stamps(ua101_engine *e, uint32_t capacity)
{
    if (e->thread_running)
        return 0;
    free(e->stamps);
    e->stamps = capacity != 0 ? calloc(capacity, sizeof(*e->stamps)) : NULL;
    e->stamp_capacity = e->stamps != NULL ? capacity : 0;
    e->stamp_count = 0;
    if (e->stamps != NULL)
        mlock(e->stamps, capacity * sizeof(*e->stamps));
    return capacity == 0 || e->stamps != NULL;
}

const ua101_bus_stamp *ua101_engine_stamps(const ua101_engine *e, uint32_t *count)
{
    *count = e->stamp_count;
    return e->stamps;
}

void ua101_engine_summarize(const ua101_engine *e, ua101_engine_summary *out)
{
    memset(out, 0, sizeof(*out));
    out->seconds = host_seconds(e, (e->stopped_host > e->started_host ? e->stopped_host
                                                                       : mach_absolute_time()) -
                                       e->started_host);
    out->capture_frames = e->cap.frames_total;
    out->capture_lost_frames = e->cap.timeline.gap_frames;
    out->capture_gaps = e->cap.timeline.gaps;
    out->playback_frames = e->play.frames_total;
    out->playback_late_frames = e->play_filled ? e->late_at_last_full - e->late_base : 0;
    out->playback_spare_known = e->spare.have_least;
    out->playback_spare_frames = e->spare.have_least ? e->spare.least : 0;
    out->rate_hz = e->format.rate_hz;
    out->errors = ua101_engine_errors(e);
    out->resyncs = e->cap.resyncs + e->play.resyncs;
    if (e->line_kept && e->line_last_host - e->line_first_host >= SUMMARY_MIN_SECONDS) {
        out->clock_seconds = e->line_last_host - e->line_first_host;
        out->clock_ppm = ((double)(e->line_last_sample - e->line_first_sample) /
                              out->clock_seconds / e->format.rate_hz - 1.0) * 1e6;
    }
}

uint64_t ua101_engine_errors(const ua101_engine *e)
{
    const pipe_state *c = &e->cap, *p = &e->play;

    return c->transfer_errors + c->packet_errors + c->submit_errors + c->order_errors +
           p->transfer_errors + p->packet_errors + p->submit_errors + p->order_errors +
           e->misaligned_packets + e->oversize_packets + (uint64_t)(e->gave_up != 0) +
           (uint64_t)(ua101_engine_failure(e) != NULL);
}

void ua101_engine_close(ua101_engine *e)
{
    if (e == NULL)
        return;
    ua101_engine_stop(e);
    close_all(e);
    free(e->stamps);
    free(e);
}
