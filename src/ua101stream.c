/*
 * ua101stream: Stage 2 streaming test for the Edirol/Roland UA-101.
 *
 * Runs capture, and optionally playback, from user space on a real-time thread,
 * writes the capture to a WAV file and reports every error it sees. Playback is
 * silence unless a tone is asked for, and the tone's level and length are capped
 * at compile time. It installs nothing and changes no system setting.
 *
 * The protocol comes from reading the Linux snd-ua101 driver: capture sets the
 * clock through its packet sizes, and playback repeats that sequence of sizes.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>

#include <errno.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <mach/mach_error.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "ua101_desc.h"
#include "ua101_stream.h"
#include "ua101_timeline.h"

typedef IOUSBDeviceInterface650 **device_ref;
typedef IOUSBInterfaceInterface650 **interface_ref;

#define HIGH_SPEED_US_PER_FRAME 125
#define MAX_QUEUE 16
/* A design choice (2026-11-09), from three overnight tests of about 17 h each at 48 kHz:
   a capture queue of 4 lost audio in 7 runs, and 8 in none, through stalls up to 9.6 ms.
   Playback stays at 4 until it's chosen separately, because its queue adds delay. */
#define DEFAULT_CAPTURE_QUEUE 8
#define DEFAULT_PLAYBACK_QUEUE 4
#define MAX_PACKETS 64
#define MAX_TONES 4
#define SIZE_FIFO_SLOTS 1024
#define RING_BYTES (64u * 1024u * 1024u)
#define SIZE_HIST_BINS 16
#define LOW_SECONDS 30 /* the last one also counts everything later */
#define GAP_HIST_BINS 8
#define HEADROOM_HIST_BINS 24

/* libusb starts isochronous transfers this many bus frames ahead. */
#define START_MARGIN_FRAMES 4
/* Measured on macOS 27: one frame ahead is refused with kIOReturnIsoTooOld. Two is accepted. */
#define MIN_HEADROOM_FRAMES 2
/* Capture settles for this long, and its sizes are discarded, before playback starts. */
#define PLAYBACK_DELAY_SECONDS 0.1
/* Transfers to examine before trusting a buffer layout. */
#define LAYOUT_PROBE_TRANSFERS 25
/* Rates are measured from here, to skip whatever the device does as it starts. */
#define RATE_BASELINE_SECONDS 1.0
#define RATE_LIMIT_PPM 100.0
#define STALL_SECONDS 1.0
/* --stall-test holds capture up once, this far in, for less than STALL_SECONDS. The transfers
   either side of the gap that it makes, up to CHECK_TRANSFERS each, are compared afterwards. */
#define STALL_TEST_AT_SECONDS 10.0
#define STALL_TEST_MAX_MS 500
#define CHECK_TRANSFERS 1000
#define INTERFACE_WAIT_TRIES 20
#define INTERFACE_WAIT_US 100000

typedef struct {
    double seconds;
    int capture_only;
    const char *wav_path;
    unsigned transfer_ms;
    unsigned capture_queue;  /* transfers queued for capture */
    unsigned playback_queue; /* transfers queued for playback */
    double tone_db;
    double tone_seconds;
    double tone_at[MAX_TONES];
    int tone_count;
    int jack_map; /* each tone plays on every output, each at its own frequency */
    unsigned dump_transfers;
    int keep_device_open;
    const char *clock_log_path;
    const char *stall_log_path;
    const char *transfer_log_path;
    unsigned stall_test_ms;
} options;

/* One reading for --clock-log. */
typedef struct {
    uint64_t bus;        /* bus frame at the start of a transfer */
    uint64_t frames;     /* frames captured up to the end of that transfer */
    uint64_t stamp_bus;  /* a bus frame, and the host time it was read at */
    uint64_t stamp_host;
} clock_sample;

/* One slow moment for --stall-log. Normal gaps reach about 2.8 ms, and normal submits stay
   well under a millisecond most of the time, so these catch what is unusual without flooding. */
#define STALL_LOG_CAPACITY 20000
#define STALL_GAP_MS 3.0
#define STALL_SUBMIT_MS 1.0

typedef struct {
    uint64_t when;   /* host time at the end of it */
    uint64_t length; /* in host ticks; 0 for a resync */
    char kind;       /* 'g' gap between completions, 's' slow submit, 'r' resync */
} stall_event;

typedef struct stream stream;

typedef struct {
    stream *owner;
    uint32_t index;
    uint8_t *buf;
    uint64_t start_frame;
    IOUSBIsocFrame frames[MAX_PACKETS];
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
    uint32_t queue; /* transfers in use, at most MAX_QUEUE */
    transfer transfers[MAX_QUEUE];
    uint32_t next_index;   /* next transfer to submit */
    uint32_t expect_index; /* next transfer that should complete */
    uint32_t idle;         /* transfers waiting to be submitted */
    uint64_t next_frame;
    atomic_uint inflight;

    atomic_ullong completed;
    atomic_ullong frames_total;
    atomic_ullong transfer_errors;
    atomic_ullong packet_errors;
    atomic_ullong submit_errors;
    atomic_ullong resyncs;
    atomic_ullong order_errors;
    atomic_int last_error;
    atomic_int last_packet_error;

    uint64_t last_done_host;
    uint64_t max_gap;
    uint64_t gap_hist[GAP_HIST_BINS];
    uint64_t headroom_hist[HEADROOM_HIST_BINS];
    int64_t min_headroom;
    uint64_t max_submit;
    uint64_t short_writes;
    uint64_t skipped_frames; /* bus frames (1 ms each) jumped over by resyncs, so lost audio */
    ua101_timeline timeline; /* where each transfer's audio belongs in time, as in the driver */
} pipe_state;

typedef enum { TONE_WAITING, TONE_PLAYING, TONE_DONE, TONE_REFUSED } tone_state;

typedef struct {
    ua101_tone tone;
    tone_state state;
    uint64_t wav_frame_when_started;
} tone_slot;

struct stream {
    options opt;
    double rate_hz;
    uint32_t packets;
    uint32_t bus_frames_per_transfer;
    double abs_to_seconds;

    pipe_state cap;
    pipe_state play;
    int play_enabled;

    atomic_int stop_request;
    const char *_Atomic fatal;
    int gave_up;

    /* Buffer layout. */
    ua101_layout layout;
    ua101_layout_stats layout_stats;
    uint32_t layout_transfers;
    uint64_t wav_start_frames;
    atomic_int wav_started;
    atomic_ullong misaligned_packets;
    atomic_ullong oversize_packets;
    atomic_ullong low_byte_nonzero;
    /* Where they came, to choose how long a driver ignores capture after power-on. They were
       seen once, within seconds of power-on, about 70 ms of them, but not when exactly. */
    uint64_t low_first_frame, low_last_frame, low_frames;
    uint32_t low_channels; /* one bit per capture channel */
    uint64_t low_by_second[LOW_SECONDS];
    uint64_t size_hist[SIZE_HIST_BINS];

    /* Implicit feedback. */
    ua101_size_fifo fifo;
    uint16_t fifo_slots[SIZE_FIFO_SLOTS];
    uint64_t play_open_host;
    int fifo_open;
    int play_started;
    uint64_t cap_frames_at_fifo_open;

    /* Output. */
    uint64_t out_pos;
    tone_slot tones[MAX_TONES];

    /* Rate. */
    int have_baseline;
    uint64_t base_host, base_bus, base_frames;
    uint64_t last_host, last_bus, last_frames;
    /* Bus frames paired with their host times by the USB stack, not by a callback. */
    int have_bus_times;
    uint64_t stamp_base_bus, stamp_base_host;
    uint64_t stamp_last_bus, stamp_last_host;

    /* Recording. */
    ua101_ring ring;
    uint8_t *ring_data;
    int wav_fd;
    atomic_int writer_stop;
    atomic_ullong ring_dropped_bytes;
    atomic_ullong wav_data_bytes;
    atomic_int write_errno;

    /* Raw copies of the first transfers, for looking at real data afterwards. */
    uint8_t *dump_data;
    IOUSBIsocFrame *dump_frames;
    uint32_t dump_count;

    /* Stored during the run and written to disk after it. */
    clock_sample *clock_log;
    uint32_t clock_log_count, clock_log_capacity;
    uint64_t clock_log_next_bus;

    stall_event *stall_log;
    uint32_t stall_log_count;
    uint64_t stall_log_dropped;
    uint64_t stall_host_start;
    double stall_wall_start;

    /* --stall-test: every capture transfer's bus frame and timeline frame number. */
    int stall_test_done;
    uint64_t *check_bus, *check_frame;
    uint32_t check_count, check_capacity;
};

static stream g_stream;
static volatile sig_atomic_t g_signal;

static const char *describe(IOReturn result)
{
    static char text[128];

    snprintf(text, sizeof(text), "0x%08x, %s", (unsigned)result, mach_error_string(result));
    return text;
}

static void on_signal(int number)
{
    g_signal = number;
}

static void fail_stream(stream *s, const char *reason)
{
    const char *expected = NULL;

    atomic_compare_exchange_strong(&s->fatal, &expected, reason);
    atomic_store(&s->stop_request, 1);
}

static double host_seconds(const stream *s, uint64_t ticks)
{
    return (double)ticks * s->abs_to_seconds;
}

static uint64_t seconds_to_host(const stream *s, double seconds)
{
    return (uint64_t)(seconds / s->abs_to_seconds);
}

static uint64_t absolute_ticks(AbsoluteTime time)
{
    return ((uint64_t)time.hi << 32) | time.lo;
}

static int read_bus_time(const pipe_state *p, uint64_t *bus, uint64_t *host)
{
    UInt64 frame = 0;
    AbsoluteTime time;

    if ((*p->intf)->GetBusFrameNumberWithTime(p->intf, &frame, &time) != kIOReturnSuccess)
        return 0;
    *bus = frame;
    *host = absolute_ticks(time);
    return 1;
}

/* Short packets normally complete with kIOReturnUnderrun, which is not an error. */
static int status_ok(IOReturn status)
{
    return status == kIOReturnSuccess || status == kIOReturnUnderrun;
}

/* Runs on the real-time thread, so it only fills memory set aside before the run. */
static void note_stall(stream *s, char kind, uint64_t when, uint64_t length)
{
    if (s->stall_log == NULL)
        return;
    if (s->stall_log_count < STALL_LOG_CAPACITY) {
        stall_event *e = &s->stall_log[s->stall_log_count++];

        e->when = when;
        e->length = length;
        e->kind = kind;
    } else {
        s->stall_log_dropped++;
    }
}

static void note_completion(stream *s, pipe_state *p, transfer *x, IOReturn result, void *arg0,
                            uint64_t now)
{
    if (arg0 != (void *)x->frames)
        p->order_errors++;
    if (x->index != p->expect_index)
        p->order_errors++;
    p->expect_index = (x->index + 1) % p->queue;

    if (p->last_done_host != 0) {
        uint64_t gap = now - p->last_done_host;
        double ms = host_seconds(s, gap) * 1000.0;
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
        if (ms >= STALL_GAP_MS)
            note_stall(s, 'g', now, gap);
    }
    p->last_done_host = now;

    if (!status_ok(result) &&
        !(result == kIOReturnAborted && atomic_load(&s->stop_request))) {
        p->transfer_errors++;
        p->last_error = result;
    }
    p->inflight--;
    p->completed++;
}

/* Skips ahead to a bus frame that can still be reached. Every frame jumped over is audio that
   was never asked for, so it's counted: it shows how much was lost, even with no empty packet. */
static void resync(stream *s, pipe_state *p, UInt64 bus, uint64_t when)
{
    uint64_t target = bus + START_MARGIN_FRAMES;

    p->resyncs++;
    if (target > p->next_frame)
        p->skipped_frames += target - p->next_frame;
    p->next_frame = target;
    note_stall(s, 'r', when, 0);
}

/* Sends one transfer on its way at the next bus frame in the sequence. */
static IOReturn submit(stream *s, pipe_state *p, transfer *x, int is_read,
                       IOAsyncCallback1 callback)
{
    uint64_t t0 = mach_absolute_time();
    UInt64 bus = 0;
    AbsoluteTime at;
    IOReturn result;
    int64_t headroom;
    uint64_t took;
    int attempt;

    result = (*p->intf)->GetBusFrameNumber(p->intf, &bus, &at);
    if (result == kIOReturnSuccess) {
        headroom = (int64_t)p->next_frame - (int64_t)bus;
        if (headroom < p->min_headroom)
            p->min_headroom = headroom;
        p->headroom_hist[headroom < 0 ? 0
                         : headroom >= HEADROOM_HIST_BINS ? HEADROOM_HIST_BINS - 1
                                                          : (unsigned)headroom]++;
        /* It's too late for this frame, so the only choice is to skip ahead. */
        if (headroom < MIN_HEADROOM_FRAMES)
            resync(s, p, bus, t0);
    }

    /* The bus keeps moving, so a request can still arrive too late. Count it and carry on. */
    for (attempt = 0;; attempt++) {
        x->start_frame = p->next_frame;
        if (is_read)
            result = (*p->intf)->ReadIsochPipeAsync(p->intf, p->pipe, x->buf, x->start_frame,
                                                    s->packets, x->frames, callback, x);
        else
            result = (*p->intf)->WriteIsochPipeAsync(p->intf, p->pipe, x->buf, x->start_frame,
                                                     s->packets, x->frames, callback, x);
        if (result != kIOReturnIsoTooOld || attempt > 0 ||
            (*p->intf)->GetBusFrameNumber(p->intf, &bus, &at) != kIOReturnSuccess)
            break;
        resync(s, p, bus, mach_absolute_time());
    }
    if (result != kIOReturnSuccess) {
        p->submit_errors++;
        p->last_error = result;
        return result;
    }
    p->next_frame += s->bus_frames_per_transfer;
    p->inflight++;

    took = mach_absolute_time() - t0;
    if (took > p->max_submit)
        p->max_submit = took;
    if (host_seconds(s, took) * 1000.0 >= STALL_SUBMIT_MS)
        note_stall(s, 's', t0 + took, took);
    return kIOReturnSuccess;
}

static void capture_done(void *refcon, IOReturn result, void *arg0);
static void playback_done(void *refcon, IOReturn result, void *arg0);

static IOReturn submit_capture(stream *s, transfer *x)
{
    pipe_state *p = &s->cap;
    uint32_t i;

    /* Marks what the hardware didn't write. See ua101_stream.h. */
    ua101_sentinel_fill(x->buf, (size_t)s->packets * p->max_packet);
    for (i = 0; i < s->packets; i++) {
        x->frames[i].frStatus = kIOReturnSuccess;
        x->frames[i].frReqCount = (UInt16)p->max_packet;
        x->frames[i].frActCount = 0;
    }
    return submit(s, p, x, 1, capture_done);
}

static int tones_are_safe(const stream *s)
{
    return s->layout != UA101_LAYOUT_UNKNOWN && s->wav_started &&
           s->low_byte_nonzero == 0 && s->size_hist[0] == 0 && s->cap.transfer_errors == 0 &&
           s->cap.packet_errors == 0 && s->cap.submit_errors == 0 && s->cap.resyncs == 0 &&
           s->play.transfer_errors == 0 && s->play.packet_errors == 0 &&
           s->play.submit_errors == 0 && s->play.resyncs == 0;
}

/*
 * The buffer always starts as silence, so nothing uninitialized can reach the
 * outputs. A tone is added only if all the checks up to now have been clean,
 * and that decision is made once for each tone, as it's due to start.
 */
static void fill_playback(stream *s, transfer *x, uint32_t bytes, uint32_t frames)
{
    int t;

    memset(x->buf, 0, bytes);
    for (t = 0; t < s->opt.tone_count; t++) {
        tone_slot *slot = &s->tones[t];

        if (slot->state == TONE_WAITING && s->out_pos + frames > slot->tone.start) {
            if (tones_are_safe(s)) {
                slot->state = TONE_PLAYING;
                slot->wav_frame_when_started = s->cap.frames_total - s->wav_start_frames;
            } else {
                slot->state = TONE_REFUSED;
            }
        }
        if (slot->state == TONE_PLAYING) {
            if (s->opt.jack_map)
                ua101_jack_mix(&slot->tone, x->buf, frames, s->play.channels, s->play.channels,
                               s->out_pos);
            else
                ua101_tone_mix(&slot->tone, x->buf, frames, s->play.channels, s->out_pos);
            if (s->out_pos + frames >= slot->tone.start + slot->tone.length)
                slot->state = TONE_DONE;
        }
    }
    s->out_pos += frames;
}

/* Sends every idle playback transfer for which the size sequence is available. */
static void pump_playback(stream *s)
{
    pipe_state *p = &s->play;

    if (!s->play_enabled || atomic_load(&s->stop_request))
        return;

    if (!s->play_started) {
        UInt64 bus = 0;
        AbsoluteTime at;

        /* Waits for one queue's worth, because the first ones all go at once. */
        if (s->fifo.count < p->queue * s->packets)
            return;
        if ((*p->intf)->GetBusFrameNumber(p->intf, &bus, &at) != kIOReturnSuccess) {
            fail_stream(s, "couldn't read the bus frame number to start playback");
            return;
        }
        p->next_frame = bus + START_MARGIN_FRAMES;
        s->play_started = 1;
    }

    while (p->idle > 0 && s->fifo.count >= s->packets) {
        transfer *x = &p->transfers[p->next_index];
        uint32_t bytes = 0, frames = 0, i;

        for (i = 0; i < s->packets; i++) {
            uint16_t size = 0;

            ua101_fifo_pop(&s->fifo, &size);
            frames += size;
            bytes += size * p->frame_bytes;
            x->frames[i].frStatus = kIOReturnSuccess;
            x->frames[i].frReqCount = (UInt16)(size * p->frame_bytes);
            x->frames[i].frActCount = 0;
        }
        fill_playback(s, x, bytes, frames);

        if (submit(s, p, x, 0, playback_done) != kIOReturnSuccess) {
            fail_stream(s, "a playback transfer couldn't be submitted");
            return;
        }
        /* Here only counted. The driver must place a transfer before filling it, so a submit
           that's moved to a later bus frame will have to be filled again. */
        ua101_timeline_place(&p->timeline, x->start_frame, s->bus_frames_per_transfer, frames);
        p->frames_total += frames;
        p->next_index = (p->next_index + 1) % p->queue;
        p->idle--;
    }
}

static void playback_done(void *refcon, IOReturn result, void *arg0)
{
    transfer *x = refcon;
    stream *s = x->owner;
    pipe_state *p = &s->play;
    uint32_t i;

    note_completion(s, p, x, result, arg0, mach_absolute_time());
    if (status_ok(result)) {
        for (i = 0; i < s->packets; i++) {
            if (!status_ok(x->frames[i].frStatus)) {
                p->packet_errors++;
                p->last_packet_error = x->frames[i].frStatus;
            } else if (x->frames[i].frActCount != x->frames[i].frReqCount) {
                p->short_writes++;
            }
        }
    }
    p->idle++;
    pump_playback(s);
}

static void keep_for_dump(stream *s, const transfer *x)
{
    size_t bytes = (size_t)s->packets * s->cap.max_packet;

    if (s->dump_count >= s->opt.dump_transfers)
        return;
    memcpy(s->dump_data + s->dump_count * bytes, x->buf, bytes);
    memcpy(s->dump_frames + s->dump_count * s->packets, x->frames,
           s->packets * sizeof(IOUSBIsocFrame));
    s->dump_count++;
}

/* first_frame is the packet's first frame, counted from the start of capture. */
static void check_low_bytes(stream *s, const uint8_t *data, uint32_t bytes, uint64_t first_frame)
{
    uint32_t channels = s->cap.frame_bytes / 4, i;
    uint64_t bad = 0, last_bad_frame = UINT64_MAX;

    for (i = 0; i + 4 <= bytes; i += 4) {
        uint64_t frame;
        uint32_t ch;
        size_t second;

        if (data[i] == 0)
            continue;
        bad++;
        frame = first_frame + i / s->cap.frame_bytes;
        ch = (i / 4) % channels;
        if (ch < 32)
            s->low_channels |= UINT32_C(1) << ch;
        if (frame == last_bad_frame)
            continue;
        last_bad_frame = frame;
        if (s->low_frames == 0)
            s->low_first_frame = frame;
        s->low_last_frame = frame;
        s->low_frames++;
        second = s->rate_hz > 0.0 ? (size_t)((double)frame / s->rate_hz) : 0;
        s->low_by_second[second < LOW_SECONDS ? second : LOW_SECONDS - 1]++;
    }
    if (bad != 0)
        s->low_byte_nonzero += bad;
}

static void record_frames(stream *s, const uint8_t *data, uint32_t bytes)
{
    if (s->wav_fd < 0)
        return;
    if (!ua101_ring_write(&s->ring, data, bytes))
        s->ring_dropped_bytes += bytes;
}

static void decide_layout(stream *s)
{
    ua101_layout verdict;

    if (s->layout != UA101_LAYOUT_UNKNOWN || ++s->layout_transfers < LAYOUT_PROBE_TRANSFERS)
        return;
    verdict = ua101_layout_verdict(&s->layout_stats);
    if (verdict == UA101_LAYOUT_UNKNOWN) {
        fail_stream(s, "the capture buffer layout is neither of the expected ones");
        return;
    }
    s->layout = verdict;
    s->wav_start_frames = s->cap.frames_total;
    s->wav_started = 1;
}

static void process_capture(stream *s, transfer *x, uint64_t now)
{
    pipe_state *p = &s->cap;
    uint16_t actual[MAX_PACKETS];
    uint32_t i, packed_offset = 0, xfer_frames = 0, damaged = 0;

    keep_for_dump(s, x);

    for (i = 0; i < s->packets; i++) {
        IOReturn status = x->frames[i].frStatus;
        uint32_t act = x->frames[i].frActCount;

        if (!status_ok(status)) {
            /* A damaged packet counts as no frames, which keeps playback in step. */
            p->packet_errors++;
            p->last_packet_error = status;
            act = 0;
            damaged++;
        } else if (act > p->max_packet) {
            s->oversize_packets++;
            act = 0;
            damaged++;
        } else if (act % p->frame_bytes != 0) {
            s->misaligned_packets++;
            act -= act % p->frame_bytes;
            damaged++;
        }
        actual[i] = (uint16_t)act;
    }
    ua101_layout_check(x->buf, s->packets, p->max_packet, actual, &s->layout_stats);

    for (i = 0; i < s->packets; i++) {
        uint32_t act = actual[i];
        uint32_t frames = act / p->frame_bytes;

        s->size_hist[frames < SIZE_HIST_BINS ? frames : SIZE_HIST_BINS - 1]++;
        xfer_frames += frames;

        if (s->fifo_open)
            ua101_fifo_push(&s->fifo, (uint16_t)frames);

        if (s->layout != UA101_LAYOUT_UNKNOWN && act != 0) {
            const uint8_t *data = x->buf + (s->layout == UA101_LAYOUT_SLOT
                                                ? (size_t)i * p->max_packet
                                                : (size_t)packed_offset);

            check_low_bytes(s, data, act, p->frames_total + xfer_frames - frames);
            record_frames(s, data, act);
        }
        packed_offset += act;
    }
    p->frames_total += xfer_frames;

    /* As the driver will: where this transfer's audio belongs in time. One with a damaged
       packet is left out, so the next one finds the gap and time is kept. */
    if (damaged == 0) {
        uint64_t first = ua101_timeline_place(&p->timeline, x->start_frame,
                                              s->bus_frames_per_transfer, xfer_frames);

        if (s->check_bus != NULL && s->check_count < s->check_capacity) {
            s->check_bus[s->check_count] = x->start_frame;
            s->check_frame[s->check_count] = first;
            s->check_count++;
        }
    }

    /* Rate, measured against the bus clock and against the host clock. */
    if (!s->have_baseline && p->completed * s->opt.transfer_ms >= RATE_BASELINE_SECONDS * 1000.0) {
        s->have_baseline = 1;
        s->base_host = now;
        s->base_bus = x->start_frame;
        s->base_frames = p->frames_total;
        s->have_bus_times = read_bus_time(p, &s->stamp_base_bus, &s->stamp_base_host);
    }
    s->last_host = now;
    s->last_bus = x->start_frame;
    s->last_frames = p->frames_total;

    if (s->clock_log != NULL && s->clock_log_count < s->clock_log_capacity &&
        x->start_frame >= s->clock_log_next_bus) {
        clock_sample *c = &s->clock_log[s->clock_log_count];

        c->bus = x->start_frame;
        c->frames = p->frames_total;
        if (read_bus_time(p, &c->stamp_bus, &c->stamp_host))
            s->clock_log_count++;
        s->clock_log_next_bus = x->start_frame + 1000;
    }

    decide_layout(s);
}

static void capture_done(void *refcon, IOReturn result, void *arg0)
{
    transfer *x = refcon;
    stream *s = x->owner;
    pipe_state *p = &s->cap;
    uint64_t now = mach_absolute_time();

    note_completion(s, p, x, result, arg0, now);

    /* A failed transfer has no usable frame list. Keep the sizes in step anyway. */
    if (status_ok(result)) {
        process_capture(s, x, now);
    } else if (!(result == kIOReturnAborted && atomic_load(&s->stop_request))) {
        uint32_t i;

        if (s->fifo_open)
            for (i = 0; i < s->packets; i++)
                ua101_fifo_push(&s->fifo, 0);
    }

    if (s->play_enabled && !s->fifo_open && now >= s->play_open_host) {
        s->fifo_open = 1;
        s->cap_frames_at_fifo_open = p->frames_total;
    }

    /* --stall-test: hold this transfer back once, as a slow moment in the system would. The
       ones still queued finish meanwhile, so the stream has to start again later. */
    if (s->opt.stall_test_ms > 0 && !s->stall_test_done &&
        (double)p->completed * s->opt.transfer_ms >= STALL_TEST_AT_SECONDS * 1000.0) {
        s->stall_test_done = 1;
        usleep(s->opt.stall_test_ms * 1000);
    }

    if (!atomic_load(&s->stop_request)) {
        if (submit_capture(s, x) != kIOReturnSuccess)
            fail_stream(s, "a capture transfer couldn't be resubmitted");
    }
    pump_playback(s);
}

static void set_realtime(stream *s)
{
    thread_time_constraint_policy_data_t policy;
    kern_return_t result;
    double period = s->opt.transfer_ms / 1000.0;

    policy.period = (uint32_t)seconds_to_host(s, period);
    policy.computation = (uint32_t)seconds_to_host(s, 0.0005);
    policy.constraint = (uint32_t)seconds_to_host(s, period);
    policy.preemptible = TRUE;
    result = thread_policy_set(pthread_mach_thread_np(pthread_self()),
                               THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&policy,
                               THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    if (result != KERN_SUCCESS)
        fail_stream(s, "couldn't get a real-time thread");
}

static void *usb_thread(void *arg)
{
    stream *s = arg;
    CFRunLoopRef loop = CFRunLoopGetCurrent();
    uint64_t stop_seen = 0;
    int aborted = 0;
    UInt64 bus = 0;
    AbsoluteTime at;
    uint32_t i;

    pthread_setname_np("ua101 usb");
    set_realtime(s);

    CFRunLoopAddSource(loop, s->cap.source, kCFRunLoopDefaultMode);
    if (s->play_enabled)
        CFRunLoopAddSource(loop, s->play.source, kCFRunLoopDefaultMode);

    if ((*s->cap.intf)->GetBusFrameNumber(s->cap.intf, &bus, &at) != kIOReturnSuccess) {
        fail_stream(s, "couldn't read the bus frame number to start capture");
    } else {
        s->cap.next_frame = bus + START_MARGIN_FRAMES;
        s->play_open_host = mach_absolute_time() + seconds_to_host(s, PLAYBACK_DELAY_SECONDS);
        for (i = 0; i < s->cap.queue && !atomic_load(&s->stop_request); i++) {
            transfer *x = &s->cap.transfers[i];

            if (submit_capture(s, x) != kIOReturnSuccess)
                fail_stream(s, "a capture transfer couldn't be submitted");
        }
    }

    for (;;) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, false);
        if (!atomic_load(&s->stop_request))
            continue;

        if (s->cap.inflight == 0 && s->play.inflight == 0)
            break;
        if (stop_seen == 0)
            stop_seen = mach_absolute_time();
        else if (host_seconds(s, mach_absolute_time() - stop_seen) > 2.0) {
            s->gave_up = 1;
            break;
        } else if (!aborted && host_seconds(s, mach_absolute_time() - stop_seen) > 0.25) {
            aborted = 1;
            (*s->cap.intf)->AbortPipe(s->cap.intf, s->cap.pipe);
            if (s->play_enabled)
                (*s->play.intf)->AbortPipe(s->play.intf, s->play.pipe);
        }
    }

    if (s->have_bus_times &&
        !read_bus_time(&s->cap, &s->stamp_last_bus, &s->stamp_last_host))
        s->have_bus_times = 0;

    CFRunLoopRemoveSource(loop, s->cap.source, kCFRunLoopDefaultMode);
    if (s->play_enabled)
        CFRunLoopRemoveSource(loop, s->play.source, kCFRunLoopDefaultMode);
    return NULL;
}

static void *writer_thread(void *arg)
{
    stream *s = arg;

    pthread_setname_np("ua101 writer");
    for (;;) {
        const uint8_t *data = NULL;
        size_t available = ua101_ring_peek(&s->ring, &data);
        ssize_t written;

        if (available == 0) {
            if (atomic_load(&s->writer_stop))
                break;
            usleep(5000);
            continue;
        }
        written = write(s->wav_fd, data, available);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            s->write_errno = errno;
            ua101_ring_consume(&s->ring, available);
            continue;
        }
        s->wav_data_bytes += (unsigned long long)written;
        ua101_ring_consume(&s->ring, (size_t)written);
    }
    return NULL;
}

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
    kern_return_t result;

    result = IOCreatePlugInInterfaceForService(service, kIOUSBDeviceUserClientTypeID,
                                               kIOCFPlugInInterfaceID, &plugin, &score);
    if (result != KERN_SUCCESS || plugin == NULL)
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
    kern_return_t result;

    result = IOCreatePlugInInterfaceForService(service, kIOUSBInterfaceUserClientTypeID,
                                               kIOCFPlugInInterfaceID, &plugin, &score);
    if (result != KERN_SUCCESS || plugin == NULL)
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

/* Opens an interface, selects its streaming setting and finds the pipe. */
static int prepare_pipe(stream *s, pipe_state *p, device_ref device, const ua101_config *config,
                        UInt8 number, const char *name)
{
    const ua101_alt *alt = ua101_find_alt(config, number, 1);
    IOReturn result;
    UInt8 count = 0, pipe;

    p->name = name;
    p->min_headroom = INT64_MAX;
    if (alt == NULL || alt->num_endpoints != 1 || !alt->has_format) {
        printf("  %s: the descriptor doesn't describe one streaming endpoint.\n", name);
        return 0;
    }
    if (alt->subframe_bytes != 4) {
        printf("  %s: %u-byte samples aren't handled yet; only 4-byte ones have been seen.\n",
               name, alt->subframe_bytes);
        return 0;
    }
    p->max_packet = alt->endpoints[0].max_packet;
    p->channels = alt->channels;
    p->frame_bytes = (uint32_t)alt->channels * alt->subframe_bytes;
    if (s->rate_hz == 0.0)
        s->rate_hz = alt->rate_hz;
    else if (s->rate_hz != alt->rate_hz) {
        printf("  Playback and capture have different rates.\n");
        return 0;
    }

    p->intf = find_interface(device, number);
    if (p->intf == NULL) {
        printf("  %s: couldn't find interface %u.\n", name, number);
        return 0;
    }
    result = (*p->intf)->USBInterfaceOpen(p->intf);
    if (result != kIOReturnSuccess) {
        printf("  %s: couldn't open the interface: %s\n", name, describe(result));
        (*p->intf)->Release(p->intf);
        p->intf = NULL;
        return 0;
    }
    result = (*p->intf)->CreateInterfaceAsyncEventSource(p->intf, &p->source);
    if (result != kIOReturnSuccess) {
        printf("  %s: couldn't create the event source: %s\n", name, describe(result));
        return 0;
    }
    result = (*p->intf)->SetAlternateInterface(p->intf, 1);
    if (result != kIOReturnSuccess) {
        printf("  %s: couldn't select the streaming setting: %s\n", name, describe(result));
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
            printf("  %s: pipe %u, endpoint 0x%02x, max packet %u, interval %u, %u channels\n",
                   name, pipe, alt->endpoints[0].address, max_packet, interval, alt->channels);
        }
    }
    if (p->pipe == 0) {
        printf("  %s: no pipe matches the descriptor.\n", name);
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

static int allocate_transfers(stream *s, pipe_state *p, unsigned queue)
{
    size_t page = (size_t)getpagesize();
    size_t bytes = (size_t)s->packets * p->max_packet;
    unsigned i;

    bytes = (bytes + page - 1) / page * page;
    p->queue = queue;
    for (i = 0; i < queue; i++) {
        transfer *x = &p->transfers[i];

        if (posix_memalign((void **)&x->buf, page, bytes) != 0)
            return 0;
        memset(x->buf, 0, bytes);
        mlock(x->buf, bytes);
        x->owner = s;
        x->index = i;
    }
    p->idle = queue;
    return 1;
}

/* The clock gets its own verdict, separate from streaming (a design choice, 2026-11-07).
   The limit itself is unchanged. A clock outside it needs rate correction to stay in step
   with other devices, but it doesn't mean any audio was lost. */
typedef enum { CLOCK_INSIDE, CLOCK_OUTSIDE, CLOCK_UNDECIDED } clock_verdict;

/* A run too short to decide isn't outside the limit, but it isn't inside either. */
static void judge_rate(double ppm, double margin, clock_verdict *clock)
{
    switch (ua101_rate_verdict(ppm, margin, RATE_LIMIT_PPM)) {
    case UA101_RATE_OUTSIDE:
        printf("  Outside the %.0f ppm limit.\n", RATE_LIMIT_PPM);
        *clock = CLOCK_OUTSIDE;
        break;
    case UA101_RATE_UNDECIDED:
        printf("  Too short to judge against the %.0f ppm limit.\n", RATE_LIMIT_PPM);
        if (*clock != CLOCK_OUTSIDE)
            *clock = CLOCK_UNDECIDED;
        break;
    case UA101_RATE_INSIDE:
        break;
    }
}

static void write_clock_log(const stream *s)
{
    FILE *f = fopen(s->opt.clock_log_path, "w");
    uint32_t i;

    if (f == NULL) {
        printf("\nCouldn't write the clock log to %s: %s\n", s->opt.clock_log_path, strerror(errno));
        return;
    }
    fprintf(f, "bus_frame,frames,stamp_bus_frame,stamp_host_seconds\n");
    for (i = 0; i < s->clock_log_count; i++) {
        const clock_sample *c = &s->clock_log[i];

        fprintf(f, "%llu,%llu,%llu,%.9f\n", (unsigned long long)c->bus,
                (unsigned long long)c->frames, (unsigned long long)c->stamp_bus,
                host_seconds(s, c->stamp_host - s->clock_log[0].stamp_host));
    }
    fclose(f);
    printf("\nClock log: %u readings in %s\n", s->clock_log_count, s->opt.clock_log_path);
}

static double wall_seconds(void)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
}

/* --transfer-log: the real packet pattern, for replaying gaps offline where the answer is known. */
static void write_transfer_log(const stream *s)
{
    FILE *f = fopen(s->opt.transfer_log_path, "w");
    uint32_t i;

    if (f == NULL) {
        printf("\nCouldn't write the transfer log to %s: %s\n", s->opt.transfer_log_path,
               strerror(errno));
        return;
    }
    fprintf(f, "bus_frame,first_frame\n");
    for (i = 0; i < s->check_count; i++)
        fprintf(f, "%llu,%llu\n", (unsigned long long)s->check_bus[i],
                (unsigned long long)s->check_frame[i]);
    fclose(f);
    printf("\nTransfer log: %u transfers in %s\n", s->check_count, s->opt.transfer_log_path);
}

/* The Mac's timer drifts with heat, so each time is placed between the wall clock readings
   at both ends of the run. That keeps it close enough to match against the system log. */
static void write_stall_log(const stream *s)
{
    FILE *f = fopen(s->opt.stall_log_path, "w");
    uint64_t host_end = mach_absolute_time();
    double wall_end = wall_seconds();
    double span = (double)(host_end - s->stall_host_start);
    uint32_t i;

    if (f == NULL) {
        printf("\nCouldn't write the stall log to %s: %s\n", s->opt.stall_log_path, strerror(errno));
        return;
    }
    fprintf(f, "time,kind,ms\n");
    for (i = 0; i < s->stall_log_count; i++) {
        const stall_event *e = &s->stall_log[i];
        double wall = s->stall_wall_start +
                      (double)(e->when - s->stall_host_start) / span * (wall_end - s->stall_wall_start);
        time_t whole = (time_t)wall;
        struct tm tm;
        char text[16];

        localtime_r(&whole, &tm);
        strftime(text, sizeof text, "%H:%M:%S", &tm);
        fprintf(f, "%s.%03d,%c,%.2f\n", text, (int)((wall - (double)whole) * 1000.0), e->kind,
                host_seconds(s, e->length) * 1000.0);
    }
    fclose(f);
    printf("\nStall log: %u events in %s; %llu not stored\n", s->stall_log_count,
           s->opt.stall_log_path, (unsigned long long)s->stall_log_dropped);
}

static void usage(void)
{
    printf("Usage: ua101stream [options]\n"
           "  --seconds N        how long to run (default 10)\n"
           "  --capture-only     leave the playback interface alone\n"
           "  --wav PATH         where to record (default /tmp/ua101/capture.wav)\n"
           "  --no-wav           don't record\n"
           "  --transfer-ms N    length of each transfer (default 2)\n"
           "  --capture-queue N  transfers queued for capture (default %d)\n"
           "  --playback-queue N transfers queued for playback (default %d)\n"
           "  --queue N          the same number for both\n"
           "  --tone-at SEC      play a tone this far into playback; up to %d times\n"
           "  --tone-db DB       tone level (default -60, limit %.0f)\n"
           "  --tone-seconds S   tone length (default 1, limit %.0f)\n"
           "  --jack-map         each tone plays on every output at once, each at its own\n"
           "                     frequency, for ua101jackmap; same level and length limits\n"
           "  --dump N           print the raw contents of the first N capture transfers\n"
           "  --clock-log FILE   save one clock reading per second as CSV, after the run\n"
           "  --stall-log FILE   save the time of each long gap, slow submit and resync, after the run\n"
           "  --transfer-log FILE save each capture transfer's bus frame and timeline frame\n"
           "                     number as CSV, after the run\n"
           "  --keep-device-open keep the device, not just the interfaces, open\n"
           "  --stall-test MS    with --capture-only: hold capture up once, %.0f s in, for MS ms\n"
           "                     (up to %d), and check that the audio after the gap keeps its\n"
           "                     place in time\n"
           "Exit status: 0 if streaming passes and the clock is inside the limit, 2 if streaming\n"
           "passes but the clock is outside the limit or the run is too short to judge, 1 if\n"
           "streaming fails. With --stall-test: 0 if the gap is in place and nothing else went\n"
           "wrong, 1 if not.\n",
           DEFAULT_CAPTURE_QUEUE, DEFAULT_PLAYBACK_QUEUE, MAX_TONES, UA101_TONE_MAX_DBFS,
           UA101_TONE_MAX_SECONDS, STALL_TEST_AT_SECONDS, STALL_TEST_MAX_MS);
}

static int parse_options(int argc, char **argv, options *opt)
{
    int i;

    opt->seconds = 10.0;
    opt->wav_path = "/tmp/ua101/capture.wav";
    opt->transfer_ms = 2;
    opt->capture_queue = DEFAULT_CAPTURE_QUEUE;
    opt->playback_queue = DEFAULT_PLAYBACK_QUEUE;
    opt->tone_db = -60.0;
    opt->tone_seconds = 1.0;

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];
        const char *value = (i + 1 < argc) ? argv[i + 1] : NULL;

        if (strcmp(arg, "--capture-only") == 0) {
            opt->capture_only = 1;
        } else if (strcmp(arg, "--no-wav") == 0) {
            opt->wav_path = NULL;
        } else if (strcmp(arg, "--keep-device-open") == 0) {
            opt->keep_device_open = 1;
        } else if (strcmp(arg, "--jack-map") == 0) {
            opt->jack_map = 1;
        } else if (value == NULL) {
            usage();
            return 0;
        } else if (strcmp(arg, "--seconds") == 0) {
            opt->seconds = atof(value);
            i++;
        } else if (strcmp(arg, "--wav") == 0) {
            opt->wav_path = value;
            i++;
        } else if (strcmp(arg, "--transfer-ms") == 0) {
            opt->transfer_ms = (unsigned)atoi(value);
            i++;
        } else if (strcmp(arg, "--queue") == 0) {
            opt->capture_queue = opt->playback_queue = (unsigned)atoi(value);
            i++;
        } else if (strcmp(arg, "--capture-queue") == 0) {
            opt->capture_queue = (unsigned)atoi(value);
            i++;
        } else if (strcmp(arg, "--playback-queue") == 0) {
            opt->playback_queue = (unsigned)atoi(value);
            i++;
        } else if (strcmp(arg, "--tone-at") == 0) {
            if (opt->tone_count >= MAX_TONES) {
                usage();
                return 0;
            }
            opt->tone_at[opt->tone_count++] = atof(value);
            i++;
        } else if (strcmp(arg, "--tone-db") == 0) {
            opt->tone_db = atof(value);
            i++;
        } else if (strcmp(arg, "--tone-seconds") == 0) {
            opt->tone_seconds = atof(value);
            i++;
        } else if (strcmp(arg, "--dump") == 0) {
            opt->dump_transfers = (unsigned)atoi(value);
            i++;
        } else if (strcmp(arg, "--clock-log") == 0) {
            opt->clock_log_path = value;
            i++;
        } else if (strcmp(arg, "--stall-log") == 0) {
            opt->stall_log_path = value;
            i++;
        } else if (strcmp(arg, "--transfer-log") == 0) {
            opt->transfer_log_path = value;
            i++;
        } else if (strcmp(arg, "--stall-test") == 0) {
            int ms = atoi(value);

            if (ms < 1 || ms > STALL_TEST_MAX_MS) {
                printf("--stall-test takes 1 to %d ms.\n", STALL_TEST_MAX_MS);
                return 0;
            }
            opt->stall_test_ms = (unsigned)ms;
            i++;
        } else {
            usage();
            return 0;
        }
    }

    if (!(opt->seconds > 0.0) || opt->transfer_ms < 1 || opt->transfer_ms > 8 ||
        opt->capture_queue < 2 || opt->capture_queue > MAX_QUEUE || opt->playback_queue < 2 ||
        opt->playback_queue > MAX_QUEUE) {
        printf("--seconds must be above 0, --transfer-ms 1 to 8, and each queue 2 to %d.\n",
               MAX_QUEUE);
        return 0;
    }
    if (opt->capture_only && opt->tone_count > 0) {
        printf("A tone needs playback, so it can't be combined with --capture-only.\n");
        return 0;
    }
    if (opt->jack_map && opt->tone_count == 0) {
        printf("--jack-map changes what --tone-at plays, so it needs at least one --tone-at.\n");
        return 0;
    }
    /* Capture only, so a stall test never sends anything out. */
    if (opt->stall_test_ms > 0 &&
        (!opt->capture_only || opt->seconds < STALL_TEST_AT_SECONDS + 4.0)) {
        printf("--stall-test needs --capture-only and a run of at least %.0f s.\n",
               STALL_TEST_AT_SECONDS + 4.0);
        return 0;
    }
    return 1;
}

static int prepare_recording(stream *s)
{
    const char *path = s->opt.wav_path;
    uint8_t header[UA101_WAV_HEADER_BYTES];
    double need;
    struct statfs fs;
    char *dir, *slash;

    if (path == NULL)
        return 1;

    need = s->opt.seconds * s->rate_hz * s->cap.frame_bytes;
    if (need > (double)UA101_WAV_MAX_DATA_BYTES) {
        printf("That's too long for one WAV file. Use fewer seconds or --no-wav.\n");
        return 0;
    }

    dir = strdup(path);
    slash = dir != NULL ? strrchr(dir, '/') : NULL;
    if (slash != NULL && slash != dir) {
        *slash = '\0';
        mkdir(dir, 0755);
        if (statfs(dir, &fs) == 0 &&
            (double)fs.f_bavail * fs.f_bsize < need + 1024.0 * 1024.0 * 1024.0) {
            printf("Not enough free disk space in %s.\n", dir);
            free(dir);
            return 0;
        }
    }
    free(dir);

    s->wav_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (s->wav_fd < 0) {
        printf("Couldn't create %s: %s\n", path, strerror(errno));
        return 0;
    }
    /* A placeholder until the size is known. */
    ua101_wav_header(header, (uint16_t)s->cap.channels, (uint32_t)s->rate_hz, 32, 24, 0);
    if (write(s->wav_fd, header, sizeof(header)) != (ssize_t)sizeof(header)) {
        printf("Couldn't write to %s: %s\n", path, strerror(errno));
        return 0;
    }

    /* Touched now so the real-time thread never takes a page fault on it. */
    s->ring_data = malloc(RING_BYTES);
    if (s->ring_data == NULL) {
        printf("Out of memory.\n");
        return 0;
    }
    memset(s->ring_data, 0, RING_BYTES);
    mlock(s->ring_data, RING_BYTES);
    ua101_ring_init(&s->ring, s->ring_data, RING_BYTES);
    return 1;
}

static void finish_recording(stream *s)
{
    uint8_t header[UA101_WAV_HEADER_BYTES];
    int t;

    if (s->wav_fd < 0)
        return;
    ua101_wav_header(header, (uint16_t)s->cap.channels, (uint32_t)s->rate_hz, 32, 24,
                     s->wav_data_bytes);
    if (pwrite(s->wav_fd, header, sizeof(header), 0) != (ssize_t)sizeof(header))
        s->write_errno = errno;
    close(s->wav_fd);
    s->wav_fd = -1;

    /*
     * Tells ua101wavcheck, or for a jack map ua101jackmap, what was sent. Each removes
     * the other's file, so a file left from an earlier run can't be mistaken for this one.
     */
    if (s->opt.tone_count > 0) {
        char path[1024], other[1024];
        FILE *file;

        snprintf(path, sizeof(path), "%s.%s", s->opt.wav_path, s->opt.jack_map ? "jack" : "tones");
        snprintf(other, sizeof(other), "%s.%s", s->opt.wav_path, s->opt.jack_map ? "tones" : "jack");
        unlink(other);
        file = fopen(path, "w");
        if (file != NULL) {
            for (t = 0; t < s->opt.tone_count; t++) {
                const tone_slot *slot = &s->tones[t];
                unsigned ch, outputs;

                if (slot->state != TONE_DONE)
                    continue;
                if (!s->opt.jack_map) {
                    fprintf(file, "%.3f %.3f %.3f %.3f %llu %llu\n", s->opt.tone_db,
                            s->opt.tone_seconds, slot->tone.freq_hz[0], slot->tone.freq_hz[1],
                            (unsigned long long)slot->tone.start,
                            (unsigned long long)slot->wav_frame_when_started);
                    continue;
                }
                /* Level, length, start, its recording frame, then each output's frequency. */
                outputs = s->play.channels < UA101_JACK_OUTPUTS ? s->play.channels
                                                                : UA101_JACK_OUTPUTS;
                fprintf(file, "%.3f %.3f %llu %llu %u", s->opt.tone_db, s->opt.tone_seconds,
                        (unsigned long long)slot->tone.start,
                        (unsigned long long)slot->wav_frame_when_started, outputs);
                for (ch = 0; ch < outputs; ch++)
                    fprintf(file, " %.3f", ua101_jack_freq(ch));
                fprintf(file, "\n");
            }
            fclose(file);
        }
    }
}

static void print_dump(const stream *s)
{
    size_t bytes = (size_t)s->packets * s->cap.max_packet;
    uint32_t t, p;

    if (s->dump_count == 0)
        return;
    printf("\nRaw capture transfers (sentinel word is %08x, shown as ........):\n",
           UA101_SENTINEL_WORD);
    for (t = 0; t < s->dump_count; t++) {
        const IOUSBIsocFrame *frames = s->dump_frames + (size_t)t * s->packets;
        const uint8_t *data = s->dump_data + (size_t)t * bytes;

        printf("Transfer %u: bytes", t);
        for (p = 0; p < s->packets; p++)
            printf(" %u", frames[p].frActCount);
        printf("\n  status");
        for (p = 0; p < s->packets; p++)
            printf(" %x", (unsigned)frames[p].frStatus);
        printf("\n");
        /* The first two slots in full, which is where the layouts differ. */
        for (p = 0; p < 2 && p < s->packets; p++) {
            size_t word;

            for (word = 0; word < s->cap.max_packet / 4; word++) {
                uint32_t value;

                memcpy(&value, data + (size_t)p * s->cap.max_packet + word * 4, 4);
                if (word % 12 == 0)
                    printf("  slot %u +%3zu:", p, word * 4);
                if (value == UA101_SENTINEL_WORD)
                    printf(" ........");
                else
                    printf(" %08x", value);
                if (word % 12 == 11)
                    printf("\n");
            }
            if (s->cap.max_packet / 4 % 12 != 0)
                printf("\n");
        }
    }
}

static void print_hist(const char *label, const uint64_t *hist, unsigned bins,
                       const char *const *names)
{
    unsigned i;

    printf("  %s:", label);
    for (i = 0; i < bins; i++)
        if (hist[i] != 0)
            printf(" %s=%llu", names[i], (unsigned long long)hist[i]);
    printf("\n");
}

static void print_pipe_report(const stream *s, const pipe_state *p)
{
    static const char *const gap_names[GAP_HIST_BINS] = {"<1ms", "1-2",  "2-3",  "3-4",
                                                         "4-6",  "6-10", "10-20", ">20ms"};
    char names_store[HEADROOM_HIST_BINS][8];
    const char *names[HEADROOM_HIST_BINS];
    unsigned i;

    for (i = 0; i < HEADROOM_HIST_BINS; i++) {
        snprintf(names_store[i], sizeof(names_store[i]), "%u", i);
        names[i] = names_store[i];
    }

    printf("\n%s:\n", p->name);
    printf("  Transfers %llu, frames %llu\n", (unsigned long long)p->completed,
           (unsigned long long)p->frames_total);
    printf("  Errors: transfer %llu, packet %llu, submit %llu, resync %llu, order %llu\n",
           (unsigned long long)p->transfer_errors, (unsigned long long)p->packet_errors,
           (unsigned long long)p->submit_errors, (unsigned long long)p->resyncs,
           (unsigned long long)p->order_errors);
    if (p->skipped_frames != 0)
        printf("  Resyncs skipped %llu ms of bus time, so that much audio was lost\n",
               (unsigned long long)p->skipped_frames);
    if (p->timeline.gaps == 0 && p->timeline.overlaps == 0)
        printf("  Timeline, as the driver will keep it: no gaps\n");
    else
        printf("  Timeline, as the driver will keep it: %llu gaps over %llu ms of bus time, with\n"
               "  %llu frames put in them by the clock (%llu gaps before it had a line); %llu "
               "overlaps\n",
               (unsigned long long)p->timeline.gaps,
               (unsigned long long)p->timeline.gap_bus_frames,
               (unsigned long long)p->timeline.gap_frames,
               (unsigned long long)p->timeline.early_gaps,
               (unsigned long long)p->timeline.overlaps);
    if (p->last_error != 0)
        printf("  Last transfer error: %s\n", describe(p->last_error));
    if (p->last_packet_error != 0)
        printf("  Last packet error: %s\n", describe(p->last_packet_error));
    print_hist("Time between completions", p->gap_hist, GAP_HIST_BINS, gap_names);
    printf("  Longest gap %.2f ms; slowest submit %.0f us\n",
           host_seconds(s, p->max_gap) * 1000.0, host_seconds(s, p->max_submit) * 1e6);
    print_hist("Bus frames of headroom at submit", p->headroom_hist, HEADROOM_HIST_BINS, names);
    if (p->min_headroom != INT64_MAX)
        printf("  Least headroom %lld ms\n", (long long)p->min_headroom);
}

static uint64_t error_count(const stream *s)
{
    /*
     * Every capture packet carries audio at every rate, so an empty one is lost audio,
     * even with no USB error and a clean recording: losing whole frames doesn't break
     * the monitor mix sum, and barely moves the rate over a long run.
     */
    uint64_t total = s->cap.transfer_errors + s->cap.packet_errors + s->cap.submit_errors +
                     s->cap.resyncs + s->cap.order_errors + s->misaligned_packets +
                     s->oversize_packets + s->low_byte_nonzero + s->ring_dropped_bytes +
                     s->size_hist[0];

    if (s->play_enabled)
        total += s->play.transfer_errors + s->play.packet_errors + s->play.submit_errors +
                 s->play.resyncs + s->play.order_errors + s->play.short_writes +
                 s->fifo.dropped_packets;
    if (s->layout == UA101_LAYOUT_SLOT)
        total += s->layout_stats.slot_bad_words;
    else if (s->layout == UA101_LAYOUT_PACKED)
        total += s->layout_stats.packed_bad_words;
    return total;
}

/*
 * --stall-test: whether the audio after the gap kept its place in time. The line through the
 * transfers before the gap comes from frames that were counted, not from the timeline's estimate,
 * so it checks that estimate independently. The check itself is good to about 0.05 frames, so
 * the offset is judged in whole frames. The frames in a gap are never seen, so the timeline
 * promises the place within one frame, not exactly. Returns 2 if exact, 1 if one frame off, and
 * 0 if further off or not checked.
 */
static int report_stall_test(const stream *s)
{
    const ua101_timeline *t = &s->cap.timeline;
    uint32_t gap = 0, i, before, after;
    double offset, ms = (double)s->opt.transfer_ms;

    printf("\nStall test:\n");
    for (i = 1; i < s->check_count; i++)
        if (s->check_bus[i] != s->check_bus[i - 1] + s->bus_frames_per_transfer) {
            gap = i;
            break;
        }
    if (gap == 0) {
        printf("  Capture was held up for %u ms, but no gap followed, so nothing was checked.\n",
               s->opt.stall_test_ms);
        return 0;
    }
    before = gap < CHECK_TRANSFERS ? gap : CHECK_TRANSFERS;
    after = s->check_count - gap < CHECK_TRANSFERS ? s->check_count - gap : CHECK_TRANSFERS;
    offset = ua101_timeline_gap_offset(s->check_bus + gap - before, s->check_frame + gap - before,
                                       before, before + after);

    printf("  Capture was held up for %u ms, %.0f s in. The stream started again %llu ms of bus\n"
           "  time later than it would have, and the timeline put %llu frames in %s.\n",
           s->opt.stall_test_ms, STALL_TEST_AT_SECONDS,
           (unsigned long long)(s->check_bus[gap] - s->check_bus[gap - 1] -
                                s->bus_frames_per_transfer),
           (unsigned long long)t->gap_frames, t->gaps == 1 ? "the gap" : "its gaps");
    printf("  After the gap, frame numbers sit %+.3f frames from the line through the %.1f s\n"
           "  before it (over %.1f s after it). Counting on without the timeline, they would\n"
           "  sit %+.1f frames off.\n",
           offset, before * ms / 1000.0, after * ms / 1000.0, offset - (double)t->gap_frames);
    /* The line says how many frames really fit the gap; set against the clock's estimate before
       it was rounded, that shows whether a miss was rounding near a half frame. */
    printf("  The clock's estimate before rounding was %.3f frames; the line puts it at %.3f.\n",
           t->last_gap_estimate, (double)t->gap_frames - offset);
    if (t->gaps != 1)
        printf("  There were %llu gaps, not 1, so the second figure is only a guide.\n",
               (unsigned long long)t->gaps);
    if (before < 250 || after < 250) {
        printf("  Too few transfers either side to judge.\n");
        return 0;
    }
    if (fabs(offset - round(offset)) > 0.2)
        printf("  The offset isn't near a whole frame, so the unit's own timing moved at the gap.\n");
    return round(offset) == 0.0 ? 2 : fabs(round(offset)) == 1.0 ? 1 : 0;
}

static int report(stream *s, double elapsed)
{
    static const char *const size_names[SIZE_HIST_BINS] = {"0", "1", "2",  "3",  "4",  "5",  "6", "7",
                                                           "8", "9", "10", "11", "12", "13", "14", "15+"};
    int pass = 1;
    clock_verdict clock = CLOCK_INSIDE;
    int t;
    const char *fatal = atomic_load(&s->fatal);

    printf("\n=== Results after %.1f s ===\n", elapsed);

    printf("\nBuffer layout:\n");
    printf("  Verdict: %s\n", ua101_layout_string(s->layout));
    printf("  %llu packets, %llu short, %llu data bytes; sentinel disagreements: slot %llu, "
           "packed %llu\n",
           (unsigned long long)s->layout_stats.packets,
           (unsigned long long)s->layout_stats.short_packets,
           (unsigned long long)s->layout_stats.data_bytes,
           (unsigned long long)s->layout_stats.slot_bad_words,
           (unsigned long long)s->layout_stats.packed_bad_words);
    printf("  Misaligned packets %llu, oversize %llu, samples with a nonzero low byte %llu\n",
           (unsigned long long)s->misaligned_packets, (unsigned long long)s->oversize_packets,
           (unsigned long long)s->low_byte_nonzero);
    if (s->low_frames != 0) {
        double ms = 1000.0 / s->rate_hz;
        unsigned ch, sec;

        printf("  Nonzero low bytes in %llu frames, from %.1f ms to %.1f ms after capture "
               "started, on channels",
               (unsigned long long)s->low_frames, (double)s->low_first_frame * ms,
               (double)s->low_last_frame * ms);
        for (ch = 0; ch < 32; ch++)
            if (s->low_channels & (UINT32_C(1) << ch))
                printf(" %u", ch + 1);
        printf("\n  Frames with them, by second:");
        for (sec = 0; sec < LOW_SECONDS; sec++)
            if (s->low_by_second[sec] != 0)
                printf(" %u%s=%llu", sec, sec == LOW_SECONDS - 1 ? "+" : "",
                       (unsigned long long)s->low_by_second[sec]);
        printf("\n");
    }

    printf("\nCapture packet sizes:\n");
    print_hist("Packets by frame count", s->size_hist, SIZE_HIST_BINS, size_names);
    if (s->size_hist[0] != 0)
        printf("  %llu empty packets: audio was lost, so any rate below includes that gap and\n"
               "  isn't the clock's real rate.\n",
               (unsigned long long)s->size_hist[0]);

    print_pipe_report(s, &s->cap);
    if (s->play_enabled) {
        print_pipe_report(s, &s->play);
        printf("  Short writes %llu\n", (unsigned long long)s->play.short_writes);
        printf("  Size FIFO: %u packets held at the end, %llu dropped\n", s->fifo.count,
               (unsigned long long)s->fifo.dropped_packets);
        /* Every captured frame must be accounted for by the size sequence. */
        if (s->fifo_open) {
            uint64_t captured = s->cap.frames_total - s->cap_frames_at_fifo_open;
            uint64_t accounted = s->play.frames_total + ua101_fifo_held_frames(&s->fifo) +
                                 s->fifo.dropped_frames;

            printf("  Captured frames since the FIFO opened %llu; played plus held plus dropped "
                   "%llu\n",
                   (unsigned long long)captured, (unsigned long long)accounted);
            if (captured != accounted) {
                printf("  The size sequences don't match.\n");
                pass = 0;
            }
        }
        if (s->opt.tone_count == 0)
            printf("  Output was digital silence for the whole run.\n");
    }

    printf("\nClock:\n");
    if (s->have_baseline && s->last_bus > s->base_bus) {
        double frames = (double)(s->last_frames - s->base_frames);
        double bus_seconds = (double)(s->last_bus - s->base_bus) / 1000.0;
        double callback_seconds = host_seconds(s, s->last_host - s->base_host);
        double by_bus = frames / bus_seconds;
        double by_callbacks = callback_seconds > 0.0 ? frames / callback_seconds : 0.0;
        double ppm_bus = (by_bus / s->rate_hz - 1.0) * 1e6;
        double bus_margin = ua101_rate_margin_ppm(bus_seconds);

        printf("  Nominal %.0f Hz over %.1f s\n", s->rate_hz, bus_seconds);
        printf("  Device against USB bus:  %.3f Hz (%+.1f ppm, within %.1f)\n", by_bus, ppm_bus,
               bus_margin);
        judge_rate(ppm_bus, bus_margin, &clock);
        if (s->cap.skipped_frames != 0)
            printf("  Capture skipped %llu ms in resyncs, so these rates include that gap and\n"
                   "  aren't the clock's real rate.\n",
                   (unsigned long long)s->cap.skipped_frames);
        if (frames < 0.5 * s->rate_hz * bus_seconds)
            printf("  The unit sent almost no audio: %.0f frames, where about %.0f were expected.\n"
                   "  It does this when the digital input switch is on with no optical source.\n",
                   frames, s->rate_hz * bus_seconds);

        if (s->have_bus_times && s->stamp_last_host > s->stamp_base_host) {
            double stamp_seconds = host_seconds(s, s->stamp_last_host - s->stamp_base_host);
            double stamp_bus = (double)(s->stamp_last_bus - s->stamp_base_bus) / 1000.0;
            double bus_host = stamp_bus / stamp_seconds;
            double by_host = by_bus * bus_host;
            double ppm_host = (by_host / s->rate_hz - 1.0) * 1e6;
            double host_margin = ua101_host_margin_ppm(stamp_seconds);

            printf("  USB bus against host:    %+.1f ppm, within %.1f\n", (bus_host - 1.0) * 1e6,
                   host_margin);
            printf("  Device against host:     %.3f Hz (%+.1f ppm, within %.1f)\n", by_host,
                   ppm_host, host_margin + bus_margin);
            judge_rate(ppm_host, host_margin + bus_margin, &clock);
        } else {
            printf("  Couldn't get bus timestamps, so the host clock wasn't compared.\n");
            pass = 0;
            if (clock != CLOCK_OUTSIDE)
                clock = CLOCK_UNDECIDED;
        }
        /* Includes changes in callback latency, so it's for comparison and not judged. */
        printf("  From callback times:     %.3f Hz (%+.1f ppm)\n", by_callbacks,
               (by_callbacks / s->rate_hz - 1.0) * 1e6);
    } else {
        printf("  Not enough data to measure.\n");
        pass = 0;
        clock = CLOCK_UNDECIDED;
    }

    if (s->opt.wav_path != NULL) {
        unsigned long long expected = (s->cap.frames_total - s->wav_start_frames) * s->cap.frame_bytes;

        printf("\nRecording:\n");
        printf("  %s\n", s->opt.wav_path);
        printf("  %llu bytes written, %llu expected, %llu dropped\n",
               (unsigned long long)s->wav_data_bytes, expected,
               (unsigned long long)s->ring_dropped_bytes);
        if (s->write_errno != 0) {
            printf("  Write error: %s\n", strerror(s->write_errno));
            pass = 0;
        }
        if (s->wav_data_bytes != expected)
            pass = 0;
    }

    for (t = 0; t < s->opt.tone_count; t++) {
        const tone_slot *slot = &s->tones[t];

        printf("\nTone %d: %s, %.1f dBFS for %.2f s, output frame %llu\n", t + 1,
               slot->state == TONE_DONE      ? "played"
               : slot->state == TONE_REFUSED ? "NOT played, because a check had failed"
               : slot->state == TONE_PLAYING ? "still playing when the run ended"
                                             : "never reached",
               s->opt.tone_db, s->opt.tone_seconds, (unsigned long long)slot->tone.start);
        if (slot->state != TONE_DONE)
            pass = 0;
    }

    print_dump(s);

    if (s->clock_log != NULL)
        write_clock_log(s);
    if (s->stall_log != NULL)
        write_stall_log(s);
    if (s->opt.transfer_log_path != NULL && s->check_bus != NULL)
        write_transfer_log(s);

    if (error_count(s) != 0)
        pass = 0;
    if (s->layout == UA101_LAYOUT_UNKNOWN)
        pass = 0;
    if (s->gave_up) {
        printf("\nSome transfers never completed when stopping.\n");
        pass = 0;
    }
    if (fatal != NULL) {
        printf("\nStopped early: %s\n", fatal);
        pass = 0;
    }

    if (s->opt.stall_test_ms > 0) {
        /* The resyncs are what the test makes happen; anything else is a real fault. */
        uint64_t other = error_count(s) - s->cap.resyncs;
        int placed = report_stall_test(s);

        printf("\n=== Stall test result: %s%s ===\n",
               !placed      ? "the gap is NOT in place"
               : other != 0 ? "the gap is in place, but other errors happened"
               : fatal != NULL || s->gave_up ? "the gap is in place, but the run didn't end well"
                                              : "the gap is in place",
               !placed ? "" : placed == 2 ? ", exactly" : ", one frame off");
        printf("    (%llu resyncs, which the test causes, and %llu other errors)\n",
               (unsigned long long)s->cap.resyncs, (unsigned long long)other);
        return placed && other == 0 && fatal == NULL && !s->gave_up ? 0 : 1;
    }

    printf("\n=== Stage 2 streaming result: %s (%llu errors) ===\n", pass ? "PASS" : "FAIL",
           (unsigned long long)error_count(s));
    printf("=== Clock result: %s ===\n",
           clock == CLOCK_INSIDE    ? "inside the 100 ppm limit"
           : clock == CLOCK_OUTSIDE ? "OUTSIDE the 100 ppm limit: needs rate correction to stay in step "
                                      "with other devices"
                                    : "too short to judge");
    return !pass ? 1 : clock == CLOCK_INSIDE ? 0 : 2;
}

static int configure_device(device_ref device, int *opened)
{
    IOReturn result;
    UInt8 current = 0;

    result = (*device)->USBDeviceOpen(device);
    if (result != kIOReturnSuccess) {
        printf("  Couldn't open the device: %s\n", describe(result));
        if (result == kIOReturnExclusiveAccess)
            printf("  Another program has it open. Quit audio or MIDI apps.\n");
        return 0;
    }
    *opened = 1;
    (*device)->GetConfiguration(device, &current);
    if (current != 1) {
        result = (*device)->SetConfiguration(device, 1);
        if (result != kIOReturnSuccess) {
            printf("  Couldn't set the configuration: %s\n", describe(result));
            return 0;
        }
        printf("  Set configuration 1 (it was %u)\n", current);
    }
    return 1;
}

/* Reads each clock a few times, so a wrong assumption stops the run before it starts. */
static int check_bus_timing(stream *s, pipe_state *p)
{
    UInt32 microseconds = 0;
    UInt64 bus0 = 0, bus1 = 0;
    AbsoluteTime t0, t1;
    UInt32 bandwidth = 0;
    double host_ms;
    double ticks_per_ms;

    if ((*p->intf)->GetFrameListTime(p->intf, &microseconds) != kIOReturnSuccess ||
        microseconds != HIGH_SPEED_US_PER_FRAME) {
        printf("  GetFrameListTime returned %u us. High speed is needed (%u us).\n",
               (unsigned)microseconds, HIGH_SPEED_US_PER_FRAME);
        return 0;
    }
    if ((*p->intf)->GetBusFrameNumberWithTime(p->intf, &bus0, &t0) != kIOReturnSuccess)
        return 0;
    usleep(200000);
    if ((*p->intf)->GetBusFrameNumberWithTime(p->intf, &bus1, &t1) != kIOReturnSuccess)
        return 0;
    host_ms = (double)(absolute_ticks(t1) - absolute_ticks(t0)) * s->abs_to_seconds * 1000.0;
    ticks_per_ms = host_ms > 0.0 ? (double)(bus1 - bus0) / host_ms : 0.0;
    (*p->intf)->GetBandwidthAvailable(p->intf, &bandwidth);
    printf("  Frame list time %u us; bus frame number %llu, %.3f per ms; bandwidth left %u\n",
           (unsigned)microseconds, (unsigned long long)bus1, ticks_per_ms, (unsigned)bandwidth);
    if (fabs(ticks_per_ms - 1.0) > 0.02) {
        printf("  Bus frames aren't counting in milliseconds, so scheduling would be wrong.\n");
        return 0;
    }
    return 1;
}

int main(int argc, char **argv)
{
    stream *s = &g_stream;
    mach_timebase_info_data_t timebase;
    IOUSBConfigurationDescriptorPtr descriptor = NULL;
    ua101_config config;
    io_service_t service;
    device_ref device = NULL;
    int device_open = 0, exit_code = 1, writer_running = 0, t;
    pthread_t usb, writer;
    uint64_t started = 0, ended = 0, last_completed = 0, last_progress;
    double next_print = 10.0, elapsed = 0.0;
    struct sigaction action;

    setvbuf(stdout, NULL, _IOLBF, 0);
    s->wav_fd = -1;
    if (!parse_options(argc, argv, &s->opt))
        return 2;
    s->play_enabled = !s->opt.capture_only;

    mach_timebase_info(&timebase);
    s->abs_to_seconds = (double)timebase.numer / (double)timebase.denom / 1e9;
    ua101_fifo_init(&s->fifo, s->fifo_slots, SIZE_FIFO_SLOTS);

    printf("=== UA-101 Stage 2 streaming test ===\n");
    printf("Mode: %s; %.1f s; %u ms transfers, capture queue of %u",
           s->play_enabled ? (s->opt.tone_count ? "capture and playback, with a test tone"
                                                : "capture and silent playback")
                           : "capture only",
           s->opt.seconds, s->opt.transfer_ms, s->opt.capture_queue);
    if (s->play_enabled)
        printf(", playback queue of %u", s->opt.playback_queue);
    printf("\n");

    memset(&action, 0, sizeof(action));
    action.sa_handler = on_signal;
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);

    service = find_device();
    if (service == IO_OBJECT_NULL) {
        printf("No UA-101 in Hi-Speed mode was found. Run ua101probe for details.\n");
        return 1;
    }
    device = open_device_object(service);
    IOObjectRelease(service);
    if (device == NULL) {
        printf("Couldn't get a device object.\n");
        return 1;
    }

    printf("\nSetup:\n");
    if (!configure_device(device, &device_open))
        goto done;
    if ((*device)->GetConfigurationDescriptorPtr(device, 0, &descriptor) != kIOReturnSuccess ||
        descriptor == NULL) {
        printf("  Couldn't read the configuration descriptor.\n");
        goto done;
    }
    if (ua101_parse_config((const uint8_t *)descriptor, descriptor->wTotalLength, &config) !=
        UA101_PARSE_OK) {
        printf("  Couldn't parse the configuration descriptor.\n");
        goto done;
    }

    /* Capture first, as the streams need it to be running. */
    if (!prepare_pipe(s, &s->cap, device, &config, UA101_IF_CAPTURE, "Capture"))
        goto done;
    if (s->play_enabled &&
        !prepare_pipe(s, &s->play, device, &config, UA101_IF_PLAYBACK, "Playback"))
        goto done;

    /* Stage 3 needs another process to own MIDI, so don't rely on holding the device. */
    if (!s->opt.keep_device_open) {
        (*device)->USBDeviceClose(device);
        device_open = 0;
        printf("  Device closed; streaming with only the interfaces open\n");
    }

    s->packets = s->opt.transfer_ms * 8;
    s->bus_frames_per_transfer = ua101_iso_bus_frames(s->packets, s->cap.interval);
    if (s->packets > MAX_PACKETS || s->bus_frames_per_transfer == 0 ||
        (s->play_enabled && ua101_iso_bus_frames(s->packets, s->play.interval) !=
                                s->bus_frames_per_transfer)) {
        printf("  Transfers of %u packets don't fit the endpoint intervals.\n", s->packets);
        goto done;
    }
    /* Playback must be able to carry any packet size that capture can produce. */
    if (s->play_enabled &&
        s->cap.max_packet / s->cap.frame_bytes > s->play.max_packet / s->play.frame_bytes) {
        printf("  A capture packet can hold more frames than a playback packet.\n");
        goto done;
    }
    if (!check_bus_timing(s, &s->cap))
        goto done;

    if (!allocate_transfers(s, &s->cap, s->opt.capture_queue) ||
        (s->play_enabled && !allocate_transfers(s, &s->play, s->opt.playback_queue))) {
        printf("  Out of memory.\n");
        goto done;
    }
    ua101_timeline_init(&s->cap.timeline, s->rate_hz, 0.001);
    ua101_timeline_init(&s->play.timeline, s->rate_hz, 0.001);
    if (s->opt.stall_test_ms > 0 || s->opt.transfer_log_path != NULL) {
        s->check_capacity = (uint32_t)(s->opt.seconds * 1000.0 / s->opt.transfer_ms) + 64;
        s->check_bus = malloc(s->check_capacity * sizeof(uint64_t));
        s->check_frame = malloc(s->check_capacity * sizeof(uint64_t));
        if (s->check_bus == NULL || s->check_frame == NULL) {
            printf("  Out of memory.\n");
            goto done;
        }
    }
    if (s->opt.dump_transfers > 0) {
        s->dump_data = malloc((size_t)s->opt.dump_transfers * s->packets * s->cap.max_packet);
        s->dump_frames = malloc((size_t)s->opt.dump_transfers * s->packets * sizeof(IOUSBIsocFrame));
        if (s->dump_data == NULL || s->dump_frames == NULL) {
            printf("  Out of memory.\n");
            goto done;
        }
        memset(s->dump_data, 0, (size_t)s->opt.dump_transfers * s->packets * s->cap.max_packet);
        memset(s->dump_frames, 0,
               (size_t)s->opt.dump_transfers * s->packets * sizeof(IOUSBIsocFrame));
    }
    if (s->opt.clock_log_path != NULL) {
        s->clock_log_capacity = (uint32_t)s->opt.seconds + 16;
        s->clock_log = malloc(s->clock_log_capacity * sizeof(clock_sample));
        if (s->clock_log == NULL) {
            printf("  Out of memory.\n");
            goto done;
        }
    }
    if (s->opt.stall_log_path != NULL) {
        s->stall_log = calloc(STALL_LOG_CAPACITY, sizeof(stall_event));
        if (s->stall_log == NULL) {
            printf("  Out of memory.\n");
            goto done;
        }
        s->stall_host_start = mach_absolute_time();
        s->stall_wall_start = wall_seconds();
    }

    for (t = 0; t < s->opt.tone_count; t++) {
        uint64_t start = (uint64_t)(s->opt.tone_at[t] * s->rate_hz);

        if (ua101_tone_init(&s->tones[t].tone, s->rate_hz, s->opt.tone_db, s->opt.tone_seconds,
                            1000.0, 1500.0, start) != 0) {
            printf("  Tone refused: the limits are %.0f dBFS and %.0f s.\n", UA101_TONE_MAX_DBFS,
                   UA101_TONE_MAX_SECONDS);
            goto done;
        }
        if (s->opt.tone_at[t] < 0.5 ||
            s->opt.tone_at[t] + s->opt.tone_seconds + 0.5 > s->opt.seconds) {
            printf("  A tone must start at least 0.5 s in and end 0.5 s before the run does.\n");
            goto done;
        }
        s->tones[t].state = TONE_WAITING;
        printf("  Tone %d: %.1f dBFS, %.2f s, %s, at %.2f s\n", t + 1, s->opt.tone_db,
               s->opt.tone_seconds,
               s->opt.jack_map ? "on every output, each at its own frequency (jack map)"
                               : "on outputs 1 and 2",
               s->opt.tone_at[t]);
    }

    if (!prepare_recording(s))
        goto done;
    if (s->wav_fd >= 0) {
        if (pthread_create(&writer, NULL, writer_thread, s) != 0) {
            printf("  Couldn't start the writer thread.\n");
            goto done;
        }
        writer_running = 1;
    }

    printf("\nStreaming:\n");
    started = mach_absolute_time();
    last_progress = started;
    if (pthread_create(&usb, NULL, usb_thread, s) != 0) {
        printf("  Couldn't start the USB thread.\n");
        goto done;
    }

    for (;;) {
        uint64_t now, completed;

        usleep(50000);
        now = mach_absolute_time();
        elapsed = host_seconds(s, now - started);
        if (g_signal != 0 || atomic_load(&s->stop_request) || elapsed >= s->opt.seconds)
            break;

        completed = s->cap.completed;
        if (completed != last_completed) {
            last_completed = completed;
            last_progress = now;
        } else if (host_seconds(s, now - last_progress) > STALL_SECONDS) {
            fail_stream(s, "no capture transfer has completed for over a second");
            break;
        }

        if (elapsed >= next_print) {
            printf("  %5.0f s: %llu frames, %u packets in the size FIFO, errors %llu\n", elapsed,
                   (unsigned long long)s->cap.frames_total, s->fifo.count,
                   (unsigned long long)error_count(s));
            next_print += 10.0;
        }
    }
    if (g_signal != 0) {
        printf("  Stopping: signal %d\n", (int)g_signal);
        fail_stream(s, "interrupted");
    }
    atomic_store(&s->stop_request, 1);
    pthread_join(usb, NULL);
    ended = mach_absolute_time();

    if (writer_running) {
        atomic_store(&s->writer_stop, 1);
        pthread_join(writer, NULL);
        writer_running = 0;
    }
    finish_recording(s);

    /* Playback first and then capture, the reverse of how they started. */
    release_pipe(&s->play);
    release_pipe(&s->cap);

    exit_code = report(s, host_seconds(s, ended - started));

done:
    if (writer_running) {
        atomic_store(&s->writer_stop, 1);
        pthread_join(writer, NULL);
    }
    release_pipe(&s->play);
    release_pipe(&s->cap);
    if (device_open)
        (*device)->USBDeviceClose(device);
    if (device != NULL)
        (*device)->Release(device);
    return exit_code;
}
