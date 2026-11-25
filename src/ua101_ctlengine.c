/* See ua101_ctlengine.h. */
#include "ua101_ctlengine.h"

#include <string.h>

#define NEVER UINT64_MAX
#define RATE_CODE_192K 4
#define CHANNELS_AT_192K 6
#define SEND_RETRY_US 100000

void ua101_eng_default_config(ua101_eng_config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->read_timeout_us = 1000000;
    cfg->read_tries = 3;
    cfg->silent_retry_us = 3000000;
    cfg->settle_us = 500000; /* the notes: bursts of up to 7 messages in 377 ms */
    cfg->verify_quiet_us = 300000;
    cfg->min_gap_us = 5000; /* a guess: at most 200 messages a second */
    cfg->memory_settle_us = 500000;
    cfg->ticket_life_us = 30000000;
    cfg->lost_reply_us = 10000000;
    cfg->volume_ceiling = UA101_CTL_UNITY_VOLUME;
    cfg->on_bit_is_mute = 1;
}

void ua101_eng_init(ua101_engine *e, const ua101_eng_io *io, const ua101_eng_config *cfg)
{
    memset(e, 0, sizeof(*e));
    e->io = *io;
    if (cfg)
        e->cfg = *cfg;
    else
        ua101_eng_default_config(&e->cfg);
    if (e->cfg.volume_ceiling > UA101_MAX_VOLUME)
        e->cfg.volume_ceiling = UA101_MAX_VOLUME;
}

static void fire(ua101_engine *e)
{
    unsigned events = e->events;

    e->events = 0;
    if (events && e->io.event)
        e->io.event(e->io.ctx, events);
}

static void set_state(ua101_engine *e, ua101_eng_state state)
{
    if (e->state != state) {
        e->state = state;
        e->events |= UA101_EV_STATE;
    }
}

/* The guard: the only place a message leaves. It trusts no caller. */
static int emit(ua101_engine *e, const uint8_t *msg, size_t len, uint64_t now)
{
    ua101_risk risk = ua101_ctlmsg_risk(msg, len);
    int allowed = risk == UA101_RISK_READ || (risk == UA101_RISK_SETTING && e->allow_writes) ||
                  (risk == UA101_RISK_MEMORY && e->allow_writes && e->allow_memory &&
                   e->memory_pass);

    /* After a refusal, wait before the next try, so a caller that sleeps
     * until the next deadline doesn't spin. */
    if (len == 0 || !allowed || e->state == UA101_ENG_OFFLINE) {
        e->guard_blocks++;
        e->next_send_at = now + SEND_RETRY_US;
        return 0;
    }
    if (!e->io.send || !e->io.send(e->io.ctx, msg, len)) {
        e->send_failures++;
        e->next_send_at = now + SEND_RETRY_US;
        return 0;
    }
    e->seq++;
    e->next_send_at = now + e->cfg.min_gap_us;
    if (risk == UA101_RISK_READ)
        e->reads_sent++;
    else if (risk == UA101_RISK_SETTING)
        e->sets_sent++;
    else
        e->memory_sent++;
    return 1;
}

#ifdef UA101_CTLENGINE_TESTING
/* For the tests only: hands raw bytes straight to the guard, past every other check. */
int ua101_eng_test_emit(ua101_engine *e, const uint8_t *msg, size_t len, uint64_t now)
{
    return emit(e, msg, len, now);
}
#endif

static int emit_msg(ua101_engine *e, const ua101_msg *m, uint64_t now)
{
    uint8_t bytes[UA101_CTLMSG_MAX_BYTES];

    return emit(e, bytes, ua101_ctlmsg_build(m, bytes), now);
}

static void clear_pending(ua101_engine *e)
{
    memset(e->queued, 0, sizeof(e->queued));
    memset(e->unverified, 0, sizeof(e->unverified));
    e->fifo_head = e->fifo_count = 0;
    e->unverified_count = 0;
}

static void send_read(ua101_engine *e, uint64_t now)
{
    ua101_msg m = {.kind = UA101_MSG_READ_BLOCK};

    e->read_wanted = 0;
    /* If the way to the unit refuses it, the timeout below tries again. */
    if (emit_msg(e, &m, now)) {
        /* When the list is full the newest entry is replaced: a block is then
         * taken to be older than it is, never newer, which is the safe side. */
        if (e->asked == UA101_ENG_READS_OUT)
            e->asked--;
        e->asked_seq[e->asked] = e->seq;
        e->asked_at[e->asked++] = now;
    }
    e->read_waiting = 1;
    e->read_deadline = now + e->cfg.read_timeout_us;
}

static void want_read(ua101_engine *e, uint64_t at)
{
    if (!e->read_wanted || at < e->read_at)
        e->read_at = at;
    e->read_wanted = 1;
}

void ua101_eng_transport(ua101_engine *e, int up, uint64_t now)
{
    /* saving_paused is kept: if the unit was told to pause its saving and is
     * still the same unit, it is owed a resume once it answers again. */
    clear_pending(e);
    e->drags = 0;
    if (e->saving_hold)
        e->saving_hold = 2; /* let go: resumed, and read, once the unit answers again */
    e->settling = 0;
    e->read_waiting = e->read_wanted = 0;
    e->read_tries_done = 0;
    e->asked = 0;
    e->ticket = 0;
    e->next_send_at = now;
    if (up) {
        set_state(e, UA101_ENG_SYNCING);
        send_read(e, now);
    } else {
        set_state(e, UA101_ENG_OFFLINE);
    }
    fire(e);
}

/* Which of our reads a block from the unit answers: the oldest one waiting.
 * Returns 0, older than every change, for a block nobody asked for. */
static uint64_t answered_read(ua101_engine *e, uint64_t now)
{
    uint64_t seq;

    /* A reply that never came would make every later block look one read
     * older than it is. Give up on reads that have waited far too long. */
    while (e->asked > 1 && now >= e->asked_at[0] + e->cfg.lost_reply_us) {
        e->asked--;
        memmove(e->asked_seq, e->asked_seq + 1, e->asked * sizeof(e->asked_seq[0]));
        memmove(e->asked_at, e->asked_at + 1, e->asked * sizeof(e->asked_at[0]));
    }
    if (!e->asked)
        return 0;
    seq = e->asked_seq[0];
    e->asked--;
    memmove(e->asked_seq, e->asked_seq + 1, e->asked * sizeof(e->asked_seq[0]));
    memmove(e->asked_at, e->asked_at + 1, e->asked * sizeof(e->asked_at[0]));
    return seq;
}

static void take_block(ua101_engine *e, const uint8_t *block, uint64_t now)
{
    uint8_t next[UA101_CTL_BLOCK_BYTES];
    unsigned id;

    e->read_seq = answered_read(e, now);

    /* Everything that isn't one of our fields comes straight from the unit. */
    memcpy(next, block, sizeof(next));
    for (id = 0; id < UA101_FIELD_COUNT; id++) {
        ua101_field f = ua101_field_from_id(id);
        uint32_t ours = ua101_block_get(e->want, f);
        uint32_t theirs = ua101_block_get(block, f);

        /* A change still waiting, or sent after this block was asked for, is
         * newer than the block: keep ours and check it next time. */
        if (e->queued[id] || (e->unverified[id] && e->sent_seq[id] > e->read_seq)) {
            ua101_block_put(next, f, ours);
            continue;
        }
        if (e->unverified[id]) {
            e->unverified[id] = 0;
            e->unverified_count--;
            if (ours != theirs) {
                e->disagreements++;
                e->last_disagree_field = f;
                e->last_disagree_sent = ours;
                e->last_disagree_unit = theirs;
                e->events |= UA101_EV_DISAGREE;
            }
        }
    }
    if (e->state != UA101_ENG_READY || memcmp(next, e->want, sizeof(next)) != 0)
        e->events |= UA101_EV_SETTINGS;
    e->blocks_received++;
    memcpy(e->unit, block, sizeof(e->unit));
    memcpy(e->want, next, sizeof(e->want));
    ua101_settings_parse(e->want, &e->shown);
    e->read_waiting = 0;
    e->read_tries_done = 0;
    set_state(e, UA101_ENG_READY);
}

void ua101_eng_receive(ua101_engine *e, const uint8_t *msg, size_t len, uint64_t now)
{
    ua101_ctl_view view;

    if (e->state == UA101_ENG_OFFLINE)
        return;
    switch (ua101_ctl_classify(msg, len, &view)) {
    case UA101_CTL_SETTINGS:
        take_block(e, view.data, now);
        break;
    case UA101_CTL_CHANGE:
        /* Never act on one of these: wait for the burst to end, then read. */
        e->settling = 1;
        e->last_change_at = now;
        e->events |= UA101_EV_SETTLING;
        break;
    case UA101_CTL_OTHER:
        e->other_messages++;
        break;
    default:
        e->bad_messages++;
        /* A damaged block still used up one of our reads. */
        if (len == UA101_CTL_BLOCK_MESSAGE_BYTES)
            (void)answered_read(e, now);
        break;
    }
    ua101_eng_tick(e, now);
}

static int verify_due(const ua101_engine *e)
{
    return e->state == UA101_ENG_READY && e->unverified_count && !e->fifo_count &&
           !e->saving_paused && !e->read_waiting;
}

void ua101_eng_tick(ua101_engine *e, uint64_t now)
{
    if (e->state == UA101_ENG_OFFLINE) {
        fire(e);
        return;
    }

    if (e->read_waiting && now >= e->read_deadline) {
        e->read_waiting = 0;
        if (++e->read_tries_done < e->cfg.read_tries) {
            send_read(e, now);
        } else {
            /* Changes can't be checked or sent to a unit that doesn't answer. */
            e->read_tries_done = 0;
            clear_pending(e);
            e->drags = 0;
            if (e->saving_hold)
                e->saving_hold = 2;
            set_state(e, UA101_ENG_SILENT);
            want_read(e, now + e->cfg.silent_retry_us);
        }
    }

    if (e->settling && now >= e->last_change_at + e->cfg.settle_us) {
        e->settling = 0;
        want_read(e, now);
    }

    while (e->state == UA101_ENG_READY && e->fifo_count && now >= e->next_send_at) {
        ua101_msg m = {.kind = UA101_MSG_FIELD};
        unsigned id;

        if (e->drags && !e->saving_paused) {
            ua101_msg pause = {.kind = UA101_MSG_AUTOSAVE, .value = 0};

            if (!emit_msg(e, &pause, now))
                break;
            e->saving_paused = 1;
            continue;
        }
        id = e->fifo[e->fifo_head];
        m.field = ua101_field_from_id(id);
        m.value = ua101_block_get(e->want, m.field);
        if (!emit_msg(e, &m, now))
            break;
        e->fifo_head = (e->fifo_head + 1) % UA101_FIELD_COUNT;
        e->fifo_count--;
        e->queued[id] = 0;
        if (!e->unverified[id]) {
            e->unverified[id] = 1;
            e->unverified_count++;
        }
        e->sent_seq[id] = e->seq;
        e->last_write_at = now;
    }

    if (e->state == UA101_ENG_READY && e->saving_paused && !e->drags && !e->fifo_count &&
        e->saving_hold != 1 && now >= e->next_send_at) {
        ua101_msg resume = {.kind = UA101_MSG_AUTOSAVE, .value = 1};

        if (emit_msg(e, &resume, now)) {
            e->saving_paused = 0;
            e->last_write_at = now;
            if (e->saving_hold) {
                e->saving_hold = 0;
                want_read(e, now + e->cfg.verify_quiet_us);
            }
        }
    }

    if (verify_due(e) && now >= e->last_write_at + e->cfg.verify_quiet_us)
        want_read(e, now);

    if (e->read_wanted && !e->read_waiting && now >= e->read_at && now >= e->next_send_at)
        send_read(e, now);

    fire(e);
}

static uint64_t later(uint64_t a, uint64_t b)
{
    return a > b ? a : b;
}

uint64_t ua101_eng_next_deadline(const ua101_engine *e)
{
    uint64_t t = NEVER, c;

    if (e->state == UA101_ENG_OFFLINE)
        return NEVER;
    if (e->read_waiting && e->read_deadline < t)
        t = e->read_deadline;
    if (e->settling && (c = e->last_change_at + e->cfg.settle_us) < t)
        t = c;
    if (e->state == UA101_ENG_READY &&
        (e->fifo_count || (e->saving_paused && !e->drags && e->saving_hold != 1)) &&
        e->next_send_at < t)
        t = e->next_send_at;
    if (verify_due(e) &&
        (c = later(e->last_write_at + e->cfg.verify_quiet_us, e->next_send_at)) < t)
        t = c;
    if (e->read_wanted && !e->read_waiting && (c = later(e->read_at, e->next_send_at)) < t)
        t = c;
    return t;
}

void ua101_eng_allow_writes(ua101_engine *e, int allow)
{
    e->allow_writes = allow != 0;
}

void ua101_eng_writes_refused(ua101_engine *e, uint64_t now)
{
    e->allow_writes = 0;
    e->allow_memory = 0;
    e->ticket = 0;
    clear_pending(e);
    e->drags = 0;
    e->saving_paused = 0;
    e->saving_hold = 0;
    if (e->state != UA101_ENG_OFFLINE) {
        want_read(e, now);
        ua101_eng_tick(e, now);
    }
}

void ua101_eng_allow_memory(ua101_engine *e, int allow)
{
    e->allow_memory = allow != 0;
    if (!allow)
        e->ticket = 0;
}

void ua101_eng_set_volume_ceiling(ua101_engine *e, uint32_t ceiling)
{
    e->cfg.volume_ceiling = ceiling > UA101_MAX_VOLUME ? UA101_MAX_VOLUME : ceiling;
}

const ua101_settings *ua101_eng_settings(const ua101_engine *e)
{
    return &e->shown;
}

int ua101_eng_monitor_on(const ua101_engine *e, unsigned group, unsigned channel)
{
    if (group >= UA101_CTL_GROUPS || channel >= ua101_ctl_group_channels(group))
        return 0;
    return (e->shown.channel[group][channel].on != 0) != (e->cfg.on_bit_is_mute != 0);
}

int ua101_eng_busy(const ua101_engine *e)
{
    return e->fifo_count || e->unverified_count || e->saving_paused;
}

void ua101_eng_refresh(ua101_engine *e, uint64_t now)
{
    if (e->state == UA101_ENG_OFFLINE)
        return;
    want_read(e, now);
    ua101_eng_tick(e, now);
}

/* Every ordinary change comes through here. */
static ua101_eng_result set_field(ua101_engine *e, ua101_field f, uint32_t value)
{
    ua101_eng_result result = UA101_ENG_OK;
    int id = ua101_field_id(f);

    if (e->state != UA101_ENG_READY)
        return UA101_ENG_ERR_NOT_READY;
    if (!e->allow_writes)
        return UA101_ENG_ERR_READ_ONLY;
    if (id < 0)
        return UA101_ENG_ERR_NO_CHANNEL;
    if (ua101_field_is_mixer(f)) {
        if (!e->shown.soft_control)
            return UA101_ENG_ERR_SOFT_CTRL;
        /* At 192 kHz Roland's app ignores channels 7 to 10. */
        if (e->shown.rate_code == RATE_CODE_192K && f.group != UA101_GROUP_MONITOR &&
            f.kind != UA101_F_SOLO &&
            (f.kind == UA101_F_LINK ? f.index * 2u : f.index) >= CHANNELS_AT_192K)
            return UA101_ENG_ERR_NO_CHANNEL;
    }
    if (f.kind == UA101_F_VOLUME && value <= UA101_MAX_VOLUME && value > e->cfg.volume_ceiling) {
        value = e->cfg.volume_ceiling;
        result = UA101_ENG_CLAMPED;
    }
    if (!ua101_field_value_ok(f, value))
        return UA101_ENG_ERR_VALUE;

    if (ua101_block_get(e->want, f) == value)
        return result; /* already so, or already on its way */
    ua101_block_put(e->want, f, value);
    ua101_settings_parse(e->want, &e->shown);
    e->events |= UA101_EV_SETTINGS;
    if (!e->queued[id]) {
        e->queued[id] = 1;
        e->fifo[(e->fifo_head + e->fifo_count) % UA101_FIELD_COUNT] = (uint8_t)id;
        e->fifo_count++;
    }
    return result;
}

static ua101_eng_result worse(ua101_eng_result a, ua101_eng_result b)
{
    return a > b ? a : b;
}

static int linked(const ua101_engine *e, unsigned group, unsigned channel)
{
    return group < UA101_CTL_GROUPS && channel < ua101_ctl_group_channels(group) &&
           e->shown.link[group][channel / 2];
}

/* Sets one channel, and its partner when the pair is linked. */
static ua101_eng_result set_channel(ua101_engine *e, uint8_t kind, unsigned group,
                                    unsigned channel, uint32_t value, uint64_t now)
{
    ua101_field f = {kind, (uint8_t)group, (uint8_t)channel};
    ua101_eng_result r;

    if (group > 0xFF || channel > 0xFF)
        return UA101_ENG_ERR_NO_CHANNEL;
    r = set_field(e, f, value);
    if (r <= UA101_ENG_CLAMPED && kind != UA101_F_PAN && linked(e, group, channel)) {
        f.index = (uint8_t)(channel ^ 1u);
        r = worse(r, set_field(e, f, value));
    }
    ua101_eng_tick(e, now);
    return r;
}

ua101_eng_result ua101_eng_set_volume(ua101_engine *e, unsigned group, unsigned channel,
                                      uint32_t volume, uint64_t now)
{
    return set_channel(e, UA101_F_VOLUME, group, channel, volume, now);
}

ua101_eng_result ua101_eng_set_pan(ua101_engine *e, unsigned group, unsigned channel,
                                   uint32_t pan, uint64_t now)
{
    return set_channel(e, UA101_F_PAN, group, channel, pan, now);
}

ua101_eng_result ua101_eng_set_monitor(ua101_engine *e, unsigned group, unsigned channel,
                                       int on, uint64_t now)
{
    uint32_t bit = (on != 0) != (e->cfg.on_bit_is_mute != 0);

    return set_channel(e, UA101_F_ON, group, channel, bit, now);
}

static ua101_eng_result set_one(ua101_engine *e, uint8_t kind, unsigned group, unsigned index,
                                uint32_t value, uint64_t now)
{
    ua101_field f = {kind, (uint8_t)group, (uint8_t)index};
    ua101_eng_result r =
        group > 0xFF || index > 0xFF ? UA101_ENG_ERR_NO_CHANNEL : set_field(e, f, value);

    ua101_eng_tick(e, now);
    return r;
}

ua101_eng_result ua101_eng_set_link(ua101_engine *e, unsigned group, unsigned pair, int on,
                                    uint64_t now)
{
    return set_one(e, UA101_F_LINK, group, pair, on != 0, now);
}

ua101_eng_result ua101_eng_set_solo(ua101_engine *e, unsigned group, int channel, uint64_t now)
{
    uint32_t value = 0; /* no solo */

    /* MONITOR OUT has no solo: there is no such setting, and set_field says so. */
    if (channel != UA101_ENG_SOLO_NONE) {
        /* At 192 kHz Roland's app ignores channels 7 to 10. */
        if (channel < 0 || channel >= UA101_CTL_MAX_CHANNELS ||
            (e->shown.rate_code == RATE_CODE_192K && channel >= CHANNELS_AT_192K))
            return UA101_ENG_ERR_NO_CHANNEL;
        value = (uint32_t)channel + 1u;
    }
    return set_one(e, UA101_F_SOLO, group, 0, value, now);
}

int ua101_eng_soloed(const ua101_engine *e, unsigned group, unsigned channel)
{
    unsigned solo;

    if ((group != UA101_GROUP_INPUT && group != UA101_GROUP_OUTPUT) ||
        channel >= UA101_CTL_MAX_CHANNELS)
        return 0;
    solo = e->shown.solo[group - 1];
    return solo == channel + 1u || (linked(e, group, channel) && solo == (channel ^ 1u) + 1u);
}

ua101_eng_result ua101_eng_toggle_solo(ua101_engine *e, unsigned group, unsigned channel,
                                       uint64_t now)
{
    if (channel >= UA101_CTL_MAX_CHANNELS)
        return UA101_ENG_ERR_NO_CHANNEL;
    return ua101_eng_set_solo(e, group,
                              ua101_eng_soloed(e, group, channel) ? UA101_ENG_SOLO_NONE
                                                                  : (int)channel,
                              now);
}

ua101_eng_result ua101_eng_set_patch(ua101_engine *e, unsigned output, uint32_t source,
                                     uint64_t now)
{
    return set_one(e, UA101_F_PATCH, 0, output, source, now);
}

ua101_eng_result ua101_eng_patch_preset(ua101_engine *e, int hard_thru, uint64_t now)
{
    uint32_t first = hard_thru ? UA101_PATCH_HARD_THRU_FIRST : UA101_PATCH_INITIAL_FIRST;
    ua101_eng_result r = UA101_ENG_OK;
    unsigned output;

    for (output = 0; output < UA101_CTL_PATCH_OUTPUTS && r == UA101_ENG_OK; output++) {
        ua101_field f = {UA101_F_PATCH, 0, (uint8_t)output};

        r = set_field(e, f, first + output);
    }
    ua101_eng_tick(e, now);
    return r;
}

void ua101_eng_drag_begin(ua101_engine *e, uint64_t now)
{
    if (e->state == UA101_ENG_READY && e->allow_writes)
        e->drags++;
    ua101_eng_tick(e, now);
}

void ua101_eng_drag_end(ua101_engine *e, uint64_t now)
{
    if (e->drags)
        e->drags--;
    ua101_eng_tick(e, now);
}

ua101_eng_result ua101_eng_memory_prepare(ua101_engine *e, const ua101_msg *m, uint64_t now,
                                          uint32_t *ticket)
{
    uint8_t bytes[UA101_CTLMSG_MAX_BYTES];
    size_t len = ua101_ctlmsg_build(m, bytes);

    *ticket = 0;
    e->ticket = 0;
    if (e->state != UA101_ENG_READY)
        return UA101_ENG_ERR_NOT_READY;
    if (!e->allow_writes || !e->allow_memory)
        return UA101_ENG_ERR_READ_ONLY;
    if (len == 0 || ua101_ctlmsg_risk(bytes, len) != UA101_RISK_MEMORY)
        return UA101_ENG_ERR_VALUE;
    e->ticket_msg = *m;
    if (++e->ticket_counter == 0)
        e->ticket_counter = 1;
    e->ticket = e->ticket_counter;
    e->ticket_expires = now + e->cfg.ticket_life_us;
    *ticket = e->ticket;
    return UA101_ENG_OK;
}

ua101_eng_result ua101_eng_memory_confirm(ua101_engine *e, uint32_t ticket, uint64_t now)
{
    int sent;

    if (ticket == 0 || ticket != e->ticket || now >= e->ticket_expires) {
        e->ticket = 0;
        return UA101_ENG_ERR_TICKET;
    }
    if (e->state != UA101_ENG_READY)
        return UA101_ENG_ERR_NOT_READY;
    if (!e->allow_writes || !e->allow_memory)
        return UA101_ENG_ERR_READ_ONLY;
    /* Changes on their way would land on the wrong side of a load or store. */
    if (ua101_eng_busy(e) || e->drags || e->read_waiting || now < e->next_send_at)
        return UA101_ENG_ERR_BUSY;
    e->ticket = 0; /* one use */
    e->memory_pass = 1;
    sent = emit_msg(e, &e->ticket_msg, now);
    e->memory_pass = 0;
    if (!sent)
        return UA101_ENG_ERR_NOT_READY;
    /* The unit's settings may all be different now. */
    want_read(e, now + e->cfg.memory_settle_us);
    ua101_eng_tick(e, now);
    return UA101_ENG_OK;
}

void ua101_eng_memory_cancel(ua101_engine *e)
{
    e->ticket = 0;
}

ua101_eng_result ua101_eng_pause_saving(ua101_engine *e, uint64_t now)
{
    ua101_msg pause = {.kind = UA101_MSG_AUTOSAVE, .value = 0};

    if (e->state != UA101_ENG_READY)
        return UA101_ENG_ERR_NOT_READY;
    if (!e->allow_writes)
        return UA101_ENG_ERR_READ_ONLY;
    /* By itself means by itself: no change waiting or unchecked, no drag, no read out. */
    if (ua101_eng_busy(e) || e->drags || e->read_waiting || now < e->next_send_at)
        return UA101_ENG_ERR_BUSY;
    if (!emit_msg(e, &pause, now))
        return UA101_ENG_ERR_NOT_READY;
    e->saving_paused = 1;
    e->saving_hold = 1;
    e->last_write_at = now;
    want_read(e, now + e->cfg.verify_quiet_us);
    ua101_eng_tick(e, now);
    return UA101_ENG_OK;
}

ua101_eng_result ua101_eng_resume_saving(ua101_engine *e, uint64_t now)
{
    if (e->saving_hold != 1)
        return UA101_ENG_ERR_VALUE; /* nothing is held */
    e->saving_hold = 2;
    ua101_eng_tick(e, now);
    return UA101_ENG_OK;
}

int ua101_eng_saving_held(const ua101_engine *e)
{
    return e->saving_hold == 1;
}

void ua101_eng_finish(ua101_engine *e, uint64_t now)
{
    uint64_t gap = e->cfg.min_gap_us;

    e->drags = 0;
    if (e->saving_hold)
        e->saving_hold = 2;
    e->cfg.min_gap_us = 0;
    e->next_send_at = now;
    ua101_eng_tick(e, now);
    e->cfg.min_gap_us = gap;
}
