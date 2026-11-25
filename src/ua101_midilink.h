/*
 * A MIDI client's end of the link to the helper (src/ua101_link.h, version 12): attach to the
 * unit's MIDI IN and OUT sockets, hand MIDI bytes towards MIDI OUT, get what arrives at MIDI
 * IN. The per-user agent (src/ua101midiagent.c) uses it; so does tests/test_ctlhelper.c,
 * against the helper's real service and a pretend unit.
 *
 * Like the control app's end (src/ua101_ctllink.h) it never waits: every answer comes back as
 * an event on the queue given at open, and every call must be made on that queue too.
 *
 * It decides nothing about what may be sent, and it can't name a cable: it hands over MIDI
 * bytes, and the helper's gate (src/ua101_midigate.h) does the rest.
 *
 * No xpc types appear here.
 */
#ifndef UA101_MIDILINK_H
#define UA101_MIDILINK_H

#include <dispatch/dispatch.h>
#include <stddef.h>
#include <stdint.h>

/* An attach not answered after this long is reported as "no helper". */
#define UA101_MIDILINK_ATTACH_SECONDS 5.0

typedef struct {
    /* The answer to an attach. status is a UA101_LINK_ value (0: attached). `watching`: the
       helper will say (changed) when a unit appears, so there's no need to ask again. */
    void (*attached)(void *context, int status, int watching, const char *why);
    /* MIDI bytes from the unit's MIDI IN socket, in order, with the Mac's clock
       (mach_absolute_time) when the USB read that brought them completed. */
    void (*midi)(void *context, const uint8_t *bytes, size_t len, uint64_t host_time);
    /* The unit came, went or was lost, the Mac woke, or the helper started again: no longer
       attached; attach again. */
    void (*changed)(void *context);
} ua101_midilink_events;

typedef struct ua101_midilink ua101_midilink;

/* Makes a link; nothing is connected or sent until the first attach. */
ua101_midilink *ua101_midilink_open(dispatch_queue_t queue, const ua101_midilink_events *events,
                                    void *context);
void ua101_midilink_attach(ua101_midilink *l);
/* MIDI bytes for MIDI OUT. Returns 1 if they were handed to the helper, which sends no answer:
   what it drops it counts, and says in its log. */
int ua101_midilink_send(ua101_midilink *l, const uint8_t *bytes, size_t len);
void ua101_midilink_detach(ua101_midilink *l);
/* No event is called after this returns. */
void ua101_midilink_close(ua101_midilink *l);

/* For a test in one process: links attach to this xpc_endpoint_t instead of the installed
   helper's Mach service; NULL undoes it. */
void ua101_midilink_use_endpoint(void *xpc_endpoint);

#endif
