/*
 * Writes a corrected copy of a recording made on a clock that isn't quite at its
 * nominal rate, so the copy lasts as long as the recording really did, provided the
 * rate it's given is right. It never changes the original, and it won't overwrite
 * an existing file.
 *
 * The audio is resampled, so the copy is no longer bit-exact, and ua101wavcheck's
 * exact checks don't apply to it.
 */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ua101_resample.h"
#include "ua101_stream.h"

/* This tool is for small clock errors. A bigger difference is probably a typing mistake. */
#define MAX_CORRECTION_PPM 1000.0
#define BLOCK_FRAMES 65536

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

int main(int argc, char **argv)
{
    struct stat info;
    const uint8_t *map, *data = NULL;
    uint64_t data_bytes = 0, input_frames, output_frames, done = 0;
    size_t size, offset;
    unsigned channels = 0;
    uint32_t rate = 0;
    uint16_t bits = 0, valid = 0;
    double actual_hz, ppm;
    ua101_resampler resampler;
    uint8_t header[UA101_WAV_HEADER_BYTES];
    int32_t *block;
    int in, out;

    if (argc != 4) {
        printf("Usage: ua101fixrate IN.wav OUT.wav ACTUAL_HZ\n"
               "ACTUAL_HZ is the rate the unit really ran at, from a long ua101stream run's\n"
               "\"Device against host\" line.\n");
        return 2;
    }
    actual_hz = atof(argv[3]);

    in = open(argv[1], O_RDONLY);
    if (in < 0 || fstat(in, &info) != 0) {
        printf("Couldn't open %s: %s\n", argv[1], strerror(errno));
        return 2;
    }
    size = (size_t)info.st_size;
    if (size < 12) {
        printf("%s is too short to be a WAV file.\n", argv[1]);
        return 2;
    }
    map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, in, 0);
    if (map == MAP_FAILED) {
        printf("Couldn't map %s: %s\n", argv[1], strerror(errno));
        return 2;
    }
    if (memcmp(map, "RIFF", 4) != 0 || memcmp(map + 8, "WAVE", 4) != 0) {
        printf("%s isn't a WAV file.\n", argv[1]);
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
            if (offset + 8 + data_bytes > size)
                data_bytes = size - offset - 8;
            break;
        }
        offset += 8 + (size_t)length + (length & 1);
    }
    if (data == NULL || channels == 0 || rate == 0 || bits != 32 || ((uintptr_t)data & 3) != 0) {
        printf("Expected aligned 32-bit audio with a data chunk.\n");
        return 2;
    }

    ppm = (actual_hz / rate - 1.0) * 1e6;
    if (!(actual_hz > 0.0) || fabs(ppm) > MAX_CORRECTION_PPM) {
        printf("%s Hz is %+.0f ppm from the file's %u Hz, and the limit is %.0f ppm.\n", argv[3],
               ppm, rate, MAX_CORRECTION_PPM);
        return 2;
    }
    input_frames = data_bytes / ((uint64_t)channels * 4);

    if (!ua101_resampler_init(&resampler, actual_hz, (double)rate)) {
        printf("Out of memory.\n");
        return 2;
    }
    output_frames = ua101_resampler_output_frames(&resampler, input_frames);
    if (output_frames * channels * 4 > UA101_WAV_MAX_DATA_BYTES) {
        printf("The corrected recording would be too big for a WAV file.\n");
        return 2;
    }
    block = malloc((size_t)BLOCK_FRAMES * channels * sizeof(int32_t));
    if (block == NULL) {
        printf("Out of memory.\n");
        return 2;
    }

    out = open(argv[2], O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (out < 0) {
        printf("Couldn't create %s: %s\n", argv[2], strerror(errno));
        return 2;
    }
    ua101_wav_header(header, (uint16_t)channels, rate, bits, valid,
                     output_frames * channels * 4);
    if (write(out, header, sizeof(header)) != (ssize_t)sizeof(header)) {
        printf("Couldn't write %s: %s\n", argv[2], strerror(errno));
        return 1;
    }
    while (done < output_frames) {
        size_t count = output_frames - done < BLOCK_FRAMES ? (size_t)(output_frames - done)
                                                           : BLOCK_FRAMES;
        size_t bytes = count * channels * sizeof(int32_t);

        ua101_resampler_run(&resampler, (const int32_t *)data, input_frames, channels, done, count,
                            block);
        if (write(out, block, bytes) != (ssize_t)bytes) {
            printf("Couldn't write %s: %s\n", argv[2], strerror(errno));
            return 1;
        }
        done += count;
    }
    close(out);

    printf("%s: %llu frames, labeled %u Hz, lasting %.3f s by its label\n", argv[1],
           (unsigned long long)input_frames, rate, (double)input_frames / rate);
    printf("%s: %llu frames at %u Hz, lasting %.3f s\n", argv[2],
           (unsigned long long)output_frames, rate, (double)output_frames / rate);
    printf("This assumes the unit really ran at the %.3f Hz you gave (%+.1f ppm). The tool can't check that.\n",
           actual_hz, ppm);
    printf("=== Rate correction: DONE ===\n");
    ua101_resampler_free(&resampler);
    free(block);
    return 0;
}
