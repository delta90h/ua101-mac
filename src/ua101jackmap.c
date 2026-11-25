/*
 * ua101jackmap: reads recordings made by `ua101stream --jack-map` with loop cables
 * from the outputs to the inputs, where every output plays a frequency of its own.
 *
 *   ua101jackmap levels FILE.wav
 *       every output's level on every capture channel, and where each one arrived
 *   ua101jackmap solve STRAIGHT.wav SHIFTED.wav SENS1.wav [SENS2.wav]
 *       works out which jack each channel is, from these runs:
 *         STRAIGHT  OUTPUT n to INPUT n, with SENS 1 and 2 all the way down
 *         SHIFTED   OUTPUT n to INPUT n+1, and OUTPUT 8 to INPUT 1
 *         SENS1     the straight cables, with SENS 1 turned up
 *         SENS2     the straight cables, with SENS 1 back down and SENS 2 turned up
 *
 * Why so many runs: a straight loop shows the output side and the input side only
 * together, so a swap made the same way on both sides would look perfect. Shifting
 * every cable by one jack separates the two sides, up to which input is which. The
 * SENS knobs act on INPUT 1 and INPUT 2 alone, and tie the answer to the printed
 * labels. SENS2 is optional; with it, the answer is checked a second way.
 *
 * Contains no Apple APIs.
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
#define MAX_BURSTS 4
#define ANALOG_JACKS 8
#define FULL_SCALE 2147483648.0
/* An output this close to the level sent, on some input, arrived there. */
#define RECORDED_DB 30.0
/* Any other input this close to the best means the output reached two inputs. */
#define SECOND_DB 20.0
/* Turning a SENS knob up has to raise one channel this much, and move no other this much. */
#define ANCHOR_RISE_DB 3.0
#define ANCHOR_STILL_DB 1.0

typedef struct {
    double db, seconds;
    unsigned long long out_start, wav_guess;
    unsigned outputs;
    double freq[UA101_JACK_OUTPUTS];
} burst;

typedef struct {
    const char *path;
    const int32_t *data;
    size_t frames;
    unsigned channels;
    unsigned inputs; /* capture channels that are inputs: all but a monitor mix */
    uint32_t rate;
    burst first;
    double level[UA101_JACK_OUTPUTS][MAX_CHANNELS]; /* dBFS, from the first burst */
    int route[UA101_JACK_OUTPUTS];                  /* capture channel from 0, or -1 */
    int problems;
} recording;

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

/* Reads the first burst from FILE.wav.jack. Returns the number of bursts, or 0. */
static int load_bursts(const char *wav_path, burst *first)
{
    char path[1024], line[1024];
    FILE *file;
    int count = 0;

    snprintf(path, sizeof(path), "%s.jack", wav_path);
    file = fopen(path, "r");
    if (file == NULL)
        return 0;
    while (count < MAX_BURSTS && fgets(line, sizeof(line), file) != NULL) {
        burst b;
        char *p = line, *end;
        unsigned i;
        int used;

        if (sscanf(p, "%lf %lf %llu %llu %u%n", &b.db, &b.seconds, &b.out_start, &b.wav_guess,
                   &b.outputs, &used) != 5 ||
            b.outputs == 0 || b.outputs > UA101_JACK_OUTPUTS)
            break;
        p += used;
        for (i = 0; i < b.outputs; i++) {
            b.freq[i] = strtod(p, &end);
            if (end == p)
                break;
            p = end;
        }
        if (i < b.outputs)
            break;
        if (count == 0)
            *first = b;
        count++;
    }
    fclose(file);
    return count;
}

/* Amplitude of one frequency on one channel, Hann-windowed, in dBFS. */
static double level_dbfs(const recording *r, size_t start, size_t count, unsigned ch,
                         double freq)
{
    double re = 0.0, im = 0.0, wsum = 0.0, amp;
    size_t n;

    for (n = 0; n < count; n++) {
        size_t m = start + n;
        double w = 0.5 - 0.5 * cos(2.0 * M_PI * ((double)n + 0.5) / (double)count);
        /* The phase is reduced first, so it stays exact far into the file. */
        double turns = fmod((double)m * freq, (double)r->rate) / (double)r->rate;
        double x = (double)r->data[m * r->channels + ch] * w;

        re += x * cos(2.0 * M_PI * turns);
        im -= x * sin(2.0 * M_PI * turns);
        wsum += w;
    }
    amp = 2.0 * sqrt(re * re + im * im) / wsum / FULL_SCALE;
    return amp > 0.0 ? 20.0 * log10(amp) : -400.0;
}

static int load(const char *path, recording *r)
{
    struct stat info;
    const uint8_t *map, *data = NULL;
    uint64_t data_bytes = 0;
    size_t size, offset, start, count;
    uint16_t bits = 0;
    unsigned p, ch;
    int fd, bursts;

    memset(r, 0, sizeof(*r));
    r->path = path;
    fd = open(path, O_RDONLY);
    if (fd < 0 || fstat(fd, &info) != 0 || info.st_size < 12) {
        printf("Couldn't open %s: %s\n", path, fd < 0 ? strerror(errno) : "too short");
        return 0;
    }
    size = (size_t)info.st_size;
    map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED || memcmp(map, "RIFF", 4) != 0 || memcmp(map + 8, "WAVE", 4) != 0) {
        printf("%s isn't a WAV file.\n", path);
        return 0;
    }
    for (offset = 12; offset + 8 <= size;) {
        uint32_t length = get_u32(map + offset + 4);

        if (memcmp(map + offset, "fmt ", 4) == 0 && offset + 8 + 16 <= size) {
            r->channels = get_u16(map + offset + 10);
            r->rate = get_u32(map + offset + 12);
            bits = get_u16(map + offset + 22);
        } else if (memcmp(map + offset, "data", 4) == 0) {
            data = map + offset + 8;
            data_bytes = length;
            if (offset + 8 + data_bytes > size)
                data_bytes = size - offset - 8;
            break;
        }
        offset += 8 + (size_t)length + (length & 1);
    }
    if (data == NULL || r->channels == 0 || r->channels > MAX_CHANNELS || r->rate == 0 ||
        bits != 32 || ((uintptr_t)data & 3) != 0) {
        printf("%s: expected 32-bit audio with a data chunk.\n", path);
        return 0;
    }
    r->data = (const int32_t *)data;
    r->frames = (size_t)(data_bytes / ((uint64_t)r->channels * 4));
    /* At the rates with 12 capture channels, the last two are the monitor mix. */
    r->inputs = r->channels == UA101_MIX_CHANNELS ? r->channels - UA101_MIX_OUTPUTS : r->channels;

    bursts = load_bursts(path, &r->first);
    if (bursts == 0) {
        printf("%s: no jack map tone was played (no usable %s.jack).\n", path, path);
        return 0;
    }
    /* The steady middle half, well clear of the fades and of the latency. */
    count = (size_t)(r->first.seconds * r->rate) / 2;
    start = (size_t)r->first.wav_guess + count / 2;
    if (count < r->rate / 10 || start + count > r->frames) {
        printf("%s: the recording ends before the tone does.\n", path);
        return 0;
    }
    for (p = 0; p < r->first.outputs; p++) {
        int best = -1, second = -1;

        for (ch = 0; ch < r->channels; ch++)
            r->level[p][ch] = level_dbfs(r, start, count, ch, r->first.freq[p]);
        for (ch = 0; ch < r->inputs; ch++) {
            if (best < 0 || r->level[p][ch] > r->level[p][best]) {
                second = best;
                best = (int)ch;
            } else if (second < 0 || r->level[p][ch] > r->level[p][second]) {
                second = (int)ch;
            }
        }
        r->route[p] = best >= 0 && r->level[p][best] >= r->first.db - RECORDED_DB ? best : -1;
        if (r->route[p] >= 0 && second >= 0 && r->level[p][second] >= r->level[p][best] - SECOND_DB) {
            printf("%s: playback %u reached two inputs, ch %d and ch %d.\n", path, p + 1, best + 1,
                   second + 1);
            r->problems++;
        }
    }
    for (p = 0; p < r->first.outputs; p++) {
        unsigned q;

        for (q = p + 1; q < r->first.outputs; q++)
            if (r->route[p] >= 0 && r->route[p] == r->route[q]) {
                printf("%s: playback %u and %u both reached ch %d.\n", path, p + 1, q + 1,
                       r->route[p] + 1);
                r->problems++;
            }
    }
    return 1;
}

static unsigned arrived(const recording *r)
{
    unsigned p, n = 0;

    for (p = 0; p < r->first.outputs; p++)
        n += r->route[p] >= 0;
    return n;
}

static int levels(const char *path)
{
    recording r;
    unsigned p, ch;

    if (!load(path, &r))
        return 1;
    printf("%s\n%u channels, %u Hz, %zu frames (%.2f s)\n", path, r.channels, r.rate, r.frames,
           (double)r.frames / r.rate);
    printf("Jack map tone sent at %.1f dBFS for %.2f s on %u playback channels\n\n", r.first.db,
           r.first.seconds, r.first.outputs);
    printf("Level of each playback channel's frequency on each capture channel (dBFS):\n");
    printf("               ");
    for (ch = 0; ch < r.channels; ch++) {
        if (ch < r.inputs)
            printf("  ch%-3u", ch + 1);
        else
            printf(ch == r.inputs ? "  mixL " : "  mixR ");
    }
    printf("\n");
    for (p = 0; p < r.first.outputs; p++) {
        printf("  play %2u %4.0f Hz", p + 1, r.first.freq[p]);
        for (ch = 0; ch < r.channels; ch++)
            printf(" %6.1f", r.level[p][ch]);
        printf("\n");
    }
    if (r.inputs < r.channels)
        printf("mixL and mixR are the monitor mix, so they're never taken for an input. They hear\n"
               "playback 1 and 2 directly and again through the cables a little later, so those\n"
               "two can partly cancel there.\n");
    printf("\nWhere each one arrived (an input within %.0f dB of the level sent):\n", RECORDED_DB);
    for (p = 0; p < r.first.outputs; p++) {
        if (r.route[p] < 0) {
            printf("  Playback %2u: not recorded on any input\n", p + 1);
            continue;
        }
        printf("  Playback %2u: capture ch %d, at %.1f dBFS (%+.1f dB against what was sent)\n",
               p + 1, r.route[p] + 1, r.level[p][r.route[p]],
               r.level[p][r.route[p]] - r.first.db);
    }
    printf("\n=== Jack map levels: %u of %u arrived, %s (%d problems) ===\n", arrived(&r),
           r.first.outputs, r.problems == 0 ? "each on an input of its own" : "NOT one each",
           r.problems);
    return r.problems == 0 ? 0 : 1;
}

/* For a SENS run against the straight run: the one capture channel that rose, or -1. */
static int anchor(const recording *straight, const recording *sens, const char *knob)
{
    unsigned p;
    int risen = -1, moved = 0;

    printf("\n%s (%s against the straight run), level change on each looped input:\n", knob,
           sens->path);
    for (p = 0; p < straight->first.outputs; p++) {
        int ch = straight->route[p];
        double delta;

        if (ch < 0)
            continue;
        if (sens->route[p] != ch) {
            printf("  Playback %u arrived on ch %d here, but on ch %d in the straight run, so the\n"
                   "  cables aren't the same as there.\n",
                   p + 1, sens->route[p] + 1, ch + 1);
            return -1;
        }
        delta = sens->level[p][ch] - straight->level[p][ch];
        printf("  ch %2d: %+5.1f dB\n", ch + 1, delta);
        if (delta >= ANCHOR_RISE_DB) {
            if (risen >= 0)
                moved++;
            risen = ch;
        } else if (fabs(delta) >= ANCHOR_STILL_DB) {
            moved++;
        }
    }
    if (risen < 0 || moved > 0) {
        printf("  Needs exactly one channel %.0f dB or more up and the rest within %.0f dB, so\n"
               "  it doesn't show which channel %s acts on.\n",
               ANCHOR_RISE_DB, ANCHOR_STILL_DB, knob);
        return -1;
    }
    printf("  %s acts on ch %d.\n", knob, risen + 1);
    return risen;
}

static int solve(int argc, char **argv)
{
    recording r[4];
    int count = argc, i, start, a1, a2 = -1, differs = 0;
    int next[MAX_CHANNELS], jack_of_channel[MAX_CHANNELS], channel_of_jack[ANALOG_JACKS + 1];
    unsigned p, j, steps;
    const char *names[4] = {"straight", "shifted", "SENS 1", "SENS 2"};

    for (i = 0; i < count; i++) {
        if (!load(argv[i], &r[i]))
            return 1;
        if (i > 0 && (r[i].rate != r[0].rate || r[i].channels != r[0].channels ||
                      r[i].first.outputs != r[0].first.outputs)) {
            printf("The %s run has a different rate or channel count from the straight run.\n",
                   names[i]);
            return 1;
        }
        printf("%s run: %s, %u of %u playback channels arrived, %d problems\n", names[i], argv[i],
               arrived(&r[i]), r[i].first.outputs, r[i].problems);
        if (r[i].problems != 0)
            return 1;
    }
    if (arrived(&r[0]) != ANALOG_JACKS || arrived(&r[1]) != ANALOG_JACKS) {
        printf("Solving needs all %d analog outputs looped to the %d analog inputs in both the\n"
               "straight and the shifted run.\n",
               ANALOG_JACKS, ANALOG_JACKS);
        return 1;
    }

    /*
     * In the straight run, playback p reached the channel of the input with the same label
     * as its output, and in the shifted run the channel of the next input. So the two runs
     * together say which channel is the next input after each one: next[].
     */
    for (i = 0; i < MAX_CHANNELS; i++)
        next[i] = -1;
    for (p = 0; p < r[0].first.outputs; p++) {
        if ((r[0].route[p] < 0) != (r[1].route[p] < 0)) {
            printf("Playback %u arrived in only one of the straight and shifted runs.\n", p + 1);
            return 1;
        }
        if (r[0].route[p] >= 0)
            next[r[0].route[p]] = r[1].route[p];
    }
    for (p = 0; p < r[0].first.outputs; p++)
        if (r[1].route[p] >= 0 && next[r[1].route[p]] < 0) {
            printf("In the shifted run, playback %u reached ch %d, which no output reached in the\n"
                   "straight run, so the two runs didn't use the same inputs.\n",
                   p + 1, r[1].route[p] + 1);
            return 1;
        }
    for (p = 0; p < r[0].first.outputs && r[0].route[p] < 0; p++)
        ;
    start = r[0].route[p];
    for (steps = 1, i = next[start]; i != start && steps <= ANALOG_JACKS; steps++)
        i = next[i];
    if (steps != ANALOG_JACKS) {
        printf("\nThe shifted run isn't every cable moved one input up: following it from input\n"
               "to input comes back after %u steps, not %d. %s\n",
               steps, ANALOG_JACKS,
               steps == 1 ? "It looks the same as the straight run, so the cables may not have "
                            "moved."
                          : "Check the shifted cables against the checklist.");
        return 1;
    }

    /* The SENS 1 run says which channel is INPUT 1, and the rest follow in order. */
    a1 = anchor(&r[0], &r[2], "SENS 1");
    if (a1 < 0)
        return 1;
    if (count == 4) {
        a2 = anchor(&r[0], &r[3], "SENS 2");
        if (a2 < 0)
            return 1;
        if (a2 != next[a1]) {
            printf("\nThe two knobs disagree: by SENS 1 and the shifted run, INPUT 2 is ch %d, but\n"
                   "SENS 2 acts on ch %d. One of the runs wasn't done as the checklist says.\n",
                   next[a1] + 1, a2 + 1);
            return 1;
        }
        printf("\nSENS 2 agrees with SENS 1 and the shifted run.\n");
    }
    for (i = 0; i < MAX_CHANNELS; i++)
        jack_of_channel[i] = 0;
    for (j = 1, i = a1; j <= ANALOG_JACKS; j++, i = next[i]) {
        channel_of_jack[j] = i;
        jack_of_channel[i] = (int)j;
    }

    printf("\nInputs: printed label to capture channel (the manual: INPUT n is ch n)\n");
    for (j = 1; j <= ANALOG_JACKS; j++) {
        int same = channel_of_jack[j] + 1 == (int)j;

        printf("  INPUT %u: ch %d%s\n", j, channel_of_jack[j] + 1, same ? "" : "   DIFFERS");
        differs += !same;
    }
    printf("\nOutputs: playback channel to printed label (the manual: playback n is OUTPUT n)\n");
    for (p = 0; p < r[0].first.outputs; p++) {
        int jack;

        if (r[0].route[p] < 0) {
            printf("  Playback %2u: not on an analog output%s\n", p + 1,
                   p >= ANALOG_JACKS ? " (the manual: the optical output)" : "   DIFFERS");
            differs += p < ANALOG_JACKS;
            continue;
        }
        jack = jack_of_channel[r[0].route[p]];
        printf("  Playback %2u: OUTPUT %d%s\n", p + 1, jack, jack == (int)p + 1 ? "" : "   DIFFERS");
        differs += jack != (int)p + 1;
    }
    if (differs == 0)
        printf("\n=== Jack map: matches the manual's channel map ===\n");
    else
        printf("\n=== Jack map: DIFFERS from the manual's channel map in %d places ===\n", differs);
    return differs == 0 ? 0 : 2;
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "levels") == 0)
        return levels(argv[2]);
    if ((argc == 5 || argc == 6) && strcmp(argv[1], "solve") == 0)
        return solve(argc - 2, argv + 2);
    printf("Usage: ua101jackmap levels FILE.wav\n"
           "       ua101jackmap solve STRAIGHT.wav SHIFTED.wav SENS1.wav [SENS2.wav]\n"
           "Exit status: 0 when clean (solve: and the map matches the manual's), 2 when solved\n"
           "but the map differs, 1 when something was wrong or couldn't be worked out.\n");
    return 1;
}
