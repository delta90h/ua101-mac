/*
 * A pretend UA-101 for the control app: it holds a settings block and five
 * memories, answers the read request, applies the set messages and reports
 * front-panel switches, all in SysEx as docs/control-protocol.md describes.
 * It lets the engine and the windows be built with the real unit switched off.
 *
 * WHAT IT PROVES: that the engine speaks the protocol as the notes describe it.
 * WHAT IT DOESN'T: that the real unit behaves this way. Only reading the block
 * and the switch messages were seen on the real unit. Everything about writes
 * here is the notes' reading of Roland's driver, plus these guesses:
 *  - a set is applied silently: no reply and no echo;
 *  - sets are applied in the order they arrive, and a block reply shows them;
 *  - the mixer's values are stored even while Soft Control is out
 *    (set ignore_mixer_without_soft_control to try the other way);
 *  - with saving not paused, each set is saved at once; while paused, one save
 *    happens at the resume;
 *  - Load, Store and Reset copy the sections named in the message; the
 *    switches' section is never copied, since they are physical;
 *  - "reset the settings in use" (Roland's Current Setting) puts the patch bay
 *    and the mixer in use back to the factory ones and leaves the memories.
 */
#ifndef UA101_CTLSIM_H
#define UA101_CTLSIM_H

#include <stddef.h>
#include <stdint.h>

#include "ua101_ctlmsg.h"

#define UA101_SIM_QUEUE 32

typedef enum { UA101_SIM_SWITCH_DIGITAL = 2, UA101_SIM_SWITCH_MONO = 4, UA101_SIM_SWITCH_SOFT = 5 } ua101_sim_switch;

typedef struct {
    uint8_t block[UA101_CTL_BLOCK_BYTES];
    uint8_t memory[UA101_MEMORIES][UA101_CTL_BLOCK_BYTES];

    /* Messages waiting to go to the computer. */
    uint8_t out[UA101_SIM_QUEUE][UA101_CTL_BLOCK_MESSAGE_BYTES];
    size_t out_len[UA101_SIM_QUEUE];
    unsigned out_head, out_count;

    int saving_paused, unsaved;

    /* Ways to misbehave, for the tests. */
    int silent;                             /* answers nothing */
    int corrupt_next_reply;                 /* one block reply with a wrong checksum */
    int ignore_kind_plus_1;                 /* ignores sets of this ua101_field_kind + 1 */
    int ignore_mixer_without_soft_control;

    uint64_t reads, sets_applied, sets_ignored, saves;
    uint64_t memory_commands, unknown_messages, bad_messages, dropped;
    ua101_msg last_memory_command;
} ua101_sim;

/* Starts as the real unit read on 2026-11-06 at 48 kHz (settings-output-48k.txt),
 * which is also what a reset gives back here. */
void ua101_sim_init(ua101_sim *s);
const uint8_t *ua101_sim_factory_block(void);

/* One SysEx message from the computer. */
void ua101_sim_receive(ua101_sim *s, const uint8_t *msg, size_t len);
/* Takes the next message for the computer. Returns 0 when there is none. */
int ua101_sim_next(ua101_sim *s, const uint8_t **msg, size_t *len);

/* A front-panel switch moves and bounces: `messages` change messages are
 * queued, alternating, the last one holding the final value. */
void ua101_sim_switch_move(ua101_sim *s, ua101_sim_switch which, int in, unsigned messages);
/* As if switched off, the rate knob turned, and on again. Code 0 to 4. */
void ua101_sim_set_rate_code(ua101_sim *s, unsigned code);

#endif
