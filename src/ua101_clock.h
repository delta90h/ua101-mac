/*
 * A delay-locked loop, for the driver's clock: it follows how a counter maps to time, smoothing
 * out jitter and the counter's steps while keeping up with slow drift. Two of them make the
 * clock: frames captured against USB bus time, whose counts are exact, and bus frame numbers
 * against host time, from the bus timestamps. Contains no Apple APIs.
 *
 * It starts wide, so it locks within seconds, and narrows to its working bandwidth. Readings far
 * off the line can be rejected, such as a bus timestamp that reads one bus frame early (about 1
 * in 5 did, in Stage 2), and if they keep being rejected it starts again, as after a sleep.
 */
#ifndef UA101_CLOCK_H
#define UA101_CLOCK_H

#include <stdint.h>

typedef struct {
    /* Settings. */
    double bandwidth_hz;       /* working bandwidth */
    double start_bandwidth_hz; /* at the start, narrowing to bandwidth_hz over about 20 s */
    double nominal_period;     /* seconds per count, the first guess */
    double reject_seconds;     /* readings further off than this are ignored; 0 accepts all */
    unsigned restart_after;    /* rejections in a row that make it start again */

    /* State. */
    unsigned long updates;
    uint64_t x_ref;   /* counter at the reference point */
    double y_ref;     /* filtered time there */
    double period;    /* filtered seconds per count */
    double elapsed;   /* seconds followed so far, for narrowing */
    unsigned long rejected;
    unsigned in_a_row;
    unsigned long restarts;
} ua101_dll;

void ua101_dll_init(ua101_dll *d, double nominal_period, double bandwidth_hz,
                    double reject_seconds);

/* One reading: counter x was at time y. Returns 1 if used, 0 if ignored (rejected, or x didn't
   move forward). */
int ua101_dll_update(ua101_dll *d, uint64_t x, double y);

/* Whether it has a line yet: at least two readings. */
int ua101_dll_locked(const ua101_dll *d);
/* The filtered time at counter x, and the filtered seconds per count. */
double ua101_dll_time(const ua101_dll *d, uint64_t x);
double ua101_dll_period(const ua101_dll *d);

/*
 * The USB bus clock against the host clock, from the stamps GetBusFrameNumberWithTime gives.
 * Measured on the unit (2026-11-09, tests/data/ua101-bus-stamps-20s.txt): a stamp is the bus
 * frame current when it was asked, with the time it was asked, not when that frame began. So
 * stamps taken in the capture callback spread over each frame's first few hundred us, and some
 * (2 to 21% in different runs) still have the frame before.
 *
 * It starts from a batch of stamps, through the place in the frame they share and the frame most
 * of them are in, and from then on takes a stamp more than half a frame off the line as the
 * neighbouring frame. They spread more than frame counts do, so it wants a narrower bandwidth
 * (UA101_BUS_CLOCK_BANDWIDTH_HZ). The bus and the host clock are both the Mac's, so the line
 * between them hardly moves.
 *
 * That line follows where most stamps fall, and that isn't a steady place. Measured again
 * (2026-11-09 evening): the counter's tick is sharp, to
 * a few us, and the completions cluster around it, some on each side. Where most come just
 * after the tick, the line sits at the start of each frame; where most come just before it, at
 * the end. That differed between runs and changed within one, so the clock moved by up to 1 ms.
 * But each stamp also proves the tick of the frame it names came at or before it, and the next
 * tick after it. So the stamps either side of the tick pin it down, whichever side most fall,
 * and the clock gives the time of the tick itself (ua101_bus_clock_time), the same in every run.
 */
#define UA101_BUS_CLOCK_BATCH 16
/* The stamps after the batch that check it: if most of them had to be moved the same way, the
   batch was mostly a frame behind, and the line is moved a frame before it's used. */
#define UA101_BUS_CLOCK_CHECK 64
/* With the stamps' spread, 0.1 Hz moved the rate by up to about 3.5 ppm; 0.02 Hz by about 0.2. */
#define UA101_BUS_CLOCK_BANDWIDTH_HZ 0.02
/* The tick's bounds, in frames: this close together, they have found it. Stamps all on one side
   leave them about 0.9 apart; stamps on both sides, a few us. */
#define UA101_BUS_CLOCK_TICK_FOUND 0.25
/* Stamps after the check to wait for the tick to be found, before the clock is used anyway, with
   the tick taken just before the earliest stamp. */
#define UA101_BUS_CLOCK_TICK_WAIT 1000

typedef struct {
    ua101_dll dll;
    unsigned batch_count;
    uint64_t batch_frame; /* the first stamp of the batch */
    double batch_time;
    double batch[UA101_BUS_CLOCK_BATCH]; /* each stamp against the first, on the nominal line */
    unsigned long corrected; /* stamps taken as a neighbouring frame */
    unsigned long starts;    /* lines found from a batch: 1, unless it had to start again */
    unsigned checked;        /* stamps since the batch, up to UA101_BUS_CLOCK_CHECK */
    long balance;            /* their corrections added up, + for a frame on, - for one back */
    int settled;             /* the check is done and the tick placed, so it can be published */
    unsigned long moved;     /* times the check moved the line a frame */

    /* The counter's tick, as the host time of frame tick_frame's tick. Each stamp was taken at
       or after the tick of the frame it names, and before the next, so its time bounds it. */
    int tick_started;
    uint64_t tick_frame;         /* the newest stamp's frame, which the times below are for */
    double tick_lo, tick_hi;     /* the bounds, each relaxing slowly so the tick can move */
    double tick;                 /* the time in use, moved gently once published */
    int tick_found;              /* the bounds have been within UA101_BUS_CLOCK_TICK_FOUND */
    unsigned long tick_wait;     /* stamps since the check, while it wasn't found */
    unsigned long tick_disagree; /* stamps since settling that the clock's ticks can't explain */
    /* The ticks themselves, the line the clock gives once settled: a loop of its own, fed the
       tick each stamp, so it doesn't swing when the stamps' cluster moves and the line above
       follows it. */
    ua101_dll ticks;
} ua101_bus_clock;

void ua101_bus_clock_init(ua101_bus_clock *c, double frame_seconds, double bandwidth_hz,
                          double reject_seconds);
/* One stamp: bus frame `frame` was current at time `time`. Returns 1 if it was used. */
int ua101_bus_clock_update(ua101_bus_clock *c, uint64_t frame, double time);
/* Whether the line has been checked and the tick placed, so it can be used. */
int ua101_bus_clock_settled(const ua101_bus_clock *c);
/* The time of bus frame `frame`'s tick, or of a point that far through the frames. */
double ua101_bus_clock_time(const ua101_bus_clock *c, double frame);
/* The seconds per bus frame that goes with it. */
double ua101_bus_clock_period(const ua101_bus_clock *c);

#endif
