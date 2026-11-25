/*
 * Streaming logic for the Edirol/Roland UA-101 that doesn't depend on Apple
 * APIs, so it can be unit-tested on any computer: scheduling arithmetic, the
 * packet-size FIFO behind implicit feedback, the isochronous buffer layout
 * check, the test tone and its safety limits, a lock-free ring and WAV headers.
 */
#ifndef UA101_STREAM_H
#define UA101_STREAM_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Bus frames (1 ms each) covered by a high-speed isochronous transfer. Returns
 * 0 when the transfer doesn't fill a whole number of bus frames, or the
 * interval isn't valid, because such a transfer can't be scheduled back to back.
 */
uint32_t ua101_iso_bus_frames(uint32_t packets, uint8_t interval);

/* Packet-size FIFO: capture sizes, in frames, replayed by playback. */
typedef struct {
    uint16_t *slots;
    uint32_t capacity;
    uint32_t head;
    uint32_t count;
    uint64_t pushed_frames;
    uint64_t popped_frames;
    uint64_t dropped_frames;
    uint64_t dropped_packets;
} ua101_size_fifo;

void ua101_fifo_init(ua101_size_fifo *fifo, uint16_t *slots, uint32_t capacity);
/* When the FIFO is full the oldest size is dropped, so playback sees recent ones. */
void ua101_fifo_push(ua101_size_fifo *fifo, uint16_t frames);
int ua101_fifo_pop(ua101_size_fifo *fifo, uint16_t *frames);
uint64_t ua101_fifo_held_frames(const ua101_size_fifo *fifo);

/*
 * Isochronous IN buffer layout. Each capture buffer is filled with a sentinel
 * before it's submitted, so afterwards the bytes the hardware wrote can be told
 * apart from the ones it didn't. The sentinel's low byte is not zero, and a
 * 24-bit sample in a 4-byte container has a zero low byte, so a sample can't be
 * mistaken for it.
 *
 * SLOT:   packet i starts at i * requested bytes, leaving gaps after short packets.
 * PACKED: packet i starts at the sum of the earlier actual byte counts.
 */
#define UA101_SENTINEL_WORD 0x5AA5A55Au

typedef enum {
    UA101_LAYOUT_UNKNOWN = 0,
    UA101_LAYOUT_SLOT,
    UA101_LAYOUT_PACKED
} ua101_layout;

typedef struct {
    uint64_t packets;
    uint64_t short_packets;
    uint64_t data_bytes;
    uint64_t slot_bad_words;   /* words that contradict the SLOT layout */
    uint64_t packed_bad_words; /* words that contradict the PACKED layout */
} ua101_layout_stats;

void ua101_sentinel_fill(uint8_t *buf, size_t bytes);
/* buf must be 4-byte aligned. Every packet was requested with slot_bytes. */
void ua101_layout_check(const uint8_t *buf, uint32_t packets, uint32_t slot_bytes,
                        const uint16_t *actual, ua101_layout_stats *stats);
/* UNKNOWN unless the evidence rules out exactly one of the two layouts. */
ua101_layout ua101_layout_verdict(const ua101_layout_stats *stats);
const char *ua101_layout_string(ua101_layout layout);

/*
 * Test tone. The limits are compile-time on purpose: no command-line option can
 * make the tone loud or long. Output 1 gets the first frequency and output 2
 * the second, which also shows the channel order in a recording.
 */
#define UA101_TONE_MAX_DBFS (-40.0)
#define UA101_TONE_MAX_SECONDS 5.0
#define UA101_TONE_FADE_SECONDS 0.02
#define UA101_TONE_CHANNELS 2

typedef struct {
    double rate_hz;
    double freq_hz[UA101_TONE_CHANNELS];
    double amplitude; /* linear, 1.0 is full scale */
    uint64_t start;   /* first frame of the tone in the output stream */
    uint64_t length;  /* frames */
    uint64_t fade;    /* frames at each end */
} ua101_tone;

/* Returns 0, or -1 when a value is outside the limits and the tone is refused. */
int ua101_tone_init(ua101_tone *tone, double rate_hz, double dbfs, double seconds,
                    double freq0_hz, double freq1_hz, uint64_t start);
/* Fade gain from 0 to 1 for frame n of the tone, counted from its start. */
double ua101_tone_envelope(const ua101_tone *tone, uint64_t n);
/* Linear sample value on tone channel ch at frame n of the tone. */
double ua101_tone_value(const ua101_tone *tone, unsigned ch, uint64_t n);
/* The same sample exactly as it's sent: 24 bits in the top of a signed 32-bit word. */
int32_t ua101_tone_word(const ua101_tone *tone, unsigned ch, uint64_t n);
/*
 * Writes the part of the tone that falls in this block into interleaved 4-byte
 * containers (24-bit little-endian in the top three bytes). Other channels and
 * frames are left as they are, so the caller must start from silence.
 */
void ua101_tone_mix(const ua101_tone *tone, uint8_t *dst, uint32_t frames,
                    uint32_t channels, uint64_t first_frame);

/*
 * The jack map: every output plays a frequency of its own at the same time, so a
 * recording made with loop cables shows which output reached which input. The tone's
 * level, length, fades and start are used, and its two frequencies are ignored.
 * The frequencies are spread out and none is near a low harmonic of another.
 */
#define UA101_JACK_OUTPUTS 10
/* Output counted from 0. Returns 0 for an output beyond the table. */
double ua101_jack_freq(unsigned output);
/* Like ua101_tone_mix, with ua101_jack_freq(ch) on each of the first `outputs` channels. */
void ua101_jack_mix(const ua101_tone *tone, uint8_t *dst, uint32_t frames, uint32_t channels,
                    unsigned outputs, uint64_t first_frame);

/* Single-producer, single-consumer byte ring. Never blocks the producer. */
typedef struct {
    uint8_t *data;
    size_t capacity;
    _Atomic size_t head; /* total bytes written */
    _Atomic size_t tail; /* total bytes read */
} ua101_ring;

void ua101_ring_init(ua101_ring *ring, uint8_t *data, size_t capacity);
/* Returns 1 when all the bytes were stored, 0 when none were because they don't fit. */
int ua101_ring_write(ua101_ring *ring, const void *src, size_t bytes);
/* Returns how many contiguous bytes can be read now, and where they are. */
size_t ua101_ring_peek(ua101_ring *ring, const uint8_t **ptr);
void ua101_ring_consume(ua101_ring *ring, size_t bytes);

/*
 * Uncertainty of a rate found by counting frames over USB bus time, in ppm: one
 * packet more or less at either end, at 8000 packets per second. It depends only
 * on the time measured, not on the frames received, so a run that delivers no
 * audio can't widen it enough to pass.
 */
double ua101_rate_margin_ppm(double seconds);

/*
 * Uncertainty of the USB bus against the host clock, in ppm, from two
 * GetBusFrameNumberWithTime timestamps this many seconds apart.
 */
double ua101_host_margin_ppm(double seconds);

/*
 * Compares a rate with a limit, allowing for its margin. UNDECIDED means the margin
 * reaches both sides of the limit, so the run was too short to tell. A measurement
 * that couldn't be made counts as outside.
 */
typedef enum {
    UA101_RATE_INSIDE,
    UA101_RATE_OUTSIDE,
    UA101_RATE_UNDECIDED
} ua101_rate_result;

ua101_rate_result ua101_rate_verdict(double ppm, double margin_ppm, double limit_ppm);

/* WAVE_FORMAT_EXTENSIBLE header, needed for more than 2 channels or 16 bits. */
#define UA101_WAV_HEADER_BYTES 68
#define UA101_WAV_MAX_DATA_BYTES 0xF0000000ull

void ua101_wav_header(uint8_t out[UA101_WAV_HEADER_BYTES], uint16_t channels,
                      uint32_t rate_hz, uint16_t container_bits, uint16_t valid_bits,
                      uint64_t data_bytes);

#endif
