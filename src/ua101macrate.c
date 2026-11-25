/*
 * ua101macrate: for the cable test. Plays one quiet tone on the left channel of
 * the Mac's headphone output and measures the rate the Mac's own audio clock
 * really runs at, against the host clock, from Core Audio's timestamps.
 *
 * Without --play it only shows the output device and plays nothing. With --play
 * it refuses any output that isn't headphones, so nothing comes out of the
 * speakers, and it never changes the volume. The tone uses the project's caps
 * (at most -40 dBFS and 5 s); the rest of the run is silence.
 *
 *   ua101macrate [--play] [--seconds S] [--tone-at S] [--db DB] [--freq HZ] [--log FILE]
 */
#include <CoreAudio/CoreAudio.h>
#include <mach/mach_time.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ua101_stream.h"

#define MAX_STAMPS 200000

typedef struct {
    ua101_tone tone;
    uint64_t tone_at; /* frames after the first callback */
    double first;     /* sample time of the first callback, or -1 */
    double *sample_time;
    uint64_t *host_time;
    _Atomic unsigned count;
} player;

static OSStatus render(AudioObjectID device, const AudioTimeStamp *now, const AudioBufferList *in,
                       const AudioTimeStamp *in_time, AudioBufferList *out,
                       const AudioTimeStamp *out_time, void *context)
{
    player *p = context;
    unsigned b, ch = 0, c;
    (void)device; (void)now; (void)in; (void)in_time;

    if (p->first < 0.0)
        p->first = out_time->mSampleTime;
    if ((out_time->mFlags & kAudioTimeStampSampleHostTimeValid) == kAudioTimeStampSampleHostTimeValid) {
        unsigned i = p->count;
        if (i < MAX_STAMPS) {
            p->sample_time[i] = out_time->mSampleTime;
            p->host_time[i] = out_time->mHostTime;
            p->count = i + 1;
        }
    }
    for (b = 0; b < out->mNumberBuffers; b++) {
        AudioBuffer *buf = &out->mBuffers[b];
        float *s = buf->mData;
        unsigned channels = buf->mNumberChannels, frames, f;
        if (s == NULL || channels == 0)
            continue;
        frames = buf->mDataByteSize / (unsigned)(sizeof(float) * channels);
        memset(s, 0, buf->mDataByteSize);
        for (c = 0; c < channels; c++, ch++) {
            if (ch != 0)
                continue; /* the tone goes on the left only */
            for (f = 0; f < frames; f++) {
                double pos = out_time->mSampleTime + f - p->first;
                if (pos >= (double)p->tone_at)
                    s[f * channels + c] = (float)ua101_tone_value(&p->tone, 0, (uint64_t)pos - p->tone_at);
            }
        }
    }
    return noErr;
}

static int get(AudioObjectID obj, AudioObjectPropertySelector sel, AudioObjectPropertyScope scope,
               void *data, UInt32 size)
{
    AudioObjectPropertyAddress a = {sel, scope, kAudioObjectPropertyElementMain};
    return AudioObjectGetPropertyData(obj, &a, 0, NULL, &size, data) == noErr ? 0 : -1;
}

int main(int argc, char **argv)
{
    double seconds = 30.0, tone_at = 10.0, db = -40.0, freq = 997.0, nominal = 0.0, actual = 0.0;
    const char *log_path = NULL;
    int play = 0, i, headphones;
    AudioDeviceID device = 0;
    AudioStreamBasicDescription format;
    AudioObjectID stream = 0;
    CFStringRef cfname = NULL;
    char name[256] = "?";
    Float32 volume = -1.0f;
    AudioDeviceIOProcID proc = NULL;
    static player p;
    mach_timebase_info_data_t tb;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--play") == 0) {
            play = 1;
        } else if (i + 1 < argc && strcmp(argv[i], "--seconds") == 0) {
            seconds = atof(argv[++i]);
        } else if (i + 1 < argc && strcmp(argv[i], "--tone-at") == 0) {
            tone_at = atof(argv[++i]);
        } else if (i + 1 < argc && strcmp(argv[i], "--db") == 0) {
            db = atof(argv[++i]);
        } else if (i + 1 < argc && strcmp(argv[i], "--freq") == 0) {
            freq = atof(argv[++i]);
        } else if (i + 1 < argc && strcmp(argv[i], "--log") == 0) {
            log_path = argv[++i];
        } else {
            printf("Usage: ua101macrate [--play] [--seconds S] [--tone-at S] [--db DB] [--freq HZ] [--log FILE]\n"
                   "Without --play it only shows the output device. Defaults: 30 s, tone at 10 s,\n"
                   "-40 dBFS, 997 Hz, on the left only.\n");
            return 2;
        }
    }
    if (!(seconds >= 5.0 && seconds <= 600.0) || !(tone_at >= 1.0) ||
        !(tone_at + UA101_TONE_MAX_SECONDS + 1.0 <= seconds)) {
        printf("--seconds must be 5 to 600, with the tone (at --tone-at, at least 1 s in) ending\n"
               "at least 1 s before the end.\n");
        return 2;
    }

    if (get(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultOutputDevice,
            kAudioObjectPropertyScopeGlobal, &device, sizeof device) != 0 || device == 0) {
        printf("No default output device.\n");
        return 2;
    }
    if (get(device, kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, &cfname, sizeof cfname) == 0 &&
        cfname != NULL) {
        CFStringGetCString(cfname, name, sizeof name, kCFStringEncodingUTF8);
        CFRelease(cfname);
    }
    get(device, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, &nominal, sizeof nominal);
    if (get(device, kAudioDevicePropertyVolumeScalar, kAudioObjectPropertyScopeOutput, &volume, sizeof volume) != 0)
        volume = -1.0f;
    headphones = strcasestr(name, "headphone") != NULL;
    printf("Default output: %s, %.0f Hz", name, nominal);
    if (volume >= 0.0f)
        printf(", volume %.0f%%", volume * 100.0);
    printf("\n");
    if (!play) {
        printf("Nothing played. Add --play to play the tone%s.\n",
               headphones ? "" : " (it's refused unless the output is headphones)");
        return 0;
    }
    if (!headphones) {
        printf("Refused: the default output isn't headphones, so the tone could come out of the\n"
               "speakers. Plug the cable into the headphone jack and choose it in Sound settings.\n");
        return 2;
    }
    if (get(device, kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput, &stream, sizeof stream) != 0 ||
        get(stream, kAudioStreamPropertyVirtualFormat, kAudioObjectPropertyScopeGlobal, &format, sizeof format) != 0 ||
        format.mFormatID != kAudioFormatLinearPCM || !(format.mFormatFlags & kAudioFormatFlagIsFloat) ||
        format.mBitsPerChannel != 32) {
        printf("Refused: the output doesn't take 32-bit float samples.\n");
        return 2;
    }
    if (ua101_tone_init(&p.tone, nominal, db, UA101_TONE_MAX_SECONDS, freq, freq, 0) != 0) {
        printf("Refused: the level must be at most %.0f dBFS and the frequency below half the rate.\n",
               UA101_TONE_MAX_DBFS);
        return 2;
    }
    p.tone_at = (uint64_t)(tone_at * nominal + 0.5);
    p.first = -1.0;
    p.sample_time = calloc(MAX_STAMPS, sizeof *p.sample_time);
    p.host_time = calloc(MAX_STAMPS, sizeof *p.host_time);
    if (p.sample_time == NULL || p.host_time == NULL) {
        printf("Out of memory.\n");
        return 2;
    }
    if (AudioDeviceCreateIOProcID(device, render, &p, &proc) != noErr || AudioDeviceStart(device, proc) != noErr) {
        printf("Couldn't start the output.\n");
        return 2;
    }
    printf("Playing silence for %.0f s, with a %.0f s, %.0f dBFS, %.0f Hz tone on the left at %.0f s...\n",
           seconds, UA101_TONE_MAX_SECONDS, db, freq, tone_at);
    fflush(stdout);
    usleep((useconds_t)(seconds * 0.5 * 1e6));
    get(device, kAudioDevicePropertyActualSampleRate, kAudioObjectPropertyScopeGlobal, &actual, sizeof actual);
    usleep((useconds_t)(seconds * 0.5 * 1e6));
    AudioDeviceStop(device, proc);
    AudioDeviceDestroyIOProcID(device, proc);

    mach_timebase_info(&tb);
    {
        unsigned n = p.count, k, used = 0;
        double sx = 0, sy = 0, sxx = 0, sxy = 0, x0, slope, worst = 0.0;
        FILE *f = log_path ? fopen(log_path, "w") : NULL;
        if (f)
            fprintf(f, "host_seconds,sample_time\n");
        if (n < 100) {
            printf("Too few timestamps (%u).\n", n);
            return 1;
        }
        x0 = (double)p.host_time[0] * tb.numer / tb.denom / 1e9;
        /* The first second is left out, while the output settles. */
        for (k = 0; k < n; k++) {
            double x = (double)p.host_time[k] * tb.numer / tb.denom / 1e9 - x0, y = p.sample_time[k] - p.sample_time[0];
            if (f)
                fprintf(f, "%.9f,%.1f\n", x, y);
            if (x < 1.0)
                continue;
            sx += x; sy += y; sxx += x * x; sxy += x * y; used++;
        }
        if (f)
            fclose(f);
        slope = (used * sxy - sx * sy) / (used * sxx - sx * sx);
        for (k = 0; k < n; k++) {
            double x = (double)p.host_time[k] * tb.numer / tb.denom / 1e9 - x0, y = p.sample_time[k] - p.sample_time[0];
            double r = y - (sy / used + slope * (x - sx / used));
            if (x >= 1.0 && fabs(r) > worst)
                worst = fabs(r);
        }
        printf("Mac output against host: %.4f Hz (%+.3f ppm), from %u timestamps over %.1f s;\n"
               "  largest timestamp scatter %.2f samples\n",
               slope, (slope / nominal - 1.0) * 1e6, used,
               (double)(p.host_time[n - 1] - p.host_time[0]) * tb.numer / tb.denom / 1e9, worst);
        if (actual > 0.0)
            printf("Core Audio's own estimate: %.4f Hz (%+.3f ppm)\n", actual, (actual / nominal - 1.0) * 1e6);
    }
    return 0;
}
