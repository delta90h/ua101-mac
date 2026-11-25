/*
 * The unit's MIDI interface (interface 2: bulk OUT 0x04 and bulk IN 0x83), which only one
 * program can hold at a time. The helper holds it, for everything that needs it:
 *   cable 1  Roland's control messages, for the control app (docs/control-protocol.md);
 *   cable 0  the MIDI IN and OUT sockets, for MIDI clients (src/ua101_midigate.h).
 *
 * This part only moves whole USB-MIDI packets (4 bytes each) in and out. It doesn't look at
 * them: which cable a packet belongs to, and what may be sent at all, is decided by its one
 * caller, the service (src/ua101_service.c). That is where the rule "nothing that changes the
 * unit" lives.
 *
 * src/ua101_midiport.c does it with IOKit, the same calls ua101settings used on the unit.
 * tests/test_ctlhelper.c has a pretend one, with the pretend unit behind it.
 */
#ifndef UA101_MIDIPORT_H
#define UA101_MIDIPORT_H

#include <dispatch/dispatch.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct ua101_midiport ua101_midiport;

typedef struct {
    /* What the unit sent: `bytes` is a whole number of 4-byte packets, any cable. `host_time`
       is the Mac's clock (mach_absolute_time) taken when the USB read completed, before the
       packets were handed to the queue. */
    void (*packets)(void *context, const uint8_t *data, size_t bytes, uint64_t host_time);
    /* Reading failed (the unit went, or the interface was taken away). Nothing more comes;
       the caller closes the port. */
    void (*lost)(void *context, const char *why);
} ua101_midiport_events;

/* Opens the interface and starts listening. The events are called on `queue`, one at a time.
   Returns NULL with the reason in `why` if it can't. Sends nothing. */
ua101_midiport *ua101_midiport_open(dispatch_queue_t queue, const ua101_midiport_events *events,
                                    void *context, FILE *log, char *why, size_t why_bytes);

/* Sends whole packets to the unit, waiting a short time at most. Returns 1 if they went. */
int ua101_midiport_send(ua101_midiport *p, const uint8_t *packets, size_t bytes);

/* Stops listening and gives the interface back. No event is called after this returns.
   Call it on the events' queue. */
void ua101_midiport_close(ua101_midiport *p);

#endif
