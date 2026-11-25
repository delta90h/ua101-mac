/*
 * ua101linktest: the whole path from Core Audio to the unit, in one process, installing nothing.
 * The helper's service (src/ua101_service.c) listens on an anonymous XPC endpoint instead of a
 * launchd name; the plug-in (src/plugin/ua101_plugin.c) connects to it through its real XPC link
 * (src/plugin/ua101_link_xpc.c) and maps the real shared block; a thread here stands in for Core
 * Audio's I/O cycle, timing itself only from the plug-in's zero timestamps, as Core Audio does.
 *
 * It streams for SECONDS, stops, starts again and streams 5 s more, then gives back the unit.
 * Playback is digital silence: only zeros are ever written. Needs the unit; changes no setting.
 * Rate correction is on, as the plug-in's default, unless --no-correction is given (the stand-in
 * for Core Audio's storage then holds it off).
 *
 * Usage: ua101linktest [SECONDS] [--no-correction]    (default 60)
 */
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/plugin/ua101_link_xpc.h"
#include "../src/ua101_service.h"
#include "../src/ua101_shared.h"

void *UA101_Create(CFAllocatorRef allocator, CFUUIDRef requested_type);
void ua101_plugin_counts(uint64_t *input_missing, uint64_t *output_clipped);
int ua101_plugin_correction(double steps[2], uint64_t resets[2], double errors[2]);
void ua101_plugin_steering(double *offset, uint64_t *starts);

#define BUFFER 512 /* frames per I/O cycle, as an app's buffer size */
#define MAX_CH 16
#define SETTLE_SECONDS 5.0
#define PPM_LIMIT 100.0
#define RESTART_SECONDS 5.0

static double g_tick_s;
static AudioServerPlugInDriverRef D;
static atomic_int g_stop;
static atomic_int g_alive_notes, g_restart_requests;
static int g_correction = 1;

/* Where the helper's clock line puts each cycle on the unit's timeline, in run 1 after the
   settling time: sample time (ours) and unit frame. */
#define MAX_POINTS 400000
static double g_point_x[MAX_POINTS], g_point_u[MAX_POINTS];
/* With each point: the capture step that cycle, in ppm, and how far the helper's line was from
   the smoothed copy the step followed, in frames. */
static double g_point_step[MAX_POINTS], g_point_off[MAX_POINTS];
static size_t g_points;

static uint64_t ticks(double seconds)
{
    return (uint64_t)(seconds / g_tick_s);
}

static void on_signal(int n)
{
    (void)n;
    atomic_store(&g_stop, 1);
}

/* --- Core Audio's side of the host interface: only what this test needs to see. --- */

static OSStatus host_changed(AudioServerPlugInHostRef h, AudioObjectID o, UInt32 n,
                             const AudioObjectPropertyAddress *a)
{
    UInt32 i;

    (void)h;
    (void)o;
    for (i = 0; i < n; i++)
        if (a[i].mSelector == kAudioDevicePropertyDeviceIsAlive)
            atomic_fetch_add(&g_alive_notes, 1);
    return 0;
}

static OSStatus host_copy(AudioServerPlugInHostRef h, CFStringRef k, CFPropertyListRef *v)
{
    (void)h;
    *v = NULL;
    /* Only the rate correction setting is kept, and only when it's off. */
    if (g_correction || CFStringCompare(k, CFSTR("rate correction"), 0) != kCFCompareEqualTo)
        return kAudioHardwareUnknownPropertyError;
    *v = CFRetain(kCFBooleanFalse);
    return 0;
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

static OSStatus host_request(AudioServerPlugInHostRef h, AudioObjectID d, UInt64 a, void *i)
{
    (void)h;
    (void)d;
    (void)a;
    (void)i;
    atomic_fetch_add(&g_restart_requests, 1);
    return 0;
}

static AudioServerPlugInHostInterface g_host = {host_changed, host_copy, host_write, host_delete,
                                                host_request};

static int get(AudioObjectID o, AudioObjectPropertySelector s, AudioObjectPropertyScope sc,
               void *out, UInt32 avail)
{
    AudioObjectPropertyAddress a = {s, sc, kAudioObjectPropertyElementMain};
    UInt32 got = 0;

    return (*D)->GetPropertyData(D, o, 0, &a, 0, NULL, avail, &got, out) == 0 ? (int)got : -1;
}

/* --- One run of the stand-in I/O cycle. --- */

typedef struct {
    AudioObjectID dev, in, out;
    double rate;
    uint32_t in_ch, out_ch, in_off, out_off, period;
    double seconds;

    uint64_t cycles, zts_fail, zts_new, not_whole, backwards, skipped, seed_changes;
    UInt64 first_seed;
    double late_max_us, late_sum_us;
    uint64_t late_over_2ms;
    double ppm_min, ppm_max, ppm_sum;
    uint64_t ppm_n;
    uint64_t low_bytes[MAX_CH];
    float peak[MAX_CH];
    uint64_t missing_settle, missing_end, play_missing_settle, play_missing_end;
    uint64_t helper_errors_settle, helper_errors_end;
    int settled;
    int corrected; /* rate correction, as it stood at the end of the run */
    double steps[2], errors[2];
    uint64_t resets[2];
    /* Each cycle's step after the settling time, in ppm: capture, playback. */
    uint64_t step_n;
    double step_mean[2], step_m2[2], step_lo[2], step_hi[2];
    /* The helper's line less the smoothed copy the steps follow, over the same cycles. */
    double steer_sum2, steer_worst;
    uint64_t steer_starts;
    /* How far behind each cycle's start the newest captured frame was, by the helper's line,
       after the settling time; and the cycles where that was further than the cycle reads. */
    double behind_min, behind_max;
    uint64_t behind_n, behind_short;
    int record_line; /* fill g_point_x and g_point_u */
    uint32_t line_seed; /* only points from one stream of the helper's */
    /* The plug-in's own time in each cycle's two I/O calls. */
    double work_sum_us, work_max_us;
    uint64_t work_over_1ms;
} run;

static void set_realtime(double period_s)
{
    thread_time_constraint_policy_data_t policy;

    policy.period = (uint32_t)ticks(period_s);
    /* Half the period, as generous as Core Audio is with its I/O thread: a thread that keeps
       using more than it asked for can be demoted. 0.5 ms was enough before rate correction. */
    policy.computation = (uint32_t)ticks(period_s / 2.0);
    policy.constraint = (uint32_t)ticks(period_s);
    policy.preemptible = TRUE;
    thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
                      (thread_policy_t)&policy, THREAD_TIME_CONSTRAINT_POLICY_COUNT);
}

static void helper_counts(uint64_t *play_missing, uint64_t *errors)
{
    const ua101_shared *b = ua101_service_block();

    *play_missing = b != NULL ? atomic_load(&b->stats.playback_missing) : 0;
    *errors = b != NULL ? atomic_load(&b->stats.errors) : 0;
}

static void *io_thread(void *arg)
{
    run *r = arg;
    static float inbuf[BUFFER * MAX_CH];
    static const float zeros[BUFFER * MAX_CH];
    const double nominal = 1.0 / (r->rate * g_tick_s); /* ticks per frame */
    double tpf = nominal, s_ref, h_ref;
    Float64 s;
    UInt64 h, seed, seed_ref;
    uint64_t start = mach_absolute_time(), end = start + ticks(r->seconds);
    uint64_t settle = start + ticks(SETTLE_SECONDS), clipped;
    int64_t x;
    uint32_t i, c;

    set_realtime(BUFFER / r->rate);
    if ((*D)->GetZeroTimeStamp(D, r->dev, 1, &s, &h, &seed) != 0) {
        r->zts_fail++;
        return NULL;
    }
    s_ref = s;
    h_ref = (double)h;
    seed_ref = r->first_seed = seed;
    /* With correction, zero timestamps lie on a line of exactly `nominal` from our sample 0. */
    const double origin = (double)h - s * nominal;
    /* The first cycle: the next whole buffer after now. */
    x = (int64_t)(s + ((double)mach_absolute_time() - h_ref) / tpf);
    x = (x / BUFFER + 1) * BUFFER;

    while (!atomic_load(&g_stop)) {
        uint64_t wake = (uint64_t)(h_ref + ((double)x - s_ref) * tpf), now;
        AudioServerPlugInIOCycleInfo cycle;
        double late;

        if (wake > end)
            break;
        mach_wait_until(wake);
        now = mach_absolute_time();
        late = now > wake ? (double)(now - wake) * g_tick_s * 1e6 : 0.0;
        r->late_sum_us += late;
        if (late > r->late_max_us)
            r->late_max_us = late;
        r->late_over_2ms += late > 2000.0;

        /* As Core Audio does: a zero timestamp each cycle, its rate from consecutive ones. */
        if ((*D)->GetZeroTimeStamp(D, r->dev, 1, &s, &h, &seed) != 0) {
            r->zts_fail++;
        } else if (seed != seed_ref) {
            r->seed_changes++;
            seed_ref = seed;
            s_ref = s;
            h_ref = (double)h;
        } else if (s != s_ref) {
            r->zts_new++;
            if (fmod(s, (double)r->period) != 0.0)
                r->not_whole++;
            if (s < s_ref) {
                r->backwards++;
            } else {
                double measured = ((double)h - h_ref) / (s - s_ref);
                double ppm = (nominal / measured - 1.0) * 1e6;

                r->skipped += s - s_ref > 1.5 * r->period;
                if (now > settle) {
                    if (r->ppm_n == 0 || ppm < r->ppm_min)
                        r->ppm_min = ppm;
                    if (r->ppm_n == 0 || ppm > r->ppm_max)
                        r->ppm_max = ppm;
                    r->ppm_sum += ppm;
                    r->ppm_n++;
                }
                tpf = measured;
            }
            s_ref = s;
            h_ref = (double)h;
        }

        if (now > settle && ua101_plugin_correction(r->steps, r->resets, r->errors)) {
            /* The cycle is about to read capture up to in_off frames behind its start. Was the
               newest frame the helper had written at least that recent? */
            const ua101_shared *b = ua101_service_block();
            ua101_clock_line line;

            if (b != NULL && ua101_shared_read(&b->clock, &line)) {
                double unit_now = (double)line.sample +
                                  ((origin - line.host) + (double)x * nominal) /
                                      line.ticks_per_sample;
                double behind = unit_now + 1.0 -
                                (double)ua101_frame_ring_end((ua101_frame_ring *)&b->capture);

                if (r->behind_n == 0 || behind < r->behind_min)
                    r->behind_min = behind;
                if (r->behind_n == 0 || behind > r->behind_max)
                    r->behind_max = behind;
                r->behind_short += behind > (double)r->in_off;
                r->behind_n++;
            }
        }

        memset(&cycle, 0, sizeof(cycle));
        cycle.mInputTime.mSampleTime = (Float64)(x - BUFFER - (int64_t)r->in_off);
        cycle.mOutputTime.mSampleTime = (Float64)(x + (int64_t)r->out_off);
        cycle.mInputTime.mFlags = cycle.mOutputTime.mFlags = kAudioTimeStampSampleTimeValid;
        {
            uint64_t w0 = mach_absolute_time();
            double us;

            (*D)->DoIOOperation(D, r->dev, r->in, 1, kAudioServerPlugInIOOperationReadInput,
                                BUFFER, &cycle, inbuf, NULL);
            (*D)->DoIOOperation(D, r->dev, r->out, 1, kAudioServerPlugInIOOperationWriteMix,
                                BUFFER, &cycle, (void *)zeros, NULL);
            us = (double)(mach_absolute_time() - w0) * g_tick_s * 1e6;
            r->work_sum_us += us;
            if (us > r->work_max_us)
                r->work_max_us = us;
            r->work_over_1ms += us > 1000.0;
        }
        if (now > settle && r->record_line && g_points < MAX_POINTS) {
            const ua101_shared *b = ua101_service_block();
            ua101_clock_line line;

            /* From the line's own point, so the large host times cancel exactly. */
            if (b != NULL && ua101_shared_read(&b->clock, &line) &&
                (g_points == 0 || line.seed == r->line_seed)) {
                r->line_seed = line.seed;
                g_point_x[g_points] = (double)x;
                g_point_u[g_points] = (double)line.sample +
                                      ((origin - line.host) + (double)x * nominal) /
                                          line.ticks_per_sample;
                {
                    double st[2], er[2], off = 0;
                    uint64_t rs[2], starts;

                    ua101_plugin_correction(st, rs, er);
                    ua101_plugin_steering(&off, &starts);
                    g_point_step[g_points] = (st[0] - 1.0) * 1e6;
                    g_point_off[g_points] = off;
                }
                g_points++;
            }
        }
        if (now > settle) {
            double st[2], er[2];
            uint64_t rs[2];
            int j;

            if (ua101_plugin_correction(st, rs, er)) {
                double off;

                ua101_plugin_steering(&off, &r->steer_starts);
                r->steer_sum2 += off * off;
                if (fabs(off) > r->steer_worst)
                    r->steer_worst = fabs(off);
                r->step_n++;
                for (j = 0; j < 2; j++) {
                    double ppm = (st[j] - 1.0) * 1e6, delta = ppm - r->step_mean[j];

                    r->step_mean[j] += delta / (double)r->step_n;
                    r->step_m2[j] += delta * (ppm - r->step_mean[j]);
                    if (r->step_n == 1 || ppm < r->step_lo[j])
                        r->step_lo[j] = ppm;
                    if (r->step_n == 1 || ppm > r->step_hi[j])
                        r->step_hi[j] = ppm;
                }
            }
        }

        /* What came in: 24 bits each, so x * 2^31 has a zero low byte. */
        for (i = 0; i < BUFFER; i++)
            for (c = 0; c < r->in_ch; c++) {
                float v = inbuf[i * r->in_ch + c];
                int64_t bits = (int64_t)((double)v * 2147483648.0);

                r->low_bytes[c] += (bits & 0xFF) != 0;
                if (fabsf(v) > r->peak[c])
                    r->peak[c] = fabsf(v);
            }
        r->cycles++;
        if (!r->settled && now > settle) {
            r->settled = 1;
            ua101_plugin_counts(&r->missing_settle, &clipped);
            helper_counts(&r->play_missing_settle, &r->helper_errors_settle);
        }
        x += BUFFER;
    }
    ua101_plugin_counts(&r->missing_end, &clipped);
    helper_counts(&r->play_missing_end, &r->helper_errors_end);
    r->corrected = ua101_plugin_correction(r->steps, r->resets, r->errors);
    return NULL;
}

/* The rms of v around a smooth curve through it (a quadratic, by least squares): what is left
   once slow drift is taken out. */
static double rms_about_curve(const double *v, size_t n)
{
    double a[3][4] = {{0}}, c[3], sum2 = 0;
    size_t k;
    int i, j, m;

    if (n < 8)
        return 0.0;
    for (k = 0; k < n; k++) {
        double t = 2.0 * (double)k / (double)(n - 1) - 1.0, p[3] = {1.0, t, t * t};

        for (i = 0; i < 3; i++) {
            for (j = 0; j < 3; j++)
                a[i][j] += p[i] * p[j];
            a[i][3] += p[i] * v[k];
        }
    }
    for (i = 0; i < 3; i++)
        for (m = i + 1; m < 3; m++) {
            double f = a[m][i] / a[i][i];

            for (j = i; j < 4; j++)
                a[m][j] -= f * a[i][j];
        }
    for (i = 2; i >= 0; i--) {
        c[i] = a[i][3];
        for (j = i + 1; j < 3; j++)
            c[i] -= a[i][j] * c[j];
        c[i] /= a[i][i];
    }
    for (k = 0; k < n; k++) {
        double t = 2.0 * (double)k / (double)(n - 1) - 1.0, e = v[k] - c[0] - c[1] * t - c[2] * t * t;

        sum2 += e * e;
    }
    return sqrt(sum2 / (double)n);
}

/*
 * How far the helper's line wanders from a smooth curve (a quadratic through the whole run, for
 * the crystal's slow drift): with correction on, the audio's timing follows the line each cycle,
 * so this is the wobble it gets. Then the same for the line averaged over 0.1, 1 and 5 s, which is
 * about what a correction smoothed that much would leave.
 */
static void report_line_wobble(const char *name, double rate)
{
    static double fit[MAX_POINTS];
    static const double spans[3] = {0.1, 1.0, 5.0};
    const size_t n = g_points;
    double xm = 0, xs, a[3][4] = {{0}}, c[3], sum2 = 0, worst = 0;
    size_t k, w;
    int i, j, m;

    if (n < 1000)
        return;
    for (k = 0; k < n; k++)
        xm += g_point_x[k];
    xm /= (double)n;
    xs = (g_point_x[n - 1] - g_point_x[0]) / 2.0;
    /* Least squares in a centred, scaled x, by Gaussian elimination on the normal equations. */
    for (k = 0; k < n; k++) {
        double t = (g_point_x[k] - xm) / xs, p[3] = {1.0, t, t * t};

        for (i = 0; i < 3; i++) {
            for (j = 0; j < 3; j++)
                a[i][j] += p[i] * p[j];
            a[i][3] += p[i] * (g_point_u[k] - g_point_u[0]);
        }
    }
    for (i = 0; i < 3; i++)
        for (m = i + 1; m < 3; m++) {
            double f = a[m][i] / a[i][i];

            for (j = i; j < 4; j++)
                a[m][j] -= f * a[i][j];
        }
    for (i = 2; i >= 0; i--) {
        c[i] = a[i][3];
        for (j = i + 1; j < 3; j++)
            c[i] -= a[i][j] * c[j];
        c[i] /= a[i][i];
    }
    for (k = 0; k < n; k++) {
        double t = (g_point_x[k] - xm) / xs, e;

        fit[k] = g_point_u[0] + c[0] + c[1] * t + c[2] * t * t;
        e = g_point_u[k] - fit[k];
        sum2 += e * e;
        if (fabs(e) > worst)
            worst = fabs(e);
    }
    printf("%s: the helper's line around a smooth curve, %zu cycles: %.4f frames rms (%.0f ns), "
           "%.4f at worst (%.0f ns)\n",
           name, n, sqrt(sum2 / (double)n), sqrt(sum2 / (double)n) / rate * 1e9, worst,
           worst / rate * 1e9);
    {
        /* What the steps would have been landing on the helper's own line every cycle, as the
           plug-in did before it smoothed the line: the "before" for the steps reported above. */
        double mean = 0, m2 = 0, move2 = 0;
        size_t count = 0;

        for (k = 1; k < n; k++) {
            double dx = g_point_x[k] - g_point_x[k - 1], step, delta;

            if (dx != (double)BUFFER)
                continue; /* a point was skipped */
            step = (g_point_u[k] - g_point_u[k - 1]) / dx * 1e6;
            count++;
            delta = step - mean;
            mean += delta / (double)count;
            m2 += delta * (step - mean);
        }
        if (count > 1) {
            move2 = sqrt(m2 / (double)(count - 1));
            printf("%s:   followed exactly, the step would have wobbled %.2f ppm rms: line moves "
                   "of %.4f frames rms a cycle, %.0f ns\n",
                   name, move2, move2 * 1e-6 * BUFFER, move2 * 1e-6 * BUFFER / rate * 1e9);
        }
        /* The unit's rate against the Mac's drifts when the Mac warms or cools, and both kinds
           of step follow that. Around that slow drift, what is left is the wobble. */
        {
            static double followed[MAX_POINTS];

            for (k = 1; k < n; k++)
                followed[k - 1] = (g_point_u[k] - g_point_u[k - 1]) /
                                  (g_point_x[k] - g_point_x[k - 1]) * 1e6;
            printf("%s:   around their own slow drift: followed exactly %.2f ppm rms, the steps "
                   "taken %.2f ppm rms\n",
                   name, rms_about_curve(followed, n - 1), rms_about_curve(g_point_step, n));
        }
    }
    if (getenv("UA101_TRACE") != NULL) {
        /* For a closer look afterwards: each cycle's start, the helper's line there, the
           capture step and the smoothed line's distance from the helper's. */
        FILE *f = fopen(getenv("UA101_TRACE"), "w");

        if (f != NULL) {
            fprintf(f, "sample_time,unit_frame_by_helper_line,capture_step_ppm,offset_frames\n");
            for (k = 0; k < n; k++)
                fprintf(f, "%.0f,%.6f,%.4f,%.6f\n", g_point_x[k], g_point_u[k], g_point_step[k],
                        g_point_off[k]);
            fclose(f);
        }
    }
    for (i = 0; i < 3; i++) {
        size_t width = (size_t)(spans[i] * rate / BUFFER + 0.5), count = 0;
        double run_sum = 0, s2 = 0, worst_avg = 0;

        if (width < 2 || n < 2 * width)
            continue;
        for (k = 0; k < width; k++)
            run_sum += g_point_u[k] - fit[k];
        for (w = 0; w + width <= n; w++) {
            double e = run_sum / (double)width;

            s2 += e * e;
            if (fabs(e) > worst_avg)
                worst_avg = fabs(e);
            count++;
            if (w + width < n)
                run_sum += (g_point_u[w + width] - fit[w + width]) - (g_point_u[w] - fit[w]);
        }
        printf("%s:   averaged over %.1f s: %.4f frames rms (%.0f ns), %.4f at worst (%.0f ns)\n",
               name, spans[i], sqrt(s2 / (double)count), sqrt(s2 / (double)count) / rate * 1e9,
               worst_avg, worst_avg / rate * 1e9);
    }
}

/* Starts I/O, runs the cycle for `seconds`, stops. Returns 1 if it all held. */
static int stream(run *r, const char *name)
{
    pthread_t t;
    uint64_t t0 = mach_absolute_time();
    double start_s;
    UInt32 running = 0;
    int ok = 1;
    uint32_t c;

    if ((*D)->StartIO(D, r->dev, 1) != 0) {
        printf("%s: StartIO failed\n", name);
        return 0;
    }
    start_s = (double)(mach_absolute_time() - t0) * g_tick_s;
    get(r->dev, kAudioDevicePropertyDeviceIsRunning, kAudioObjectPropertyScopeGlobal, &running,
        sizeof(running));
    printf("%s: started in %.0f ms; running %u\n", name, start_s * 1e3, running);
    pthread_create(&t, NULL, io_thread, r);
    pthread_join(t, NULL);
    if ((*D)->StopIO(D, r->dev, 1) != 0) {
        printf("%s: StopIO failed\n", name);
        ok = 0;
    }
    get(r->dev, kAudioDevicePropertyDeviceIsRunning, kAudioObjectPropertyScopeGlobal, &running,
        sizeof(running));

    printf("%s: %llu cycles of %d frames; woke late by %.0f us on average, %.0f us at most, "
           "%llu times over 2 ms\n",
           name, r->cycles, BUFFER, r->cycles ? r->late_sum_us / (double)r->cycles : 0.0,
           r->late_max_us, r->late_over_2ms);
    printf("%s: zero timestamps: %llu new, %llu not whole periods, %llu backwards, %llu skipped, "
           "%llu failed, %llu seed changes\n",
           name, r->zts_new, r->not_whole, r->backwards, r->skipped, r->zts_fail,
           r->seed_changes);
    if (r->ppm_n > 0)
        printf("%s: rate from zero timestamps after %.0f s: %+.2f ppm on average "
               "(%+.2f to %+.2f, %llu periods; limit %.0f)\n",
               name, SETTLE_SECONDS, r->ppm_sum / (double)r->ppm_n, r->ppm_min, r->ppm_max,
               r->ppm_n, PPM_LIMIT);
    printf("%s: the plug-in's I/O work per cycle: %.0f us on average, %.0f us at most, %llu times "
           "over 1 ms\n",
           name, r->cycles ? r->work_sum_us / (double)r->cycles : 0.0, r->work_max_us,
           r->work_over_1ms);
    printf("%s: capture frames missing: %llu in all, %llu after %.0f s\n", name, r->missing_end,
           r->missing_end - r->missing_settle, SETTLE_SECONDS);
    /* Signed: if the helper's stream started over on a new block, its counts did too. */
    printf("%s: playback frames the helper sent as silence because the plug-in was late: "
           "%lld after %.0f s; transfer errors after %.0f s: %lld\n",
           name, (long long)(r->play_missing_end - r->play_missing_settle), SETTLE_SECONDS,
           SETTLE_SECONDS, (long long)(r->helper_errors_end - r->helper_errors_settle));
    printf("%s: input peaks, dBFS:", name);
    for (c = 0; c < r->in_ch; c++) {
        if (r->peak[c] > 0)
            printf(" %u:%.0f", c + 1, 20.0 * log10(r->peak[c]));
        else
            printf(" %u:-inf", c + 1);
    }
    printf("\n");
    for (c = 0; c < r->in_ch; c++)
        if (r->low_bytes[c] != 0) {
            printf("%s: channel %u had %llu samples with a nonzero low byte\n", name, c + 1,
                   r->low_bytes[c]);
            ok = 0;
        }

    if (r->behind_n > 0) {
        printf("%s: capture's newest frame was %.0f to %.0f frames behind each cycle's start "
               "(the cycle reads up to %u behind); further behind than that %llu times in %llu\n",
               name, r->behind_min, r->behind_max, r->in_off, r->behind_short, r->behind_n);
    }
    if (r->corrected) {
        printf("%s: rate correction: started %llu and %llu times (want 1 each); largest line "
               "move at a cycle's start %.4f and %.4f frames, settling included\n",
               name, r->resets[0], r->resets[1], r->errors[0], r->errors[1]);
        if (r->step_n > 1) {
            /* A step off the mean by p ppm moves the timing by p * BUFFER millionths of a frame
               in that cycle: the line's own wobble, which the correction follows. */
            double sd0 = sqrt(r->step_m2[0] / (double)(r->step_n - 1));
            double sd1 = sqrt(r->step_m2[1] / (double)(r->step_n - 1));

            printf("%s: steps after %.0f s, %llu cycles: capture %+.3f ppm on average (rms %.2f, "
                   "%+.1f to %+.1f), playback %+.3f (rms %.2f, %+.1f to %+.1f); per cycle, "
                   "the timing moves by %.4f frames rms, %.0f ns\n",
                   name, SETTLE_SECONDS, r->step_n, r->step_mean[0], sd0, r->step_lo[0],
                   r->step_hi[0], r->step_mean[1], sd1, r->step_lo[1], r->step_hi[1],
                   sd0 * 1e-6 * BUFFER, sd0 * 1e-6 * BUFFER / r->rate * 1e9);
            /* The steps follow a smoothed copy of the helper's line, not the line itself. */
            printf("%s: steering: the smoothed line started %llu time(s) (want 1); after %.0f s "
                   "the helper's line was %.4f frames rms from it (%.0f ns), %.4f at most (%.0f "
                   "ns)\n",
                   name, r->steer_starts, SETTLE_SECONDS,
                   sqrt(r->steer_sum2 / (double)r->step_n),
                   sqrt(r->steer_sum2 / (double)r->step_n) / r->rate * 1e9, r->steer_worst,
                   r->steer_worst / r->rate * 1e9);
            if (r->steer_starts != 1)
                ok = 0;
        }
    } else
        printf("%s: rate correction off: the bit-exact path\n", name);
    if (r->record_line)
        report_line_wobble(name, r->rate);
    printf("%s: running after StopIO: %u\n", name, running);

    /* The user's split verdict (2026-11-07): streaming passes or fails on its own; the clock
       is reported separately, against the same 100 ppm limit. */
    ok &= running == 0 && r->zts_fail == 0 && r->not_whole == 0 && r->backwards == 0 &&
          r->skipped == 0 && r->seed_changes == 0 && r->cycles > 0;
    ok &= r->settled && r->missing_end == r->missing_settle &&
          r->play_missing_end == r->play_missing_settle &&
          r->helper_errors_end == r->helper_errors_settle;
    /* With correction, a resampler starting again mid-run means its place was lost. */
    ok &= r->corrected == g_correction && (!r->corrected || (r->resets[0] == 1 && r->resets[1] == 1));
    printf("%s: streaming %s\n", name, ok ? "PASS" : "FAIL");
    return ok;
}

int main(int argc, char **argv)
{
    mach_timebase_info_data_t tb;
    ua101_service_options so;
    xpc_endpoint_t ep;
    AudioObjectPropertyAddress list = {kAudioPlugInPropertyDeviceList,
                                       kAudioObjectPropertyScopeGlobal,
                                       kAudioObjectPropertyElementMain};
    AudioStreamBasicDescription f;
    UInt32 size = 0, u;
    run r1, r2;
    double seconds = 60.0, waited = 0.0;
    int ok, i, bad_args = 0;
    uint64_t t0;
    UInt32 latency_in = 0, latency_out = 0;

    setvbuf(stdout, NULL, _IOLBF, 0);
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-correction") == 0)
            g_correction = 0;
        else if (i == 1)
            seconds = atof(argv[i]);
        else
            bad_args = 1;
    }
    if (bad_args || !(seconds >= 10.0 && seconds <= 3600.0)) {
        printf("Usage: ua101linktest [SECONDS] [--no-correction]   (10 to 3600, default 60)\n");
        return 2;
    }
    mach_timebase_info(&tb);
    g_tick_s = (double)tb.numer / (double)tb.denom / 1e9;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    printf("=== UA-101 plug-in through the helper, in one process ===\n");
    printf("%.0f s, then %.0f s after a restart; I/O cycle of %d frames; playback is zeros; "
           "rate correction %s\n\n",
           seconds, RESTART_SECONDS, BUFFER, g_correction ? "on" : "off");

    memset(&so, 0, sizeof(so));
    so.input_offset = -1; /* as ua101d: 5 ms and 21.3 ms at the unit's rate */
    so.output_offset = -1;
    so.allow_uid = (int)getuid();
    so.log = stdout;
    ep = ua101_service_start_anonymous(&so);
    if (ep == NULL) {
        printf("FAIL: the service didn't start\n");
        return 1;
    }
    ua101_link_xpc_use_endpoint(ep);

    D = UA101_Create(NULL, kAudioServerPlugInTypeUUID);
    t0 = mach_absolute_time();
    if (D == NULL || (*D)->Initialize(D, &g_host) != 0) {
        printf("FAIL: the plug-in didn't initialize\n");
        return 1;
    }
    /* The device appears once the helper has opened the unit. */
    for (i = 0; i < 200; i++) {
        size = 0;
        (*D)->GetPropertyDataSize(D, kAudioObjectPlugInObject, 0, &list, 0, NULL, &size);
        if (size > 0)
            break;
        usleep(50000);
    }
    waited = (double)(mach_absolute_time() - t0) * g_tick_s;
    if (size == 0) {
        printf("FAIL: no device after %.1f s (is the unit on and free?)\n", waited);
        ua101_service_shutdown();
        return 1;
    }
    memset(&r1, 0, sizeof(r1));
    get(kAudioObjectPlugInObject, kAudioPlugInPropertyDeviceList, kAudioObjectPropertyScopeGlobal,
        &r1.dev, sizeof(r1.dev));
    get(r1.dev, kAudioDevicePropertyStreams, kAudioObjectPropertyScopeInput, &r1.in,
        sizeof(r1.in));
    get(r1.dev, kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput, &r1.out,
        sizeof(r1.out));
    get(r1.dev, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, &r1.rate,
        sizeof(r1.rate));
    get(r1.in, kAudioStreamPropertyVirtualFormat, kAudioObjectPropertyScopeGlobal, &f, sizeof(f));
    r1.in_ch = f.mChannelsPerFrame;
    get(r1.out, kAudioStreamPropertyVirtualFormat, kAudioObjectPropertyScopeGlobal, &f, sizeof(f));
    r1.out_ch = f.mChannelsPerFrame;
    get(r1.dev, kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeInput, &u, sizeof(u));
    r1.in_off = u;
    get(r1.dev, kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeOutput, &u, sizeof(u));
    r1.out_off = u;
    get(r1.dev, kAudioDevicePropertyZeroTimeStampPeriod, kAudioObjectPropertyScopeGlobal, &u,
        sizeof(u));
    r1.period = u;
    get(r1.dev, kAudioDevicePropertyLatency, kAudioObjectPropertyScopeInput, &latency_in,
        sizeof(latency_in));
    get(r1.dev, kAudioDevicePropertyLatency, kAudioObjectPropertyScopeOutput, &latency_out,
        sizeof(latency_out));
    printf("\nDevice %u appeared after %.2f s: %.0f Hz, %u in, %u out; safety offsets %u in, "
           "%u out; latency %u in, %u out; zero timestamp period %u\n\n",
           r1.dev, waited, r1.rate, r1.in_ch, r1.out_ch, r1.in_off, r1.out_off, latency_in,
           latency_out, r1.period);
    if (r1.in_ch == 0 || r1.in_ch > MAX_CH || r1.out_ch == 0 || r1.out_ch > MAX_CH) {
        printf("FAIL: channel counts out of range\n");
        ua101_service_shutdown();
        return 1;
    }

    r1.seconds = seconds;
    r1.record_line = g_correction;
    ok = stream(&r1, "run 1");
    r2 = r1;
    r2.record_line = 0;
    memset(&r2.cycles, 0, sizeof(r2) - offsetof(run, cycles));
    r2.seconds = RESTART_SECONDS + SETTLE_SECONDS;
    if (!atomic_load(&g_stop)) {
        printf("\n");
        ok &= stream(&r2, "run 2");
        if (r2.first_seed == r1.first_seed) {
            printf("run 2: FAIL: the restart kept the old seed\n");
            ok = 0;
        }
    }
    printf("\nDeviceIsAlive notices: %d (want 0); restart requests: %d (want 0)\n",
           atomic_load(&g_alive_notes), atomic_load(&g_restart_requests));
    ok &= atomic_load(&g_alive_notes) == 0 && atomic_load(&g_restart_requests) == 0;
    ua101_service_shutdown();
    printf("\nStreaming: %s\n", ok ? "PASS" : "FAIL");
    if (r1.corrected) {
        /* Apps see the nominal rate by construction; the unit's own clock shows in the step:
           unit frames per frame of ours. */
        double unit_ppm = r1.step_n > 0 ? r1.step_mean[0] : (r1.steps[0] - 1.0) * 1e6;

        printf("Clock: apps see %.0f Hz (zero timestamps %+.2f ppm, rate correction on); the "
               "unit itself runs %+.1f ppm; %s the %.0f ppm limit\n",
               r1.rate, r1.ppm_n > 0 ? r1.ppm_sum / (double)r1.ppm_n : 0.0, unit_ppm,
               fabs(unit_ppm) <= PPM_LIMIT ? "within" : "outside", PPM_LIMIT);
    } else if (r1.ppm_n > 0) {
        double worst = fabs(r1.ppm_min) > fabs(r1.ppm_max) ? r1.ppm_min : r1.ppm_max;

        printf("Clock: the unit's rate from the zero timestamps is %+.1f ppm from %.0f Hz at "
               "worst; %s the %.0f ppm limit\n",
               worst, r1.rate, fabs(worst) <= PPM_LIMIT ? "within" : "outside", PPM_LIMIT);
    }
    return ok ? 0 : 1;
}
