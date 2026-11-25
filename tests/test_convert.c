/* Sample conversion: every 24-bit value through the plug-in's path and back, and the edges. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../src/ua101_convert.h"

static int failures;

#define CHECK(cond, ...)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                            \
            printf(__VA_ARGS__);                                                                   \
            printf("\n");                                                                          \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

int main(void)
{
    enum { N = 1 << 24 };
    int32_t *in = malloc(N * sizeof(int32_t)), *back = malloc(N * sizeof(int32_t));
    float *f = malloc(N * sizeof(float));
    size_t i, clipped, bad = 0, bad_float = 0;
    float edges[] = {1.0f, 1.5f, -1.0f, -1.5f, INFINITY, -INFINITY, NAN, 1e-9f, -1e-9f,
                     0.5f / 8388608.0f, 1.5f / 8388608.0f, -0.5f / 8388608.0f};
    int32_t want[] = {8388607, 8388607, -8388608, -8388608, 8388607, -8388608, 0, 0, 0,
                      0, 2, 0};
    int32_t out[sizeof(edges) / sizeof(edges[0])];

    if (in == NULL || back == NULL || f == NULL)
        return 1;

    /* Every value the unit can send, in the top 24 bits, with the low byte as the unit leaves
       it: zero. */
    for (i = 0; i < N; i++)
        in[i] = (int32_t)((uint32_t)i << 8);
    ua101_capture_to_float(in, f, N);
    for (i = 0; i < N; i++)
        if (f[i] != (float)((double)in[i] / 2147483648.0))
            bad_float++;
    CHECK(bad_float == 0, "%zu values didn't become x / 2^31", bad_float);
    CHECK(f[0] == 0.0f && f[1 << 23] == -1.0f && f[(1 << 23) - 1] == 1.0f - 1.0f / 8388608.0f,
          "the ends are 0, -1 and 1 - 2^-23");
    clipped = ua101_float_to_playback(f, back, N);
    for (i = 0; i < N; i++)
        if (back[i] != in[i])
            bad++;
    CHECK(bad == 0, "%zu of 2^24 values changed on the way through", bad);
    CHECK(clipped == 0, "%zu clipped, none should", clipped);

    /* Out of range, NaN, and rounding to the nearest step. */
    clipped = ua101_float_to_playback(edges, out, sizeof(edges) / sizeof(edges[0]));
    for (i = 0; i < sizeof(edges) / sizeof(edges[0]); i++)
        CHECK(out[i] == (int32_t)((uint32_t)want[i] << 8), "%g gave %d, want %d", (double)edges[i],
              out[i] >> 8, want[i]);
    /* 1.0, +-1.5 and +-inf; -1.0 is full scale exactly, and NaN isn't a clip. */
    CHECK(clipped == 5, "%zu clipped, want 5", clipped);

    /* The largest size, for saying how far over full scale an app's samples went. */
    {
        float some[] = {0.25f, -1.5f, 1.25f, NAN, -0.5f}, nans[] = {NAN, NAN};
        float inf[] = {0.5f, -INFINITY, 2.0f};

        CHECK(ua101_float_peak(some, 5) == 1.5f, "the largest size is 1.5, got %g",
              (double)ua101_float_peak(some, 5));
        CHECK(ua101_float_peak(some, 1) == 0.25f && ua101_float_peak(some + 2, 3) == 1.25f,
              "only the samples given are looked at");
        CHECK(ua101_float_peak(nans, 2) == 0.0f && ua101_float_peak(some, 0) == 0.0f,
              "NaN and nothing at all give 0");
        CHECK(isinf(ua101_float_peak(inf, 3)), "an infinity gives infinity");
        CHECK(ua101_float_peak(f, N) == 1.0f, "over every value the unit can send it is 1.0 (-1)");
    }

    free(in);
    free(back);
    free(f);
    if (failures == 0)
        printf("test_convert: all 2^24 values bit-exact; edges right\n");
    return failures == 0 ? 0 : 1;
}
