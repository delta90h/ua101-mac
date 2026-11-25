#include "ua101_shared.h"

#include <math.h>
#include <string.h>

/* A reader gives up after this many tries, rather than wait on a real-time thread. The writer
   holds the lock for a few stores, so in practice the first or second try succeeds. */
#define READ_TRIES 8

static size_t round_up(size_t n, size_t to)
{
    return (n + to - 1) / to * to;
}

static int power_of_two(uint64_t n)
{
    return n != 0 && (n & (n - 1)) == 0;
}

uint64_t ua101_shared_ring_frames(double rate_hz)
{
    uint64_t n = 1;

    while ((double)n < rate_hz * 0.5 && n < (UINT64_C(1) << 24))
        n <<= 1;
    return n;
}

size_t ua101_shared_bytes(uint32_t capture_frame_bytes, uint32_t playback_frame_bytes,
                          uint64_t ring_frames, size_t page_bytes)
{
    size_t at;

    if (page_bytes == 0 || !power_of_two(ring_frames) || ring_frames > (UINT64_C(1) << 24) ||
        capture_frame_bytes == 0 || playback_frame_bytes == 0 || capture_frame_bytes > 4096 ||
        playback_frame_bytes > 4096)
        return 0;
    at = round_up(sizeof(ua101_shared), page_bytes);
    at += round_up((size_t)ring_frames * capture_frame_bytes, page_bytes);
    at += round_up((size_t)ring_frames * playback_frame_bytes, page_bytes);
    return at;
}

int ua101_shared_init(ua101_shared *s, size_t bytes, double rate_hz, uint32_t capture_channels,
                      uint32_t playback_channels, uint64_t ring_frames, size_t page_bytes)
{
    uint32_t cap_fb = capture_channels * 4, play_fb = playback_channels * 4;
    size_t need = ua101_shared_bytes(cap_fb, play_fb, ring_frames, page_bytes);
    uint8_t *base = (uint8_t *)s, *cap_data, *play_data;

    if (s == NULL || need == 0 || bytes < need || !(rate_hz > 0.0) || capture_channels > 64 ||
        playback_channels > 64)
        return 0;
    memset(s, 0, sizeof(*s));
    s->magic = UA101_SHARED_MAGIC;
    s->version = UA101_SHARED_VERSION;
    s->bytes = need;
    s->rate_hz = rate_hz;
    s->capture_channels = capture_channels;
    s->playback_channels = playback_channels;
    s->capture_frame_bytes = cap_fb;
    s->playback_frame_bytes = play_fb;
    s->ring_frames = ring_frames;
    atomic_init(&s->state, UA101_STATE_STOPPED);
    atomic_init(&s->stats.playback_lead_min, INT64_MAX);
    atomic_init(&s->stats.playback_lead_max, INT64_MIN);

    cap_data = base + round_up(sizeof(ua101_shared), page_bytes);
    play_data = cap_data + round_up((size_t)ring_frames * cap_fb, page_bytes);
    memset(cap_data, 0, (size_t)ring_frames * cap_fb);
    memset(play_data, 0, (size_t)ring_frames * play_fb);
    return ua101_frame_ring_init(&s->capture, cap_data, ring_frames, cap_fb) &&
           ua101_frame_ring_init(&s->playback, play_data, ring_frames, play_fb);
}

/* Each ring's frames must lie inside the block, after the header, and not overlap. */
static int ring_fits(const ua101_shared *s, const ua101_frame_ring *r, uint32_t frame_bytes,
                     size_t bytes, int64_t *start, int64_t *end)
{
    int64_t ring_at = (int64_t)((const uint8_t *)r - (const uint8_t *)s);

    if (r->frame_bytes != frame_bytes || r->capacity != s->ring_frames)
        return 0;
    *start = ring_at + r->data_offset;
    *end = *start + (int64_t)(s->ring_frames * frame_bytes);
    return *start >= (int64_t)sizeof(ua101_shared) && *end <= (int64_t)bytes;
}

int ua101_shared_check(const ua101_shared *s, size_t bytes)
{
    int64_t cap_start, cap_end, play_start, play_end;

    if (s == NULL || bytes < sizeof(ua101_shared) || s->magic != UA101_SHARED_MAGIC ||
        s->version != UA101_SHARED_VERSION || s->bytes > bytes || !(s->rate_hz > 0.0) ||
        s->capture_frame_bytes != s->capture_channels * 4 ||
        s->playback_frame_bytes != s->playback_channels * 4 ||
        ua101_shared_bytes(s->capture_frame_bytes, s->playback_frame_bytes, s->ring_frames, 1) ==
            0)
        return 0;
    if (!ring_fits(s, &s->capture, s->capture_frame_bytes, s->bytes, &cap_start, &cap_end) ||
        !ring_fits(s, &s->playback, s->playback_frame_bytes, s->bytes, &play_start, &play_end))
        return 0;
    return cap_end <= play_start || play_end <= cap_start;
}

/* One read of a field the other process can write, so what was checked is what is used. */
static uint64_t once_u64(const uint64_t *p)
{
    return *(const volatile uint64_t *)p;
}

static int64_t once_i64(const int64_t *p)
{
    return *(const volatile int64_t *)p;
}

static uint32_t once_u32(const uint32_t *p)
{
    return *(const volatile uint32_t *)p;
}

static int view_of(ua101_shared *s, size_t bytes, ua101_frame_ring *r, uint64_t ring_frames,
                   uint32_t frame_bytes, ua101_frame_view *v, int64_t *start, int64_t *end)
{
    int64_t ring_at = (int64_t)((uint8_t *)r - (uint8_t *)s);
    int64_t offset = once_i64(&r->data_offset);

    /* The block is far smaller than 2^62 bytes, so anything larger is not an offset in it. */
    if (offset < -(int64_t)bytes || offset > (int64_t)bytes ||
        once_u64(&r->capacity) != ring_frames || once_u32(&r->frame_bytes) != frame_bytes)
        return 0;
    *start = ring_at + offset;
    *end = *start + (int64_t)(ring_frames * frame_bytes);
    if (*start < (int64_t)sizeof(ua101_shared) || *end > (int64_t)bytes)
        return 0;
    v->ring = r;
    v->data = (uint8_t *)s + *start;
    v->capacity = ring_frames;
    v->frame_bytes = frame_bytes;
    return 1;
}

int ua101_shared_views(ua101_shared *s, size_t bytes, uint32_t capture_channels,
                       uint32_t playback_channels, ua101_frame_view *capture,
                       ua101_frame_view *playback)
{
    int64_t cap_start = 0, cap_end = 0, play_start = 0, play_end = 0;
    uint64_t ring_frames;

    memset(capture, 0, sizeof(*capture));
    memset(playback, 0, sizeof(*playback));
    if (s == NULL || bytes < sizeof(ua101_shared) || bytes > (SIZE_MAX >> 2) ||
        capture_channels > 64 || playback_channels > 64 ||
        once_u32(&s->magic) != UA101_SHARED_MAGIC || once_u32(&s->version) != UA101_SHARED_VERSION)
        return 0;
    ring_frames = once_u64(&s->ring_frames);
    if (!power_of_two(ring_frames) || ring_frames > (UINT64_C(1) << 24))
        return 0;
    if (capture_channels != 0 &&
        (once_u32(&s->capture_channels) != capture_channels ||
         !view_of(s, bytes, &s->capture, ring_frames, capture_channels * 4, capture, &cap_start,
                  &cap_end)))
        return 0;
    if (playback_channels != 0 &&
        (once_u32(&s->playback_channels) != playback_channels ||
         !view_of(s, bytes, &s->playback, ring_frames, playback_channels * 4, playback,
                  &play_start, &play_end)))
        return 0;
    if (capture_channels != 0 && playback_channels != 0 &&
        !(cap_end <= play_start || play_end <= cap_start))
        return 0;
    return 1;
}

uint32_t ua101_offset_frames(double seconds, double rate_hz)
{
    double frames = seconds * rate_hz;

    if (!(frames > 0.0))
        return 0;
    if (frames > 1e6)
        return 1000000;
    /* Less a millionth of a frame, so a time that is a whole number of frames stays that. */
    return (uint32_t)ceil(frames - 1e-6);
}

static uint64_t bits_of(double x)
{
    uint64_t b;

    memcpy(&b, &x, sizeof(b));
    return b;
}

static double double_of(uint64_t b)
{
    double x;

    memcpy(&x, &b, sizeof(x));
    return x;
}

/* The lock's two halves, after Boehm, "Can seqlocks get along with programming language memory
   models?" (2012): the fence keeps the field stores from being seen before seq turns odd. */
static uint32_t lock(ua101_shared_clock *c)
{
    uint32_t seq = atomic_load_explicit(&c->seq, memory_order_relaxed);

    atomic_store_explicit(&c->seq, seq + 1, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    return seq;
}

static void unlock(ua101_shared_clock *c, uint32_t seq)
{
    atomic_store_explicit(&c->seq, seq + 2, memory_order_release);
}

void ua101_shared_publish(ua101_shared_clock *c, uint64_t sample, double host,
                          double ticks_per_sample)
{
    uint32_t seq;

    if (!(ticks_per_sample > 0.0) || !isfinite(host))
        return;
    seq = lock(c);
    atomic_store_explicit(&c->sample, sample, memory_order_relaxed);
    atomic_store_explicit(&c->host_bits, bits_of(host), memory_order_relaxed);
    atomic_store_explicit(&c->ticks_per_sample_bits, bits_of(ticks_per_sample),
                          memory_order_relaxed);
    unlock(c, seq);
}

void ua101_shared_new_seed(ua101_shared_clock *c)
{
    uint32_t seq = lock(c);

    atomic_store_explicit(&c->seed, atomic_load_explicit(&c->seed, memory_order_relaxed) + 1,
                          memory_order_relaxed);
    atomic_store_explicit(&c->ticks_per_sample_bits, 0, memory_order_relaxed);
    unlock(c, seq);
}

int ua101_shared_read(const ua101_shared_clock *c, ua101_clock_line *line)
{
    int tries;

    for (tries = 0; tries < READ_TRIES; tries++) {
        uint32_t before = atomic_load_explicit(&c->seq, memory_order_acquire), after;
        ua101_clock_line l;
        uint64_t slope;

        if (before & 1)
            continue;
        l.seed = atomic_load_explicit(&c->seed, memory_order_relaxed);
        l.sample = atomic_load_explicit(&c->sample, memory_order_relaxed);
        l.host = double_of(atomic_load_explicit(&c->host_bits, memory_order_relaxed));
        slope = atomic_load_explicit(&c->ticks_per_sample_bits, memory_order_relaxed);
        atomic_thread_fence(memory_order_acquire);
        after = atomic_load_explicit(&c->seq, memory_order_relaxed);
        if (after != before)
            continue;
        if (slope == 0)
            return 0; /* no line yet */
        l.ticks_per_sample = double_of(slope);
        *line = l;
        return 1;
    }
    return 0;
}

double ua101_line_host(const ua101_clock_line *l, double sample)
{
    return l->host + (sample - (double)l->sample) * l->ticks_per_sample;
}

double ua101_line_sample(const ua101_clock_line *l, double host)
{
    return (double)l->sample + (host - l->host) / l->ticks_per_sample;
}

int ua101_zero_timestamp(const ua101_clock_line *l, uint64_t now, uint32_t period,
                         double *sample_out, uint64_t *host_out)
{
    double sample_now, zero, host;

    if (period == 0 || !(l->ticks_per_sample > 0.0))
        return 0;
    sample_now = ua101_line_sample(l, (double)now);
    if (!(sample_now >= 0.0))
        return 0;
    zero = floor(sample_now / (double)period) * (double)period;
    host = ua101_line_host(l, zero);
    if (!(host >= 0.0))
        return 0;
    *sample_out = zero;
    *host_out = (uint64_t)llround(host);
    return 1;
}
