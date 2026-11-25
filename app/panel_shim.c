#include "panel_shim.h"

#include <stdlib.h>
#include <string.h>

#include "../src/ua101_ctlengine.h"
#include "../src/ua101_ctllink.h"
#include "../src/ua101_ctlsim.h"
#include "../src/ua101_link.h"

_Static_assert(PANEL_LINK_OK == UA101_LINK_OK && PANEL_LINK_ERR_VERSION == UA101_LINK_ERR_VERSION &&
               PANEL_LINK_ERR_ABSENT == UA101_LINK_ERR_ABSENT && PANEL_LINK_ERR_NO_HELPER == UA101_LINK_ERR_NO_HELPER &&
               PANEL_LINK_ERR_ASLEEP == UA101_LINK_ERR_ASLEEP && PANEL_LINK_ERR_REFUSED == UA101_LINK_ERR_REFUSED &&
               PANEL_LINK_ERR_DENIED == UA101_LINK_ERR_DENIED, "link answers");

_Static_assert(PANEL_STATE_OFFLINE == (int)UA101_ENG_OFFLINE && PANEL_STATE_SYNCING == (int)UA101_ENG_SYNCING &&
               PANEL_STATE_READY == (int)UA101_ENG_READY && PANEL_STATE_SILENT == (int)UA101_ENG_SILENT, "states");
_Static_assert(PANEL_OK == (int)UA101_ENG_OK && PANEL_CLAMPED == (int)UA101_ENG_CLAMPED &&
               PANEL_ERR_NOT_READY == (int)UA101_ENG_ERR_NOT_READY && PANEL_ERR_READ_ONLY == (int)UA101_ENG_ERR_READ_ONLY &&
               PANEL_ERR_SOFT_CTRL == (int)UA101_ENG_ERR_SOFT_CTRL && PANEL_ERR_NO_CHANNEL == (int)UA101_ENG_ERR_NO_CHANNEL &&
               PANEL_ERR_VALUE == (int)UA101_ENG_ERR_VALUE &&
               PANEL_ERR_BUSY == (int)UA101_ENG_ERR_BUSY && PANEL_ERR_TICKET == (int)UA101_ENG_ERR_TICKET, "results");
_Static_assert(PANEL_EV_STATE == UA101_EV_STATE && PANEL_EV_SETTINGS == UA101_EV_SETTINGS &&
               PANEL_EV_SETTLING == UA101_EV_SETTLING && PANEL_EV_DISAGREE == UA101_EV_DISAGREE, "events");
_Static_assert(PANEL_F_VOLUME == (int)UA101_F_VOLUME && PANEL_F_PAN == (int)UA101_F_PAN && PANEL_F_ON == (int)UA101_F_ON &&
               PANEL_F_LINK == (int)UA101_F_LINK && PANEL_F_SOLO == (int)UA101_F_SOLO && PANEL_F_PATCH == (int)UA101_F_PATCH, "fields");
_Static_assert(PANEL_SWITCH_DIGITAL == (int)UA101_SIM_SWITCH_DIGITAL && PANEL_SWITCH_MONO == (int)UA101_SIM_SWITCH_MONO &&
               PANEL_SWITCH_SOFT == (int)UA101_SIM_SWITCH_SOFT, "switches");
_Static_assert(PANEL_UNITY_VOLUME == UA101_CTL_UNITY_VOLUME && PANEL_MAX_VOLUME == UA101_MAX_VOLUME &&
               PANEL_MAX_PAN == UA101_MAX_PAN && PANEL_CENTER_PAN == UA101_CENTER_PAN, "values");
_Static_assert(PANEL_MAX_MESSAGE >= UA101_CTL_BLOCK_MESSAGE_BYTES, "message size");
_Static_assert(PANEL_PASS_SAVING == UA101_CTL_PASS_SAVING, "what a helper passes on");
_Static_assert(PANEL_PASS_PATCH == UA101_CTL_PASS_PATCH, "what a helper passes on");
_Static_assert(PANEL_PASS_PATCH_OUTPUT_0 == UA101_CTL_PASS_PATCH_OUTPUT(0) &&
                   (PANEL_PASS_PATCH_OUTPUT_0 << 4) == UA101_CTL_PASS_PATCH_OUTPUT(4),
               "what a helper passes on");
_Static_assert(PANEL_PASS_VOLUME == UA101_CTL_PASS_VOLUME &&
                   PANEL_PASS_VOLUME_CHANNEL_0 == UA101_CTL_PASS_VOLUME_CHANNEL(0),
               "what a helper passes on");
_Static_assert(PANEL_PASS_PAN == UA101_CTL_PASS_PAN &&
                   PANEL_PASS_PAN_CHANNEL_0 == UA101_CTL_PASS_PAN_CHANNEL(0),
               "what a helper passes on");
_Static_assert(PANEL_PASS_ON == UA101_CTL_PASS_ON && PANEL_PASS_ON_CHANNEL_0 == UA101_CTL_PASS_ON_CHANNEL(0),
               "what a helper passes on");
_Static_assert(PANEL_PASS_LINK == UA101_CTL_PASS_LINK && PANEL_PASS_LINK_PAIR_0 == UA101_CTL_PASS_LINK_PAIR(0) &&
                   PANEL_PASS_SOLO == UA101_CTL_PASS_SOLO && PANEL_PASS_SOLO_GROUP_1 == UA101_CTL_PASS_SOLO_GROUP(1) &&
                   PANEL_MAX_SOLO == UA101_MAX_SOLO && PANEL_MAX_SOLO == UA101_SOLO_PASSED_MAX,
               "what a helper passes on");
_Static_assert(PANEL_PASS_MEMORY == UA101_CTL_PASS_MEMORY && PANEL_PASS_MEM_LOAD_0 == UA101_CTL_PASS_MEM_LOAD(0) &&
                   PANEL_PASS_MEM_STORE_0 == UA101_CTL_PASS_MEM_STORE(0) &&
                   PANEL_PASS_MEM_RESET_0 == UA101_CTL_PASS_MEM_RESET(0) &&
                   PANEL_PASS_MEM_RESET_CURRENT == UA101_CTL_PASS_MEM_RESET_CURRENT &&
                   PANEL_PASS_MEM_RESET_ALL == UA101_CTL_PASS_MEM_RESET_ALL,
               "what a helper passes on");
_Static_assert(UA101_CTL_BLOCK_BYTES == 256, "block size");

#define E(e) ((ua101_engine *)(e))
#define CE(e) ((const ua101_engine *)(e))
#define S(s) ((ua101_sim *)(s))
#define CS(s) ((const ua101_sim *)(s))

panel_engine *panel_engine_new(panel_send_fn send, panel_event_fn event, void *ctx)
{
    ua101_engine *e = calloc(1, sizeof(*e));
    ua101_eng_io io;

    if (!e)
        return NULL;
    io.send = send;
    io.event = event;
    io.ctx = ctx;
    ua101_eng_init(e, &io, NULL);
    return (panel_engine *)e;
}

void panel_engine_free(panel_engine *e) { free(e); }
void panel_transport(panel_engine *e, int up, uint64_t now) { ua101_eng_transport(E(e), up, now); }
void panel_receive(panel_engine *e, const uint8_t *msg, size_t len, uint64_t now) { ua101_eng_receive(E(e), msg, len, now); }
void panel_tick(panel_engine *e, uint64_t now) { ua101_eng_tick(E(e), now); }
uint64_t panel_next_deadline(const panel_engine *e) { return ua101_eng_next_deadline(CE(e)); }
void panel_refresh(panel_engine *e, uint64_t now) { ua101_eng_refresh(E(e), now); }
void panel_finish(panel_engine *e, uint64_t now) { ua101_eng_finish(E(e), now); }

void panel_allow_writes(panel_engine *e, int allow) { ua101_eng_allow_writes(E(e), allow); }
void panel_allow_memory(panel_engine *e, int allow) { ua101_eng_allow_memory(E(e), allow); }
int panel_writes_allowed(const panel_engine *e) { return CE(e)->allow_writes; }
int panel_memory_allowed(const panel_engine *e) { return CE(e)->allow_memory; }

int panel_state(const panel_engine *e) { return (int)CE(e)->state; }
int panel_busy(const panel_engine *e) { return ua101_eng_busy(CE(e)); }
int panel_settling(const panel_engine *e) { return CE(e)->settling; }
uint32_t panel_volume_ceiling(const panel_engine *e) { return CE(e)->cfg.volume_ceiling; }
uint64_t panel_ticket_life_us(const panel_engine *e) { return CE(e)->cfg.ticket_life_us; }

int panel_rate_code(const panel_engine *e) { return ua101_eng_settings(CE(e))->rate_code; }
uint32_t panel_rate_hz(const panel_engine *e) { return ua101_ctl_rate_hz(ua101_eng_settings(CE(e))->rate_code); }
int panel_clock_source(const panel_engine *e) { return ua101_eng_settings(CE(e))->clock_source; }
int panel_clock_status(const panel_engine *e) { return ua101_eng_settings(CE(e))->clock_status; }
int panel_mono(const panel_engine *e) { return ua101_eng_settings(CE(e))->mono; }
int panel_soft_control(const panel_engine *e) { return ua101_eng_settings(CE(e))->soft_control; }
int panel_solo_raw(const panel_engine *e, unsigned group)
{
    return group >= 1 && group <= 2 ? ua101_eng_settings(CE(e))->solo[group - 1] : 0;
}
int panel_soloed(const panel_engine *e, unsigned group, unsigned channel) { return ua101_eng_soloed(CE(e), group, channel); }
unsigned panel_group_channels(unsigned group) { return ua101_ctl_group_channels(group); }

int panel_channel_exists(const panel_engine *e, unsigned group, unsigned channel)
{
    if (group >= UA101_CTL_GROUPS || channel >= ua101_ctl_group_channels(group))
        return 0;
    /* As the engine: at 192 kHz (rate code 4) the mixer has channels 1 to 6. */
    if (ua101_eng_settings(CE(e))->rate_code == 4 && group != UA101_GROUP_MONITOR && channel >= 6)
        return 0;
    return 1;
}

static int has(unsigned group, unsigned channel)
{
    return group < UA101_CTL_GROUPS && channel < ua101_ctl_group_channels(group);
}

uint32_t panel_volume(const panel_engine *e, unsigned group, unsigned channel)
{
    return has(group, channel) ? ua101_eng_settings(CE(e))->channel[group][channel].volume : 0;
}
uint32_t panel_pan(const panel_engine *e, unsigned group, unsigned channel)
{
    return has(group, channel) ? ua101_eng_settings(CE(e))->channel[group][channel].pan : 0;
}
int panel_monitor_on(const panel_engine *e, unsigned group, unsigned channel)
{
    return ua101_eng_monitor_on(CE(e), group, channel);
}
int panel_on_bit(const panel_engine *e, unsigned group, unsigned channel)
{
    return has(group, channel) ? ua101_eng_settings(CE(e))->channel[group][channel].on != 0 : 0;
}
int panel_link(const panel_engine *e, unsigned group, unsigned pair)
{
    return has(group, pair * 2) ? ua101_eng_settings(CE(e))->link[group][pair] : 0;
}
uint32_t panel_patch(const panel_engine *e, unsigned output)
{
    return output < UA101_CTL_PATCH_OUTPUTS ? ua101_eng_settings(CE(e))->patch[1][output] : 0;
}
double panel_volume_db(uint32_t volume) { return ua101_ctl_volume_db(volume); }

int panel_set_volume(panel_engine *e, unsigned g, unsigned c, uint32_t v, uint64_t now) { return (int)ua101_eng_set_volume(E(e), g, c, v, now); }
int panel_set_pan(panel_engine *e, unsigned g, unsigned c, uint32_t v, uint64_t now) { return (int)ua101_eng_set_pan(E(e), g, c, v, now); }
int panel_set_monitor(panel_engine *e, unsigned g, unsigned c, int on, uint64_t now) { return (int)ua101_eng_set_monitor(E(e), g, c, on, now); }
/* The raw bit, whatever it means: the engine takes "on", and turns it into the bit by its
   one assumption (cfg.on_bit_is_mute), so here the bit is turned back by the same one. */
int panel_set_on_bit(panel_engine *e, unsigned g, unsigned c, int bit, uint64_t now)
{
    return (int)ua101_eng_set_monitor(E(e), g, c, (bit != 0) != (CE(e)->cfg.on_bit_is_mute != 0), now);
}
int panel_on_bit_is_mute(const panel_engine *e) { return CE(e)->cfg.on_bit_is_mute != 0; }
int panel_set_link(panel_engine *e, unsigned g, unsigned pair, int on, uint64_t now) { return (int)ua101_eng_set_link(E(e), g, pair, on, now); }
int panel_set_solo(panel_engine *e, unsigned g, int channel, uint64_t now)
{
    return (int)ua101_eng_set_solo(E(e), g, channel < 0 ? UA101_ENG_SOLO_NONE : channel, now);
}
int panel_toggle_solo(panel_engine *e, unsigned g, unsigned c, uint64_t now) { return (int)ua101_eng_toggle_solo(E(e), g, c, now); }
int panel_set_patch(panel_engine *e, unsigned output, uint32_t source, uint64_t now) { return (int)ua101_eng_set_patch(E(e), output, source, now); }
int panel_patch_preset(panel_engine *e, int hard_thru, uint64_t now) { return (int)ua101_eng_patch_preset(E(e), hard_thru, now); }
void panel_drag_begin(panel_engine *e, uint64_t now) { ua101_eng_drag_begin(E(e), now); }
void panel_drag_end(panel_engine *e, uint64_t now) { ua101_eng_drag_end(E(e), now); }

/* A Load, Store or Reset as the engine's message. Returns 0 for one that doesn't exist. */
static int memory_msg(ua101_msg *m, int command, unsigned memory, int patch_bay, int mixer)
{
    memset(m, 0, sizeof(*m));
    switch (command) {
    case PANEL_MEM_RESET_ALL:
        m->kind = UA101_MSG_RESET_ALL;
        return 1;
    case PANEL_MEM_RESET_CURRENT:
        m->kind = UA101_MSG_RESET_CURRENT;
        return 1;
    case PANEL_MEM_RESET_MEMORY:
        m->kind = UA101_MSG_RESET_MEMORY;
        break;
    case PANEL_MEM_STORE:
        m->kind = UA101_MSG_STORE;
        break;
    case PANEL_MEM_LOAD:
        m->kind = UA101_MSG_LOAD;
        break;
    default:
        return 0;
    }
    if (memory >= UA101_MEMORIES)
        return 0;
    m->memory = (uint8_t)memory;
    if (m->kind == UA101_MSG_STORE || m->kind == UA101_MSG_LOAD) {
        /* Sections: switches, patch row 0, patch rows 1-2, mixer. The windows
         * offer the two whose meaning the notes support. */
        m->sections[2] = patch_bay ? 1 : 0;
        m->sections[3] = mixer ? 1 : 0;
    }
    return 1;
}

int panel_memory_prepare(panel_engine *e, int command, unsigned memory, int patch_bay, int mixer,
                         uint64_t now, uint32_t *ticket, uint8_t bytes[18], size_t *len)
{
    ua101_msg m;

    *ticket = 0;
    *len = 0;
    if (!memory_msg(&m, command, memory, patch_bay, mixer))
        return PANEL_ERR_VALUE;
    *len = ua101_ctlmsg_build(&m, bytes);
    return (int)ua101_eng_memory_prepare(E(e), &m, now, ticket);
}

size_t panel_memory_message(int command, unsigned memory, int patch_bay, int mixer, uint8_t out[18])
{
    ua101_msg m;

    memset(out, 0, 18);
    if (!memory_msg(&m, command, memory, patch_bay, mixer))
        return 0;
    return ua101_ctlmsg_build(&m, out);
}

uint64_t panel_memory_message_bit(const uint8_t *msg, size_t len)
{
    uint8_t want[18];
    unsigned memory;

    /* Read from the bytes at their places, then held against the whole message made afresh. */
    if (msg == NULL || (len != 13 && len != 16) || msg[6] != 0x00 || msg[7] != 0x00)
        return 0;
    if (len == 13) {
        if (msg[10] != 0x00)
            return 0;
        if (msg[8] == 0x00 && msg[9] == 0x02)
            return panel_memory_message(PANEL_MEM_RESET_ALL, 0, 0, 0, want) == 13 &&
                           memcmp(msg, want, 13) == 0
                       ? PANEL_PASS_MEM_RESET_ALL : 0;
        if (msg[8] == 0x00 && msg[9] == 0x03)
            return panel_memory_message(PANEL_MEM_RESET_CURRENT, 0, 0, 0, want) == 13 &&
                           memcmp(msg, want, 13) == 0
                       ? PANEL_PASS_MEM_RESET_CURRENT : 0;
        if (msg[8] < 3 || msg[8] > 7 || msg[9] != 0x03)
            return 0;
        memory = msg[8] - 3u;
        return panel_memory_message(PANEL_MEM_RESET_MEMORY, memory, 0, 0, want) == 13 &&
                       memcmp(msg, want, 13) == 0
                   ? PANEL_PASS_MEM_RESET_0 << memory : 0;
    }
    if (msg[8] < 3 || msg[8] > 7 || (msg[9] != 0x08 && msg[9] != 0x04) || msg[10] != 0 || msg[11] != 0)
        return 0;
    memory = msg[8] - 3u;
    {
        int load = msg[9] == 0x08;
        uint8_t chosen = load ? 1 : 2;
        int patch_bay = msg[12] == chosen, mixer = msg[13] == chosen;

        if ((!patch_bay && msg[12] != 0) || (!mixer && msg[13] != 0) || (!patch_bay && !mixer))
            return 0;
        if (panel_memory_message(load ? PANEL_MEM_LOAD : PANEL_MEM_STORE, memory, patch_bay, mixer, want) != 16 ||
            memcmp(msg, want, 16) != 0)
            return 0;
        return (load ? PANEL_PASS_MEM_LOAD_0 : PANEL_PASS_MEM_STORE_0) << memory;
    }
}

void panel_set_memory_settle(panel_engine *e, uint64_t micros) { E(e)->cfg.memory_settle_us = micros; }

int panel_memory_confirm(panel_engine *e, uint32_t ticket, uint64_t now) { return (int)ua101_eng_memory_confirm(E(e), ticket, now); }
void panel_memory_cancel(panel_engine *e) { ua101_eng_memory_cancel(E(e)); }

uint64_t panel_reads_sent(const panel_engine *e) { return CE(e)->reads_sent; }
uint64_t panel_sets_sent(const panel_engine *e) { return CE(e)->sets_sent; }
uint64_t panel_memory_sent(const panel_engine *e) { return CE(e)->memory_sent; }
uint64_t panel_guard_blocks(const panel_engine *e) { return CE(e)->guard_blocks; }
uint64_t panel_disagreements(const panel_engine *e) { return CE(e)->disagreements; }

void panel_last_disagree(const panel_engine *e, int *kind, int *group, int *index, uint32_t *sent, uint32_t *unit)
{
    *kind = CE(e)->last_disagree_field.kind;
    *group = CE(e)->last_disagree_field.group;
    *index = CE(e)->last_disagree_field.index;
    *sent = CE(e)->last_disagree_sent;
    *unit = CE(e)->last_disagree_unit;
}

static uint32_t block_value(const uint8_t *block, int kind, unsigned group, unsigned index)
{
    ua101_field f;

    f.kind = (uint8_t)kind;
    f.group = (uint8_t)group;
    f.index = (uint8_t)index;
    return ua101_field_id(f) < 0 ? 0 : ua101_block_get(block, f);
}

uint32_t panel_factory_value(int kind, unsigned group, unsigned index)
{
    return block_value(ua101_sim_factory_block(), kind, group, index);
}

void panel_factory_block(uint8_t out[256]) { memcpy(out, ua101_sim_factory_block(), 256); }

uint32_t panel_block_value(const uint8_t block[256], int kind, unsigned group, unsigned index)
{
    return block_value(block, kind, group, index);
}

void panel_block_switches(const uint8_t block[256], uint32_t *rate_hz, int *rate_code,
                          int *soft_control, int *mono)
{
    ua101_settings st;

    ua101_settings_parse(block, &st);
    *rate_hz = ua101_ctl_rate_hz(st.rate_code);
    *rate_code = st.rate_code;
    *soft_control = st.soft_control;
    *mono = st.mono;
}

int panel_value_ok(int kind, unsigned group, unsigned index, uint32_t value)
{
    ua101_field f;

    if (kind < 0 || kind > 0xFF || group > 0xFF || index > 0xFF)
        return 0;
    f.kind = (uint8_t)kind;
    f.group = (uint8_t)group;
    f.index = (uint8_t)index;
    return ua101_field_id(f) >= 0 && ua101_field_value_ok(f, value);
}

unsigned panel_waiting(const panel_engine *e) { return CE(e)->fifo_count; }

int panel_factory_monitor_on(const panel_engine *e, unsigned group, unsigned channel)
{
    return (panel_factory_value(PANEL_F_ON, group, channel) != 0) != (CE(e)->cfg.on_bit_is_mute != 0);
}

panel_sim *panel_sim_new(void)
{
    ua101_sim *s = calloc(1, sizeof(*s));

    if (s)
        ua101_sim_init(s);
    return (panel_sim *)s;
}

void panel_sim_free(panel_sim *s) { free(s); }
void panel_sim_receive(panel_sim *s, const uint8_t *msg, size_t len) { ua101_sim_receive(S(s), msg, len); }

size_t panel_sim_next(panel_sim *s, uint8_t out[PANEL_MAX_MESSAGE])
{
    const uint8_t *msg;
    size_t len;

    if (!ua101_sim_next(S(s), &msg, &len) || len > PANEL_MAX_MESSAGE)
        return 0;
    memcpy(out, msg, len);
    return len;
}

void panel_sim_switch(panel_sim *s, int which, int in, unsigned messages)
{
    ua101_sim_switch_move(S(s), (ua101_sim_switch)which, in, messages);
}
void panel_sim_set_rate_code(panel_sim *s, unsigned code) { ua101_sim_set_rate_code(S(s), code); }
void panel_sim_set_silent(panel_sim *s, int silent) { S(s)->silent = silent; }
void panel_sim_ignore_kind(panel_sim *s, int kind) { S(s)->ignore_kind_plus_1 = kind < 0 ? 0 : kind + 1; }
uint32_t panel_sim_value(const panel_sim *s, int kind, unsigned group, unsigned index)
{
    return block_value(CS(s)->block, kind, group, index);
}

void panel_sim_block(const panel_sim *s, uint8_t out[256]) { memcpy(out, CS(s)->block, 256); }

void panel_sim_memory(const panel_sim *s, unsigned memory, uint8_t out[256])
{
    memset(out, 0, 256);
    if (memory < UA101_MEMORIES)
        memcpy(out, CS(s)->memory[memory], 256);
}

void panel_sim_set_memory(panel_sim *s, unsigned memory, const uint8_t block[256])
{
    if (memory < UA101_MEMORIES)
        memcpy(S(s)->memory[memory], block, 256);
}

void panel_sim_poke(panel_sim *s, unsigned index, uint8_t value)
{
    if (index < UA101_CTL_BLOCK_BYTES)
        S(s)->block[index] = value;
}

static ua101_settings sim_settings(const panel_sim *s)
{
    ua101_settings st;

    ua101_settings_parse(CS(s)->block, &st);
    return st;
}

int panel_sim_soft_control(const panel_sim *s) { return sim_settings(s).soft_control; }
int panel_sim_mono(const panel_sim *s) { return sim_settings(s).mono; }
int panel_sim_digital(const panel_sim *s) { return sim_settings(s).clock_source != 0; }
int panel_sim_rate_code(const panel_sim *s) { return sim_settings(s).rate_code; }
int panel_sim_saving_paused(const panel_sim *s) { return CS(s)->saving_paused; }
uint64_t panel_sim_reads(const panel_sim *s) { return CS(s)->reads; }
uint64_t panel_sim_sets_applied(const panel_sim *s) { return CS(s)->sets_applied; }
uint64_t panel_sim_saves(const panel_sim *s) { return CS(s)->saves; }
uint64_t panel_sim_memory_commands(const panel_sim *s) { return CS(s)->memory_commands; }
uint64_t panel_sim_unknown(const panel_sim *s) { return CS(s)->unknown_messages + CS(s)->bad_messages; }

int panel_is_read_request(const uint8_t *msg, size_t len)
{
    return ua101_ctl_is_read_request(msg, len);
}

uint64_t panel_helper_passes(void) { return UA101_CTL_PASSES; }
uint64_t panel_helper_passes_on(void) { return UA101_CTL_PASSES_ON; }
uint64_t panel_helper_passes_memory(void) { return UA101_CTL_PASSES_MEMORY; }
const char *panel_old_memory_go_ahead_path(void) { return UA101_OLD_MEMORY_GO_AHEAD_PATH; }
unsigned panel_memory_fresh_seconds(void) { return UA101_MEMORY_FRESH_SECONDS; }
unsigned panel_memory_gap_seconds(void) { return UA101_MEMORY_GAP_SECONDS; }
unsigned panel_helper_link_version(void) { return UA101_LINK_VERSION; }
const char *panel_old_go_ahead_path(void) { return UA101_OLD_GO_AHEAD_PATH; }
void panel_writes_refused(panel_engine *e, uint64_t now) { ua101_eng_writes_refused(E(e), now); }
int panel_pause_saving(panel_engine *e, uint64_t now) { return (int)ua101_eng_pause_saving(E(e), now); }
int panel_resume_saving(panel_engine *e, uint64_t now) { return (int)ua101_eng_resume_saving(E(e), now); }
int panel_saving_held(const panel_engine *e) { return ua101_eng_saving_held(CE(e)); }
void panel_unit_block(const panel_engine *e, uint8_t out[256]) { memcpy(out, CE(e)->unit, 256); }
uint64_t panel_blocks_received(const panel_engine *e) { return CE(e)->blocks_received; }
uint64_t panel_other_messages(const panel_engine *e) { return CE(e)->other_messages; }

void panel_saving_message(int resume, uint8_t out[13])
{
    ua101_msg m = {.kind = UA101_MSG_AUTOSAVE, .value = resume ? 1u : 0u};
    uint8_t bytes[UA101_CTLMSG_MAX_BYTES];
    size_t len = ua101_ctlmsg_build(&m, bytes);

    memset(out, 0, 13);
    if (len == 13)
        memcpy(out, bytes, 13);
}

int panel_is_saving_message(const uint8_t *msg, size_t len)
{
    uint8_t pause[13], resume[13];

    panel_saving_message(0, pause);
    panel_saving_message(1, resume);
    return msg != NULL && len == 13 && pause[0] == 0xF0 &&
           (memcmp(msg, pause, 13) == 0 || memcmp(msg, resume, 13) == 0);
}

void panel_patch_message(unsigned output, uint32_t source, uint8_t out[13])
{
    ua101_msg m = {.kind = UA101_MSG_FIELD, .value = source};
    uint8_t bytes[UA101_CTLMSG_MAX_BYTES];
    size_t len;

    memset(out, 0, 13);
    if (output >= UA101_CTL_PATCH_OUTPUTS)
        return;
    m.field.kind = UA101_F_PATCH;
    m.field.index = (uint8_t)output;
    len = ua101_ctlmsg_build(&m, bytes);
    if (len == 13)
        memcpy(out, bytes, 13);
}

int panel_patch_message_output(const uint8_t *msg, size_t len)
{
    uint8_t want[13];
    unsigned output;
    uint32_t source;

    if (msg == NULL || len != 13)
        return -1;
    for (output = 0; output < UA101_CTL_PATCH_OUTPUTS; output++)
        for (source = UA101_PATCH_SOURCE_MIN; source <= UA101_PATCH_SOURCE_MAX; source++) {
            panel_patch_message(output, source, want);
            if (want[0] == 0xF0 && memcmp(msg, want, 13) == 0)
                return (int)output;
        }
    return -1;
}

/* At most 0 dB. The first digit sent is the highest (read back from the unit, 2026-11-12), so
 * the number itself is what the unit takes. */
int panel_volume_sendable(uint32_t v) { return v <= PANEL_UNITY_VOLUME; }

void panel_volume_message(unsigned group, unsigned channel, uint32_t v, uint8_t out[18])
{
    ua101_msg m = {.kind = UA101_MSG_FIELD, .value = v};
    uint8_t bytes[UA101_CTLMSG_MAX_BYTES];
    size_t len;

    memset(out, 0, 18);
    if (group >= UA101_CTL_GROUPS || channel >= ua101_ctl_group_channels(group))
        return;
    m.field.kind = UA101_F_VOLUME;
    m.field.group = (uint8_t)group;
    m.field.index = (uint8_t)channel;
    len = ua101_ctlmsg_build(&m, bytes);
    if (len == 18)
        memcpy(out, bytes, 18);
}

int panel_volume_message_channel(const uint8_t *msg, size_t len)
{
    uint8_t want[18];
    unsigned group, channel, i;
    uint32_t v = 0;

    if (msg == NULL || len != 18 || msg[7] < 3 || msg[7] > 5)
        return -1;
    group = msg[7] - 3u;
    channel = msg[8];
    for (i = 0; i < 6; i++) {
        if (msg[10 + i] > 0x0F)
            return -1;
        v = (v << 4) | msg[10 + i];
    }
    if (!panel_volume_sendable(v))
        return -1;
    panel_volume_message(group, channel, v, want);
    if (want[0] != 0xF0 || memcmp(msg, want, 18) != 0)
        return -1;
    return (int)(group == 0 ? channel : 2u + (group - 1u) * 10u + channel);
}

/* Within 0x0000 to 0x8000. The first digit sent is the highest (read back from the unit,
 * 2026-11-12), so the number itself is what the unit takes. */
int panel_pan_sendable(uint32_t v) { return v <= PANEL_MAX_PAN; }

void panel_pan_message(unsigned group, unsigned channel, uint32_t v, uint8_t out[16])
{
    ua101_msg m = {.kind = UA101_MSG_FIELD, .value = v};
    uint8_t bytes[UA101_CTLMSG_MAX_BYTES];
    size_t len;

    memset(out, 0, 16);
    if (group < 1 || group >= UA101_CTL_GROUPS || channel >= ua101_ctl_group_channels(group))
        return;
    m.field.kind = UA101_F_PAN;
    m.field.group = (uint8_t)group;
    m.field.index = (uint8_t)channel;
    len = ua101_ctlmsg_build(&m, bytes);
    if (len == 16)
        memcpy(out, bytes, 16);
}

int panel_pan_message_channel(const uint8_t *msg, size_t len)
{
    uint8_t want[16];
    unsigned group, channel, i;
    uint32_t v = 0;

    if (msg == NULL || len != 16 || msg[7] < 4 || msg[7] > 5)
        return -1;
    group = msg[7] - 3u;
    channel = msg[8];
    for (i = 0; i < 4; i++) {
        if (msg[10 + i] > 0x0F)
            return -1;
        v = (v << 4) | msg[10 + i];
    }
    if (!panel_pan_sendable(v))
        return -1;
    panel_pan_message(group, channel, v, want);
    if (want[0] != 0xF0 || memcmp(msg, want, 16) != 0)
        return -1;
    return (int)(2u + (group - 1u) * 10u + channel);
}

void panel_on_message(unsigned group, unsigned channel, unsigned bit, uint8_t out[13])
{
    ua101_msg m = {.kind = UA101_MSG_FIELD, .value = bit};
    uint8_t bytes[UA101_CTLMSG_MAX_BYTES];
    size_t len;

    memset(out, 0, 13);
    if (group >= UA101_CTL_GROUPS || channel >= ua101_ctl_group_channels(group) || bit > 1)
        return;
    m.field.kind = UA101_F_ON;
    m.field.group = (uint8_t)group;
    m.field.index = (uint8_t)channel;
    len = ua101_ctlmsg_build(&m, bytes);
    if (len == 13)
        memcpy(out, bytes, 13);
}

int panel_on_message_channel(const uint8_t *msg, size_t len)
{
    uint8_t want[13];
    unsigned group, channel;

    if (msg == NULL || len != 13 || msg[7] < 3 || msg[7] > 5 || msg[9] != 0x02 || msg[10] > 1)
        return -1;
    group = msg[7] - 3u;
    channel = msg[8];
    panel_on_message(group, channel, msg[10], want);
    if (want[0] != 0xF0 || memcmp(msg, want, 13) != 0)
        return -1;
    return (int)(group == 0 ? channel : 2u + (group - 1u) * 10u + channel);
}

void panel_link_message(unsigned group, unsigned pair, unsigned bit, uint8_t out[13])
{
    ua101_msg m = {.kind = UA101_MSG_FIELD, .value = bit};
    uint8_t bytes[UA101_CTLMSG_MAX_BYTES];
    size_t len;

    memset(out, 0, 13);
    if (group >= UA101_CTL_GROUPS || pair >= ua101_ctl_group_channels(group) / 2u || bit > 1)
        return;
    m.field.kind = UA101_F_LINK;
    m.field.group = (uint8_t)group;
    m.field.index = (uint8_t)pair;
    len = ua101_ctlmsg_build(&m, bytes);
    if (len == 13)
        memcpy(out, bytes, 13);
}

int panel_link_message_pair(const uint8_t *msg, size_t len)
{
    uint8_t want[13];
    unsigned group, pair;

    if (msg == NULL || len != 13 || msg[7] < 3 || msg[7] > 5 || msg[9] != 0x01 || msg[10] > 1 ||
        (msg[8] & 1u))
        return -1;
    group = msg[7] - 3u;
    pair = msg[8] / 2u;
    panel_link_message(group, pair, msg[10], want);
    if (want[0] != 0xF0 || memcmp(msg, want, 13) != 0)
        return -1;
    return (int)(group == 0 ? pair : 1u + (group - 1u) * 5u + pair);
}

void panel_solo_message(unsigned group, unsigned value, uint8_t out[13])
{
    ua101_msg m = {.kind = UA101_MSG_FIELD, .value = value};
    uint8_t bytes[UA101_CTLMSG_MAX_BYTES];
    size_t len;

    memset(out, 0, 13);
    if (group < 1 || group > 2 || value > PANEL_MAX_SOLO)
        return;
    m.field.kind = UA101_F_SOLO;
    m.field.group = (uint8_t)group;
    len = ua101_ctlmsg_build(&m, bytes);
    if (len == 13)
        memcpy(out, bytes, 13);
}

int panel_solo_message_group(const uint8_t *msg, size_t len)
{
    uint8_t want[13];
    unsigned group;

    if (msg == NULL || len != 13 || msg[7] < 4 || msg[7] > 5 || msg[8] != 0x00 || msg[9] != 0x00 ||
        msg[10] > PANEL_MAX_SOLO)
        return -1;
    group = msg[7] - 3u;
    panel_solo_message(group, msg[10], want);
    if (want[0] != 0xF0 || memcmp(msg, want, 13) != 0)
        return -1;
    return (int)group;
}

void panel_set_min_gap(panel_engine *e, uint64_t micros) { E(e)->cfg.min_gap_us = micros; }

unsigned panel_unchecked(const panel_engine *e)
{
    return CE(e)->fifo_count + CE(e)->unverified_count;
}
