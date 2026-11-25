/*
 * ua101wavcheck: checks a recording made by ua101stream. Reports each channel's
 * level and anything a 24-bit converter couldn't have produced, looks for clicks,
 * and, when the run played test tones, finds them and measures how clean they are.
 * Comparing two tones in one run shows whether any sample was lost in between.
 */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ua101_analysis.h"
#include "ua101_stream.h"

#define MAX_CHANNELS 32
#define MAX_TONES 8
/* Frames either side of the guess at 48 kHz (85 ms); scaled with the rate. */
#define TONE_SEARCH_RADIUS 4096
/*
 * An output whose frequency is this far below the level sent on every capture channel
 * wasn't recorded at all, as at 192 kHz, where the capture has no monitor mix. Measuring
 * it would only measure noise. Through a cable, a tone arrives within a few dB.
 */
#define TONE_RECORDED_DB 30.0

static int64_t search_radius(double rate_hz)
{
    int64_t r = (int64_t)(rate_hz * TONE_SEARCH_RADIUS / 48000.0 + 0.5);
    return r > TONE_SEARCH_RADIUS ? r : TONE_SEARCH_RADIUS;
}

typedef struct {
    double db, seconds, freq0, freq1;
    unsigned long long out_start, wav_guess;
} tone_entry;

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static int load_tones(const char *wav_path, tone_entry *entries)
{
    char path[1024];
    FILE *file;
    int count = 0;

    snprintf(path, sizeof(path), "%s.tones", wav_path);
    file = fopen(path, "r");
    if (file == NULL)
        return 0;
    while (count < MAX_TONES) {
        tone_entry *e = &entries[count];

        if (fscanf(file, "%lf %lf %lf %lf %llu %llu", &e->db, &e->seconds, &e->freq0, &e->freq1,
                   &e->out_start, &e->wav_guess) != 6)
            break;
        count++;
    }
    fclose(file);
    return count;
}

/*
 * Checks the recovered output against what was sent, sample for sample, and
 * returns 1 only when every one of them matches. *where is the tone's position.
 */
static int check_tone_exact(const int32_t *recovered, size_t frames, double rate_hz,
                            const tone_entry *e, int64_t *where, int64_t *latency,
                            uint64_t *compared)
{
    const unsigned outputs[UA101_TONE_CHANNELS] = {0, 1};
    ua101_tone tone;
    unsigned c;
    int ok = 1;

    if (ua101_tone_init(&tone, rate_hz, e->db, e->seconds, e->freq0, e->freq1, e->out_start) != 0)
        return 0;
    *where = ua101_tone_locate(recovered, frames, UA101_MIX_OUTPUTS, &tone, outputs,
                               (int64_t)e->wav_guess, search_radius(rate_hz));
    if (*where < 0) {
        printf("  Couldn't locate it in the recovered output.\n");
        return 0;
    }
    *latency = *where - (int64_t)e->out_start;
    printf("  Bit-exact check against what was sent, at recording frame %lld (offset %lld):\n",
           (long long)*where, (long long)*latency);
    for (c = 0; c < UA101_TONE_CHANNELS; c++) {
        ua101_exact_check check;

        ua101_tone_compare(recovered, frames, c, &tone, (size_t)*where, &check);
        *compared += check.compared;
        printf("    Output %u: %llu of %llu samples differ", c + 1,
               (unsigned long long)check.different, (unsigned long long)check.compared);
        if (check.different != 0) {
            printf(" (first at sample %zu, largest %.1f 24-bit steps)", check.first_different,
                   (double)check.largest / 256.0);
            ok = 0;
        }
        printf("\n");
    }
    return ok;
}

/*
 * Returns 1 when everything measured about this tone is clean, 0 when something isn't,
 * and -1 when neither output was recorded, so there was nothing to measure.
 */
static int check_tone(const int32_t *samples, size_t frames, unsigned channels, double rate_hz,
                      const tone_entry *e, int number, int64_t *latency, int report_verdict)
{
    ua101_tone tone;
    unsigned best[UA101_TONE_CHANNELS] = {0, 0};
    double best_level[UA101_TONE_CHANNELS];
    int recorded[UA101_TONE_CHANNELS], recorded_count = 0;
    unsigned c, ch;
    size_t window, first;
    int64_t where;
    int ok = 1;

    if (ua101_tone_init(&tone, rate_hz, e->db, e->seconds, e->freq0, e->freq1, e->out_start) != 0) {
        printf("  Tone %d: its settings are outside the limits.\n", number);
        return 0;
    }
    /* The steady part only, away from the fades and with room for the search. */
    window = (size_t)(tone.length / 2);
    first = (size_t)e->wav_guess + (size_t)(tone.length / 4);
    if (first + window > frames) {
        printf("  Tone %d: the recording ends before it.\n", number);
        return 0;
    }

    printf("\nTone %d: sent at %.1f dBFS, %.0f Hz on output 1 and %.0f Hz on output 2\n", number,
           e->db, e->freq0, e->freq1);
    printf("  Level by capture channel (dBFS, %.0f Hz / %.0f Hz):\n", e->freq0, e->freq1);
    for (c = 0; c < UA101_TONE_CHANNELS; c++) {
        best_level[c] = -1000.0;
        for (ch = 0; ch < channels; ch++) {
            double level = ua101_goertzel_dbfs(samples, first, window, channels, ch,
                                               tone.freq_hz[c], rate_hz);

            if (level > best_level[c]) {
                best_level[c] = level;
                best[c] = ch;
            }
        }
        recorded[c] = best_level[c] >= e->db - TONE_RECORDED_DB;
        recorded_count += recorded[c];
    }
    for (ch = 0; ch < channels; ch++)
        printf("    ch %2u: %7.1f / %7.1f\n", ch + 1,
               ua101_goertzel_dbfs(samples, first, window, channels, ch, tone.freq_hz[0], rate_hz),
               ua101_goertzel_dbfs(samples, first, window, channels, ch, tone.freq_hz[1], rate_hz));

    if (recorded_count == 0) {
        printf("  Not recorded: no capture channel has either output within %.0f dB of the level\n"
               "  sent, so nothing brings the outputs back here and this tone can't be checked.\n"
               "  A cable from an output to an input would.\n",
               TONE_RECORDED_DB);
        return -1;
    }

    /* An output that wasn't recorded adds only noise to the search, far below the other. */
    where = ua101_tone_locate(samples, frames, channels, &tone, best, (int64_t)e->wav_guess,
                              search_radius(rate_hz));
    if (where < 0) {
        printf("  Couldn't locate it.\n");
        return 0;
    }
    *latency = where - (int64_t)e->out_start;
    printf("  Found at recording frame %lld. Output frame %llu to recording frame: offset %lld "
           "frames\n",
           (long long)where, e->out_start, (long long)*latency);

    for (c = 0; c < UA101_TONE_CHANNELS; c++) {
        ua101_tone_fit fit;

        if (!recorded[c]) {
            printf("  Output %u: not recorded (at most %.1f dBFS, on ch %u), so not checked\n", c + 1,
                   best_level[c], best[c] + 1);
            continue;
        }
        ua101_tone_measure(samples, frames, channels, best[c], &tone, c, (size_t)where, &fit);
        printf("  Output %u on capture ch %u: level %.2f dBFS (%+.2f dB against what was sent), "
               "fractional delay %+.3f frames\n",
               c + 1, best[c] + 1, fit.level_dbfs, fit.level_dbfs - e->db, fit.delay_fraction);
        /* With input noise in this channel, this estimate means little beside the exact check. */
        printf("    Including the inputs' noise: residual %.1f dB, worst 1 ms %.1f dB, largest "
               "phase step %.3f deg (limit %.2f)%s\n",
               fit.residual_db, fit.worst_block_db, fit.max_phase_dev_deg, fit.slip_limit_deg,
               !report_verdict ? "" : fit.clean ? ": clean" : ": NOT CLEAN");
        if (!fit.clean)
            ok = 0;
    }
    return ok;
}

int main(int argc, char **argv)
{
    const char *path;
    struct stat info;
    const uint8_t *map, *data = NULL;
    uint64_t data_bytes = 0;
    size_t size, offset, frames;
    unsigned channels = 0, ch;
    uint32_t rate = 0;
    uint16_t bits = 0, valid = 0;
    tone_entry tones[MAX_TONES];
    size_t tone_start[MAX_TONES], tone_length[MAX_TONES];
    int tone_count, t, fd, problems = 0, exact = 0, not_recorded = 0, reference_tone = 0;
    int64_t first_latency = 0;
    int32_t *recovered = NULL;
    ua101_exact_check probe;
    uint64_t exact_compared = 0;

    if (argc != 2) {
        printf("Usage: ua101wavcheck FILE.wav\n");
        return 2;
    }
    path = argv[1];
    fd = open(path, O_RDONLY);
    if (fd < 0 || fstat(fd, &info) != 0) {
        printf("Couldn't open %s: %s\n", path, strerror(errno));
        return 2;
    }
    size = (size_t)info.st_size;
    if (size < 12) {
        printf("%s is too short to be a WAV file.\n", path);
        return 2;
    }
    map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) {
        printf("Couldn't map %s: %s\n", path, strerror(errno));
        return 2;
    }
    if (memcmp(map, "RIFF", 4) != 0 || memcmp(map + 8, "WAVE", 4) != 0) {
        printf("%s isn't a WAV file.\n", path);
        return 2;
    }
    for (offset = 12; offset + 8 <= size;) {
        uint32_t length = get_u32(map + offset + 4);

        if (memcmp(map + offset, "fmt ", 4) == 0 && offset + 8 + 16 <= size) {
            channels = get_u16(map + offset + 10);
            rate = get_u32(map + offset + 12);
            bits = get_u16(map + offset + 22);
            valid = (length >= 20 && offset + 8 + 20 <= size) ? get_u16(map + offset + 26) : bits;
        } else if (memcmp(map + offset, "data", 4) == 0) {
            data = map + offset + 8;
            data_bytes = length;
            if (offset + 8 + data_bytes > size) {
                printf("The data chunk says it's longer than the file.\n");
                problems++;
                data_bytes = size - offset - 8;
            } else if (offset + 8 + data_bytes < size) {
                printf("The file has %llu bytes after its data chunk.\n",
                       (unsigned long long)(size - offset - 8 - data_bytes));
                problems++;
            }
            break;
        }
        offset += 8 + (size_t)length + (length & 1);
    }
    if (data == NULL || channels == 0 || channels > MAX_CHANNELS || rate == 0 || bits != 32) {
        printf("Expected 32-bit audio with a data chunk.\n");
        return 2;
    }
    if (((uintptr_t)data & 3) != 0) {
        printf("The data isn't aligned to 4 bytes.\n");
        return 2;
    }
    if (data_bytes % ((uint64_t)channels * 4) != 0) {
        printf("The data isn't a whole number of frames.\n");
        problems++;
    }
    frames = (size_t)(data_bytes / ((uint64_t)channels * 4));

    printf("%s\n", path);
    printf("%u channels, %u Hz, %u-bit containers with %u valid bits, %zu frames (%.2f s)\n\n",
           channels, rate, bits, valid, frames, (double)frames / rate);

    /* Finding nothing wrong only counts if there was enough to look at. */
    if (frames < rate) {
        printf("Less than 1 s of audio, which is too little to judge.\n\n");
        problems++;
    }

    tone_count = load_tones(path, tones);

    /* First see whether the monitor mix is usable as a bit-exact reference. */
    if (channels == UA101_MIX_CHANNELS && frames > 0) {
        recovered = malloc(frames * UA101_MIX_OUTPUTS * sizeof(int32_t));
        if (recovered == NULL) {
            printf("Out of memory.\n");
            return 2;
        }
        ua101_mix_recover((const int32_t *)data, frames, recovered);

        /* Measured over the first 0.1 s, away from the tones, whose positions aren't known yet. */
        size_t probe_frames = rate / 10 < frames ? rate / 10 : frames;

        memset(&probe, 0, sizeof(probe));
        ua101_silence_check(recovered, 0, probe_frames, 0, &probe);
        ua101_silence_check(recovered, 0, probe_frames, 1, &probe);
        exact = probe.compared > 0 && probe.different * 100 < probe.compared;
        if (!exact)
            printf("The monitor mix isn't an exact sum of the inputs here, so the bit-exact checks\n"
                   "don't apply. The mixer may not be at unity: center the direct monitor knob and\n"
                   "turn the Soft Control switch off.\n\n");
    } else if (frames > 0) {
        /* At 192 kHz the unit has fewer channels, and where the mix goes isn't measured yet. */
        printf("The bit-exact checks need the %u-channel layout measured at 44.1 and 48 kHz, with\n"
               "the monitor mix on the last two channels. This file has %u, so only the level,\n"
               "click and tone checks below apply.\n\n",
               UA101_MIX_CHANNELS, channels);
    }

    printf("ch      peak       rms        dc   quietest/loudest 100 ms  low byte set  clicks\n");
    for (ch = 0; ch < channels; ch++) {
        ua101_channel_stats st;

        ua101_channel_analyze((const int32_t *)data, frames, channels, ch, (double)rate, &st);
        printf("%2u  %8.1f  %8.1f  %8.1f       %8.1f %8.1f  %12llu  %6llu", ch + 1, st.peak_dbfs,
               st.rms_dbfs, st.dc_dbfs, st.min_block_rms_dbfs, st.max_block_rms_dbfs,
               (unsigned long long)st.low_byte_nonzero, (unsigned long long)st.clicks);
        if (st.clicks > 0)
            printf("  first at %.3f s", (double)st.first_click / rate);
        printf("\n");
        if (st.low_byte_nonzero != 0)
            problems++;
        /*
         * A click on an input may be real sound. When the exact checks hold, they've
         * shown the capture is intact, so it's a note and not a fault in the stream.
         */
        if (st.clicks != 0 && !exact)
            problems++;
    }

    for (t = 0; t < tone_count; t++) {
        int64_t latency = 0, where = 0;
        int result;

        /* The level table is useful either way, but only without exact checks does it decide. */
        result = check_tone((const int32_t *)data, frames, channels, (double)rate, &tones[t], t + 1,
                            &latency, !exact);
        if (result == 0 && !exact)
            problems++;
        if (exact) {
            if (!check_tone_exact(recovered, frames, (double)rate, &tones[t], &where, &latency,
                                  &exact_compared))
                problems++;
            tone_start[t] = (size_t)where;
            tone_length[t] = (size_t)(tones[t].seconds * rate + 0.5);
        } else if (result < 0) {
            /* Not recorded, so it has no position to compare. */
            not_recorded++;
            continue;
        }
        if (reference_tone == 0) {
            first_latency = latency;
            reference_tone = t + 1;
        } else if (latency != first_latency) {
            printf("  The offset changed by %lld frames since tone %d, so frames were lost or "
                   "added.\n",
                   (long long)(latency - first_latency), reference_tone);
            problems++;
        } else {
            printf("  Same offset as tone %d, so no frames were lost or added in between.\n",
                   reference_tone);
        }
    }
    if (not_recorded > 0)
        printf("\n%d of %d tones weren't recorded, so the tone checks didn't cover them. The level,\n"
               "click and low-byte checks still apply.\n",
               not_recorded, tone_count);

    /* Every frame that wasn't part of a tone must recover to exact silence. */
    if (exact) {
        unsigned side;

        printf("\nMonitor mix identity, every frame outside the tones "
               "(outputs recovered as exactly zero):\n");
        for (side = 0; side < UA101_MIX_OUTPUTS; side++) {
            ua101_exact_check check;
            size_t position = 0;

            memset(&check, 0, sizeof(check));
            for (t = 0; t < tone_count; t++) {
                if (tone_start[t] > position)
                    ua101_silence_check(recovered, position, tone_start[t], side, &check);
                if (tone_start[t] + tone_length[t] > position)
                    position = tone_start[t] + tone_length[t];
            }
            if (frames > position)
                ua101_silence_check(recovered, position, frames, side, &check);
            printf("  Output %u: %llu of %llu frames differ", side + 1,
                   (unsigned long long)check.different, (unsigned long long)check.compared);
            if (check.different != 0) {
                printf(" (first at %.3f s, largest %.1f 24-bit steps)",
                       (double)check.first_different / rate, (double)check.largest / 256.0);
                problems++;
            }
            printf("\n");
            exact_compared += check.compared;
        }
        printf("  %llu samples compared in all.\n", (unsigned long long)exact_compared);
        free(recovered);
    }

    printf("\n=== WAV check: %s%s (%d problems) ===\n", problems == 0 ? "CLEAN" : "NOT CLEAN",
           not_recorded == 0 ? "" : not_recorded == tone_count ? ", tones not checked"
                                                                : ", some tones not checked",
           problems);
    munmap((void *)map, size);
    close(fd);
    return problems == 0 ? 0 : 1;
}
