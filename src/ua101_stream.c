#include "ua101_stream.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

uint32_t ua101_iso_bus_frames(uint32_t packets, uint8_t interval)
{
    uint32_t microframes;

    if (interval < 1 || interval > 4 || packets == 0)
        return 0;
    microframes = packets * (1u << (interval - 1));
    return (microframes % 8 == 0) ? microframes / 8 : 0;
}

void ua101_fifo_init(ua101_size_fifo *fifo, uint16_t *slots, uint32_t capacity)
{
    memset(fifo, 0, sizeof(*fifo));
    fifo->slots = slots;
    fifo->capacity = capacity;
}

void ua101_fifo_push(ua101_size_fifo *fifo, uint16_t frames)
{
    if (fifo->count == fifo->capacity) {
        fifo->dropped_frames += fifo->slots[fifo->head];
        fifo->dropped_packets++;
        fifo->head = (fifo->head + 1) % fifo->capacity;
        fifo->count--;
    }
    fifo->slots[(fifo->head + fifo->count) % fifo->capacity] = frames;
    fifo->count++;
    fifo->pushed_frames += frames;
}

int ua101_fifo_pop(ua101_size_fifo *fifo, uint16_t *frames)
{
    if (fifo->count == 0)
        return 0;
    *frames = fifo->slots[fifo->head];
    fifo->head = (fifo->head + 1) % fifo->capacity;
    fifo->count--;
    fifo->popped_frames += *frames;
    return 1;
}

uint64_t ua101_fifo_held_frames(const ua101_size_fifo *fifo)
{
    uint64_t held = 0;
    uint32_t i;

    for (i = 0; i < fifo->count; i++)
        held += fifo->slots[(fifo->head + i) % fifo->capacity];
    return held;
}

void ua101_sentinel_fill(uint8_t *buf, size_t bytes)
{
    const uint32_t word = UA101_SENTINEL_WORD;
    size_t i;

    for (i = 0; i + 4 <= bytes; i += 4)
        memcpy(buf + i, &word, 4);
}

static int is_sentinel(const uint8_t *buf, size_t offset)
{
    uint32_t word;

    memcpy(&word, buf + offset, 4);
    return word == UA101_SENTINEL_WORD;
}

void ua101_layout_check(const uint8_t *buf, uint32_t packets, uint32_t slot_bytes,
                        const uint16_t *actual, ua101_layout_stats *stats)
{
    size_t total = (size_t)packets * slot_bytes;
    size_t packed_end = 0;
    size_t offset;
    uint32_t p;

    for (p = 0; p < packets; p++) {
        size_t start = (size_t)p * slot_bytes;
        size_t act = actual[p] > slot_bytes ? slot_bytes : actual[p];

        /* Under SLOT the data fills the start of each slot and the rest is untouched. */
        for (offset = 0; offset + 4 <= slot_bytes; offset += 4)
            if (is_sentinel(buf, start + offset) != (offset >= act))
                stats->slot_bad_words++;

        stats->packets++;
        if (act < slot_bytes)
            stats->short_packets++;
        stats->data_bytes += act;
        packed_end += act;
    }

    /* Under PACKED the data is one run at the start of the buffer. */
    for (offset = 0; offset + 4 <= total; offset += 4)
        if (is_sentinel(buf, offset) != (offset >= packed_end))
            stats->packed_bad_words++;
}

ua101_layout ua101_layout_verdict(const ua101_layout_stats *stats)
{
    if (stats->data_bytes == 0)
        return UA101_LAYOUT_UNKNOWN;
    if (stats->slot_bad_words == 0 && stats->packed_bad_words > 0)
        return UA101_LAYOUT_SLOT;
    if (stats->packed_bad_words == 0 && stats->slot_bad_words > 0)
        return UA101_LAYOUT_PACKED;
    return UA101_LAYOUT_UNKNOWN;
}

const char *ua101_layout_string(ua101_layout layout)
{
    switch (layout) {
    case UA101_LAYOUT_SLOT:
        return "slot (each packet at its requested offset)";
    case UA101_LAYOUT_PACKED:
        return "packed (packets contiguous)";
    default:
        return "unknown";
    }
}

int ua101_tone_init(ua101_tone *tone, double rate_hz, double dbfs, double seconds,
                    double freq0_hz, double freq1_hz, uint64_t start)
{
    /* Written so that a NaN fails each comparison and is refused. */
    if (!(rate_hz > 0.0))
        return -1;
    if (!(dbfs <= UA101_TONE_MAX_DBFS) || !(dbfs >= -140.0))
        return -1;
    if (!(seconds > 0.0) || !(seconds <= UA101_TONE_MAX_SECONDS))
        return -1;
    if (!(freq0_hz > 0.0) || !(freq0_hz < rate_hz / 2.0))
        return -1;
    if (!(freq1_hz > 0.0) || !(freq1_hz < rate_hz / 2.0))
        return -1;

    tone->rate_hz = rate_hz;
    tone->freq_hz[0] = freq0_hz;
    tone->freq_hz[1] = freq1_hz;
    tone->amplitude = pow(10.0, dbfs / 20.0);
    tone->start = start;
    tone->length = (uint64_t)(seconds * rate_hz + 0.5);
    tone->fade = (uint64_t)(UA101_TONE_FADE_SECONDS * rate_hz + 0.5);
    if (tone->fade * 2 > tone->length)
        tone->fade = tone->length / 2;
    return 0;
}

double ua101_tone_envelope(const ua101_tone *tone, uint64_t n)
{
    uint64_t edge;

    if (n >= tone->length)
        return 0.0;
    edge = n < tone->length - 1 - n ? n : tone->length - 1 - n;
    if (edge >= tone->fade)
        return 1.0;
    return 0.5 - 0.5 * cos(M_PI * ((double)edge + 0.5) / (double)tone->fade);
}

static double tone_value_at(const ua101_tone *tone, double freq_hz, uint64_t n)
{
    double phase = 2.0 * M_PI * freq_hz * (double)n / tone->rate_hz;

    return tone->amplitude * ua101_tone_envelope(tone, n) * sin(phase);
}

static int32_t word_from_value(double value)
{
    return (int32_t)((uint32_t)(int32_t)lround(value * 8388607.0) << 8);
}

static void put_word(uint8_t *out, uint32_t word)
{
    out[0] = (uint8_t)word;
    out[1] = (uint8_t)(word >> 8);
    out[2] = (uint8_t)(word >> 16);
    out[3] = (uint8_t)(word >> 24);
}

double ua101_tone_value(const ua101_tone *tone, unsigned ch, uint64_t n)
{
    if (ch >= UA101_TONE_CHANNELS)
        return 0.0;
    return tone_value_at(tone, tone->freq_hz[ch], n);
}

int32_t ua101_tone_word(const ua101_tone *tone, unsigned ch, uint64_t n)
{
    return word_from_value(ua101_tone_value(tone, ch, n));
}

double ua101_jack_freq(unsigned output)
{
    /* Primes, at least 27 Hz from any second or third harmonic of another. */
    static const double freqs[UA101_JACK_OUTPUTS] = {401.0,  523.0,  661.0,  829.0,  937.0,
                                                     1103.0, 1259.0, 1433.0, 1601.0, 1789.0};

    return output < UA101_JACK_OUTPUTS ? freqs[output] : 0.0;
}

void ua101_jack_mix(const ua101_tone *tone, uint8_t *dst, uint32_t frames, uint32_t channels,
                    unsigned outputs, uint64_t first_frame)
{
    uint32_t f;

    if (outputs > channels)
        outputs = channels;
    if (outputs > UA101_JACK_OUTPUTS)
        outputs = UA101_JACK_OUTPUTS;
    for (f = 0; f < frames; f++) {
        uint64_t pos = first_frame + f;
        unsigned ch;

        if (pos < tone->start || pos >= tone->start + tone->length)
            continue;
        for (ch = 0; ch < outputs; ch++)
            put_word(dst + ((size_t)f * channels + ch) * 4,
                     (uint32_t)word_from_value(
                         tone_value_at(tone, ua101_jack_freq(ch), pos - tone->start)));
    }
}

void ua101_tone_mix(const ua101_tone *tone, uint8_t *dst, uint32_t frames,
                    uint32_t channels, uint64_t first_frame)
{
    uint32_t f;

    for (f = 0; f < frames; f++) {
        uint64_t pos = first_frame + f;
        unsigned ch;

        if (pos < tone->start || pos >= tone->start + tone->length)
            continue;
        for (ch = 0; ch < UA101_TONE_CHANNELS && ch < channels; ch++)
            put_word(dst + ((size_t)f * channels + ch) * 4,
                     (uint32_t)ua101_tone_word(tone, ch, pos - tone->start));
    }
}

void ua101_ring_init(ua101_ring *ring, uint8_t *data, size_t capacity)
{
    ring->data = data;
    ring->capacity = capacity;
    atomic_init(&ring->head, 0);
    atomic_init(&ring->tail, 0);
}

int ua101_ring_write(ua101_ring *ring, const void *src, size_t bytes)
{
    size_t head = atomic_load_explicit(&ring->head, memory_order_relaxed);
    size_t tail = atomic_load_explicit(&ring->tail, memory_order_acquire);
    size_t offset, first;

    if (bytes > ring->capacity - (head - tail))
        return 0;
    offset = head % ring->capacity;
    first = ring->capacity - offset;
    if (first > bytes)
        first = bytes;
    memcpy(ring->data + offset, src, first);
    memcpy(ring->data, (const uint8_t *)src + first, bytes - first);
    atomic_store_explicit(&ring->head, head + bytes, memory_order_release);
    return 1;
}

size_t ua101_ring_peek(ua101_ring *ring, const uint8_t **ptr)
{
    size_t tail = atomic_load_explicit(&ring->tail, memory_order_relaxed);
    size_t head = atomic_load_explicit(&ring->head, memory_order_acquire);
    size_t available = head - tail;
    size_t offset = tail % ring->capacity;
    size_t first = ring->capacity - offset;

    *ptr = ring->data + offset;
    return available < first ? available : first;
}

void ua101_ring_consume(ua101_ring *ring, size_t bytes)
{
    size_t tail = atomic_load_explicit(&ring->tail, memory_order_relaxed);

    atomic_store_explicit(&ring->tail, tail + bytes, memory_order_release);
}

static void put_u16(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
}

static void put_u32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
    out[2] = (uint8_t)(value >> 16);
    out[3] = (uint8_t)(value >> 24);
}

void ua101_wav_header(uint8_t out[UA101_WAV_HEADER_BYTES], uint16_t channels,
                      uint32_t rate_hz, uint16_t container_bits, uint16_t valid_bits,
                      uint64_t data_bytes)
{
    /* KSDATAFORMAT_SUBTYPE_PCM */
    static const uint8_t k_pcm_guid[16] = {0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
                                           0x10, 0x00, 0x80, 0x00, 0x00, 0xAA,
                                           0x00, 0x38, 0x9B, 0x71};
    uint16_t block = (uint16_t)(channels * (container_bits / 8));
    uint32_t data = data_bytes > UA101_WAV_MAX_DATA_BYTES
                        ? (uint32_t)UA101_WAV_MAX_DATA_BYTES
                        : (uint32_t)data_bytes;

    memset(out, 0, UA101_WAV_HEADER_BYTES);
    memcpy(out, "RIFF", 4);
    put_u32(out + 4, UA101_WAV_HEADER_BYTES - 8 + data);
    memcpy(out + 8, "WAVEfmt ", 8);
    put_u32(out + 16, 40);
    put_u16(out + 20, 0xFFFE);
    put_u16(out + 22, channels);
    put_u32(out + 24, rate_hz);
    put_u32(out + 28, rate_hz * block);
    put_u16(out + 32, block);
    put_u16(out + 34, container_bits);
    put_u16(out + 36, 22);
    put_u16(out + 38, valid_bits);
    put_u32(out + 40, 0); /* no speaker positions for these channels */
    memcpy(out + 44, k_pcm_guid, 16);
    memcpy(out + 60, "data", 4);
    put_u32(out + 64, data);
}

double ua101_rate_margin_ppm(double seconds)
{
    if (!(seconds > 0.0))
        return INFINITY;
    return 2.0 / (8000.0 * seconds) * 1e6;
}

/*
 * Measured over 591 readings on this Mac, one per second: a timestamp is either on
 * time or about one 1 ms bus frame early, and two of them disagreed by up to 1.11 ms.
 * That's well beyond the 200 us quoted for this call.
 */
#define BUS_TIME_SPREAD_SECONDS 0.0012

double ua101_host_margin_ppm(double seconds)
{
    if (!(seconds > 0.0))
        return INFINITY;
    return BUS_TIME_SPREAD_SECONDS / seconds * 1e6;
}

ua101_rate_result ua101_rate_verdict(double ppm, double margin_ppm, double limit_ppm)
{
    if (!isfinite(ppm) || !isfinite(margin_ppm))
        return UA101_RATE_OUTSIDE;
    if (fabs(ppm) - margin_ppm > limit_ppm)
        return UA101_RATE_OUTSIDE;
    if (fabs(ppm) + margin_ppm > limit_ppm)
        return UA101_RATE_UNDECIDED;
    return UA101_RATE_INSIDE;
}
