#include "ua101_analysis.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define FULL_SCALE 2147483648.0
#define SILENT_DBFS (-200.0)

/* A click stands this far above the channel's normal second difference. */
#define CLICK_RATIO 16.0
/* Below about -120 dBFS, 24-bit audio is only its last few bits. */
#define CLICK_FLOOR (1e-6 * FULL_SCALE)

static double to_db(double linear)
{
    return linear > 1e-10 ? 20.0 * log10(linear) : SILENT_DBFS;
}

void ua101_channel_analyze(const int32_t *samples, size_t frames, unsigned channels,
                           unsigned ch, double rate_hz, ua101_channel_stats *out)
{
    size_t block = (size_t)(rate_hz * 0.1);
    double sum = 0.0, sum_sq = 0.0, sum_d2 = 0.0, block_sq = 0.0;
    double min_block = -1.0, max_block = 0.0, threshold;
    int64_t peak = 0;
    size_t i, in_block = 0;

    memset(out, 0, sizeof(*out));
    if (block == 0)
        block = 1;

    for (i = 0; i < frames; i++) {
        int32_t raw = samples[i * channels + ch];
        double x = (double)raw;
        int64_t mag = raw < 0 ? -(int64_t)raw : (int64_t)raw;

        if (mag > peak)
            peak = mag;
        if (raw & 0xFF)
            out->low_byte_nonzero++;
        sum += x;
        sum_sq += x * x;
        block_sq += x * x;
        if (i >= 2) {
            double d2 = x - 2.0 * (double)samples[(i - 1) * channels + ch] +
                        (double)samples[(i - 2) * channels + ch];
            sum_d2 += d2 * d2;
        }
        if (++in_block == block) {
            double rms = sqrt(block_sq / (double)block);

            if (min_block < 0.0 || rms < min_block)
                min_block = rms;
            if (rms > max_block)
                max_block = rms;
            block_sq = 0.0;
            in_block = 0;
        }
    }
    if (frames == 0)
        return;

    out->peak_dbfs = to_db((double)peak / FULL_SCALE);
    out->rms_dbfs = to_db(sqrt(sum_sq / (double)frames) / FULL_SCALE);
    out->dc_dbfs = to_db(fabs(sum / (double)frames) / FULL_SCALE);
    out->min_block_rms_dbfs = to_db((min_block < 0.0 ? 0.0 : min_block) / FULL_SCALE);
    out->max_block_rms_dbfs = to_db(max_block / FULL_SCALE);

    threshold = frames > 2 ? CLICK_RATIO * sqrt(sum_d2 / (double)(frames - 2)) : 0.0;
    if (threshold < CLICK_FLOOR)
        threshold = CLICK_FLOOR;
    for (i = 2; i < frames; i++) {
        double d2 = (double)samples[i * channels + ch] -
                    2.0 * (double)samples[(i - 1) * channels + ch] +
                    (double)samples[(i - 2) * channels + ch];

        if (fabs(d2) > threshold) {
            if (out->clicks == 0)
                out->first_click = i;
            out->clicks++;
        }
    }
}

double ua101_goertzel_dbfs(const int32_t *samples, size_t first, size_t count,
                           unsigned channels, unsigned ch, double freq_hz, double rate_hz)
{
    double w = 2.0 * M_PI * freq_hz / rate_hz;
    double re = 0.0, im = 0.0;
    size_t i;

    if (count == 0)
        return SILENT_DBFS;
    for (i = 0; i < count; i++) {
        double x = (double)samples[(first + i) * channels + ch] / FULL_SCALE;
        double angle = w * (double)i;

        re += x * cos(angle);
        im += x * sin(angle);
    }
    return to_db(2.0 * sqrt(re * re + im * im) / (double)count);
}

/*
 * The score at each start is the correlation with the whole tone. Between the
 * fades the tone is a pure sine, so that part of the correlation is the
 * imaginary part of a sum of in[m] * e^(i w (m - start)), which a running
 * (prefix) sum gives in one step for every start. Only the fades are summed
 * sample by sample. This finds the same frame as correlating every sample, at
 * any rate and tone length, without the cost growing with the tone's length.
 */
int64_t ua101_tone_locate(const int32_t *samples, size_t frames, unsigned channels,
                          const ua101_tone *tone, const unsigned ch_for_tone[UA101_TONE_CHANNELS],
                          int64_t guess, int64_t radius)
{
    size_t length = (size_t)tone->length, fade = (size_t)tone->fade, span, count, n, s, k;
    int64_t lo = guess - radius, hi = guess + radius, best_start = -1;
    double best = -1.0;
    double *pre_re, *pre_im, *rot_re, *rot_im, *env, *scores;
    unsigned c;

    if (length == 0 || frames < length || radius < 0)
        return -1;
    if (lo < 0)
        lo = 0;
    if (hi > (int64_t)(frames - length))
        hi = (int64_t)(frames - length);
    if (hi < lo)
        return -1;
    if (fade * 2 > length)
        fade = length / 2;
    /* The frames the search can touch: from lo to the end of a tone starting at hi. */
    count = (size_t)(hi - lo) + 1;
    span = count - 1 + length;
    pre_re = malloc((span + 1) * sizeof(double));
    pre_im = malloc((span + 1) * sizeof(double));
    rot_re = malloc(span * sizeof(double));
    rot_im = malloc(span * sizeof(double));
    env = malloc((fade + 1) * sizeof(double));
    scores = calloc(count, sizeof(double));
    if (pre_re == NULL || pre_im == NULL || rot_re == NULL || rot_im == NULL || env == NULL ||
        scores == NULL)
        goto done;
    /* The fade-out mirrors the fade-in. */
    for (k = 0; k < fade; k++)
        env[k] = ua101_tone_envelope(tone, k);

    for (c = 0; c < UA101_TONE_CHANNELS; c++) {
        const int32_t *in = samples + ch_for_tone[c];
        double f = tone->freq_hz[c], rate = tone->rate_hz;

        /* e^(i w m) for each frame, with the phase reduced first so it stays exact. */
        pre_re[0] = pre_im[0] = 0.0;
        for (n = 0; n < span; n++) {
            size_t m = (size_t)lo + n;
            double turns = fmod((double)m * f, rate) / rate;
            double x = (double)in[m * channels];

            rot_re[n] = cos(2.0 * M_PI * turns);
            rot_im[n] = sin(2.0 * M_PI * turns);
            pre_re[n + 1] = pre_re[n] + x * rot_re[n];
            pre_im[n + 1] = pre_im[n] + x * rot_im[n];
        }
        for (s = 0; s < count; s++) {
            double sr = 0.0, si = 0.0, turns, cr, ci;

            for (k = 0; k < fade; k++) {
                size_t m1 = s + k, m2 = s + length - 1 - k;
                double x1 = (double)in[((size_t)lo + m1) * channels] * env[k];
                double x2 = (double)in[((size_t)lo + m2) * channels] * env[k];

                sr += x1 * rot_re[m1] + x2 * rot_re[m2];
                si += x1 * rot_im[m1] + x2 * rot_im[m2];
            }
            if (length > 2 * fade) {
                sr += pre_re[s + length - fade] - pre_re[s + fade];
                si += pre_im[s + length - fade] - pre_im[s + fade];
            }
            /* Times e^(-i w start), imaginary part: the sum of x * sin(w (m - start)). */
            turns = fmod((double)((size_t)lo + s) * f, rate) / rate;
            cr = cos(2.0 * M_PI * turns);
            ci = -sin(2.0 * M_PI * turns);
            scores[s] += tone->amplitude * (sr * ci + si * cr);
        }
    }
    for (s = 0; s < count; s++) {
        if (scores[s] > best) {
            best = scores[s];
            best_start = lo + (int64_t)s;
        }
    }
done:
    free(pre_re);
    free(pre_im);
    free(rot_re);
    free(rot_im);
    free(env);
    free(scores);
    return best_start;
}

void ua101_tone_measure(const int32_t *samples, size_t frames, unsigned channels, unsigned ch,
                        const ua101_tone *tone, unsigned tone_ch, size_t start,
                        ua101_tone_fit *out)
{
    double w = 2.0 * M_PI * tone->freq_hz[tone_ch] / tone->rate_hz;
    size_t length = (size_t)tone->length;
    size_t ms = (size_t)(tone->rate_hz / 1000.0 + 0.5);
    size_t blocks10 = ms * 10;
    double ss = 0.0, cc = 0.0, sc = 0.0, xs = 0.0, xc = 0.0;
    double a, b, det, amp, tone_rms, phase;
    double err_sq = 0.0, worst_sq = 0.0, block_sq = 0.0, max_dev = 0.0;
    size_t n, in_block = 0;

    memset(out, 0, sizeof(*out));
    out->level_dbfs = SILENT_DBFS;
    out->residual_db = 0.0;
    out->worst_block_db = 0.0;
    out->slip_limit_deg = 0.5 * 360.0 * tone->freq_hz[tone_ch] / tone->rate_hz;
    if (start + length > frames || length == 0 || ms == 0)
        return;

    /* Fits the tone, with its fades, at any phase, so a fractional delay still fits. */
    for (n = 0; n < length; n++) {
        double env = ua101_tone_envelope(tone, n);
        double s = env * sin(w * (double)n);
        double c = env * cos(w * (double)n);
        double x = (double)samples[(start + n) * channels + ch] / FULL_SCALE;

        ss += s * s;
        cc += c * c;
        sc += s * c;
        xs += x * s;
        xc += x * c;
    }
    det = ss * cc - sc * sc;
    if (fabs(det) < 1e-12)
        return;
    a = (xs * cc - xc * sc) / det;
    b = (xc * ss - xs * sc) / det;
    amp = sqrt(a * a + b * b);
    if (amp < 1e-12)
        return;
    tone_rms = amp / sqrt(2.0);
    phase = atan2(b, a);

    out->level_dbfs = to_db(amp);
    out->delay_fraction = -phase / w;

    for (n = 0; n < length; n++) {
        double env = ua101_tone_envelope(tone, n);
        double fit = env * (a * sin(w * (double)n) + b * cos(w * (double)n));
        double x = (double)samples[(start + n) * channels + ch] / FULL_SCALE;
        double e = x - fit;

        err_sq += e * e;
        block_sq += e * e;
        if (++in_block == ms) {
            if (block_sq / (double)ms > worst_sq)
                worst_sq = block_sq / (double)ms;
            block_sq = 0.0;
            in_block = 0;
        }
    }
    out->residual_db = to_db(sqrt(err_sq / (double)length) / tone_rms);
    out->worst_block_db = to_db(sqrt(worst_sq) / tone_rms);

    /* Phase in each 10 ms block of the steady part. A lost sample shows as a step. */
    for (n = (size_t)tone->fade; n + blocks10 + (size_t)tone->fade <= length; n += blocks10) {
        double re = 0.0, im = 0.0, dev;
        size_t i;

        for (i = 0; i < blocks10; i++) {
            double x = (double)samples[(start + n + i) * channels + ch] / FULL_SCALE;
            double angle = w * (double)(n + i);

            re += x * sin(angle);
            im += x * cos(angle);
        }
        dev = fabs(atan2(im, re) - phase);
        if (dev > M_PI)
            dev = 2.0 * M_PI - dev;
        if (dev > max_dev)
            max_dev = dev;
    }
    out->max_phase_dev_deg = max_dev * 180.0 / M_PI;

    /*
     * Clean means no sample was lost or repeated inside the tone, and no millisecond
     * stands out from the rest of the residual, which is normally just the noise floor.
     */
    out->clean = out->max_phase_dev_deg < out->slip_limit_deg &&
                 out->worst_block_db < out->residual_db + 15.0;
}

void ua101_mix_recover(const int32_t *samples, size_t frames, int32_t *recovered)
{
    size_t f;

    for (f = 0; f < frames; f++) {
        const int32_t *frame = samples + f * UA101_MIX_CHANNELS;
        unsigned side;

        for (side = 0; side < UA101_MIX_OUTPUTS; side++) {
            int64_t value = (int64_t)frame[10 + side];
            unsigned input;

            for (input = side; input < 10; input += 2)
                value -= frame[input];
            if (value > INT32_MAX)
                value = INT32_MAX;
            else if (value < INT32_MIN)
                value = INT32_MIN;
            recovered[f * UA101_MIX_OUTPUTS + side] = (int32_t)value;
        }
    }
}

static void note_difference(ua101_exact_check *check, size_t position, int64_t difference)
{
    int64_t size = difference < 0 ? -difference : difference;

    if (size == 0)
        return;
    if (check->different == 0)
        check->first_different = position;
    check->different++;
    if (size > check->largest)
        check->largest = size;
}

void ua101_tone_compare(const int32_t *recovered, size_t frames, unsigned output,
                        const ua101_tone *tone, size_t start, ua101_exact_check *out)
{
    size_t length = (size_t)tone->length;
    size_t n;

    memset(out, 0, sizeof(*out));
    for (n = 0; n < length && start + n < frames; n++) {
        int64_t got = recovered[(start + n) * UA101_MIX_OUTPUTS + output];

        note_difference(out, n, got - ua101_tone_word(tone, output, n));
        out->compared++;
    }
    /* A tone cut off by the end of the recording can't count as a match. */
    if (out->compared < length) {
        note_difference(out, out->compared, 1);
        out->different += length - out->compared - 1;
    }
}

void ua101_silence_check(const int32_t *recovered, size_t first, size_t last, unsigned output,
                         ua101_exact_check *out)
{
    size_t f;

    for (f = first; f < last; f++) {
        note_difference(out, f, recovered[f * UA101_MIX_OUTPUTS + output]);
        out->compared++;
    }
}
