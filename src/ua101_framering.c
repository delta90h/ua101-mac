#include "ua101_framering.h"

#include <string.h>

static uint8_t *frames_of(const ua101_frame_ring *r)
{
    return (uint8_t *)((uintptr_t)r + (uintptr_t)r->data_offset);
}

int ua101_frame_ring_init(ua101_frame_ring *r, uint8_t *data, uint64_t capacity_frames,
                          uint32_t frame_bytes)
{
    memset(r, 0, sizeof(*r));
    if (data == NULL || frame_bytes == 0 || capacity_frames == 0 ||
        (capacity_frames & (capacity_frames - 1)) != 0)
        return 0;
    r->data_offset = (int64_t)((uintptr_t)data - (uintptr_t)r);
    r->frame_bytes = frame_bytes;
    r->capacity = capacity_frames;
    atomic_init(&r->end, 0);
    atomic_init(&r->writing_to, 0);
    atomic_init(&r->start, 0);
    atomic_init(&r->started, 0);
    return 1;
}

void ua101_frame_ring_reset(ua101_frame_ring *r)
{
    atomic_store_explicit(&r->started, 0, memory_order_relaxed);
    atomic_store_explicit(&r->end, 0, memory_order_relaxed);
    atomic_store_explicit(&r->writing_to, 0, memory_order_relaxed);
    atomic_store_explicit(&r->start, 0, memory_order_relaxed);
    r->lost = 0;
    r->repeated = 0;
    atomic_thread_fence(memory_order_release);
}

/* Copies n frames into the ring at absolute frame number frame, or silence if src is NULL. */
static void put(const ua101_frame_view *v, uint64_t frame, const uint8_t *src, uint64_t n)
{
    while (n > 0) {
        uint64_t slot = frame & (v->capacity - 1);
        uint64_t run = v->capacity - slot < n ? v->capacity - slot : n;
        uint8_t *to = v->data + slot * v->frame_bytes;

        if (src != NULL) {
            memcpy(to, src, run * v->frame_bytes);
            src += run * v->frame_bytes;
        } else {
            memset(to, 0, run * v->frame_bytes);
        }
        frame += run;
        n -= run;
    }
}

static void get(const ua101_frame_view *v, uint64_t frame, uint8_t *dst, uint64_t n)
{
    while (n > 0) {
        uint64_t slot = frame & (v->capacity - 1);
        uint64_t run = v->capacity - slot < n ? v->capacity - slot : n;

        memcpy(dst, v->data + slot * v->frame_bytes, run * v->frame_bytes);
        dst += run * v->frame_bytes;
        frame += run;
        n -= run;
    }
}

void ua101_frame_view_write(const ua101_frame_view *v, uint64_t first, const void *src,
                            uint64_t frames)
{
    ua101_frame_ring *r = v->ring;
    const uint8_t *from = src;
    uint64_t end, last, lo;

    if (frames == 0 || v->frame_bytes == 0)
        return;
    if (!atomic_load_explicit(&r->started, memory_order_relaxed)) {
        atomic_store_explicit(&r->start, first, memory_order_relaxed);
        atomic_store_explicit(&r->end, first, memory_order_relaxed);
        atomic_store_explicit(&r->writing_to, first, memory_order_relaxed);
        atomic_store_explicit(&r->started, 1, memory_order_release);
    }
    end = atomic_load_explicit(&r->end, memory_order_relaxed);
    if (first + frames <= end) {
        r->repeated += frames;
        return;
    }
    if (first < end) {
        uint64_t skip = end - first;

        r->repeated += skip;
        from += skip * v->frame_bytes;
        frames -= skip;
        first = end;
    }
    r->lost += first - end;
    last = first + frames;

    /*
     * Says how far this write reaches before touching anything, so a reader can tell which of
     * the frames it copied may have been overwritten meanwhile (see ua101_frame_ring_read).
     * The fence keeps the frame writes from being seen before this.
     */
    atomic_store_explicit(&r->writing_to, last, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);

    /* Only the newest whole ring of what's written can ever be read. */
    lo = last - end > v->capacity ? last - v->capacity : end;
    if (lo < first)
        put(v, lo, NULL, first - lo); /* the frames that never came */
    if (lo > first) {
        from += (lo - first) * v->frame_bytes;
        frames -= lo - first;
        first = lo;
    }
    put(v, first, from, frames);
    atomic_store_explicit(&r->end, last, memory_order_release);
}

uint64_t ua101_frame_view_end(const ua101_frame_view *v)
{
    return atomic_load_explicit(&v->ring->end, memory_order_acquire);
}

uint64_t ua101_frame_view_read(const ua101_frame_view *v, uint64_t first, void *dst,
                               uint64_t frames, ua101_frame_ring_miss *miss)
{
    ua101_frame_ring *r = v->ring;
    uint8_t *to = dst;
    uint64_t fb = v->frame_bytes, lo = first, hi = first + frames;
    uint64_t start, end, oldest, c_lo, c_hi, real_lo, real_hi, not_yet;

    if (frames == 0 || v->frame_bytes == 0 ||
        !atomic_load_explicit(&r->started, memory_order_acquire)) {
        if (frames != 0)
            memset(to, 0, frames * fb);
        if (miss != NULL)
            miss->not_yet = frames, miss->overwritten = 0;
        return 0;
    }
    start = atomic_load_explicit(&r->start, memory_order_relaxed);
    end = atomic_load_explicit(&r->end, memory_order_acquire);
    oldest = end > v->capacity ? end - v->capacity : 0;

    /* Before the first frame, or not written yet: [lo, start) and [end, hi). */
    not_yet = (hi < start ? hi : start) > lo ? (hi < start ? hi : start) - lo : 0;
    not_yet += hi > (lo > end ? lo : end) ? hi - (lo > end ? lo : end) : 0;

    /* What was written, and of that, what was still there when it looked. */
    c_lo = lo > start ? lo : start;
    c_hi = hi < end ? hi : end;
    real_lo = real_hi = lo;
    if (c_lo < c_hi) {
        real_lo = c_lo > oldest ? c_lo : oldest;
        real_hi = c_hi;
        if (real_lo < real_hi) {
            uint64_t writing_to, safe;

            get(v, real_lo, to + (real_lo - lo) * fb, real_hi - real_lo);
            /*
             * A write that started during the copy may have reused slots it copied: anything
             * older than a ring before where that write reaches. The fence pairs with the
             * writer's, so if the copy saw any of a write's frames, it sees that write's
             * writing_to here. (The frames themselves are plain memory; the fences give the
             * order on the processors this runs on.)
             */
            atomic_thread_fence(memory_order_acquire);
            writing_to = atomic_load_explicit(&r->writing_to, memory_order_relaxed);
            safe = writing_to > v->capacity ? writing_to - v->capacity : 0;
            if (real_lo < safe)
                real_lo = safe < real_hi ? safe : real_hi;
        } else {
            real_lo = real_hi;
        }
    }

    /* Silence for everything that isn't real. */
    memset(to, 0, (real_lo - lo) * fb);
    memset(to + (real_hi - lo) * fb, 0, (hi - real_hi) * fb);
    if (miss != NULL) {
        miss->not_yet = not_yet;
        miss->overwritten = frames - (real_hi - real_lo) - not_yet;
    }
    return real_hi - real_lo;
}

void ua101_frame_view_of(ua101_frame_view *v, ua101_frame_ring *r)
{
    v->ring = r;
    v->data = frames_of(r);
    v->capacity = r->capacity;
    v->frame_bytes = r->frame_bytes;
}

void ua101_frame_ring_write(ua101_frame_ring *r, uint64_t first, const void *src, uint64_t frames)
{
    ua101_frame_view v;

    ua101_frame_view_of(&v, r);
    ua101_frame_view_write(&v, first, src, frames);
}

uint64_t ua101_frame_ring_end(ua101_frame_ring *r)
{
    return atomic_load_explicit(&r->end, memory_order_acquire);
}

uint64_t ua101_frame_ring_read(ua101_frame_ring *r, uint64_t first, void *dst, uint64_t frames,
                               ua101_frame_ring_miss *miss)
{
    ua101_frame_view v;

    ua101_frame_view_of(&v, r);
    return ua101_frame_view_read(&v, first, dst, frames, miss);
}
