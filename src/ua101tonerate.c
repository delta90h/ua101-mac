/*
 * ua101tonerate: makes a quiet test tone file, or measures a recorded tone's
 * frequency very precisely. Used by the cable test, where the Mac plays a tone
 * into the UA-101: the tone's frequency in the recording shows how the Mac's
 * audio clock and the unit's clock compare, and the same measurement after
 * ua101fixrate shows whether the rate correction is right.
 *
 * Contains no Apple APIs, so it also runs on recordings copied elsewhere.
 *
 *   ua101tonerate make OUT.wav [--freq HZ] [--db DB] [--seconds S] [--rate HZ] [--ppm X]
 *   ua101tonerate measure IN.wav CHANNEL [--freq HZ]
 *
 * "make" writes a stereo file with the tone on the left only and silence on the
 * right, because the cable goes into a balanced input that hears left minus
 * right. --ppm plays the tone as if from a clock that far off, to test "measure".
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

#include "ua101_stream.h"

#define DEFAULT_FREQ 997.0
#define BLOCK_SECONDS 0.05
#define EDGE_BLOCKS 2
#define MIN_BLOCKS 20

static uint16_t get_u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void usage(void)
{
    printf("Usage:\n"
           "  ua101tonerate make OUT.wav [--freq HZ] [--db DB] [--seconds S] [--rate HZ] [--ppm X]\n"
           "  ua101tonerate measure IN.wav CHANNEL [--freq HZ]\n"
           "make: a stereo file with the tone on the left only, 0.5 s of silence either side.\n"
           "  Defaults: %.0f Hz, -40 dBFS, 5 s, 48000 Hz. Level and length are capped at\n"
           "  %.0f dBFS and %.0f s, like every test tone in this project.\n"
           "measure: CHANNEL counts from 1. Prints the tone's frequency and its offset in ppm.\n",
           DEFAULT_FREQ, UA101_TONE_MAX_DBFS, UA101_TONE_MAX_SECONDS);
}

static int make(int argc, char **argv)
{
    double freq = DEFAULT_FREQ, db = -40.0, seconds = 5.0, rate = 48000.0, ppm = 0.0;
    const double pad = 0.5;
    ua101_tone tone;
    uint8_t header[UA101_WAV_HEADER_BYTES];
    uint64_t frames, pad_frames, n;
    FILE *f;
    int i;

    for (i = 3; i + 1 < argc; i += 2) {
        double v = atof(argv[i + 1]);
        if (strcmp(argv[i], "--freq") == 0)
            freq = v;
        else if (strcmp(argv[i], "--db") == 0)
            db = v;
        else if (strcmp(argv[i], "--seconds") == 0)
            seconds = v;
        else if (strcmp(argv[i], "--rate") == 0)
            rate = v;
        else if (strcmp(argv[i], "--ppm") == 0)
            ppm = v;
        else
            break;
    }
    if (i != argc || !(rate >= 8000.0 && rate <= 192000.0) || !(fabs(ppm) <= 10000.0)) {
        usage();
        return 2;
    }
    if (ua101_tone_init(&tone, rate, db, seconds, freq * (1.0 + ppm * 1e-6),
                        freq * (1.0 + ppm * 1e-6), 0) != 0) {
        printf("Refused: the level must be at most %.0f dBFS, the length at most %.0f s, and the\n"
               "frequency below half the sample rate.\n",
               UA101_TONE_MAX_DBFS, UA101_TONE_MAX_SECONDS);
        return 2;
    }
    pad_frames = (uint64_t)(pad * rate + 0.5);
    frames = tone.length + 2 * pad_frames;
    f = fopen(argv[2], "wb");
    if (f == NULL) {
        printf("Couldn't create %s: %s\n", argv[2], strerror(errno));
        return 2;
    }
    ua101_wav_header(header, 2, (uint32_t)rate, 32, 24, frames * 8);
    fwrite(header, 1, sizeof header, f);
    for (n = 0; n < frames; n++) {
        int32_t s[2] = {0, 0};
        if (n >= pad_frames && n - pad_frames < tone.length)
            s[0] = ua101_tone_word(&tone, 0, n - pad_frames);
        fwrite(s, 4, 2, f);
    }
    if (fclose(f) != 0) {
        printf("Couldn't write %s: %s\n", argv[2], strerror(errno));
        return 2;
    }
    printf("Wrote %s: %.4f Hz on the left only, %.0f dBFS, %.2f s, at %.0f Hz.\n", argv[2],
           tone.freq_hz[0], db, seconds, rate);
    return 0;
}

typedef struct {
    const uint8_t *data;
    uint64_t frames;
    unsigned channels;
    unsigned container; /* bytes per sample */
    uint32_t rate;
} wav;

static int open_wav(const char *path, wav *w)
{
    struct stat info;
    const uint8_t *map;
    size_t size, offset;
    uint16_t format = 0, bits = 0;
    uint64_t data_bytes = 0;
    int fd = open(path, O_RDONLY);

    memset(w, 0, sizeof *w);
    if (fd < 0 || fstat(fd, &info) != 0) {
        printf("Couldn't open %s: %s\n", path, strerror(errno));
        return -1;
    }
    size = (size_t)info.st_size;
    map = size >= 12 ? mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0) : MAP_FAILED;
    close(fd);
    if (map == MAP_FAILED || memcmp(map, "RIFF", 4) != 0 || memcmp(map + 8, "WAVE", 4) != 0) {
        printf("%s isn't a WAV file.\n", path);
        return -1;
    }
    for (offset = 12; offset + 8 <= size;) {
        uint32_t length = get_u32(map + offset + 4);
        if (memcmp(map + offset, "fmt ", 4) == 0 && length >= 16 && offset + 8 + 16 <= size) {
            format = get_u16(map + offset + 8);
            w->channels = get_u16(map + offset + 10);
            w->rate = get_u32(map + offset + 12);
            bits = get_u16(map + offset + 22);
        } else if (memcmp(map + offset, "data", 4) == 0) {
            w->data = map + offset + 8;
            data_bytes = length;
            /* A recording cut short can claim more than the file holds. */
            if (offset + 8 + data_bytes > size)
                data_bytes = size - offset - 8;
            break;
        }
        offset += 8 + (size_t)length + (length & 1);
    }
    if (w->data == NULL || (format != 1 && format != 0xFFFE) ||
        (bits != 16 && bits != 24 && bits != 32) || w->channels == 0 || w->rate == 0) {
        printf("%s: only 16, 24 or 32-bit integer PCM WAV files can be measured.\n", path);
        return -1;
    }
    w->container = bits / 8;
    w->frames = data_bytes / (w->channels * w->container);
    return 0;
}

/* One sample as a fraction of full scale. */
static double sample(const wav *w, uint64_t frame, unsigned ch)
{
    const uint8_t *p = w->data + (frame * w->channels + ch) * w->container;
    switch (w->container) {
    case 2:
        return (int16_t)get_u16(p) / 32768.0;
    case 3:
        return (double)((int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24)) /
               2147483648.0;
    default:
        return (int32_t)get_u32(p) / 2147483648.0;
    }
}

static int measure(int argc, char **argv)
{
    double freq = DEFAULT_FREQ;
    wav w;
    unsigned ch;
    uint64_t block, blocks, b, first = 0, best_first = 0, best_count = 0, count = 0;
    double *re, *im, *amp, wsum = 0.0, peak = 0.0;
    double noise_sum = 0.0;
    uint64_t noise_n = 0;

    if (argc != 4 && !(argc == 6 && strcmp(argv[4], "--freq") == 0)) {
        usage();
        return 2;
    }
    if (argc == 6)
        freq = atof(argv[5]);
    if (open_wav(argv[2], &w) != 0)
        return 2;
    ch = (unsigned)atoi(argv[3]);
    if (ch < 1 || ch > w.channels || !(freq > 0.0 && freq < w.rate / 2.0)) {
        printf("The file has %u channels at %u Hz; pick a channel from 1 to %u and a\n"
               "frequency below %u Hz.\n", w.channels, w.rate, w.channels, w.rate / 2);
        return 2;
    }
    ch--;
    block = (uint64_t)(BLOCK_SECONDS * w.rate + 0.5);
    blocks = w.frames / block;
    if (blocks < MIN_BLOCKS + 2 * EDGE_BLOCKS) {
        printf("The file is too short to measure (%.2f s).\n", (double)w.frames / w.rate);
        return 2;
    }
    re = calloc(blocks, sizeof *re);
    im = calloc(blocks, sizeof *im);
    amp = calloc(blocks, sizeof *amp);
    if (re == NULL || im == NULL || amp == NULL) {
        printf("Out of memory.\n");
        return 2;
    }

    /*
     * Each block is windowed and compared with the expected frequency, using the
     * absolute frame number, so the phase carries on from block to block. If the
     * real frequency is a little different, the phase drifts steadily, and the
     * slope of that drift is the difference. The window keeps the tone's mirror
     * image from leaking into the phase.
     */
    for (b = 0; b < block; b++) {
        double hann = 0.5 - 0.5 * cos(2.0 * M_PI * ((double)b + 0.5) / (double)block);
        wsum += hann;
    }
    for (b = 0; b < blocks; b++) {
        uint64_t k, n0 = b * block;
        double sr = 0.0, si = 0.0;
        for (k = 0; k < block; k++) {
            double hann = 0.5 - 0.5 * cos(2.0 * M_PI * ((double)k + 0.5) / (double)block);
            /* The phase is reduced with fmod so it stays exact in long files. */
            double cycles = fmod((double)(n0 + k) * freq, (double)w.rate) / (double)w.rate;
            double x = sample(&w, n0 + k, ch) * hann;
            sr += x * cos(2.0 * M_PI * cycles);
            si -= x * sin(2.0 * M_PI * cycles);
        }
        re[b] = sr;
        im[b] = si;
        amp[b] = 2.0 * sqrt(sr * sr + si * si) / wsum;
        if (amp[b] > peak)
            peak = amp[b];
    }
    /* The tone is the longest run of blocks above half the loudest. */
    for (b = 0; b <= blocks; b++) {
        if (b < blocks && amp[b] > 0.5 * peak) {
            if (count++ == 0)
                first = b;
        } else {
            if (count > best_count) {
                best_count = count;
                best_first = first;
            }
            count = 0;
        }
    }
    for (b = 0; b < blocks; b++) {
        if (amp[b] < 0.01 * peak) {
            noise_sum += amp[b] * amp[b];
            noise_n++;
        }
    }
    if (peak <= 0.0 || best_count < MIN_BLOCKS + 2 * EDGE_BLOCKS) {
        printf("No steady %.1f Hz tone of at least %.1f s on channel %u.\n", freq,
               (MIN_BLOCKS + 2 * EDGE_BLOCKS) * BLOCK_SECONDS, ch + 1);
        return 1;
    }

    {
        uint64_t lo = best_first + EDGE_BLOCKS, n = best_count - 2 * EDGE_BLOCKS;
        double *phase = calloc(n, sizeof *phase);
        double mean_b = 0.0, mean_p = 0.0, sbb = 0.0, sbp = 0.0, slope, icept, ss = 0.0;
        double worst = 0.0, level = 0.0, se, rel, step_deg;
        uint64_t i;

        if (phase == NULL) {
            printf("Out of memory.\n");
            return 2;
        }
        for (i = 0; i < n; i++) {
            phase[i] = atan2(im[lo + i], re[lo + i]);
            if (i > 0) {
                double d = phase[i] - phase[i - 1];
                d -= 2.0 * M_PI * floor((d + M_PI) / (2.0 * M_PI));
                phase[i] = phase[i - 1] + d;
            }
            mean_b += (double)i;
            mean_p += phase[i];
            level += amp[lo + i];
        }
        mean_b /= (double)n;
        mean_p /= (double)n;
        level /= (double)n;
        for (i = 0; i < n; i++) {
            sbb += ((double)i - mean_b) * ((double)i - mean_b);
            sbp += ((double)i - mean_b) * (phase[i] - mean_p);
        }
        slope = sbp / sbb;
        icept = mean_p - slope * mean_b;
        /*
         * A lost sample is a step in the phase. The fitted line splits a step in
         * half, so it's the jump between neighbouring blocks that shows it.
         */
        for (i = 0; i < n; i++) {
            double r = phase[i] - (icept + slope * (double)i);
            double prev = i > 0 ? phase[i - 1] - (icept + slope * (double)(i - 1)) : r;
            ss += r * r;
            if (fabs(r - prev) > worst)
                worst = fabs(r - prev);
        }
        se = sqrt(ss / (double)(n - 2) / sbb);
        /* Radians per block to a relative frequency difference. */
        rel = slope / (2.0 * M_PI * freq * (double)block / (double)w.rate);
        step_deg = 360.0 * freq / (double)w.rate;

        printf("%s, channel %u: %u channels at %u Hz, %.2f s\n", argv[2], ch + 1, w.channels,
               w.rate, (double)w.frames / w.rate);
        printf("  Tone from %.2f s to %.2f s; measured over %.2f s\n",
               best_first * BLOCK_SECONDS, (best_first + best_count) * BLOCK_SECONDS,
               n * BLOCK_SECONDS);
        printf("  Level %.1f dBFS", 20.0 * log10(level));
        if (noise_n > 0 && noise_sum > 0.0)
            printf("; outside the tone %.1f dBFS at this frequency",
                   20.0 * log10(sqrt(noise_sum / (double)noise_n)));
        else if (noise_n > 0)
            printf("; digital silence outside the tone");
        printf("\n");
        printf("  Frequency %.6f Hz: %+.3f ppm from %.4f Hz (within %.3f)\n",
               freq * (1.0 + rel), rel * 1e6, freq,
               2.0 * se / (2.0 * M_PI * freq * (double)block / (double)w.rate) * 1e6);
        printf("  Largest phase jump between %.0f ms blocks %.2f degrees; one lost sample would\n"
               "  jump %.2f\n",
               BLOCK_SECONDS * 1000.0, worst * 180.0 / M_PI, step_deg);
        if (worst * 180.0 / M_PI > step_deg / 2.0)
            printf("  The phase jumped: samples were lost or added during the tone, so the\n"
                   "  frequency above is unreliable.\n");
        free(phase);
    }
    free(re);
    free(im);
    free(amp);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 3 && strcmp(argv[1], "make") == 0)
        return make(argc, argv);
    if (argc >= 4 && strcmp(argv[1], "measure") == 0)
        return measure(argc, argv);
    usage();
    return 2;
}
