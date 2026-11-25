/*
 * Sample conversion between the unit and Core Audio. The unit's samples are 24 bits in the top
 * of 4-byte containers (src/ua101_stream.h); apps get 32-bit floats from -1 to just under 1.
 * A float holds 24 bits exactly, so a sample converted to float and back is unchanged: the path
 * through the plug-in is bit-exact. Contains no Apple APIs and allocates nothing.
 */
#ifndef UA101_CONVERT_H
#define UA101_CONVERT_H

#include <stddef.h>
#include <stdint.h>

/* Capture: each 4-byte sample to a float, x / 2^31, exact for 24-bit samples. */
void ua101_capture_to_float(const int32_t *in, float *out, size_t samples);

/* Playback: each float to a 24-bit sample in the top of 4 bytes, rounded to the nearest, with
   anything outside -1 to 1 - 2^-23 clipped and NaN taken as 0. Returns how many were clipped. */
size_t ua101_float_to_playback(const float *in, int32_t *out, size_t samples);

/* The largest size among these floats, where full scale is 1.0: for saying how far over full
   scale an app's samples went. NaN counts as 0; an infinity gives infinity. */
float ua101_float_peak(const float *in, size_t samples);

#endif
