/*
 * Unplugging, switching off, sleep and wake, and a helper that doesn't answer: the real plug-in
 * (src/plugin/ua101_plugin.c), its real XPC link (src/plugin/ua101_link_xpc.c) and the helper's
 * real service (src/ua101_service.c) in one process, with a pretend unit.
 *
 * The pretend unit stands in for the two parts that need hardware. It implements the USB
 * engine's functions (src/ua101_engine.h): it opens only while "on the bus", and its stream
 * publishes a clock and a heartbeat into the shared block as the real one does, and can be made
 * to die. And it implements the watcher (src/ua101_watch.h), so the test calls "the unit left",
 * "the unit appeared", "the Mac will sleep" and "the Mac woke" by hand, on the service's queue,
 * exactly as IOKit would. A fake host stands in for Core Audio. Nothing here touches a real
 * unit, and nothing is put to sleep.
 *
 * The plug-in keeps the device listed for a while after the unit leaves (30 s as installed),
 * in case it comes back, and for as long as an app's I/O is still running on it. The older
 * cases here run with that time at 0.4 s, so with nothing playing they see the device go as
 * they always did, a moment later; the "gap" cases at the end set longer times and look at
 * what happens inside them.
 */
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../src/plugin/ua101_link_xpc.h"
#include "../src/ua101_engine.h"
#include "../src/ua101_midiport.h"
#include "../src/ua101_service.h"
#include "../src/ua101_shared.h"
#include "../src/ua101_watch.h"

void *UA101_Create(CFAllocatorRef allocator, CFUUIDRef requested_type);
void ua101_plugin_wait_for_hello(void);
void ua101_plugin_set_gap_seconds(double seconds);
void ua101_plugin_reload_offsets(void);

#define SHORT_GAP 0.4 /* for the cases that aren't about the gap: 30 s as installed */

static int failures, checks;

#define CHECK(cond, ...)                                                                           \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(cond)) {                                                                             \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                            \
            printf(__VA_ARGS__);                                                                   \
            printf("\n");                                                                          \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

static double g_tick_s;

static double now_s(void)
{
    return (double)mach_absolute_time() * g_tick_s;
}

/* --- The pretend unit: the engine. --- */

static struct {
    _Atomic int on_bus;        /* plugged in and switched on */
    _Atomic int rate, in, out; /* what it streams at, as the rate switch stands */
    _Atomic int open_hang_ms;  /* the next open blocks this long, as a stuck USB call would */
    _Atomic int start_hang_ms; /* and the next start */
    _Atomic int dies_ms;       /* every stream fails this long after starting; 0 never */
    _Atomic int kill;          /* the running stream fails now, as when the unit goes */
    _Atomic int open_tries, opens, starts, stops;
    _Atomic int is_open, is_streaming;
    _Atomic int records;       /* each stream starts with this many frames in the capture ring */
    _Atomic int spare_known;   /* what the engine says playback's least room was, in frames */
    _Atomic int spare;
} U;

struct ua101_engine {
    ua101_shared *sh;
    pthread_t thread;
    int thread_running;
    atomic_int stop_request;
    const char *_Atomic failure;
    double started, stopped;
};

ua101_engine *ua101_engine_open(const ua101_engine_options *opt, ua101_engine_format *format,
                                FILE *log, char *why, size_t why_bytes)
{
    ua101_engine *e;
    int hang = atomic_exchange(&U.open_hang_ms, 0);

    (void)opt;
    (void)log;
    atomic_fetch_add(&U.open_tries, 1);
    if (hang > 0)
        usleep((useconds_t)hang * 1000);
    if (!atomic_load(&U.on_bus)) {
        snprintf(why, why_bytes, "no UA-101 is connected (pretend)");
        return NULL;
    }
    e = calloc(1, sizeof(*e));
    if (e == NULL)
        return NULL;
    format->rate_hz = atomic_load(&U.rate);
    format->capture_channels = (uint32_t)atomic_load(&U.in);
    format->playback_channels = (uint32_t)atomic_load(&U.out);
    atomic_fetch_add(&U.opens, 1);
    atomic_store(&U.is_open, 1);
    return e;
}

static void *pretend_stream(void *arg)
{
    ua101_engine *e = arg;
    mach_timebase_info_data_t tb;
    double ticks_per_frame;
    int dies = atomic_load(&U.dies_ms);

    /* As the real engine: sample time 0 is about now, and the clock comes at once. */
    mach_timebase_info(&tb);
    ticks_per_frame = 1e9 * (double)tb.denom / (double)tb.numer / e->sh->rate_hz;
    ua101_shared_publish(&e->sh->clock, 0, (double)mach_absolute_time() + 0.37, ticks_per_frame);
    if (atomic_load(&U.records) > 0) {
        /* Recorded silence, so a cycle has frames to find. */
        size_t frames = (size_t)atomic_load(&U.records);
        void *zeros = calloc(frames, e->sh->capture_frame_bytes);

        if (zeros != NULL)
            ua101_frame_ring_write(&e->sh->capture, 0, zeros, frames);
        free(zeros);
    }
    atomic_store(&e->sh->state, UA101_STATE_RUNNING);
    while (!atomic_load(&e->stop_request)) {
        usleep(5000);
        atomic_fetch_add(&e->sh->heartbeat, 1);
        if (atomic_load(&U.kill) || (dies > 0 && now_s() - e->started > dies / 1000.0)) {
            atomic_store(&e->failure, "no capture transfer has completed (pretend)");
            atomic_store(&e->stop_request, 1);
        }
    }
    return NULL;
}

int ua101_engine_start(ua101_engine *e, ua101_shared *shared, size_t shared_bytes)
{
    ua101_frame_view cap, play;
    int hang = atomic_exchange(&U.start_hang_ms, 0);

    if (hang > 0)
        usleep((useconds_t)hang * 1000);
    if (e->thread_running ||
        !ua101_shared_views(shared, shared_bytes, (uint32_t)atomic_load(&U.in),
                            (uint32_t)atomic_load(&U.out), &cap, &play))
        return 0;
    e->sh = shared;
    atomic_store(&e->stop_request, 0);
    atomic_store(&e->failure, NULL);
    atomic_store(&U.kill, 0);
    ua101_frame_ring_reset(&shared->capture);
    ua101_frame_ring_reset(&shared->playback);
    ua101_shared_new_seed(&shared->clock);
    atomic_store(&shared->state, UA101_STATE_STARTING);
    e->started = now_s();
    e->stopped = 0.0;
    if (pthread_create(&e->thread, NULL, pretend_stream, e) != 0)
        return 0;
    e->thread_running = 1;
    atomic_fetch_add(&U.starts, 1);
    atomic_store(&U.is_streaming, 1);
    return 1;
}

const char *ua101_engine_failure(const ua101_engine *e)
{
    return atomic_load(&((ua101_engine *)e)->failure);
}

void ua101_engine_stop(ua101_engine *e)
{
    if (!e->thread_running)
        return;
    atomic_store(&e->stop_request, 1);
    pthread_join(e->thread, NULL);
    e->thread_running = 0;
    e->stopped = now_s();
    atomic_store(&e->sh->state,
                 ua101_engine_failure(e) != NULL ? UA101_STATE_FAILED : UA101_STATE_STOPPED);
    atomic_fetch_add(&U.stops, 1);
    atomic_store(&U.is_streaming, 0);
}

void ua101_engine_summarize(const ua101_engine *e, ua101_engine_summary *out)
{
    memset(out, 0, sizeof(*out));
    out->seconds = (e->stopped > 0.0 ? e->stopped : now_s()) - e->started;
    out->errors = ua101_engine_failure(e) != NULL;
    out->rate_hz = e->sh != NULL ? e->sh->rate_hz : 0.0;
    out->playback_spare_known = atomic_load(&U.spare_known);
    out->playback_spare_frames = atomic_load(&U.spare);
}

void ua101_engine_close(ua101_engine *e)
{
    if (e == NULL)
        return;
    ua101_engine_stop(e);
    atomic_store(&U.is_open, 0);
    free(e);
}

/* --- The unit's MIDI interface: the control app's, which this test has none of
   (tests/test_ctlhelper.c). --- */

ua101_midiport *ua101_midiport_open(dispatch_queue_t queue, const ua101_midiport_events *events,
                                    void *context, FILE *log, char *why, size_t why_bytes)
{
    (void)queue;
    (void)events;
    (void)context;
    (void)log;
    snprintf(why, why_bytes, "no MIDI interface in this test");
    return NULL;
}

int ua101_midiport_send(ua101_midiport *p, const uint8_t *packets, size_t bytes)
{
    (void)p;
    (void)packets;
    (void)bytes;
    return 0;
}

void ua101_midiport_close(ua101_midiport *p)
{
    (void)p;
}

/* --- The pretend unit: the bus and the Mac's sleep. --- */

static struct {
    dispatch_queue_t queue;
    ua101_watch_events events;
} W;

int ua101_watch_start(dispatch_queue_t queue, const ua101_watch_events *events, int *units)
{
    W.queue = queue;
    W.events = *events;
    *units = atomic_load(&U.on_bus);
    return 1;
}

/* Each returns once the helper has handled the event, as IOKit's callbacks do. For "will
   sleep" that is the moment the real watcher lets the Mac sleep. */
static void unit_goes(void)
{
    atomic_store(&U.on_bus, 0);
    atomic_store(&U.kill, 1); /* a running stream's transfers fail too */
    dispatch_sync(W.queue, ^{
      W.events.unit_left();
    });
}

static void unit_comes(void)
{
    atomic_store(&U.on_bus, 1);
    dispatch_sync(W.queue, ^{
      W.events.unit_arrived();
    });
}

static void mac_sleeps(void)
{
    dispatch_sync(W.queue, ^{
      W.events.will_sleep();
    });
}

static void mac_wakes(void)
{
    dispatch_sync(W.queue, ^{
      W.events.did_wake();
    });
}

/* --- The fake host. --- */

static atomic_int g_restarts;
static _Atomic UInt64 g_restart_action;

static OSStatus host_properties_changed(AudioServerPlugInHostRef host, AudioObjectID object,
                                        UInt32 count, const AudioObjectPropertyAddress *a)
{
    (void)host;
    (void)object;
    (void)count;
    (void)a;
    return 0;
}

static OSStatus host_copy(AudioServerPlugInHostRef h, CFStringRef k, CFPropertyListRef *v)
{
    (void)h;
    (void)k;
    *v = NULL;
    return kAudioHardwareUnknownPropertyError;
}

static OSStatus host_write(AudioServerPlugInHostRef h, CFStringRef k, CFPropertyListRef v)
{
    (void)h;
    (void)k;
    (void)v;
    return 0;
}

static OSStatus host_delete(AudioServerPlugInHostRef h, CFStringRef k)
{
    (void)h;
    (void)k;
    return 0;
}

static OSStatus host_request_change(AudioServerPlugInHostRef h, AudioObjectID device,
                                    UInt64 action, void *info)
{
    (void)h;
    (void)device;
    (void)info;
    atomic_store(&g_restart_action, action);
    atomic_fetch_add(&g_restarts, 1);
    return 0;
}

static AudioServerPlugInHostInterface g_host = {host_properties_changed, host_copy, host_write,
                                                host_delete, host_request_change};
static AudioServerPlugInDriverRef D;

/* --- Asking the plug-in. --- */

static int get(AudioObjectID object, AudioObjectPropertySelector s, void *out, UInt32 avail)
{
    AudioObjectPropertyAddress a = {s, kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    UInt32 got = 0;

    if ((*D)->GetPropertyData(D, object, 0, &a, 0, NULL, avail, &got, out) != 0)
        return -1;
    return (int)got;
}

/* The device Core Audio would list, or 0. */
static AudioObjectID device(void)
{
    AudioObjectID ids[4] = {0};
    int n = get(kAudioObjectPlugInObject, kAudioPlugInPropertyDeviceList, ids, sizeof(ids));

    return n >= (int)sizeof(AudioObjectID) ? ids[0] : 0;
}

/* 1 alive, 0 dead, -1 no such device. */
static int alive(AudioObjectID dev)
{
    UInt32 v = 0;

    return get(dev, kAudioDevicePropertyDeviceIsAlive, &v, sizeof(v)) == 4 ? (int)v : -1;
}

static double rate_of(AudioObjectID dev)
{
    Float64 v = 0.0;

    return get(dev, kAudioDevicePropertyNominalSampleRate, &v, sizeof(v)) == 8 ? v : 0.0;
}

static OSStatus start_io(AudioObjectID dev)
{
    return (*D)->StartIO(D, dev, 0);
}

static OSStatus stop_io(AudioObjectID dev)
{
    return (*D)->StopIO(D, dev, 0);
}

static int stamps(AudioObjectID dev)
{
    Float64 sample = 0;
    UInt64 host = 0, seed = 0;

    return (*D)->GetZeroTimeStamp(D, dev, 0, &sample, &host, &seed) == 0 && host != 0;
}

/* What Core Audio does when the plug-in asks for a configuration change. */
static OSStatus host_restarts_io(AudioObjectID dev)
{
    stop_io(dev);
    (*D)->PerformDeviceConfigurationChange(D, dev, atomic_load(&g_restart_action), NULL);
    return start_io(dev);
}

static int wait_until(int (^cond)(void), double seconds)
{
    double limit = now_s() + seconds;

    while (now_s() < limit) {
        if (cond())
            return 1;
        usleep(5000);
    }
    return cond();
}

#define WAIT(cond, seconds) wait_until(^{ return (int)(cond); }, seconds)

/* The helper's log, for the lines a person would look for. */
static char g_log_path[64];

static int log_has(const char *text)
{
    static char buffer[262144];
    FILE *f = fopen(g_log_path, "r");
    size_t n;

    if (f == NULL)
        return 0;
    n = fread(buffer, 1, sizeof(buffer) - 1, f);
    fclose(f);
    buffer[n] = '\0';
    return strstr(buffer, text) != NULL;
}

/* The unit on the bus, its device listed, nothing streaming. Returns the device. */
static AudioObjectID settled(void)
{
    if (!atomic_load(&U.on_bus))
        unit_comes();
    CHECK(WAIT(device() != 0 && alive(device()) == 1, 5.0), "the device is listed");
    return device();
}

/* Returns once the plug-in has asked the helper about the unit again, after `tries` looks for
   it, and has taken the answer in. */
static void plugin_asked(int tries)
{
    CHECK(WAIT(atomic_load(&U.open_tries) > tries, 2.0), "the plug-in asked the helper again");
    ua101_plugin_wait_for_hello();
}

static void unit_goes_and_plugin_knows(void)
{
    int tries = atomic_load(&U.open_tries);

    unit_goes();
    plugin_asked(tries);
}

static void unit_comes_and_plugin_knows(void)
{
    int tries = atomic_load(&U.open_tries);

    unit_comes();
    plugin_asked(tries);
}

/* --- The cases. --- */

/* The unit is off when the Mac starts. The plug-in is told the helper watches the bus, so it
   doesn't keep asking; switching the unit on brings the device, with nothing asked again. */
static void test_switched_on_later(void)
{
    int tries;
    AudioObjectID dev;

    ua101_plugin_wait_for_hello();
    CHECK(WAIT(atomic_load(&U.open_tries) >= 1, 3.0), "the first hello tried the unit");
    CHECK(device() == 0, "no unit, no device");
    tries = atomic_load(&U.open_tries);
    usleep(3500000); /* longer than the plug-in's 3 s between tries */
    CHECK(atomic_load(&U.open_tries) == tries, "nobody keeps asking while the bus is watched "
                                               "(%d tries more)",
          atomic_load(&U.open_tries) - tries);
    unit_comes();
    CHECK(WAIT(device() != 0, 2.0), "switched on: the device appears");
    dev = device();
    CHECK(alive(dev) == 1 && rate_of(dev) == 48000.0, "alive, at the unit's rate");
    CHECK(log_has("a UA-101 appeared on the bus"), "the log says it appeared");
}

/* Unplugged while the helper holds it open with no app using it: it used to stay listed until
   the next start failed. */
static void test_unplug_idle_open(void)
{
    settled();
    CHECK(WAIT(atomic_load(&U.is_open), 1.0), "the helper holds the unit just after a hello");
    unit_goes();
    CHECK(!atomic_load(&U.is_open), "the helper let go of it at once");
    CHECK(WAIT(device() == 0, 2.0), "unplugged while idle: the device goes from the list");
}

/* Unplugged after the helper gave it back (30 s idle as installed; 1 s here). */
static void test_unplug_idle_closed(void)
{
    AudioObjectID dev = settled();
    int tries;

    CHECK(WAIT(!atomic_load(&U.is_open), 4.0), "the helper gives the unit back when idle");
    CHECK(log_has("giving the unit back"), "and says so");
    CHECK(device() == dev && alive(dev) == 1, "the device stays listed meanwhile");
    tries = atomic_load(&U.open_tries);
    unit_goes();
    CHECK(WAIT(device() == 0, 2.0), "unplugged while given back: the device goes from the list");
    CHECK(atomic_load(&U.open_tries) - tries <= 1, "with one look for it, not a search");
}

/* Unplugged or switched off while an app streams, the bus noticing first. */
static void test_unplug_streaming(void)
{
    AudioObjectID dev = settled(), again;

    CHECK(start_io(dev) == 0, "I/O starts");
    CHECK(atomic_load(&U.is_streaming) && stamps(dev), "the unit streams");
    unit_goes();
    CHECK(!atomic_load(&U.is_open) && !atomic_load(&U.is_streaming),
          "the helper stopped and let go at once");
    usleep(800000); /* past the time the device is kept with nothing playing */
    CHECK(device() == dev && alive(dev) == 1, "the device stays, alive, while that I/O runs");
    CHECK(stop_io(dev) == 0, "I/O stops");
    CHECK(WAIT(device() == 0, 2.0), "then it goes from the list");
    CHECK(log_has("streaming FAIL") || log_has("streaming PASS"), "the run left a verdict");

    unit_comes();
    CHECK(WAIT(device() != 0, 2.0), "plugged back: the device returns");
    again = device();
    CHECK(again != dev && alive(again) == 1, "as a new object, alive");
    CHECK(start_io(again) == 0 && atomic_load(&U.is_streaming) && stamps(again),
          "and streams again");
    CHECK(stop_io(again) == 0, "I/O stops");
}

/* What an app plays over full scale reaches the line the run leaves in the helper's log, by the
   real link: the plug-in counts it, the stop carries it, the helper writes it. */
static void test_overs_reach_the_log(void)
{
    enum { FRAMES = 512, MOST_CH = 16 };
    static float buf[FRAMES * MOST_CH];
    AudioObjectPropertyAddress streams = {kAudioDevicePropertyStreams,
                                          kAudioObjectPropertyScopeOutput,
                                          kAudioObjectPropertyElementMain};
    const uint32_t ch = (uint32_t)atomic_load(&U.out);
    AudioObjectID dev = settled(), out = 0;
    AudioServerPlugInIOCycleInfo cycle;
    UInt32 got = 0;
    int k;

    CHECK((*D)->GetPropertyData(D, dev, 0, &streams, 0, NULL, sizeof(out), &got, &out) == 0 &&
              got == sizeof(out) && out != 0 && ch <= MOST_CH,
          "the device has an output stream");
    CHECK(start_io(dev) == 0 && atomic_load(&U.is_streaming) && stamps(dev), "I/O starts");
    memset(&cycle, 0, sizeof(cycle));
    cycle.mInputTime.mFlags = cycle.mOutputTime.mFlags = kAudioTimeStampSampleTimeValid;
    for (k = 0; k < 4; k++) {
        memset(buf, 0, sizeof(buf));
        if (k == 1)
            buf[7 * ch + 3] = -2.0f; /* one sample from an app, twice full scale */
        cycle.mOutputTime.mSampleTime = cycle.mInputTime.mSampleTime = 4096.0 + k * FRAMES;
        CHECK((*D)->DoIOOperation(D, dev, out, 0, kAudioServerPlugInIOOperationWriteMix, FRAMES,
                                  &cycle, buf, NULL) == 0,
              "a cycle of playback");
    }
    CHECK(stop_io(dev) == 0, "I/O stops");
    CHECK(WAIT(log_has("the apps sent 1 samples at or over full scale (highest +6.02 dB); the "
                       "rate correction's own result went over at 0 samples"),
               2.0),
          "the run's line says what the app sent over, how far, and that the correction cut "
          "nothing");
}

/* The room the two delays left reaches the helper's log too, in a line of its own after the
   verdict: the offsets the run used (the plug-in's, by the user's settings), recording's least
   room as the plug-in measured it, and playback's as the engine did. */
static void test_room_reaches_the_log(void)
{
    enum { FRAMES = 512, MOST_CH = 16 };
    static float buf[FRAMES * MOST_CH];
    AudioObjectPropertyAddress streams = {kAudioDevicePropertyStreams,
                                          kAudioObjectPropertyScopeInput,
                                          kAudioObjectPropertyElementMain};
    AudioObjectPropertyAddress margin = {0x5541696Du /* 'UAim' */, kAudioObjectPropertyScopeGlobal,
                                         kAudioObjectPropertyElementMain};
    AudioObjectID dev = settled(), in = 0;
    AudioServerPlugInIOCycleInfo cycle;
    UInt32 got = 0;
    double eight = 8.0;
    CFNumberRef n8 = CFNumberCreate(NULL, kCFNumberDoubleType, &eight);
    int restarts;

    CHECK((*D)->GetPropertyData(D, dev, 0, &streams, 0, NULL, sizeof(in), &got, &in) == 0 &&
              got == sizeof(in) && in != 0,
          "the device has an input stream");
    atomic_store(&U.records, 4096);
    atomic_store(&U.spare_known, 1);
    atomic_store(&U.spare, 137);
    CHECK(start_io(dev) == 0 && atomic_load(&U.is_streaming) && stamps(dev), "I/O starts");
    memset(&cycle, 0, sizeof(cycle));
    cycle.mInputTime.mFlags = cycle.mOutputTime.mFlags = kAudioTimeStampSampleTimeValid;
    cycle.mInputTime.mSampleTime = cycle.mOutputTime.mSampleTime = 1000.0;
    CHECK((*D)->DoIOOperation(D, dev, in, 0, kAudioServerPlugInIOOperationReadInput, FRAMES, &cycle,
                              buf, NULL) == 0,
          "a cycle of recording");
    CHECK(stop_io(dev) == 0, "I/O stops");
    /* 4096 frames recorded, and a cycle for 1000 to 1511. Rate correction is on here, as
       installed: its filter runs 27 frames behind and reaches 26 ahead, so the cycle wanted
       the unit's frames up to 1510: 2585 frames of room, 53.85 ms. */
    CHECK(WAIT(log_has("room: recording read 240 frames behind (5.00 ms), playback written 1024 "
                       "ahead (21.33 ms); the least room left: recording +53.85 ms, playback "
                       "+2.85 ms"),
               2.0),
          "the run's room line: the helper's own offsets, and what each direction had left");

    /* With the user's setting the line names the offset the run really used, and a figure
       nobody measured says so. */
    restarts = atomic_load(&g_restarts);
    CHECK((*D)->SetPropertyData(D, dev, 0, &margin, 0, NULL, sizeof(n8), &n8) == 0, "8 ms set");
    CHECK(WAIT(atomic_load(&g_restarts) == restarts + 1 && atomic_load(&g_restart_action) == 3, 2.0),
          "a change of the offsets is asked of Core Audio");
    CHECK((*D)->PerformDeviceConfigurationChange(D, dev, 3, NULL) == 0, "performed");
    atomic_store(&U.spare_known, 0);
    atomic_store(&U.spare, 0);
    CHECK(start_io(dev) == 0 && stamps(dev), "I/O starts again");
    cycle.mInputTime.mSampleTime = 4096.0 - 12;
    CHECK((*D)->DoIOOperation(D, dev, in, 0, kAudioServerPlugInIOOperationReadInput, FRAMES, &cycle,
                              buf, NULL) == 0,
          "a cycle that wants 499 frames that haven't come");
    CHECK(stop_io(dev) == 0, "I/O stops");
    CHECK(WAIT(log_has("room: recording read 384 frames behind (8.00 ms), playback written 1024 "
                       "ahead (21.33 ms); the least room left: recording -10.40 ms, playback "
                       "not measured"),
               2.0),
          "the room line with 8 ms: 499 frames short, playback not measured");
    CHECK(log_has("streaming FAIL") && log_has("the plug-in found 499 capture frames missing"),
          "and the verdict counts those frames, as before");
    /* A run in which no cycle came: neither figure was measured, and the line says so. */
    CHECK(!log_has("(8.00 ms), playback written 1024 ahead (21.33 ms); the least room left: "
                   "recording not measured"),
          "no such line yet");
    CHECK(start_io(dev) == 0 && stamps(dev) && stop_io(dev) == 0, "a run without a cycle");
    CHECK(WAIT(log_has("room: recording read 384 frames behind (8.00 ms), playback written 1024 "
                       "ahead (21.33 ms); the least room left: recording not measured, playback "
                       "not measured"),
               2.0),
          "its room line: nothing measured either way");

    CFRelease(n8);
    atomic_store(&U.records, 0);
    ua101_plugin_reload_offsets(); /* the fake host keeps nothing: unset again */
    (*D)->PerformDeviceConfigurationChange(D, dev, 3, NULL);
}

/* The same, but the stream fails before the bus says anything, and the bus's word comes late. */
static void test_power_off_engine_first(void)
{
    AudioObjectID dev = settled();

    CHECK(start_io(dev) == 0, "I/O starts");
    atomic_store(&U.on_bus, 0);
    atomic_store(&U.kill, 1);
    CHECK(WAIT(!atomic_load(&U.is_open), 2.0), "the stream failed by itself: the helper let go");
    CHECK(WAIT(log_has("streaming stopped by itself"), 2.0), "the log says why");
    CHECK(device() == dev && alive(dev) == 1, "the device stays while that I/O runs");
    CHECK(stop_io(dev) == 0, "I/O stops");
    CHECK(WAIT(device() == 0, 2.0), "the device goes from the list");
    dispatch_sync(W.queue, ^{
      W.events.unit_left(); /* the bus, late */
    });
    usleep(100000);
    CHECK(device() == 0, "the bus's late word changes nothing");
    unit_comes();
    CHECK(WAIT(device() != 0 && alive(device()) == 1, 2.0), "switched on again: it returns");
}

/* Switched off, the rate switch turned, switched on: the device comes back at the new rate. */
static void test_rate_switch(void)
{
    AudioObjectID dev = settled(), other;

    unit_goes();
    CHECK(WAIT(device() == 0, 2.0), "switched off");
    atomic_store(&U.rate, 96000);
    unit_comes();
    CHECK(WAIT(device() != 0, 2.0), "switched on at another rate");
    other = device();
    CHECK(other != dev && rate_of(other) == 96000.0, "a new device at 96 kHz (%.0f)",
          rate_of(other));
    CHECK(start_io(other) == 0 && stamps(other) && stop_io(other) == 0, "which streams");
    unit_goes();
    CHECK(WAIT(device() == 0, 2.0), "switched off");
    atomic_store(&U.rate, 48000);
    unit_comes();
    CHECK(WAIT(device() != 0 && rate_of(device()) == 48000.0, 2.0), "and back at 48 kHz");
}

/* The Mac sleeps with no app streaming. */
static void test_sleep_idle(void)
{
    AudioObjectID dev = settled();
    int tries;

    if (!atomic_load(&U.is_open)) { /* have the helper holding it, the harder case */
        CHECK(start_io(dev) == 0 && stop_io(dev) == 0, "a start and stop");
    }
    CHECK(atomic_load(&U.is_open), "the helper holds the unit");
    mac_sleeps();
    CHECK(!atomic_load(&U.is_open), "the unit was given back before the Mac was let sleep");
    CHECK(device() == dev && alive(dev) == 1, "the device stays listed and alive");
    tries = atomic_load(&U.open_tries);
    mac_wakes();
    usleep(200000);
    CHECK(atomic_load(&U.open_tries) == tries, "waking opens nothing when nothing was streaming");
    CHECK(device() == dev && alive(dev) == 1, "the same device after the wake");
    CHECK(start_io(dev) == 0 && atomic_load(&U.is_streaming) && stamps(dev),
          "and it streams when asked");
    CHECK(stop_io(dev) == 0, "I/O stops");
}

/* The Mac sleeps while an app streams and Core Audio leaves the I/O running. */
static void test_sleep_streaming(void)
{
    AudioObjectID dev = settled();
    int restarts, starts;

    CHECK(start_io(dev) == 0 && atomic_load(&U.is_streaming), "I/O starts");
    restarts = atomic_load(&g_restarts);
    starts = atomic_load(&U.starts);
    mac_sleeps();
    CHECK(!atomic_load(&U.is_streaming) && !atomic_load(&U.is_open),
          "streaming stopped and the unit was given back before the Mac was let sleep");
    CHECK(device() == dev && alive(dev) == 1, "the device stays listed and alive");
    usleep(100000);
    CHECK(atomic_load(&g_restarts) == restarts, "nothing is asked of Core Audio while asleep");
    mac_wakes();
    CHECK(WAIT(atomic_load(&g_restarts) == restarts + 1, 2.0),
          "after the wake Core Audio is asked to restart I/O");
    CHECK(device() == dev && alive(dev) == 1, "on the same device");
    CHECK(host_restarts_io(dev) == 0, "the restart works");
    CHECK(atomic_load(&U.is_streaming) && atomic_load(&U.starts) == starts + 1 && stamps(dev),
          "and the unit streams again");
    usleep(300000);
    CHECK(atomic_load(&g_restarts) == restarts + 1, "one restart, not a loop");
    CHECK(stop_io(dev) == 0, "I/O stops");
    CHECK(log_has("the Mac is going to sleep") && log_has("the Mac woke"), "the log has both");
}

static void *start_thread(void *arg)
{
    AudioObjectID dev = *(AudioObjectID *)arg;

    return (void *)(intptr_t)start_io(dev);
}

/* Core Audio stops the I/O itself for the sleep, and starts it again at the wake a moment
   before the helper has heard of the wake. */
static void test_sleep_host_stops_io(void)
{
    AudioObjectID dev = settled();
    pthread_t t;
    void *result = NULL;
    double t0;
    OSStatus status;

    CHECK(start_io(dev) == 0, "I/O starts");
    mac_sleeps();
    CHECK(stop_io(dev) == 0, "Core Audio stops I/O for the sleep");
    CHECK(device() == dev && alive(dev) == 1, "the device stays as it is while asleep");

    t0 = now_s();
    pthread_create(&t, NULL, start_thread, &dev);
    usleep(300000);
    CHECK(!atomic_load(&U.is_open), "a start before the wake opens nothing yet");
    mac_wakes();
    pthread_join(t, &result);
    CHECK((OSStatus)(intptr_t)result == 0, "that start waited for the wake and then worked");
    CHECK(now_s() - t0 < 1.0, "within a moment of it (%.2f s)", now_s() - t0);
    CHECK(atomic_load(&U.is_streaming) && stamps(dev), "the unit streams");
    CHECK(stop_io(dev) == 0, "I/O stops");

    /* And if the wake doesn't come: the start is refused after a bounded wait, device kept. */
    mac_sleeps();
    t0 = now_s();
    status = start_io(dev);
    CHECK(status == kAudioHardwareNotRunningError, "a start while asleep is refused (%d)",
          (int)status);
    CHECK(now_s() - t0 > 1.5 && now_s() - t0 < 3.5, "after waiting about 2 s (%.2f)",
          now_s() - t0);
    CHECK(device() == dev && alive(dev) == 1, "and the device stays");
    CHECK(!atomic_load(&U.is_open), "with the unit left alone");
    mac_wakes();
    CHECK(start_io(dev) == 0 && stop_io(dev) == 0, "once awake it starts");
}

/* The unit is unplugged or switched off while the Mac sleeps. */
static void test_wake_unit_gone(void)
{
    AudioObjectID dev = settled();
    int tries;

    /* While streaming. */
    CHECK(start_io(dev) == 0, "I/O starts");
    mac_sleeps();
    unit_goes();
    usleep(100000);
    CHECK(device() == dev && alive(dev) == 1, "nothing changes while asleep");
    tries = atomic_load(&U.open_tries);
    mac_wakes();
    plugin_asked(tries);
    CHECK(start_io(dev) == kAudioHardwareNotRunningError,
          "after the wake the plug-in knows the unit is away: a second start is refused");
    CHECK(stop_io(dev) == 0, "I/O stops");
    CHECK(WAIT(device() == 0, 2.0), "and the device goes from the list");

    /* It comes back during another sleep, and goes again during a third, with nothing streaming. */
    mac_sleeps();
    unit_comes();
    usleep(100000);
    CHECK(device() == 0, "nothing changes while asleep");
    mac_wakes();
    CHECK(WAIT(device() != 0 && alive(device()) == 1, 2.0), "plugged in during sleep: listed "
                                                            "after the wake");
    mac_sleeps();
    unit_goes();
    mac_wakes();
    CHECK(WAIT(device() == 0, 2.0), "unplugged during sleep: gone after the wake");
}

/* The unit is on the bus but its stream dies each time, as Roland describes after a sleep
   during playback. The helper must not open, start and lose it for ever. */
static void test_wont_stream(void)
{
    AudioObjectID dev = settled();
    int starts = atomic_load(&U.starts), restarts = atomic_load(&g_restarts), i, tries;

    atomic_store(&U.dies_ms, 50);
    CHECK(start_io(dev) == 0, "I/O starts");
    for (i = 0; i < 6 && alive(dev) == 1; i++) {
        if (!WAIT(atomic_load(&g_restarts) > restarts || alive(dev) != 1, 3.0))
            break;
        if (alive(dev) != 1)
            break;
        restarts = atomic_load(&g_restarts);
        host_restarts_io(dev);
    }
    CHECK(atomic_load(&g_restarts) == restarts && !atomic_load(&U.is_open),
          "after failing in a row no more restarts are asked for");
    CHECK(device() == dev && alive(dev) == 1, "the device stays while that I/O runs");
    CHECK(atomic_load(&U.starts) - starts == 3, "tried 3 times, not for ever (%d)",
          atomic_load(&U.starts) - starts);
    CHECK(log_has("not trying again until the unit is switched off and on"),
          "the log says what to do");
    stop_io(dev);
    CHECK(WAIT(device() == 0, 2.0), "the device goes from the list");
    tries = atomic_load(&U.opens);
    usleep(3500000);
    CHECK(atomic_load(&U.opens) == tries, "and the unit is left alone");

    /* Switched off and on, it is tried again. */
    atomic_store(&U.dies_ms, 0);
    unit_goes();
    unit_comes();
    CHECK(WAIT(device() != 0, 2.0), "switched off and on: the device returns");
    dev = device();
    CHECK(start_io(dev) == 0 && atomic_load(&U.is_streaming), "and streams");
    usleep(300000);
    CHECK(alive(dev) == 1 && atomic_load(&U.is_streaming), "and keeps streaming");
    CHECK(stop_io(dev) == 0, "I/O stops");

    /* A sleep and wake also ends the giving up. */
    atomic_store(&U.dies_ms, 50);
    restarts = atomic_load(&g_restarts);
    CHECK(start_io(dev) == 0, "I/O starts");
    for (i = 0; i < 6 && alive(dev) == 1; i++) {
        if (!WAIT(atomic_load(&g_restarts) > restarts || alive(dev) != 1, 3.0))
            break;
        if (alive(dev) != 1)
            break;
        restarts = atomic_load(&g_restarts);
        host_restarts_io(dev);
    }
    stop_io(dev);
    CHECK(WAIT(device() == 0, 2.0), "given up on again");
    atomic_store(&U.dies_ms, 0);
    mac_sleeps();
    mac_wakes();
    CHECK(WAIT(device() != 0 && alive(device()) == 1, 2.0), "a sleep and wake: tried again");
}

/* The helper is stuck inside a call to the unit. Core Audio's calls must come back. */
static void test_helper_hangs(void)
{
    AudioObjectID dev = settled();
    double t0;
    OSStatus status;
    int starts;

    /* Stuck while opening the unit. */
    CHECK(WAIT(!atomic_load(&U.is_open), 4.0), "the unit is given back, so a start opens it");
    ua101_link_xpc_set_time_limits(0.3, 0.3, 0.3);
    atomic_store(&U.open_hang_ms, 1500);
    t0 = now_s();
    status = start_io(dev);
    CHECK(status != 0, "a start the helper doesn't answer is refused");
    CHECK(now_s() - t0 < 1.0, "within the time limit, not when the helper comes back (%.2f s)",
          now_s() - t0);
    ua101_link_xpc_set_time_limits(15.0, 5.0, 5.0);
    CHECK(WAIT(device() != 0 && alive(device()) == 1, 6.0),
          "once the helper answers again the device is back");
    dev = device();
    CHECK(!atomic_load(&U.is_streaming), "nothing was left streaming");
    CHECK(start_io(dev) == 0 && stamps(dev) && stop_io(dev) == 0, "and it starts and stops");

    /* Stuck while starting the stream: the start arrives late, with nobody waiting for it. */
    starts = atomic_load(&U.starts);
    ua101_link_xpc_set_time_limits(15.0, 0.3, 5.0);
    atomic_store(&U.start_hang_ms, 1200);
    t0 = now_s();
    status = start_io(dev);
    CHECK(status != 0, "a start that isn't answered in time is refused");
    CHECK(now_s() - t0 < 1.0, "within the time limit (%.2f s)", now_s() - t0);
    ua101_link_xpc_set_time_limits(15.0, 5.0, 5.0);
    CHECK(WAIT(atomic_load(&U.starts) == starts + 1, 3.0), "the helper got to it later");
    CHECK(WAIT(!atomic_load(&U.is_streaming), 3.0), "and stopped again: it streams for nobody");
    CHECK(device() == dev && alive(dev) == 1, "the device stayed");
    CHECK(start_io(dev) == 0 && atomic_load(&U.is_streaming) && stop_io(dev) == 0,
          "and the next start works");
}

/* --- The gap: the device kept for a while after the unit leaves (30 s as installed). --- */

/* The cable is pulled and pushed back in with nothing playing, or with the app paused: the
   same device object stays, refuses to start meanwhile, and starts when the unit is back. */
static void test_gap_back_idle(void)
{
    AudioObjectID dev = settled();
    OSStatus status;
    int tries, starts;

    ua101_plugin_set_gap_seconds(4.0);
    unit_goes_and_plugin_knows();
    CHECK(!atomic_load(&U.is_open), "the helper let go of the unit");
    CHECK(device() == dev && alive(dev) == 1, "unplugged: the same device stays listed, alive");
    CHECK(rate_of(dev) == 48000.0, "and still says what it is");

    tries = atomic_load(&U.open_tries);
    starts = atomic_load(&U.starts);
    status = start_io(dev);
    CHECK(status == kAudioHardwareNotRunningError, "a start meanwhile is refused (%d)",
          (int)status);
    CHECK(atomic_load(&U.open_tries) - tries == 1 && atomic_load(&U.starts) == starts,
          "after one look for the unit, and without asking the helper to stream (%d looks)",
          atomic_load(&U.open_tries) - tries);
    CHECK(device() == dev && alive(dev) == 1, "and the device stays as it is");
    CHECK(stop_io(dev) == kAudioHardwareNotRunningError, "nothing was started");

    unit_comes_and_plugin_knows();
    CHECK(device() == dev && alive(dev) == 1, "plugged back in time: the same object");
    CHECK(start_io(dev) == 0 && atomic_load(&U.is_streaming) && stamps(dev),
          "and pressing play works");
    CHECK(stop_io(dev) == 0, "I/O stops");

    /* Play is pressed the moment the unit is back, before the bus has said so. */
    unit_goes_and_plugin_knows();
    CHECK(device() == dev && alive(dev) == 1, "unplugged again: kept again");
    atomic_store(&U.on_bus, 1);
    CHECK(start_io(dev) == 0 && atomic_load(&U.is_streaming) && stamps(dev),
          "a start finds the unit by itself, before the bus's word");
    dispatch_sync(W.queue, ^{
      W.events.unit_arrived(); /* the bus, late */
    });
    usleep(200000);
    CHECK(device() == dev && alive(dev) == 1 && atomic_load(&U.is_streaming),
          "and the bus's late word changes nothing");
    CHECK(stop_io(dev) == 0, "I/O stops");
    ua101_plugin_set_gap_seconds(SHORT_GAP);
}

/* The cable is pulled and pushed back in while an app plays: the device stays alive, the
   app's I/O runs on in silence, and when the unit is back Core Audio is asked to restart it. */
static void test_gap_back_streaming(void)
{
    AudioObjectID dev = settled();
    OSStatus status;
    int restarts, starts;

    ua101_plugin_set_gap_seconds(4.0);
    CHECK(start_io(dev) == 0 && atomic_load(&U.is_streaming), "I/O starts");
    restarts = atomic_load(&g_restarts);
    starts = atomic_load(&U.starts);
    unit_goes_and_plugin_knows();
    CHECK(!atomic_load(&U.is_streaming) && !atomic_load(&U.is_open),
          "the helper stopped and let go");
    CHECK(device() == dev && alive(dev) == 1, "unplugged while playing: listed and alive");
    CHECK(stamps(dev), "the app's I/O keeps its clock");
    usleep(200000);
    CHECK(atomic_load(&g_restarts) == restarts && alive(dev) == 1,
          "and nothing is asked of Core Audio while the unit is away");
    status = start_io(dev);
    CHECK(status == kAudioHardwareNotRunningError, "a second start meanwhile is refused (%d)",
          (int)status);

    unit_comes();
    CHECK(WAIT(atomic_load(&g_restarts) == restarts + 1, 2.0),
          "plugged back in time: Core Audio is asked to restart I/O");
    CHECK(device() == dev && alive(dev) == 1, "on the same object");
    CHECK(host_restarts_io(dev) == 0, "the restart works");
    CHECK(atomic_load(&U.is_streaming) && atomic_load(&U.starts) == starts + 1 && stamps(dev),
          "and the unit streams again");
    usleep(300000);
    CHECK(atomic_load(&g_restarts) == restarts + 1, "one restart, not a loop");
    CHECK(stop_io(dev) == 0, "I/O stops");
    CHECK(stop_io(dev) == kAudioHardwareNotRunningError,
          "once: the refused start left nothing counted");

    /* The app pauses while the unit is away, and plays when it is back. */
    CHECK(start_io(dev) == 0, "I/O starts");
    unit_goes_and_plugin_knows();
    CHECK(stop_io(dev) == 0, "the app pauses while the unit is away");
    CHECK(device() == dev && alive(dev) == 1, "the device stays after that stop");
    CHECK(start_io(dev) == kAudioHardwareNotRunningError, "play is refused while it is away");
    unit_comes_and_plugin_knows();
    CHECK(device() == dev && start_io(dev) == 0 && atomic_load(&U.is_streaming) && stamps(dev),
          "and works on the same object when it is back");

    /* The unit is back in time, but Core Audio takes longer than the rest of the wait to
       restart the I/O: the wait ended when the unit came back, so its timer does nothing. */
    ua101_plugin_set_gap_seconds(1.5);
    restarts = atomic_load(&g_restarts);
    unit_goes_and_plugin_knows();
    unit_comes();
    CHECK(WAIT(atomic_load(&g_restarts) == restarts + 1, 2.0), "back: a restart is asked for");
    CHECK(start_io(dev) == 0 && stop_io(dev) == 0,
          "and a second start is taken again: the wait is over, restart or not");
    usleep(1800000);
    CHECK(device() == dev && alive(dev) == 1, "a slow restart still finds the device alive");
    CHECK(host_restarts_io(dev) == 0 && atomic_load(&U.is_streaming) && stamps(dev),
          "and works");
    CHECK(stop_io(dev) == 0, "I/O stops");
    ua101_plugin_set_gap_seconds(SHORT_GAP);
}

/* The unit stays away longer than the time. With nothing playing the device goes as it did
   before, only later, and a unit that returns after that is a new object. While an app's I/O
   is still running the device stays for as long as that runs. */
static void test_gap_runs_out(void)
{
    AudioObjectID dev = settled(), again;
    int restarts, starts;
    double t0;

    /* Nothing playing. */
    ua101_plugin_set_gap_seconds(1.0);
    unit_goes_and_plugin_knows();
    t0 = now_s();
    CHECK(device() == dev && alive(dev) == 1, "unplugged: kept");
    CHECK(WAIT(device() == 0, 3.0), "not back in time: the device goes from the list");
    CHECK(now_s() - t0 > 0.7, "when its time is up, not before (%.2f s)", now_s() - t0);
    CHECK(alive(dev) == -1, "the old object is no more");
    unit_comes();
    CHECK(WAIT(device() != 0, 2.0), "plugged in later: the device returns");
    again = device();
    CHECK(again != dev && alive(again) == 1, "as a new object");
    CHECK(start_io(again) == 0 && stamps(again), "which streams");
    ua101_plugin_set_gap_seconds(1.0);

    /* Playing: the time doesn't count while the app's I/O runs. Back long after it, the same
       object carries on. */
    dev = again;
    restarts = atomic_load(&g_restarts);
    starts = atomic_load(&U.starts);
    unit_goes_and_plugin_knows();
    usleep(1800000);
    CHECK(device() == dev && alive(dev) == 1,
          "unplugged while playing, long past the time: still listed and alive");
    CHECK(stamps(dev) && atomic_load(&g_restarts) == restarts,
          "its I/O runs on, and nothing is asked of Core Audio");
    CHECK(start_io(dev) == kAudioHardwareNotRunningError, "a second start is still refused");
    unit_comes();
    CHECK(WAIT(atomic_load(&g_restarts) == restarts + 1, 2.0),
          "plugged back long after the time: Core Audio is asked to restart I/O");
    CHECK(device() == dev && alive(dev) == 1, "on the same object");
    CHECK(host_restarts_io(dev) == 0 && atomic_load(&U.is_streaming) &&
              atomic_load(&U.starts) == starts + 1 && stamps(dev),
          "and the unit streams again");

    /* Playing, the time runs out, and then the app stops with the unit still away: only its
       I/O was keeping the device, so it goes at once. */
    unit_goes_and_plugin_knows();
    usleep(1800000);
    CHECK(device() == dev && alive(dev) == 1, "away past the time while playing: kept");
    CHECK(stop_io(dev) == 0, "the app stops");
    CHECK(WAIT(device() == 0, 0.5), "the device goes from the list at once");
    unit_comes();
    CHECK(WAIT(device() != 0 && device() != dev && alive(device()) == 1, 2.0),
          "plugged in later: a new object");

    /* Playing, the app stops before the time is up: the rest of the time still counts. */
    dev = device();
    ua101_plugin_set_gap_seconds(1.5);
    CHECK(start_io(dev) == 0, "I/O starts");
    unit_goes_and_plugin_knows();
    t0 = now_s();
    CHECK(stop_io(dev) == 0, "the app stops at once");
    usleep(700000);
    CHECK(device() == dev && alive(dev) == 1, "the device is kept for the rest of the time");
    CHECK(WAIT(device() == 0, 3.0), "and goes when it is up");
    CHECK(now_s() - t0 > 1.2, "not before (%.2f s)", now_s() - t0);
    unit_comes();
    CHECK(WAIT(device() != 0 && alive(device()) == 1, 2.0), "plugged in later: it returns");

    /* Each leaving has its own time: out, back, out again. The first wait's timer, still
       pending, must not end the second wait early. */
    dev = device();
    ua101_plugin_set_gap_seconds(1.5);
    unit_goes_and_plugin_knows();
    usleep(900000);
    unit_comes_and_plugin_knows();
    CHECK(device() == dev, "out and back in time: the same object");
    unit_goes_and_plugin_knows();
    t0 = now_s();
    usleep(900000); /* 1.8 s after the first leaving, 0.9 s after the second */
    CHECK(device() == dev && alive(dev) == 1,
          "out again: the first wait's timer doesn't end the second wait");
    CHECK(WAIT(device() == 0, 3.0), "which runs out in its own time");
    CHECK(now_s() - t0 > 1.2, "(%.2f s after the second leaving)", now_s() - t0);
    ua101_plugin_set_gap_seconds(SHORT_GAP);
}

/* Switched off, the rate switch turned, switched on, all within the time: the device isn't
   kept, it is replaced at once by one at the new rate. */
static void test_gap_other_rate(void)
{
    AudioObjectID dev = settled(), other, third;

    /* Nothing playing. */
    ua101_plugin_set_gap_seconds(1.5);
    unit_goes_and_plugin_knows();
    CHECK(device() == dev && alive(dev) == 1, "switched off: kept");
    atomic_store(&U.rate, 96000);
    unit_comes_and_plugin_knows();
    other = device();
    CHECK(other != 0 && other != dev && rate_of(other) == 96000.0,
          "back at another rate: a new device at 96 kHz at once (%.0f)", rate_of(other));
    CHECK(alive(dev) == -1, "and the old object is no more");
    usleep(1800000);
    CHECK(device() == other && alive(other) == 1, "the old wait's timer leaves the new one alone");
    CHECK(start_io(other) == 0 && atomic_load(&U.is_streaming) && stamps(other),
          "and it streams");

    /* Playing. */
    unit_goes_and_plugin_knows();
    CHECK(device() == other && alive(other) == 1, "switched off while playing: kept");
    atomic_store(&U.rate, 48000);
    unit_comes();
    CHECK(WAIT(alive(other) == 0, 1.0),
          "back at another rate while I/O runs: dead at once, so I/O is stopped");
    CHECK(stop_io(other) == 0, "I/O stops");
    CHECK(WAIT(device() != 0 && device() != other, 2.0), "then a new device");
    third = device();
    CHECK(rate_of(third) == 48000.0 && alive(third) == 1, "at 48 kHz");
    CHECK(start_io(third) == 0 && stamps(third) && stop_io(third) == 0, "which streams");
    ua101_plugin_set_gap_seconds(SHORT_GAP);
}

/* The Mac sleeps while the unit is away. "Asleep" says nothing about the unit, so the device
   stays as it is, and what the helper says after the wake decides. (The timer of the real gap
   counts only time awake; here nothing really sleeps, so that part can't be seen.) */
static void test_gap_sleep(void)
{
    AudioObjectID dev = settled();
    OSStatus status;
    int restarts, starts, tries;
    double t0;

    /* The unit comes back during the sleep, nothing playing. */
    ua101_plugin_set_gap_seconds(5.0);
    unit_goes_and_plugin_knows();
    mac_sleeps();
    t0 = now_s();
    status = start_io(dev);
    CHECK(status == kAudioHardwareNotRunningError, "a start while away and asleep is refused (%d)",
          (int)status);
    CHECK(now_s() - t0 > 1.5 && now_s() - t0 < 3.5, "after waiting about 2 s for the wake (%.2f)",
          now_s() - t0);
    CHECK(device() == dev && alive(dev) == 1, "and the device stays");
    unit_comes();
    usleep(100000);
    CHECK(device() == dev && alive(dev) == 1 && !atomic_load(&U.is_open),
          "plugged in during the sleep: nothing changes, nothing is opened");
    tries = atomic_load(&U.open_tries);
    mac_wakes();
    plugin_asked(tries);
    CHECK(device() == dev && alive(dev) == 1, "after the wake: the same object");
    CHECK(start_io(dev) == 0 && atomic_load(&U.is_streaming) && stamps(dev), "and play works");

    /* The same while playing: out, sleep, back in during the sleep, wake. */
    restarts = atomic_load(&g_restarts);
    starts = atomic_load(&U.starts);
    unit_goes_and_plugin_knows();
    mac_sleeps();
    unit_comes();
    usleep(100000);
    CHECK(device() == dev && alive(dev) == 1 && atomic_load(&g_restarts) == restarts,
          "away and asleep while playing: the device stays, nothing is asked of Core Audio");
    mac_wakes();
    CHECK(WAIT(atomic_load(&g_restarts) == restarts + 1, 2.0),
          "after the wake Core Audio is asked to restart I/O");
    CHECK(device() == dev && alive(dev) == 1, "on the same object");
    CHECK(host_restarts_io(dev) == 0 && atomic_load(&U.is_streaming) &&
              atomic_load(&U.starts) == starts + 1 && stamps(dev),
          "and the unit streams again");
    CHECK(stop_io(dev) == 0, "I/O stops");

    /* The unit is still away after the wake: the wait runs out as usual. */
    ua101_plugin_set_gap_seconds(1.5);
    unit_goes_and_plugin_knows();
    mac_sleeps();
    usleep(200000);
    mac_wakes();
    usleep(200000);
    CHECK(device() == dev && alive(dev) == 1, "still away after a sleep and wake: still kept");
    CHECK(WAIT(device() == 0, 3.0), "until its time is up");

    /* Away past the time while playing, and then Core Audio stops the I/O for a sleep. Nothing
       can be learned while asleep, and nobody will tell after the wake, since nothing changed:
       so the wait runs once more from that stop, and then the device goes. */
    dev = settled();
    ua101_plugin_set_gap_seconds(1.0);
    CHECK(start_io(dev) == 0, "I/O starts");
    unit_goes_and_plugin_knows();
    usleep(1300000);
    CHECK(device() == dev && alive(dev) == 1, "away past the time while playing: kept");
    mac_sleeps();
    t0 = now_s();
    CHECK(stop_io(dev) == 0, "Core Audio stops the I/O for the sleep");
    CHECK(device() == dev && alive(dev) == 1, "asleep: the device stays as it is");
    mac_wakes();
    usleep(300000);
    CHECK(device() == dev, "just after the wake it is still there");
    CHECK(WAIT(device() == 0, 3.0), "and it goes once the wait has run again, with nobody asking");
    CHECK(now_s() - t0 > 0.7, "(%.2f s after that stop)", now_s() - t0);
    ua101_plugin_set_gap_seconds(SHORT_GAP);
}

/* The log's size limit (ua101_service_trim_log, src/ua101_service.h). A file
   written as launchd leaves it, then trimmed as the helper does at start. */
static long long file_size(int fd)
{
    struct stat st;

    return fstat(fd, &st) == 0 ? (long long)st.st_size : -1;
}

static int write_lines(int fd, int from, int to)
{
    char line[64];
    int i, n;

    for (i = from; i < to; i++) {
        n = snprintf(line, sizeof(line), "2026-11-13 18:00:00.000 ua101d: line %06d\n", i);
        if (write(fd, line, (size_t)n) != n)
            return 0;
    }
    return 1;
}

static void test_log_trim(void)
{
    char path[64], *text;
    long long before, removed, after;
    int fd, pipe_fds[2];
    ssize_t got;

    printf("-- the log's size limit\n");
    snprintf(path, sizeof(path), "/tmp/ua101-test-logtrim-XXXXXX");
    fd = mkstemp(path);
    CHECK(fd >= 0, "a file for the log");
    if (fd < 0)
        return;

    /* Under the limit: left exactly as it is. 100 lines of 44 bytes. */
    write_lines(fd, 0, 100);
    before = file_size(fd);
    CHECK(ua101_service_trim_log(fd, 10000, 3000) == 0, "a small log isn't touched");
    CHECK(file_size(fd) == before, "its size stays %lld (now %lld)", before, file_size(fd));

    /* Over it: the newest part kept, from a whole line, the oldest gone. */
    write_lines(fd, 100, 1000);
    before = file_size(fd);
    removed = ua101_service_trim_log(fd, 10000, 3000);
    after = file_size(fd);
    CHECK(removed > 0 && removed + after == before, "over the limit: %lld removed of %lld, %lld kept",
          removed, before, after);
    CHECK(after <= 3000 && after > 3000 - 44, "at most the bytes to keep, less under one line (%lld)",
          after);
    text = calloc(1, (size_t)after + 1);
    got = pread(fd, text, (size_t)after, 0);
    CHECK(got == after && strncmp(text, "2026-11-13 18:00:00.000 ua101d: line ", 37) == 0,
          "the kept part starts with a whole line: \"%.40s\"", text);
    CHECK(got == after && strstr(text, "line 000999\n") != NULL && text[after - 1] == '\n',
          "and ends with the newest line");
    CHECK(strstr(text, "line 000100\n") == NULL, "the oldest lines are gone");
    free(text);

    /* Lines written afterwards go to the end, wherever the descriptor pointed before. */
    lseek(fd, 0, SEEK_SET);
    write_lines(fd, 1000, 1001);
    CHECK(file_size(fd) == after + 44, "a new line lands at the end (%lld, was %lld)", file_size(fd),
          after);

    /* At the sizes as built: a 1.5 MB log comes down to 512 KB at most. */
    write_lines(fd, 1001, 1001 + 1500 * 1024 / 44);
    removed = ua101_service_trim_log(fd, UA101_LOG_LIMIT_BYTES, UA101_LOG_KEEP_BYTES);
    CHECK(removed > 0 && file_size(fd) <= UA101_LOG_KEEP_BYTES, "as built: %lld KB kept",
          file_size(fd) / 1024);
    close(fd);
    unlink(path);

    /* Not a regular file (the log run by hand into a terminal or a pipe): left alone. */
    CHECK(pipe(pipe_fds) == 0, "a pipe");
    CHECK(ua101_service_trim_log(pipe_fds[1], 10000, 3000) == 0, "a pipe isn't trimmed");
    close(pipe_fds[0]);
    close(pipe_fds[1]);
}

int main(void)
{
    mach_timebase_info_data_t tb;
    ua101_service_options so;
    xpc_endpoint_t ep;
    FILE *log;
    int fd;

    setvbuf(stdout, NULL, _IOLBF, 0);
    mach_timebase_info(&tb);
    g_tick_s = (double)tb.numer / (double)tb.denom / 1e9;

    test_log_trim();

    snprintf(g_log_path, sizeof(g_log_path), "/tmp/ua101-test-service-XXXXXX");
    fd = mkstemp(g_log_path);
    log = fd >= 0 ? fdopen(fd, "w") : NULL;
    if (log == NULL) {
        printf("FAIL: no log file\n");
        return 1;
    }

    atomic_store(&U.rate, 48000);
    atomic_store(&U.in, 12);
    atomic_store(&U.out, 10);

    memset(&so, 0, sizeof(so));
    so.input_offset = -1;
    so.output_offset = -1;
    so.idle_close_seconds = 1.0; /* 30 s as installed */
    so.watch = 1;
    so.allow_uid = (int)getuid();
    so.log = log;
    ep = ua101_service_start_anonymous(&so);
    if (ep == NULL) {
        printf("FAIL: the service didn't start\n");
        return 1;
    }
    ua101_link_xpc_use_endpoint(ep);
    ua101_plugin_set_gap_seconds(SHORT_GAP);
    D = UA101_Create(NULL, kAudioServerPlugInTypeUUID);
    if (D == NULL || (*D)->Initialize(D, &g_host) != 0) {
        printf("FAIL: the plug-in didn't initialize\n");
        return 1;
    }

    test_switched_on_later();
    test_unplug_idle_open();
    test_unplug_idle_closed();
    test_unplug_streaming();
    test_overs_reach_the_log();
    test_room_reaches_the_log();
    test_power_off_engine_first();
    test_rate_switch();
    test_sleep_idle();
    test_sleep_streaming();
    test_sleep_host_stops_io();
    test_wake_unit_gone();
    test_wont_stream();
    test_helper_hangs();
    test_gap_back_idle();
    test_gap_back_streaming();
    test_gap_runs_out();
    test_gap_other_rate();
    test_gap_sleep();

    ua101_service_shutdown();
    if (failures != 0) {
        char command[128];

        printf("The helper's log:\n");
        snprintf(command, sizeof(command), "cat %s", g_log_path);
        if (system(command) != 0)
            printf("(not readable)\n");
    }
    unlink(g_log_path);
    printf("test_service: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
