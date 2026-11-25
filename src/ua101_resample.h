/*
 * Corrects audio captured on a clock that isn't quite at its nominal rate, by
 * working out what each sample would have been at the right moment. Contains no
 * Apple APIs. Samples are interleaved signed 32-bit, where 1.0 is 2^31.
 *
 * There are two filters, both Kaiser-windowed sincs. The one-shot design, for files, has 150 dB
 * of rejection and 256 coefficients, and at 48 kHz is flat to about 22.1 kHz, which covers the
 * unit's own stated range. The live design, for a driver, stops at about 120 dB, which is
 * where single precision stops, and keeps only the band up to 20 kHz clean (40 kHz at 88.2 kHz
 * and up), so it is several times shorter: it costs less and delays less. Between that band
 * and half the rate it rolls off, and what is there isn't clean.
 */
#ifndef UA101_RESAMPLE_H
#define UA101_RESAMPLE_H

#include <stddef.h>
#include <stdint.h>

/* Coefficients on each side of a sample in the one-shot design, and the most any design has. */
#define UA101_RESAMPLE_HALF_TAPS 128
#define UA101_RESAMPLE_PHASES 4096

typedef struct {
    double step;        /* input frames per output frame */
    unsigned half_taps; /* coefficients on each side of a sample; live audio is delayed by this */
    double *rows;  /* UA101_RESAMPLE_PHASES + 1 rows of coefficients, for ua101_resampler_run */
    float *rows_f; /* the same rows in single precision, for the live version */
} ua101_resampler;

/* The one-shot design. input_hz is what the clock really ran at, and output_hz what it should
   have been. This builds both tables, about 12.6 MB. */
int ua101_resampler_init(ua101_resampler *r, double input_hz, double output_hz);
/* The live design, in single precision only: under 1 MB at 48 kHz. ua101_resampler_run
   refuses a filter made this way and leaves its output alone. */
int ua101_resampler_init_live(ua101_resampler *r, double input_hz, double output_hz);
/* The half_taps ua101_resampler_init_live gives at these rates, without building anything:
   26 at 48 and 96 kHz, 46 at 44.1 and 88.2 kHz, 8 at 192 kHz. 0 for rates that make no sense. */
unsigned ua101_resampler_live_half_taps(double input_hz, double output_hz);
void ua101_resampler_free(ua101_resampler *r);

/* How many whole output frames fit in the input. */
uint64_t ua101_resampler_output_frames(const ua101_resampler *r, uint64_t input_frames);

/*
 * Produces output frames first to first + count - 1. Results are rounded to 24 bits
 * and saturate. Anything beyond the ends of the input counts as silence. It allocates a
 * scratch buffer on each call, about 150 KB for 12 channels, so a real-time thread uses
 * the live version below instead.
 */
void ua101_resampler_run(const ua101_resampler *r, const int32_t *input, uint64_t input_frames,
                         unsigned channels, uint64_t first, size_t count, int32_t *output);

/*
 * The live version, for a driver. Input goes in as it arrives and output is taken in whatever
 * sizes are asked for, while a clock loop adjusts the step. ua101_live_init allocates
 * everything, so push, pull and set_step never allocate, lock or wait. One thread at a time:
 * in a driver, a ring buffer carries frames between the USB thread and the one calling these.
 *
 * The position is fixed point, 32 bits of frames and 32 of fraction, counted from the start
 * of what it holds, so it can't drift however long it runs. The input starts with silence
 * before it, as in ua101_resampler_run, and with a step that is a whole number of 2^-32
 * frames, the output is what ua101_resampler_run gives, to within single precision's
 * rounding (see LIVE_ERROR in the tests for how far). Output frame n needs input up
 * to the filter's half_taps frames past its own moment, and that is the delay it adds.
 */
#define UA101_LIVE_MIN_STEP 0.5
#define UA101_LIVE_MAX_STEP 2.0

typedef struct {
    const ua101_resampler *filter; /* only its coefficients are used */
    unsigned half_taps;            /* the filter's */
    unsigned channels;
    float *held;       /* input frames held, converted, interleaved */
    size_t capacity;   /* frames it can hold, counting the filter's history */
    size_t filled;     /* frames held now */
    uint64_t position; /* of the next output frame within held, 32.32 */
    uint64_t step;     /* input frames per output frame, 32.32 */
    uint64_t pushed;   /* input frames taken in, silence included */
    uint64_t origin;   /* the input frame ua101_live_next_position counts from */
    uint64_t pulled;   /* output frames given out */
    /* What came out beyond 24 bits' range and was cut to it, since ua101_live_init: a wave
       can pass over full scale between samples that are all within it. ua101_live_reset keeps
       both, so a driver's count covers its whole run. Counting only: the output is the same. */
    uint64_t cut;      /* output samples cut */
    float cut_peak;    /* the largest of them before the cut, in 24-bit units (full scale is
                          2^23); 0 if none */
} ua101_live;

/* Starts at the filter's own step. capacity_frames must exceed twice the filter's half_taps. */
int ua101_live_init(ua101_live *l, const ua101_resampler *filter, unsigned channels,
                    size_t capacity_frames);
void ua101_live_free(ua101_live *l);

/* Takes effect from the next output frame, with no jump in position. Refuses steps outside
   UA101_LIVE_MIN_STEP to UA101_LIVE_MAX_STEP, and keeps the old one. */
int ua101_live_set_step(ua101_live *l, double step);
double ua101_live_get_step(const ua101_live *l);

/* Input frames that fit now. */
size_t ua101_live_room(const ua101_live *l);
/* Each returns how many frames it took, which is fewer than asked only when it's full. */
size_t ua101_live_push(ua101_live *l, const int32_t *input, size_t frames);
/* Silence in place of frames that were lost, so later audio stays at the right moment. */
size_t ua101_live_push_silence(ua101_live *l, size_t frames);

/* Output frames it can give now from what it holds. */
size_t ua101_live_available(const ua101_live *l);
/* Gives up to frames output frames, and returns how many. */
size_t ua101_live_pull(ua101_live *l, int32_t *output, size_t frames);

/* Back to how ua101_live_init left it, at the filter's own step, without allocating: for a
   driver that lost its place and starts again. The counts of what was cut stay. */
void ua101_live_reset(ua101_live *l);
/* Where the next output frame sits, in input frames counted from the first pushed since init
   or reset (silence included), or from where ua101_live_rebase last moved the count. A clock
   loop compares it with where that frame belongs. */
double ua101_live_next_position(const ua101_live *l);
/* Keeps that position a small number, so a double holds it exactly however long it runs: once
   at least `at_least` input frames have been counted, the count starts again at the newest
   one, and the number of frames it moved by is returned, for the caller to add to its own
   base. Otherwise returns 0. Nothing about the audio changes. */
uint64_t ua101_live_rebase(ua101_live *l, uint64_t at_least);
/* Input frames still to push before `frames` more output frames can be pulled at the current
   step; 0 if it already holds enough. */
size_t ua101_live_needed(const ua101_live *l, size_t frames);

#ifdef UA101_RESAMPLE_TESTING
/* How many times this file has allocated memory, so tests can check the live calls don't. */
unsigned long ua101_resample_allocations(void);
#endif

#ifdef UA101_RESAMPLE_TESTING
/* The straightforward version, which ua101_resampler_run must match exactly. */
void ua101_resampler_run_plain(const ua101_resampler *r, const int32_t *input,
                               uint64_t input_frames, unsigned channels, uint64_t first,
                               size_t count, int32_t *output);
#endif

#endif
