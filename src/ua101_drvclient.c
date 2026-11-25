#include "ua101_drvclient.h"

#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <string.h>

#include "ua101_drvset.h"

static AudioObjectPropertyAddress address(AudioObjectPropertySelector selector)
{
    AudioObjectPropertyAddress a = {selector, kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};

    return a;
}

/* The UA-101's device, or kAudioObjectUnknown. */
static OSStatus find_device(AudioObjectID *device)
{
    AudioObjectPropertyAddress a = address(kAudioHardwarePropertyTranslateUIDToDevice);
    CFStringRef uid = CFSTR(UA101_DEVICE_UID);
    UInt32 size = sizeof(*device);

    *device = kAudioObjectUnknown;
    return AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, sizeof(uid), &uid, &size,
                                      device);
}

/* A property list value of the device: the caller releases it. NULL if it can't be read. */
static CFPropertyListRef copy_value(AudioObjectID device, AudioObjectPropertySelector selector)
{
    AudioObjectPropertyAddress a = address(selector);
    CFPropertyListRef value = NULL;
    UInt32 size = sizeof(value);

    if (!AudioObjectHasProperty(device, &a) ||
        AudioObjectGetPropertyData(device, &a, 0, NULL, &size, &value) != 0 || size != sizeof(value))
        return NULL;
    return value;
}

static int number_of(CFPropertyListRef value, double *out)
{
    return value != NULL && CFGetTypeID(value) == CFNumberGetTypeID() &&
           CFNumberGetValue((CFNumberRef)value, kCFNumberDoubleType, out);
}

static int stat_number(CFDictionaryRef stats, CFStringRef key, double *out)
{
    return number_of(CFDictionaryGetValue(stats, key), out);
}

int ua101_drv_read(ua101_drv_state *out)
{
    AudioObjectID device;
    CFPropertyListRef in, play, correction, stats;
    OSStatus status;

    memset(out, 0, sizeof(*out));
    out->correction = -1;
    status = find_device(&device);
    if (status != 0)
        return (int)status;
    if (device == kAudioObjectUnknown)
        return 0;
    out->found = 1;

    in = copy_value(device, UA101_PROPERTY_INPUT_MARGIN);
    play = copy_value(device, UA101_PROPERTY_OUTPUT_DELAY);
    out->have_settings = number_of(in, &out->input_ms) && number_of(play, &out->output_ms);
    if (in != NULL)
        CFRelease(in);
    if (play != NULL)
        CFRelease(play);

    correction = copy_value(device, UA101_PROPERTY_RATE_CORRECTION);
    if (correction != NULL && CFGetTypeID(correction) == CFBooleanGetTypeID())
        out->correction = CFBooleanGetValue((CFBooleanRef)correction) ? 1 : 0;
    if (correction != NULL)
        CFRelease(correction);

    stats = copy_value(device, UA101_PROPERTY_STATS);
    if (stats != NULL && CFGetTypeID(stats) == CFDictionaryGetTypeID()) {
        CFDictionaryRef d = (CFDictionaryRef)stats;
        CFPropertyListRef running = CFDictionaryGetValue(d, CFSTR(UA101_STAT_RUNNING));

        out->have_stats = 1;
        out->running = running != NULL && CFGetTypeID(running) == CFBooleanGetTypeID() &&
                       CFBooleanGetValue((CFBooleanRef)running);
        out->any_run = stat_number(d, CFSTR(UA101_STAT_RATE), &out->rate) && out->rate > 0.0;
        stat_number(d, CFSTR(UA101_STAT_SECONDS), &out->seconds);
        stat_number(d, CFSTR(UA101_STAT_INPUT_OFFSET), &out->input_offset);
        stat_number(d, CFSTR(UA101_STAT_OUTPUT_OFFSET), &out->output_offset);
        out->capture_spare_known =
            stat_number(d, CFSTR(UA101_STAT_CAPTURE_SPARE), &out->capture_spare);
        out->playback_spare_known =
            stat_number(d, CFSTR(UA101_STAT_PLAYBACK_SPARE), &out->playback_spare);
        stat_number(d, CFSTR(UA101_STAT_CAPTURE_MISSING), &out->capture_missing);
        stat_number(d, CFSTR(UA101_STAT_PLAYBACK_LATE), &out->playback_late);
    }
    if (stats != NULL)
        CFRelease(stats);
    return 0;
}

static int set_value(AudioObjectPropertySelector selector, CFPropertyListRef value)
{
    AudioObjectPropertyAddress a = address(selector);
    AudioObjectID device;
    OSStatus status = find_device(&device);

    if (status != 0)
        return (int)status;
    if (device == kAudioObjectUnknown)
        return (int)kAudioHardwareBadObjectError;
    return (int)AudioObjectSetPropertyData(device, &a, 0, NULL, sizeof(value), &value);
}

static int set_number(AudioObjectPropertySelector selector, double ms)
{
    CFNumberRef n = CFNumberCreate(NULL, kCFNumberDoubleType, &ms);
    int status;

    if (n == NULL)
        return (int)kAudioHardwareUnspecifiedError;
    status = set_value(selector, n);
    CFRelease(n);
    return status;
}

int ua101_drv_set_input_ms(double ms)
{
    /* Asked here too, so a value the driver doesn't offer never travels. */
    if (ua101_input_margin_index(ms) < 0)
        return (int)kAudioHardwareIllegalOperationError;
    return set_number(UA101_PROPERTY_INPUT_MARGIN, ms);
}

int ua101_drv_set_output_ms(double ms)
{
    if (ua101_output_delay_index(ms) < 0)
        return (int)kAudioHardwareIllegalOperationError;
    return set_number(UA101_PROPERTY_OUTPUT_DELAY, ms);
}

int ua101_drv_set_correction(int on)
{
    return set_value(UA101_PROPERTY_RATE_CORRECTION, on ? kCFBooleanTrue : kCFBooleanFalse);
}

int ua101_drv_input_count(void)
{
    return UA101_INPUT_MARGIN_COUNT;
}

double ua101_drv_input_value(int index)
{
    return index >= 0 && index < UA101_INPUT_MARGIN_COUNT ? ua101_input_margins[index] * 1000.0 : 0.0;
}

int ua101_drv_input_default(void)
{
    return UA101_INPUT_MARGIN_DEFAULT;
}

int ua101_drv_input_recommended(void)
{
    return UA101_INPUT_MARGIN_RECOMMENDED;
}

int ua101_drv_input_index(double ms)
{
    return ua101_input_margin_index(ms);
}

int ua101_drv_output_count(void)
{
    return UA101_OUTPUT_DELAY_COUNT;
}

double ua101_drv_output_value(int index)
{
    return index >= 0 && index < UA101_OUTPUT_DELAY_COUNT ? ua101_output_delays[index] * 1000.0 : 0.0;
}

int ua101_drv_output_default(void)
{
    return UA101_OUTPUT_DELAY_DEFAULT;
}

int ua101_drv_output_recommended(void)
{
    return UA101_OUTPUT_DELAY_RECOMMENDED;
}

unsigned ua101_drv_output_min_buffer(int index, double rate)
{
    return ua101_output_min_buffer_frames(index, rate);
}

/* The short note under each recording margin (2026-11-20, the user approved the wording at
   09:28): what each value did in the test at the box. No buffer changes these. */
static const char *const input_notes[UA101_INPUT_MARGIN_COUNT] = {
    "gaps", "a few clicks", "clean, tight", "clean", "clean, roomy", "most room"};

const char *ua101_drv_input_note(int index)
{
    return index >= 0 && index < UA101_INPUT_MARGIN_COUNT ? input_notes[index] : "";
}

int ua101_drv_output_index(double ms)
{
    return ua101_output_delay_index(ms);
}
