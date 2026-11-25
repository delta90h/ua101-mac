/*
 * What the helper watches for besides its plug-ins: the unit appearing on the USB bus or
 * leaving it (plugged, unplugged, switched on or off), and the Mac going to sleep and waking.
 * It only watches: it never opens the unit or sends it anything.
 *
 * src/ua101_watch.c does it with IOKit. tests/test_service.c has a pretend one, which calls the
 * same events by hand, so every case runs with no unit and without putting anything to sleep.
 */
#ifndef UA101_WATCH_H
#define UA101_WATCH_H

#include <dispatch/dispatch.h>

typedef struct {
    void (*unit_arrived)(void); /* a UA-101 appeared on the bus */
    void (*unit_left)(void);    /* one went: unplugged or switched off */
    /* The Mac is about to sleep. It is held awake until this returns (30 s at most, the
       system's limit), so whatever must happen first is done inside it. */
    void (*will_sleep)(void);
    void (*did_wake)(void);     /* the Mac is awake again and its devices have power */
} ua101_watch_events;

/*
 * Starts watching, for as long as the process lives. The events are called on `queue`, one at
 * a time. *units is the number of UA-101s on the bus now; no event is sent for those. Returns
 * 0 if the bus can't be watched, and then no unit events will come (sleep events still may).
 */
int ua101_watch_start(dispatch_queue_t queue, const ua101_watch_events *events, int *units);

#endif
