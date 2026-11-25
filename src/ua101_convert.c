#include "ua101_convert.h"

#include <math.h>

void ua101_capture_to_float(const int32_t *in, float *out, size_t samples)
{
    const float scale = 1.0f / 2147483648.0f; /* 2^-31: a power of two, so exact */
    size_t i;

    for (i = 0; i < samples; i++)
        out[i] = (float)in[i] * scale;
}

/* Written without branches, so the compiler works on four samples at a time. */
size_t ua101_float_to_playback(const float *in, int32_t *out, size_t samples)
{
    const float full = 8388608.0f; /* 2^23 */
    const float top = 8388607.0f, bottom = -8388608.0f;
    size_t i;
    uint32_t clipped = 0;

    for (i = 0; i < samples; i++) {
        float x = in[i] * full;

        x = x == x ? x : 0.0f; /* NaN */
        clipped += (uint32_t)(x > top) + (uint32_t)(x < bottom);
        x = x > top ? top : x;
        x = x < bottom ? bottom : x;
        /* Rounded to the nearest whole number, halves to even, as lrintf does. Shifting a
           negative value left is undefined in C, so the top bytes are made from the unsigned
           bits. */
        out[i] = (int32_t)((uint32_t)(int32_t)rintf(x) << 8);
    }
    return clipped;
}

float ua101_float_peak(const float *in, size_t samples)
{
    float most = 0.0f;
    size_t i;

    for (i = 0; i < samples; i++) {
        float size = fabsf(in[i]);

        size = size == size ? size : 0.0f; /* NaN */
        most = size > most ? size : most;
    }
    return most;
}
