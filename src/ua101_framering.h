/*
 * A ring of audio frames between two threads, indexed by absolute frame number, for the driver:
 * the USB thread writes each transfer at the frame number the bus clock gives it, and the audio
 * thread reads frames by number. One writer and one reader, and neither ever waits.
 *
 * Unlike a byte ring, it keeps time. Frames that never came (a stall) read as silence in their
 * own place, so later audio isn't moved earlier. A reader that falls a whole ring behind gets
 * silence for frames that were overwritten, even during its own copy, and never stale or torn
 * audio. Both are counted. Contains no Apple APIs, and allocates nothing: the caller gives it
 * the memory.
 *
 * It holds no pointers, so it works in memory that two processes map at different addresses
 * (src/ua101_shared.h): it finds its frames by their distance from the ring itself. So the ring
 * must not be copied or moved after ua101_frame_ring_init, and its frames must stay at the same
 * distance from it in every mapping.
 */
#ifndef UA101_FRAMERING_H
#define UA101_FRAMERING_H

#include <stdatomic.h>
#include <stdint.h>

typedef struct {
    int64_t data_offset;  /* the frames' address minus the ring's */
    uint32_t frame_bytes; /* 0 if init failed */
    uint64_t capacity; /* frames, a power of two */

    _Atomic uint64_t end;        /* one past the newest frame written */
    _Atomic uint64_t writing_to; /* end once the write in progress is done */
    _Atomic uint64_t start;      /* the first frame ever written */
    _Atomic int started;

    /* Counted by the writer. */
    uint64_t lost;     /* frames that never came, read as silence */
    uint64_t repeated; /* frames given again, ignored */
} ua101_frame_ring;

/*
 * One side's own copy of where a ring's frames are and how big they are, taken once, after
 * checking them (ua101_shared_views). In memory two processes share, the ring's own fields can
 * be changed by the other process at any time; with a view, nothing it writes there can send a
 * copy outside the frames. Only the counters (end, writing_to, start, started) are still read
 * from the ring, and any values in them keep every copy inside the view's frames and the
 * caller's buffer.
 */
typedef struct {
    ua101_frame_ring *ring;
    uint8_t *data;
    uint64_t capacity; /* frames, a power of two */
    uint32_t frame_bytes;
} ua101_frame_view;

/* What one read couldn't give, as silence. */
typedef struct {
    uint64_t not_yet;    /* not written yet, or before the first frame */
    uint64_t overwritten; /* gone, because the reader was a whole ring behind */
} ua101_frame_ring_miss;

/* capacity_frames must be a power of two. data holds capacity_frames * frame_bytes bytes. */
int ua101_frame_ring_init(ua101_frame_ring *r, uint8_t *data, uint64_t capacity_frames,
                          uint32_t frame_bytes);

/* Empties the ring, keeping its memory, for a stream that starts counting again from 0. Neither
   side may be using it meanwhile. */
void ua101_frame_ring_reset(ua101_frame_ring *r);

/* Writer: frames first to first + frames - 1. A gap since the last write is filled with
   silence and counted as lost; frames already written are ignored and counted as repeated. */
void ua101_frame_ring_write(ua101_frame_ring *r, uint64_t first, const void *src, uint64_t frames);

/* Reader: one past the newest frame written, or 0 before the first write. */
uint64_t ua101_frame_ring_end(ua101_frame_ring *r);

/* Reader: copies frames first to first + frames - 1, with silence for any it can't give, and
   returns how many were real. miss may be NULL. */
uint64_t ua101_frame_ring_read(ua101_frame_ring *r, uint64_t first, void *dst, uint64_t frames,
                               ua101_frame_ring_miss *miss);

/* A view from the ring's own fields: for a ring only this process can write. */
void ua101_frame_view_of(ua101_frame_view *v, ua101_frame_ring *r);

/* As ua101_frame_ring_write, _end and _read, through a view. */
void ua101_frame_view_write(const ua101_frame_view *v, uint64_t first, const void *src,
                            uint64_t frames);
uint64_t ua101_frame_view_end(const ua101_frame_view *v);
uint64_t ua101_frame_view_read(const ua101_frame_view *v, uint64_t first, void *dst,
                               uint64_t frames, ua101_frame_ring_miss *miss);

#endif
