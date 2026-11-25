/*
 * ua101d: the helper that runs the UA-101's USB side for the audio plug-in (Stage 3).
 *
 * The plug-in runs inside coreaudiod, which can't do this itself, so this program streams with
 * the unit and shares one block of memory with it (src/ua101_shared.h).
 *
 * --serve is how launchd runs it once installed: it answers the plug-in over XPC
 * (src/ua101_service.h) and streams only while the plug-in asks.
 *
 * --watch only watches, for a test by hand: it prints a line when the unit appears on the bus
 * or leaves it, and when the Mac goes to sleep and wakes. It never opens the unit.
 *
 * --self-test runs it as an ordinary program: a thread in this process does what the plug-in's
 * I/O cycle will do, reading capture by sample time from the published clock and writing
 * digital silence to playback, and checks continuity and timing. It installs nothing, needs no
 * password and changes no setting. Nothing but zeros is ever sent to the outputs.
 */
#include <dispatch/dispatch.h>
#include <errno.h>
#include <fcntl.h>
#include <mach/mach.h>
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
#include <time.h>
#include <unistd.h>

#include "ua101_engine.h"
#include "ua101_link.h"
#include "ua101_service.h"
#include "ua101_shared.h"
#include "ua101_stream.h"
#include "ua101_watch.h"

#define DEFAULT_BUFFER_FRAMES 512
/* The safety offsets are times (UA101_INPUT_OFFSET_SECONDS and UA101_OUTPUT_OFFSET_SECONDS in
   src/ua101_shared.h, where the measurements behind them are), turned into frames once the
   unit's rate is known. --input-offset and --output-offset give them in frames, for tests. */
#define START_WAIT_SECONDS 3.0
#define LAG_BINS 8192        /* frames; beyond counts in the last */
#define LATE_BINS 64         /* 0.1 ms each */
#define SETTLE_CYCLES 20     /* misses before this are the start, not the steady state */
#define SETTLE_SECONDS 5.0   /* the clock loops narrow over the first seconds */
#define WAV_RING_BYTES (32u * 1024u * 1024u)
#define MAX_SECONDS 3600.0
#define IDLE_EXIT_SECONDS 30.0 /* --serve: with no plug-in connected */
#define IDLE_CLOSE_SECONDS 30.0 /* --serve: the unit is given back after this long without streaming */

typedef struct {
    int serve;
    double watch_seconds; /* --watch */
    double seconds;
    int capture_only;
    uint32_t buffer;
    uint32_t input_offset; /* frames; in --self-test, set once the rate is known */
    uint32_t output_offset;
    int input_offset_given, output_offset_given;
    unsigned capture_queue, playback_queue;
    const char *wav_path;
    const char *stamp_log_path;
} options;

/* What the stand-in for the plug-in saw. Written by its thread, read after it stops. */
typedef struct {
    uint64_t cycles;
    uint64_t line_unreadable; /* reads of the clock that gave nothing while running */
    uint64_t seed_changes;
    uint64_t backwards;       /* a zero timestamp before the last one */
    uint64_t misaligned;      /* a zero timestamp that isn't a whole number of periods on */
    uint64_t skipped_periods; /* zero timestamps more than one period on: it woke too late */
    double rate_min_ppm, rate_max_ppm, rate_step_max_ppm;
    /* After SETTLE_SECONDS: the spacing of the zero timestamps, one period apart, in ticks. */
    double spacing_min, spacing_max, spacing_sum;
    uint64_t spacing_count;
    uint64_t lag_hist[LAG_BINS]; /* frames between the cycle's sample time and capture's end */
    int64_t lag_min, lag_max;
    uint64_t late_hist[LATE_BINS]; /* how late it woke */
    double late_max_us;
    uint64_t capture_read, capture_real, capture_missing, capture_missing_at_start;
    uint64_t capture_low_bytes;
    uint64_t playback_written;
    uint64_t playback_missing_settled; /* engine's count when the cycle had settled */
    int settled;
} cycle_stats;

static options g_opt;
static ua101_shared *g_shared;
static size_t g_shared_bytes;
static atomic_int g_stop;
static volatile sig_atomic_t g_signal;
static double g_abs_to_seconds;
static cycle_stats g_cycle;
static int g_cycle_started; /* the thread started, so g_cycle is worth reporting */

/* Recording what the cycle read: its thread puts it in a ring and another writes the file. */
static ua101_ring g_wav_ring;
static uint8_t *g_wav_ring_data;
static int g_wav_fd = -1;
static atomic_int g_wav_stop;
static _Atomic uint64_t g_wav_bytes, g_wav_dropped;
static atomic_int g_wav_errno;

static void on_signal(int number)
{
    g_signal = number;
}

static void usage(void)
{
    printf("Usage: ua101d --serve [--input-offset F] [--output-offset F] [--capture-queue N]\n"
           "                       [--playback-queue N]\n"
           "       ua101d --self-test SECONDS [options]\n"
           "       ua101d --watch SECONDS\n"
           "\n"
           "--watch prints when the UA-101 appears on the USB bus or leaves it, and when the Mac\n"
           "sleeps and wakes, as the helper sees them. It never opens the unit.\n"
           "\n"
           "--serve is how launchd runs the helper once installed: it serves the audio plug-in\n"
           "as %s and logs to stderr.\n"
           "\n"
           "--self-test streams with the UA-101 as the helper does, with a thread standing in\n"
           "for the plug-in's I/O cycle. Playback is digital silence. Installs nothing.\n"
           "\n"
           "  --capture-only        never opens the playback interface\n"
           "  --buffer FRAMES       the I/O cycle's length, as an app's buffer size (default %d)\n"
           "  --input-offset F      frames behind sample time that capture is read (default:\n"
           "                        %.0f ms at the unit's rate, 240 frames at 48 kHz)\n"
           "  --output-offset F     frames ahead of sample time that playback is written\n"
           "                        (default: %.1f ms, 1024 frames at 48 kHz)\n"
           "  --capture-queue N     transfers of 2 ms queued for capture (default %d)\n"
           "  --playback-queue N    and for playback (default %d)\n"
           "  --wav PATH            records what the cycle read; keep it out of iCloud Drive\n"
           "  --stamp-log FILE      saves the USB stack's raw bus clock stamps as CSV\n",
           UA101_SERVICE_NAME, DEFAULT_BUFFER_FRAMES, UA101_INPUT_OFFSET_SECONDS * 1000.0,
           UA101_OUTPUT_OFFSET_SECONDS * 1000.0,
           UA101_ENGINE_CAPTURE_QUEUE, UA101_ENGINE_PLAYBACK_QUEUE);
}

static int parse_uint(const char *text, unsigned long lo, unsigned long hi, unsigned long *out)
{
    char *end = NULL;
    unsigned long v;

    errno = 0;
    v = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || v < lo || v > hi)
        return 0;
    *out = v;
    return 1;
}

static int parse_options(int argc, char **argv, options *opt)
{
    int i, self_test = 0;
    unsigned long v;

    opt->buffer = DEFAULT_BUFFER_FRAMES;
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *next = i + 1 < argc ? argv[i + 1] : NULL;

        if (strcmp(a, "--self-test") == 0 && next != NULL) {
            char *end = NULL;

            opt->seconds = strtod(next, &end);
            if (end == next || *end != '\0' || !(opt->seconds >= 1.0) ||
                opt->seconds > MAX_SECONDS) {
                printf("--self-test takes 1 to %.0f seconds.\n", MAX_SECONDS);
                return 0;
            }
            self_test = 1;
            i++;
        } else if (strcmp(a, "--watch") == 0 && next != NULL && argc == 3) {
            char *end = NULL;

            opt->watch_seconds = strtod(next, &end);
            if (end == next || *end != '\0' || !(opt->watch_seconds >= 1.0) ||
                opt->watch_seconds > 86400.0) {
                printf("--watch takes 1 to 86400 seconds.\n");
                return 0;
            }
            return 1;
        } else if (strcmp(a, "--serve") == 0) {
            opt->serve = 1;
        } else if (strcmp(a, "--capture-only") == 0) {
            opt->capture_only = 1;
        } else if (strcmp(a, "--buffer") == 0 && next != NULL && parse_uint(next, 16, 4096, &v)) {
            opt->buffer = (uint32_t)v;
            i++;
        } else if (strcmp(a, "--input-offset") == 0 && next != NULL &&
                   parse_uint(next, 0, 8192, &v)) {
            opt->input_offset = (uint32_t)v;
            opt->input_offset_given = 1;
            i++;
        } else if (strcmp(a, "--output-offset") == 0 && next != NULL &&
                   parse_uint(next, 0, 8192, &v)) {
            opt->output_offset = (uint32_t)v;
            opt->output_offset_given = 1;
            i++;
        } else if (strcmp(a, "--capture-queue") == 0 && next != NULL &&
                   parse_uint(next, 2, 16, &v)) {
            opt->capture_queue = (unsigned)v;
            i++;
        } else if (strcmp(a, "--playback-queue") == 0 && next != NULL &&
                   parse_uint(next, 2, 16, &v)) {
            opt->playback_queue = (unsigned)v;
            i++;
        } else if (strcmp(a, "--stamp-log") == 0 && next != NULL) {
            opt->stamp_log_path = next;
            i++;
        } else if (strcmp(a, "--wav") == 0 && next != NULL) {
            opt->wav_path = next;
            i++;
        } else {
            usage();
            return 0;
        }
    }
    if (self_test == opt->serve ||
        (opt->serve && (opt->capture_only || opt->wav_path != NULL || opt->stamp_log_path != NULL ||
                        opt->buffer != DEFAULT_BUFFER_FRAMES))) {
        usage();
        return 0;
    }
    return 1;
}

/* --watch: what the helper's watcher reports, printed. */
static void watch_say(const char *what)
{
    char when[32];
    time_t now = time(NULL);

    strftime(when, sizeof(when), "%H:%M:%S", localtime(&now));
    printf("%s  %s\n", when, what);
}

static void watch_arrived(void)
{
    watch_say("a UA-101 appeared on the bus");
}

static void watch_left(void)
{
    watch_say("a UA-101 left the bus");
}

static void watch_will_sleep(void)
{
    watch_say("the Mac is going to sleep");
}

static void watch_did_wake(void)
{
    watch_say("the Mac woke");
}

static int watch_mode(double seconds)
{
    static const ua101_watch_events events = {watch_arrived, watch_left, watch_will_sleep,
                                              watch_did_wake};
    dispatch_queue_t queue = dispatch_queue_create("ua101d.watch", DISPATCH_QUEUE_SERIAL);
    __block int units = 0, ok = 0;

    dispatch_sync(queue, ^{
      ok = ua101_watch_start(queue, &events, &units);
    });
    if (!ok) {
        printf("The bus can't be watched.\n");
        return 1;
    }
    printf("Watching for %.0f s. UA-101s on the bus now: %d. Nothing is opened or sent.\n",
           seconds, units);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(seconds * NSEC_PER_SEC)), queue, ^{
      watch_say("done");
      exit(0);
    });
    dispatch_main();
}

static uint64_t seconds_to_ticks(double seconds)
{
    return (uint64_t)(seconds / g_abs_to_seconds);
}

static void set_realtime(double period_s)
{
    thread_time_constraint_policy_data_t policy;

    policy.period = (uint32_t)seconds_to_ticks(period_s);
    policy.computation = (uint32_t)seconds_to_ticks(0.0005);
    policy.constraint = (uint32_t)seconds_to_ticks(period_s);
    policy.preemptible = TRUE;
    thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
                      (thread_policy_t)&policy, THREAD_TIME_CONSTRAINT_POLICY_COUNT);
}

static uint64_t count_low_bytes(const uint8_t *data, size_t bytes)
{
    uint64_t bad = 0;
    size_t i;

    for (i = 0; i + 4 <= bytes; i += 4)
        bad += data[i] != 0;
    return bad;
}

/*
 * What the plug-in's I/O cycle will do: wake at each period of sample time, as Core Audio does
 * from the zero timestamps, read the period of capture before it and write silence a period
 * ahead. Its buffers are made before it starts, so it never allocates.
 */
static void *cycle_thread(void *arg)
{
    cycle_stats *c = &g_cycle;
    ua101_shared *sh = g_shared;
    const uint32_t n = g_opt.buffer;
    const double rate = sh->rate_hz;
    const double nominal_tps = 1.0 / (rate * g_abs_to_seconds);
    uint8_t *in = arg, *out;
    ua101_clock_line line;
    uint32_t seed = 0;
    double last_zero_sample = -1.0, last_rate_ppm = 0.0;
    uint64_t last_zero_host = 0, cycle_index = 0;
    int have_rate = 0;

    out = in + (size_t)n * sh->capture_frame_bytes;
    pthread_setname_np("ua101d cycle");
    set_realtime((double)n / rate);
    c->rate_min_ppm = INFINITY;
    c->rate_max_ppm = -INFINITY;
    c->spacing_min = INFINITY;
    c->spacing_max = -INFINITY;
    c->lag_min = INT64_MAX;
    c->lag_max = INT64_MIN;

    while (!atomic_load(&g_stop)) {
        double zero_sample, now_sample, target;
        uint64_t zero_host, now, wake;
        uint64_t first, missed;
        int64_t lag;
        ua101_frame_ring_miss miss;

        if (!ua101_shared_read(&sh->clock, &line)) {
            c->line_unreadable++;
            usleep(1000);
            continue;
        }
        if (seed == 0)
            seed = line.seed;
        else if (line.seed != seed) {
            c->seed_changes++;
            seed = line.seed;
        }

        /* Sleeps until the next period of sample time begins. */
        now = mach_absolute_time();
        target = (floor(ua101_line_sample(&line, (double)now) / n) + 1.0) * n;
        wake = (uint64_t)llround(ua101_line_host(&line, target));
        if (wake > now)
            mach_wait_until(wake);
        if (atomic_load(&g_stop))
            break;
        now = mach_absolute_time();
        if (!ua101_shared_read(&sh->clock, &line)) {
            c->line_unreadable++;
            continue;
        }
        cycle_index++;
        c->cycles++;
        now_sample = ua101_line_sample(&line, (double)now);
        {
            double late_us = (double)(now > wake ? now - wake : 0) * g_abs_to_seconds * 1e6;
            unsigned bin = (unsigned)(late_us / 100.0);

            c->late_hist[bin < LATE_BINS ? bin : LATE_BINS - 1]++;
            if (late_us > c->late_max_us)
                c->late_max_us = late_us;
        }

        /* The zero timestamp Core Audio would be given now. */
        if (ua101_zero_timestamp(&line, now, n, &zero_sample, &zero_host)) {
            if (last_zero_sample >= 0.0) {
                double steps = (zero_sample - last_zero_sample) / n;

                if (zero_sample < last_zero_sample || zero_host < last_zero_host)
                    c->backwards++;
                else if (steps != floor(steps))
                    c->misaligned++;
                else if (steps > 1.0)
                    c->skipped_periods++;
                if (steps >= 1.0 && zero_host > last_zero_host) {
                    /* The rate these two stamps give, against the nominal rate. */
                    double ppm = ((double)(zero_host - last_zero_host) / (steps * n) /
                                      nominal_tps -
                                  1.0) *
                                 -1e6;

                    if (ppm < c->rate_min_ppm)
                        c->rate_min_ppm = ppm;
                    if (ppm > c->rate_max_ppm)
                        c->rate_max_ppm = ppm;
                    if (have_rate && fabs(ppm - last_rate_ppm) > c->rate_step_max_ppm)
                        c->rate_step_max_ppm = fabs(ppm - last_rate_ppm);
                    last_rate_ppm = ppm;
                    have_rate = 1;
                    if (steps == 1.0 && zero_sample > SETTLE_SECONDS * rate) {
                        double spacing = (double)(zero_host - last_zero_host);

                        if (spacing < c->spacing_min)
                            c->spacing_min = spacing;
                        if (spacing > c->spacing_max)
                            c->spacing_max = spacing;
                        c->spacing_sum += spacing;
                        c->spacing_count++;
                    }
                }
            }
            last_zero_sample = zero_sample;
            last_zero_host = zero_host;
        }

        /* Capture: how far behind this period's start the newest frame is, and the read. */
        lag = (int64_t)llround(target) - (int64_t)ua101_frame_ring_end(&sh->capture);
        if (lag < c->lag_min)
            c->lag_min = lag;
        if (lag > c->lag_max)
            c->lag_max = lag;
        c->lag_hist[lag < 0 ? 0 : lag >= LAG_BINS ? LAG_BINS - 1 : lag]++;

        first = (uint64_t)llround(target) >= n + g_opt.input_offset
                    ? (uint64_t)llround(target) - n - g_opt.input_offset
                    : 0;
        c->capture_real += ua101_frame_ring_read(&sh->capture, first, in, n, &miss);
        c->capture_read += n;
        missed = miss.not_yet + miss.overwritten;
        if (cycle_index <= SETTLE_CYCLES)
            c->capture_missing_at_start += missed;
        else
            c->capture_missing += missed;
        c->capture_low_bytes += count_low_bytes(in, (size_t)n * sh->capture_frame_bytes);
        if (g_wav_fd >= 0 && cycle_index > SETTLE_CYCLES) {
            if (ua101_ring_write(&g_wav_ring, in, (size_t)n * sh->capture_frame_bytes))
                g_wav_bytes += (uint64_t)n * sh->capture_frame_bytes;
            else
                g_wav_dropped += (uint64_t)n * sh->capture_frame_bytes;
        }

        /* Playback: silence, a period ahead and output_offset further. */
        if (!g_opt.capture_only) {
            ua101_frame_ring_write(&sh->playback, (uint64_t)llround(target) + g_opt.output_offset,
                                   out, n);
            c->playback_written += n;
            /* By now its first writes have been read, so later misses are the steady state. */
            if (cycle_index == SETTLE_CYCLES) {
                c->playback_missing_settled =
                    atomic_load_explicit(&sh->stats.playback_missing, memory_order_relaxed);
                c->settled = 1;
            }
        }
        (void)now_sample;
    }
    return NULL;
}

static void *wav_thread(void *arg)
{
    (void)arg;
    pthread_setname_np("ua101d wav");
    for (;;) {
        const uint8_t *data = NULL;
        size_t available = ua101_ring_peek(&g_wav_ring, &data);
        ssize_t written;

        if (available == 0) {
            if (atomic_load(&g_wav_stop))
                break;
            usleep(5000);
            continue;
        }
        written = write(g_wav_fd, data, available);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            g_wav_errno = errno;
            ua101_ring_consume(&g_wav_ring, available);
            continue;
        }
        ua101_ring_consume(&g_wav_ring, (size_t)written);
    }
    return NULL;
}

static int open_wav(const ua101_shared *sh)
{
    uint8_t header[UA101_WAV_HEADER_BYTES];

    if (g_opt.seconds * sh->rate_hz * sh->capture_frame_bytes > (double)UA101_WAV_MAX_DATA_BYTES) {
        printf("That's too long for one WAV file.\n");
        return 0;
    }
    g_wav_fd = open(g_opt.wav_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (g_wav_fd < 0) {
        printf("Couldn't create %s: %s\n", g_opt.wav_path, strerror(errno));
        return 0;
    }
    ua101_wav_header(header, (uint16_t)sh->capture_channels, (uint32_t)sh->rate_hz, 32, 24, 0);
    if (write(g_wav_fd, header, sizeof(header)) != (ssize_t)sizeof(header)) {
        printf("Couldn't write to %s: %s\n", g_opt.wav_path, strerror(errno));
        return 0;
    }
    g_wav_ring_data = malloc(WAV_RING_BYTES);
    if (g_wav_ring_data == NULL)
        return 0;
    memset(g_wav_ring_data, 0, WAV_RING_BYTES);
    mlock(g_wav_ring_data, WAV_RING_BYTES);
    ua101_ring_init(&g_wav_ring, g_wav_ring_data, WAV_RING_BYTES);
    return 1;
}

static void close_wav(const ua101_shared *sh)
{
    uint8_t header[UA101_WAV_HEADER_BYTES];
    off_t end;

    if (g_wav_fd < 0)
        return;
    end = lseek(g_wav_fd, 0, SEEK_END);
    ua101_wav_header(header, (uint16_t)sh->capture_channels, (uint32_t)sh->rate_hz, 32, 24,
                     end > UA101_WAV_HEADER_BYTES ? (uint64_t)end - UA101_WAV_HEADER_BYTES : 0);
    if (pwrite(g_wav_fd, header, sizeof(header), 0) != (ssize_t)sizeof(header))
        g_wav_errno = errno;
    close(g_wav_fd);
    g_wav_fd = -1;
}

static void write_stamp_log(const ua101_engine *engine)
{
    const ua101_bus_stamp *stamps;
    uint32_t count, i;
    FILE *f;

    stamps = ua101_engine_stamps(engine, &count);
    if (g_opt.stamp_log_path == NULL || stamps == NULL)
        return;
    f = fopen(g_opt.stamp_log_path, "w");
    if (f == NULL) {
        printf("Couldn't create %s: %s\n", g_opt.stamp_log_path, strerror(errno));
        return;
    }
    fprintf(f, "# ticks are mach_absolute_time units, %.9f s each\nbus,stamp,asked\n",
            g_abs_to_seconds);
    for (i = 0; i < count; i++)
        fprintf(f, "%llu,%llu,%llu\n", (unsigned long long)stamps[i].bus,
                (unsigned long long)stamps[i].stamp, (unsigned long long)stamps[i].asked);
    fclose(f);
    printf("Stamp log: %u stamps in %s\n", count, g_opt.stamp_log_path);
}

/* The smallest lag that covers this share of the cycles. */
static int64_t lag_percentile(const cycle_stats *c, double share)
{
    uint64_t total = 0, seen = 0;
    int64_t i;

    for (i = 0; i < LAG_BINS; i++)
        total += c->lag_hist[i];
    for (i = 0; i < LAG_BINS; i++) {
        seen += c->lag_hist[i];
        if ((double)seen >= share * (double)total)
            return i;
    }
    return LAG_BINS - 1;
}

static double nominal_ticks_per_period(void)
{
    return (double)g_opt.buffer / (g_shared->rate_hz * g_abs_to_seconds);
}

static int report(ua101_engine *engine, double elapsed)
{
    const cycle_stats *c = &g_cycle;
    ua101_shared *sh = g_shared;
    double ms_per_frame = 1000.0 / sh->rate_hz;
    uint64_t engine_errors = ua101_engine_errors(engine);
    uint64_t playback_missing = atomic_load(&sh->stats.playback_missing);
    uint64_t playback_steady =
        c->settled && playback_missing >= c->playback_missing_settled
            ? playback_missing - c->playback_missing_settled
            : playback_missing;
    int64_t lead_min = atomic_load(&sh->stats.playback_lead_min);
    int64_t lead_max = atomic_load(&sh->stats.playback_lead_max);
    int timestamps_ok, capture_ok, playback_ok, pass;
    unsigned b;

    printf("\n");
    ua101_engine_report(engine, stdout);

    printf("\nThe stand-in for the plug-in's I/O cycle, %u frames (%.2f ms):\n", g_opt.buffer,
           g_opt.buffer * ms_per_frame);
    printf("  %llu cycles in %.1f s; clock unreadable %llu times; seed changed %llu times\n",
           (unsigned long long)c->cycles, elapsed, (unsigned long long)c->line_unreadable,
           (unsigned long long)c->seed_changes);
    printf("  Zero timestamps: %llu backwards, %llu not a whole period on, %llu skipped a "
           "period\n",
           (unsigned long long)c->backwards, (unsigned long long)c->misaligned,
           (unsigned long long)c->skipped_periods);
    if (c->rate_min_ppm <= c->rate_max_ppm)
        printf("  Rate they give against %.0f Hz: %+.2f to %+.2f ppm; biggest change between "
               "two cycles %.3f ppm\n",
               sh->rate_hz, c->rate_min_ppm, c->rate_max_ppm, c->rate_step_max_ppm);
    if (c->spacing_count > 0) {
        double mean = c->spacing_sum / (double)c->spacing_count;

        printf("  After %.0f s, one period apart: %.3f ms on average (%+.1f ppm), each within "
               "%+.2f to %+.2f us of that\n",
               SETTLE_SECONDS, mean * g_abs_to_seconds * 1e3,
               (nominal_ticks_per_period() / mean - 1.0) * 1e6,
               (c->spacing_min - mean) * g_abs_to_seconds * 1e6,
               (c->spacing_max - mean) * g_abs_to_seconds * 1e6);
    }
    printf("  Woke late by:");
    for (b = 0; b < LATE_BINS; b++)
        if (c->late_hist[b] != 0)
            printf(" %.1f-%.1f ms: %llu;", b * 0.1, (b + 1) * 0.1,
                   (unsigned long long)c->late_hist[b]);
    printf(" most %.0f us\n", c->late_max_us);
    if (c->lag_min <= c->lag_max)
        printf("  Capture's newest frame behind each period's start: %lld to %lld frames "
               "(%.2f to %.2f ms); 99%% within %lld, 99.9%% within %lld\n",
               (long long)c->lag_min, (long long)c->lag_max, c->lag_min * ms_per_frame,
               c->lag_max * ms_per_frame, (long long)lag_percentile(c, 0.99),
               (long long)lag_percentile(c, 0.999));
    printf("  Capture read %u frames behind: %llu frames, %llu missing after the first %d "
           "cycles (%llu in them)\n",
           g_opt.input_offset, (unsigned long long)c->capture_read,
           (unsigned long long)c->capture_missing, SETTLE_CYCLES,
           (unsigned long long)c->capture_missing_at_start);
    if (c->lag_max > 0)
        printf("    An input offset of %lld frames (%.2f ms) would have missed nothing.\n",
               (long long)c->lag_max, c->lag_max * ms_per_frame);
    printf("  Samples with a nonzero low byte: %llu\n", (unsigned long long)c->capture_low_bytes);
    if (!g_opt.capture_only) {
        printf("  Playback written %u frames ahead: %llu frames; the engine sent %llu as "
               "silence because they weren't there yet, %llu after the first %d cycles\n",
               g_opt.output_offset, (unsigned long long)c->playback_written,
               (unsigned long long)playback_missing, (unsigned long long)playback_steady,
               SETTLE_CYCLES);
        if (lead_min <= lead_max)
            printf("    The engine read playback %lld to %lld frames ahead of sample time, so the "
                   "output offset needs to be more than %lld frames plus how late a cycle "
                   "wakes.\n",
                   (long long)lead_min, (long long)lead_max, (long long)lead_max);
    }
    if (g_wav_fd >= 0 || g_opt.wav_path != NULL)
        printf("  Recording: %llu bytes to %s, %llu dropped%s\n",
               (unsigned long long)g_wav_bytes, g_opt.wav_path,
               (unsigned long long)g_wav_dropped, g_wav_errno != 0 ? ", write errors" : "");

    timestamps_ok = c->cycles > 0 && c->seed_changes == 0 && c->backwards == 0 &&
                    c->misaligned == 0;
    capture_ok = c->capture_missing == 0 && c->capture_real > 0;
    playback_ok = g_opt.capture_only || (c->settled && playback_steady == 0);
    pass = engine_errors == 0 && timestamps_ok && capture_ok && playback_ok &&
           g_wav_dropped == 0 && g_wav_errno == 0;
    printf("\n=== Self-test result: %s ===\n", pass ? "PASS" : "FAIL");
    if (!pass) {
        if (engine_errors != 0)
            printf("  The engine had %llu errors.\n", (unsigned long long)engine_errors);
        if (!timestamps_ok)
            printf("  The timestamps weren't right.\n");
        if (!capture_ok)
            printf("  Capture missed frames in the steady state.\n");
        if (!playback_ok)
            printf("  Playback missed frames in the steady state.\n");
    }
    return pass ? 0 : 1;
}

int main(int argc, char **argv)
{
    mach_timebase_info_data_t timebase;
    ua101_engine_options eopt;
    ua101_engine_format format;
    ua101_engine *engine;
    struct sigaction action;
    char why[256];
    uint8_t *cycle_buffers = NULL;
    pthread_t cycle, wav;
    int cycle_running = 0, wav_running = 0, exit_code = 1;
    uint64_t started, ended;
    double elapsed = 0.0, next_print = 10.0;
    long page = sysconf(_SC_PAGESIZE);
    uint64_t ring_frames;

    setvbuf(stdout, NULL, _IOLBF, 0);
    if (!parse_options(argc, argv, &g_opt))
        return 2;
    if (g_opt.watch_seconds > 0.0)
        return watch_mode(g_opt.watch_seconds);
    if (g_opt.serve) {
        ua101_service_options sopt;

        memset(&sopt, 0, sizeof(sopt));
        sopt.input_offset = g_opt.input_offset_given ? (int64_t)g_opt.input_offset : -1;
        sopt.output_offset = g_opt.output_offset_given ? (int64_t)g_opt.output_offset : -1;
        sopt.capture_queue = g_opt.capture_queue;
        sopt.playback_queue = g_opt.playback_queue;
        sopt.idle_exit_seconds = IDLE_EXIT_SECONDS;
        sopt.idle_close_seconds = IDLE_CLOSE_SECONDS;
        sopt.watch = 1;
        sopt.allow_uid = -1;
        sopt.log = stderr;
        return ua101_service_run(&sopt);
    }
    mach_timebase_info(&timebase);
    g_abs_to_seconds = (double)timebase.numer / (double)timebase.denom / 1e9;

    printf("=== UA-101 helper self-test ===\n");
    printf("Mode: %s; %.0f s; an I/O cycle of %u frames\n\nSetup:\n",
           g_opt.capture_only ? "capture only" : "capture and silent playback", g_opt.seconds,
           g_opt.buffer);

    memset(&action, 0, sizeof(action));
    action.sa_handler = on_signal;
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);

    memset(&eopt, 0, sizeof(eopt));
    eopt.capture_only = g_opt.capture_only;
    eopt.capture_queue = g_opt.capture_queue;
    eopt.playback_queue = g_opt.playback_queue;
    engine = ua101_engine_open(&eopt, &format, stdout, why, sizeof(why));
    if (engine == NULL) {
        printf("  Couldn't start: %s.\n", why);
        return 1;
    }

    if (!g_opt.input_offset_given)
        g_opt.input_offset = ua101_offset_frames(UA101_INPUT_OFFSET_SECONDS, format.rate_hz);
    if (!g_opt.output_offset_given)
        g_opt.output_offset = ua101_offset_frames(UA101_OUTPUT_OFFSET_SECONDS, format.rate_hz);
    printf("  Capture read %u frames behind (%.2f ms)", g_opt.input_offset,
           g_opt.input_offset * 1000.0 / format.rate_hz);
    if (!g_opt.capture_only)
        printf(", playback written %u ahead (%.2f ms)", g_opt.output_offset,
               g_opt.output_offset * 1000.0 / format.rate_hz);
    printf("\n");

    /* As the helper will give the plug-in: one block, here mapped once, in this process. */
    ring_frames = ua101_shared_ring_frames(format.rate_hz);
    g_shared_bytes = ua101_shared_bytes(format.capture_channels * 4,
                                        (format.playback_channels ? format.playback_channels : 1) * 4,
                                        ring_frames, (size_t)page);
    g_shared = g_shared_bytes != 0 ? mmap(NULL, g_shared_bytes, PROT_READ | PROT_WRITE,
                                          MAP_ANON | MAP_SHARED, -1, 0)
                                   : MAP_FAILED;
    if (g_shared == MAP_FAILED ||
        !ua101_shared_init(g_shared, g_shared_bytes, format.rate_hz, format.capture_channels,
                           format.playback_channels ? format.playback_channels : 1, ring_frames,
                           (size_t)page)) {
        printf("  Couldn't set up the shared block.\n");
        g_shared = NULL;
        goto done;
    }
    mlock(g_shared, g_shared_bytes);
    printf("  Shared block: %zu KB, rings of %llu frames (%.2f s), %.0f Hz, %u in, %u out\n",
           g_shared_bytes / 1024, (unsigned long long)ring_frames, ring_frames / format.rate_hz,
           format.rate_hz, format.capture_channels, format.playback_channels);

    cycle_buffers = calloc(1, (size_t)g_opt.buffer *
                                  (g_shared->capture_frame_bytes + g_shared->playback_frame_bytes));
    if (cycle_buffers == NULL || (g_opt.wav_path != NULL && !open_wav(g_shared)))
        goto done;
    mlock(cycle_buffers,
          (size_t)g_opt.buffer * (g_shared->capture_frame_bytes + g_shared->playback_frame_bytes));
    if (g_wav_fd >= 0) {
        if (pthread_create(&wav, NULL, wav_thread, NULL) != 0)
            goto done;
        wav_running = 1;
    }

    if (g_opt.stamp_log_path != NULL &&
        !ua101_engine_keep_stamps(engine, (uint32_t)(g_opt.seconds * 1000.0 / 2.0) + 64)) {
        printf("  Out of memory.\n");
        goto done;
    }

    printf("\nStreaming:\n");
    if (!ua101_engine_start(engine, g_shared, g_shared_bytes)) {
        printf("  The engine couldn't start.\n");
        goto done;
    }
    started = mach_absolute_time();

    /* The plug-in starts its I/O once the clock is published. */
    while (atomic_load(&g_shared->state) == UA101_STATE_STARTING &&
           ua101_engine_failure(engine) == NULL && g_signal == 0 &&
           (double)(mach_absolute_time() - started) * g_abs_to_seconds < START_WAIT_SECONDS)
        usleep(10000);
    if (atomic_load(&g_shared->state) != UA101_STATE_RUNNING) {
        printf("  The clock wasn't published within %.0f s.\n", START_WAIT_SECONDS);
    } else {
        printf("  Clock published after %.2f s\n",
               (double)(mach_absolute_time() - started) * g_abs_to_seconds);
        if (pthread_create(&cycle, NULL, cycle_thread, cycle_buffers) == 0) {
            cycle_running = 1;
            g_cycle_started = 1;
        }
    }

    while (cycle_running) {
        usleep(50000);
        elapsed = (double)(mach_absolute_time() - started) * g_abs_to_seconds;
        if (g_signal != 0 || ua101_engine_failure(engine) != NULL || elapsed >= g_opt.seconds)
            break;
        if (elapsed >= next_print) {
            printf("  %5.0f s: %llu frames captured, %llu gaps, %llu resyncs, %llu errors\n",
                   elapsed, (unsigned long long)atomic_load(&g_shared->stats.capture_frames),
                   (unsigned long long)atomic_load(&g_shared->stats.capture_gaps),
                   (unsigned long long)atomic_load(&g_shared->stats.resyncs),
                   (unsigned long long)atomic_load(&g_shared->stats.errors));
            next_print += 10.0;
        }
    }
    if (g_signal != 0)
        printf("  Stopping: signal %d\n", (int)g_signal);
    atomic_store(&g_stop, 1);
    if (cycle_running)
        pthread_join(cycle, NULL);
    cycle_running = 0;
    ended = mach_absolute_time();
    ua101_engine_stop(engine);
    if (wav_running) {
        atomic_store(&g_wav_stop, 1);
        pthread_join(wav, NULL);
        wav_running = 0;
    }
    close_wav(g_shared);
    write_stamp_log(engine);
    if (g_cycle_started)
        exit_code = report(engine, (double)(ended - started) * g_abs_to_seconds);
    else
        ua101_engine_report(engine, stdout);
    if (g_signal != 0)
        exit_code = 1;

done:
    atomic_store(&g_stop, 1);
    if (cycle_running)
        pthread_join(cycle, NULL);
    if (wav_running) {
        atomic_store(&g_wav_stop, 1);
        pthread_join(wav, NULL);
    }
    ua101_engine_close(engine);
    if (g_shared != NULL)
        munmap(g_shared, g_shared_bytes);
    free(cycle_buffers);
    return exit_code;
}
