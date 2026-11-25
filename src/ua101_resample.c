#include "ua101_resample.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* The one-shot design's length, which is also the most any design has. */
#define TAPS (2 * UA101_RESAMPLE_HALF_TAPS)
#define MAX_CHANNELS 64

/* From the standard Kaiser design formulas, for this rejection and length. */
#define REJECTION_DB 150.0
#define KAISER_BETA (0.1102 * (REJECTION_DB - 8.7))
#define TRANSITION_CYCLES ((REJECTION_DB - 7.95) / (2.285 * (TAPS - 1)) / (2.0 * M_PI))

/*
 * The live design. Single precision stops near -120 dB, so more rejection than that buys
 * nothing. Its slope runs from the top of the band to keep (20 kHz, or 40 kHz at 88.2 kHz and
 * up, which is what the unit states) to the rate less that band, with the cutoff in the middle.
 * Whatever leaks through the slope then comes from, and lands, above the band, so everything
 * inside it stays clean, and the filter can be several times shorter than the one-shot one.
 * Aimed a little past 120 dB, so that the filter's own error (its ripple and what is left of
 * the images) sits at single precision's floor. Measured 2026-11-10 on full-scale tones from
 * 20 Hz to the top of the band, with the step 120 ppm off: worst sample -120 dBFS at every
 * rate; aimed at 120 it was -112, and 124 gave -117.
 */
#define LIVE_REJECTION_DB 127.0
#define LIVE_BAND_HZ 20000.0
#define LIVE_WIDE_BAND_HZ 40000.0
#define LIVE_WIDE_FROM_HZ 80000.0
#define LIVE_MIN_HALF_TAPS 4

#ifdef UA101_RESAMPLE_TESTING
static unsigned long g_allocations;

unsigned long ua101_resample_allocations(void)
{
    return g_allocations;
}
#endif

/* Every allocation in this file goes through here, so the tests can count them. */
static void *allocate(size_t bytes)
{
#ifdef UA101_RESAMPLE_TESTING
    g_allocations++;
#endif
    return malloc(bytes);
}

static double bessel_i0(double x)
{
    double sum = 1.0, term = 1.0;
    int k;

    for (k = 1; k < 64; k++) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < sum * 1e-18)
            break;
    }
    return sum;
}

/* A Kaiser-windowed sinc: how long, how sharp a window, and where it cuts. */
typedef struct {
    int half_taps;
    double beta;
    double cutoff; /* in cycles per input sample */
} design;

/* The one-shot design: the stopband starts at the lower of the two Nyquists. */
static design one_shot_design(double input_hz, double output_hz)
{
    double narrowest = (output_hz < input_hz ? output_hz : input_hz) / input_hz;
    design d = {UA101_RESAMPLE_HALF_TAPS, KAISER_BETA, narrowest / 2.0 - TRANSITION_CYCLES / 2.0};

    return d;
}

/* The live design, at the length Kaiser's formula gives for its slope, kept even. Rates too
   low for the slope to fit (none of the unit's) get the one-shot design. */
static design live_design(double input_hz, double output_hz)
{
    double narrowest = output_hz < input_hz ? output_hz : input_hz;
    double band = narrowest < LIVE_WIDE_FROM_HZ ? LIVE_BAND_HZ : LIVE_WIDE_BAND_HZ;
    double slope = (narrowest - 2.0 * band) / input_hz; /* its width, in cycles per input sample */
    double half, rejection;
    design d;

    if (!(slope > 0.0))
        return one_shot_design(input_hz, output_hz);
    half = ceil(((LIVE_REJECTION_DB - 7.95) / (2.285 * 2.0 * M_PI * slope) + 1.0) / 2.0);
    if (!(half <= UA101_RESAMPLE_HALF_TAPS))
        return one_shot_design(input_hz, output_hz);
    d.half_taps = half < LIVE_MIN_HALF_TAPS ? LIVE_MIN_HALF_TAPS : (int)half;
    /* Rounding the length up left a little room: the window takes it as extra rejection. */
    rejection = 7.95 + 2.285 * 2.0 * M_PI * slope * (2 * d.half_taps - 1);
    d.beta = 0.1102 * ((rejection < REJECTION_DB ? rejection : REJECTION_DB) - 8.7);
    d.cutoff = narrowest / 2.0 / input_hz;
    return d;
}

static int build(ua101_resampler *r, double input_hz, double output_hz, int live)
{
    double i0_beta, row[TAPS];
    int phase, m, half, taps;
    size_t values;
    design d;

    r->rows = NULL;
    r->rows_f = NULL;
    r->half_taps = 0;
    if (!(input_hz > 0.0) || !(output_hz > 0.0))
        return 0;
    r->step = input_hz / output_hz;
    d = live ? live_design(input_hz, output_hz) : one_shot_design(input_hz, output_hz);
    half = d.half_taps;
    taps = 2 * half;
    values = (size_t)(UA101_RESAMPLE_PHASES + 1) * (size_t)taps;
    i0_beta = bessel_i0(d.beta);

    if (!live && (r->rows = allocate(values * sizeof(double))) == NULL)
        return 0;
    if ((r->rows_f = allocate(values * sizeof(float))) == NULL) {
        ua101_resampler_free(r);
        return 0;
    }
    r->half_taps = (unsigned)half;

    for (phase = 0; phase <= UA101_RESAMPLE_PHASES; phase++) {
        double frac = (double)phase / UA101_RESAMPLE_PHASES;
        double sum = 0.0;

        for (m = 0; m < taps; m++) {
            double t = (m - (half - 1)) - frac;
            double x = t / half;
            double window = fabs(x) < 1.0 ? bessel_i0(d.beta * sqrt(1.0 - x * x)) / i0_beta : 0.0;
            double arg = 2.0 * M_PI * d.cutoff * t;
            double sinc = fabs(arg) < 1e-12 ? 1.0 : sin(arg) / arg;

            row[m] = 2.0 * d.cutoff * sinc * window;
            sum += row[m];
        }
        /* Exactly unity at DC for every phase, so silence and offsets stay clean. */
        for (m = 0; m < taps; m++)
            row[m] /= sum;
        if (r->rows != NULL)
            memcpy(r->rows + (size_t)phase * (size_t)taps, row, (size_t)taps * sizeof(double));
        for (m = 0; m < taps; m++)
            r->rows_f[(size_t)phase * (size_t)taps + (size_t)m] = (float)row[m];
    }
    return 1;
}

int ua101_resampler_init(ua101_resampler *r, double input_hz, double output_hz)
{
    return build(r, input_hz, output_hz, 0);
}

int ua101_resampler_init_live(ua101_resampler *r, double input_hz, double output_hz)
{
    return build(r, input_hz, output_hz, 1);
}

unsigned ua101_resampler_live_half_taps(double input_hz, double output_hz)
{
    if (!(input_hz > 0.0) || !(output_hz > 0.0))
        return 0;
    return (unsigned)live_design(input_hz, output_hz).half_taps;
}

void ua101_resampler_free(ua101_resampler *r)
{
    free(r->rows);
    free(r->rows_f);
    r->rows = NULL;
    r->rows_f = NULL;
    r->half_taps = 0;
}

uint64_t ua101_resampler_output_frames(const ua101_resampler *r, uint64_t input_frames)
{
    return (uint64_t)floor((double)input_frames / r->step);
}

static int32_t to_24_bits(double value)
{
    double units = round(value / 256.0);

    if (units >= 8388607.0)
        return (int32_t)(8388607 * 256);
    if (units <= -8388608.0)
        return INT32_MIN;
    return (int32_t)units * 256;
}

/* The straightforward version: kept as the reference the faster one must match exactly, and
   used if the faster one can't get memory. */
static void run_plain(const ua101_resampler *r, const int32_t *input, uint64_t input_frames,
                      unsigned channels, uint64_t first, size_t count, int32_t *output)
{
    size_t n;

    for (n = 0; n < count; n++) {
        double position = (double)(first + n) * r->step;
        double whole = floor(position);
        double scaled = (position - whole) * UA101_RESAMPLE_PHASES;
        int phase = (int)scaled;
        double blend = scaled - phase;
        const double *row = r->rows + (size_t)phase * TAPS;
        double taps[TAPS], sums[MAX_CHANNELS] = {0.0};
        int64_t base = (int64_t)whole - (UA101_RESAMPLE_HALF_TAPS - 1);
        int m;
        unsigned ch;

        /* A straight line between neighboring phases is accurate to about -146 dB. */
        for (m = 0; m < TAPS; m++)
            taps[m] = fma(blend, row[m + TAPS] - row[m], row[m]);

        for (m = 0; m < TAPS; m++) {
            int64_t at = base + m;
            const int32_t *frame;

            if (at < 0 || (uint64_t)at >= input_frames)
                continue;
            frame = input + (size_t)at * channels;
            for (ch = 0; ch < channels; ch++)
                sums[ch] = fma(taps[m], (double)frame[ch], sums[ch]);
        }
        for (ch = 0; ch < channels; ch++)
            output[n * channels + ch] = to_24_bits(sums[ch]);
    }
}

typedef double vec2 __attribute__((ext_vector_type(2)));

static vec2 load2(const double *p)
{
    vec2 v;

    memcpy(&v, p, sizeof(v));
    return v;
}

static void store2(double *p, vec2 v)
{
    memcpy(p, &v, sizeof(v));
}

#define FMA2 __builtin_elementwise_fma

/*
 * Two output frames at once: s0[ch] = t0 . x0[ch] and s1[ch] = t1 . x1[ch], each over TAPS
 * frames of interleaved doubles. Every channel of every frame is summed in the same order, with
 * the same fused multiply-adds, as run_plain, so the results are identical. Each sum is a chain
 * of TAPS steps that each wait for the one before, so the speed comes from running many chains
 * side by side: up to 8 channels of both frames.
 */
static void dot_pair(const double *t0, const double *x0, const double *t1, const double *x1,
                     unsigned channels, double *s0, double *s1)
{
    unsigned ch = 0;
    int m;

    for (; ch + 8 <= channels; ch += 8) {
        const double *p = x0 + ch, *q = x1 + ch;
        vec2 a0 = 0.0, b0 = 0.0, c0 = 0.0, d0 = 0.0, a1 = 0.0, b1 = 0.0, c1 = 0.0, d1 = 0.0;

        for (m = 0; m < TAPS; m++, p += channels, q += channels) {
            vec2 u = t0[m], v = t1[m];

            a0 = FMA2(u, load2(p), a0);
            b0 = FMA2(u, load2(p + 2), b0);
            c0 = FMA2(u, load2(p + 4), c0);
            d0 = FMA2(u, load2(p + 6), d0);
            a1 = FMA2(v, load2(q), a1);
            b1 = FMA2(v, load2(q + 2), b1);
            c1 = FMA2(v, load2(q + 4), c1);
            d1 = FMA2(v, load2(q + 6), d1);
        }
        store2(s0 + ch, a0);
        store2(s0 + ch + 2, b0);
        store2(s0 + ch + 4, c0);
        store2(s0 + ch + 6, d0);
        store2(s1 + ch, a1);
        store2(s1 + ch + 2, b1);
        store2(s1 + ch + 4, c1);
        store2(s1 + ch + 6, d1);
    }
    for (; ch + 4 <= channels; ch += 4) {
        const double *p = x0 + ch, *q = x1 + ch;
        vec2 a0 = 0.0, b0 = 0.0, a1 = 0.0, b1 = 0.0;

        for (m = 0; m < TAPS; m++, p += channels, q += channels) {
            vec2 u = t0[m], v = t1[m];

            a0 = FMA2(u, load2(p), a0);
            b0 = FMA2(u, load2(p + 2), b0);
            a1 = FMA2(v, load2(q), a1);
            b1 = FMA2(v, load2(q + 2), b1);
        }
        store2(s0 + ch, a0);
        store2(s0 + ch + 2, b0);
        store2(s1 + ch, a1);
        store2(s1 + ch + 2, b1);
    }
    for (; ch + 2 <= channels; ch += 2) {
        const double *p = x0 + ch, *q = x1 + ch;
        vec2 a0 = 0.0, a1 = 0.0;

        for (m = 0; m < TAPS; m++, p += channels, q += channels) {
            a0 = FMA2((vec2)t0[m], load2(p), a0);
            a1 = FMA2((vec2)t1[m], load2(q), a1);
        }
        store2(s0 + ch, a0);
        store2(s1 + ch, a1);
    }
    for (; ch < channels; ch++) {
        const double *p = x0 + ch, *q = x1 + ch;
        double a0 = 0.0, a1 = 0.0;

        for (m = 0; m < TAPS; m++, p += channels, q += channels) {
            a0 = fma(t0[m], *p, a0);
            a1 = fma(t1[m], *q, a1);
        }
        s0[ch] = a0;
        s1[ch] = a1;
    }
}

/* Fills taps for output frame f, as run_plain does, and returns its first input frame. */
static int64_t frame_taps(const ua101_resampler *r, uint64_t f, double *taps)
{
    double position = (double)f * r->step;
    double whole = floor(position);
    double scaled = (position - whole) * UA101_RESAMPLE_PHASES;
    int phase = (int)scaled;
    double blend = scaled - phase;
    const double *row = r->rows + (size_t)phase * TAPS;
    int m;

    for (m = 0; m < TAPS; m++)
        taps[m] = fma(blend, row[m + TAPS] - row[m], row[m]);
    return (int64_t)whole - (UA101_RESAMPLE_HALF_TAPS - 1);
}

/* Output frames worked out per pass over one converted stretch of the input. */
#define BLOCK_FRAMES 1024

void ua101_resampler_run(const ua101_resampler *r, const int32_t *input, uint64_t input_frames,
                         unsigned channels, uint64_t first, size_t count, int32_t *output)
{
    /* Each block needs its outputs' input frames plus the filter's reach on either side. */
    size_t capacity = (size_t)ceil(BLOCK_FRAMES * r->step) + TAPS + 2;
    double *scratch;
    size_t done = 0;

    if (r->rows == NULL || channels == 0 || channels > MAX_CHANNELS)
        return;
    scratch = allocate(capacity * channels * sizeof(double));
    if (scratch == NULL) {
        run_plain(r, input, input_frames, channels, first, count, output);
        return;
    }

    while (done < count) {
        size_t n = count - done < BLOCK_FRAMES ? count - done : BLOCK_FRAMES, k, i;
        uint64_t f0 = first + done;
        int64_t lo = (int64_t)floor((double)f0 * r->step) - (UA101_RESAMPLE_HALF_TAPS - 1);
        int64_t hi = (int64_t)floor((double)(f0 + n - 1) * r->step) -
                     (UA101_RESAMPLE_HALF_TAPS - 1) + TAPS;
        size_t span = (size_t)(hi - lo);

        if (span > capacity) { /* can't happen, but never write past the buffer */
            run_plain(r, input, input_frames, channels, f0, count - done, output + done * channels);
            break;
        }
        /* Converted once here, instead of once per tap that uses them. Frames beyond the ends
           of the input are silence, as in run_plain, so there's nothing to check per tap. */
        for (i = 0; i < span; i++) {
            int64_t at = lo + (int64_t)i;
            double *to = scratch + i * channels;
            unsigned ch;

            if (at < 0 || (uint64_t)at >= input_frames) {
                memset(to, 0, channels * sizeof(double));
            } else {
                const int32_t *from = input + (size_t)at * channels;

                for (ch = 0; ch < channels; ch++)
                    to[ch] = (double)from[ch];
            }
        }

        for (k = 0; k < n; k += 2) {
            double taps0[TAPS], taps1[TAPS], sums0[MAX_CHANNELS], sums1[MAX_CHANNELS];
            int pair = k + 1 < n;
            int64_t base0 = frame_taps(r, f0 + k, taps0);
            /* An odd frame at the end is simply worked out twice. */
            int64_t base1 = pair ? frame_taps(r, f0 + k + 1, taps1) : base0;
            int32_t *out = output + (done + k) * channels;
            unsigned ch;

            dot_pair(taps0, scratch + (size_t)(base0 - lo) * channels, pair ? taps1 : taps0,
                     scratch + (size_t)(base1 - lo) * channels, channels, sums0, sums1);
            for (ch = 0; ch < channels; ch++)
                out[ch] = to_24_bits(sums0[ch]);
            if (pair)
                for (ch = 0; ch < channels; ch++)
                    out[channels + ch] = to_24_bits(sums1[ch]);
        }
        done += n;
    }
    free(scratch);
}

/* The live version. Positions are 32.32 fixed point, and the top bits of the fraction pick the
   filter phase, so the phase and blend come out exactly as frame_taps works them out. */
#define FRACTION_BITS 32
#define PHASE_SHIFT 20
_Static_assert((1u << (FRACTION_BITS - PHASE_SHIFT)) == UA101_RESAMPLE_PHASES,
               "the fraction's top bits must index the phases");
#define FRACTION_MASK ((UINT64_C(1) << FRACTION_BITS) - 1)
#define BLEND_MASK ((UINT64_C(1) << PHASE_SHIFT) - 1)

/* The filter's own length is used from here on: the live design is shorter than TAPS. The held
   frames before the first output's own moment, the filter's reach back, are half_taps - 1. */
static void live_taps(const ua101_resampler *r, uint64_t position, float *taps, int count)
{
    uint64_t fraction = position & FRACTION_MASK;
    const float *row = r->rows_f + (size_t)(fraction >> PHASE_SHIFT) * (size_t)count;
    float blend = (float)(fraction & BLEND_MASK) * (1.0f / (float)(UINT64_C(1) << PHASE_SHIFT));
    int m;

    for (m = 0; m < count; m++)
        taps[m] = fmaf(blend, row[m + count] - row[m], row[m]);
}

typedef float vec4f __attribute__((ext_vector_type(4)));
typedef float vec2f __attribute__((ext_vector_type(2)));

static vec4f load4f(const float *p)
{
    vec4f v;

    memcpy(&v, p, sizeof(v));
    return v;
}

static vec2f load2f(const float *p)
{
    vec2f v;

    memcpy(&v, p, sizeof(v));
    return v;
}

static void store4f(float *p, vec4f v)
{
    memcpy(p, &v, sizeof(v));
}

static void store2f(float *p, vec2f v)
{
    memcpy(p, &v, sizeof(v));
}

/*
 * dot_pair in single precision, for the live version: four channels to a register instead of
 * two, and the even and odd taps summed apart and added at the end. The split keeps as many
 * independent chains running as the doubles had, so the wider registers turn into speed.
 * The summing order differs from run_plain's, which the live version can afford: it is held
 * to a measured error far below the unit's own noise, not to matching bit for bit.
 */
static void dot_pair_f(const float *t0, const float *x0, const float *t1, const float *x1,
                       unsigned channels, int taps, float *s0, float *s1)
{
    const size_t next = channels;
    unsigned ch = 0;
    int m;

    /* The unit's own widths, 12 in and 10 out, each in one pass: every channel's chains run
       side by side (12 of them), where a leftover group of 4 or 2 after the first 8 had only
       4 and waited on itself. Each channel is summed in the same order as below. */
    if (channels == 12) {
        const float *p = x0, *q = x1;
        vec4f a0 = 0.0f, b0 = 0.0f, e0 = 0.0f, c0 = 0.0f, d0 = 0.0f, f0 = 0.0f;
        vec4f a1 = 0.0f, b1 = 0.0f, e1 = 0.0f, c1 = 0.0f, d1 = 0.0f, f1 = 0.0f;

        for (m = 0; m < taps; m += 2, p += 24, q += 24) {
            vec4f u = t0[m], v = t1[m], w = t0[m + 1], z = t1[m + 1];

            a0 = FMA2(u, load4f(p), a0);
            b0 = FMA2(u, load4f(p + 4), b0);
            e0 = FMA2(u, load4f(p + 8), e0);
            a1 = FMA2(v, load4f(q), a1);
            b1 = FMA2(v, load4f(q + 4), b1);
            e1 = FMA2(v, load4f(q + 8), e1);
            c0 = FMA2(w, load4f(p + 12), c0);
            d0 = FMA2(w, load4f(p + 16), d0);
            f0 = FMA2(w, load4f(p + 20), f0);
            c1 = FMA2(z, load4f(q + 12), c1);
            d1 = FMA2(z, load4f(q + 16), d1);
            f1 = FMA2(z, load4f(q + 20), f1);
        }
        store4f(s0, a0 + c0);
        store4f(s0 + 4, b0 + d0);
        store4f(s0 + 8, e0 + f0);
        store4f(s1, a1 + c1);
        store4f(s1 + 4, b1 + d1);
        store4f(s1 + 8, e1 + f1);
        return;
    }
    if (channels == 10) {
        const float *p = x0, *q = x1;
        vec4f a0 = 0.0f, b0 = 0.0f, c0 = 0.0f, d0 = 0.0f, a1 = 0.0f, b1 = 0.0f, c1 = 0.0f, d1 = 0.0f;
        vec2f e0 = 0.0f, f0 = 0.0f, e1 = 0.0f, f1 = 0.0f;

        for (m = 0; m < taps; m += 2, p += 20, q += 20) {
            vec4f u = t0[m], v = t1[m], w = t0[m + 1], z = t1[m + 1];

            a0 = FMA2(u, load4f(p), a0);
            b0 = FMA2(u, load4f(p + 4), b0);
            e0 = FMA2((vec2f)t0[m], load2f(p + 8), e0);
            a1 = FMA2(v, load4f(q), a1);
            b1 = FMA2(v, load4f(q + 4), b1);
            e1 = FMA2((vec2f)t1[m], load2f(q + 8), e1);
            c0 = FMA2(w, load4f(p + 10), c0);
            d0 = FMA2(w, load4f(p + 14), d0);
            f0 = FMA2((vec2f)t0[m + 1], load2f(p + 18), f0);
            c1 = FMA2(z, load4f(q + 10), c1);
            d1 = FMA2(z, load4f(q + 14), d1);
            f1 = FMA2((vec2f)t1[m + 1], load2f(q + 18), f1);
        }
        store4f(s0, a0 + c0);
        store4f(s0 + 4, b0 + d0);
        store2f(s0 + 8, e0 + f0);
        store4f(s1, a1 + c1);
        store4f(s1 + 4, b1 + d1);
        store2f(s1 + 8, e1 + f1);
        return;
    }
    for (; ch + 8 <= channels; ch += 8) {
        const float *p = x0 + ch, *q = x1 + ch;
        vec4f a0 = 0.0f, b0 = 0.0f, c0 = 0.0f, d0 = 0.0f, a1 = 0.0f, b1 = 0.0f, c1 = 0.0f, d1 = 0.0f;

        for (m = 0; m < taps; m += 2, p += 2 * next, q += 2 * next) {
            vec4f u = t0[m], v = t1[m], w = t0[m + 1], z = t1[m + 1];

            a0 = FMA2(u, load4f(p), a0);
            b0 = FMA2(u, load4f(p + 4), b0);
            a1 = FMA2(v, load4f(q), a1);
            b1 = FMA2(v, load4f(q + 4), b1);
            c0 = FMA2(w, load4f(p + next), c0);
            d0 = FMA2(w, load4f(p + next + 4), d0);
            c1 = FMA2(z, load4f(q + next), c1);
            d1 = FMA2(z, load4f(q + next + 4), d1);
        }
        store4f(s0 + ch, a0 + c0);
        store4f(s0 + ch + 4, b0 + d0);
        store4f(s1 + ch, a1 + c1);
        store4f(s1 + ch + 4, b1 + d1);
    }
    for (; ch + 4 <= channels; ch += 4) {
        const float *p = x0 + ch, *q = x1 + ch;
        vec4f a0 = 0.0f, c0 = 0.0f, a1 = 0.0f, c1 = 0.0f;

        for (m = 0; m < taps; m += 2, p += 2 * next, q += 2 * next) {
            a0 = FMA2((vec4f)t0[m], load4f(p), a0);
            a1 = FMA2((vec4f)t1[m], load4f(q), a1);
            c0 = FMA2((vec4f)t0[m + 1], load4f(p + next), c0);
            c1 = FMA2((vec4f)t1[m + 1], load4f(q + next), c1);
        }
        store4f(s0 + ch, a0 + c0);
        store4f(s1 + ch, a1 + c1);
    }
    for (; ch + 2 <= channels; ch += 2) {
        const float *p = x0 + ch, *q = x1 + ch;
        vec2f a0 = 0.0f, c0 = 0.0f, a1 = 0.0f, c1 = 0.0f;

        for (m = 0; m < taps; m += 2, p += 2 * next, q += 2 * next) {
            a0 = FMA2((vec2f)t0[m], load2f(p), a0);
            a1 = FMA2((vec2f)t1[m], load2f(q), a1);
            c0 = FMA2((vec2f)t0[m + 1], load2f(p + next), c0);
            c1 = FMA2((vec2f)t1[m + 1], load2f(q + next), c1);
        }
        store2f(s0 + ch, a0 + c0);
        store2f(s1 + ch, a1 + c1);
    }
    for (; ch < channels; ch++) {
        const float *p = x0 + ch, *q = x1 + ch;
        float a0 = 0.0f, c0 = 0.0f, a1 = 0.0f, c1 = 0.0f;

        for (m = 0; m < taps; m += 2, p += 2 * next, q += 2 * next) {
            a0 = fmaf(t0[m], p[0], a0);
            a1 = fmaf(t1[m], q[0], a1);
            c0 = fmaf(t0[m + 1], p[next], c0);
            c1 = fmaf(t1[m + 1], q[next], c1);
        }
        s0[ch] = a0 + c0;
        s1[ch] = a1 + c1;
    }
}

/* to_24_bits for a frame of float sums, written so the compiler works on four at a time.
   Dividing by 256 and rounding are exact in single precision, so the results are the same.
   Returns how many of the frame's samples were beyond 24 bits' range and were cut to it. */
static unsigned to_24_bits_f(const float *sums, int32_t *out, unsigned channels)
{
    unsigned ch, cut = 0;

    for (ch = 0; ch < channels; ch++) {
        float units = roundf(sums[ch] * (1.0f / 256.0f));

        cut += (unsigned)(units > 8388607.0f) + (unsigned)(units < -8388608.0f);
        units = units >= 8388607.0f ? 8388607.0f : units;
        units = units <= -8388608.0f ? -8388608.0f : units;
        out[ch] = (int32_t)units * 256;
    }
    return cut;
}

/* The largest size among a frame's sums, in 24-bit units, as to_24_bits_f rounds them. Only
   called for a frame that had something cut, so it costs nothing otherwise. */
static float largest_f(const float *sums, unsigned channels)
{
    float most = 0.0f;
    unsigned ch;

    for (ch = 0; ch < channels; ch++) {
        float size = fabsf(roundf(sums[ch] * (1.0f / 256.0f)));

        most = size > most ? size : most; /* a NaN is never larger */
    }
    return most;
}

/* A frame's sums to 24 bits, with what was cut noted in the counts. */
static void give_frame(ua101_live *l, const float *sums, int32_t *out, unsigned channels)
{
    unsigned cut = to_24_bits_f(sums, out, channels);

    if (cut != 0) {
        float most = largest_f(sums, channels);

        l->cut += cut;
        if (most > l->cut_peak)
            l->cut_peak = most;
    }
}

static int step_to_fixed(double step, uint64_t *fixed)
{
    if (!(step >= UA101_LIVE_MIN_STEP) || !(step <= UA101_LIVE_MAX_STEP))
        return 0;
    *fixed = (uint64_t)llround(step * (double)(UINT64_C(1) << FRACTION_BITS));
    return 1;
}

int ua101_live_init(ua101_live *l, const ua101_resampler *filter, unsigned channels,
                    size_t capacity_frames)
{
    memset(l, 0, sizeof(*l));
    if (filter == NULL || filter->rows_f == NULL || channels == 0 || channels > MAX_CHANNELS ||
        filter->half_taps < 1 || filter->half_taps > UA101_RESAMPLE_HALF_TAPS ||
        capacity_frames <= 2 * (size_t)filter->half_taps || capacity_frames > ((size_t)1 << 30))
        return 0;
    if (!step_to_fixed(filter->step, &l->step))
        return 0;
    l->held = allocate(capacity_frames * channels * sizeof(float));
    if (l->held == NULL)
        return 0;
    l->filter = filter;
    l->half_taps = filter->half_taps;
    l->channels = channels;
    l->capacity = capacity_frames;
    /* Silence before the input, so the first output frame sits at input frame 0. */
    memset(l->held, 0, (size_t)(l->half_taps - 1) * channels * sizeof(float));
    l->filled = l->half_taps - 1;
    l->position = (uint64_t)(l->half_taps - 1) << FRACTION_BITS;
    return 1;
}

void ua101_live_free(ua101_live *l)
{
    free(l->held);
    l->held = NULL;
    l->filled = 0;
}

int ua101_live_set_step(ua101_live *l, double step)
{
    return step_to_fixed(step, &l->step);
}

double ua101_live_get_step(const ua101_live *l)
{
    return (double)l->step / (double)(UINT64_C(1) << FRACTION_BITS);
}

size_t ua101_live_room(const ua101_live *l)
{
    return l->held == NULL ? 0 : l->capacity - l->filled;
}

size_t ua101_live_push(ua101_live *l, const int32_t *input, size_t frames)
{
    size_t room = ua101_live_room(l), i, values;
    float *to;

    if (frames > room)
        frames = room;
    values = frames * l->channels;
    to = l->held + l->filled * l->channels;
    for (i = 0; i < values; i++)
        to[i] = (float)input[i]; /* exact for 24-bit samples */
    l->filled += frames;
    l->pushed += frames;
    return frames;
}

size_t ua101_live_push_silence(ua101_live *l, size_t frames)
{
    size_t room = ua101_live_room(l);

    if (frames > room)
        frames = room;
    memset(l->held + l->filled * l->channels, 0, frames * l->channels * sizeof(float));
    l->filled += frames;
    l->pushed += frames;
    return frames;
}

size_t ua101_live_available(const ua101_live *l)
{
    uint64_t limit;

    /* An output frame needs held frames up to half_taps past its own. */
    if (l->held == NULL || l->filled <= l->half_taps)
        return 0;
    limit = (uint64_t)(l->filled - l->half_taps) << FRACTION_BITS;
    if (l->position >= limit)
        return 0;
    return (size_t)((limit - l->position + l->step - 1) / l->step);
}

size_t ua101_live_pull(ua101_live *l, int32_t *output, size_t frames)
{
    size_t n = ua101_live_available(l), k, drop;
    const unsigned channels = l->channels;
    const int taps = 2 * (int)l->half_taps;
    const size_t lead = l->half_taps - 1;

    if (frames < n)
        n = frames;
    for (k = 0; k < n; k += 2) {
        float taps0[TAPS], taps1[TAPS], sums0[MAX_CHANNELS], sums1[MAX_CHANNELS];
        int pair = k + 1 < n;
        uint64_t p0 = l->position + (uint64_t)k * l->step;
        uint64_t p1 = pair ? p0 + l->step : p0;
        const float *x0 = l->held + ((size_t)(p0 >> FRACTION_BITS) - lead) * channels;
        const float *x1 = l->held + ((size_t)(p1 >> FRACTION_BITS) - lead) * channels;
        int32_t *out = output + k * channels;

        live_taps(l->filter, p0, taps0, taps);
        /* An odd frame at the end is simply worked out twice, as in ua101_resampler_run. */
        if (pair)
            live_taps(l->filter, p1, taps1, taps);
        dot_pair_f(taps0, x0, pair ? taps1 : taps0, x1, channels, taps, sums0, sums1);
        give_frame(l, sums0, out, channels);
        if (pair)
            give_frame(l, sums1, out + channels, channels);
    }
    l->position += (uint64_t)n * l->step;
    l->pulled += n;

    /* Frames the next output can't reach are let go, keeping its whole frame at `lead`. */
    drop = (size_t)(l->position >> FRACTION_BITS) - lead;
    if (drop > 0) {
        memmove(l->held, l->held + drop * channels, (l->filled - drop) * channels * sizeof(float));
        l->filled -= drop;
        l->position -= (uint64_t)drop << FRACTION_BITS;
    }
    return n;
}

void ua101_live_reset(ua101_live *l)
{
    if (l->held == NULL)
        return;
    step_to_fixed(l->filter->step, &l->step);
    memset(l->held, 0, (size_t)(l->half_taps - 1) * l->channels * sizeof(float));
    l->filled = l->half_taps - 1;
    l->position = (uint64_t)(l->half_taps - 1) << FRACTION_BITS;
    l->pushed = 0;
    l->origin = 0;
    l->pulled = 0;
}

double ua101_live_next_position(const ua101_live *l)
{
    /* held[i] is input frame i - filled + pushed, so the position maps across the same way. */
    return (double)(l->position >> FRACTION_BITS) +
           (double)(l->position & FRACTION_MASK) / (double)(UINT64_C(1) << FRACTION_BITS) +
           (double)(l->pushed - l->origin) - (double)l->filled;
}

uint64_t ua101_live_rebase(ua101_live *l, uint64_t at_least)
{
    uint64_t moved = l->pushed - l->origin;

    if (moved == 0 || moved < at_least)
        return 0;
    l->origin = l->pushed;
    return moved;
}

size_t ua101_live_needed(const ua101_live *l, size_t frames)
{
    uint64_t last, want;

    if (l->held == NULL || frames == 0)
        return 0;
    /* The last of them sits here, and needs held frames up to half_taps past its own, as
       ua101_live_available counts them. */
    last = (l->position + (uint64_t)(frames - 1) * l->step) >> FRACTION_BITS;
    want = last + l->half_taps + 1;
    return want > l->filled ? (size_t)(want - l->filled) : 0;
}

#ifdef UA101_RESAMPLE_TESTING
/* Lets the tests compare the two versions directly. */
void ua101_resampler_run_plain(const ua101_resampler *r, const int32_t *input,
                               uint64_t input_frames, unsigned channels, uint64_t first,
                               size_t count, int32_t *output)
{
    if (channels == 0 || channels > MAX_CHANNELS)
        return;
    run_plain(r, input, input_frames, channels, first, count, output);
}
#endif
