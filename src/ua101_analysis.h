/*
 * Offline checks on captured audio, used by ua101wavcheck. Contains no Apple
 * APIs. Samples are interleaved signed 32-bit, where 1.0 is 2^31.
 */
#ifndef UA101_ANALYSIS_H
#define UA101_ANALYSIS_H

#include <stddef.h>
#include <stdint.h>

#include "ua101_stream.h"

typedef struct {
    double peak_dbfs;
    double rms_dbfs;
    double dc_dbfs;
    double min_block_rms_dbfs; /* over 100 ms blocks */
    double max_block_rms_dbfs;
    uint64_t low_byte_nonzero; /* samples using bits a 24-bit converter can't set */
    uint64_t clicks;           /* outliers of the second difference */
    size_t first_click;        /* frame index, valid when clicks > 0 */
} ua101_channel_stats;

void ua101_channel_analyze(const int32_t *samples, size_t frames, unsigned channels,
                           unsigned ch, double rate_hz, ua101_channel_stats *out);

/* Level in dBFS of a frequency on one channel over a window of frames. */
double ua101_goertzel_dbfs(const int32_t *samples, size_t first, size_t count,
                           unsigned channels, unsigned ch, double freq_hz, double rate_hz);

/*
 * Finds where a known tone landed. ch_for_tone[i] is the capture channel that
 * carries tone channel i. Searches start positions from guess - radius to
 * guess + radius, and returns the frame with the best correlation, or -1.
 * The fades are what make the answer unique, because the middle is periodic.
 */
int64_t ua101_tone_locate(const int32_t *samples, size_t frames, unsigned channels,
                          const ua101_tone *tone, const unsigned ch_for_tone[UA101_TONE_CHANNELS],
                          int64_t guess, int64_t radius);

typedef struct {
    double level_dbfs;        /* fitted amplitude */
    double delay_fraction;    /* samples of delay left after the integer position */
    double residual_db;       /* everything that isn't the tone, relative to the tone */
    double worst_block_db;    /* loudest 1 ms of that residual, relative to the tone */
    double max_phase_dev_deg; /* biggest phase jump over 10 ms blocks */
    double slip_limit_deg;    /* half of what one lost sample would cause */
    int clean;
} ua101_tone_fit;

/* Measures one tone channel captured on ch, with the tone starting at frame start. */
void ua101_tone_measure(const int32_t *samples, size_t frames, unsigned channels, unsigned ch,
                        const ua101_tone *tone, unsigned tone_ch, size_t start,
                        ua101_tone_fit *out);

/*
 * The UA-101's monitor mix is digital. With the mixer at unity, measured on real
 * hardware, capture channels 11 and 12 are exact sums:
 *
 *   ch 11 = inputs 1 + 3 + 5 + 7 + 9 + what's being played on output 1
 *   ch 12 = inputs 2 + 4 + 6 + 8 + 10 + what's being played on output 2
 *
 * Subtracting the inputs recovers the playback stream bit for bit, with no cable.
 * Because it holds for every single frame, it also checks every captured frame,
 * using whatever noise is on the inputs. A frame that was damaged, or a channel
 * that slipped, breaks it. Losing a whole frame doesn't, which is what comparing
 * the positions of two tones is for.
 */
#define UA101_MIX_CHANNELS 12
#define UA101_MIX_OUTPUTS 2

/* Writes frames * 2 interleaved samples. Values that don't fit in 32 bits saturate. */
void ua101_mix_recover(const int32_t *samples, size_t frames, int32_t *recovered);

typedef struct {
    uint64_t compared;
    uint64_t different;
    size_t first_different; /* relative to the start of the comparison */
    int64_t largest;
} ua101_exact_check;

/* Compares one recovered output with the tone, sample for sample. */
void ua101_tone_compare(const int32_t *recovered, size_t frames, unsigned output,
                        const ua101_tone *tone, size_t start, ua101_exact_check *out);
/* Adds one recovered output's frames in [first, last) to a check that they're silent. */
void ua101_silence_check(const int32_t *recovered, size_t first, size_t last, unsigned output,
                         ua101_exact_check *out);

#endif
