/*
 * The audio plug-in, driven the way Core Audio drives it, with no helper and no unit: a fake
 * link (below) answers its hellos from a shared block made here, and a fake host records what
 * it notifies. Covers its objects and properties, the I/O path bit for bit, rate correction and
 * its steering, and the device appearing, going, restarting and changing format.
 */
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "../src/ua101_link.h"
#include "../src/ua101_shared.h"
#include "../src/ua101_steer.h"

void *UA101_Create(CFAllocatorRef allocator, CFUUIDRef requested_type);
void ua101_plugin_wait_for_hello(void);
void ua101_plugin_set_gap_seconds(double seconds);
void ua101_plugin_counts(uint64_t *input_missing, uint64_t *output_clipped);
void ua101_plugin_peaks(float *clipped_peak, uint64_t *cut, float *cut_peak);
int ua101_plugin_correction(double steps[2], uint64_t resets[2], double errors[2]);
void ua101_plugin_steering(double *offset, uint64_t *starts);
void ua101_plugin_reload_offsets(void);

#define RATE_CORRECTION 0x55417263u /* 'UArc', the plug-in's setting */
/* The driver's settings of 2026-11-19 (src/ua101_drvset.h), written out here on purpose. */
#define INPUT_MARGIN 0x5541696Du /* 'UAim', milliseconds */
#define OUTPUT_DELAY 0x55416F64u /* 'UAod', milliseconds */
#define STATS 0x55417374u        /* 'UAst', a dictionary */
#define CHANGE_OFFSETS 3         /* the configuration change a new safety offset asks for */
/* What correction adds at 48 kHz: the live filter's 26 frames and 1 of slack for the steering,
   0.56 ms. It was 128 with the one-shot filter. */
#define DELAY 27

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

/* --- The fake link: the helper as the plug-in sees it. --- */

static struct {
    void (*on_change)(void *);
    void *context;
    int present, start_ok, clock_comes;
    double rate;
    double ppm; /* the unit's clock against the Mac's, from the next start */
    double line_host;  /* the line published at the last start: sample 0's host time */
    double line_ticks; /* and its ticks per frame */
    uint32_t in, out;
    ua101_shared *sh;
    size_t bytes;
    int hellos, starts, stops, last_keep;
    ua101_link_counts last_counts; /* what the last stop carried */
    uint64_t block_id; /* a new one with each block, as the helper gives */
} F;

static void make_block(double rate, uint32_t in, uint32_t out)
{
    long page = sysconf(_SC_PAGESIZE);
    uint64_t frames = ua101_shared_ring_frames(rate);

    /* Each format gets a block of its own, as the helper makes a new one on each open. The old
       one stays mapped: a plug-in still running may hold it. */
    F.rate = rate;
    F.in = in;
    F.out = out;
    F.block_id++;
    F.bytes = ua101_shared_bytes(in * 4, out * 4, frames, (size_t)page);
    F.sh = mmap(NULL, F.bytes, PROT_READ | PROT_WRITE, MAP_ANON | MAP_SHARED, -1, 0);
    if (F.sh == MAP_FAILED || !ua101_shared_init(F.sh, F.bytes, rate, in, out, frames, (size_t)page)) {
        printf("FAIL: no shared block\n");
        exit(1);
    }
}

ua101_link *ua101_link_open(void (*on_change)(void *), void *context)
{
    F.on_change = on_change;
    F.context = context;
    return (ua101_link *)&F;
}

int ua101_link_hello(ua101_link *l, ua101_link_info *info, int keep_mapped, char *why, size_t n)
{
    (void)l;
    F.hellos++;
    F.last_keep = keep_mapped;
    memset(info, 0, sizeof(*info));
    if (!F.present) {
        snprintf(why, n, "the unit isn't connected");
        return UA101_LINK_ERR_ABSENT;
    }
    info->present = 1;
    info->rate_hz = F.rate;
    info->capture_channels = F.in;
    info->playback_channels = F.out;
    info->input_offset = 144;
    info->output_offset = 1024;
    info->shared = F.sh;
    info->shared_bytes = F.bytes;
    info->block_id = F.block_id;
    return UA101_LINK_OK;
}

static uint64_t seconds_to_ticks(double s)
{
    mach_timebase_info_data_t tb;

    mach_timebase_info(&tb);
    return (uint64_t)(s * 1e9 * tb.denom / tb.numer);
}

int ua101_link_start(ua101_link *l, char *why, size_t n)
{
    (void)l;
    F.starts++;
    if (!F.start_ok) {
        snprintf(why, n, "the streaming thread couldn't start");
        return UA101_LINK_ERR_START;
    }
    if (F.clock_comes) {
        /* Sample time 0 was a second ago, at the nominal rate, or F.ppm off it. */
        F.line_ticks = (double)seconds_to_ticks(1.0) / (F.rate * (1.0 + F.ppm * 1e-6));
        /* Off a whole tick, as the helper's lines are. */
        F.line_host = (double)(mach_absolute_time() - seconds_to_ticks(1.0)) + 0.37;
        /* As the helper's engine does at each start: sample time starts again from 0. */
        ua101_frame_ring_reset(&F.sh->capture);
        ua101_frame_ring_reset(&F.sh->playback);
        ua101_shared_new_seed(&F.sh->clock);
        ua101_shared_publish(&F.sh->clock, 0, F.line_host, F.line_ticks);
        atomic_store(&F.sh->state, UA101_STATE_RUNNING);
    } else {
        atomic_store(&F.sh->state, UA101_STATE_STARTING);
    }
    return UA101_LINK_OK;
}

void ua101_link_stop(ua101_link *l, const ua101_link_counts *counts)
{
    (void)l;
    F.last_counts = *counts;
    F.stops++;
    atomic_store(&F.sh->state, UA101_STATE_STOPPED);
}

void ua101_link_close(ua101_link *l)
{
    (void)l;
}

/* The helper says something changed; the plug-in says hello again. */
static void helper_changed(void)
{
    F.on_change(F.context);
    ua101_plugin_wait_for_hello();
    ua101_plugin_wait_for_hello(); /* and its notifications, queued behind it */
}

/* --- The fake host: Core Audio as the plug-in sees it. --- */

#define MAX_NOTES 256
static pthread_mutex_t g_notes_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    AudioObjectID object;
    AudioObjectPropertySelector selector;
} g_notes[MAX_NOTES];
static int g_note_count;
static int g_restarts;
static UInt64 g_restart_action;

static OSStatus host_properties_changed(AudioServerPlugInHostRef host, AudioObjectID object,
                                        UInt32 n, const AudioObjectPropertyAddress *a)
{
    UInt32 i;

    (void)host;
    pthread_mutex_lock(&g_notes_lock);
    for (i = 0; i < n && g_note_count < MAX_NOTES; i++) {
        g_notes[g_note_count].object = object;
        g_notes[g_note_count].selector = a[i].mSelector;
        g_note_count++;
    }
    pthread_mutex_unlock(&g_notes_lock);
    return 0;
}

static int noted(AudioObjectID object, AudioObjectPropertySelector selector)
{
    int i, found = 0;

    pthread_mutex_lock(&g_notes_lock);
    for (i = 0; i < g_note_count; i++)
        if (g_notes[i].object == object && g_notes[i].selector == selector)
            found = 1;
    pthread_mutex_unlock(&g_notes_lock);
    return found;
}

static void forget_notes(void)
{
    pthread_mutex_lock(&g_notes_lock);
    g_note_count = 0;
    pthread_mutex_unlock(&g_notes_lock);
}

/* Core Audio's storage for the plug-in: rate correction's key, counted by itself, and a
   dictionary for the others (the two safety offsets). */
static CFPropertyListRef g_stored;
static int g_writes;
static CFMutableDictionaryRef g_store;
static int g_store_writes, g_store_deletes;

static CFMutableDictionaryRef store(void)
{
    if (g_store == NULL)
        g_store = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
                                            &kCFTypeDictionaryValueCallBacks);
    return g_store;
}

static OSStatus host_copy(AudioServerPlugInHostRef h, CFStringRef k, CFPropertyListRef *v)
{
    (void)h;
    *v = NULL;
    if (CFStringCompare(k, CFSTR("rate correction"), 0) != kCFCompareEqualTo) {
        CFPropertyListRef other;

        pthread_mutex_lock(&g_notes_lock);
        other = CFDictionaryGetValue(store(), k);
        if (other != NULL)
            *v = CFRetain(other);
        pthread_mutex_unlock(&g_notes_lock);
        return other != NULL ? 0 : kAudioHardwareUnknownPropertyError;
    }
    if (g_stored == NULL)
        return kAudioHardwareUnknownPropertyError;
    *v = CFRetain(g_stored);
    return 0;
}

static OSStatus host_write(AudioServerPlugInHostRef h, CFStringRef k, CFPropertyListRef v)
{
    (void)h;
    if (CFStringCompare(k, CFSTR("rate correction"), 0) == kCFCompareEqualTo) {
        if (g_stored != NULL)
            CFRelease(g_stored);
        g_stored = CFRetain(v);
        g_writes++;
    } else {
        pthread_mutex_lock(&g_notes_lock);
        CFDictionarySetValue(store(), k, v);
        g_store_writes++;
        pthread_mutex_unlock(&g_notes_lock);
    }
    return 0;
}

static OSStatus host_delete(AudioServerPlugInHostRef h, CFStringRef k)
{
    (void)h;
    pthread_mutex_lock(&g_notes_lock);
    CFDictionaryRemoveValue(store(), k);
    g_store_deletes++;
    pthread_mutex_unlock(&g_notes_lock);
    return 0;
}

/* A number kept under a key, or -1 if there is none or it isn't a number. */
static double stored_number(CFStringRef key)
{
    CFPropertyListRef v;
    double x = -1.0;

    pthread_mutex_lock(&g_notes_lock);
    v = CFDictionaryGetValue(store(), key);
    if (v != NULL && CFGetTypeID(v) == CFNumberGetTypeID())
        CFNumberGetValue((CFNumberRef)v, kCFNumberDoubleType, &x);
    pthread_mutex_unlock(&g_notes_lock);
    return x;
}

static void store_put(CFStringRef key, CFPropertyListRef v)
{
    pthread_mutex_lock(&g_notes_lock);
    if (v != NULL)
        CFDictionarySetValue(store(), key, v);
    else
        CFDictionaryRemoveValue(store(), key);
    pthread_mutex_unlock(&g_notes_lock);
}

static OSStatus host_request_change(AudioServerPlugInHostRef h, AudioObjectID device,
                                    UInt64 action, void *info)
{
    (void)h;
    (void)device;
    (void)info;
    g_restarts++;
    g_restart_action = action;
    return 0;
}

static AudioServerPlugInHostInterface g_host = {host_properties_changed, host_copy, host_write,
                                                host_delete, host_request_change};

/* --- Helpers for asking the plug-in. --- */

static AudioServerPlugInDriverRef D;

static AudioObjectPropertyAddress addr(AudioObjectPropertySelector s, AudioObjectPropertyScope sc)
{
    AudioObjectPropertyAddress a = {s, sc, kAudioObjectPropertyElementMain};

    return a;
}

/* Asks for a property, checking its size and data agree. Returns the bytes, or -1. */
static int get(AudioObjectID object, AudioObjectPropertySelector s, AudioObjectPropertyScope sc,
               void *out, UInt32 avail)
{
    AudioObjectPropertyAddress a = addr(s, sc);
    UInt32 size = 0, got = 0;

    if (!(*D)->HasProperty(D, object, 0, &a))
        return -1;
    if ((*D)->GetPropertyDataSize(D, object, 0, &a, 0, NULL, &size) != 0)
        return -1;
    if ((*D)->GetPropertyData(D, object, 0, &a, 0, NULL, avail, &got, out) != 0)
        return -1;
    CHECK(got == size || (size > avail && got <= avail),
          "'%.4s' on %u: size %u, but %u came", (char *)&s, object, size, got);
    return (int)got;
}

static UInt32 get_u32(AudioObjectID object, AudioObjectPropertySelector s,
                      AudioObjectPropertyScope sc)
{
    UInt32 v = 0xDEADBEEF;

    CHECK(get(object, s, sc, &v, sizeof(v)) == 4, "'%.4s' on %u", (char *)&s, object);
    return v;
}

static int string_is(AudioObjectID object, AudioObjectPropertySelector s, const char *want)
{
    CFStringRef v = NULL;
    char text[256] = "";

    if (get(object, s, kAudioObjectPropertyScopeGlobal, &v, sizeof(v)) != sizeof(v) || v == NULL)
        return 0;
    CFStringGetCString(v, text, sizeof(text), kCFStringEncodingUTF8);
    CFRelease(v);
    return strcmp(text, want) == 0;
}

static UInt32 devices(AudioObjectID *first)
{
    AudioObjectID ids[4] = {0};
    int n = get(kAudioObjectPlugInObject, kAudioPlugInPropertyDeviceList,
                kAudioObjectPropertyScopeGlobal, ids, sizeof(ids));

    if (first != NULL)
        *first = n > 0 ? ids[0] : kAudioObjectUnknown;
    return n > 0 ? (UInt32)n / sizeof(AudioObjectID) : 0;
}

static AudioObjectID stream_of(AudioObjectID device, AudioObjectPropertyScope scope)
{
    AudioObjectID id = 0;

    CHECK(get(device, kAudioDevicePropertyStreams, scope, &id, sizeof(id)) == sizeof(id),
          "one stream in each direction");
    return id;
}

static int32_t pattern(uint64_t frame, uint32_t channel)
{
    uint32_t v = (uint32_t)(frame * 2654435761u + channel * 40503u) & 0xFFFFFFu;

    return (int32_t)(v << 8);
}

static OSStatus do_io(AudioObjectID device, AudioObjectID stream, UInt32 op, Float64 sample,
                      UInt32 frames, void *buffer)
{
    AudioServerPlugInIOCycleInfo cycle;

    memset(&cycle, 0, sizeof(cycle));
    cycle.mInputTime.mSampleTime = sample;
    cycle.mOutputTime.mSampleTime = sample;
    cycle.mInputTime.mFlags = cycle.mOutputTime.mFlags = kAudioTimeStampSampleTimeValid;
    return (*D)->DoIOOperation(D, device, stream, 1, op, frames, &cycle, buffer, NULL);
}

/* The rate correction setting as the property gives it: 1, 0, or -1 if it can't be read. */
static int correction_is(AudioObjectID dev)
{
    CFPropertyListRef v = NULL;
    int on = -1;

    if (get(dev, RATE_CORRECTION, kAudioObjectPropertyScopeGlobal, &v, sizeof(v)) == sizeof(v) &&
        v != NULL) {
        if (CFGetTypeID(v) == CFBooleanGetTypeID())
            on = CFBooleanGetValue((CFBooleanRef)v) ? 1 : 0;
        CFRelease(v);
    }
    return on;
}

static OSStatus set_correction(AudioObjectID dev, CFPropertyListRef v)
{
    AudioObjectPropertyAddress a = addr(RATE_CORRECTION, kAudioObjectPropertyScopeGlobal);
    OSStatus status = (*D)->SetPropertyData(D, dev, 0, &a, 0, NULL, sizeof(v), &v);

    ua101_plugin_wait_for_hello(); /* what it queued: storage, notes, a restart */
    return status;
}

static UInt32 latency(AudioObjectID dev, AudioObjectPropertyScope scope)
{
    return get_u32(dev, kAudioDevicePropertyLatency, scope);
}

/* A safety offset setting as its property gives it, in milliseconds, or -1. */
static double margin_is(AudioObjectID dev, AudioObjectPropertySelector which)
{
    CFPropertyListRef v = NULL;
    double ms = -1.0;

    if (get(dev, which, kAudioObjectPropertyScopeGlobal, &v, sizeof(v)) == sizeof(v) && v != NULL) {
        if (CFGetTypeID(v) == CFNumberGetTypeID())
            CFNumberGetValue((CFNumberRef)v, kCFNumberDoubleType, &ms);
        CFRelease(v);
    }
    return ms;
}

static OSStatus set_margin(AudioObjectID dev, AudioObjectPropertySelector which, CFPropertyListRef v)
{
    AudioObjectPropertyAddress a = addr(which, kAudioObjectPropertyScopeGlobal);
    OSStatus status = (*D)->SetPropertyData(D, dev, 0, &a, 0, NULL, sizeof(v), &v);

    ua101_plugin_wait_for_hello(); /* what it queued: storage, notes, the change asked for */
    return status;
}

static OSStatus set_margin_ms(AudioObjectID dev, AudioObjectPropertySelector which, double ms)
{
    CFNumberRef n = CFNumberCreate(NULL, kCFNumberDoubleType, &ms);
    OSStatus status = set_margin(dev, which, n);

    CFRelease(n);
    return status;
}

static UInt32 safety(AudioObjectID dev, AudioObjectPropertyScope scope)
{
    return get_u32(dev, kAudioDevicePropertySafetyOffset, scope);
}

/* The statistics property: the caller releases it. NULL if it can't be read. */
static CFDictionaryRef stats_of(AudioObjectID dev)
{
    CFDictionaryRef v = NULL;

    if (get(dev, STATS, kAudioObjectPropertyScopeGlobal, &v, sizeof(v)) != sizeof(v) || v == NULL)
        return NULL;
    if (CFGetTypeID(v) != CFDictionaryGetTypeID()) {
        CFRelease(v);
        return NULL;
    }
    return v;
}

/* One number of it, read afresh; returns 0 if the key isn't there. */
static int stat_of(AudioObjectID dev, const char *key, double *out)
{
    CFDictionaryRef d = stats_of(dev);
    CFStringRef k = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
    CFPropertyListRef v;
    int found = 0;

    *out = -12345.0;
    if (d != NULL && (v = CFDictionaryGetValue(d, k)) != NULL) {
        if (CFGetTypeID(v) == CFNumberGetTypeID())
            found = CFNumberGetValue((CFNumberRef)v, kCFNumberDoubleType, out) ? 1 : 0;
        else if (CFGetTypeID(v) == CFBooleanGetTypeID()) {
            *out = CFBooleanGetValue((CFBooleanRef)v) ? 1.0 : 0.0;
            found = 1;
        }
    }
    if (d != NULL)
        CFRelease(d);
    CFRelease(k);
    return found;
}

static double stat_value(AudioObjectID dev, const char *key)
{
    double x;

    return stat_of(dev, key, &x) ? x : -12345.0;
}

/* --- The tests. --- */

static void test_factory(void)
{
    void *wrong = UA101_Create(NULL, kAudioServerPlugInDriverInterfaceUUID);
    void *iface = NULL;
    CFUUIDRef other = CFUUIDCreateFromString(NULL, CFSTR("00000000-1111-2222-3333-444444444444"));

    CHECK(wrong == NULL, "only the AudioServerPlugIn type is made");
    D = UA101_Create(NULL, kAudioServerPlugInTypeUUID);
    CHECK(D != NULL, "the factory makes the driver");
    if (D == NULL)
        exit(1);
    CHECK((*D)->QueryInterface(D, CFUUIDGetUUIDBytes(kAudioServerPlugInDriverInterfaceUUID),
                               &iface) == S_OK &&
              iface == D,
          "the driver interface is there");
    (*D)->Release(D);
    CHECK((*D)->QueryInterface(D, CFUUIDGetUUIDBytes(IUnknownUUID), &iface) == S_OK,
          "so is IUnknown");
    (*D)->Release(D);
    CHECK((*D)->QueryInterface(D, CFUUIDGetUUIDBytes(other), &iface) == E_NOINTERFACE &&
              iface == NULL,
          "nothing else");
    CFRelease(other);
}

static void test_absent_then_present(void)
{
    AudioObjectID dev;

    F.present = 0;
    CHECK((*D)->Initialize(D, &g_host) == 0, "initializes");
    ua101_plugin_wait_for_hello();
    CHECK(F.hellos == 1, "said hello once at start, %d", F.hellos);
    CHECK(devices(NULL) == 0, "no device while the unit isn't there");

    F.present = 1;
    helper_changed();
    CHECK(devices(&dev) == 1, "the device appears when the helper says so");
    CHECK(noted(kAudioObjectPlugInObject, kAudioPlugInPropertyDeviceList),
          "and Core Audio is told");
}

static void test_properties(void)
{
    AudioObjectID dev, in, out, owned[4], found = 0;
    Float64 rate = 0;
    AudioValueRange ranges[2];
    AudioStreamBasicDescription f;
    AudioStreamRangedDescription r;
    AudioObjectPropertyAddress a;
    CFStringRef uid;
    UInt32 size;
    Boolean settable;

    devices(&dev);
    CHECK(get_u32(kAudioObjectPlugInObject, kAudioObjectPropertyClass,
                  kAudioObjectPropertyScopeGlobal) == kAudioPlugInClassID,
          "the plug-in's class");
    CHECK(get(kAudioObjectPlugInObject, kAudioObjectPropertyOwnedObjects,
              kAudioObjectPropertyScopeGlobal, owned, sizeof(owned)) == 4 &&
              owned[0] == dev,
          "the plug-in owns the device");
    CHECK(get(kAudioObjectPlugInObject, kAudioPlugInPropertyBoxList,
              kAudioObjectPropertyScopeGlobal, owned, sizeof(owned)) == 0,
          "and no boxes");
    CHECK(string_is(kAudioObjectPlugInObject, kAudioObjectPropertyManufacturer,
                    "ua101-driver (unofficial)"),
          "the plug-in is marked unofficial");

    CHECK(get_u32(dev, kAudioObjectPropertyClass, kAudioObjectPropertyScopeGlobal) ==
              kAudioDeviceClassID,
          "the device's class");
    CHECK(get_u32(dev, kAudioObjectPropertyOwner, kAudioObjectPropertyScopeGlobal) ==
              kAudioObjectPlugInObject,
          "the device's owner");
    CHECK(string_is(dev, kAudioObjectPropertyName, "UA-101"), "the device's name");
    CHECK(string_is(dev, kAudioObjectPropertyManufacturer, "EDIROL"), "its maker");
    CHECK(string_is(dev, kAudioDevicePropertyDeviceUID, "io.github.delta90h.ua101:UA-101"),
          "its UID");
    CHECK(get_u32(dev, kAudioDevicePropertyTransportType, kAudioObjectPropertyScopeGlobal) ==
              kAudioDeviceTransportTypeUSB,
          "USB");
    CHECK(get_u32(dev, kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal) == 1,
          "alive");
    CHECK(get_u32(dev, kAudioDevicePropertyDeviceIsRunning, kAudioObjectPropertyScopeGlobal) == 0,
          "not running");
    CHECK(get_u32(dev, kAudioDevicePropertyDeviceCanBeDefaultDevice,
                  kAudioObjectPropertyScopeOutput) == 1,
          "can be the default");
    CHECK(get(dev, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, &rate,
              sizeof(rate)) == 8 &&
              rate == 48000.0,
          "48 kHz, %g", rate);
    CHECK(get(dev, kAudioDevicePropertyAvailableNominalSampleRates,
              kAudioObjectPropertyScopeGlobal, ranges, sizeof(ranges)) == sizeof(AudioValueRange) &&
              ranges[0].mMinimum == 48000.0 && ranges[0].mMaximum == 48000.0,
          "only the unit's rate is offered");
    CHECK(get_u32(dev, kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeInput) == 144,
          "input safety offset from the helper");
    CHECK(get_u32(dev, kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeOutput) == 1024,
          "output safety offset from the helper");
    CHECK(get_u32(dev, kAudioDevicePropertyZeroTimeStampPeriod, kAudioObjectPropertyScopeGlobal) ==
              16384,
          "zero timestamp period at 48 kHz");
    CHECK(get(dev, kAudioDevicePropertyStreams, kAudioObjectPropertyScopeGlobal, owned,
              sizeof(owned)) == 8,
          "two streams in all");
    in = stream_of(dev, kAudioObjectPropertyScopeInput);
    out = stream_of(dev, kAudioObjectPropertyScopeOutput);
    CHECK(in != out && in != dev && out != dev, "distinct objects");

    CHECK(get_u32(in, kAudioStreamPropertyDirection, kAudioObjectPropertyScopeGlobal) == 1 &&
              get_u32(out, kAudioStreamPropertyDirection, kAudioObjectPropertyScopeGlobal) == 0,
          "directions");
    CHECK(get_u32(in, kAudioObjectPropertyOwner, kAudioObjectPropertyScopeGlobal) == dev,
          "the device owns its streams");
    CHECK(get_u32(in, kAudioStreamPropertyStartingChannel, kAudioObjectPropertyScopeGlobal) == 1,
          "channels start at 1");
    CHECK(get(in, kAudioStreamPropertyVirtualFormat, kAudioObjectPropertyScopeGlobal, &f,
              sizeof(f)) == sizeof(f) &&
              f.mChannelsPerFrame == 12 && f.mBitsPerChannel == 32 &&
              f.mFormatFlags == kAudioFormatFlagsNativeFloatPacked && f.mSampleRate == 48000.0 &&
              f.mBytesPerFrame == 48,
          "input: 12 channels of float");
    CHECK(get(out, kAudioStreamPropertyVirtualFormat, kAudioObjectPropertyScopeGlobal, &f,
              sizeof(f)) == sizeof(f) &&
              f.mChannelsPerFrame == 10 && f.mBytesPerFrame == 40,
          "output: 10 channels of float");
    CHECK(get(in, kAudioStreamPropertyPhysicalFormat, kAudioObjectPropertyScopeGlobal, &f,
              sizeof(f)) == sizeof(f) &&
              f.mBitsPerChannel == 32 && f.mBytesPerFrame == 48 &&
              f.mFormatFlags == kAudioFormatFlagsNativeFloatPacked,
          "physical: float too, because I/O buffers come in the physical format");
    CHECK(get(out, kAudioStreamPropertyAvailableVirtualFormats, kAudioObjectPropertyScopeGlobal, &r,
              sizeof(r)) == sizeof(r) &&
              r.mSampleRateRange.mMinimum == 48000.0 && r.mFormat.mChannelsPerFrame == 10,
          "one available format");

    /* Unknown things. */
    a = addr('zzzz', kAudioObjectPropertyScopeGlobal);
    CHECK(!(*D)->HasProperty(D, dev, 0, &a), "no property it doesn't know");
    a = addr(kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal);
    CHECK(!(*D)->HasProperty(D, 9999, 0, &a), "no object it doesn't know");
    CHECK((*D)->GetPropertyDataSize(D, 9999, 0, &a, 0, NULL, &size) ==
              kAudioHardwareBadObjectError,
          "a bad object is an error");

    /* Translating the UID. */
    uid = CFSTR("io.github.delta90h.ua101:UA-101");
    a = addr(kAudioPlugInPropertyTranslateUIDToDevice, kAudioObjectPropertyScopeGlobal);
    size = sizeof(found);
    CHECK((*D)->GetPropertyData(D, kAudioObjectPlugInObject, 0, &a, sizeof(uid), &uid, size, &size,
                                &found) == 0 &&
              found == dev,
          "its UID finds the device");
    uid = CFSTR("something else");
    CHECK((*D)->GetPropertyData(D, kAudioObjectPlugInObject, 0, &a, sizeof(uid), &uid, size, &size,
                                &found) == 0 &&
              found == kAudioObjectUnknown,
          "another UID finds nothing");

    /* What can be set. */
    a = addr(kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal);
    CHECK((*D)->IsPropertySettable(D, dev, 0, &a, &settable) == 0 && !settable,
          "the rate is the unit's switch");
    rate = 48000.0;
    CHECK((*D)->SetPropertyData(D, dev, 0, &a, 0, NULL, sizeof(rate), &rate) == 0,
          "asking for the rate it has is fine");
    rate = 44100.0;
    CHECK((*D)->SetPropertyData(D, dev, 0, &a, 0, NULL, sizeof(rate), &rate) ==
              kAudioDeviceUnsupportedFormatError,
          "another rate is refused");
    a = addr(kAudioStreamPropertyIsActive, kAudioObjectPropertyScopeGlobal);
    CHECK((*D)->IsPropertySettable(D, in, 0, &a, &settable) == 0 && settable,
          "a stream's use can be set");
    {
        UInt32 off = 0, on = 1;

        forget_notes();
        CHECK((*D)->SetPropertyData(D, in, 0, &a, 0, NULL, sizeof(off), &off) == 0 &&
                  get_u32(in, kAudioStreamPropertyIsActive, kAudioObjectPropertyScopeGlobal) == 0,
              "input set unused");
        ua101_plugin_wait_for_hello();
        CHECK(noted(in, kAudioStreamPropertyIsActive), "and told");
        (*D)->SetPropertyData(D, in, 0, &a, 0, NULL, sizeof(on), &on);
    }
    a = addr(kAudioStreamPropertyVirtualFormat, kAudioObjectPropertyScopeGlobal);
    get(out, kAudioStreamPropertyVirtualFormat, kAudioObjectPropertyScopeGlobal, &f, sizeof(f));
    CHECK((*D)->SetPropertyData(D, out, 0, &a, 0, NULL, sizeof(f), &f) == 0,
          "setting the format it has is fine");
    f.mSampleRate = 96000.0;
    CHECK((*D)->SetPropertyData(D, out, 0, &a, 0, NULL, sizeof(f), &f) ==
              kAudioDeviceUnsupportedFormatError,
          "another is refused");
}

/* The setting, with I/O stopped. Leaves it off, for the bit-exact tests that follow. */
static void test_correction_setting(void)
{
    AudioObjectID dev;
    AudioServerPlugInCustomPropertyInfo info[8];
    AudioObjectPropertyAddress a = addr(RATE_CORRECTION, kAudioObjectPropertyScopeGlobal);
    Boolean settable = 0;
    CFNumberRef number;
    int one = 1, writes;

    devices(&dev);
    CHECK(get(dev, kAudioObjectPropertyCustomPropertyInfoList, kAudioObjectPropertyScopeGlobal, info,
              sizeof(info)) == 4 * sizeof(info[0]) &&
              info[0].mSelector == RATE_CORRECTION &&
              info[0].mPropertyDataType == kAudioServerPlugInCustomPropertyDataTypeCFPropertyList &&
              info[0].mQualifierDataType == kAudioServerPlugInCustomPropertyDataTypeNone,
          "rate correction is declared as the first of the device's four custom properties");
    CHECK(correction_is(dev) == 1, "on unless it was turned off");
    CHECK((*D)->IsPropertySettable(D, dev, 0, &a, &settable) == 0 && settable, "and settable");
    CHECK(latency(dev, kAudioObjectPropertyScopeInput) == DELAY &&
              latency(dev, kAudioObjectPropertyScopeOutput) == DELAY,
          "the filter's delay is reported both ways");

    forget_notes();
    writes = g_writes;
    CHECK(set_correction(dev, kCFBooleanFalse) == 0, "turned off");
    ua101_plugin_wait_for_hello();
    CHECK(correction_is(dev) == 0, "reads off");
    CHECK(g_writes == writes + 1 && g_stored == kCFBooleanFalse, "kept in Core Audio's storage");
    CHECK(latency(dev, kAudioObjectPropertyScopeInput) == 0 &&
              latency(dev, kAudioObjectPropertyScopeOutput) == 0,
          "no latency added");
    CHECK(noted(dev, RATE_CORRECTION) && noted(dev, kAudioDevicePropertyLatency),
          "and Core Audio is told of both");
    CHECK(g_restarts == 0, "not running, so no restart is asked for");

    writes = g_writes;
    CHECK(set_correction(dev, kCFBooleanFalse) == 0 && g_writes == writes,
          "setting it as it is changes nothing");
    number = CFNumberCreate(NULL, kCFNumberIntType, &one);
    CHECK(set_correction(dev, number) == 0 && correction_is(dev) == 1, "a number works too");
    CFRelease(number);
    CHECK(set_correction(dev, CFSTR("yes")) == kAudioHardwareIllegalOperationError &&
              correction_is(dev) == 1,
          "other kinds are refused");
    CHECK(set_correction(dev, kCFBooleanFalse) == 0 && correction_is(dev) == 0 &&
              g_stored == kCFBooleanFalse,
          "off again");
}

static void test_io(void)
{
    AudioObjectID dev, in, out;
    Boolean will = 0, in_place = 0;
    Float64 sample = -1;
    UInt64 host = 0, seed = 0;
    ua101_clock_line line;
    enum { FRAMES = 512, IN_CH = 12, OUT_CH = 10 };
    static int32_t frames[8192 * IN_CH];
    static float buf[FRAMES * IN_CH];
    static int32_t played[FRAMES * OUT_CH];
    uint64_t f, missing, clipped;
    int bad = 0;
    UInt32 i, c;
    uint64_t now;

    devices(&dev);
    in = stream_of(dev, kAudioObjectPropertyScopeInput);
    out = stream_of(dev, kAudioObjectPropertyScopeOutput);

    CHECK((*D)->WillDoIOOperation(D, dev, 1, kAudioServerPlugInIOOperationReadInput, &will,
                                  &in_place) == 0 &&
              will && in_place,
          "reads input in place");
    CHECK((*D)->WillDoIOOperation(D, dev, 1, kAudioServerPlugInIOOperationWriteMix, &will,
                                  &in_place) == 0 &&
              will,
          "writes the mix");
    CHECK((*D)->WillDoIOOperation(D, dev, 1, kAudioServerPlugInIOOperationMixOutput, &will,
                                  &in_place) == 0 &&
              !will,
          "and nothing else");

    /* Starting fails cleanly. */
    F.start_ok = 0;
    CHECK((*D)->StartIO(D, dev, 1) == kAudioHardwareNotRunningError, "a refused start fails");
    CHECK(get_u32(dev, kAudioDevicePropertyDeviceIsRunning, kAudioObjectPropertyScopeGlobal) == 0,
          "and isn't running");
    F.start_ok = 1;
    F.clock_comes = 0;
    {
        uint64_t t0 = mach_absolute_time();
        double waited;

        CHECK((*D)->StartIO(D, dev, 1) == kAudioHardwareNotRunningError,
              "a start whose clock never comes fails");
        waited = (double)(mach_absolute_time() - t0) / (double)seconds_to_ticks(1.0);
        CHECK(waited > 2.9 && waited < 4.0, "after about 3 s, %.2f s", waited);
        CHECK(F.stops == 1, "and asks the helper to stop");
    }
    F.clock_comes = 1;

    /* Two clients, one stream. */
    CHECK((*D)->StartIO(D, dev, 1) == 0, "starts");
    CHECK((*D)->StartIO(D, dev, 2) == 0, "a second client starts");
    CHECK(F.starts == 3, "the helper was asked to stream once more, %d", F.starts);
    CHECK(get_u32(dev, kAudioDevicePropertyDeviceIsRunning, kAudioObjectPropertyScopeGlobal) == 1,
          "running");

    /* Zero timestamps: whole periods, on the helper's line. */
    now = mach_absolute_time();
    CHECK((*D)->GetZeroTimeStamp(D, dev, 1, &sample, &host, &seed) == 0, "a zero timestamp");
    ua101_shared_read(&F.sh->clock, &line);
    CHECK(sample >= 16384 && fmod(sample, 16384.0) == 0.0, "a whole number of periods, %g", sample);
    CHECK(host <= now + seconds_to_ticks(0.001) && seed == line.seed, "at or before now, same seed");
    CHECK(fabs((double)host - ua101_line_host(&line, sample)) < 1.0, "on the line");

    /* Capture, bit for bit: what the helper wrote is what an app gets, as x / 2^31. */
    for (f = 0; f < 8192; f++)
        for (c = 0; c < IN_CH; c++)
            frames[f * IN_CH + c] = pattern(f, c);
    ua101_frame_ring_write(&F.sh->capture, 0, frames, 8192);
    CHECK(do_io(dev, in, kAudioServerPlugInIOOperationReadInput, 1000.0, FRAMES, buf) == 0,
          "reads input");
    for (i = 0; i < FRAMES; i++)
        for (c = 0; c < IN_CH; c++)
            bad += buf[i * IN_CH + c] != (float)pattern(1000 + i, c) / 2147483648.0f;
    CHECK(bad == 0, "%d input samples differ", bad);

    /* A helper gone wrong rewrites the ring's size and place in the shared block. The cycle
       works from its own checked copy, so it reads the same frames and stays in its buffers. */
    {
        uint64_t capacity = F.sh->capture.capacity;
        uint32_t frame_bytes = F.sh->capture.frame_bytes;
        int64_t offset = F.sh->capture.data_offset;

        F.sh->capture.capacity = UINT64_C(1) << 40;
        F.sh->capture.frame_bytes = 4096;
        F.sh->capture.data_offset += (int64_t)F.bytes * 16;
        memset(buf, 0, sizeof(buf));
        do_io(dev, in, kAudioServerPlugInIOOperationReadInput, 1000.0, FRAMES, buf);
        F.sh->capture.capacity = capacity;
        F.sh->capture.frame_bytes = frame_bytes;
        F.sh->capture.data_offset = offset;
        for (i = 0; i < FRAMES; i++)
            for (c = 0; c < IN_CH; c++)
                bad += buf[i * IN_CH + c] != (float)pattern(1000 + i, c) / 2147483648.0f;
        CHECK(bad == 0, "%d input samples differ with the ring's fields rewritten", bad);
    }

    /* Before sample time 0, and past what has come: silence, and the missing frames counted. */
    bad = 0;
    do_io(dev, in, kAudioServerPlugInIOOperationReadInput, -100.0, FRAMES, buf);
    for (i = 0; i < FRAMES; i++)
        for (c = 0; c < IN_CH; c++)
            bad += buf[i * IN_CH + c] != (i < 100 ? 0.0f : (float)pattern(i - 100, c) / 2147483648.0f);
    CHECK(bad == 0, "%d samples wrong around sample time 0", bad);
    ua101_plugin_counts(&missing, &clipped);
    CHECK(missing == 0, "nothing missing yet, %llu", missing);
    do_io(dev, in, kAudioServerPlugInIOOperationReadInput, 8192.0 - 12, FRAMES, buf);
    ua101_plugin_counts(&missing, &clipped);
    CHECK(missing == FRAMES - 12, "frames not yet come are counted, %llu", missing);
    CHECK(buf[13 * IN_CH] == 0.0f, "and are silence");

    /* Playback, bit for bit, and clipping counted. */
    for (i = 0; i < FRAMES; i++)
        for (c = 0; c < OUT_CH; c++)
            buf[i * OUT_CH + c] = (float)((int32_t)((i * 977u + c * 31u) % 16777216u) - 8388608) /
                                  8388608.0f;
    buf[5] = 1.5f;
    CHECK(do_io(dev, out, kAudioServerPlugInIOOperationWriteMix, 20000.0, FRAMES, buf) == 0,
          "writes the mix");
    ua101_frame_ring_read(&F.sh->playback, 20000, played, FRAMES, NULL);
    bad = 0;
    for (i = 0; i < FRAMES; i++)
        for (c = 0; c < OUT_CH; c++) {
            int32_t want = i == 0 && c == 5
                               ? (int32_t)(8388607u << 8)
                               : (int32_t)((uint32_t)((int32_t)((i * 977u + c * 31u) % 16777216u) -
                                                      8388608)
                                           << 8);

            bad += played[i * OUT_CH + c] != want;
        }
    CHECK(bad == 0, "%d playback samples differ", bad);
    ua101_plugin_counts(&missing, &clipped);
    CHECK(clipped == 1, "one clip counted, %llu", clipped);
    {
        float peak = 0, cut_peak = 1;
        uint64_t cut = 1;

        ua101_plugin_peaks(&peak, &cut, &cut_peak);
        CHECK(peak == 1.5f, "and how far over it was: 1.5, got %g", (double)peak);
        CHECK(cut == 0 && cut_peak == 0.0f, "nothing is cut by a correction that isn't on");
    }
}

static void test_restart(void)
{
    AudioObjectID dev;
    int hellos;

    devices(&dev);
    /* The helper's stream failed and it opened the unit again: Core Audio is asked to restart. */
    atomic_store(&F.sh->state, UA101_STATE_FAILED);
    make_block(48000.0, 12, 10);
    helper_changed();
    CHECK(F.last_keep == 1, "the block in use stays mapped while running");
    CHECK(g_restarts == 1 && g_restart_action == 1, "a restart was asked for, %d", g_restarts);

    /* As Core Audio does it: stop each client, perform, start again. */
    hellos = F.hellos;
    CHECK((*D)->StopIO(D, dev, 1) == 0 && F.stops == 1, "first client stops; still streaming");
    CHECK((*D)->StopIO(D, dev, 2) == 0 && F.stops == 2, "the last stops the helper");
    CHECK(F.hellos == hellos + 1 && F.last_keep == 0, "and maps the helper's new block");
    CHECK((*D)->PerformDeviceConfigurationChange(D, dev, 1, NULL) == 0, "performs the change");
    CHECK((*D)->StartIO(D, dev, 1) == 0, "starts again");
    {
        Float64 s;
        UInt64 h, seed;
        ua101_clock_line line;

        ua101_shared_read(&F.sh->clock, &line);
        CHECK((*D)->GetZeroTimeStamp(D, dev, 1, &s, &h, &seed) == 0 && seed == line.seed,
              "on the new block's clock");
    }
    CHECK(devices(NULL) == 1, "the same device throughout");
    {
        AudioObjectID now;

        devices(&now);
        CHECK(now == dev, "with the same object");
    }
}

/* The helper crashed while I/O ran. Its old block still says it is running, and nothing more is
   written to it; launchd starts a new helper at the plug-in's next message, with a new block. */
static void test_helper_crash(void)
{
    AudioObjectID dev;
    int restarts = g_restarts, starts;
    uint64_t capacity;

    devices(&dev);
    CHECK(atomic_load(&F.sh->state) == UA101_STATE_RUNNING, "the old block says running");
    make_block(48000.0, 12, 10);
    helper_changed();
    CHECK(F.last_keep == 1, "the block in use stays mapped while running");
    CHECK(g_restarts == restarts + 1 && g_restart_action == 1,
          "a restart is asked for when the helper has a new block, %d", g_restarts - restarts);
    CHECK((*D)->StopIO(D, dev, 1) == 0, "Core Audio stops it");
    CHECK((*D)->PerformDeviceConfigurationChange(D, dev, 1, NULL) == 0, "performs the change");

    /* A block that doesn't fit the format the helper gave is never used. */
    starts = F.starts;
    capacity = F.sh->capture.capacity;
    F.sh->capture.capacity = capacity * 2;
    CHECK((*D)->StartIO(D, dev, 1) != 0 && F.starts == starts, "a block that doesn't fit is refused");
    F.sh->capture.capacity = capacity;
    CHECK((*D)->StartIO(D, dev, 1) == 0, "starts again on the new block");
    {
        Float64 s;
        UInt64 h, seed;
        ua101_clock_line line;

        ua101_shared_read(&F.sh->clock, &line);
        CHECK((*D)->GetZeroTimeStamp(D, dev, 1, &s, &h, &seed) == 0 && seed == line.seed,
              "on the new block's clock");
    }
    /* Another hello with nothing changed asks for nothing. */
    restarts = g_restarts;
    helper_changed();
    CHECK(g_restarts == restarts, "no restart when the block is the same");
}

/*
 * Threads, as in Core Audio: the I/O cycle runs on a thread of its own while hellos from the
 * helper, property calls and a change of setting come in on others. Here for the thread
 * sanitizer (`make tsan`), which reports any field two threads touch without a lock or an
 * atomic; under the usual build it only shows that nothing gets stuck or goes wrong.
 */
static atomic_int g_io_stop;
static AudioObjectID g_io_dev, g_io_in, g_io_out;
static _Atomic uint64_t g_io_cycles, g_io_failed;

static void *io_thread(void *arg)
{
    enum { FRAMES = 256 };
    static float buf[FRAMES * 12];
    int64_t at = 0;

    (void)arg;
    while (!atomic_load(&g_io_stop)) {
        Float64 sample;
        UInt64 host, seed;

        if ((*D)->GetZeroTimeStamp(D, g_io_dev, 1, &sample, &host, &seed) != 0 ||
            do_io(g_io_dev, g_io_in, kAudioServerPlugInIOOperationReadInput, (Float64)at, FRAMES,
                  buf) != 0)
            atomic_fetch_add(&g_io_failed, 1);
        memset(buf, 0, sizeof(buf));
        if (do_io(g_io_dev, g_io_out, kAudioServerPlugInIOOperationWriteMix, (Float64)(at + 1024),
                  FRAMES, buf) != 0)
            atomic_fetch_add(&g_io_failed, 1);
        at += FRAMES;
        atomic_fetch_add(&g_io_cycles, 1);
    }
    return NULL;
}

/* One spell of I/O on its thread, with everything else going on beside it. */
static void run_threads(AudioObjectID dev, const char *what)
{
    pthread_t io;
    uint64_t cycles0 = atomic_load(&g_io_cycles), missing, clipped;
    double steps[2], errors[2], offset;
    uint64_t resets[2], starts;
    int k;

    atomic_store(&g_io_stop, 0);
    if (pthread_create(&io, NULL, io_thread, NULL) != 0) {
        CHECK(0, "no I/O thread");
        return;
    }
    for (k = 0; k < 200; k++) {
        helper_changed(); /* a hello while I/O runs, the block unchanged */
        latency(dev, kAudioObjectPropertyScopeInput);
        get_u32(dev, kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeOutput);
        get_u32(dev, kAudioDevicePropertyDeviceIsRunning, kAudioObjectPropertyScopeGlobal);
        devices(NULL);
        correction_is(dev);
        margin_is(dev, INPUT_MARGIN);
        margin_is(dev, OUTPUT_DELAY);
        stat_value(dev, "capture spare");
        ua101_plugin_counts(&missing, &clipped);
        ua101_plugin_correction(steps, resets, errors);
        ua101_plugin_steering(&offset, &starts);
        /* A second client comes and goes. */
        (*D)->StartIO(D, dev, 2);
        (*D)->StopIO(D, dev, 2);
        usleep(500);
    }
    atomic_store(&g_io_stop, 1);
    pthread_join(io, NULL);
    CHECK(atomic_load(&g_io_cycles) > cycles0 + 10 && atomic_load(&g_io_failed) == 0,
          "%s: the I/O thread ran beside the rest, %llu cycles, %llu failed", what,
          atomic_load(&g_io_cycles) - cycles0, atomic_load(&g_io_failed));
}

static void test_threads(void)
{
    AudioObjectID dev;
    int restarts = g_restarts;

    devices(&dev);
    g_io_dev = dev;
    g_io_in = stream_of(dev, kAudioObjectPropertyScopeInput);
    g_io_out = stream_of(dev, kAudioObjectPropertyScopeOutput);
    run_threads(dev, "bit-exact");

    /* The same with rate correction, switched on as an app would while I/O runs. */
    CHECK(set_correction(dev, kCFBooleanTrue) == 0, "correction on");
    ua101_plugin_wait_for_hello();
    CHECK((*D)->StopIO(D, dev, 1) == 0 &&
              (*D)->PerformDeviceConfigurationChange(D, dev, 2, NULL) == 0 &&
              (*D)->StartIO(D, dev, 1) == 0,
          "restarted with correction");
    run_threads(dev, "corrected");

    /* Back to how it was: running, bit-exact. */
    CHECK(set_correction(dev, kCFBooleanFalse) == 0, "correction off");
    ua101_plugin_wait_for_hello();
    CHECK((*D)->StopIO(D, dev, 1) == 0 &&
              (*D)->PerformDeviceConfigurationChange(D, dev, 2, NULL) == 0 &&
              (*D)->StartIO(D, dev, 1) == 0,
          "restarted without it");
    CHECK(g_restarts == restarts + 2, "only the two setting changes asked for a restart, %d",
          g_restarts - restarts);
}

/* Two tones: 1 kHz, and 15 kHz near the top of the band. Other channels carry silence. */
static double tone(uint32_t channel, double seconds)
{
    static const double hz[2] = {1000.0, 15000.0};

    return channel < 2 ? 0.5 * sin(2.0 * M_PI * hz[channel] * seconds) : 0.0;
}

static int32_t to_24(double x)
{
    return (int32_t)((uint32_t)(int32_t)lrint(x * 8388608.0) << 8);
}

/* The helper's capture ring, filled up to `upto` from `*written`, as the unit recorded the
   tones: unit frame k at k / unit_rate seconds after the unit's sample 0. */
static void record(uint64_t *written, uint64_t upto, double unit_rate, uint32_t ch)
{
    static int32_t frames[1024 * 16];

    while (*written < upto) {
        uint64_t n = upto - *written < 1024 ? upto - *written : 1024, i;
        uint32_t c;

        for (i = 0; i < n; i++)
            for (c = 0; c < ch; c++)
                frames[i * ch + c] = to_24(tone(c, (double)(*written + i) / unit_rate));
        ua101_frame_ring_write(&F.sh->capture, *written, frames, n);
        *written += n;
    }
}

/*
 * Rate correction, switched on while running, on a unit whose clock is 119 ppm slow. Apps get
 * whole periods at exactly 48 kHz; tones recorded on the unit's clock come out at exactly
 * their frequency on ours, and tones played on ours land on the unit's frames at theirs, both
 * DELAY frames late. A half-frame move in the helper's line is followed gently, over a few
 * seconds, without starting over; a big jump, or a gap in sample time, starts the resampler
 * again in place. Leaves client 1 running, corrected.
 */
static void test_correction(void)
{
    enum { FRAMES = 512, IN_CH = 12, OUT_CH = 10, CYCLES = 375 /* 4 s */, SETTLED = 24000 };
    static float buf[FRAMES * IN_CH];
    static int32_t played[1024 * OUT_CH];
    const double unit_rate = 48000.0 * (1.0 - 119e-6);
    double tpf = (double)seconds_to_ticks(1.0) / 48000.0, origin, worst = 0, worst_play = 0;
    double steps[2], errors[2], offset = 0, peak = 0;
    uint64_t resets[2], written = 0, checked, missing = 0, starts = 0;
    AudioObjectID dev, in, out;
    Float64 sample = 0;
    UInt64 host = 0, seed = 0;
    ua101_clock_line line;
    int restarts = g_restarts, others = 0, k;
    int64_t v = 0;
    UInt32 i, c;

    devices(&dev);
    in = stream_of(dev, kAudioObjectPropertyScopeInput);
    out = stream_of(dev, kAudioObjectPropertyScopeOutput);

    /* On while running: kept, and a restart asked for; the running I/O stays as it began. */
    forget_notes();
    CHECK(set_correction(dev, kCFBooleanTrue) == 0 && correction_is(dev) == 1, "turned on while running");
    CHECK(g_restarts == restarts + 1 && g_restart_action == 2, "a restart is asked for, %d",
          g_restarts - restarts);
    CHECK(g_stored == kCFBooleanTrue, "kept");
    CHECK(latency(dev, kAudioObjectPropertyScopeInput) == 0 &&
              !ua101_plugin_correction(steps, resets, errors),
          "the running I/O isn't corrected until it starts again");
    CHECK((*D)->StopIO(D, dev, 1) == 0, "Core Audio stops it");
    ua101_plugin_wait_for_hello();
    CHECK(latency(dev, kAudioObjectPropertyScopeInput) == DELAY &&
              noted(dev, kAudioDevicePropertyLatency),
          "then the latency is the filter's, and Core Audio is told");
    CHECK((*D)->PerformDeviceConfigurationChange(D, dev, 2, NULL) == 0, "performs the change");
    F.ppm = -119.0;
    CHECK((*D)->StartIO(D, dev, 1) == 0, "starts again");
    CHECK(ua101_plugin_correction(steps, resets, errors), "corrected");
    origin = F.line_host;

    /* Zero timestamps: whole periods of our own timeline, at exactly 48 kHz from sample 0. */
    CHECK((*D)->GetZeroTimeStamp(D, dev, 1, &sample, &host, &seed) == 0, "a zero timestamp");
    ua101_shared_read(&F.sh->clock, &line);
    CHECK(sample >= 16384 && fmod(sample, 16384.0) == 0.0, "a whole number of periods, %g", sample);
    CHECK(fabs((double)host - (origin + sample * tpf)) <= 1.0,
          "at exactly the nominal rate, %.1f ticks off", (double)host - (origin + sample * tpf));
    CHECK(host <= mach_absolute_time() && seed == line.seed, "at or before now, same seed");

    /* Capture. */
    for (k = 0; k < CYCLES; k++, v += FRAMES) {
        record(&written, (uint64_t)ceil((double)(v + FRAMES) * tpf / F.line_ticks) + 1, unit_rate,
               IN_CH);
        do_io(dev, in, kAudioServerPlugInIOOperationReadInput, (Float64)v, FRAMES, buf);
        if (v < SETTLED)
            continue;
        for (i = 0; i < FRAMES; i++) {
            double t = (double)(v + i) / 48000.0 - DELAY / unit_rate;

            for (c = 0; c < IN_CH; c++) {
                double e = fabs(buf[i * IN_CH + c] - tone(c, t));

                if (c >= 2)
                    others += buf[i * IN_CH + c] != 0.0f;
                else if (e > worst)
                    worst = e;
            }
        }
    }
    ua101_plugin_correction(steps, resets, errors);
    printf("  capture: worst error %.1f dB, step %+.3f ppm, line error %.2g frames\n",
           20.0 * log10(worst), (steps[0] - 1.0) * 1e6, errors[0]);
    CHECK(worst < 2e-6, "recorded tones come out true, worst %.1f dB", 20.0 * log10(worst));
    CHECK(others == 0, "silent channels stay silent, %d not", others);
    CHECK(fabs(steps[0] - unit_rate / 48000.0) < 1e-9, "the step is the unit's rate, %+.4f ppm",
          (steps[0] - 1.0) * 1e6);
    CHECK(resets[0] == 1 && errors[0] < 1e-4, "started once, then stayed on the line: %llu, %.2g",
          resets[0], errors[0]);

    /* Playback, from sample time 0. */
    checked = (uint64_t)ceil(SETTLED * unit_rate / 48000.0);
    for (k = 0, v = 0; k < CYCLES; k++, v += FRAMES) {
        uint64_t end = (uint64_t)floor((double)(v + FRAMES - 2) * unit_rate / 48000.0);

        for (i = 0; i < FRAMES; i++)
            for (c = 0; c < OUT_CH; c++)
                buf[i * OUT_CH + c] = (float)tone(c, (double)(v + i) / 48000.0);
        do_io(dev, out, kAudioServerPlugInIOOperationWriteMix, (Float64)v, FRAMES, buf);
        /* Unit frames whose moment comes before this cycle's end have all been written. */
        while (checked < end) {
            uint64_t n = end - checked < 1024 ? end - checked : 1024, j;
            ua101_frame_ring_miss miss;

            ua101_frame_ring_read(&F.sh->playback, checked, played, n, &miss);
            missing += miss.not_yet + miss.overwritten;
            for (j = 0; j < n; j++) {
                double t = ((double)(checked + j) * 48000.0 / unit_rate - DELAY) / 48000.0;

                for (c = 0; c < OUT_CH; c++) {
                    double got = (double)played[j * OUT_CH + c] / 2147483648.0;

                    if (c >= 2)
                        others += played[j * OUT_CH + c] != 0;
                    else if (fabs(got - tone(c, t)) > worst_play)
                        worst_play = fabs(got - tone(c, t));
                }
            }
            checked += n;
        }
    }
    ua101_plugin_correction(steps, resets, errors);
    printf("  playback: worst error %.1f dB, step %+.3f ppm, line error %.2g frames\n",
           20.0 * log10(worst_play), (steps[1] - 1.0) * 1e6, errors[1]);
    CHECK(missing == 0, "every unit frame due was written, %llu not", missing);
    CHECK(worst_play < 2e-6, "played tones land true, worst %.1f dB", 20.0 * log10(worst_play));
    CHECK(others == 0, "silent channels stay silent, %d not", others);
    CHECK(fabs(steps[1] - 48000.0 / unit_rate) < 1e-9, "the step is ours over the unit's, %+.4f ppm",
          (steps[1] - 1.0) * 1e6);
    CHECK(resets[1] == 1 && errors[1] < 1e-4, "started once, then stayed on the line: %llu, %.2g",
          resets[1], errors[1]);

    /* The helper's line moves half a frame later. Followed in one cycle, as it used to be, that
       was a step 977 ppm off for that cycle. Now the smoothed line takes it up over a few
       seconds: no new start, the step never far off, and back where it was in 4 s. */
    v = (int64_t)CYCLES * FRAMES;
    ua101_shared_publish(&F.sh->clock, 0, F.line_host + 0.5 * F.line_ticks, F.line_ticks);
    for (k = 0; k < CYCLES; k++, v += FRAMES) {
        record(&written, (uint64_t)ceil((double)(v + FRAMES) * tpf / F.line_ticks) + 1, unit_rate,
               IN_CH);
        do_io(dev, in, kAudioServerPlugInIOOperationReadInput, (Float64)v, FRAMES, buf);
        ua101_plugin_correction(steps, resets, errors);
        if (fabs(steps[0] - unit_rate / 48000.0) > peak)
            peak = fabs(steps[0] - unit_rate / 48000.0);
    }
    ua101_plugin_steering(&offset, &starts);
    printf("  half a frame: the step moved by %.1f ppm at most; after 4 s %.4f frames are left "
           "and the step is %+.3f ppm off\n",
           peak * 1e6, offset, (steps[0] - unit_rate / 48000.0) * 1e6);
    CHECK(resets[0] == 1 && starts == 1, "half a frame is followed, not started over: %llu, %llu",
          resets[0], starts);
    CHECK(peak > 5e-6 && peak < 80e-6, "gently: %.1f ppm at most", peak * 1e6);
    CHECK(errors[0] < 0.05, "the resampler stays on the smoothed line, %.3f frames", errors[0]);
    CHECK(fabs(offset) < 0.02 && fabs(steps[0] - unit_rate / 48000.0) < 0.5e-6,
          "and it has settled: %.4f frames, %+.3f ppm", offset,
          (steps[0] - unit_rate / 48000.0) * 1e6);

    /* A jump of 100 frames, then a gap in sample time: each starts it again in place. */
    ua101_shared_publish(&F.sh->clock, 0, F.line_host + 100.5 * F.line_ticks, F.line_ticks);
    do_io(dev, in, kAudioServerPlugInIOOperationReadInput, (Float64)v, FRAMES, buf);
    v += 2 * FRAMES;
    do_io(dev, in, kAudioServerPlugInIOOperationReadInput, (Float64)v, FRAMES, buf);
    do_io(dev, out, kAudioServerPlugInIOOperationWriteMix, 1e6, FRAMES, buf);
    ua101_plugin_correction(steps, resets, errors);
    ua101_plugin_steering(&offset, &starts);
    CHECK(resets[0] == 3 && resets[1] == 2, "started again for each, %llu and %llu", resets[0],
          resets[1]);
    CHECK(starts == 2, "the smoothed line started over once, for the jump: %llu", starts);

    /* A new seed, as when the helper's sample time starts again from 0: both start again. */
    ua101_shared_new_seed(&F.sh->clock);
    ua101_shared_publish(&F.sh->clock, 0, F.line_host + 100.5 * F.line_ticks, F.line_ticks);
    v += FRAMES;
    do_io(dev, in, kAudioServerPlugInIOOperationReadInput, (Float64)v, FRAMES, buf);
    do_io(dev, out, kAudioServerPlugInIOOperationWriteMix, 1e6 + FRAMES, FRAMES, buf);
    ua101_plugin_correction(steps, resets, errors);
    ua101_plugin_steering(&offset, &starts);
    CHECK(resets[0] == 4 && resets[1] == 3 && starts == 3,
          "a new seed starts all three again: %llu, %llu, %llu", resets[0], resets[1], starts);
}

/* --- Steering: the smoothed copy of the helper's line. --- */

/* The loop by itself, at three I/O buffer sizes and a long way into a run. */
static void test_steer_module(void)
{
    static const int sizes[] = {64, 512, 4096};
    static const double after[3] = {1.0, 2.0, 3.5};
    const double ratio = 1.0 - 119e-6, omega = 2.0 * M_PI * UA101_STEER_HZ;
    /* 2^40 frames is 265 days at 48 kHz: the arithmetic must not care. */
    const int64_t t0 = (int64_t)1 << 40, u0 = ((int64_t)1 << 41) + 12345;
    int k, j;

    for (k = 0; k < 3; k++) {
        const int size = sizes[k];
        int64_t now = t0;
        double left[3] = {0, 0, 0}, when[3] = {0, 0, 0}, worst_line = 0, exact, shift = 0.0;
        ua101_steer s, copy;
        int n, cycles_12s = 12 * 48000 / size, took;

        ua101_steer_init(&s, 48000.0, 32.0);
        CHECK(ua101_steer_update(&s, now, u0, 0.25, ratio) == 1 && s.starts == 1,
              "the first reading is taken as it stands");
        copy = s;
        CHECK(ua101_steer_update(&s, now, u0, 0.75, ratio) == 0 &&
                  ua101_steer_update(&s, now - 5, u0, 0.75, ratio) == 0 &&
                  memcmp(&s, &copy, sizeof(s)) == 0,
              "a reading that isn't later changes nothing");
        CHECK(ua101_steer_update(&s, now + size, u0, 0.25 + size * ratio, 1.5) == 0 &&
                  memcmp(&s, &copy, sizeof(s)) == 0,
              "nor does a slope that isn't a clock's");
        /* 12 s on a line that doesn't move, then half a frame, then 4 s more. */
        for (n = 1; n <= cycles_12s + 4 * 48000 / size; n++) {
            double seconds;

            now = t0 + (int64_t)n * size;
            if (n == cycles_12s + 1)
                shift = 0.5;
            took = ua101_steer_update(&s, now, u0, 0.25 + shift + (double)(now - t0) * ratio, ratio);
            if (took)
                break;
            seconds = (double)(n - cycles_12s - 1) * size / 48000.0;
            if (n <= cycles_12s) {
                /* On the line, and exact: one cycle on is `size` frames times the slope. */
                exact = ua101_steer_frame(&s, now + size, u0) - ua101_steer_frame(&s, now, u0) -
                        size * ratio;
                if (fabs(s.offset) > worst_line)
                    worst_line = fabs(s.offset);
                if (fabs(exact) > worst_line)
                    worst_line = fabs(exact);
            } else {
                for (j = 0; j < 3; j++)
                    if (fabs(seconds - after[j]) <= 0.5 * size / 48000.0) {
                        left[j] = s.offset;
                        when[j] = seconds;
                    }
            }
        }
        CHECK(n > cycles_12s + 4 * 48000 / size && s.starts == 1, "it never started over");
        CHECK(worst_line < 1e-9, "on a still line it stays put, to %.2g frames", worst_line);
        printf("  steering alone, %4d-frame cycles: half a frame leaves %+.4f, %+.4f and %+.4f "
               "frames after 1, 2 and 3.5 s\n",
               size, left[0], left[1], left[2]);
        /* A critically damped loop: (1 - wt) e^(-wt) of the move, whatever the buffer size
           (the longest cycles here are 85 ms, and still within 0.01 frames of it). */
        for (j = 0; j < 3; j++) {
            double want = 0.5 * (1.0 - omega * when[j]) * exp(-omega * when[j]);

            CHECK(fabs(left[j] - want) < 0.015, "as the loop's design says after %.2f s: %+.4f, not %+.4f",
                  when[j], left[j], want);
        }
        CHECK(fabs(left[2]) < 0.05, "settled within 3.5 s: %+.4f frames", left[2]);
        CHECK(fabs(ua101_steer_time(&s, s.frame + 1000, s.time) -
                   (1000.0 - s.part) / s.ratio) < 1e-9,
              "and maps back from unit frames to ours");

        /* More than `lost` frames off, or told to, it takes the helper's line as it stands. */
        now += size;
        CHECK(ua101_steer_update(&s, now, u0, 33.5 + (double)(now - t0) * ratio, ratio) == 1 &&
                  s.starts == 2 && s.offset == 0.0,
              "a jump starts it over");
        ua101_steer_restart(&s);
        now += size;
        CHECK(ua101_steer_update(&s, now, u0, 33.5 + (double)(now - t0) * ratio, ratio) == 1 &&
                  s.starts == 3,
              "and so does a restart");
    }
}

/* The same every run, so the limits below can be tight. */
static uint32_t g_wobble = 20261110u;

static double wobble_noise(void)
{
    double sum = 0;
    int i;

    for (i = 0; i < 12; i++) {
        g_wobble = g_wobble * 1664525u + 1013904223u;
        sum += (double)(g_wobble >> 8) / 16777216.0;
    }
    return sum - 6.0; /* close to a normal distribution, variance 1 */
}

/* How far a 15 kHz tone, n samples at 48 kHz, is from one whose level and phase only drift
   evenly over the stretch: the worst sample. That leaves out slow wander and keeps the wobble. */
static double tone_wobble(const double *y, size_t n)
{
    const double w = 2.0 * M_PI * 15000.0 / 48000.0;
    double a[4][5] = {{0}}, c[4], worst = 0;
    size_t k;
    int i, j, m;

    for (k = 0; k < n; k++) {
        double tau = 2.0 * (double)k / (double)n - 1.0, si = sin(w * (double)k), co = cos(w * (double)k);
        double p[4] = {si, co, tau * si, tau * co};

        for (i = 0; i < 4; i++) {
            for (j = 0; j < 4; j++)
                a[i][j] += p[i] * p[j];
            a[i][4] += p[i] * y[k];
        }
    }
    for (i = 0; i < 4; i++)
        for (m = i + 1; m < 4; m++) {
            double f = a[m][i] / a[i][i];

            for (j = i; j < 5; j++)
                a[m][j] -= f * a[i][j];
        }
    for (i = 3; i >= 0; i--) {
        c[i] = a[i][4];
        for (j = i + 1; j < 4; j++)
            c[i] -= a[i][j] * c[j];
        c[i] /= a[i][i];
    }
    for (k = 0; k < n; k++) {
        double tau = 2.0 * (double)k / (double)n - 1.0, si = sin(w * (double)k), co = cos(w * (double)k);
        double e = fabs(y[k] - (c[0] + c[2] * tau) * si - (c[1] + c[3] * tau) * co);

        if (e > worst)
            worst = e;
    }
    return worst;
}

/*
 * Steering on a helper's line that wobbles as the unit's was measured to
 *: from one cycle to the next it moves 63
 * to 68 ns rms, and over a run it wanders about 4.8 us rms. The moves here are the ripple the
 * helper's clock loop leaves of the unit's frame counts, 5.8 times a second at this rate, plus
 * a random walk; the wander is two slow swings. Followed exactly, as the plug-in used to, that
 * is a step wobbling by about 6 ppm rms. With the smoothed line: about 1 ppm, which is the slow
 * wander's own; no starts; capture and playback locked to each other (capture takes each move
 * of the smoothed line one cycle after playback, whose sample time is ahead); a 15 kHz tone wobbling
 * less than it did; settled within 3 s of a start whose first slope was 25 ppm out, and within
 * 3.5 s of a half-frame move. Cycles as Core Audio runs them: input, then output further on.
 * Leaves client 1 running, corrected.
 */
static void test_steering(void)
{
    enum { FRAMES = 512, IN_CH = 12, OUT_CH = 10, BLOCK = 93 /* cycles: 1 s */ };
    enum { START = 281 /* 3 s */, SETTLED = 469 /* 5 s */, WOBBLE = 3750 /* 40 s */, MOVE = 375 /* 4 s */ };
    static float buf[FRAMES * IN_CH], zeros[FRAMES * OUT_CH];
    static double got[BLOCK * FRAMES], followed[BLOCK * FRAMES];
    const double unit_rate = 48000.0 * (1.0 - 119e-6), truth = unit_rate / 48000.0;
    const double frame_ns = 1e9 / unit_rate;
    double steps[2], errors[2], offset, walk = 0, dev = 0, last_dev = 0;
    double n = 0, cap_sum = 0, cap_sq = 0, play_sum = 0, play_sq = 0, follow_sum = 0, follow_sq = 0;
    double dev_sum = 0, dev_sq = 0, lock = 0, offset_worst = 0, start_worst = 0, error_worst = 0;
    double wobble_got = 0, wobble_followed = 0, move_peak = 0, start_offset = 0, last_play = 0;
    uint64_t resets[2], starts, written = 0, missing0, missing, clipped;
    AudioObjectID dev_id, in, out;
    /* Well past the frames the tests before this one left in the capture ring. */
    int64_t x = (int64_t)1 << 20;
    size_t filled = 0;
    int k;

    devices(&dev_id);
    in = stream_of(dev_id, kAudioObjectPropertyScopeInput);
    out = stream_of(dev_id, kAudioObjectPropertyScopeOutput);
    CHECK((*D)->StopIO(D, dev_id, 1) == 0, "stops");
    ua101_plugin_wait_for_hello();
    F.ppm = -119.0;
    CHECK((*D)->StartIO(D, dev_id, 1) == 0, "starts again");
    ua101_plugin_counts(&missing0, &clipped);
    written = (uint64_t)floor((double)x * truth) - 4096;

    for (k = 0; k < WOBBLE + MOVE; k++, x += FRAMES) {
        double t = (double)k * FRAMES / 48000.0, cap, play, follow;
        uint64_t anchor = (uint64_t)floor((double)x * truth);
        /* The helper's first idea of the slope is out, and comes right within seconds. */
        double slope_off = 25e-6 * exp(-t / 1.0);
        UInt32 i;

        last_dev = dev;
        if (k < WOBBLE) {
            double ripple = 0;
            int h;

            for (h = 1; h <= 6; h++)
                ripple += 0.0079 / (h * h) * sin(2.0 * M_PI * h * 5.808 * t);
            walk += 0.00173 * wobble_noise();
            dev = ripple + walk + 0.149 * sin(2.0 * M_PI * t / 18.0 + 1.0) +
                  0.285 * sin(2.0 * M_PI * t / 45.0 + 2.0);
        } else if (k == WOBBLE) {
            dev += 0.5; /* and then it holds still, half a frame further on */
        }
        /* The line as the helper writes it: a recent frame, when it was, and the slope. Saying
           the unit is `dev` frames further on is saying each frame came that much sooner. */
        ua101_shared_publish(&F.sh->clock, anchor,
                             F.line_host + ((double)anchor - dev) * F.line_ticks,
                             F.line_ticks * (1.0 - slope_off));
        record(&written, anchor + 1, unit_rate, IN_CH);
        do_io(dev_id, in, kAudioServerPlugInIOOperationReadInput, (Float64)(x - FRAMES - 144),
              FRAMES, buf);
        do_io(dev_id, out, kAudioServerPlugInIOOperationWriteMix, (Float64)(x + 1024), FRAMES,
              zeros);
        ua101_plugin_correction(steps, resets, errors);
        ua101_plugin_steering(&offset, &starts);
        cap = (steps[0] - truth) * 1e6;
        play = (steps[1] - 1.0 / truth) * 1e6;
        /* Landing on the helper's own line at the end of each cycle, as it used to. */
        follow = (dev - last_dev) / FRAMES * 1e6;

        if (k < START) {
            if (fabs(offset) > start_offset)
                start_offset = fabs(offset);
        } else if (k < SETTLED) {
            if (fabs(cap) > start_worst)
                start_worst = fabs(cap);
        } else if (k < WOBBLE) {
            n++;
            cap_sum += cap;
            cap_sq += cap * cap;
            play_sum += play;
            play_sq += play * play;
            follow_sum += follow;
            follow_sq += follow * follow;
            dev_sum += dev;
            dev_sq += dev * dev;
            if (fabs(cap + last_play) > lock)
                lock = fabs(cap + last_play);
            if (fabs(offset) > offset_worst)
                offset_worst = fabs(offset);
            /* The 15 kHz channel, a second at a time, beside what following the helper's line
               exactly would have recorded. */
            for (i = 0; i < FRAMES; i++) {
                double at = (double)(x - FRAMES - 144 + i) / 48000.0 +
                            (last_dev + (dev - last_dev) * (i + 1) / FRAMES - DELAY) / unit_rate;

                got[filled] = buf[i * IN_CH + 1];
                followed[filled++] = tone(1, at);
            }
            if (filled == (size_t)BLOCK * FRAMES) {
                double a = tone_wobble(got, filled), b = tone_wobble(followed, filled);

                if (a > wobble_got)
                    wobble_got = a;
                if (b > wobble_followed)
                    wobble_followed = b;
                filled = 0;
            }
        } else if (fabs(cap) > move_peak) {
            move_peak = fabs(cap);
        }
        last_play = play;
        /* The largest since the start, settling included. */
        error_worst = errors[0] > errors[1] ? errors[0] : errors[1];
    }
    ua101_plugin_counts(&missing, &clipped);
    {
        double cap_rms = sqrt(cap_sq / n - (cap_sum / n) * (cap_sum / n));
        double play_rms = sqrt(play_sq / n - (play_sum / n) * (play_sum / n));
        double follow_rms = sqrt(follow_sq / n - (follow_sum / n) * (follow_sum / n));
        double wander = sqrt(dev_sq / n - (dev_sum / n) * (dev_sum / n));

        printf("  the line fed: moves of %.0f ns rms a cycle, wander %.1f us rms; followed exactly "
               "the step would wobble %.2f ppm rms\n",
               follow_rms * 1e-6 * FRAMES * frame_ns, wander * frame_ns / 1e3, follow_rms);
        printf("  smoothed: capture %.2f ppm rms, playback %.2f; the two apart by %.4f ppm at most; "
               "%.3f frames behind the helper's line at most\n",
               cap_rms, play_rms, lock, offset_worst);
        printf("  15 kHz tone, wobble within a second: %.1f dB, against %.1f dB following exactly\n",
               20.0 * log10(wobble_got), 20.0 * log10(wobble_followed));
        printf("  start: %.3f frames off at most in the first 3 s, then within %.2f ppm; half a "
               "frame: %.1f ppm at most, %.4f frames and %+.3f ppm left after 4 s\n",
               start_offset, start_worst, move_peak, offset, (steps[0] - truth) * 1e6);
        CHECK(follow_rms * 1e-6 * FRAMES * frame_ns > 60.0 &&
                  follow_rms * 1e-6 * FRAMES * frame_ns < 70.0 && wander * frame_ns > 3500.0 &&
                  wander * frame_ns < 6000.0,
              "the line wobbles as the unit's was measured to");
        CHECK(follow_rms > 5.5 && follow_rms < 7.0, "which followed exactly is about 6 ppm: %.2f",
              follow_rms);
        CHECK(cap_rms < 1.5 && play_rms < 1.5, "smoothed, the step wobbles far less: %.2f and %.2f ppm",
              cap_rms, play_rms);
        CHECK(lock < 0.05, "capture and playback stay locked to each other: %.4f ppm apart", lock);
        CHECK(error_worst < 0.05, "and both sit on the smoothed line: %.4f frames", error_worst);
        CHECK(offset_worst < 0.25, "which stays close to the helper's: %.3f frames", offset_worst);
        /* What is left is the slow wander within each second, which both follow. */
        CHECK(wobble_got < wobble_followed && wobble_got < 0.0126,
              "a 15 kHz tone wobbles less than it did: %.1f dB against %.1f",
              20.0 * log10(wobble_got), 20.0 * log10(wobble_followed));
        CHECK(start_offset < 0.5 && start_worst < 4.0,
              "settled within 3 s of the start: %.3f frames, then %.2f ppm", start_offset,
              start_worst);
        CHECK(move_peak < 40.0 && fabs(offset) < 0.05 && fabs(steps[0] - truth) < 0.5e-6,
              "half a frame is taken up gently and has settled: %.1f ppm, %.4f frames, %+.3f ppm",
              move_peak, offset, (steps[0] - truth) * 1e6);
    }
    CHECK(resets[0] == 1 && resets[1] == 1 && starts == 1,
          "nothing started over: %llu, %llu, %llu", resets[0], resets[1], starts);
    CHECK(missing == missing0, "no capture frame was asked for before it was there: %llu",
          missing - missing0);
}

/*
 * What playback loses at full scale, with correction on, is told apart: samples the apps sent
 * at or over full scale are counted as clipped, with the largest; samples of the correction's
 * own result that passed over full scale between the apps' samples are counted as cut, with
 * the largest. Each run starts from nothing, and the stop carries the run's numbers.
 */
static void test_playback_overs(void)
{
    enum { FRAMES = 512, OUT_CH = 10, CYCLES = 12 };
    static float buf[FRAMES * OUT_CH];
    const float full = 1.0f - 1.0f / 8388608.0f;
    uint64_t missing, clipped0, clipped, cut = 1, cut_before;
    float peak = 1, cut_peak = 1;
    AudioObjectID dev, out;
    double steps[2], errors[2];
    uint64_t resets[2];
    int64_t x = (int64_t)1 << 22;
    int stops, k;
    UInt32 i;

    devices(&dev);
    out = stream_of(dev, kAudioObjectPropertyScopeOutput);
    CHECK((*D)->StopIO(D, dev, 1) == 0, "stops");
    ua101_plugin_wait_for_hello();
    F.ppm = -119.0;
    CHECK((*D)->StartIO(D, dev, 1) == 0, "starts again");
    CHECK(ua101_plugin_correction(steps, resets, errors), "corrected");
    ua101_plugin_counts(&missing, &clipped0);
    ua101_plugin_peaks(&peak, &cut, &cut_peak);
    CHECK(peak == 0.0f && cut == 0 && cut_peak == 0.0f, "a run starts with nothing over: %g, %llu, %g",
          (double)peak, cut, (double)cut_peak);

    /* A 12 kHz tone on one channel, every sample at full scale and none over, the wave's
       peaks between them 3.01 dB over. Silence on the rest. */
    for (i = 0; i < FRAMES; i++)
        buf[i * OUT_CH + 2] = (i & 2) ? -full : full;
    for (k = 0; k < CYCLES; k++, x += FRAMES)
        do_io(dev, out, kAudioServerPlugInIOOperationWriteMix, (Float64)x, FRAMES, buf);
    ua101_plugin_counts(&missing, &clipped);
    ua101_plugin_peaks(&peak, &cut, &cut_peak);
    printf("  12 cycles of a full-scale 12 kHz tone, corrected: %llu clipped, %llu cut by the "
           "correction, the largest %+.2f dB over\n",
           clipped - clipped0, cut, 20.0 * log10((double)cut_peak));
    CHECK(clipped == clipped0 && peak == 0.0f,
          "samples within full scale aren't counted as the apps': %llu, %g", clipped - clipped0,
          (double)peak);
    CHECK(cut > CYCLES * FRAMES / 4 && cut <= CYCLES * FRAMES,
          "the correction's own cuts are counted: %llu", cut);
    CHECK(cut_peak > 1.40f && cut_peak < 1.42f, "the largest is the wave's peak, 1.414: %g",
          (double)cut_peak);

    /* Silence, then one sample twice full scale from an app. */
    memset(buf, 0, sizeof(buf));
    for (k = 0; k < 2; k++, x += FRAMES)
        do_io(dev, out, kAudioServerPlugInIOOperationWriteMix, (Float64)x, FRAMES, buf);
    ua101_plugin_peaks(&peak, &cut_before, &cut_peak);
    buf[100 * OUT_CH + 7] = -2.0f;
    buf[101 * OUT_CH + 7] = 1.25f;
    do_io(dev, out, kAudioServerPlugInIOOperationWriteMix, (Float64)x, FRAMES, buf);
    x += FRAMES;
    memset(buf, 0, sizeof(buf));
    do_io(dev, out, kAudioServerPlugInIOOperationWriteMix, (Float64)x, FRAMES, buf);
    ua101_plugin_counts(&missing, &clipped);
    ua101_plugin_peaks(&peak, &cut, &cut_peak);
    CHECK(clipped == clipped0 + 2 && peak == 2.0f,
          "the apps' own are counted with the largest: %llu, %g", clipped - clipped0, (double)peak);
    CHECK(cut >= cut_before, "and the correction's count only grows: %llu after %llu", cut,
          cut_before);

    /* The stop carries the run's numbers. */
    stops = F.stops;
    memset(&F.last_counts, 0, sizeof(F.last_counts));
    CHECK((*D)->StopIO(D, dev, 1) == 0 && F.stops == stops + 1, "stops, and the helper is told");
    CHECK(F.last_counts.clipped == 2 && F.last_counts.clipped_peak == 2.0,
          "the stop says what the apps sent over: %llu, %g", F.last_counts.clipped,
          F.last_counts.clipped_peak);
    CHECK(F.last_counts.corrected == 1 && F.last_counts.cut == cut &&
              F.last_counts.cut_peak == (double)cut_peak,
          "and what the correction cut: %d, %llu, %g", F.last_counts.corrected,
          F.last_counts.cut, F.last_counts.cut_peak);
    ua101_plugin_wait_for_hello();
    CHECK((*D)->StartIO(D, dev, 1) == 0, "starts again");
    ua101_plugin_peaks(&peak, &cut, &cut_peak);
    CHECK(peak == 0.0f && cut == 0 && cut_peak == 0.0f, "the next run starts with nothing over");
}

static void test_unit_goes(void)
{
    AudioObjectID dev, again;

    devices(&dev);
    forget_notes();
    F.present = 0;
    helper_changed();
    CHECK(devices(NULL) == 1, "while running, the device stays until I/O stops");
    CHECK(get_u32(dev, kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal) == 0,
          "but isn't alive");
    CHECK(noted(dev, kAudioDevicePropertyDeviceIsAlive), "and Core Audio is told");
    CHECK((*D)->StopIO(D, dev, 1) == 0, "Core Audio stops it");
    ua101_plugin_wait_for_hello();
    CHECK(devices(NULL) == 0, "then it's gone");

    F.present = 1;
    helper_changed();
    CHECK(devices(&again) == 1 && again != dev, "it comes back as new objects, %u then %u", dev,
          again);
    CHECK(string_is(again, kAudioDevicePropertyDeviceUID, "io.github.delta90h.ua101:UA-101"),
          "with the same UID");
}

static void test_format_change(void)
{
    AudioObjectID dev, again;
    AudioStreamBasicDescription f;
    Float64 rate = 0;

    devices(&dev);
    make_block(96000.0, 10, 10);
    helper_changed();
    CHECK(devices(&again) == 1 && again != dev, "a new format is a new device object");
    CHECK(get(again, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, &rate,
              sizeof(rate)) == 8 &&
              rate == 96000.0,
          "at 96 kHz");
    CHECK(get_u32(again, kAudioDevicePropertyZeroTimeStampPeriod,
                  kAudioObjectPropertyScopeGlobal) == 32768,
          "with a longer zero timestamp period");
    CHECK(get(stream_of(again, kAudioObjectPropertyScopeInput), kAudioStreamPropertyVirtualFormat,
              kAudioObjectPropertyScopeGlobal, &f, sizeof(f)) == sizeof(f) &&
              f.mChannelsPerFrame == 10,
          "and its channels");
}

/*
 * The helper gives the unit back after a while without streaming, without telling anyone, and
 * opens it again with a new block at the next hello. So a first start says hello before it
 * asks for streaming.
 */
static void test_idle_close(void)
{
    AudioObjectID dev, again;
    Float64 s, rate = 0;
    UInt64 h, seed;
    ua101_clock_line line;
    int hellos = F.hellos, starts;

    CHECK(devices(&dev) == 1, "a device, idle");
    make_block(96000.0, 10, 10); /* closed and opened again: the same format, a new block */
    CHECK((*D)->StartIO(D, dev, 1) == 0, "a start after the helper gave the unit back works");
    CHECK(F.hellos == hellos + 1 && F.last_keep == 0, "it said hello first and mapped the block");
    ua101_shared_read(&F.sh->clock, &line);
    CHECK((*D)->GetZeroTimeStamp(D, dev, 1, &s, &h, &seed) == 0 && seed == line.seed,
          "and runs on the new block's clock");
    hellos = F.hellos;
    CHECK((*D)->StartIO(D, dev, 2) == 0 && F.hellos == hellos, "a second client says no hello");
    CHECK((*D)->StopIO(D, dev, 2) == 0 && (*D)->StopIO(D, dev, 1) == 0, "stops");
    CHECK(devices(&again) == 1 && again == dev, "the same device object throughout");

    /* The unit was switched off while the helper had let go of it. */
    F.present = 0;
    starts = F.starts;
    CHECK((*D)->StartIO(D, dev, 1) != 0 && F.starts == starts,
          "a start with the unit gone is refused, and the helper isn't asked to stream");
    CHECK(devices(NULL) == 0, "and the device is withdrawn");
    F.present = 1;
    helper_changed();
    CHECK(devices(&dev) == 1, "it comes back");

    /* The rate switch was turned while the helper had let go of it. */
    make_block(48000.0, 12, 10);
    starts = F.starts;
    CHECK((*D)->StartIO(D, dev, 1) != 0 && F.starts == starts,
          "a start after the format changed is refused");
    CHECK(devices(&again) == 1 && again != dev, "and the device comes back as a new object");
    CHECK(get(again, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, &rate,
              sizeof(rate)) == 8 &&
              rate == 48000.0,
          "at the new rate");
    CHECK((*D)->StartIO(D, again, 1) == 0 && (*D)->StopIO(D, again, 1) == 0,
          "which starts and stops");
}

/* A channel's name (kAudioObjectPropertyElementName on the device), or "" when it has none. */
static const char *channel_name(AudioObjectID dev, AudioObjectPropertyScope scope, UInt32 channel)
{
    static char text[64];
    AudioObjectPropertyAddress a = {kAudioObjectPropertyElementName, scope, channel};
    CFStringRef v = NULL;
    UInt32 size = 0, got = 0;

    text[0] = 0;
    if (!(*D)->HasProperty(D, dev, 0, &a))
        return text;
    if ((*D)->GetPropertyDataSize(D, dev, 0, &a, 0, NULL, &size) != 0 || size != sizeof(v) ||
        (*D)->GetPropertyData(D, dev, 0, &a, 0, NULL, sizeof(v), &got, &v) != 0 ||
        got != sizeof(v) || v == NULL) {
        CHECK(0, "channel %u's name: it says it has one, but it can't be read", channel);
        return text;
    }
    CFStringGetCString(v, text, sizeof(text), kCFStringEncodingUTF8);
    CFRelease(v);
    return text;
}

/* The box's printed labels, from the jack map measured 2026-11-14. */
static void check_channel_names(AudioObjectID dev, UInt32 ins, UInt32 outs)
{
    static const char *const in_names[] = {
        "INPUT 1", "INPUT 2", "INPUT 3", "INPUT 4", "INPUT 5", "INPUT 6",
        "INPUT 7", "INPUT 8", "OPTICAL IN L", "OPTICAL IN R", "MONITOR OUT L", "MONITOR OUT R"};
    static const char *const out_names[] = {
        "OUTPUT 1", "OUTPUT 2", "OUTPUT 3", "OUTPUT 4", "OUTPUT 5",
        "OUTPUT 6", "OUTPUT 7", "OUTPUT 8", "OPTICAL OUT L", "OPTICAL OUT R"};
    const char *got;
    UInt32 c;

    for (c = 1; c <= ins; c++) {
        got = channel_name(dev, kAudioObjectPropertyScopeInput, c);
        CHECK(strcmp(got, in_names[c - 1]) == 0, "input %u is \"%s\", not \"%s\"", c,
              in_names[c - 1], got);
    }
    for (c = 1; c <= outs; c++) {
        got = channel_name(dev, kAudioObjectPropertyScopeOutput, c);
        CHECK(strcmp(got, out_names[c - 1]) == 0, "output %u is \"%s\", not \"%s\"", c,
              out_names[c - 1], got);
    }
    CHECK(channel_name(dev, kAudioObjectPropertyScopeInput, ins + 1)[0] == 0,
          "no name for input %u, which this format hasn't got", ins + 1);
    CHECK(channel_name(dev, kAudioObjectPropertyScopeOutput, outs + 1)[0] == 0,
          "no name for output %u, which this format hasn't got", outs + 1);
    CHECK(channel_name(dev, kAudioObjectPropertyScopeInput, 0)[0] == 0 &&
              channel_name(dev, kAudioObjectPropertyScopeOutput, 0)[0] == 0,
          "no channel name for element 0 (the device as a whole)");
    CHECK(channel_name(dev, kAudioObjectPropertyScopeGlobal, 1)[0] == 0,
          "and none in the global scope");
}

/* Channel names per rate: 12 in and 10 out at 44.1 to 96 kHz, only the first 6 each way at 192. */
static void test_channel_names(void)
{
    AudioObjectID dev, again;
    Boolean settable = 1;
    AudioObjectPropertyAddress a = {kAudioObjectPropertyElementName,
                                    kAudioObjectPropertyScopeInput, 1};

    CHECK(devices(&dev) == 1, "a device at 48 kHz");
    check_channel_names(dev, 12, 10);
    CHECK((*D)->IsPropertySettable(D, dev, 0, &a, &settable) == 0 && !settable,
          "a channel's name can't be set");

    make_block(192000.0, 6, 6);
    helper_changed();
    CHECK(devices(&again) == 1 && again != dev, "192 kHz is a new device object");
    check_channel_names(again, 6, 6);
    CHECK(strcmp(channel_name(again, kAudioObjectPropertyScopeInput, 9), "") == 0 &&
              strcmp(channel_name(again, kAudioObjectPropertyScopeOutput, 9), "") == 0,
          "at 192 kHz no optical channels are named");

    make_block(44100.0, 12, 10);
    helper_changed();
    CHECK(devices(&dev) == 1 && dev != again, "back at 44.1 kHz");
    check_channel_names(dev, 12, 10);
}


/* Before I/O has ever started there is nothing measured: the statistics say only that. */
static void test_stats_before_a_run(void)
{
    AudioObjectID dev;
    CFDictionaryRef d;

    devices(&dev);
    d = stats_of(dev);
    CHECK(d != NULL && CFDictionaryGetCount(d) == 1 &&
              CFDictionaryGetValue(d, CFSTR("running")) == kCFBooleanFalse,
          "before any run the statistics hold only \"running\": no");
    if (d != NULL)
        CFRelease(d);
}

/* Stops whatever I/O the earlier cases left running. */
static void stop_all(AudioObjectID dev)
{
    int k;

    for (k = 0; k < 16 && (*D)->StopIO(D, dev, 1) == 0; k++) {
    }
}

static int stored_is(CFStringRef key, double ms)
{
    return fabs(stored_number(key) - ms) < 0.01;
}

/*
 * The two safety offsets as the user's settings, with I/O stopped. The fake
 * helper gives 144 and 1024 frames, 3 ms and 21.33 ms at 48 kHz. Leaves both unset, 48 kHz.
 */
static void test_driver_settings(void)
{
    AudioObjectID dev;
    AudioServerPlugInCustomPropertyInfo info[8];
    AudioObjectPropertyAddress in_a = addr(INPUT_MARGIN, kAudioObjectPropertyScopeGlobal);
    AudioObjectPropertyAddress out_a = addr(OUTPUT_DELAY, kAudioObjectPropertyScopeGlobal);
    AudioObjectPropertyAddress stats_a = addr(STATS, kAudioObjectPropertyScopeGlobal);
    Boolean settable = 0;
    int restarts, writes, k;
    CFPropertyListRef v;
    const void *none = NULL;

    make_block(48000.0, 12, 10);
    helper_changed();
    CHECK(devices(&dev) == 1, "a device at 48 kHz");
    stop_all(dev);
    set_correction(dev, kCFBooleanFalse);
    store_put(CFSTR("recording margin ms"), NULL);
    store_put(CFSTR("playback delay ms"), NULL);
    ua101_plugin_reload_offsets();

    /* Declared, readable, settable. */
    CHECK(get(dev, kAudioObjectPropertyCustomPropertyInfoList, kAudioObjectPropertyScopeGlobal, info,
              sizeof(info)) == 4 * sizeof(info[0]) &&
              info[1].mSelector == INPUT_MARGIN && info[2].mSelector == OUTPUT_DELAY &&
              info[3].mSelector == STATS,
          "the two settings and the statistics are declared as custom properties");
    for (k = 1; k < 4; k++)
        CHECK(info[k].mPropertyDataType == kAudioServerPlugInCustomPropertyDataTypeCFPropertyList &&
                  info[k].mQualifierDataType == kAudioServerPlugInCustomPropertyDataTypeNone,
              "custom property %d is a property list without a qualifier", k);
    CHECK(margin_is(dev, INPUT_MARGIN) == 3.0, "unset, the recording margin reads the helper's 3 ms, %g",
          margin_is(dev, INPUT_MARGIN));
    CHECK(fabs(margin_is(dev, OUTPUT_DELAY) - 1024.0 / 48.0) < 1e-9,
          "and the playback delay its 21.33 ms, %g", margin_is(dev, OUTPUT_DELAY));
    CHECK((*D)->IsPropertySettable(D, dev, 0, &in_a, &settable) == 0 && settable &&
              (*D)->IsPropertySettable(D, dev, 0, &out_a, &settable) == 0 && settable,
          "both can be set");
    CHECK((*D)->HasProperty(D, dev, 0, &stats_a) &&
              (*D)->IsPropertySettable(D, dev, 0, &stats_a, &settable) == 0 && !settable,
          "the statistics can only be read");
    v = kCFBooleanTrue;
    CHECK((*D)->SetPropertyData(D, dev, 0, &stats_a, 0, NULL, sizeof(v), &v) != 0,
          "setting them is refused");
    CHECK(!(*D)->HasProperty(D, kAudioObjectPlugInObject, 0, &in_a) &&
              !(*D)->HasProperty(D, stream_of(dev, kAudioObjectPropertyScopeInput), 0, &in_a),
          "they are the device's, not the plug-in's or a stream's");

    /* A value that is the same count of frames as now: kept, and nothing for Core Audio. */
    forget_notes();
    restarts = g_restarts;
    writes = g_store_writes;
    CHECK(set_margin_ms(dev, OUTPUT_DELAY, 21.3) == 0, "21.3 ms is taken");
    CHECK(g_store_writes == writes + 1 && stored_is(CFSTR("playback delay ms"), 1024.0 / 48.0),
          "kept in Core Audio's storage as 21.33, %g", stored_number(CFSTR("playback delay ms")));
    CHECK(g_restarts == restarts, "the same 1024 frames: no change is asked of Core Audio");
    CHECK(noted(dev, OUTPUT_DELAY) && safety(dev, kAudioObjectPropertyScopeOutput) == 1024,
          "the setting is told; the offset is as it was");

    /* A value that moves the offset: kept, a change is asked for, and the offset moves when
       Core Audio performs it, not before. */
    forget_notes();
    writes = g_store_writes;
    CHECK(set_margin_ms(dev, INPUT_MARGIN, 8.0) == 0, "8 ms is taken");
    CHECK(g_store_writes == writes + 1 && stored_is(CFSTR("recording margin ms"), 8.0), "kept");
    CHECK(g_restarts == restarts + 1 && g_restart_action == CHANGE_OFFSETS,
          "a configuration change is asked for, action %llu", g_restart_action);
    CHECK(margin_is(dev, INPUT_MARGIN) == 8.0 && noted(dev, INPUT_MARGIN), "the setting reads 8 ms");
    CHECK(safety(dev, kAudioObjectPropertyScopeInput) == 144,
          "but Core Audio still has 144 frames until it performs the change");
    CHECK(!noted(dev, kAudioDevicePropertySafetyOffset), "and isn't told otherwise yet");
    CHECK((*D)->PerformDeviceConfigurationChange(D, dev, CHANGE_OFFSETS, NULL) == 0, "performed");
    ua101_plugin_wait_for_hello();
    CHECK(safety(dev, kAudioObjectPropertyScopeInput) == 384, "then 384 frames, %u",
          safety(dev, kAudioObjectPropertyScopeInput));
    CHECK(safety(dev, kAudioObjectPropertyScopeOutput) == 1024, "playback's is untouched");
    CHECK(noted(dev, kAudioDevicePropertySafetyOffset), "and Core Audio is told");

    /* The same again changes nothing. */
    writes = g_store_writes;
    CHECK(set_margin_ms(dev, INPUT_MARGIN, 8.0) == 0 && g_store_writes == writes &&
              g_restarts == restarts + 1,
          "setting it as it is changes nothing");

    /* Changed and changed back before Core Audio got to it: one change asked for, and the
       offset ends where the setting is. */
    CHECK(set_margin_ms(dev, INPUT_MARGIN, 5.0) == 0 && g_restarts == restarts + 2,
          "5 ms asks for a change");
    CHECK(set_margin_ms(dev, INPUT_MARGIN, 8.0) == 0 && g_restarts == restarts + 2,
          "back to 8 ms, the count Core Audio still has: nothing more is asked");
    CHECK((*D)->PerformDeviceConfigurationChange(D, dev, CHANGE_OFFSETS, NULL) == 0 &&
              safety(dev, kAudioObjectPropertyScopeInput) == 384 &&
              margin_is(dev, INPUT_MARGIN) == 8.0 && stored_is(CFSTR("recording margin ms"), 8.0),
          "the change asked for earlier leaves 8 ms, 384 frames");

    /* Only the values on the list. */
    writes = g_store_writes;
    {
        static const double bad[] = {0.0, 0.5, 1.5, 2.9, 4.0, 6.5, 12.0, 21.33, -5.0, 10.1, 1e9, NAN, INFINITY};
        static const double bad_out[] = {0.0, 1.0, 3.0, 5.0, 7.0, 9.0, 9.9, 11.0, 20.0, 21.5, -12.0, NAN};
        size_t i;

        for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
            CHECK(set_margin_ms(dev, INPUT_MARGIN, bad[i]) == kAudioHardwareIllegalOperationError,
                  "a recording margin of %g ms is refused", bad[i]);
        for (i = 0; i < sizeof(bad_out) / sizeof(bad_out[0]); i++)
            CHECK(set_margin_ms(dev, OUTPUT_DELAY, bad_out[i]) == kAudioHardwareIllegalOperationError,
                  "a playback delay of %g ms is refused", bad_out[i]);
    }
    CHECK(set_margin(dev, INPUT_MARGIN, CFSTR("8")) == kAudioHardwareIllegalOperationError &&
              set_margin(dev, INPUT_MARGIN, kCFBooleanTrue) == kAudioHardwareIllegalOperationError &&
              set_margin(dev, OUTPUT_DELAY, CFSTR("12")) == kAudioHardwareIllegalOperationError,
          "and so is anything that isn't a number");
    v = NULL;
    CHECK((*D)->SetPropertyData(D, dev, 0, &in_a, 0, NULL, sizeof(v), &v) ==
              kAudioHardwareIllegalOperationError &&
              (*D)->SetPropertyData(D, dev, 0, &in_a, 0, NULL, 4, &v) ==
                  kAudioHardwareBadPropertySizeError &&
              (*D)->SetPropertyData(D, dev, 0, &in_a, 0, NULL, sizeof(v), none) ==
                  kAudioHardwareIllegalOperationError,
          "nor nothing at all, or too few bytes");
    ua101_plugin_wait_for_hello();
    CHECK(g_store_writes == writes && g_restarts == restarts + 2 &&
              margin_is(dev, INPUT_MARGIN) == 8.0 && safety(dev, kAudioObjectPropertyScopeInput) == 384 &&
              fabs(margin_is(dev, OUTPUT_DELAY) - 1024.0 / 48.0) < 1e-9 &&
              safety(dev, kAudioObjectPropertyScopeOutput) == 1024,
          "none of it kept, asked for or used");

    /* Every value on the lists is taken, at its count of frames. */
    {
        /* The first two margins and the first four delays are the ones below the recommended
           (2026-11-20): the plug-in takes them like any other. */
        static const double in_ms[] = {1.0, 2.0, 3.0, 5.0, 8.0, 10.0};
        static const UInt32 in_frames[] = {48, 96, 144, 240, 384, 480};
        static const double out_ms[] = {2.0, 4.0, 6.0, 8.0, 10.0, 12.0, 14.0, 16.0, 21.3};
        static const UInt32 out_frames[] = {96, 192, 288, 384, 480, 576, 672, 768, 1024};
        size_t i;

        for (i = 0; i < 6; i++) {
            CHECK(set_margin_ms(dev, INPUT_MARGIN, in_ms[i]) == 0, "%g ms in", in_ms[i]);
            (*D)->PerformDeviceConfigurationChange(D, dev, CHANGE_OFFSETS, NULL);
            CHECK(safety(dev, kAudioObjectPropertyScopeInput) == in_frames[i],
                  "a recording margin of %g ms is %u frames, %u", in_ms[i], in_frames[i],
                  safety(dev, kAudioObjectPropertyScopeInput));
        }
        for (i = 0; i < 9; i++) {
            CHECK(set_margin_ms(dev, OUTPUT_DELAY, out_ms[i]) == 0, "%g ms out", out_ms[i]);
            (*D)->PerformDeviceConfigurationChange(D, dev, CHANGE_OFFSETS, NULL);
            CHECK(safety(dev, kAudioObjectPropertyScopeOutput) == out_frames[i],
                  "a playback delay of %g ms is %u frames, %u", out_ms[i], out_frames[i],
                  safety(dev, kAudioObjectPropertyScopeOutput));
        }
        ua101_plugin_wait_for_hello();
    }

    /* Core Audio won't perform a change: the old offset stays, and the setting goes back to
       it, in the storage too, so what a panel shows is what is used. */
    restarts = g_restarts;
    forget_notes();
    CHECK(set_margin_ms(dev, OUTPUT_DELAY, 12.0) == 0 && g_restarts == restarts + 1 &&
              margin_is(dev, OUTPUT_DELAY) == 12.0 && stored_is(CFSTR("playback delay ms"), 12.0),
          "12 ms asked for");
    CHECK((*D)->AbortDeviceConfigurationChange(D, dev, CHANGE_OFFSETS, NULL) == 0, "aborted");
    ua101_plugin_wait_for_hello();
    CHECK(safety(dev, kAudioObjectPropertyScopeOutput) == 1024 &&
              fabs(margin_is(dev, OUTPUT_DELAY) - 1024.0 / 48.0) < 1e-9 &&
              stored_is(CFSTR("playback delay ms"), 1024.0 / 48.0),
          "after an abort the setting is 21.33 ms again, as used: %g, kept %g",
          margin_is(dev, OUTPUT_DELAY), stored_number(CFSTR("playback delay ms")));
    CHECK(margin_is(dev, INPUT_MARGIN) == 10.0 && safety(dev, kAudioObjectPropertyScopeInput) == 480,
          "and the other one is left alone");
    CHECK(set_margin_ms(dev, OUTPUT_DELAY, 12.0) == 0 && g_restarts == restarts + 2,
          "asking again works");
    CHECK((*D)->PerformDeviceConfigurationChange(D, dev, CHANGE_OFFSETS, NULL) == 0 &&
              safety(dev, kAudioObjectPropertyScopeOutput) == 576,
          "and this time it is performed: 576 frames");
    /* An abort of another kind of change touches nothing. */
    CHECK((*D)->AbortDeviceConfigurationChange(D, dev, 1, NULL) == 0 &&
              (*D)->AbortDeviceConfigurationChange(D, dev, 2, NULL) == 0 &&
              safety(dev, kAudioObjectPropertyScopeOutput) == 576 &&
              margin_is(dev, OUTPUT_DELAY) == 12.0,
          "aborting a restart or a correction change leaves the offsets alone");
    ua101_plugin_wait_for_hello();

    store_put(CFSTR("recording margin ms"), NULL);
    store_put(CFSTR("playback delay ms"), NULL);
    ua101_plugin_reload_offsets();
    CHECK(safety(dev, kAudioObjectPropertyScopeInput) == 144 &&
              safety(dev, kAudioObjectPropertyScopeOutput) == 1024,
          "unset again: the helper's own offsets");
}

/*
 * The settings and the measured room while I/O runs: a change is taken by Core Audio stopping
 * I/O, performing it and starting again; each run measures the least room recording had, and
 * passes on the helper's two counts for playback; the stop tells the helper what the run used.
 * Leaves I/O stopped, both unset, bit-exact.
 */
static void test_driver_settings_running(void)
{
    AudioObjectID dev, in, out;
    enum { FRAMES = 512, IN_CH = 12, OUT_CH = 10 };
    static int32_t frames[8192 * IN_CH];
    static float buf[FRAMES * IN_CH];
    int restarts = g_restarts, stops;
    double x;

    devices(&dev);
    in = stream_of(dev, kAudioObjectPropertyScopeInput);
    out = stream_of(dev, kAudioObjectPropertyScopeOutput);
    CHECK(set_margin_ms(dev, INPUT_MARGIN, 8.0) == 0 && set_margin_ms(dev, OUTPUT_DELAY, 12.0) == 0 &&
              (*D)->PerformDeviceConfigurationChange(D, dev, CHANGE_OFFSETS, NULL) == 0,
          "8 ms and 12 ms set");
    restarts = g_restarts;

    CHECK((*D)->StartIO(D, dev, 1) == 0, "starts");
    /* As the helper's engine does at each start. */
    atomic_store(&F.sh->stats.playback_spare_min, INT64_MAX);
    atomic_store(&F.sh->stats.playback_late, 0);
    CHECK(stat_value(dev, "running") == 1.0 && stat_value(dev, "rate") == 48000.0 &&
              stat_value(dev, "input offset") == 384.0 && stat_value(dev, "output offset") == 576.0,
          "the statistics name the run: running, 48 kHz, 384 and 576 frames");
    CHECK(!stat_of(dev, "capture spare", &x) && !stat_of(dev, "playback spare", &x) &&
              stat_value(dev, "capture missing") == 0.0 && stat_value(dev, "playback late") == 0.0,
          "nothing measured before the first cycle, nothing lost");

    /* Recording: the ring holds frames 0 to 8191; a cycle that wants 1000 to 1511 leaves 6680. */
    memset(frames, 0, sizeof(frames));
    ua101_frame_ring_write(&F.sh->capture, 0, frames, 8192);
    do_io(dev, in, kAudioServerPlugInIOOperationReadInput, 1000.0, FRAMES, buf);
    CHECK(stat_value(dev, "capture spare") == 6680.0, "recording's room: 8192 - 1512 = 6680, %g",
          stat_value(dev, "capture spare"));
    do_io(dev, in, kAudioServerPlugInIOOperationReadInput, 5000.0, FRAMES, buf);
    CHECK(stat_value(dev, "capture spare") == 2680.0, "the least so far: 2680, %g",
          stat_value(dev, "capture spare"));
    do_io(dev, in, kAudioServerPlugInIOOperationReadInput, 2000.0, FRAMES, buf);
    CHECK(stat_value(dev, "capture spare") == 2680.0, "more room later doesn't raise it");
    do_io(dev, in, kAudioServerPlugInIOOperationReadInput, 8192.0 - FRAMES, FRAMES, buf);
    CHECK(stat_value(dev, "capture spare") == 0.0 && stat_value(dev, "capture missing") == 0.0,
          "a cycle that gets exactly the newest frame: 0 room, nothing missing");
    /* A cycle 12 frames from the end: 500 frames haven't come. */
    do_io(dev, in, kAudioServerPlugInIOOperationReadInput, 8192.0 - 12, FRAMES, buf);
    CHECK(stat_value(dev, "capture spare") == -500.0 && stat_value(dev, "capture missing") == 500.0,
          "500 frames short: room %g, missing %g", stat_value(dev, "capture spare"),
          stat_value(dev, "capture missing"));
    /* Once the helper's stream has stopped (the unit left), nothing is measured: the frames
       are still counted missing. */
    atomic_store(&F.sh->state, UA101_STATE_FAILED);
    do_io(dev, in, kAudioServerPlugInIOOperationReadInput, 20000.0, FRAMES, buf);
    atomic_store(&F.sh->state, UA101_STATE_RUNNING);
    CHECK(stat_value(dev, "capture spare") == -500.0 && stat_value(dev, "capture missing") == 1012.0,
          "with the stream stopped the room isn't measured: %g, missing %g",
          stat_value(dev, "capture spare"), stat_value(dev, "capture missing"));

    /* Playback: the helper's counts, as each cycle finds them in the block. */
    memset(buf, 0, sizeof(buf));
    do_io(dev, out, kAudioServerPlugInIOOperationWriteMix, 30000.0, FRAMES, buf);
    CHECK(!stat_of(dev, "playback spare", &x), "while the helper has measured nothing: no figure");
    atomic_store(&F.sh->stats.playback_spare_min, 137);
    CHECK(!stat_of(dev, "playback spare", &x), "a property call doesn't look into the block itself");
    do_io(dev, out, kAudioServerPlugInIOOperationWriteMix, 30512.0, FRAMES, buf);
    CHECK(stat_value(dev, "playback spare") == 137.0 && stat_value(dev, "playback late") == 0.0,
          "playback's room as the helper counts it: 137, %g", stat_value(dev, "playback spare"));
    atomic_store(&F.sh->stats.playback_spare_min, -40);
    atomic_store(&F.sh->stats.playback_late, 96);
    do_io(dev, out, kAudioServerPlugInIOOperationWriteMix, 31024.0, FRAMES, buf);
    CHECK(stat_value(dev, "playback spare") == -40.0 && stat_value(dev, "playback late") == 96.0,
          "and its late frames: -40 and 96");
    CHECK(stat_value(dev, "seconds") > 0.0 && stat_value(dev, "seconds") < 30.0,
          "the run's length so far, %g s", stat_value(dev, "seconds"));

    /* A change while running: asked of Core Audio, which stops I/O, performs it and starts
       I/O again. The stop tells the helper what the run used and measured. */
    CHECK(set_margin_ms(dev, INPUT_MARGIN, 10.0) == 0 && g_restarts == restarts + 1 &&
              g_restart_action == CHANGE_OFFSETS,
          "10 ms while running asks for the change");
    CHECK(stat_value(dev, "input offset") == 384.0 && safety(dev, kAudioObjectPropertyScopeInput) == 384,
          "the run that is on keeps its 384 frames");
    stops = F.stops;
    CHECK((*D)->StopIO(D, dev, 1) == 0 && F.stops == stops + 1, "Core Audio stops I/O");
    CHECK(F.last_counts.input_offset == 384 && F.last_counts.output_offset == 576,
          "the helper is told the offsets the run used: %u and %u", F.last_counts.input_offset,
          F.last_counts.output_offset);
    CHECK(F.last_counts.capture_spare_known && F.last_counts.capture_spare == -500 &&
              F.last_counts.capture_missing == 1012,
          "and recording's least room, %lld, with the %llu frames missing",
          (long long)F.last_counts.capture_spare, (unsigned long long)F.last_counts.capture_missing);
    CHECK(stat_value(dev, "running") == 0.0 && stat_value(dev, "capture spare") == -500.0 &&
              stat_value(dev, "capture missing") == 1012.0 &&
              stat_value(dev, "playback spare") == -40.0 && stat_value(dev, "playback late") == 96.0 &&
              stat_value(dev, "input offset") == 384.0 && stat_value(dev, "seconds") > 0.0,
          "stopped, the statistics keep the run's figures");
    CHECK((*D)->PerformDeviceConfigurationChange(D, dev, CHANGE_OFFSETS, NULL) == 0 &&
              safety(dev, kAudioObjectPropertyScopeInput) == 480,
          "performed: 480 frames");
    CHECK((*D)->StartIO(D, dev, 1) == 0, "and I/O starts again");
    atomic_store(&F.sh->stats.playback_spare_min, INT64_MAX);
    atomic_store(&F.sh->stats.playback_late, 0);
    CHECK(stat_value(dev, "running") == 1.0 && stat_value(dev, "input offset") == 480.0 &&
              stat_value(dev, "output offset") == 576.0 && !stat_of(dev, "capture spare", &x) &&
              !stat_of(dev, "playback spare", &x) && stat_value(dev, "capture missing") == 0.0 &&
              stat_value(dev, "playback late") == 0.0,
          "the new run starts its figures afresh, with 480 frames");
    stops = F.stops;
    CHECK((*D)->StopIO(D, dev, 1) == 0 && F.stops == stops + 1 &&
              F.last_counts.input_offset == 480 && !F.last_counts.capture_spare_known,
          "a run without a cycle tells the helper its offsets and no room");

    /* With rate correction the recording path is another one: it measures too. Sample time 0
       was a second ago on the fake helper's line, so sample time n is unit frame n. */
    CHECK(set_correction(dev, kCFBooleanTrue) == 0 && (*D)->StartIO(D, dev, 1) == 0, "corrected");
    ua101_frame_ring_write(&F.sh->capture, 0, frames, 8192);
    do_io(dev, in, kAudioServerPlugInIOOperationReadInput, 1000.0, FRAMES, buf);
    CHECK(stat_of(dev, "capture spare", &x) && fabs(x - 6680.0) < 64.0,
          "corrected, a cycle for 1000 to 1511 leaves about 6680 frames, %g", x);
    do_io(dev, in, kAudioServerPlugInIOOperationReadInput, 1000.0 + FRAMES, FRAMES, buf);
    CHECK(stat_of(dev, "capture spare", &x) && fabs(x - 6168.0) < 64.0 &&
              stat_value(dev, "capture missing") == 0.0,
          "and the next about 6168, %g", x);
    do_io(dev, in, kAudioServerPlugInIOOperationReadInput, 8192.0 - 12, FRAMES, buf);
    CHECK(stat_of(dev, "capture spare", &x) && x < -400.0 && x > -600.0 &&
              stat_value(dev, "capture missing") > 400.0,
          "a cycle past the ring's end: about -500, %g", x);
    CHECK((*D)->StopIO(D, dev, 1) == 0 && F.last_counts.capture_spare_known &&
              F.last_counts.capture_spare == (int64_t)x && F.last_counts.corrected,
          "told to the helper at the stop");
    set_correction(dev, kCFBooleanFalse);

    store_put(CFSTR("recording margin ms"), NULL);
    store_put(CFSTR("playback delay ms"), NULL);
    ua101_plugin_reload_offsets();
}

/* What Core Audio kept is read when the plug-in starts: values from the lists only, and they
   hold at every rate as times. Leaves both unset. */
static void test_driver_settings_kept(void)
{
    AudioObjectID dev;
    double eight = 8.0, twelve = 12.0, four = 4.0, five = 5.0;
    CFNumberRef n8 = CFNumberCreate(NULL, kCFNumberDoubleType, &eight);
    CFNumberRef n12 = CFNumberCreate(NULL, kCFNumberDoubleType, &twelve);
    CFNumberRef n4 = CFNumberCreate(NULL, kCFNumberDoubleType, &four);
    CFNumberRef n5 = CFNumberCreate(NULL, kCFNumberDoubleType, &five);

    devices(&dev);
    store_put(CFSTR("recording margin ms"), n8);
    store_put(CFSTR("playback delay ms"), n12);
    ua101_plugin_reload_offsets();
    CHECK(safety(dev, kAudioObjectPropertyScopeInput) == 384 &&
              safety(dev, kAudioObjectPropertyScopeOutput) == 576 &&
              margin_is(dev, INPUT_MARGIN) == 8.0 && margin_is(dev, OUTPUT_DELAY) == 12.0,
          "kept settings are read at the start: 8 and 12 ms, 384 and 576 frames");

    /* The same times at another rate: the unit switched to 96 kHz is a new device. */
    make_block(96000.0, 12, 10);
    helper_changed();
    CHECK(devices(&dev) == 1 && safety(dev, kAudioObjectPropertyScopeInput) == 768 &&
              safety(dev, kAudioObjectPropertyScopeOutput) == 1152 &&
              margin_is(dev, INPUT_MARGIN) == 8.0 && margin_is(dev, OUTPUT_DELAY) == 12.0,
          "at 96 kHz the same 8 and 12 ms are 768 and 1152 frames: %u and %u",
          safety(dev, kAudioObjectPropertyScopeInput), safety(dev, kAudioObjectPropertyScopeOutput));
    make_block(48000.0, 12, 10);
    helper_changed();
    devices(&dev);

    /* Anything else in the storage leaves the helper's own value in force. */
    store_put(CFSTR("recording margin ms"), n4);
    store_put(CFSTR("playback delay ms"), CFSTR("12"));
    ua101_plugin_reload_offsets();
    CHECK(safety(dev, kAudioObjectPropertyScopeInput) == 144 &&
              safety(dev, kAudioObjectPropertyScopeOutput) == 1024,
          "a kept value that isn't on the list, or isn't a number, is ignored");
    store_put(CFSTR("recording margin ms"), n12); /* a playback value under recording's key */
    store_put(CFSTR("playback delay ms"), n5);    /* and the other way round (8 ms is on both) */
    ua101_plugin_reload_offsets();
    CHECK(safety(dev, kAudioObjectPropertyScopeInput) == 144 &&
              safety(dev, kAudioObjectPropertyScopeOutput) == 1024,
          "nor one from the other list");
    store_put(CFSTR("recording margin ms"), n8);
    store_put(CFSTR("playback delay ms"), NULL);
    ua101_plugin_reload_offsets();
    CHECK(safety(dev, kAudioObjectPropertyScopeInput) == 384 &&
              safety(dev, kAudioObjectPropertyScopeOutput) == 1024,
          "one alone is taken, the other stays the helper's");

    store_put(CFSTR("recording margin ms"), NULL);
    ua101_plugin_reload_offsets();
    CFRelease(n8);
    CFRelease(n12);
    CFRelease(n4);
    CFRelease(n5);
}

int main(void)
{
    make_block(48000.0, 12, 10);
    F.start_ok = 1;
    F.clock_comes = 1;
    /* The plug-in keeps the device listed for 30 s after the unit leaves, in case it comes
       back. These cases are about everything else, so here it keeps nothing and the device
       goes at once; the time itself is tested with the real service (tests/test_service.c). */
    ua101_plugin_set_gap_seconds(0.0);

    test_factory();
    test_absent_then_present();
    test_stats_before_a_run();
    test_properties();
    test_correction_setting();
    test_io();
    test_restart();
    test_helper_crash();
    test_threads();
    test_correction();
    test_steer_module();
    test_steering();
    test_playback_overs();
    test_unit_goes();
    test_format_change();
    test_idle_close();
    test_channel_names();
    test_driver_settings();
    test_driver_settings_running();
    test_driver_settings_kept();

    if (failures == 0)
        printf("test_plugin: %d checks passed\n", checks);
    else
        printf("test_plugin: %d of %d checks failed\n", failures, checks);
    return failures == 0 ? 0 : 1;
}
