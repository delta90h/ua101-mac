/*
 * UA101.driver: the AudioServerPlugIn that makes the UA-101 an audio device in every app.
 *
 * Core Audio loads it into its own sandboxed process, which can't talk to USB, so the helper
 * (ua101d) streams with the unit and shares one block of memory with this plug-in
 * (src/ua101_shared.h), reached through src/ua101_link.h. Here, the I/O cycle reads capture from
 * the block's ring by sample time, writes playback into the other ring, and gives Core Audio
 * zero timestamps from the clock line the helper publishes. Nothing on that path locks, waits
 * or allocates.
 *
 * Objects: the plug-in, one device, an input stream and an output stream. The device appears
 * once the helper has opened the unit, with the unit's rate and channel counts. The rate is set
 * by the switch on the unit, so it's the only rate offered and setting another is refused.
 * Channels are numbered until the jack map gives them names.
 *
 * A short gap keeps the device (a design choice, 2026-11-13). When the unit leaves (cable
 * out, switched off, or the helper not answering), the same device object stays listed and
 * alive for up to GAP_SECONDS, or for as long as an app's I/O is still running on it, and
 * starts are refused meanwhile. If the unit is back within that time with the same rate and
 * channels, the same object carries on, so an app that was open keeps a device it can play
 * through. Otherwise the device goes, and a unit that returns later, or at another rate, is a
 * new object. See "The gap" below.
 *
 * Apps get 32-bit floats; the unit's 24-bit samples convert to them and back exactly
 * (src/ua101_convert.h).
 *
 * Rate correction, a design choice and on unless turned off: the unit's clock runs about
 * 119 ppm slow against the Mac's, so apps would see 47994 frames a second where they expect
 * 48000. With correction on, Core Audio's sample time is a timeline of our own at exactly the
 * nominal rate against host time, and each I/O cycle works out, from the helper's clock line,
 * where its frames fall on the unit's timeline; ua101_live (src/ua101_resample.h) resamples
 * between the two. The cycles don't follow the helper's line itself, which moves a little every
 * time it is written, but a smoothed copy of it (src/ua101_steer.h), so the pitch stays steady.
 * The filter needs some frames past each moment (26 at 48 kHz), so both directions run that
 * many frames behind, and a little more for what the smoothing lets through, which is reported
 * as latency. With it off (a device property, kept in Core Audio's storage), sample time is the
 * unit's own and the path is bit-exact. A change made while I/O runs takes effect when I/O next
 * starts.
 *
 * The two safety offsets, the user's to choose since 2026-11-19 (src/ua101_drvset.h): how far
 * behind the unit recording is read, and how far ahead playback is written. Each is a device
 * property, a number of milliseconds from a short list, kept in Core Audio's storage. Until
 * one is set, the helper's own values hold (5 ms and 21.3 ms). A change that moves an offset
 * goes through Core Audio's configuration change, as it must: Core Audio stops I/O, takes the
 * new offsets and starts I/O again, so the sound breaks for a moment. To choose by, every run
 * measures the room the two left at its worst moment: readable as one more property while it
 * runs and after, and given to the helper for its log when the run stops.
 */
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <mach/mach_time.h>
#include <math.h>
#include <os/log.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "../ua101_convert.h"
#include "../ua101_drvset.h"
#include "../ua101_link.h"
#include "../ua101_resample.h"
#include "../ua101_shared.h"
#include "../ua101_steer.h"

#define DEVICE_UID UA101_DEVICE_UID
#define MODEL_UID "io.github.delta90h.ua101:model"
#define MAX_CHANNELS 16
#define CHUNK_FRAMES 256      /* the I/O path converts this many frames at a time */
#define START_WAIT_SECONDS 3.0 /* for the helper's clock, at the first StartIO */
#define RETRY_SECONDS 3.0     /* between hellos while the unit isn't there */
/* A start that finds the helper still asleep waits this long for it to wake: Core Audio may
   restart I/O after a wake a moment before the helper hears of the wake itself. */
#define WAKE_WAIT_SECONDS 2.0
#define WAKE_POLL_US 50000
/* How long the device stays listed after the unit leaves, waiting for it to return, when no
   I/O is running on it: see "The gap". One timer runs for it, counted in time the Mac is
   awake; nothing asks again meanwhile unless the helper can't say when a unit appears (then
   RETRY_SECONDS, as before). */
#define GAP_SECONDS 30.0
#define CHANGE_RESTART 1      /* a configuration change: the helper's stream started over */
#define CHANGE_CORRECTION 2   /* one asked for when rate correction is switched while running */
#define CHANGE_OFFSETS 3      /* one asked for when a safety offset is to change */

/* The rate correction setting: a device property holding a CFBoolean, and its storage key. */
#define PROPERTY_RATE_CORRECTION UA101_PROPERTY_RATE_CORRECTION /* 'UArc' */
#define CORRECTION_KEY "rate correction"
#define LIVE_FRAMES 1024    /* each resampler's room: the filter's reach plus a chunk */
/* Added to the filter's delay, so a smoothed line that runs this far ahead of the helper's
   still asks for no frame sooner than the bit-exact path would. Measured: under a tenth of a
   frame at 48 kHz once settled, half a frame after the line moves by half a frame. */
#define STEER_SLACK_SECONDS 20e-6
#define MAX_STEP_OFF 0.01   /* a cycle's step stays within 1% of one frame per frame */
#define LOST_FRAMES 32.0    /* off by more than this, a resampler starts again in place */
/* Each resampler's position is counted from a base that moves up every this many frames, so
   the numbers compared each cycle stay small and a double holds them exactly at any uptime.
   Left to grow, they would round to 2 millionths of a frame after two days at 48 kHz. */
#define REBASE_FRAMES 65536

enum { GAP_NONE = 0, GAP_WAITING, GAP_OVERTIME };

typedef struct {
    AudioServerPlugInDriverInterface *interface; /* the driver reference points here */
    _Atomic ULONG refs;
    AudioServerPlugInHostRef host;
    os_log_t log;
    dispatch_queue_t queue; /* hellos the helper asks for, retries and notifications */
    ua101_link *link;
    /* Held across a hello and across starting or stopping I/O, so the block can't be swapped
       while I/O uses it. Property calls never take it, so they never wait for the helper. */
    pthread_mutex_t map_lock;
    char last_why[256]; /* under map_lock: the last reason logged, so each is logged once */
    pthread_mutex_t lock; /* the fields below, up to the I/O thread's */
    ua101_link_info info;
    int published, alive, retry_pending;
    /* The unit left and the device is kept, waiting for it (see "The gap"): GAP_WAITING, or
       GAP_OVERTIME once the time is up and only running I/O still keeps the device. */
    int gap;
    unsigned gap_generation; /* which gap: a timer left over from an earlier one does nothing */
    double gap_seconds;      /* GAP_SECONDS; a test makes it short, and 0 keeps nothing */
    AudioObjectID next_id, device_id, input_id, output_id;
    unsigned io_clients;
    int input_active, output_active;
    int correct_wanted;  /* the rate correction setting */
    int correct_running; /* what running I/O uses: the setting when it started */
    /* The two safety offsets as times, in seconds; 0 while the helper's own value holds
       (src/ua101_drvset.h). The settings, and what Core Audio has been told: they differ only
       between a change being asked for and Core Audio performing it. */
    double in_margin_wanted, out_delay_wanted;
    double in_margin_used, out_delay_used;
    /* The run that is on, or the last one, for the statistics property. */
    struct {
        int any; /* I/O has started at least once */
        double rate, ticks_per_second;
        uint64_t start_host;
        uint32_t in_offset, out_offset; /* frames, as Core Audio was told */
        uint64_t missing_base;          /* input_missing as it began */
        /* Once it has stopped. */
        double seconds;
        int64_t cap_spare, play_spare; /* UA101_SPARE_NONE if not measured */
        uint64_t missing, late;
    } run;

    /* The I/O thread's: set at the first StartIO, unchanged while I/O runs. */
    ua101_shared *io;
    /* Its rings, through our own copy of where their frames are, checked at StartIO: nothing
       the helper's process writes into the block afterwards can send a copy out of bounds. */
    ua101_frame_view cap_ring, play_ring;
    uint32_t io_in, io_out;
    uint32_t zts_period;
    int have_zts;
    Float64 zts_sample;
    UInt64 zts_host, zts_seed;
    int32_t in_temp[CHUNK_FRAMES * MAX_CHANNELS];
    int32_t out_temp[CHUNK_FRAMES * MAX_CHANNELS];
    _Atomic uint64_t input_missing, output_clipped;
    uint64_t run_missing, run_clipped; /* those two as the run began, for its own counts */
    /* For this run, written by the I/O thread only: the largest of the samples counted in
       output_clipped, where full scale is 1.0; and what the rate correction's own result went
       over at, as the playback resampler counts it (samples, and the largest of them). All 0
       when nothing was over. Counting only: none of it changes a sample. */
    _Atomic float output_peak;
    _Atomic uint64_t output_cut;
    _Atomic float output_cut_peak;
    /* For this run too: the least room recording had, written by the I/O thread only, and the
       helper's two counts for playback as the I/O thread last saw them in the block (property
       calls never touch the block, which a hello may unmap). Frames; UA101_SPARE_NONE if not
       measured. Counting only. */
    _Atomic int64_t cap_spare, play_spare;
    _Atomic uint64_t play_late;

    /* The I/O thread's rate correction. The filter and resamplers are made at StartIO, never on
       the I/O thread, and the filter is kept for the next start at the same rate. */
    int io_correct;
    double filter_rate; /* what `filter` was made for, 0 if nothing yet */
    ua101_resampler filter;
    ua101_live cap, play;
    double nominal_ticks; /* host ticks per frame at the nominal rate */
    double origin_host;   /* the host time of our sample time 0 */
    uint32_t delay;       /* frames each direction runs behind: see correction_delay */
    ua101_clock_line line; /* the helper's, as last read */
    double line_time, line_ratio; /* see use_line */
    ua101_steer steer;     /* the smoothed copy of it that both directions aim at */
    uint32_t steer_seed;   /* the helper's seed it follows */
    int cap_on, play_on;   /* whether each resampler has its place */
    int cap_fresh, play_fresh; /* just started: the next check isn't counted as an error */
    uint64_t cap_start, play_start; /* the smoothed line's start each began on */
    int64_t cap_next, cap_base, cap_unit;   /* next sample time; unit frames of input 0, next */
    int64_t play_next, play_base, play_unit; /* next sample time; sample time of input 0; unit */
    int32_t pull_temp[(CHUNK_FRAMES + 8) * MAX_CHANNELS];
    /* For reports and tests. */
    _Atomic double cap_step, play_step, cap_error, play_error;
    _Atomic uint64_t cap_resets, play_resets;
    _Atomic double steer_offset;
    _Atomic uint64_t steer_starts;
} driver;

static AudioServerPlugInDriverInterface g_interface;
static driver g_driver = {.interface = &g_interface,
                          .map_lock = PTHREAD_MUTEX_INITIALIZER,
                          .lock = PTHREAD_MUTEX_INITIALIZER,
                          .next_id = 2,
                          .input_active = 1,
                          .output_active = 1,
                          .gap_seconds = GAP_SECONDS,
                          .correct_wanted = 1};

static driver *from_ref(AudioServerPlugInDriverRef ref)
{
    return (driver *)ref == &g_driver ? &g_driver : NULL;
}

/* --- The device appearing and going, from the helper's hello. --- */

static void say_hello(driver *d);

/* Notifications go out from the plug-in's queue, never from inside a call from Core Audio. */
static void notify(driver *d, AudioObjectID object, UInt32 selector)
{
    dispatch_async(d->queue, ^{
      AudioObjectPropertyAddress a = {selector, kAudioObjectPropertyScopeGlobal,
                                      kAudioObjectPropertyElementMain};

      if (d->host != NULL)
          d->host->PropertiesChanged(d->host, object, 1, &a);
    });
}

static void notify_device_list(driver *d)
{
    dispatch_async(d->queue, ^{
      AudioObjectPropertyAddress a[2] = {
          {kAudioPlugInPropertyDeviceList, kAudioObjectPropertyScopeGlobal,
           kAudioObjectPropertyElementMain},
          {kAudioObjectPropertyOwnedObjects, kAudioObjectPropertyScopeGlobal,
           kAudioObjectPropertyElementMain}};

      if (d->host != NULL)
          d->host->PropertiesChanged(d->host, kAudioObjectPlugInObject, 2, a);
    });
}

/* The device's latency, which rate correction changes, in both directions. */
static void notify_latency(driver *d, AudioObjectID device)
{
    dispatch_async(d->queue, ^{
      AudioObjectPropertyAddress a[2] = {
          {kAudioDevicePropertyLatency, kAudioObjectPropertyScopeInput,
           kAudioObjectPropertyElementMain},
          {kAudioDevicePropertyLatency, kAudioObjectPropertyScopeOutput,
           kAudioObjectPropertyElementMain}};

      if (d->host != NULL)
          d->host->PropertiesChanged(d->host, device, 2, a);
    });
}

/* The rate correction setting from a property list value: a CFBoolean, or a number. */
static int correction_value(CFPropertyListRef v, int *on)
{
    int n = 0;

    if (v == NULL)
        return 0;
    if (CFGetTypeID(v) == CFBooleanGetTypeID()) {
        *on = CFBooleanGetValue((CFBooleanRef)v) ? 1 : 0;
        return 1;
    }
    if (CFGetTypeID(v) == CFNumberGetTypeID() && CFNumberGetValue((CFNumberRef)v, kCFNumberIntType, &n)) {
        *on = n != 0;
        return 1;
    }
    return 0;
}

/* Whether the device's frames are being corrected now, or will be when I/O starts. Under lock. */
static int correcting(const driver *d)
{
    return d->io_clients > 0 ? d->correct_running : d->correct_wanted;
}

/* Frames each direction runs behind with rate correction on: the live filter's reach past each
   moment, plus the slack for the smoothed line. 27 at 48 kHz, 47 at 44.1 kHz, 12 at 192 kHz. */
static uint32_t correction_delay(double rate)
{
    return ua101_resampler_live_half_taps(rate, rate) + (uint32_t)ceil(STEER_SLACK_SECONDS * rate);
}

/* A safety offset setting from a property list value: a number of milliseconds. */
static int margin_value(CFPropertyListRef v, double *ms)
{
    return v != NULL && CFGetTypeID(v) == CFNumberGetTypeID() &&
           CFNumberGetValue((CFNumberRef)v, kCFNumberDoubleType, ms) && isfinite(*ms);
}

/* The safety offsets Core Audio is told, in frames: the user's, or the helper's. Under lock. */
static uint32_t input_offset(const driver *d)
{
    return d->in_margin_used > 0.0 ? ua101_offset_frames(d->in_margin_used, d->info.rate_hz)
                                   : d->info.input_offset;
}

static uint32_t output_offset(const driver *d)
{
    return d->out_delay_used > 0.0 ? ua101_offset_frames(d->out_delay_used, d->info.rate_hz)
                                   : d->info.output_offset;
}

/* Both offsets changed, as far as Core Audio knows. */
static void notify_offsets(driver *d, AudioObjectID device)
{
    dispatch_async(d->queue, ^{
      AudioObjectPropertyAddress a[4] = {
          {kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeInput,
           kAudioObjectPropertyElementMain},
          {kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeOutput,
           kAudioObjectPropertyElementMain},
          {UA101_PROPERTY_INPUT_MARGIN, kAudioObjectPropertyScopeGlobal,
           kAudioObjectPropertyElementMain},
          {UA101_PROPERTY_OUTPUT_DELAY, kAudioObjectPropertyScopeGlobal,
           kAudioObjectPropertyElementMain}};

      if (d->host != NULL)
          d->host->PropertiesChanged(d->host, device, 4, a);
    });
}

static int same_format(const ua101_link_info *a, const ua101_link_info *b)
{
    return a->rate_hz == b->rate_hz && a->capture_channels == b->capture_channels &&
           a->playback_channels == b->playback_channels;
}

static void retry_later(driver *d)
{
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(RETRY_SECONDS * NSEC_PER_SEC)),
                   d->queue, ^{
                     pthread_mutex_lock(&d->lock);
                     d->retry_pending = 0;
                     pthread_mutex_unlock(&d->lock);
                     say_hello(d);
                   });
}

/*
 * The gap. A unit that leaves and is back a moment later (the cable pulled and pushed in, the
 * power switch) used to cost the device object: withdrawn, then published as a new one, and
 * an app that was open kept the old, dead object until it was quit and opened again (measured
 * with the user, 2026-11-13). So a hello that finds no unit now starts a gap instead: the
 * device stays listed, with the same objects, and one timer is set for gap_seconds.
 *
 * During the gap:
 *   - the device is reported ALIVE. "Not alive" is final for an object in Core Audio: apps
 *     drop a dead device and never look at it again, which is the very thing the gap is there
 *     to avoid. Sleep already works this way (the device stays listed and alive while the
 *     helper has given the unit back), and the user's open app played again after it.
 *   - every StartIO is refused with "not running", the device kept: nothing can stream.
 *   - I/O that was running when the unit left keeps running, on the block it has. The helper
 *     has stopped, so capture reads as silence and playback goes nowhere; the clock carries
 *     on from the last line. Nothing is told to Core Audio.
 * The gap ends in one of three ways:
 *   - a hello finds the unit with the same rate and channels: the same objects carry on. If
 *     I/O is running, the helper's block is a new one, so Core Audio is asked to restart I/O
 *     (the path a wake and a helper crash already use).
 *   - a hello finds it with another rate or other channels: as before the gap existed. With
 *     no I/O running the device is replaced by new objects at once; with I/O running it is
 *     marked dead, and replaced once I/O has stopped.
 *   - the timer runs out with no I/O running: the device is withdrawn, as before the gap
 *     existed, only later.
 * A timer that runs out while I/O is running ends nothing: the device stays, alive, for as
 * long as that I/O runs, and the first two endings still apply (a design choice,
 * 2026-11-13). Marking it dead there was tried first, on the real host: the app went on
 * playing into the dead device, nothing stopped its I/O, and the dead object stayed in the
 * list, in the way of the new one, until the app paused. So nothing was gained, and a unit
 * that came back after 31 s cost a pause, a pick and a play. Once that I/O stops with the
 * unit still away and the time up, the hello at the stop withdraws the device at once (or, if
 * the Mac is asleep just then, the wait runs once more: gap_waits_again).
 * "Asleep" changes nothing, in a gap as outside one. The timer counts time the Mac is awake
 * (dispatch_time's clock stands still in sleep), so a sleep in the middle of a gap neither
 * uses it up nor ends it: after the wake the helper says whether the unit is there, and the
 * rest of the time runs from then.
 *
 * It costs nothing while waiting: one pending timer, no asking, no power assertion.
 */

/* On d->queue, gap_seconds after a gap began: ends it if it is still that gap and no I/O runs. */
static void gap_over(driver *d, unsigned generation)
{
    int ended = 0, running = 0;

    /* map_lock, so this never lands in the middle of a hello or a start. */
    pthread_mutex_lock(&d->map_lock);
    pthread_mutex_lock(&d->lock);
    if (d->gap == GAP_WAITING && generation == d->gap_generation) {
        ended = 1;
        running = d->io_clients > 0;
        if (running) {
            d->gap = GAP_OVERTIME;
        } else {
            d->gap = GAP_NONE;
            d->published = 0;
        }
    }
    pthread_mutex_unlock(&d->lock);
    pthread_mutex_unlock(&d->map_lock);
    if (!ended)
        return;
    os_log(d->log, "UA-101 didn't come back in time: the device %{public}s",
           running ? "stays while I/O is running on it, until that stops or the unit returns"
                   : "goes from the list");
    if (!running)
        notify_device_list(d);
}

static void gap_ends_later(driver *d, unsigned generation, double seconds)
{
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(seconds * NSEC_PER_SEC)), d->queue,
                   ^{
                     gap_over(d, generation);
                   });
}

/* I/O has stopped while the Mac is asleep, so nothing could be learned about the unit. If only
   that I/O was keeping the device (the gap's time was up), the wait runs once more from here,
   so the device can't be left listed for ever with nobody to ask about it. The caller holds
   map_lock. */
static void gap_waits_again(driver *d)
{
    unsigned generation = 0;
    double seconds = 0.0;
    int again = 0;

    pthread_mutex_lock(&d->lock);
    if (d->gap == GAP_OVERTIME && d->io_clients == 0) {
        again = 1;
        d->gap = GAP_WAITING;
        generation = ++d->gap_generation;
        seconds = d->gap_seconds;
    }
    pthread_mutex_unlock(&d->lock);
    if (again)
        gap_ends_later(d, generation, seconds);
}

/* For tests: how long a gap lasts, from the next one on. 0 keeps nothing: the device goes at
   once, as it did before 2026-11-13. */
void ua101_plugin_set_gap_seconds(double seconds)
{
    pthread_mutex_lock(&g_driver.lock);
    g_driver.gap_seconds = seconds;
    pthread_mutex_unlock(&g_driver.lock);
}

/*
 * Publishes, updates or withdraws the device from what the helper says. The caller holds
 * map_lock. Running I/O keeps the block and format it started with: if the unit came back
 * different, the device is marked dead and Core Audio stops I/O; if the helper's stream
 * started over on a new block, Core Audio is asked to restart I/O. A unit that isn't there
 * starts a gap (above) rather than taking the device away. Once I/O stops, the next hello
 * brings the device back, with new objects if it was dead or changed, and the same device
 * UID so apps keep their choice.
 *
 * Returns the hello's status. "Asleep" (the Mac is going to sleep or waking, and the helper
 * has given the unit back) says nothing about the unit, so nothing changes: the device stays
 * as it is, and the helper sends an event after the wake if there is anything to catch up on.
 */
static int say_hello_locked(driver *d)
{
    ua101_link_info info;
    char why[256] = "";
    int status, running, list_changed = 0, alive_changed = 0, restart = 0, retry = 0;
    int gap_began = 0, gap_closed = 0, gap_gave_up = 0;
    unsigned generation = 0;
    double gap_seconds = 0.0;
    AudioObjectID device;

    pthread_mutex_lock(&d->lock);
    running = d->io_clients > 0;
    pthread_mutex_unlock(&d->lock);

    status = ua101_link_hello(d->link, &info, running, why, sizeof(why));
    if (status == UA101_LINK_ERR_ASLEEP) {
        if (strcmp(why, d->last_why) != 0)
            os_log(d->log, "UA-101: %{public}s; the device stays as it is", why);
        snprintf(d->last_why, sizeof(d->last_why), "%s", why);
        return status;
    }
    if (status == UA101_LINK_OK && info.present &&
        (info.capture_channels == 0 || info.capture_channels > MAX_CHANNELS ||
         info.playback_channels == 0 || info.playback_channels > MAX_CHANNELS ||
         info.rate_hz <= 0.0)) {
        status = UA101_LINK_ERR_REQUEST;
        snprintf(why, sizeof(why), "a format it can't use: %.0f Hz, %u in, %u out", info.rate_hz,
                 info.capture_channels, info.playback_channels);
    }
    if (status == UA101_LINK_OK && !info.present) {
        status = UA101_LINK_ERR_ABSENT;
        snprintf(why, sizeof(why), "the unit isn't there");
    }

    pthread_mutex_lock(&d->lock);
    if (status == UA101_LINK_OK && running) {
        if (!same_format(&info, &d->info) || !d->alive) {
            alive_changed = d->alive;
            d->alive = 0;
            d->gap = GAP_NONE; /* back, but not as it was: the gap has nothing left to wait for */
        } else {
            gap_closed = d->gap;
            d->gap = GAP_NONE;
            uint32_t state = atomic_load(&d->io->state);

            /* The helper's stream stopped, or the helper now streams into another block: it
               opened the unit again, or crashed and launchd started a new one. A helper that
               crashed leaves the old block saying it is still running. */
            restart = state == UA101_STATE_FAILED || state == UA101_STATE_STOPPED ||
                      info.block_id != d->info.block_id;
        }
    } else if (status == UA101_LINK_OK) {
        if (!d->published || !d->alive || !same_format(&info, &d->info)) {
            d->device_id = d->next_id++;
            d->input_id = d->next_id++;
            d->output_id = d->next_id++;
            list_changed = 1;
        }
        gap_closed = d->gap && !list_changed;
        d->gap = GAP_NONE;
        d->info = info;
        d->published = 1;
        d->alive = 1;
    } else {
        if (d->published && d->alive && d->gap == GAP_OVERTIME && !running) {
            /* The time was up long ago and only running I/O kept the device: that has stopped. */
            d->gap = GAP_NONE;
            d->published = 0;
            list_changed = 1;
            gap_gave_up = 1;
        } else if (d->published && d->alive && (d->gap || d->gap_seconds > 0.0)) {
            /* The gap: the device stays as it is, and one timer says when to stop waiting. */
            if (!d->gap) {
                d->gap = GAP_WAITING;
                gap_began = 1;
                generation = ++d->gap_generation;
                gap_seconds = d->gap_seconds;
            }
        } else if (d->published && !running) {
            d->published = 0;
            list_changed = 1;
        } else if (d->published && d->alive) {
            d->alive = 0;
            alive_changed = 1;
        }
        /* A helper that watches the bus says when a unit appears; otherwise ask again. */
        retry = !d->retry_pending && !info.watching;
        d->retry_pending |= retry;
    }
    device = d->device_id;
    pthread_mutex_unlock(&d->lock);

    if (status != UA101_LINK_OK && strcmp(why, d->last_why) != 0)
        os_log(d->log, "UA-101 not available: %{public}s", why);
    snprintf(d->last_why, sizeof(d->last_why), "%s", why);
    if (status == UA101_LINK_OK && list_changed)
        os_log(d->log, "UA-101 at %.0f Hz, %u in, %u out", info.rate_hz, info.capture_channels,
               info.playback_channels);
    if (gap_began) {
        os_log(d->log, "UA-101: the device stays listed for %.0f s in case the unit comes back; "
                       "nothing can start meanwhile",
               gap_seconds);
        gap_ends_later(d, generation, gap_seconds);
    }
    if (gap_closed)
        os_log(d->log, "UA-101 is back as it was: the same device carries on");
    if (gap_gave_up)
        os_log(d->log, "UA-101 is still away and I/O has stopped: the device goes from the list");
    if (list_changed)
        notify_device_list(d);
    if (alive_changed)
        notify(d, device, kAudioDevicePropertyDeviceIsAlive);
    if (restart && d->host != NULL) {
        os_log(d->log, "UA-101's stream started over; restarting I/O");
        /* From the queue with no lock held, never from inside a call from Core Audio. */
        dispatch_async(d->queue, ^{
          d->host->RequestDeviceConfigurationChange(d->host, device, CHANGE_RESTART, NULL);
        });
    }
    if (retry)
        retry_later(d);
    return status;
}

/* On d->queue. */
static void say_hello(driver *d)
{
    pthread_mutex_lock(&d->map_lock);
    say_hello_locked(d);
    pthread_mutex_unlock(&d->map_lock);
}

static void on_link_change(void *context)
{
    driver *d = context;

    dispatch_async(d->queue, ^{
      say_hello(d);
    });
}

/* For tests: returns once the hellos asked for so far have been handled. */
void ua101_plugin_wait_for_hello(void)
{
    if (g_driver.queue != NULL)
        dispatch_sync(g_driver.queue, ^{
                      });
}

/* --- Properties. --- */

/* Copies a value out, or gives its size when `out` is NULL. */
static OSStatus put(void *out, UInt32 avail, UInt32 *out_size, const void *value, UInt32 bytes)
{
    if (out == NULL) {
        *out_size = bytes;
        return 0;
    }
    if (avail < bytes)
        return kAudioHardwareBadPropertySizeError;
    memcpy(out, value, bytes);
    *out_size = bytes;
    return 0;
}

#define PUT(value) put(out, avail, out_size, &(value), (UInt32)sizeof(value))

static OSStatus put_u32(void *out, UInt32 avail, UInt32 *out_size, UInt32 v)
{
    return put(out, avail, out_size, &v, sizeof(v));
}

/* A list of objects: as many as fit, as the HAL expects of lists. */
static OSStatus put_ids(void *out, UInt32 avail, UInt32 *out_size, const AudioObjectID *ids,
                        UInt32 count)
{
    UInt32 n = count;

    if (out == NULL) {
        *out_size = count * (UInt32)sizeof(AudioObjectID);
        return 0;
    }
    if (n > avail / sizeof(AudioObjectID))
        n = avail / (UInt32)sizeof(AudioObjectID);
    memcpy(out, ids, n * sizeof(AudioObjectID));
    *out_size = n * (UInt32)sizeof(AudioObjectID);
    return 0;
}

/* A string: the caller releases it. */
static OSStatus put_string(void *out, UInt32 avail, UInt32 *out_size, const char *text)
{
    CFStringRef s;

    if (out == NULL) {
        *out_size = sizeof(CFStringRef);
        return 0;
    }
    if (avail < sizeof(CFStringRef))
        return kAudioHardwareBadPropertySizeError;
    s = CFStringCreateWithCString(NULL, text, kCFStringEncodingUTF8);
    memcpy(out, &s, sizeof(s));
    *out_size = sizeof(CFStringRef);
    return 0;
}

/* A channel's name, as printed on the box, from the jack map measured with the user on
 * 2026-11-14: capture 1-8 are INPUT 1-8,
 * 9-10 OPTICAL IN left and right, 11-12 the monitor mix (what MONITOR OUT plays); playback 1-8
 * are OUTPUT 1-8, 9-10 OPTICAL OUT left and right. At 192 kHz the unit carries only the first 6
 * each way, in the same order, so a name follows the channel's number and the format's count.
 * NULL for a channel the format doesn't have, and for element 0 (the device as a whole). */
static const char *channel_name(const driver *d, int input, UInt32 channel)
{
    static const char *const in_names[] = {
        "INPUT 1", "INPUT 2", "INPUT 3", "INPUT 4", "INPUT 5", "INPUT 6",
        "INPUT 7", "INPUT 8", "OPTICAL IN L", "OPTICAL IN R", "MONITOR OUT L", "MONITOR OUT R"};
    static const char *const out_names[] = {
        "OUTPUT 1", "OUTPUT 2", "OUTPUT 3", "OUTPUT 4", "OUTPUT 5",
        "OUTPUT 6", "OUTPUT 7", "OUTPUT 8", "OPTICAL OUT L", "OPTICAL OUT R"};
    const UInt32 count = input ? d->info.capture_channels : d->info.playback_channels;
    const UInt32 named = input ? (UInt32)(sizeof(in_names) / sizeof(in_names[0]))
                               : (UInt32)(sizeof(out_names) / sizeof(out_names[0]));

    if (channel < 1 || channel > count || channel > named)
        return NULL;
    return input ? in_names[channel - 1] : out_names[channel - 1];
}

static void virtual_format(const driver *d, int input, AudioStreamBasicDescription *f)
{
    UInt32 channels = input ? d->info.capture_channels : d->info.playback_channels;

    memset(f, 0, sizeof(*f));
    f->mSampleRate = d->info.rate_hz;
    f->mFormatID = kAudioFormatLinearPCM;
    f->mFormatFlags = kAudioFormatFlagsNativeFloatPacked;
    f->mBytesPerPacket = 4 * channels;
    f->mFramesPerPacket = 1;
    f->mBytesPerFrame = 4 * channels;
    f->mChannelsPerFrame = channels;
    f->mBitsPerChannel = 32;
}

/* The same as the virtual format, and it has to be: Core Audio hands ReadInput and WriteMix
 * buffers in the stream's native (physical) format, and this plug-in reads and writes floats
 * there. Declared as 24-bit integers (until 2026-11-10), the real host converted on top:
 * recorded floats were read as integers, and the mix arrived as integers. The linked-in tests
 * call DoIOOperation themselves and couldn't see it. Float holds the unit's 24 bits exactly. */
static void physical_format(const driver *d, int input, AudioStreamBasicDescription *f)
{
    virtual_format(d, input, f);
}

static int same_asbd(const AudioStreamBasicDescription *a, const AudioStreamBasicDescription *b)
{
    return a->mSampleRate == b->mSampleRate && a->mFormatID == b->mFormatID &&
           a->mFormatFlags == b->mFormatFlags && a->mBytesPerPacket == b->mBytesPerPacket &&
           a->mFramesPerPacket == b->mFramesPerPacket && a->mBytesPerFrame == b->mBytesPerFrame &&
           a->mChannelsPerFrame == b->mChannelsPerFrame && a->mBitsPerChannel == b->mBitsPerChannel;
}

static uint32_t zero_timestamp_period(double rate_hz)
{
    uint32_t period = 16384;

    while (period < rate_hz / 3.0)
        period *= 2;
    return period;
}

static OSStatus plugin_property(driver *d, const AudioObjectPropertyAddress *a, UInt32 qsize,
                                const void *q, UInt32 avail, UInt32 *out_size, void *out)
{
    AudioObjectID devices[1] = {d->device_id};
    UInt32 count = d->published ? 1 : 0;

    switch (a->mSelector) {
    case kAudioObjectPropertyBaseClass:
        return put_u32(out, avail, out_size, kAudioObjectClassID);
    case kAudioObjectPropertyClass:
        return put_u32(out, avail, out_size, kAudioPlugInClassID);
    case kAudioObjectPropertyOwner:
        return put_u32(out, avail, out_size, kAudioObjectUnknown);
    case kAudioObjectPropertyManufacturer:
        return put_string(out, avail, out_size, "ua101-driver (unofficial)");
    case kAudioObjectPropertyOwnedObjects:
    case kAudioPlugInPropertyDeviceList:
        return put_ids(out, avail, out_size, devices, count);
    case kAudioPlugInPropertyBoxList:
        return put_ids(out, avail, out_size, devices, 0);
    case kAudioPlugInPropertyTranslateUIDToBox:
        return put_u32(out, avail, out_size, kAudioObjectUnknown);
    case kAudioPlugInPropertyTranslateUIDToDevice: {
        AudioObjectID found = kAudioObjectUnknown;
        CFStringRef uid;

        if (out != NULL) {
            if (qsize != sizeof(CFStringRef) || q == NULL)
                return kAudioHardwareBadPropertySizeError;
            memcpy(&uid, q, sizeof(uid));
            if (d->published && uid != NULL) {
                CFStringRef mine = CFStringCreateWithCString(NULL, DEVICE_UID,
                                                             kCFStringEncodingUTF8);

                if (CFStringCompare(uid, mine, 0) == kCFCompareEqualTo)
                    found = d->device_id;
                CFRelease(mine);
            }
        }
        return put_u32(out, avail, out_size, found);
    }
    case kAudioPlugInPropertyResourceBundle:
        return put_string(out, avail, out_size, "");
    default:
        return kAudioHardwareUnknownPropertyError;
    }
}

/* A number as a property list value: the caller releases it. */
static OSStatus put_number(void *out, UInt32 avail, UInt32 *out_size, double value)
{
    CFNumberRef n;

    if (out == NULL) {
        *out_size = sizeof(CFNumberRef);
        return 0;
    }
    if (avail < sizeof(CFNumberRef))
        return kAudioHardwareBadPropertySizeError;
    n = CFNumberCreate(NULL, kCFNumberDoubleType, &value);
    memcpy(out, &n, sizeof(n));
    *out_size = sizeof(CFNumberRef);
    return 0;
}

static void stat_number(CFMutableDictionaryRef dict, const char *key, double value)
{
    CFStringRef k = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
    CFNumberRef n = CFNumberCreate(NULL, kCFNumberDoubleType, &value);

    if (k != NULL && n != NULL)
        CFDictionarySetValue(dict, k, n);
    if (k != NULL)
        CFRelease(k);
    if (n != NULL)
        CFRelease(n);
}

/* The statistics property: the run that is on, or the last one (src/ua101_drvset.h). Under
   lock. The caller releases it. */
static CFDictionaryRef copy_stats(const driver *d)
{
    CFMutableDictionaryRef dict = CFDictionaryCreateMutable(
        NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    const int running = d->io_clients > 0;
    int64_t cap, play;
    uint64_t missing, late;
    double seconds;

    if (dict == NULL)
        return NULL;
    CFDictionarySetValue(dict, CFSTR(UA101_STAT_RUNNING), running ? kCFBooleanTrue : kCFBooleanFalse);
    if (!d->run.any)
        return dict;
    if (running) {
        uint64_t now = mach_absolute_time();

        seconds = now > d->run.start_host && d->run.ticks_per_second > 0.0
                      ? (double)(now - d->run.start_host) / d->run.ticks_per_second
                      : 0.0;
        cap = atomic_load_explicit(&d->cap_spare, memory_order_relaxed);
        play = atomic_load_explicit(&d->play_spare, memory_order_relaxed);
        missing = atomic_load_explicit(&d->input_missing, memory_order_relaxed) - d->run.missing_base;
        late = atomic_load_explicit(&d->play_late, memory_order_relaxed);
    } else {
        seconds = d->run.seconds;
        cap = d->run.cap_spare;
        play = d->run.play_spare;
        missing = d->run.missing;
        late = d->run.late;
    }
    stat_number(dict, UA101_STAT_RATE, d->run.rate);
    stat_number(dict, UA101_STAT_SECONDS, seconds);
    stat_number(dict, UA101_STAT_INPUT_OFFSET, d->run.in_offset);
    stat_number(dict, UA101_STAT_OUTPUT_OFFSET, d->run.out_offset);
    if (cap != UA101_SPARE_NONE)
        stat_number(dict, UA101_STAT_CAPTURE_SPARE, (double)cap);
    stat_number(dict, UA101_STAT_CAPTURE_MISSING, (double)missing);
    if (play != UA101_SPARE_NONE)
        stat_number(dict, UA101_STAT_PLAYBACK_SPARE, (double)play);
    stat_number(dict, UA101_STAT_PLAYBACK_LATE, (double)late);
    return dict;
}

static OSStatus device_property(driver *d, const AudioObjectPropertyAddress *a, UInt32 avail,
                                UInt32 *out_size, void *out)
{
    const int input = a->mScope == kAudioObjectPropertyScopeInput;
    const int output = a->mScope == kAudioObjectPropertyScopeOutput;
    AudioObjectID streams[2] = {d->input_id, d->output_id};

    switch (a->mSelector) {
    case kAudioObjectPropertyBaseClass:
        return put_u32(out, avail, out_size, kAudioObjectClassID);
    case kAudioObjectPropertyClass:
        return put_u32(out, avail, out_size, kAudioDeviceClassID);
    case kAudioObjectPropertyOwner:
        return put_u32(out, avail, out_size, kAudioObjectPlugInObject);
    case kAudioObjectPropertyName:
    case kAudioObjectPropertyModelName:
        return put_string(out, avail, out_size, "UA-101");
    case kAudioObjectPropertyManufacturer:
        return put_string(out, avail, out_size, "EDIROL");
    case kAudioObjectPropertyOwnedObjects:
    case kAudioDevicePropertyStreams:
        if (input)
            return put_ids(out, avail, out_size, &streams[0], 1);
        if (output)
            return put_ids(out, avail, out_size, &streams[1], 1);
        return put_ids(out, avail, out_size, streams, 2);
    case kAudioObjectPropertyControlList:
        return put_ids(out, avail, out_size, streams, 0);
    case kAudioDevicePropertyDeviceUID:
        return put_string(out, avail, out_size, DEVICE_UID);
    case kAudioDevicePropertyModelUID:
        return put_string(out, avail, out_size, MODEL_UID);
    case kAudioDevicePropertyTransportType:
        return put_u32(out, avail, out_size, kAudioDeviceTransportTypeUSB);
    case kAudioDevicePropertyRelatedDevices:
        return put_ids(out, avail, out_size, &d->device_id, 1);
    case kAudioDevicePropertyClockDomain:
        return put_u32(out, avail, out_size, 0);
    case kAudioDevicePropertyDeviceIsAlive:
        return put_u32(out, avail, out_size, d->alive ? 1 : 0);
    case kAudioDevicePropertyDeviceIsRunning:
        return put_u32(out, avail, out_size, d->io_clients > 0 ? 1 : 0);
    case kAudioDevicePropertyDeviceCanBeDefaultDevice:
    case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
        return put_u32(out, avail, out_size, 1);
    case kAudioDevicePropertyLatency:
        /* The converters' own delay isn't measured yet; rate correction adds its filter's. */
        return put_u32(out, avail, out_size,
                       (input || output) && correcting(d) ? correction_delay(d->info.rate_hz) : 0);
    case kAudioObjectPropertyCustomPropertyInfoList: {
        AudioServerPlugInCustomPropertyInfo info[4] = {
            {PROPERTY_RATE_CORRECTION, kAudioServerPlugInCustomPropertyDataTypeCFPropertyList,
             kAudioServerPlugInCustomPropertyDataTypeNone},
            {UA101_PROPERTY_INPUT_MARGIN, kAudioServerPlugInCustomPropertyDataTypeCFPropertyList,
             kAudioServerPlugInCustomPropertyDataTypeNone},
            {UA101_PROPERTY_OUTPUT_DELAY, kAudioServerPlugInCustomPropertyDataTypeCFPropertyList,
             kAudioServerPlugInCustomPropertyDataTypeNone},
            {UA101_PROPERTY_STATS, kAudioServerPlugInCustomPropertyDataTypeCFPropertyList,
             kAudioServerPlugInCustomPropertyDataTypeNone}};

        return PUT(info);
    }
    /* The two settings, in milliseconds: the user's, or the helper's value while none is set. */
    case UA101_PROPERTY_INPUT_MARGIN:
        return put_number(out, avail, out_size,
                          d->in_margin_wanted > 0.0
                              ? d->in_margin_wanted * 1000.0
                              : d->info.input_offset * 1000.0 / d->info.rate_hz);
    case UA101_PROPERTY_OUTPUT_DELAY:
        return put_number(out, avail, out_size,
                          d->out_delay_wanted > 0.0
                              ? d->out_delay_wanted * 1000.0
                              : d->info.output_offset * 1000.0 / d->info.rate_hz);
    case UA101_PROPERTY_STATS: {
        CFDictionaryRef stats;

        if (out == NULL) {
            *out_size = sizeof(CFDictionaryRef);
            return 0;
        }
        if (avail < sizeof(CFDictionaryRef))
            return kAudioHardwareBadPropertySizeError;
        stats = copy_stats(d);
        if (stats == NULL)
            return kAudioHardwareUnspecifiedError;
        memcpy(out, &stats, sizeof(stats));
        *out_size = sizeof(CFDictionaryRef);
        return 0;
    }
    case PROPERTY_RATE_CORRECTION: {
        /* The setting, not what running I/O uses. The caller releases it. */
        CFBooleanRef v = d->correct_wanted ? kCFBooleanTrue : kCFBooleanFalse;
        OSStatus status = PUT(v);

        if (status == 0 && out != NULL)
            CFRetain(v);
        return status;
    }
    case kAudioDevicePropertySafetyOffset:
        return put_u32(out, avail, out_size,
                       input ? input_offset(d) : output ? output_offset(d) : 0);
    case kAudioDevicePropertyNominalSampleRate: {
        Float64 rate = d->info.rate_hz;

        return PUT(rate);
    }
    case kAudioDevicePropertyAvailableNominalSampleRates: {
        AudioValueRange range = {d->info.rate_hz, d->info.rate_hz};

        if (out != NULL && avail < sizeof(range)) {
            *out_size = 0; /* a list with room for none */
            return 0;
        }
        return PUT(range);
    }
    case kAudioDevicePropertyIsHidden:
        return put_u32(out, avail, out_size, 0);
    case kAudioDevicePropertyPreferredChannelsForStereo: {
        UInt32 pair[2] = {1, 2};

        return put(out, avail, out_size, pair, sizeof(pair));
    }
    case kAudioDevicePropertyZeroTimeStampPeriod:
        return put_u32(out, avail, out_size, zero_timestamp_period(d->info.rate_hz));
    case kAudioObjectPropertyElementName: {
        /* Per channel, in the input or output scope: what Audio MIDI Setup and apps show. */
        const char *name = input || output ? channel_name(d, input, a->mElement) : NULL;

        if (name == NULL)
            return kAudioHardwareUnknownPropertyError;
        return put_string(out, avail, out_size, name);
    }
    default:
        return kAudioHardwareUnknownPropertyError;
    }
}

static OSStatus stream_property(driver *d, int input, const AudioObjectPropertyAddress *a,
                                UInt32 avail, UInt32 *out_size, void *out)
{
    AudioStreamBasicDescription f;
    AudioStreamRangedDescription r;

    switch (a->mSelector) {
    case kAudioObjectPropertyBaseClass:
        return put_u32(out, avail, out_size, kAudioObjectClassID);
    case kAudioObjectPropertyClass:
        return put_u32(out, avail, out_size, kAudioStreamClassID);
    case kAudioObjectPropertyOwner:
        return put_u32(out, avail, out_size, d->device_id);
    case kAudioObjectPropertyOwnedObjects:
        return put_ids(out, avail, out_size, &d->device_id, 0);
    case kAudioStreamPropertyIsActive:
        return put_u32(out, avail, out_size, input ? d->input_active : d->output_active);
    case kAudioStreamPropertyDirection:
        return put_u32(out, avail, out_size, input ? 1 : 0);
    case kAudioStreamPropertyTerminalType:
        return put_u32(out, avail, out_size, kAudioStreamTerminalTypeLine);
    case kAudioStreamPropertyStartingChannel:
        return put_u32(out, avail, out_size, 1);
    case kAudioStreamPropertyLatency:
        return put_u32(out, avail, out_size, 0);
    case kAudioStreamPropertyVirtualFormat:
        virtual_format(d, input, &f);
        return PUT(f);
    case kAudioStreamPropertyPhysicalFormat:
        physical_format(d, input, &f);
        return PUT(f);
    case kAudioStreamPropertyAvailableVirtualFormats:
    case kAudioStreamPropertyAvailablePhysicalFormats:
        if (a->mSelector == kAudioStreamPropertyAvailableVirtualFormats)
            virtual_format(d, input, &r.mFormat);
        else
            physical_format(d, input, &r.mFormat);
        r.mSampleRateRange.mMinimum = d->info.rate_hz;
        r.mSampleRateRange.mMaximum = d->info.rate_hz;
        if (out != NULL && avail < sizeof(r)) {
            *out_size = 0;
            return 0;
        }
        return PUT(r);
    default:
        return kAudioHardwareUnknownPropertyError;
    }
}

/* One place answers both "how big" (out == NULL) and "what", so they can't disagree. */
static OSStatus property(driver *d, AudioObjectID object, const AudioObjectPropertyAddress *a,
                         UInt32 qsize, const void *q, UInt32 avail, UInt32 *out_size, void *out)
{
    OSStatus status;
    UInt32 ignored = 0;

    if (a == NULL)
        return kAudioHardwareIllegalOperationError;
    if (out_size == NULL)
        out_size = &ignored;
    pthread_mutex_lock(&d->lock);
    if (object == kAudioObjectPlugInObject)
        status = plugin_property(d, a, qsize, q, avail, out_size, out);
    else if (d->published && object == d->device_id)
        status = device_property(d, a, avail, out_size, out);
    else if (d->published && (object == d->input_id || object == d->output_id))
        status = stream_property(d, object == d->input_id, a, avail, out_size, out);
    else
        status = kAudioHardwareBadObjectError;
    pthread_mutex_unlock(&d->lock);
    return status;
}

static Boolean has_property(AudioServerPlugInDriverRef ref, AudioObjectID object, pid_t pid,
                            const AudioObjectPropertyAddress *a)
{
    driver *d = from_ref(ref);
    UInt32 size = 0;

    (void)pid;
    return d != NULL && property(d, object, a, 0, NULL, 0, &size, NULL) == 0;
}

static OSStatus is_property_settable(AudioServerPlugInDriverRef ref, AudioObjectID object,
                                     pid_t pid, const AudioObjectPropertyAddress *a,
                                     Boolean *settable)
{
    driver *d = from_ref(ref);
    UInt32 size = 0;
    OSStatus status;

    (void)pid;
    if (d == NULL || settable == NULL)
        return kAudioHardwareBadObjectError;
    status = property(d, object, a, 0, NULL, 0, &size, NULL);
    if (status != 0)
        return status;
    /* Whether a stream is in use, rate correction and the two safety offsets. The rate is the
       unit's switch. */
    *settable = object != kAudioObjectPlugInObject &&
                (a->mSelector == kAudioStreamPropertyIsActive ||
                 a->mSelector == PROPERTY_RATE_CORRECTION ||
                 a->mSelector == UA101_PROPERTY_INPUT_MARGIN ||
                 a->mSelector == UA101_PROPERTY_OUTPUT_DELAY);
    return 0;
}

static OSStatus get_property_data_size(AudioServerPlugInDriverRef ref, AudioObjectID object,
                                       pid_t pid, const AudioObjectPropertyAddress *a,
                                       UInt32 qsize, const void *q, UInt32 *size)
{
    driver *d = from_ref(ref);

    (void)pid;
    if (d == NULL || size == NULL)
        return kAudioHardwareBadObjectError;
    return property(d, object, a, qsize, q, 0, size, NULL);
}

static OSStatus get_property_data(AudioServerPlugInDriverRef ref, AudioObjectID object,
                                  pid_t pid, const AudioObjectPropertyAddress *a, UInt32 qsize,
                                  const void *q, UInt32 avail, UInt32 *out_size, void *out)
{
    driver *d = from_ref(ref);

    (void)pid;
    if (d == NULL || out == NULL || out_size == NULL)
        return kAudioHardwareBadObjectError;
    return property(d, object, a, qsize, q, avail, out_size, out);
}

static OSStatus set_property_data(AudioServerPlugInDriverRef ref, AudioObjectID object,
                                  pid_t pid, const AudioObjectPropertyAddress *a, UInt32 qsize,
                                  const void *q, UInt32 size, const void *data)
{
    driver *d = from_ref(ref);
    AudioObjectID changed_object = kAudioObjectUnknown;
    OSStatus status;
    UInt32 ignored;
    int correction_changed = 0, correction = 0, running = 0;
    int margin_changed = 0, margin_input = 0, margin_moves = 0;
    double margin_ms = 0.0, margin_was_ms = 0.0;

    (void)pid;
    if (d == NULL || a == NULL)
        return kAudioHardwareBadObjectError;
    status = property(d, object, a, qsize, q, 0, &ignored, NULL);
    if (status != 0)
        return status;
    if (data == NULL)
        return kAudioHardwareIllegalOperationError;

    pthread_mutex_lock(&d->lock);
    if (object == kAudioObjectPlugInObject) {
        status = kAudioHardwareUnsupportedOperationError;
    } else if (a->mSelector == kAudioStreamPropertyIsActive &&
               (object == d->input_id || object == d->output_id)) {
        UInt32 v;

        if (size < sizeof(v)) {
            status = kAudioHardwareBadPropertySizeError;
        } else {
            int *flag = object == d->input_id ? &d->input_active : &d->output_active;

            memcpy(&v, data, sizeof(v));
            if (*flag != (v != 0)) {
                *flag = v != 0;
                changed_object = object;
            }
        }
    } else if (a->mSelector == kAudioDevicePropertyNominalSampleRate && object == d->device_id) {
        Float64 rate;

        if (size < sizeof(rate))
            status = kAudioHardwareBadPropertySizeError;
        else {
            memcpy(&rate, data, sizeof(rate));
            /* The unit's switch sets it; asking for the rate it already has is fine. */
            status = rate == d->info.rate_hz ? 0 : kAudioDeviceUnsupportedFormatError;
        }
    } else if ((a->mSelector == kAudioStreamPropertyVirtualFormat ||
                a->mSelector == kAudioStreamPropertyPhysicalFormat) &&
               (object == d->input_id || object == d->output_id)) {
        AudioStreamBasicDescription want, have;

        if (size < sizeof(want))
            status = kAudioHardwareBadPropertySizeError;
        else {
            memcpy(&want, data, sizeof(want));
            if (a->mSelector == kAudioStreamPropertyVirtualFormat)
                virtual_format(d, object == d->input_id, &have);
            else
                physical_format(d, object == d->input_id, &have);
            status = same_asbd(&want, &have) ? 0 : kAudioDeviceUnsupportedFormatError;
        }
    } else if (a->mSelector == PROPERTY_RATE_CORRECTION && object == d->device_id) {
        CFPropertyListRef v;

        if (size < sizeof(v)) {
            status = kAudioHardwareBadPropertySizeError;
        } else {
            memcpy(&v, data, sizeof(v));
            if (!correction_value(v, &correction))
                status = kAudioHardwareIllegalOperationError;
            else if (correction != d->correct_wanted) {
                d->correct_wanted = correction;
                correction_changed = 1;
                running = d->io_clients > 0;
                changed_object = d->device_id;
            }
        }
    } else if ((a->mSelector == UA101_PROPERTY_INPUT_MARGIN ||
                a->mSelector == UA101_PROPERTY_OUTPUT_DELAY) &&
               object == d->device_id) {
        CFPropertyListRef v;
        int index;

        margin_input = a->mSelector == UA101_PROPERTY_INPUT_MARGIN;
        if (size < sizeof(v)) {
            status = kAudioHardwareBadPropertySizeError;
        } else {
            memcpy(&v, data, sizeof(v));
            /* Only a value from the list: nothing else is ever used as an offset. */
            if (!margin_value(v, &margin_ms) ||
                (index = margin_input ? ua101_input_margin_index(margin_ms)
                                      : ua101_output_delay_index(margin_ms)) < 0) {
                status = kAudioHardwareIllegalOperationError;
            } else {
                const double seconds = margin_input ? ua101_input_margins[index]
                                                    : ua101_output_delays[index];
                double *wanted = margin_input ? &d->in_margin_wanted : &d->out_delay_wanted;
                double *used = margin_input ? &d->in_margin_used : &d->out_delay_used;
                const uint32_t now = margin_input ? input_offset(d) : output_offset(d);

                if (*wanted != seconds) {
                    margin_was_ms = now * 1000.0 / d->info.rate_hz;
                    margin_ms = seconds * 1000.0;
                    *wanted = seconds;
                    margin_changed = 1;
                    /* The same count of frames is nothing new to Core Audio: only the setting
                       is kept. Another count is Core Audio's to take, in a change of its own. */
                    margin_moves = ua101_offset_frames(seconds, d->info.rate_hz) != now;
                    if (!margin_moves)
                        *used = seconds;
                    running = d->io_clients > 0;
                    changed_object = d->device_id;
                }
            }
        }
    } else {
        status = kAudioHardwareUnsupportedOperationError;
    }
    pthread_mutex_unlock(&d->lock);
    if (margin_changed) {
        AudioObjectID device = changed_object;
        const int input = margin_input, moves = margin_moves;
        const double ms = margin_ms;

        os_log(d->log, "%{public}s %.2f ms, was %.2f ms%{public}s",
               input ? "recording margin" : "playback delay", ms, margin_was_ms,
               !moves ? " (the same count of frames)"
                      : running ? ": Core Audio restarts I/O for it" : "");
        notify(d, device, input ? UA101_PROPERTY_INPUT_MARGIN : UA101_PROPERTY_OUTPUT_DELAY);
        /* Kept by Core Audio for next time, and Core Audio is asked to take the new offset.
           Both from the queue, never from inside this call. */
        dispatch_async(d->queue, ^{
          CFNumberRef n;

          if (d->host == NULL)
              return;
          n = CFNumberCreate(NULL, kCFNumberDoubleType, &ms);
          if (n != NULL) {
              d->host->WriteToStorage(
                  d->host, input ? CFSTR(UA101_INPUT_MARGIN_KEY) : CFSTR(UA101_OUTPUT_DELAY_KEY), n);
              CFRelease(n);
          }
          if (moves)
              d->host->RequestDeviceConfigurationChange(d->host, device, CHANGE_OFFSETS, NULL);
        });
    } else if (correction_changed) {
        AudioObjectID device = changed_object;

        os_log(d->log, "rate correction %{public}s%{public}s", correction ? "on" : "off",
               running ? ", from the next start" : "");
        notify(d, device, PROPERTY_RATE_CORRECTION);
        /* Kept by Core Audio for next time; and running I/O is asked to restart, which picks
           the setting up. Both from the queue, never from inside this call. */
        dispatch_async(d->queue, ^{
          if (d->host == NULL)
              return;
          d->host->WriteToStorage(d->host, CFSTR(CORRECTION_KEY),
                                  correction ? kCFBooleanTrue : kCFBooleanFalse);
          if (running)
              d->host->RequestDeviceConfigurationChange(d->host, device, CHANGE_CORRECTION, NULL);
        });
        if (!running)
            notify_latency(d, device);
    } else if (changed_object != kAudioObjectUnknown) {
        notify(d, changed_object, kAudioStreamPropertyIsActive);
    }
    return status;
}

/* --- Basic operations. --- */

static HRESULT query_interface(void *ref, REFIID iid, LPVOID *out)
{
    driver *d = from_ref(ref);
    CFUUIDRef asked;
    HRESULT result = E_NOINTERFACE;

    if (d == NULL || out == NULL)
        return E_POINTER;
    asked = CFUUIDCreateFromUUIDBytes(NULL, iid);
    if (asked == NULL)
        return E_NOINTERFACE;
    if (CFEqual(asked, IUnknownUUID) || CFEqual(asked, kAudioServerPlugInDriverInterfaceUUID)) {
        atomic_fetch_add(&d->refs, 1);
        *out = d;
        result = S_OK;
    } else {
        *out = NULL;
    }
    CFRelease(asked);
    return result;
}

static ULONG add_ref(void *ref)
{
    driver *d = from_ref(ref);

    return d != NULL ? atomic_fetch_add(&d->refs, 1) + 1 : 0;
}

static ULONG release(void *ref)
{
    driver *d = from_ref(ref);

    /* The one driver lives as long as Core Audio's process. */
    if (d == NULL || atomic_load(&d->refs) == 0)
        return 0;
    return atomic_fetch_sub(&d->refs, 1) - 1;
}

/* The two safety offsets from Core Audio's storage, if the user set them: only values from
   the lists, and anything else there leaves the helper's own value in force. */
static void load_offsets(driver *d)
{
    AudioServerPlugInHostRef host = d->host;
    CFPropertyListRef v = NULL;
    double in = 0.0, out = 0.0;

    if (host == NULL)
        return;
    if (host->CopyFromStorage(host, CFSTR(UA101_INPUT_MARGIN_KEY), &v) == 0 && v != NULL) {
        double ms;
        int index = margin_value(v, &ms) ? ua101_input_margin_index(ms) : -1;

        if (index >= 0)
            in = ua101_input_margins[index];
        CFRelease(v);
    }
    v = NULL;
    if (host->CopyFromStorage(host, CFSTR(UA101_OUTPUT_DELAY_KEY), &v) == 0 && v != NULL) {
        double ms;
        int index = margin_value(v, &ms) ? ua101_output_delay_index(ms) : -1;

        if (index >= 0)
            out = ua101_output_delays[index];
        CFRelease(v);
    }
    pthread_mutex_lock(&d->lock);
    d->in_margin_wanted = d->in_margin_used = in;
    d->out_delay_wanted = d->out_delay_used = out;
    pthread_mutex_unlock(&d->lock);
    if (in > 0.0 || out > 0.0)
        os_log(d->log, "the user's settings: recording margin %.2f ms, playback delay %.2f ms "
                       "(0 = the helper's own)",
               in * 1000.0, out * 1000.0);
}

/* For tests: reads the two again, as at Core Audio's start. Only with no I/O running. */
void ua101_plugin_reload_offsets(void)
{
    load_offsets(&g_driver);
}

static OSStatus initialize(AudioServerPlugInDriverRef ref, AudioServerPlugInHostRef host)
{
    driver *d = from_ref(ref);

    if (d == NULL)
        return kAudioHardwareBadObjectError;
    d->host = host;
    d->log = os_log_create("io.github.delta90h.ua101", "plugin");
    d->queue = dispatch_queue_create("ua101.plugin", DISPATCH_QUEUE_SERIAL);
    if (host != NULL) {
        CFPropertyListRef v = NULL;
        int on;

        /* Rate correction stays on unless it was turned off. */
        if (host->CopyFromStorage(host, CFSTR(CORRECTION_KEY), &v) == 0 && v != NULL) {
            if (correction_value(v, &on))
                d->correct_wanted = on;
            CFRelease(v);
        }
    }
    load_offsets(d);
    d->link = ua101_link_open(on_link_change, d);
    if (d->link == NULL)
        return kAudioHardwareUnspecifiedError;
    /* Opening the unit takes a moment; the device appears when it's done. */
    dispatch_async(d->queue, ^{
      say_hello(d);
    });
    return 0;
}

static OSStatus create_device(AudioServerPlugInDriverRef ref, CFDictionaryRef description,
                              const AudioServerPlugInClientInfo *client, AudioObjectID *out_id)
{
    (void)ref;
    (void)description;
    (void)client;
    (void)out_id;
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus destroy_device(AudioServerPlugInDriverRef ref, AudioObjectID device)
{
    (void)ref;
    (void)device;
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus add_device_client(AudioServerPlugInDriverRef ref, AudioObjectID device,
                                  const AudioServerPlugInClientInfo *client)
{
    (void)ref;
    (void)device;
    (void)client;
    return 0;
}

static OSStatus remove_device_client(AudioServerPlugInDriverRef ref, AudioObjectID device,
                                     const AudioServerPlugInClientInfo *client)
{
    (void)ref;
    (void)device;
    (void)client;
    return 0;
}

/* Core Audio has stopped I/O for the change; I/O starts again on the helper's new block. */
static OSStatus perform_configuration_change(AudioServerPlugInDriverRef ref, AudioObjectID device,
                                             UInt64 action, void *info)
{
    driver *d = from_ref(ref);
    int running;

    (void)info;
    if (d == NULL)
        return kAudioHardwareBadObjectError;
    if (action == CHANGE_CORRECTION) {
        /* The setting itself is already changed; StartIO picks it up. */
        pthread_mutex_lock(&d->lock);
        running = d->io_clients > 0;
        pthread_mutex_unlock(&d->lock);
        if (running)
            os_log(d->log, "rate correction changes when I/O next starts");
        notify_latency(d, device);
        return 0;
    }
    if (action == CHANGE_OFFSETS) {
        /* Core Audio has stopped I/O, if any ran, and reads the offsets afresh after this. */
        uint32_t in, out;

        pthread_mutex_lock(&d->lock);
        d->in_margin_used = d->in_margin_wanted;
        d->out_delay_used = d->out_delay_wanted;
        in = input_offset(d);
        out = output_offset(d);
        pthread_mutex_unlock(&d->lock);
        os_log(d->log, "safety offsets now %u frames in, %u out", in, out);
        notify_offsets(d, device);
        return 0;
    }
    if (action != CHANGE_RESTART)
        return 0;
    pthread_mutex_lock(&d->map_lock);
    pthread_mutex_lock(&d->lock);
    running = d->io_clients > 0;
    pthread_mutex_unlock(&d->lock);
    if (running)
        os_log(d->log, "a restart was asked for, but I/O didn't stop");
    else
        say_hello_locked(d);
    pthread_mutex_unlock(&d->map_lock);
    return 0;
}

/* Core Audio won't perform a change it was asked for. For the safety offsets that means the
   old ones stay in force, so the settings go back to them: what a panel shows is then what
   is used, and asking again starts afresh. */
static OSStatus abort_configuration_change(AudioServerPlugInDriverRef ref, AudioObjectID device,
                                           UInt64 action, void *info)
{
    driver *d = from_ref(ref);
    double in, out;

    (void)info;
    if (d == NULL || action != CHANGE_OFFSETS)
        return 0;
    pthread_mutex_lock(&d->lock);
    in = d->in_margin_wanted = d->in_margin_used;
    out = d->out_delay_wanted = d->out_delay_used;
    pthread_mutex_unlock(&d->lock);
    os_log(d->log, "Core Audio didn't take the new safety offsets: the settings are as before");
    dispatch_async(d->queue, ^{
      const double ms[2] = {in * 1000.0, out * 1000.0};
      const CFStringRef keys[2] = {CFSTR(UA101_INPUT_MARGIN_KEY), CFSTR(UA101_OUTPUT_DELAY_KEY)};
      int i;

      if (d->host == NULL)
          return;
      for (i = 0; i < 2; i++) {
          CFNumberRef n = ms[i] > 0.0 ? CFNumberCreate(NULL, kCFNumberDoubleType, &ms[i]) : NULL;

          if (n != NULL) {
              d->host->WriteToStorage(d->host, keys[i], n);
              CFRelease(n);
          } else {
              d->host->DeleteFromStorage(d->host, keys[i]);
          }
      }
    });
    notify_offsets(d, device);
    return 0;
}

/* --- I/O. --- */

/* The filter and both resamplers for rate correction, made here because they allocate. The
   filter (under 2 MB) is kept while the rate stays the same. Returns 0 if memory ran out. */
static int prepare_correction(driver *d, double rate, uint32_t in, uint32_t out)
{
    if (d->filter_rate != rate) {
        if (d->filter_rate != 0.0)
            ua101_resampler_free(&d->filter);
        d->filter_rate = 0.0;
        /* One frame in for one out, as the step only ever moves by parts per million. */
        if (!ua101_resampler_init_live(&d->filter, rate, rate))
            return 0;
        d->filter_rate = rate;
    }
    ua101_live_free(&d->cap);
    ua101_live_free(&d->play);
    return ua101_live_init(&d->cap, &d->filter, in, LIVE_FRAMES) &&
           ua101_live_init(&d->play, &d->filter, out, LIVE_FRAMES);
}

static void use_line(driver *d, const ua101_clock_line *l);

/* Starts the helper's stream and waits for its clock. The caller holds map_lock, not lock.
   On success *correct says whether this run corrects the rate. */
static OSStatus begin_streaming(driver *d, int *correct)
{
    char why[256] = "";
    ua101_shared *sh;
    ua101_clock_line line;
    ua101_frame_view cap_ring, play_ring;
    double rate;
    uint32_t in, out;
    uint64_t limit;
    size_t bytes;
    mach_timebase_info_data_t tb;

    pthread_mutex_lock(&d->lock);
    sh = d->info.shared;
    bytes = d->info.shared_bytes;
    rate = d->info.rate_hz;
    in = d->info.capture_channels;
    out = d->info.playback_channels;
    *correct = d->correct_wanted;
    pthread_mutex_unlock(&d->lock);
    if (sh == NULL)
        return kAudioHardwareNotRunningError;
    if (!ua101_shared_views(sh, bytes, in, out, &cap_ring, &play_ring)) {
        os_log(d->log, "UA-101: the helper's shared block doesn't fit the format it gave");
        return kAudioHardwareNotRunningError;
    }
    if (*correct && !prepare_correction(d, rate, in, out)) {
        os_log(d->log, "no memory for rate correction; running without it");
        *correct = 0;
    }
    if (ua101_link_start(d->link, why, sizeof(why)) != UA101_LINK_OK) {
        os_log(d->log, "UA-101 couldn't start: %{public}s", why);
        return kAudioHardwareNotRunningError;
    }
    /* The helper publishes its clock a fraction of a second after it starts. */
    mach_timebase_info(&tb);
    limit = mach_absolute_time() + (uint64_t)(START_WAIT_SECONDS * 1e9 * tb.denom / tb.numer);
    for (;;) {
        uint32_t state = atomic_load(&sh->state);

        if (state == UA101_STATE_RUNNING && ua101_shared_read(&sh->clock, &line))
            break;
        if (state == UA101_STATE_FAILED || mach_absolute_time() > limit) {
            const ua101_link_counts none = {0};

            os_log(d->log, "UA-101 started, but its clock didn't come");
            ua101_link_stop(d->link, &none);
            return kAudioHardwareNotRunningError;
        }
        usleep(5000);
    }
    /* No I/O runs yet, so the I/O thread's fields can be set without a lock. */
    d->io = sh;
    d->cap_ring = cap_ring;
    d->play_ring = play_ring;
    d->io_in = in;
    d->io_out = out;
    d->zts_period = zero_timestamp_period(rate);
    d->have_zts = 0;
    d->io_correct = *correct;
    d->run_missing = atomic_load(&d->input_missing);
    d->run_clipped = atomic_load(&d->output_clipped);
    atomic_store(&d->output_peak, 0.0f);
    atomic_store(&d->output_cut, 0);
    atomic_store(&d->output_cut_peak, 0.0f);
    atomic_store(&d->cap_spare, UA101_SPARE_NONE);
    atomic_store(&d->play_spare, UA101_SPARE_NONE);
    atomic_store(&d->play_late, 0);
    /* Our timeline starts where the unit's does, then keeps to the nominal rate. */
    d->nominal_ticks = 1e9 * (double)tb.denom / (double)tb.numer / rate;
    d->origin_host = ua101_line_host(&line, 0.0);
    use_line(d, &line);
    d->delay = correction_delay(rate);
    /* The first cycle gives the smoothed line its start: the helper's line as it stands. */
    ua101_steer_init(&d->steer, rate, LOST_FRAMES);
    d->steer_seed = line.seed;
    atomic_store(&d->steer_offset, 0.0);
    atomic_store(&d->steer_starts, 0);
    d->zts_seed = line.seed;
    d->cap_on = d->play_on = 0;
    atomic_store(&d->cap_resets, 0);
    atomic_store(&d->play_resets, 0);
    atomic_store(&d->cap_error, 0.0);
    atomic_store(&d->play_error, 0.0);
    atomic_store(&d->cap_step, 0.0);
    atomic_store(&d->play_step, 0.0);
    return 0;
}

static OSStatus start_io(AudioServerPlugInDriverRef ref, AudioObjectID device, UInt32 client)
{
    driver *d = from_ref(ref);
    OSStatus status = 0;
    int ok, first, held, correct = 0, latency_changed = 0, asleep = 0;

    (void)client;
    if (d == NULL)
        return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&d->map_lock);
    pthread_mutex_lock(&d->lock);
    ok = d->published && device == d->device_id;
    first = ok && d->io_clients == 0;
    /* In a gap nothing starts, not even beside I/O that was running when the unit left. */
    held = ok && d->gap;
    if (ok && !first && !held)
        d->io_clients++;
    pthread_mutex_unlock(&d->lock);
    if (first) {
        /* The helper gives the unit back after a while without streaming, and opens it again,
           with a new block, at the next hello. So each first start says hello: it maps the
           block the helper will stream into, and finds out if the unit went or changed
           meanwhile, in which case this device is gone, dead or in a gap and the start is
           refused. In a gap this hello is also what finds a unit that just came back. */
        int waited = 0, hello = say_hello_locked(d);

        while (hello == UA101_LINK_ERR_ASLEEP &&
               waited < (int)(WAKE_WAIT_SECONDS * 1e6 / WAKE_POLL_US)) {
            usleep(WAKE_POLL_US);
            waited++;
            hello = say_hello_locked(d);
        }
        pthread_mutex_lock(&d->lock);
        ok = d->published && d->alive && device == d->device_id;
        held = ok && d->gap;
        pthread_mutex_unlock(&d->lock);
        first = ok;
        asleep = hello == UA101_LINK_ERR_ASLEEP;
    }
    if (!ok) {
        status = kAudioHardwareBadObjectError;
    } else if (asleep || held) {
        /* The device is still there; it just can't start until the Mac has woken, or until
           the unit is back. */
        status = kAudioHardwareNotRunningError;
    } else if (first) {
        status = begin_streaming(d, &correct);
        if (status == 0) {
            pthread_mutex_lock(&d->lock);
            d->io_clients = 1;
            d->correct_running = correct;
            latency_changed = correct != d->correct_wanted; /* it couldn't be had */
            /* For the statistics property: this run, with the offsets Core Audio has. */
            d->run.any = 1;
            d->run.rate = d->info.rate_hz;
            d->run.ticks_per_second = d->nominal_ticks * d->info.rate_hz;
            d->run.start_host = mach_absolute_time();
            d->run.in_offset = input_offset(d);
            d->run.out_offset = output_offset(d);
            d->run.missing_base = d->run_missing;
            pthread_mutex_unlock(&d->lock);
            notify(d, device, kAudioDevicePropertyDeviceIsRunning);
            if (latency_changed)
                notify_latency(d, device);
        }
    }
    pthread_mutex_unlock(&d->map_lock);
    return status;
}

static OSStatus stop_io(AudioServerPlugInDriverRef ref, AudioObjectID device, UInt32 client)
{
    driver *d = from_ref(ref);
    OSStatus status = 0;
    int last = 0, latency_changed = 0;

    (void)client;
    if (d == NULL)
        return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&d->map_lock);
    pthread_mutex_lock(&d->lock);
    if (!d->published || device != d->device_id)
        status = kAudioHardwareBadObjectError;
    else if (d->io_clients == 0)
        status = kAudioHardwareNotRunningError;
    else
        last = --d->io_clients == 0;
    /* A setting changed while running shows in the latency from now. */
    latency_changed = last && d->correct_running != d->correct_wanted;
    if (last) {
        /* The run as it ended, for the statistics property. Core Audio has stopped the I/O
           thread by now. */
        uint64_t now = mach_absolute_time();

        d->run.seconds = now > d->run.start_host && d->run.ticks_per_second > 0.0
                             ? (double)(now - d->run.start_host) / d->run.ticks_per_second
                             : 0.0;
        d->run.cap_spare = atomic_load(&d->cap_spare);
        d->run.play_spare = atomic_load(&d->play_spare);
        d->run.missing = atomic_load(&d->input_missing) - d->run.missing_base;
        d->run.late = atomic_load(&d->play_late);
    }
    pthread_mutex_unlock(&d->lock);
    if (last) {
        /* Core Audio has stopped the I/O thread by now, so the counts are the run's own. */
        ua101_link_counts counts = {
            .capture_missing = atomic_load(&d->input_missing) - d->run_missing,
            .clipped = atomic_load(&d->output_clipped) - d->run_clipped,
            .clipped_peak = (double)atomic_load(&d->output_peak),
            .corrected = d->io_correct,
            .cut = atomic_load(&d->output_cut),
            .cut_peak = (double)atomic_load(&d->output_cut_peak),
            .input_offset = d->run.in_offset,
            .output_offset = d->run.out_offset,
            .capture_spare_known = atomic_load(&d->cap_spare) != UA101_SPARE_NONE,
            .capture_spare = atomic_load(&d->cap_spare),
        };

        ua101_link_stop(d->link, &counts);
        d->io = NULL;
        notify(d, device, kAudioDevicePropertyDeviceIsRunning);
        if (latency_changed)
            notify_latency(d, device);
        /* Catches up with anything that changed while I/O ran, and maps the helper's current
           block, before the next start. */
        if (say_hello_locked(d) == UA101_LINK_ERR_ASLEEP)
            gap_waits_again(d);
    }
    pthread_mutex_unlock(&d->map_lock);
    return status;
}

static OSStatus get_zero_time_stamp(AudioServerPlugInDriverRef ref, AudioObjectID device,
                                    UInt32 client, Float64 *sample_time, UInt64 *host_time,
                                    UInt64 *seed)
{
    driver *d = from_ref(ref);
    ua101_clock_line line;
    double sample;
    uint64_t host;

    (void)client;
    if (d == NULL || d->io == NULL || device != d->device_id)
        return kAudioHardwareBadObjectError;
    if (d->io_correct) {
        /* Our own timeline: whole periods at exactly the nominal rate from its origin. */
        double now = (double)mach_absolute_time();
        double zero = floor((now - d->origin_host) / d->nominal_ticks / d->zts_period) *
                      d->zts_period;

        *sample_time = zero;
        *host_time = (UInt64)llround(d->origin_host + zero * d->nominal_ticks);
        *seed = d->zts_seed;
        return 0;
    }
    if (ua101_shared_read(&d->io->clock, &line) &&
        ua101_zero_timestamp(&line, mach_absolute_time(), d->zts_period, &sample, &host)) {
        d->zts_sample = sample;
        d->zts_host = host;
        d->zts_seed = line.seed;
        d->have_zts = 1;
    } else if (!d->have_zts) {
        return kAudioHardwareNotRunningError;
    }
    /* If the line couldn't be read just now, the last stamp still holds. */
    *sample_time = d->zts_sample;
    *host_time = d->zts_host;
    *seed = d->zts_seed;
    return 0;
}

static OSStatus will_do_io_operation(AudioServerPlugInDriverRef ref, AudioObjectID device,
                                     UInt32 client, UInt32 operation, Boolean *will_do,
                                     Boolean *in_place)
{
    driver *d = from_ref(ref);

    (void)client;
    if (d == NULL || device != d->device_id || will_do == NULL || in_place == NULL)
        return kAudioHardwareBadObjectError;
    *will_do = operation == kAudioServerPlugInIOOperationReadInput ||
               operation == kAudioServerPlugInIOOperationWriteMix;
    *in_place = true;
    return 0;
}

static OSStatus begin_io_operation(AudioServerPlugInDriverRef ref, AudioObjectID device,
                                   UInt32 client, UInt32 operation, UInt32 frames,
                                   const AudioServerPlugInIOCycleInfo *cycle)
{
    (void)ref;
    (void)device;
    (void)client;
    (void)operation;
    (void)frames;
    (void)cycle;
    return 0;
}

/* One read of capture, for the run's least room: the ring's end as it was before the copy, one
   past the last frame wanted, and what the copy couldn't give (src/ua101_drvset.h). Only while
   the helper streams: once the unit has left, nothing more can come and nothing is measured. */
static void note_capture_spare(driver *d, uint64_t end, uint64_t need_end, uint64_t missed)
{
    int64_t spare;

    if (end == 0 || atomic_load_explicit(&d->io->state, memory_order_relaxed) != UA101_STATE_RUNNING)
        return;
    spare = ua101_spare_of(end, need_end, missed);
    if (spare < atomic_load_explicit(&d->cap_spare, memory_order_relaxed))
        atomic_store_explicit(&d->cap_spare, spare, memory_order_relaxed);
}

/* Capture from sample time `first`, as floats. Frames before sample time 0, or that never
   came, are silence. */
static void read_input(driver *d, int64_t first, UInt32 frames, float *out)
{
    const uint32_t ch = d->io_in;
    UInt32 done = 0;

    while (done < frames) {
        UInt32 n = frames - done < CHUNK_FRAMES ? frames - done : CHUNK_FRAMES;
        int64_t f = first + done;
        ua101_frame_ring_miss miss;
        uint64_t end;

        if (f < 0) {
            UInt32 z = (int64_t)n < -f ? n : (UInt32)-f;

            memset(out + (size_t)done * ch, 0, (size_t)z * ch * sizeof(float));
            done += z;
            continue;
        }
        end = ua101_frame_view_end(&d->cap_ring);
        ua101_frame_view_read(&d->cap_ring, (uint64_t)f, d->in_temp, n, &miss);
        if (miss.not_yet + miss.overwritten != 0)
            atomic_fetch_add_explicit(&d->input_missing, miss.not_yet + miss.overwritten,
                                      memory_order_relaxed);
        note_capture_spare(d, end, (uint64_t)f + n, miss.not_yet + miss.overwritten);
        ua101_capture_to_float(d->in_temp, out + (size_t)done * ch, (size_t)n * ch);
        done += n;
    }
}

/* Floats just converted for playback, of which `clipped` were at or over full scale: counted,
   and the largest noted. Looks at the floats again only when something was clipped. */
static size_t note_clipped(driver *d, const float *in, size_t samples, size_t clipped)
{
    if (clipped != 0) {
        float most = ua101_float_peak(in, samples);

        if (most > atomic_load_explicit(&d->output_peak, memory_order_relaxed))
            atomic_store_explicit(&d->output_peak, most, memory_order_relaxed);
    }
    return clipped;
}

/* Playback at sample time `first`, from floats. */
static void write_output(driver *d, int64_t first, UInt32 frames, const float *in)
{
    const uint32_t ch = d->io_out;
    UInt32 done = 0;
    size_t clipped = 0;

    while (done < frames) {
        UInt32 n = frames - done < CHUNK_FRAMES ? frames - done : CHUNK_FRAMES;
        int64_t f = first + done;

        if (f < 0) {
            done += (int64_t)n < -f ? n : (UInt32)-f;
            continue;
        }
        clipped += note_clipped(d, in + (size_t)done * ch, (size_t)n * ch,
                                ua101_float_to_playback(in + (size_t)done * ch, d->out_temp,
                                                        (size_t)n * ch));
        ua101_frame_view_write(&d->play_ring, (uint64_t)f, d->out_temp, n);
        done += n;
    }
    if (clipped != 0)
        atomic_fetch_add_explicit(&d->output_clipped, clipped, memory_order_relaxed);
}

/* --- Rate correction, on the I/O thread. --- */

/*
 * The helper's clock line, freshly read, or as last read if it's being rewritten just now, as
 * our sample time at its point and unit frames per our frame. Host times are around 10^12
 * ticks, where a double resolves only about a thousandth of a tick, so the mapping works from
 * the line's own point, where the numbers are small: a millionth of a frame matters at 15 kHz.
 */
static void use_line(driver *d, const ua101_clock_line *l)
{
    d->line = *l;
    /* Both host times are close together, so this difference is exact. */
    d->line_time = (l->host - d->origin_host) / d->nominal_ticks;
    d->line_ratio = d->nominal_ticks / l->ticks_per_sample;
}

/*
 * Reads the helper's line and moves the smoothed copy toward it, as of our sample time `now`.
 * Both directions call this each cycle; only the later sample time of the two moves the copy,
 * so it moves once a cycle. A new seed, or a line that jumped, makes the copy start over from
 * the helper's line, and then both resamplers start again in place.
 */
static void steer_to_line(driver *d, int64_t now)
{
    ua101_clock_line l;

    if (ua101_shared_read(&d->io->clock, &l))
        use_line(d, &l);
    if (d->line.seed != d->steer_seed) {
        d->steer_seed = d->line.seed;
        ua101_steer_restart(&d->steer);
    }
    /* The line's point is a recent frame, so the part beside it stays small and exact. */
    ua101_steer_update(&d->steer, now, (int64_t)d->line.sample,
                       ((double)now - d->line_time) * d->line_ratio, d->line_ratio);
    atomic_store_explicit(&d->steer_offset, d->steer.offset, memory_order_relaxed);
    atomic_store_explicit(&d->steer_starts, d->steer.starts, memory_order_relaxed);
}

/* The step that puts the frame `frames` ahead exactly where the smoothed line says it belongs,
   so both directions sit on that line at the end of every cycle. */
static void aim(ua101_live *l, double target, UInt32 frames, _Atomic double *seen)
{
    double step = (target - ua101_live_next_position(l)) / (double)frames;

    if (step < 1.0 - MAX_STEP_OFF)
        step = 1.0 - MAX_STEP_OFF;
    if (step > 1.0 + MAX_STEP_OFF)
        step = 1.0 + MAX_STEP_OFF;
    ua101_live_set_step(l, step);
    atomic_store_explicit(seen, step, memory_order_relaxed);
}

static void note_error(_Atomic double *worst, double error)
{
    if (fabs(error) > atomic_load_explicit(worst, memory_order_relaxed))
        atomic_store_explicit(worst, fabs(error), memory_order_relaxed);
}

/* Feeds the capture resampler `frames` more unit frames from the ring. Frames before the
   unit's 0, or that never came, go in as silence; the missing ones are counted. */
static void feed_capture(driver *d, size_t frames)
{
    while (frames > 0) {
        size_t n = frames < CHUNK_FRAMES ? frames : CHUNK_FRAMES;
        ua101_frame_ring_miss miss;

        if (d->cap_unit < 0) {
            if ((int64_t)n > -d->cap_unit)
                n = (size_t)-d->cap_unit;
            ua101_live_push_silence(&d->cap, n);
        } else {
            uint64_t end = ua101_frame_view_end(&d->cap_ring);

            ua101_frame_view_read(&d->cap_ring, (uint64_t)d->cap_unit, d->in_temp, n, &miss);
            if (miss.not_yet + miss.overwritten != 0)
                atomic_fetch_add_explicit(&d->input_missing, miss.not_yet + miss.overwritten,
                                          memory_order_relaxed);
            note_capture_spare(d, end, (uint64_t)d->cap_unit + n, miss.not_yet + miss.overwritten);
            ua101_live_push(&d->cap, d->in_temp, n);
        }
        d->cap_unit += (int64_t)n;
        frames -= n;
    }
}

/* Capture for our sample times `first` on: the unit's frames, `delay` behind, at the moments
   those sample times stand for. */
static void read_input_corrected(driver *d, int64_t first, UInt32 frames, float *out)
{
    const uint32_t ch = d->io_in;
    UInt32 done = 0;

    steer_to_line(d, first);
    if (d->cap_on && d->steer.starts == d->cap_start && first == d->cap_next) {
        double error = ua101_steer_frame(&d->steer, first, d->cap_base) - (double)d->delay -
                       ua101_live_next_position(&d->cap);

        if (fabs(error) > LOST_FRAMES)
            d->cap_on = 0;
        else if (!d->cap_fresh)
            note_error(&d->cap_error, error);
        d->cap_fresh = 0;
    } else {
        d->cap_on = 0;
    }
    if (!d->cap_on) {
        /* Start again in place: input 0 is the unit frame just before the first one wanted. */
        ua101_live_reset(&d->cap);
        d->cap_base = d->steer.frame +
                      (int64_t)floor(ua101_steer_frame(&d->steer, first, d->steer.frame) -
                                     (double)d->delay);
        d->cap_unit = d->cap_base;
        d->cap_start = d->steer.starts;
        d->cap_on = d->cap_fresh = 1;
        atomic_fetch_add_explicit(&d->cap_resets, 1, memory_order_relaxed);
    }
    d->cap_base += (int64_t)ua101_live_rebase(&d->cap, REBASE_FRAMES);
    aim(&d->cap,
        ua101_steer_frame(&d->steer, first + (int64_t)frames, d->cap_base) - (double)d->delay,
        frames, &d->cap_step);
    while (done < frames) {
        UInt32 n = frames - done < CHUNK_FRAMES ? frames - done : CHUNK_FRAMES;
        size_t got;

        feed_capture(d, ua101_live_needed(&d->cap, n));
        got = ua101_live_pull(&d->cap, d->in_temp, n);
        if (got < n) /* can't happen: it was fed what it needed */
            memset(d->in_temp + got * ch, 0, (n - got) * ch * sizeof(int32_t));
        ua101_capture_to_float(d->in_temp, out + (size_t)done * ch, (size_t)n * ch);
        done += n;
    }
    d->cap_next = first + frames;
}

/* Playback for our sample times `first` on: resampled onto the unit's frames, which play
   `delay` behind the moments those sample times stand for. */
static void write_output_corrected(driver *d, int64_t first, UInt32 frames, const float *in)
{
    const uint32_t ch = d->io_out;
    UInt32 done = 0;
    size_t clipped = 0;

    steer_to_line(d, first);
    if (d->play_on && d->steer.starts == d->play_start && first == d->play_next) {
        double error = ua101_steer_time(&d->steer, d->play_unit, d->play_base) -
                       (double)d->delay - ua101_live_next_position(&d->play);

        if (fabs(error) > LOST_FRAMES)
            d->play_on = 0;
        else if (!d->play_fresh)
            note_error(&d->play_error, error);
        d->play_fresh = 0;
    } else {
        d->play_on = 0;
    }
    if (!d->play_on) {
        /* Start again in place: input 0 is `first`, and the first unit frame made is the
           first one whose moment, less the delay, isn't before it. Its first cycle gives
           fewer frames than it takes, so up to a frame of error is left for the next. */
        ua101_live_reset(&d->play);
        d->play_base = first;
        d->play_unit = d->steer.frame +
                       (int64_t)ceil(ua101_steer_frame(&d->steer, first + (int64_t)d->delay,
                                                       d->steer.frame));
        d->play_start = d->steer.starts;
        d->play_on = d->play_fresh = 1;
        atomic_fetch_add_explicit(&d->play_resets, 1, memory_order_relaxed);
    }
    d->play_base += (int64_t)ua101_live_rebase(&d->play, REBASE_FRAMES);
    aim(&d->play,
        ua101_steer_time(&d->steer, d->play_unit + (int64_t)frames, d->play_base) -
            (double)d->delay,
        frames, &d->play_step);
    while (done < frames) {
        UInt32 n = frames - done < CHUNK_FRAMES ? frames - done : CHUNK_FRAMES;
        size_t got;

        clipped += note_clipped(d, in + (size_t)done * ch, (size_t)n * ch,
                                ua101_float_to_playback(in + (size_t)done * ch, d->out_temp,
                                                        (size_t)n * ch));
        ua101_live_push(&d->play, d->out_temp, n);
        done += n;
        while ((got = ua101_live_pull(&d->play, d->pull_temp, CHUNK_FRAMES + 8)) > 0) {
            int64_t at = d->play_unit;
            size_t skip = at < 0 ? ((int64_t)got < -at ? got : (size_t)-at) : 0;

            if (skip < got)
                ua101_frame_view_write(&d->play_ring, (uint64_t)(at + (int64_t)skip),
                                       d->pull_temp + skip * ch, got - skip);
            d->play_unit += (int64_t)got;
        }
    }
    d->play_next = first + frames;
    if (clipped != 0)
        atomic_fetch_add_explicit(&d->output_clipped, clipped, memory_order_relaxed);
    /* What the correction's own result went over at: the resampler counts it since StartIO. */
    if (d->play.cut != atomic_load_explicit(&d->output_cut, memory_order_relaxed)) {
        atomic_store_explicit(&d->output_cut, d->play.cut, memory_order_relaxed);
        atomic_store_explicit(&d->output_cut_peak, d->play.cut_peak / 8388608.0f,
                              memory_order_relaxed);
    }
}

static OSStatus do_io_operation(AudioServerPlugInDriverRef ref, AudioObjectID device,
                                AudioObjectID stream, UInt32 client, UInt32 operation,
                                UInt32 frames, const AudioServerPlugInIOCycleInfo *cycle,
                                void *main_buffer, void *secondary_buffer)
{
    driver *d = from_ref(ref);

    (void)client;
    (void)secondary_buffer;
    if (d == NULL || d->io == NULL || device != d->device_id || cycle == NULL)
        return kAudioHardwareBadObjectError;
    if (main_buffer == NULL)
        return 0;
    if (frames == 0)
        return 0;
    if (operation == kAudioServerPlugInIOOperationReadInput && stream == d->input_id) {
        int64_t first = (int64_t)llround(cycle->mInputTime.mSampleTime);

        if (d->io_correct)
            read_input_corrected(d, first, frames, main_buffer);
        else
            read_input(d, first, frames, main_buffer);
    } else if (operation == kAudioServerPlugInIOOperationWriteMix && stream == d->output_id) {
        int64_t first = (int64_t)llround(cycle->mOutputTime.mSampleTime);

        if (d->io_correct)
            write_output_corrected(d, first, frames, main_buffer);
        else
            write_output(d, first, frames, main_buffer);
        /* The helper's counts of what playback's room was, kept where a property call may
           read them. */
        atomic_store_explicit(&d->play_spare,
                              atomic_load_explicit(&d->io->stats.playback_spare_min,
                                                   memory_order_relaxed),
                              memory_order_relaxed);
        atomic_store_explicit(&d->play_late,
                              atomic_load_explicit(&d->io->stats.playback_late,
                                                   memory_order_relaxed),
                              memory_order_relaxed);
    }
    return 0;
}

static OSStatus end_io_operation(AudioServerPlugInDriverRef ref, AudioObjectID device,
                                 UInt32 client, UInt32 operation, UInt32 frames,
                                 const AudioServerPlugInIOCycleInfo *cycle)
{
    (void)ref;
    (void)device;
    (void)client;
    (void)operation;
    (void)frames;
    (void)cycle;
    return 0;
}

static AudioServerPlugInDriverInterface g_interface = {
    NULL,
    query_interface,
    add_ref,
    release,
    initialize,
    create_device,
    destroy_device,
    add_device_client,
    remove_device_client,
    perform_configuration_change,
    abort_configuration_change,
    has_property,
    is_property_settable,
    get_property_data_size,
    get_property_data,
    set_property_data,
    start_io,
    stop_io,
    get_zero_time_stamp,
    will_do_io_operation,
    begin_io_operation,
    do_io_operation,
    end_io_operation,
};

/* The factory named in Info.plist, and the only symbol the bundle exports (it's built with
   hidden visibility). Core Audio asks for the AudioServerPlugIn type. */
__attribute__((visibility("default"))) void *UA101_Create(CFAllocatorRef allocator,
                                                          CFUUIDRef requested_type);
void *UA101_Create(CFAllocatorRef allocator, CFUUIDRef requested_type)
{
    (void)allocator;
    if (requested_type == NULL || !CFEqual(requested_type, kAudioServerPlugInTypeUUID))
        return NULL;
    atomic_fetch_add(&g_driver.refs, 1);
    return &g_driver;
}

/* For reports and tests: frames the I/O cycle found missing, and samples it clipped. */
void ua101_plugin_counts(uint64_t *input_missing, uint64_t *output_clipped)
{
    *input_missing = atomic_load(&g_driver.input_missing);
    *output_clipped = atomic_load(&g_driver.output_clipped);
}

/* For tests: in the run since StartIO, the largest clipped sample, and what the rate
   correction's own result was cut at. Peaks are sizes where full scale is 1.0. */
void ua101_plugin_peaks(float *clipped_peak, uint64_t *cut, float *cut_peak)
{
    *clipped_peak = atomic_load(&g_driver.output_peak);
    *cut = atomic_load(&g_driver.output_cut);
    *cut_peak = atomic_load(&g_driver.output_cut_peak);
}

/* For reports and tests, about the current run's rate correction: the last step each way
   (unit frames per frame for capture, frames per unit frame for playback; 0 if none yet), how
   many times each resampler started in place, counting its first, and the largest error each
   found at the start of a cycle, in frames. Returns whether this run corrects at all. */
int ua101_plugin_correction(double steps[2], uint64_t resets[2], double errors[2])
{
    steps[0] = atomic_load(&g_driver.cap_step);
    steps[1] = atomic_load(&g_driver.play_step);
    resets[0] = atomic_load(&g_driver.cap_resets);
    resets[1] = atomic_load(&g_driver.play_resets);
    errors[0] = atomic_load(&g_driver.cap_error);
    errors[1] = atomic_load(&g_driver.play_error);
    return g_driver.io != NULL && g_driver.io_correct;
}

/* For reports and tests, about the current run's steering: how far the helper's line was from
   the smoothed copy at the last cycle, in unit frames, and how many times the copy started over
   from the helper's line, counting its first. */
void ua101_plugin_steering(double *offset, uint64_t *starts)
{
    *offset = atomic_load(&g_driver.steer_offset);
    *starts = atomic_load(&g_driver.steer_starts);
}
