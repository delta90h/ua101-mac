/*
 * The control app's engine: it keeps a picture of the UA-101's settings, turns
 * what the windows ask for into messages, and decides what may be sent.
 * No Apple APIs, no threads, no clock of its own: the caller passes the time
 * in, and passes in the function that carries a message to the unit. So the
 * whole of it runs in a test against the pretend unit (ua101_ctlsim.c).
 *
 * Rules it enforces (docs/control-protocol.md, and the user's standing rules):
 *  - It starts read-only. Until ua101_eng_allow_writes(), only the request for
 *    the settings block can leave it.
 *  - Load, Store, Reset and the preset command need ua101_eng_allow_memory()
 *    AND two steps for each command: prepare, then confirm with the ticket.
 *  - One last check sits on the way out (the guard): every message is read
 *    back and classed, whatever code built it, and refused if not allowed.
 *  - The mixer is refused while Soft Control is out, as Roland's app does.
 *  - Volume is held at 0 dB unless the ceiling is raised.
 *  - Every change is checked: after a quiet moment the block is read again
 *    and compared. The unit's answer wins and a disagreement is reported.
 *  - A drag pauses the unit's saving and resumes it at the end.
 *  - A front-panel switch sends a burst: wait for quiet, then read the block.
 *
 * Call everything from one thread (or one serial queue).
 */
#ifndef UA101_CTLENGINE_H
#define UA101_CTLENGINE_H

#include <stddef.h>
#include <stdint.h>

#include "ua101_ctlmsg.h"

typedef enum {
    UA101_ENG_OFFLINE = 0, /* no way to the unit */
    UA101_ENG_SYNCING,     /* asked for the settings, waiting */
    UA101_ENG_READY,       /* settings known */
    UA101_ENG_SILENT       /* the unit doesn't answer; asking again now and then */
} ua101_eng_state;

typedef enum {
    UA101_ENG_OK = 0,
    UA101_ENG_CLAMPED,        /* done, with the volume held at the ceiling */
    UA101_ENG_ERR_NOT_READY,  /* settings not known yet */
    UA101_ENG_ERR_READ_ONLY,  /* writes haven't been allowed */
    UA101_ENG_ERR_SOFT_CTRL,  /* Soft Control is out: the unit bypasses the mixer */
    UA101_ENG_ERR_NO_CHANNEL, /* no such channel (at this sample rate) */
    UA101_ENG_ERR_VALUE,
    UA101_ENG_ERR_BUSY,        /* changes are still on their way */
    UA101_ENG_ERR_TICKET       /* wrong, used or expired ticket */
} ua101_eng_result;

/* Bits passed to the event function. */
#define UA101_EV_STATE 0x01u    /* ua101_eng_state changed */
#define UA101_EV_SETTINGS 0x02u /* the picture the windows show changed */
#define UA101_EV_SETTLING 0x04u /* a switch moved; a fresh read follows */
#define UA101_EV_DISAGREE 0x08u /* the unit holds another value than was sent */

typedef struct {
    /* Carries one SysEx message towards the unit. Returns 1 if taken. */
    int (*send)(void *ctx, const uint8_t *msg, size_t len);
    /* Optional. Called at the end of a call that changed something. */
    void (*event)(void *ctx, unsigned events);
    void *ctx;
} ua101_eng_io;

typedef struct {
    uint64_t read_timeout_us;  /* wait for the block this long */
    unsigned read_tries;       /* then give up and go SILENT */
    uint64_t silent_retry_us;  /* how often to ask a silent unit again */
    uint64_t settle_us;        /* quiet after a switch message before reading */
    uint64_t verify_quiet_us;  /* quiet after the last change before checking it */
    uint64_t min_gap_us;       /* shortest time between two messages */
    uint64_t memory_settle_us; /* wait after Load/Reset before reading */
    uint64_t ticket_life_us;   /* a prepared memory command expires after this */
    uint64_t lost_reply_us;    /* a read unanswered this long is taken as lost */
    uint32_t volume_ceiling;   /* highest volume accepted */
    int on_bit_is_mute;        /* ASSUMED 1: the block's on/off bit set means muted */
} ua101_eng_config;

void ua101_eng_default_config(ua101_eng_config *cfg);

#define UA101_ENG_READS_OUT 8

typedef struct {
    ua101_eng_io io;
    ua101_eng_config cfg;
    ua101_eng_state state;
    int allow_writes, allow_memory;

    uint8_t unit[UA101_CTL_BLOCK_BYTES]; /* the last block the unit sent */
    uint8_t want[UA101_CTL_BLOCK_BYTES]; /* the same, with our changes: what is shown */
    ua101_settings shown;                /* want, parsed */

    /* Per field: waiting to be sent; sent but not yet checked; when it was sent. */
    uint8_t queued[UA101_FIELD_COUNT];
    uint8_t unverified[UA101_FIELD_COUNT];
    uint64_t sent_seq[UA101_FIELD_COUNT];
    uint8_t fifo[UA101_FIELD_COUNT];
    unsigned fifo_head, fifo_count;
    unsigned unverified_count;

    uint64_t seq; /* counts every message sent */
    uint64_t next_send_at, last_write_at;

    int read_waiting, read_wanted;
    uint64_t read_seq, read_deadline, read_at;
    unsigned read_tries_done;
    /* Reads sent and not answered yet, oldest first. Replies come back in the
     * order the requests went out, so each block belongs to the oldest one. */
    uint64_t asked_seq[UA101_ENG_READS_OUT], asked_at[UA101_ENG_READS_OUT];
    unsigned asked;

    unsigned drags;
    int saving_paused; /* the suspend message has gone out */
    /* The unit's saving paused by itself (ua101_eng_pause_saving): 1 held, 2 let go, with a
     * read owed after the resume. 0 otherwise. */
    int saving_hold;
    int settling;
    uint64_t last_change_at;

    ua101_msg ticket_msg;
    uint32_t ticket, ticket_counter;
    uint64_t ticket_expires;
    int memory_pass; /* set only while a confirmed memory command goes out */

    unsigned events;

    /* Counters, for the windows and the tests. */
    uint64_t reads_sent, sets_sent, memory_sent;
    uint64_t blocks_received; /* settings blocks taken from the unit */
    uint64_t guard_blocks;   /* messages the guard stopped: should stay 0 */
    uint64_t send_failures;
    uint64_t bad_messages, other_messages;
    uint64_t disagreements;
    ua101_field last_disagree_field;
    uint32_t last_disagree_sent, last_disagree_unit;
} ua101_engine;

void ua101_eng_init(ua101_engine *e, const ua101_eng_io *io, const ua101_eng_config *cfg);

/* The way to the unit came up (asks for the settings) or went away. */
void ua101_eng_transport(ua101_engine *e, int up, uint64_t now);
/* One complete SysEx message from the unit's control cable. */
void ua101_eng_receive(ua101_engine *e, const uint8_t *msg, size_t len, uint64_t now);
/* Does whatever is due. Call it at ua101_eng_next_deadline(), not on a timer. */
void ua101_eng_tick(ua101_engine *e, uint64_t now);
/* When tick is next needed; UINT64_MAX when nothing is pending. */
uint64_t ua101_eng_next_deadline(const ua101_engine *e);

/* The app's two switches (its Lock Changes turns both off). Both are off after init. */
void ua101_eng_allow_writes(ua101_engine *e, int allow);
void ua101_eng_allow_memory(ua101_engine *e, int allow);
/* The way to the unit refused a change (the helper: it isn't a change this helper passes
 * on, or a Load, Store or Reset against one of its rules). Both switches go off, what was
 * waiting is thrown away, nothing more is owed to the unit, and the block is read again so
 * the picture is the unit's. If a pause of the unit's saving got through before the refusal,
 * the unit may still be paused: that can't be read, and is mended by switching changes on
 * again and resuming. */
void ua101_eng_writes_refused(ua101_engine *e, uint64_t now);
/* Raise or lower the highest volume accepted (at most UA101_MAX_VOLUME). */
void ua101_eng_set_volume_ceiling(ua101_engine *e, uint32_t ceiling);

/* What the windows show. Valid in READY (and kept, stale, in SILENT). */
const ua101_settings *ua101_eng_settings(const ua101_engine *e);
/* 1 if the monitor channel is on, by the assumed meaning of the bit. */
int ua101_eng_monitor_on(const ua101_engine *e, unsigned group, unsigned channel);
/* 1 while any change is waiting to be sent or checked. */
int ua101_eng_busy(const ua101_engine *e);
/* Ask for the block again. */
void ua101_eng_refresh(ua101_engine *e, uint64_t now);

/* Mixer. With the pair linked, volume and on/off go to both channels. */
ua101_eng_result ua101_eng_set_volume(ua101_engine *e, unsigned group, unsigned channel,
                                      uint32_t volume, uint64_t now);
ua101_eng_result ua101_eng_set_pan(ua101_engine *e, unsigned group, unsigned channel,
                                   uint32_t pan, uint64_t now);
ua101_eng_result ua101_eng_set_monitor(ua101_engine *e, unsigned group, unsigned channel,
                                       int on, uint64_t now);
ua101_eng_result ua101_eng_set_link(ua101_engine *e, unsigned group, unsigned pair, int on,
                                    uint64_t now);
/* Solo, as Roland's own program does it (tried on the unit, 2026-11-18): the INPUT
   group and the OUTPUT group each hold one number, 0 for "no solo" or the one soloed channel's
   number, 1 to 10. MONITOR OUT has no solo. `channel` is 0 to 9, or UA101_ENG_SOLO_NONE. One
   message, whether or not the pair is linked. */
#define UA101_ENG_SOLO_NONE (-1)
ua101_eng_result ua101_eng_set_solo(ua101_engine *e, unsigned group, int channel, uint64_t now);
/* 1 if the channel's SOLO lamp is lit, as Roland's panel lights it: the channel is its
   group's soloed one, or the linked partner of it. */
int ua101_eng_soloed(const ua101_engine *e, unsigned group, unsigned channel);
/* A click on a channel's SOLO, as Roland's panel does it: on a lit one the group's solo goes
   out; on a dark one this channel becomes the group's one solo. */
ua101_eng_result ua101_eng_toggle_solo(ua101_engine *e, unsigned group, unsigned channel,
                                       uint64_t now);
/* Patch bay. Source: 1 MONITOR OUT, 2-6 INPUT pairs, 7-11 WAVE OUT pairs. */
ua101_eng_result ua101_eng_set_patch(ua101_engine *e, unsigned output, uint32_t source,
                                     uint64_t now);
/* Hard Thru or Initial Setting, made of five ordinary patch changes. */
ua101_eng_result ua101_eng_patch_preset(ua101_engine *e, int hard_thru, uint64_t now);

/* Around a mouse drag: the unit's saving is paused from the first change to the last. */
void ua101_eng_drag_begin(ua101_engine *e, uint64_t now);
void ua101_eng_drag_end(ua101_engine *e, uint64_t now);

/* The unit's saving, by itself and with no other change: the first write ever made on a real
 * unit. Pause sends "pause saving" at once, keeps it paused and reads the
 * block after the usual quiet time; resume sends "resume saving" and reads again. Whether the
 * unit took either can't be read from the block: the reads show that nothing else changed.
 * If the way to the unit drops or it goes silent meanwhile, the resume is owed and is sent
 * when the unit answers again, as after a drag. */
ua101_eng_result ua101_eng_pause_saving(ua101_engine *e, uint64_t now);
ua101_eng_result ua101_eng_resume_saving(ua101_engine *e, uint64_t now);
/* 1 between a pause and its resume. */
int ua101_eng_saving_held(const ua101_engine *e);

/* Load, Store, Reset, preset: m->kind is one of the UA101_MSG_ memory kinds.
 * Prepare gives a ticket; nothing is sent until confirm gets the same ticket. */
ua101_eng_result ua101_eng_memory_prepare(ua101_engine *e, const ua101_msg *m, uint64_t now,
                                          uint32_t *ticket);
ua101_eng_result ua101_eng_memory_confirm(ua101_engine *e, uint32_t ticket, uint64_t now);
void ua101_eng_memory_cancel(ua101_engine *e);

/* Before quitting: sends what is waiting and resumes the unit's saving. */
void ua101_eng_finish(ua101_engine *e, uint64_t now);

#endif
