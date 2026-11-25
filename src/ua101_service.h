/*
 * The helper's side of the link (src/ua101_link.h): a Mach service the audio plug-in connects
 * to. It opens the unit when a plug-in says hello or asks for streaming, streams while any
 * plug-in has asked it to, and gives back the unit after a while without streaming, so other
 * programs can use it (a design choice, 2026-11-10); the next hello or start opens it again,
 * with a new block. Core Audio keeps its plug-in connected all the time, so the helper stays as
 * an idle process; with no plug-in connected it exits after a while and launchd starts it again
 * on demand. Everything runs on one serial queue, so nothing here needs a lock: each XPC
 * connection, the timers and the watcher's events all step onto it.
 *
 * It also watches the bus and the Mac's sleep (src/ua101_watch.h). A unit that is unplugged or
 * switched off is closed and the plug-ins told at once, streaming or idle; one that appears is
 * announced, so plug-ins needn't keep asking. Before the Mac sleeps the unit is given back, and
 * after it wakes a plug-in whose stream was cut is told to start over. A unit that is on the
 * bus but keeps failing to stream is given up on until it is switched off and on.
 *
 * For the control app it also holds the unit's MIDI interface (src/ua101_midiport.h) while such
 * an app is attached, passes on what the unit says on the control cable, and gives levels from
 * the audio it streams (src/ua101_link.h, version 15). It passes on to the unit: the request
 * for the settings, always; and "pause saving", "resume saving", the patch bay source of an
 * output pair, of the mixer's channels the volume up to 0 dB, the pan and the on/off, of its
 * pairs the stereo link, and of the INPUT and the OUTPUT group the solo, each only as the
 * messages and within the values tried on the unit; and since version 17 Load, Store and
 * Reset of the unit's memories, 37 exact messages, never while sound streams, only after a
 * fresh read of the settings (which it writes into its log first), and one at a time
 * (ua101_service_memory_passes). Up to version 17 all of these also needed the user's
 * go-ahead files; since version 18 the helper looks for none (src/ua101_link.h).
 * Since version 15 it also tells each attached control app, at most 10 times a second and only
 * while MIDI passes, that MIDI arrived at the MIDI IN socket or really left through MIDI OUT
 * (src/ua101_midilamp.h), for the app's two MIDI lamps; nothing goes to the unit for it.
 * Nothing else, whatever the app asks. The check is ua101_service_passes, used by
 * send_to_unit in src/ua101_service.c, and this program isn't built with the code that makes a
 * message (src/ua101_ctlbuild.c).
 *
 * For MIDI clients (the per-user agent that publishes "UA-101 MIDI", link version 12) it
 * holds the same interface while one is attached, gives them what arrives at the MIDI IN
 * socket (cable 0), with the time the USB read completed, and sends what they hand over to the
 * MIDI OUT socket through src/ua101_midigate.h: cable 0 only, never SysEx addressed to the
 * UA-101 itself, at the wire's speed. That is a second sender beside send_to_unit, and
 * nothing of a MIDI client's passes through send_to_unit or reaches the control cable.
 */
#ifndef UA101_SERVICE_H
#define UA101_SERVICE_H

#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <xpc/xpc.h>

#include "ua101_midigate.h"
#include "ua101_shared.h"

typedef struct {
    /* The safety offsets given to the plug-in, in frames, for tests; negative for the usual
       times (UA101_INPUT_OFFSET_SECONDS, UA101_OUTPUT_OFFSET_SECONDS) at the unit's rate. */
    int64_t input_offset, output_offset;
    unsigned capture_queue, playback_queue;
    double idle_exit_seconds; /* with no plug-in connected; 0 never exits */
    double idle_close_seconds; /* open but not streaming this long, the unit is given back; 0 never */
    /* Watches the bus and the Mac's sleep (src/ua101_watch.h): a unit that goes or comes is
       noticed at once, also while idle, and the unit is given back before the Mac sleeps. */
    int watch;
    int allow_uid;            /* also accept this user, for the in-process test; -1 for none */
    FILE *log;
} ua101_service_options;

/* As installed: listens on UA101_SERVICE_NAME, which launchd must know, and never returns
   unless that fails. */
int ua101_service_run(const ua101_service_options *opt);

/* For a test in one process: listens without launchd and returns an endpoint a link can connect
   to (src/plugin/ua101_link_xpc.h). */
xpc_endpoint_t ua101_service_start_anonymous(const ua101_service_options *opt);

/*
 * Who may ask for what. Audio (hello, start, stop) is for Core Audio's user; control (ctl_open,
 * ctl_send, ctl_close, levels) and MIDI (midi_open, midi_send, midi_close) are for the user
 * the helper runs as (the account that installed it) and for every other account a person logs
 * in to (ua101_service_person), so each account on the Mac gets the mixer and MIDI; never for
 * Core Audio's user. Root, and a test's allow_uid, may ask for both.
 */
int ua101_service_allows(uid_t uid, int control_request, uid_t coreaudio_uid, uid_t own_uid,
                         int allow_uid);
/* Whether `uid` is an account a person logs in to: 501 and up, not nobody (-2). */
int ua101_service_person(uid_t uid);
/* Whether a request belongs to the user's side (a control app's or a MIDI client's: ctl_...,
   levels, midi_...), which is what `control_request` above means; every other is audio's. */
int ua101_service_owner_request(const char *op);

/* The stamp at the start of each line of the helper's log: "2026-11-12 12:02:37.364", local
   time to the millisecond. */
void ua101_service_stamp(char *out, size_t bytes, time_t seconds, long microseconds);

/*
 * The log's size limit. launchd appends the helper's lines to one file
 * (/Library/Logs/ua101d.log, the plist's StandardErrorPath) and nothing else trims it. So when
 * the helper starts (at boot and after an install), before it opens anything, a log over
 * UA101_LOG_LIMIT_BYTES is cut down to its newest UA101_LOG_KEEP_BYTES or less, starting at a
 * whole line. Cut in place: the helper runs as the user and can't rename in /Library/Logs.
 */
#define UA101_LOG_LIMIT_BYTES (1024 * 1024)
#define UA101_LOG_KEEP_BYTES (512 * 1024)

/* Trims the file open on `fd` as above, with the given limit and keep (keep < limit). Returns
   the bytes removed: 0 for a file at or under the limit, or one that isn't a regular file;
   -1 if it couldn't. Afterwards the descriptor writes at the end of the file (O_APPEND). */
long long ua101_service_trim_log(int fd, long long limit, long long keep);

#define UA101_SAVING_MESSAGE_BYTES 13
#define UA101_VOLUME_MESSAGE_BYTES 18
#define UA101_PAN_MESSAGE_BYTES 16
#define UA101_ON_MESSAGE_BYTES 13
#define UA101_LINK_MESSAGE_BYTES 13
#define UA101_SOLO_MESSAGE_BYTES 13
#define UA101_MEMORY_MESSAGE_BYTES 16 /* a Load or a Store */
#define UA101_RESET_MESSAGE_BYTES 13

/*
 * Whether a control app's message is passed on to the unit. Passed on: the request for the
 * settings. "Pause saving" and "resume saving", byte for byte. The patch bay source, 1 to 11,
 * of an output pair in UA101_PATCH_OUTPUTS_PASSED (src/ua101_link.h). The volume of a channel
 * in UA101_VOLUMES_PASSED, when the value is at most 0 dB (the first digit sent being the
 * highest, proven on the unit). The pan of a channel in UA101_PANS_PASSED, when the value is
 * within 0x0000 to 0x8000 (the first digit sent being the highest, proven on the unit). The
 * on/off of a channel in UA101_ONS_PASSED, set to 0 or 1, byte for byte as one of that
 * channel's two messages written out in src/ua101_service.c. (Up to link version 17 each of
 * these changes also needed the user's go-ahead file; since version 18 none does.) Since
 * link version 13 the three lists name every channel of the mixer (the pan's: every one but
 * MONITOR OUT L and R, which have none).
 * Since link version 14: the stereo link of a pair in UA101_LINKS_PASSED, set to 0 or 1, and
 * the solo of a group in UA101_SOLOS_PASSED, set to 0 (none) or to a channel 1 to
 * UA101_SOLO_PASSED_MAX, each byte for byte as one of the messages written out in
 * src/ua101_service.c.
 * Every other message that sets something, and everything unknown: never. A Load, Store or
 * Reset: never by this function; they are judged by ua101_service_memory_passes below, which
 * needs to know the moment. `why` gets the reason for a refusal.
 */
int ua101_service_passes(const uint8_t *msg, size_t len, const char **why);

/*
 * Load, Store and Reset (link version 17). Everything such a message is judged by at the
 * moment it arrives: the helper fills this in (send_to_unit), a test writes any moment it
 * likes. Times are in microseconds on one clock that only runs forward; 0 means "never".
 */
typedef struct {
    int streaming;       /* sound is being played or recorded through the unit */
    uint64_t now_us;
    uint64_t block_us;   /* when the unit last sent its whole settings block */
    uint64_t change_us;  /* when a change was last passed on to the unit */
    uint64_t memory_us;  /* when a Load, Store or Reset was last passed on */
} ua101_service_moment;

/*
 * Whether a Load, Store or Reset is passed on to the unit. Every one of these must hold:
 *  - it is one of the 37 messages written out in src/ua101_service.c (Load and Store of Memory
 *    1 to 5 with the patch bay, the mixer or both; Reset of the settings in use, of Memory 1
 *    to 5, or of everything), by the parser AND byte for byte; Roland's patch bay preset: never;
 *  - no sound is streaming;
 *  - none was passed on in the UA101_MEMORY_GAP_SECONDS before;
 *  - the unit sent its whole settings block (a control app asked for it) within the
 *    UA101_MEMORY_FRESH_SECONDS before, and after the last change and the last such command.
 * `why` gets the reason for a refusal.
 */
int ua101_service_memory_passes(const uint8_t *msg, size_t len, const ua101_service_moment *at,
                                const char **why);

/* What the control side did, for the tests and the log. */
typedef struct {
    uint64_t ctl_sent;     /* settings requests passed on to the unit */
    uint64_t ctl_changes;  /* messages that change the unit passed on (saving, patch bay, volume, pan, on/off) */
    uint64_t ctl_memory;   /* of those: Load, Store and Reset commands (link version 17) */
    uint64_t ctl_refused;  /* messages not passed on: anything else, or too many */
    uint64_t ctl_received; /* control messages from the unit, passed to the apps */
    uint64_t midi_packets; /* packets from the MIDI IN socket (cable 0): passed to the MIDI
                              clients attached then, dropped if there was none */
    uint64_t levels_given;
    uint64_t denied;       /* requests from a user who may not make them */
    int port_open;         /* the MIDI interface is held now */
    /* The MIDI clients' side. */
    int midi_clients;              /* attached now */
    uint64_t midi_in_events;       /* "midi" events sent to clients, one per read with cable 0 in it */
    uint64_t midi_in_bytes;        /* MIDI bytes in them */
    uint64_t midi_refused;         /* midi_send from a client that wasn't attached, or too long */
    uint64_t midi_write_failures;  /* writes the unit didn't take in time */
    ua101_midisplit_counts split;  /* what arrived, by cable */
    ua101_midigate_counts gate;    /* what the clients handed over, and what became of it */
    /* The control app's MIDI lamps (link version 15, src/ua101_midilamp.h). */
    uint64_t midi_lamp_events;     /* "midi_lamps" events told (once each, however many apps) */
    uint64_t midi_lamp_wakeups;    /* one-shot wake-ups that came */
    int midi_lamp_timer_on;        /* a wake-up is booked now: only within 0.1 s of MIDI */
} ua101_service_counts;

void ua101_service_get_counts(ua101_service_counts *out);

/* For a test: stops streaming and gives back the unit, as at exit. */
void ua101_service_shutdown(void);

/* For a test: the helper's own mapping of the block, for its counters; NULL while the unit
   isn't open. */
const ua101_shared *ua101_service_block(void);

#endif
