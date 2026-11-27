/*
 * The control app's end of the link to the helper (src/ua101_link.h, version 20): attach to the
 * unit's control messages, hand one SysEx message towards the unit, ask for levels, and hear
 * when MIDI passes the unit's MIDI sockets (for the app's MIDI lamps).
 *
 * It never waits: every answer comes back as an event on the queue given at open, and every
 * call must be made on that queue too (the app uses the main queue). A helper that hangs or
 * isn't installed therefore can't hang the app's windows.
 *
 * It decides nothing about what may be sent. The app's engine guards that above it, and the
 * helper decides by itself below it (ua101_service_passes and ua101_service_memory_passes in
 * src/ua101_service.h): the settings request, the ordinary changes it lists, Load, Store and
 * Reset by rules of their own, nothing else. (Up to link version 17 the helper also wanted the
 * user's go-ahead files, and this link could ask whether they were there; since version 18
 * there are none.)
 *
 * No xpc types appear here, so Swift can use it through app/panel_shim.h's header.
 */
#ifndef UA101_CTLLINK_H
#define UA101_CTLLINK_H

#include <dispatch/dispatch.h>
#include <stddef.h>
#include <stdint.h>

#define UA101_CTLLINK_MAX_CHANNELS 16
/* An attach not answered after this long is reported as "no helper". */
#define UA101_CTLLINK_ATTACH_SECONDS 5.0

typedef struct {
    int streaming; /* 0: no app plays or records, so there are no levels */
    double rate_hz;
    uint32_t capture_channels, playback_channels;
    /* The largest |sample| since the last ask; 0x80000000 is full scale. */
    uint32_t capture[UA101_CTLLINK_MAX_CHANNELS], playback[UA101_CTLLINK_MAX_CHANNELS];
} ua101_ctl_levels;

typedef struct {
    /* The answer to an attach. status is a UA101_LINK_ value (0: attached). `watching`: the
       helper will say (changed) when a unit appears, so there's no need to ask again. */
    void (*attached)(void *context, int status, int streaming, int watching, const char *why);
    /* One whole SysEx message from the unit's control cable. */
    void (*message)(void *context, const uint8_t *msg, size_t len);
    /* The unit came, went or was lost, the Mac woke, or the helper started again: no longer
       attached; attach again. */
    void (*changed)(void *context);
    void (*audio)(void *context, int streaming);
    void (*levels)(void *context, const ua101_ctl_levels *levels);
    /* A message handed to ua101_ctllink_send wasn't passed on to the unit. */
    void (*refused)(void *context, int status, const char *why);
    /* Told before every `attached` that succeeded: which changes this helper passes on
       (UA101_CTL_PASS_ bits in src/ua101_link.h; with UA101_CTL_PASS_ON, the channels whose
       on/off it passes on are in `passes_on`, UA101_CTL_PASS_ON_CHANNEL bits; the Load, Store
       and Reset commands in `passes_memory`, UA101_CTL_PASS_MEM_ bits; since link version 20
       the memories whose block it reads on request in `passes_read`,
       UA101_CTL_PASS_READ_MEMORY bits, 0 from an older helper). */
    void (*passes)(void *context, uint64_t passes, uint64_t passes_on, uint64_t passes_memory,
                   uint64_t passes_read);
    /* While attached: MIDI arrived at the unit's MIDI IN socket (`in`), or left through its
       MIDI OUT socket (`out`), just now. At most 10 a second, and only while MIDI passes. */
    void (*midi)(void *context, int in, int out);
} ua101_ctllink_events;

typedef struct ua101_ctllink ua101_ctllink;

/* Makes a link; nothing is connected or sent until the first attach. */
ua101_ctllink *ua101_ctllink_open(dispatch_queue_t queue, const ua101_ctllink_events *events,
                                  void *context);
void ua101_ctllink_attach(ua101_ctllink *l);
/* The link version the helper answered to at the last attach: UA101_LINK_VERSION, or
   UA101_LINK_VERSION_OLDER_HELPER for a helper not yet updated (it is then spoken to in its
   own version, and reads no memory). 0 before the first answer. */
unsigned ua101_ctllink_version(const ua101_ctllink *l);
/* Returns 1 if the message was handed to the helper (which may still refuse it). */
int ua101_ctllink_send(ua101_ctllink *l, const uint8_t *msg, size_t len);
void ua101_ctllink_ask_levels(ua101_ctllink *l);
void ua101_ctllink_detach(ua101_ctllink *l);
/* No event is called after this returns. */
void ua101_ctllink_close(ua101_ctllink *l);

/* For a test in one process: links attach to this xpc_endpoint_t instead of the installed
   helper's Mach service; NULL undoes it. */
void ua101_ctllink_use_endpoint(void *xpc_endpoint);

#endif
