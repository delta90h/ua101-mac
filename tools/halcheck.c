// Silent check through the real Core Audio: opens the UA-101 device, writes digital silence,
// counts input frames. No sound.
#pragma clang diagnostic ignored "-Wunused-parameter"
#include <CoreAudio/CoreAudio.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <stdatomic.h>
#include <stdlib.h>
static _Atomic long cycles, in_frames, out_frames, overloads; static _Atomic int in_ch, out_ch;
static float peak; static float chpk[16]; static long chfirst[16], chlast[16], chloud[16]; static float early[16]; static char chname[16][32]; static double first_t, last_t, first_h, last_h, rate_scalar;
#define MAX_CH 16
// One buffer of interleaved input: nch channels, the first of them channel cbase (0-based) of the
// device. The buffer's own channel count, not a fixed 12: at 192 kHz the UA-101 has 6 inputs, and
// until 2026-11-14 this said i % 12, so every per-channel line there was mislabelled.
static void count_input(const float *p, UInt32 k, UInt32 nch, int cbase, long start_frame) {
    if (nch == 0) return;
    for (UInt32 i = 0; i < k; i++) { float a = fabsf(p[i]); if (a > peak) peak = a; int c = cbase + (int)(i % nch); long f = start_frame + (long)(i / nch);
        if (c >= MAX_CH) continue;
        if (f < 24000) { if (a > early[c]) early[c] = a; } else { if (a > chpk[c]) chpk[c] = a; if (a > 0.001f) { if (!chloud[c]) chfirst[c] = f; chlast[c] = f; chloud[c]++; } } }
}
static OSStatus io(AudioObjectID d, const AudioTimeStamp *now, const AudioBufferList *in, const AudioTimeStamp *it,
                   AudioBufferList *out, const AudioTimeStamp *ot, void *c) {
    long n = 0; int ch = 0;
    for (UInt32 b = 0; in && b < in->mNumberBuffers; b++) {
        const float *p = in->mBuffers[b].mData; UInt32 k = in->mBuffers[b].mDataByteSize / 4, nch = in->mBuffers[b].mNumberChannels;
        if (nch) n = k / nch;
        count_input(p, k, nch, ch, in_frames);
        ch += nch;
    }
    int och = 0; long on = 0;
    for (UInt32 b = 0; out && b < out->mNumberBuffers; b++) {
        memset(out->mBuffers[b].mData, 0, out->mBuffers[b].mDataByteSize);
        och += out->mBuffers[b].mNumberChannels; if (out->mBuffers[b].mNumberChannels) on = out->mBuffers[b].mDataByteSize / 4 / out->mBuffers[b].mNumberChannels;
    }
    if (!cycles) { first_t = it->mSampleTime; first_h = it->mHostTime; }
    last_t = it->mSampleTime + n; last_h = it->mHostTime; rate_scalar = now->mRateScalar;
    in_ch = ch; out_ch = och; in_frames += n; out_frames += on; cycles++;
    return 0;
}
static OSStatus ovl(AudioObjectID o, UInt32 n, const AudioObjectPropertyAddress *a, void *c) { overloads++; return 0; }
// The device's own channel names (kAudioObjectPropertyElementName), if it has them: the driver
// gives them since 2026-11-14. Without them the lines carry numbers only.
static void read_names(AudioObjectID dev) {
    for (int c = 0; c < MAX_CH; c++) {
        AudioObjectPropertyAddress na = { kAudioObjectPropertyElementName, kAudioObjectPropertyScopeInput, (UInt32)c + 1 };
        CFStringRef name = NULL; UInt32 s = sizeof name;
        chname[c][0] = 0;
        if (AudioObjectHasProperty(dev, &na) && AudioObjectGetPropertyData(dev, &na, 0, NULL, &s, &name) == 0 && name) {
            CFStringGetCString(name, chname[c], sizeof chname[c], kCFStringEncodingUTF8); CFRelease(name);
        }
    }
}
static void print_channels(int n) {
    if (n > MAX_CH) n = MAX_CH;
    for (int c = 0; c < n; c++) printf("in %2d%s%s%s: first 0.5 s peak %.6f; after: peak %.6f (%.1f dBFS), samples above -60 dB: %ld (frames %ld..%ld)\n", c + 1, chname[c][0] ? " (" : "", chname[c], chname[c][0] ? ")" : "", early[c], chpk[c], chpk[c] > 0 ? 20 * log10(chpk[c]) : -999.0, chloud[c], chfirst[c], chlast[c]);
}
// Offline check of the per-channel counting (no device, no sound): halcheck --selftest
static int selftest(void) {
    static float buf[12 * 30000]; int bad = 0;
    for (int nch = 6; nch <= 12; nch += 6) {
        memset(chpk, 0, sizeof chpk); memset(chloud, 0, sizeof chloud); memset(early, 0, sizeof early); memset(chfirst, 0, sizeof chfirst); memset(chlast, 0, sizeof chlast); peak = 0;
        memset(buf, 0, sizeof buf);
        int frames = 30000, loud = nch - 1;  // the last channel loud, after 0.5 s
        for (int f = 24000; f < frames; f++) buf[f * nch + loud] = 0.5f;
        count_input(buf, (UInt32)(frames * nch), (UInt32)nch, 0, 0);
        for (int c = 0; c < MAX_CH; c++) {
            int want = c == loud ? frames - 24000 : 0;
            if (chloud[c] != want || (c == loud) != (chpk[c] == 0.5f)) { printf("FAIL: %d channels, channel %d: %ld loud samples, want %d\n", nch, c + 1, chloud[c], want); bad++; }
        }
        print_channels(nch);
    }
    printf(bad ? "selftest: FAILED\n" : "selftest: per-channel lines right at 6 and 12 inputs\n");
    return bad ? 1 : 0;
}
int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--selftest") == 0) return selftest();
    int secs = argc > 1 ? atoi(argv[1]) : 10;
    AudioObjectPropertyAddress a = { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 sz = 0; AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, NULL, &sz);
    AudioObjectID ids[64]; if (sz > sizeof ids) sz = sizeof ids;
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &sz, ids);
    AudioObjectID dev = 0;
    for (UInt32 i = 0; i < sz / sizeof ids[0]; i++) {
        CFStringRef name = NULL; UInt32 s = sizeof name;
        AudioObjectPropertyAddress na = { kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        if (AudioObjectGetPropertyData(ids[i], &na, 0, NULL, &s, &name) == 0 && name) {
            if (CFStringCompare(name, CFSTR("UA-101"), 0) == 0) dev = ids[i];
            CFRelease(name);
        }
    }
    if (!dev) { printf("no UA-101 device\n"); return 1; }
    read_names(dev);
    Float64 rate = 0; sz = sizeof rate;
    AudioObjectPropertyAddress ra = { kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    AudioObjectGetPropertyData(dev, &ra, 0, NULL, &sz, &rate);
    UInt32 v; const struct { const char *n; AudioObjectPropertySelector s; AudioObjectPropertyScope sc; } q[] = {
        {"buffer frames", kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal},
        {"input latency", kAudioDevicePropertyLatency, kAudioObjectPropertyScopeInput},
        {"output latency", kAudioDevicePropertyLatency, kAudioObjectPropertyScopeOutput},
        {"input safety offset", kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeInput},
        {"output safety offset", kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeOutput} };
    printf("device %u, nominal rate %.0f\n", dev, rate);
    for (int i = 0; i < 5; i++) { AudioObjectPropertyAddress pa = { q[i].s, q[i].sc, kAudioObjectPropertyElementMain }; sz = sizeof v; v = 0;
        OSStatus e = AudioObjectGetPropertyData(dev, &pa, 0, NULL, &sz, &v); printf("%s: %u (err %d)\n", q[i].n, v, (int)e); }
    AudioObjectPropertyAddress oa = { kAudioDeviceProcessorOverload, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    AudioObjectAddPropertyListener(dev, &oa, ovl, NULL);
    AudioDeviceIOProcID pid; OSStatus e = AudioDeviceCreateIOProcID(dev, io, NULL, &pid);
    if (e) { printf("CreateIOProcID err %d\n", (int)e); return 1; }
    double t0 = CFAbsoluteTimeGetCurrent(); e = AudioDeviceStart(dev, pid); printf("AudioDeviceStart took %.2f s\n", CFAbsoluteTimeGetCurrent() - t0); if (e) { printf("AudioDeviceStart err %d\n", (int)e); return 1; }
    for (int s = 0; s < secs; s++) { sleep(1); printf("t=%2d s: cycles %ld, in %ld, out %ld, overloads %ld\n", s + 1, (long)cycles, (long)in_frames, (long)out_frames, (long)overloads); fflush(stdout); }
    AudioDeviceStop(dev, pid); AudioDeviceDestroyIOProcID(dev, pid);
    print_channels(in_ch);
    printf("channels: %d in, %d out; input peak %.6f (%.1f dBFS); rate scalar %.9f\n", (int)in_ch, (int)out_ch, peak, peak > 0 ? 20 * log10(peak) : -999.0, rate_scalar);
    return 0;
}
