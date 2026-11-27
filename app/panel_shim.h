/*
 * A thin C layer between the control engine and the SwiftUI windows.
 * Swift sees only the two opaque handles and plain numbers, never the
 * engine's structs, so the windows can't reach into the engine's state.
 *
 * A message can be handed to two things: the pretend unit (ua101_ctlsim.c),
 * or the helper's link (src/ua101_ctllink.h, which Swift sees through
 * Bridge.h). The helper passes on the settings request,
 * "pause saving", "resume saving" and the patch bay source of
 * the output pairs it names and the volume, the pan and the on/off of the
 * channels it names (all five pairs and every channel since link version 13),
 * and since link version 14 the stereo link of the pairs and the solo of the
 * groups it names (every pair; INPUT and OUTPUT); and since link version 17
 * the Load, Store and Reset commands it names; nothing else. (Up to link
 * version 17 the changes also needed the user's go-ahead files; since
 * version 18 there are none.)
 */
#ifndef UA101_PANEL_SHIM_H
#define UA101_PANEL_SHIM_H

#include <stddef.h>
#include <stdint.h>

typedef struct panel_engine panel_engine; /* really ua101_engine */
typedef struct panel_sim panel_sim;       /* really ua101_sim */

/* Mirrors of the engine's numbers; panel_shim.c checks they still match. */
enum { PANEL_STATE_OFFLINE = 0, PANEL_STATE_SYNCING, PANEL_STATE_READY, PANEL_STATE_SILENT };
enum {
    PANEL_OK = 0,
    PANEL_CLAMPED,
    PANEL_ERR_NOT_READY,
    PANEL_ERR_READ_ONLY,
    PANEL_ERR_SOFT_CTRL,
    PANEL_ERR_NO_CHANNEL,
    PANEL_ERR_VALUE,
    PANEL_ERR_BUSY,
    PANEL_ERR_TICKET
};
enum { PANEL_EV_STATE = 1, PANEL_EV_SETTINGS = 2, PANEL_EV_SETTLING = 4, PANEL_EV_DISAGREE = 8 };
enum { PANEL_F_VOLUME = 0, PANEL_F_PAN, PANEL_F_ON, PANEL_F_LINK, PANEL_F_SOLO, PANEL_F_PATCH };
enum { PANEL_MEM_RESET_ALL = 0, PANEL_MEM_RESET_MEMORY, PANEL_MEM_STORE, PANEL_MEM_LOAD,
       PANEL_MEM_RESET_CURRENT /* the settings in use, no memory: Roland's "Current Setting" */ };
enum { PANEL_SWITCH_DIGITAL = 2, PANEL_SWITCH_MONO = 4, PANEL_SWITCH_SOFT = 5 };

/* What the helper answers (src/ua101_link.h). */
enum {
    PANEL_LINK_OK = 0,
    PANEL_LINK_ERR_VERSION = 1,
    PANEL_LINK_ERR_ABSENT = 2,
    PANEL_LINK_ERR_NO_HELPER = 5,
    PANEL_LINK_ERR_ASLEEP = 6,
    PANEL_LINK_ERR_REFUSED = 7,
    PANEL_LINK_ERR_DENIED = 8
};
/* The changes a helper passes on (UA101_CTL_PASS_). */
enum {
    PANEL_PASS_SAVING = 1,          /* pause and resume the unit's saving */
    PANEL_PASS_PATCH = 2,           /* the patch bay source of some output pairs: */
    PANEL_PASS_PATCH_OUTPUT_0 = 0x100, /* this bit, shifted left by the pair's number 0 to 4 */
    PANEL_PASS_VOLUME = 4,          /* the volume, up to +12 dB, of some channels: */
    PANEL_PASS_VOLUME_CHANNEL_0 = 0x10000, /* this bit, shifted left by the channel's number:
                                       0 and 1 MONITOR OUT L and R, 2 to 11 INPUT 1 to 10,
                                       12 to 21 OUTPUT 1 to 10 */
    PANEL_PASS_PAN = 8,             /* the pan of some channels: */
    PANEL_PASS_ON = 0x10,           /* the on/off of some channels, named in a second number
                                       (ctl_passes_on), PANEL_PASS_ON_CHANNEL_0 << channel */
    PANEL_PASS_LINK = 0x20,         /* the stereo link of some pairs, named in that second
                                       number, PANEL_PASS_LINK_PAIR_0 << pair */
    PANEL_PASS_SOLO = 0x40,         /* the solo of some groups, named in that second number,
                                       PANEL_PASS_SOLO_GROUP_1 << (group - 1) */
    PANEL_PASS_MEMORY = 0x80        /* some Load, Store and Reset commands (link version 17),
                                       named in a
                                       third number (ctl_passes_memory), PANEL_PASS_MEM_ bits */
};
/* In that third number: Load Memory m+1 (m 0 to 4) is this bit shifted left by m, ... */
#define PANEL_PASS_MEM_LOAD_0 1ull
/* ... Store into Memory m+1 this one shifted left by m, Reset Memory m+1 the next ... */
#define PANEL_PASS_MEM_STORE_0 0x100ull
#define PANEL_PASS_MEM_RESET_0 0x10000ull
/* ... and Reset of the settings in use, and Reset of everything. */
#define PANEL_PASS_MEM_RESET_CURRENT 0x1000000ull
#define PANEL_PASS_MEM_RESET_ALL 0x2000000ull
/* In that second number: this bit, shifted left by the channel's number, as for the volume. */
#define PANEL_PASS_ON_CHANNEL_0 1ull
/* In the same number: this bit, shifted left by the pair's number: 0 MONITOR OUT L-R, 1 to 5
 * INPUT 1-2 to 9-10, 6 to 10 OUTPUT 1-2 to 9-10. */
#define PANEL_PASS_LINK_PAIR_0 0x100000000ull
/* In the same number: this bit for the INPUT group's solo, the next one for the OUTPUT group's. */
#define PANEL_PASS_SOLO_GROUP_1 0x1000000000000ull
/* The highest number a group's solo takes: channel 10; 0 is "no solo". */
#define PANEL_MAX_SOLO 10u
/* This bit, shifted left by the channel's number, as for the volume (2 to 21). */
#define PANEL_PASS_PAN_CHANNEL_0 0x10000000000ull
/* What a helper built from this source passes on, in its own two numbers (UA101_CTL_PASSES and
 * UA101_CTL_PASSES_ON in src/ua101_link.h), and its link version: for the self-test, which
 * holds the stand-in helper against them. The app itself reads the list from the helper's
 * answer and never from here. */
uint64_t panel_helper_passes(void);
uint64_t panel_helper_passes_on(void);
uint64_t panel_helper_passes_memory(void);
unsigned panel_helper_link_version(void);
/* Where the user's two go-ahead files were up to link version 17. Nothing looks for them
 * any more (version 18); the report names one that is still there. */
const char *panel_old_go_ahead_path(void);
const char *panel_old_memory_go_ahead_path(void);
/* The helper's times for Load, Store and Reset, in seconds: how fresh the read of the
 * settings before a command must be, and the shortest time between two commands. */
unsigned panel_memory_fresh_seconds(void);
unsigned panel_memory_gap_seconds(void);

/* Full scale in the helper's levels: the largest |sample| there is. */
#define PANEL_LEVEL_FULL_SCALE 0x80000000u

#define PANEL_UNITY_VOLUME 0x200000u
#define PANEL_MAX_VOLUME 0x7FFFFFu
#define PANEL_MAX_PAN 0x8000u
#define PANEL_CENTER_PAN 0x4000u
#define PANEL_MAX_MESSAGE 300

typedef int (*panel_send_fn)(void *ctx, const uint8_t *msg, size_t len);
typedef void (*panel_event_fn)(void *ctx, unsigned events);

/* ---- the engine ---- */
panel_engine *panel_engine_new(panel_send_fn send, panel_event_fn event, void *ctx);
void panel_engine_free(panel_engine *e);
void panel_transport(panel_engine *e, int up, uint64_t now);
void panel_receive(panel_engine *e, const uint8_t *msg, size_t len, uint64_t now);
void panel_tick(panel_engine *e, uint64_t now);
uint64_t panel_next_deadline(const panel_engine *e);
void panel_refresh(panel_engine *e, uint64_t now);
void panel_finish(panel_engine *e, uint64_t now);

void panel_allow_writes(panel_engine *e, int allow);
void panel_allow_memory(panel_engine *e, int allow);
/* The helper refused a change: read-only again, nothing waiting, read the unit. */
void panel_writes_refused(panel_engine *e, uint64_t now);
int panel_writes_allowed(const panel_engine *e);
int panel_memory_allowed(const panel_engine *e);

int panel_state(const panel_engine *e);
int panel_busy(const panel_engine *e);
int panel_settling(const panel_engine *e);
uint32_t panel_volume_ceiling(const panel_engine *e);
uint64_t panel_ticket_life_us(const panel_engine *e);

int panel_rate_code(const panel_engine *e);
uint32_t panel_rate_hz(const panel_engine *e);
int panel_clock_source(const panel_engine *e);
int panel_clock_status(const panel_engine *e);
int panel_mono(const panel_engine *e);
int panel_soft_control(const panel_engine *e);
/* A group's solo as the unit holds it: 0 for none, or the soloed channel's number 1 to 10. */
int panel_solo_raw(const panel_engine *e, unsigned group);
/* 1 if the channel's SOLO lamp is lit: it is its group's soloed channel, or that one's linked
 * partner (as Roland's panel lights it). */
int panel_soloed(const panel_engine *e, unsigned group, unsigned channel);
unsigned panel_group_channels(unsigned group);
/* 1 if the mixer has this channel at the current sample rate. */
int panel_channel_exists(const panel_engine *e, unsigned group, unsigned channel);
uint32_t panel_volume(const panel_engine *e, unsigned group, unsigned channel);
uint32_t panel_pan(const panel_engine *e, unsigned group, unsigned channel);
int panel_monitor_on(const panel_engine *e, unsigned group, unsigned channel);
/* The block's on/off bit itself, 0 or 1, whatever it means (1 is ASSUMED to mean muted). */
int panel_on_bit(const panel_engine *e, unsigned group, unsigned channel);
int panel_link(const panel_engine *e, unsigned group, unsigned pair);
uint32_t panel_patch(const panel_engine *e, unsigned output);
double panel_volume_db(uint32_t volume);

int panel_set_volume(panel_engine *e, unsigned group, unsigned channel, uint32_t v, uint64_t now);
int panel_set_pan(panel_engine *e, unsigned group, unsigned channel, uint32_t v, uint64_t now);
int panel_set_monitor(panel_engine *e, unsigned group, unsigned channel, int on, uint64_t now);
/* Sets the on/off bit itself to 0 or 1, without a meaning put on it. */
int panel_set_on_bit(panel_engine *e, unsigned group, unsigned channel, int bit, uint64_t now);
/* The engine's one assumption: 1 if it takes the bit 1 to mean muted. */
int panel_on_bit_is_mute(const panel_engine *e);
int panel_set_link(panel_engine *e, unsigned group, unsigned pair, int on, uint64_t now);
/* Solo: one channel of the INPUT group or of the OUTPUT group at a time. `channel` 0 to 9, or
 * -1 for "no solo". One message. */
int panel_set_solo(panel_engine *e, unsigned group, int channel, uint64_t now);
/* A click on a channel's SOLO: a lit one puts the group's solo out, a dark one makes this
 * channel the group's one solo. */
int panel_toggle_solo(panel_engine *e, unsigned group, unsigned channel, uint64_t now);
int panel_set_patch(panel_engine *e, unsigned output, uint32_t source, uint64_t now);
int panel_patch_preset(panel_engine *e, int hard_thru, uint64_t now);
void panel_drag_begin(panel_engine *e, uint64_t now);
void panel_drag_end(panel_engine *e, uint64_t now);

/* The unit's saving by itself: see ua101_eng_pause_saving. */
int panel_pause_saving(panel_engine *e, uint64_t now);
int panel_resume_saving(panel_engine *e, uint64_t now);
int panel_saving_held(const panel_engine *e);
/* The last block the unit sent, as it came (256 bytes), and how many have come. */
void panel_unit_block(const panel_engine *e, uint8_t out[256]);
uint64_t panel_blocks_received(const panel_engine *e);
/* Messages from the unit that were neither a block nor a switch: an answer to a change? */
uint64_t panel_other_messages(const panel_engine *e);
/* The two messages, for the words on screen and the record: 13 bytes each. */
void panel_saving_message(int resume, uint8_t out[13]);
/* 1 only for one of those two, byte for byte. */
int panel_is_saving_message(const uint8_t *msg, size_t len);
/* The message that sets an output pair's patch bay source, for the record: 13 bytes, all 0
 * for an output or a source that doesn't exist. */
void panel_patch_message(unsigned output, uint32_t source, uint8_t out[13]);
/* The output pair, 0 to 4, if this is such a message with a source 1 to 11, byte for byte;
 * -1 for anything else. */
int panel_patch_message_output(const uint8_t *msg, size_t len);
/* A VOLUME FOR THE REAL UNIT. A helper passes on only a value that is at most 0x7FFFFF (about
 * +12 dB; up to link version 18, 0 dB), the first
 * digit sent being the highest (proven on the unit, 2026-11-12; src/ua101_service.c,
 * volume_change_passes). The app follows the same rule with code of its own: 1 if v is such
 * a value. */
int panel_volume_sendable(uint32_t v);
/* The message that sets a channel's volume, for the record: 18 bytes, all 0 for a channel or
 * a value that doesn't exist. */
void panel_volume_message(unsigned group, unsigned channel, uint32_t v, uint8_t out[18]);
/* The channel's number, 0 to 21 as in PANEL_PASS_VOLUME_CHANNEL_0, if this is such a message,
 * byte for byte, with a value that is at most 0x7FFFFF; -1 for anything else. */
int panel_volume_message_channel(const uint8_t *msg, size_t len);
/* A PAN FOR THE REAL UNIT. A helper passes on only a value within 0x0000 to 0x8000, the first
 * digit sent being the highest (proven on the unit, 2026-11-12; src/ua101_service.c,
 * pan_change_passes). The app follows the same rule with code of its own: 1 if v is such a
 * value. */
int panel_pan_sendable(uint32_t v);
/* The message that sets a channel's pan, for the record: 16 bytes, all 0 for a channel or a
 * value that doesn't exist. */
void panel_pan_message(unsigned group, unsigned channel, uint32_t v, uint8_t out[16]);
/* The channel's number, 2 to 21 as in PANEL_PASS_PAN_CHANNEL_0, if this is such a message,
 * byte for byte, with a sendable value; -1 for anything else. */
int panel_pan_message_channel(const uint8_t *msg, size_t len);
/* AN ON/OFF FOR THE REAL UNIT. A helper passes on only the bit 0 or 1 (src/ua101_service.c,
 * on_change_passes). The message that sets a channel's on/off bit, for the record: 13 bytes,
 * all 0 for a channel that doesn't exist or a bit other than 0 and 1. */
void panel_on_message(unsigned group, unsigned channel, unsigned bit, uint8_t out[13]);
/* The channel's number, 0 to 21 as for the volume, if this is such a message, byte for byte,
 * with the bit 0 or 1; -1 for anything else. */
int panel_on_message_channel(const uint8_t *msg, size_t len);
/* A LINK FOR THE REAL UNIT. A helper passes on only the bit 0 or 1 (src/ua101_service.c,
 * link_change_passes). The message that sets a pair's stereo link, for the record: 13 bytes,
 * all 0 for a pair that doesn't exist or a bit other than 0 and 1. */
void panel_link_message(unsigned group, unsigned pair, unsigned bit, uint8_t out[13]);
/* The pair's number, 0 to 10 as in PANEL_PASS_LINK_PAIR_0, if this is such a message, byte for
 * byte, with the bit 0 or 1; -1 for anything else. */
int panel_link_message_pair(const uint8_t *msg, size_t len);
/* A SOLO FOR THE REAL UNIT. A helper passes on only 0 (none) or a channel's number 1 to 10
 * (src/ua101_service.c, solo_change_passes). The message that sets a group's solo, for the
 * record: 13 bytes, all 0 for MONITOR OUT, which has none, or a value above 10. */
void panel_solo_message(unsigned group, unsigned value, uint8_t out[13]);
/* The group, 1 or 2, if this is such a message, byte for byte, with a value 0 to 10; -1 for
 * anything else. */
int panel_solo_message_group(const uint8_t *msg, size_t len);
/* The shortest time between two messages. The helper passes on 20 a second at most. */
void panel_set_min_gap(panel_engine *e, uint64_t micros);
/* Changes sent or waiting whose read-back hasn't come yet. */
unsigned panel_unchecked(const panel_engine *e);

/* Load, Store, Reset: two steps. `bytes` gets the message that confirm would send. */
int panel_memory_prepare(panel_engine *e, int command, unsigned memory, int patch_bay, int mixer,
                         uint64_t now, uint32_t *ticket, uint8_t bytes[18], size_t *len);
int panel_memory_confirm(panel_engine *e, uint32_t ticket, uint64_t now);
void panel_memory_cancel(panel_engine *e);
/* The message of a Load, Store or Reset, for the words on screen, with no engine: command is
 * PANEL_MEM_..., memory 0 to 4 (not looked at for PANEL_MEM_RESET_ALL and _RESET_CURRENT),
 * patch_bay and mixer for a Load or a Store. Returns its length, 0 for one that doesn't exist. */
size_t panel_memory_message(int command, unsigned memory, int patch_bay, int mixer, uint8_t out[18]);
/* A LOAD, STORE OR RESET FOR THE REAL UNIT. A helper passes on 37 exact messages only
 * (src/ua101_service.c, memory_command_passes): Load and Store of Memory 1 to 5 with the
 * patch bay, the mixer or both, and Reset of the settings in use, of Memory 1 to 5, or of
 * everything. The app follows the same rule with code of its own: the command's bit as in
 * PANEL_PASS_MEM_ if this is such a message, byte for byte; 0 for anything else. */
uint64_t panel_memory_message_bit(const uint8_t *msg, size_t len);
/* How long after a Load, Store or Reset the engine waits before it reads the settings back. */
void panel_set_memory_settle(panel_engine *e, uint64_t micros);

uint64_t panel_reads_sent(const panel_engine *e);
uint64_t panel_sets_sent(const panel_engine *e);
uint64_t panel_memory_sent(const panel_engine *e);
uint64_t panel_guard_blocks(const panel_engine *e);
uint64_t panel_disagreements(const panel_engine *e);
void panel_last_disagree(const panel_engine *e, int *kind, int *group, int *index,
                         uint32_t *sent, uint32_t *unit);

/* What a reset gives: the value of one setting in the factory block. */
uint32_t panel_factory_value(int kind, unsigned group, unsigned index);
/* The factory block itself, 256 bytes (the block the unit gave after the user's Factory
 * Reset; k_factory in src/ua101_ctlsim.c). */
void panel_factory_block(uint8_t out[256]);
/* For a settings block that isn't the engine's (a backup file, Backup.swift): the value of one
 * setting in it, read the way the engine reads its own; 0 for a setting that doesn't exist. */
uint32_t panel_block_value(const uint8_t block[256], int kind, unsigned group, unsigned index);
/* The switches such a block holds, read the way the engine reads its own: the sample rate in
 * Hz (0 for a code that isn't a rate), the rate's code, and 1 or 0 for SOFT CTRL and MONO. */
void panel_block_switches(const uint8_t block[256], uint32_t *rate_hz, int *rate_code,
                          int *soft_control, int *mono);
/* 1 if that setting exists and takes this value (ua101_field_value_ok). */
int panel_value_ok(int kind, unsigned group, unsigned index, uint32_t value);
/* Changes waiting to be sent: not yet handed towards the unit. */
unsigned panel_waiting(const panel_engine *e);
/* 1 if that channel is on after a reset, read the way the engine reads the bit. */
int panel_factory_monitor_on(const panel_engine *e, unsigned group, unsigned channel);

/* 1 only for the request for the settings block, byte for byte: the one
 * message the way to the real unit lets through in this version. */
int panel_is_read_request(const uint8_t *msg, size_t len);

/* ---- the pretend unit ---- */
panel_sim *panel_sim_new(void);
void panel_sim_free(panel_sim *s);
void panel_sim_receive(panel_sim *s, const uint8_t *msg, size_t len);
/* Copies the next message for the computer into out. Returns its length, 0 for none. */
size_t panel_sim_next(panel_sim *s, uint8_t out[PANEL_MAX_MESSAGE]);
void panel_sim_switch(panel_sim *s, int which, int in, unsigned messages);
void panel_sim_set_rate_code(panel_sim *s, unsigned code);
void panel_sim_set_silent(panel_sim *s, int silent);
/* kind: PANEL_F_..., or -1 for none. The pretend unit then ignores such changes. */
void panel_sim_ignore_kind(panel_sim *s, int kind);
uint32_t panel_sim_value(const panel_sim *s, int kind, unsigned group, unsigned index);
/* The pretend unit's whole settings block as it is now, 256 bytes (for the self-test). */
void panel_sim_block(const panel_sim *s, uint8_t out[256]);
/* For the self-test: what one of the pretend unit's five memories holds (memory 0 to 4), the
 * same put there by hand, and one byte of its settings in use set by hand, as if the unit had
 * changed it by itself. No message does these, and the app never calls them for the real unit. */
void panel_sim_memory(const panel_sim *s, unsigned memory, uint8_t out[256]);
void panel_sim_set_memory(panel_sim *s, unsigned memory, const uint8_t block[256]);
void panel_sim_poke(panel_sim *s, unsigned index, uint8_t value);
int panel_sim_soft_control(const panel_sim *s);
int panel_sim_mono(const panel_sim *s);
int panel_sim_digital(const panel_sim *s);
int panel_sim_rate_code(const panel_sim *s);
int panel_sim_saving_paused(const panel_sim *s);
uint64_t panel_sim_reads(const panel_sim *s);
uint64_t panel_sim_sets_applied(const panel_sim *s);
uint64_t panel_sim_saves(const panel_sim *s);
uint64_t panel_sim_memory_commands(const panel_sim *s);
uint64_t panel_sim_unknown(const panel_sim *s);

#endif
