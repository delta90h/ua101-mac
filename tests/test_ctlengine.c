/*
 * The control app's engine against the pretend unit. No hardware, no Apple APIs.
 *
 * Every message travels as it would on USB: packed into USB-MIDI packets on
 * cable 1, with ordinary MIDI on cable 0 mixed in, and put together again at
 * the other end. Time is a number the test moves forward, and the engine is
 * only ever woken at the moment it asked for.
 *
 * This shows the engine keeps its own rules and speaks the protocol as
 * docs/control-protocol.md describes it. It can't show the real unit agrees:
 * no write has ever been sent to one. See src/ua101_ctlsim.h.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/ua101_ctlengine.h"
#include "../src/ua101_ctlsim.h"

static int g_failures;
static int g_tests;

#define EXPECT(cond)                                                     \
    do {                                                                 \
        g_tests++;                                                       \
        if (!(cond)) {                                                   \
            g_failures++;                                                \
            printf("FAILED line %d: %s\n", __LINE__, #cond);             \
        }                                                                \
    } while (0)

#define MAX_SENT 20000
#define IN UA101_GROUP_INPUT
#define OUT UA101_GROUP_OUTPUT
#define MON UA101_GROUP_MONITOR
#define UNITY UA101_CTL_UNITY_VOLUME

typedef struct {
    ua101_sim sim;
    ua101_engine eng;
    uint64_t now;
    ua101_sysex_rx to_unit, to_host;
    ua101_msg sent[MAX_SENT];
    unsigned sent_count;
    unsigned risk[4];
    unsigned events, ticks;
    int wire_refuses; /* the way to the unit takes nothing */
    int hold_replies; /* the unit's messages wait on the wire */
} rig;

/* Carries a message over pretend USB and hands each rebuilt one to `deliver`. */
static void over_usb(ua101_sysex_rx *rx, const uint8_t *msg, size_t len,
                     void (*deliver)(rig *, const uint8_t *, size_t), rig *r)
{
    static const uint8_t k_note_on_cable0[4] = {0x09, 0x90, 0x3C, 0x40};
    uint8_t packets[400];
    size_t n = ua101_ctl_pack_sysex(msg, len, UA101_CTL_CABLE, packets, sizeof(packets)), i;
    const uint8_t *whole;
    size_t whole_len;

    EXPECT(n > 0);
    for (i = 0; i < n; i += 4) {
        if (i == 8)
            EXPECT(!ua101_sysex_rx_feed(rx, k_note_on_cable0, &whole, &whole_len));
        if (ua101_sysex_rx_feed(rx, packets + i, &whole, &whole_len))
            deliver(r, whole, whole_len);
    }
}

static void unit_gets(rig *r, const uint8_t *msg, size_t len)
{
    ua101_sim_receive(&r->sim, msg, len);
}

static void host_gets(rig *r, const uint8_t *msg, size_t len)
{
    ua101_eng_receive(&r->eng, msg, len, r->now);
}

static int rig_send(void *ctx, const uint8_t *msg, size_t len)
{
    rig *r = ctx;

    if (r->wire_refuses)
        return 0;
    r->risk[ua101_ctlmsg_risk(msg, len)]++;
    if (r->sent_count < MAX_SENT)
        ua101_ctlmsg_parse(msg, len, &r->sent[r->sent_count++]);
    over_usb(&r->to_unit, msg, len, unit_gets, r);
    return 1;
}

static void rig_event(void *ctx, unsigned events)
{
    ((rig *)ctx)->events |= events;
}

static void rig_init(rig *r, const ua101_eng_config *cfg)
{
    ua101_eng_io io = {rig_send, rig_event, r};

    memset(r, 0, sizeof(*r));
    r->now = 1000000;
    ua101_sim_init(&r->sim);
    ua101_sysex_rx_init(&r->to_unit);
    ua101_sysex_rx_init(&r->to_host);
    ua101_eng_init(&r->eng, &io, cfg);
}

static void deliver(rig *r)
{
    const uint8_t *msg;
    size_t len;

    while (!r->hold_replies && ua101_sim_next(&r->sim, &msg, &len))
        over_usb(&r->to_host, msg, len, host_gets, r);
}

/* Lets time pass, waking the engine only when it asked to be woken. */
static void advance(rig *r, uint64_t us)
{
    uint64_t until = r->now + us;
    unsigned guard = 0;

    for (;;) {
        uint64_t next;

        deliver(r);
        next = ua101_eng_next_deadline(&r->eng);
        if (next > until || ++guard > 100000)
            break;
        if (next > r->now)
            r->now = next;
        ua101_eng_tick(&r->eng, r->now);
        r->ticks++;
    }
    EXPECT(guard <= 100000);
    r->now = until;
}

/* Runs until the engine has nothing more to do. */
static void settle(rig *r)
{
    unsigned guard = 0;

    while (++guard < 1000) {
        deliver(r);
        if (ua101_eng_next_deadline(&r->eng) == UINT64_MAX)
            break;
        advance(r, 100000);
    }
    EXPECT(guard < 1000);
    /* Past the pause between two messages, so the next one can go at once. */
    advance(r, 10000);
}

/* A rig that is connected, with Soft Control in and writes allowed. */
static void rig_ready(rig *r, const ua101_eng_config *cfg)
{
    rig_init(r, cfg);
    ua101_sim_switch_move(&r->sim, UA101_SIM_SWITCH_SOFT, 1, 0);
    ua101_eng_transport(&r->eng, 1, r->now);
    settle(r);
    ua101_eng_allow_writes(&r->eng, 1);
    EXPECT(r->eng.state == UA101_ENG_READY);
    EXPECT(ua101_eng_settings(&r->eng)->soft_control == 1);
}

static unsigned count_kind(const rig *r, ua101_msg_kind kind, unsigned from)
{
    unsigned i, n = 0;

    for (i = from; i < r->sent_count; i++)
        n += r->sent[i].kind == kind;
    return n;
}

static ua101_field fld(uint8_t kind, uint8_t group, uint8_t index)
{
    ua101_field f = {kind, group, index};

    return f;
}

static uint32_t unit_value(const rig *r, uint8_t kind, uint8_t group, uint8_t index)
{
    return ua101_block_get(r->sim.block, fld(kind, group, index));
}

/* What must hold at the end of every case. */
static void rig_check(const rig *r)
{
    EXPECT(r->risk[UA101_RISK_NEVER] == 0);
    EXPECT(r->eng.guard_blocks == 0);
    EXPECT(r->sim.unknown_messages == 0);
    EXPECT(r->sim.bad_messages == 0);
    EXPECT(r->sim.dropped == 0);
    EXPECT(r->sent_count < MAX_SENT);
}

/* ---- the message coding ---- */

static void test_examples(void)
{
    /* The worked examples in docs/control-protocol.md, byte for byte. */
    /* NOTE: the notes' example for this one reads "00 02 00 00 00 00", which
     * would be 0x020000 (-24 dB), not 0 dB. The block the real unit returned,
     * and the notes' own pan example, put the highest nibble first, so this
     * code does too: "02 00 00 00 00 00". The checksum is 73 either way, so it
     * can't tell them apart. To be settled by the first write on the unit. */
    static const uint8_t in1_0db[] = {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x04, 0x00,
                                      0x07, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x73, 0xF7};
    static const uint8_t out1_center[] = {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x05,
                                          0x00, 0x03, 0x04, 0x00, 0x00, 0x00, 0x74, 0xF7};
    static const uint8_t patch[] = {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00,
                                    0x02, 0x01, 0x00, 0x07, 0x76, 0xF7};
    static const uint8_t read_all[] = {0xF0, 0x41, 0x10, 0x00, 0x69, 0x11, 0x02, 0x00,
                                       0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x7C, 0xF7};
    /* Checksums hardcoded in Roland's driver: suspend 00, resume 7F, reset all 7E. */
    static const uint8_t suspend[] = {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00,
                                      0x00, 0x00, 0x00, 0x00, 0x00, 0xF7};
    static const uint8_t resume[] = {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00,
                                     0x00, 0x00, 0x00, 0x01, 0x7F, 0xF7};
    static const uint8_t reset_all[] = {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00,
                                        0x00, 0x00, 0x02, 0x00, 0x7E, 0xF7};
    uint8_t b[UA101_CTLMSG_MAX_BYTES];
    ua101_msg m;

    memset(&m, 0, sizeof(m));
    m.kind = UA101_MSG_FIELD;
    m.field = fld(UA101_F_VOLUME, IN, 0);
    m.value = UNITY;
    EXPECT(ua101_ctlmsg_build(&m, b) == sizeof(in1_0db) && !memcmp(b, in1_0db, sizeof(in1_0db)));
    m.field = fld(UA101_F_PAN, OUT, 0);
    m.value = UA101_CENTER_PAN;
    EXPECT(ua101_ctlmsg_build(&m, b) == sizeof(out1_center) &&
           !memcmp(b, out1_center, sizeof(out1_center)));
    m.field = fld(UA101_F_PATCH, 0, 0);
    m.value = 7;
    EXPECT(ua101_ctlmsg_build(&m, b) == sizeof(patch) && !memcmp(b, patch, sizeof(patch)));
    m.kind = UA101_MSG_READ_BLOCK;
    EXPECT(ua101_ctlmsg_build(&m, b) == sizeof(read_all) && !memcmp(b, read_all, sizeof(read_all)));
    m.kind = UA101_MSG_AUTOSAVE;
    m.value = 0;
    EXPECT(ua101_ctlmsg_build(&m, b) == sizeof(suspend) && !memcmp(b, suspend, sizeof(suspend)));
    m.value = 1;
    EXPECT(ua101_ctlmsg_build(&m, b) == sizeof(resume) && !memcmp(b, resume, sizeof(resume)));
    m.kind = UA101_MSG_RESET_ALL;
    EXPECT(ua101_ctlmsg_build(&m, b) == sizeof(reset_all) &&
           !memcmp(b, reset_all, sizeof(reset_all)));

    /* Link uses pair*2; the on/off, link and solo codes differ in the last address byte. */
    m.kind = UA101_MSG_FIELD;
    m.field = fld(UA101_F_LINK, OUT, 3);
    m.value = 1;
    EXPECT(ua101_ctlmsg_build(&m, b) == 13 && b[7] == 5 && b[8] == 6 && b[9] == 1 && b[10] == 1);
    m.field = fld(UA101_F_ON, MON, 1);
    EXPECT(ua101_ctlmsg_build(&m, b) == 13 && b[7] == 3 && b[8] == 1 && b[9] == 2);
    m.field = fld(UA101_F_SOLO, IN, 0);
    EXPECT(ua101_ctlmsg_build(&m, b) == 13 && b[7] == 4 && b[8] == 0 && b[9] == 0);

    /* Store marks a chosen section with 2, load with 1; memory m is at m+3. */
    memset(&m, 0, sizeof(m));
    m.kind = UA101_MSG_STORE;
    m.memory = 4;
    m.sections[2] = m.sections[3] = 1;
    EXPECT(ua101_ctlmsg_build(&m, b) == 16 && b[8] == 7 && b[9] == 4 && b[10] == 0 &&
           b[11] == 0 && b[12] == 2 && b[13] == 2);
    m.kind = UA101_MSG_LOAD;
    m.memory = 0;
    EXPECT(ua101_ctlmsg_build(&m, b) == 16 && b[8] == 3 && b[9] == 8 && b[12] == 1 && b[13] == 1);
    m.kind = UA101_MSG_PRESET;
    m.value = 4;
    EXPECT(ua101_ctlmsg_build(&m, b) == 16 && b[8] == 0 && b[9] == 8 && b[12] == 4 && b[13] == 0);
    m.kind = UA101_MSG_RESET_MEMORY;
    m.memory = 2;
    EXPECT(ua101_ctlmsg_build(&m, b) == 13 && b[8] == 5 && b[9] == 3 && b[10] == 0);
    /* The settings in use: the same message with 00 where a memory has 03 to 07, whatever
       the memory field says. */
    {
        static const uint8_t reset_current[] = {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00,
                                                0x00, 0x00, 0x03, 0x00, 0x7D, 0xF7};

        m.kind = UA101_MSG_RESET_CURRENT;
        EXPECT(ua101_ctlmsg_build(&m, b) == sizeof(reset_current) &&
               !memcmp(b, reset_current, sizeof(reset_current)));
        memset(&m, 0, sizeof(m));
        EXPECT(ua101_ctlmsg_parse(reset_current, sizeof(reset_current), &m) == UA101_MSG_RESET_CURRENT);
    }
}

static void test_fields(void)
{
    static const uint32_t k_tries[] = {0, 1, 2, 7, 11, 0x4000, 0x8000, 0x123456, UNITY,
                                       UA101_MAX_VOLUME};
    uint8_t block[UA101_CTL_BLOCK_BYTES], before[UA101_CTL_BLOCK_BYTES], b[UA101_CTLMSG_MAX_BYTES];
    unsigned id, t, changed, i;
    ua101_settings s;
    ua101_msg m, back;

    for (id = 0; id < UA101_FIELD_COUNT; id++) {
        ua101_field f = ua101_field_from_id(id);

        EXPECT(ua101_field_id(f) == (int)id);
        for (t = 0; t < sizeof(k_tries) / sizeof(k_tries[0]); t++) {
            uint32_t v = k_tries[t], got;
            size_t len;

            memset(&m, 0, sizeof(m));
            m.kind = UA101_MSG_FIELD;
            m.field = f;
            m.value = v;
            len = ua101_ctlmsg_build(&m, b);
            if (!ua101_field_value_ok(f, v)) {
                EXPECT(len == 0);
                continue;
            }
            /* Built, read back, and classed as an ordinary setting. */
            EXPECT(len > 0 && ua101_ctlmsg_parse(b, len, &back) == UA101_MSG_FIELD);
            EXPECT(back.field.kind == f.kind && back.field.group == f.group &&
                   back.field.index == f.index && back.value == v);
            EXPECT(ua101_ctlmsg_risk(b, len) == UA101_RISK_SETTING);

            /* Put into a block, it lands where the existing parser reads it,
             * and nowhere else that the parser can see. */
            memcpy(block, ua101_sim_factory_block(), sizeof(block));
            memcpy(before, block, sizeof(block));
            ua101_block_put(block, f, v);
            EXPECT(ua101_block_get(block, f) == v);
            ua101_settings_parse(block, &s);
            EXPECT(s.bad_nibbles == 0);
            switch (f.kind) {
            case UA101_F_VOLUME:
                got = s.channel[f.group][f.index].volume;
                break;
            case UA101_F_PAN:
                got = s.channel[f.group][f.index].pan;
                break;
            case UA101_F_ON:
                got = s.channel[f.group][f.index].on;
                break;
            case UA101_F_LINK:
                got = s.link[f.group][f.index];
                break;
            case UA101_F_SOLO:
                got = s.solo[f.group - 1];
                break;
            default:
                got = s.patch[1][f.index];
                break;
            }
            EXPECT(got == v);
            ua101_block_put(block, f, ua101_block_get(before, f));
            EXPECT(memcmp(block, before, sizeof(block)) == 0);
        }
    }
    /* No two fields share a byte's bits: set every field, then read each back. */
    memset(block, 0, sizeof(block));
    for (id = 0; id < UA101_FIELD_COUNT; id++) {
        ua101_field f = ua101_field_from_id(id);

        ua101_block_put(block, f, f.kind == UA101_F_VOLUME ? 0x100000u + id
                                  : f.kind == UA101_F_PAN  ? 0x1000u + id
                                  : f.kind == UA101_F_PATCH ? 1u + id % 11u
                                  : f.kind == UA101_F_SOLO ? id : id & 1u);
    }
    for (id = 0, changed = 0; id < UA101_FIELD_COUNT; id++) {
        ua101_field f = ua101_field_from_id(id);
        uint32_t want = f.kind == UA101_F_VOLUME ? 0x100000u + id
                        : f.kind == UA101_F_PAN  ? 0x1000u + id
                        : f.kind == UA101_F_PATCH ? 1u + id % 11u
                        : f.kind == UA101_F_SOLO ? id : id & 1u;

        changed += ua101_block_get(block, f) != want;
    }
    EXPECT(changed == 0);
    /* The fields stay out of the switches, the other patch rows and the unused tail. */
    for (i = 0; i < 12; i++)
        EXPECT(block[i] == 0);
    for (i = 17; i < 22; i++)
        EXPECT(block[i] == 0);
    for (i = 248; i < 256; i++)
        EXPECT(block[i] == 0);

    /* Fields that don't exist. */
    EXPECT(ua101_field_id(fld(UA101_F_PAN, MON, 0)) < 0);
    EXPECT(ua101_field_id(fld(UA101_F_VOLUME, MON, 2)) < 0);
    EXPECT(ua101_field_id(fld(UA101_F_VOLUME, IN, 10)) < 0);
    EXPECT(ua101_field_id(fld(UA101_F_LINK, IN, 5)) < 0);
    EXPECT(ua101_field_id(fld(UA101_F_SOLO, MON, 0)) < 0);
    EXPECT(ua101_field_id(fld(UA101_F_PATCH, 0, 5)) < 0);
    EXPECT(ua101_field_id(fld(UA101_F_VOLUME, 3, 0)) < 0);
}

static uint8_t *raw_set(uint8_t *b, uint8_t a0, uint8_t a1, uint8_t a2, uint8_t a3, uint8_t v)
{
    static const uint8_t head[] = {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12};

    memcpy(b, head, 6);
    b[6] = a0, b[7] = a1, b[8] = a2, b[9] = a3, b[10] = v;
    b[11] = ua101_ctl_checksum(b + 6, 5);
    b[12] = 0xF7;
    return b;
}

static void test_risk(void)
{
    uint8_t b[32];
    ua101_msg m;
    size_t len;
    unsigned n;

    memset(&m, 0, sizeof(m));
    m.kind = UA101_MSG_READ_BLOCK;
    len = ua101_ctlmsg_build(&m, b);
    EXPECT(ua101_ctlmsg_risk(b, len) == UA101_RISK_READ);
    for (m.kind = UA101_MSG_RESET_ALL; m.kind <= UA101_MSG_PRESET; m.kind++) {
        m.value = 3;
        m.sections[3] = 1;
        len = ua101_ctlmsg_build(&m, b);
        EXPECT(len > 0 && ua101_ctlmsg_risk(b, len) == UA101_RISK_MEMORY);
    }
    m.kind = UA101_MSG_AUTOSAVE;
    m.value = 1;
    len = ua101_ctlmsg_build(&m, b);
    EXPECT(ua101_ctlmsg_risk(b, len) == UA101_RISK_SETTING);

    /* The notes' "commands to avoid", and other things nobody decoded. */
    EXPECT(ua101_ctlmsg_risk(raw_set(b, 0, 0, 0, 1, 0), 13) == UA101_RISK_NEVER);
    for (n = 0; n <= 5; n++)
        EXPECT(ua101_ctlmsg_risk(raw_set(b, 0, 1, 0, (uint8_t)n, 1), 13) == UA101_RISK_NEVER);
    /* "Reset the settings in use" (Roland's Current Setting: its "reset one" with the number 0,
       read from its app and driver 2026-11-20 and 21) is a memory command since link version
       17; its neighbours stay unknown. */
    EXPECT(ua101_ctlmsg_risk(raw_set(b, 0, 0, 0, 3, 0), 13) == UA101_RISK_MEMORY);
    EXPECT(ua101_ctlmsg_parse(raw_set(b, 0, 0, 0, 3, 0), 13, &m) == UA101_MSG_RESET_CURRENT);
    EXPECT(ua101_ctlmsg_risk(raw_set(b, 0, 0, 0, 3, 1), 13) == UA101_RISK_NEVER);
    EXPECT(ua101_ctlmsg_risk(raw_set(b, 0, 0, 1, 3, 0), 13) == UA101_RISK_NEVER);
    EXPECT(ua101_ctlmsg_risk(raw_set(b, 0, 0, 2, 3, 0), 13) == UA101_RISK_NEVER);
    EXPECT(ua101_ctlmsg_risk(raw_set(b, 0, 0, 0, 4, 0), 13) == UA101_RISK_NEVER);
    EXPECT(ua101_ctlmsg_risk(raw_set(b, 0, 0, 8, 3, 0), 13) == UA101_RISK_NEVER);
    EXPECT(ua101_ctlmsg_risk(raw_set(b, 0, 2, 0, 0, 7), 13) == UA101_RISK_NEVER); /* patch row 0 */
    EXPECT(ua101_ctlmsg_risk(raw_set(b, 0, 2, 1, 5, 7), 13) == UA101_RISK_NEVER);
    EXPECT(ua101_ctlmsg_risk(raw_set(b, 0, 2, 1, 0, 12), 13) == UA101_RISK_NEVER);
    EXPECT(ua101_ctlmsg_risk(raw_set(b, 0, 2, 1, 0, 0), 13) == UA101_RISK_NEVER);
    EXPECT(ua101_ctlmsg_risk(raw_set(b, 0, 4, 10, 2, 1), 13) == UA101_RISK_NEVER);
    EXPECT(ua101_ctlmsg_risk(raw_set(b, 0, 4, 3, 1, 1), 13) == UA101_RISK_NEVER); /* odd link */
    EXPECT(ua101_ctlmsg_risk(raw_set(b, 0, 6, 0, 2, 1), 13) == UA101_RISK_NEVER);
    EXPECT(ua101_ctlmsg_risk(raw_set(b, 1, 4, 0, 2, 1), 13) == UA101_RISK_NEVER);
    /* A good message with one bit wrong. */
    raw_set(b, 0, 4, 0, 2, 1);
    EXPECT(ua101_ctlmsg_risk(b, 13) == UA101_RISK_SETTING);
    b[11] ^= 1;
    EXPECT(ua101_ctlmsg_risk(b, 13) == UA101_RISK_NEVER);
    EXPECT(ua101_ctlmsg_risk(b, 0) == UA101_RISK_NEVER);
    /* Other requests: Roland's driver has them, its app never sends them. */
    {
        uint8_t req[16] = {0xF0, 0x41, 0x10, 0x00, 0x69, 0x11, 0x01, 0, 0, 0, 0, 0, 0, 0x48, 0x37, 0xF7};

        EXPECT(ua101_ctlmsg_risk(req, 16) == UA101_RISK_NEVER);
        req[6] = 0x02, req[7] = 0x01, req[12] = 0x02, req[13] = 0;
        req[14] = ua101_ctl_checksum(req + 6, 8);
        EXPECT(ua101_ctlmsg_risk(req, 16) == UA101_RISK_NEVER);
    }
    /* A volume above the top, and a nibble that isn't one. */
    {
        uint8_t v[18] = {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0, 4, 0, 7, 8, 0, 0, 0, 0, 0, 0, 0xF7};

        v[16] = ua101_ctl_checksum(v + 6, 10);
        EXPECT(ua101_ctlmsg_risk(v, 18) == UA101_RISK_NEVER);
        v[10] = 0x10;
        v[16] = ua101_ctl_checksum(v + 6, 10);
        EXPECT(ua101_ctlmsg_risk(v, 18) == UA101_RISK_NEVER);
    }
}

/* ---- the pretend unit by itself ---- */

static void test_sim(void)
{
    uint8_t req[UA101_CTL_READ_REQUEST_BYTES], b[UA101_CTLMSG_MAX_BYTES];
    const uint8_t *msg;
    ua101_ctl_view view;
    ua101_settings s;
    ua101_sim sim;
    ua101_msg m;
    size_t len;

    ua101_sim_init(&sim);
    ua101_ctl_read_request(req);
    ua101_sim_receive(&sim, req, sizeof(req));
    EXPECT(ua101_sim_next(&sim, &msg, &len) && len == UA101_CTL_BLOCK_MESSAGE_BYTES);
    EXPECT(ua101_ctl_classify(msg, len, &view) == UA101_CTL_SETTINGS);
    ua101_settings_parse(view.data, &s);
    /* As the real unit read: 48 kHz, Initial Setting, all at 0 dB. */
    EXPECT(ua101_ctl_rate_hz(s.rate_code) == 48000 && !s.soft_control && !s.mono);
    EXPECT(s.patch[1][0] == 7 && s.patch[1][4] == 11);
    EXPECT(s.channel[IN][9].volume == UNITY && s.channel[IN][9].pan == 0x8000);
    EXPECT(s.link[MON][0] == 1 && s.link[IN][0] == 0 && s.link[OUT][4] == 1);
    EXPECT(!ua101_sim_next(&sim, &msg, &len));

    /* A set is applied without a word. */
    memset(&m, 0, sizeof(m));
    m.kind = UA101_MSG_FIELD;
    m.field = fld(UA101_F_VOLUME, OUT, 3);
    m.value = 0x100000;
    ua101_sim_receive(&sim, b, ua101_ctlmsg_build(&m, b));
    EXPECT(ua101_block_get(sim.block, m.field) == 0x100000 && sim.saves == 1);
    EXPECT(!ua101_sim_next(&sim, &msg, &len));

    /* A switch: the block follows, and the burst ends on the final value. */
    ua101_sim_switch_move(&sim, UA101_SIM_SWITCH_SOFT, 1, 3);
    EXPECT(sim.block[6] == 0x04);
    EXPECT(ua101_sim_next(&sim, &msg, &len) && len == 13 && msg[9] == 5 && msg[10] == 1);
    EXPECT(ua101_ctl_classify(msg, len, &view) == UA101_CTL_CHANGE);
    EXPECT(ua101_sim_next(&sim, &msg, &len) && msg[10] == 0);
    EXPECT(ua101_sim_next(&sim, &msg, &len) && msg[10] == 1);
    ua101_sim_switch_move(&sim, UA101_SIM_SWITCH_DIGITAL, 1, 1);
    EXPECT(ua101_sim_next(&sim, &msg, &len) && msg[9] == 2 && msg[10] == 9 && sim.block[5] == 9);

    /* Garbage is counted and changes nothing. */
    b[3] ^= 0x40;
    ua101_sim_receive(&sim, b, 18);
    EXPECT(sim.bad_messages == 1);
    ua101_sim_receive(&sim, raw_set(b, 0, 1, 0, 4, 1), 13);
    EXPECT(sim.unknown_messages == 1 && sim.block[6] == 0x04);
}

/* ---- the engine ---- */

static void test_connect_and_read_only(void)
{
    const ua101_settings *s;
    ua101_msg m;
    uint32_t ticket;
    unsigned i;
    rig r;

    rig_init(&r, NULL);
    EXPECT(r.eng.state == UA101_ENG_OFFLINE);
    EXPECT(ua101_eng_next_deadline(&r.eng) == UINT64_MAX);
    EXPECT(ua101_eng_set_patch(&r.eng, 0, 2, r.now) == UA101_ENG_ERR_NOT_READY);
    EXPECT(r.sent_count == 0);

    ua101_eng_transport(&r.eng, 1, r.now);
    EXPECT(r.eng.state == UA101_ENG_SYNCING && r.sent_count == 1);
    EXPECT(ua101_eng_set_patch(&r.eng, 0, 2, r.now) == UA101_ENG_ERR_NOT_READY);
    settle(&r);
    EXPECT(r.eng.state == UA101_ENG_READY);
    EXPECT((r.events & (UA101_EV_STATE | UA101_EV_SETTINGS)) == (UA101_EV_STATE | UA101_EV_SETTINGS));
    s = ua101_eng_settings(&r.eng);
    EXPECT(ua101_ctl_rate_hz(s->rate_code) == 48000 && s->patch[1][2] == 9);
    EXPECT(s->channel[OUT][0].volume == UNITY && s->soft_control == 0);
    /* The on/off bit reads 0 everywhere; by the assumed meaning that is "on". */
    EXPECT(ua101_eng_monitor_on(&r.eng, IN, 0) == 1);

    /* Connected and idle: one read went out, and nothing is asked for again. */
    EXPECT(r.sent_count == 1 && r.risk[UA101_RISK_READ] == 1);
    r.ticks = 0;
    advance(&r, 3600ull * 1000000);
    EXPECT(r.ticks == 0 && r.sent_count == 1);

    /* Read-only until told otherwise: every way in is refused, nothing leaves. */
    ua101_sim_switch_move(&r.sim, UA101_SIM_SWITCH_SOFT, 1, 1);
    settle(&r);
    EXPECT(ua101_eng_settings(&r.eng)->soft_control == 1);
    EXPECT(ua101_eng_set_volume(&r.eng, IN, 0, 0, r.now) == UA101_ENG_ERR_READ_ONLY);
    EXPECT(ua101_eng_set_pan(&r.eng, IN, 0, 0x4000, r.now) == UA101_ENG_ERR_READ_ONLY);
    EXPECT(ua101_eng_set_monitor(&r.eng, IN, 0, 0, r.now) == UA101_ENG_ERR_READ_ONLY);
    EXPECT(ua101_eng_set_link(&r.eng, IN, 0, 1, r.now) == UA101_ENG_ERR_READ_ONLY);
    EXPECT(ua101_eng_set_patch(&r.eng, 0, 2, r.now) == UA101_ENG_ERR_READ_ONLY);
    EXPECT(ua101_eng_patch_preset(&r.eng, 1, r.now) == UA101_ENG_ERR_READ_ONLY);
    ua101_eng_drag_begin(&r.eng, r.now);
    ua101_eng_drag_end(&r.eng, r.now);
    memset(&m, 0, sizeof(m));
    m.kind = UA101_MSG_RESET_ALL;
    EXPECT(ua101_eng_memory_prepare(&r.eng, &m, r.now, &ticket) == UA101_ENG_ERR_READ_ONLY);
    EXPECT(ua101_eng_memory_confirm(&r.eng, ticket, r.now) == UA101_ENG_ERR_TICKET);
    ua101_eng_finish(&r.eng, r.now);
    settle(&r);
    EXPECT(r.risk[UA101_RISK_SETTING] == 0 && r.risk[UA101_RISK_MEMORY] == 0);
    for (i = 0; i < r.sent_count; i++)
        EXPECT(r.sent[i].kind == UA101_MSG_READ_BLOCK);
    EXPECT(memcmp(r.sim.block + 7, ua101_sim_factory_block() + 7, 249) == 0);
    EXPECT(r.eng.shown.channel[IN][0].volume == UNITY);
    rig_check(&r);
}

static void test_soft_control_gate(void)
{
    rig r;

    rig_init(&r, NULL);
    ua101_eng_transport(&r.eng, 1, r.now);
    settle(&r);
    ua101_eng_allow_writes(&r.eng, 1);
    /* Soft Control is out: the mixer is refused, the patch bay isn't. */
    EXPECT(ua101_eng_set_volume(&r.eng, IN, 0, 0, r.now) == UA101_ENG_ERR_SOFT_CTRL);
    EXPECT(ua101_eng_set_pan(&r.eng, OUT, 1, 0, r.now) == UA101_ENG_ERR_SOFT_CTRL);
    EXPECT(ua101_eng_set_monitor(&r.eng, MON, 0, 0, r.now) == UA101_ENG_ERR_SOFT_CTRL);
    EXPECT(ua101_eng_set_link(&r.eng, IN, 0, 1, r.now) == UA101_ENG_ERR_SOFT_CTRL);
    EXPECT(r.risk[UA101_RISK_SETTING] == 0);
    EXPECT(ua101_eng_set_patch(&r.eng, 1, 2, r.now) == UA101_ENG_OK);
    settle(&r);
    EXPECT(unit_value(&r, UA101_F_PATCH, 0, 1) == 2 && r.eng.disagreements == 0);

    /* Pressed in (a burst of 7): allowed once the block says so. */
    ua101_sim_switch_move(&r.sim, UA101_SIM_SWITCH_SOFT, 1, 7);
    settle(&r);
    EXPECT(ua101_eng_set_volume(&r.eng, IN, 0, 0x1000, r.now) == UA101_ENG_OK);
    settle(&r);
    EXPECT(unit_value(&r, UA101_F_VOLUME, IN, 0) == 0x1000);
    rig_check(&r);
}

static void test_write_and_verify(void)
{
    unsigned before;
    rig r;

    rig_ready(&r, NULL);
    before = r.sent_count;
    r.events = 0;
    EXPECT(ua101_eng_set_volume(&r.eng, IN, 2, 0x0ABCDE, r.now) == UA101_ENG_OK);
    /* Shown at once, sent at once, and marked as not checked yet. */
    EXPECT(ua101_eng_settings(&r.eng)->channel[IN][2].volume == 0x0ABCDE);
    EXPECT(r.events & UA101_EV_SETTINGS);
    EXPECT(r.sent_count == before + 1 && r.sent[before].kind == UA101_MSG_FIELD);
    EXPECT(r.sent[before].field.kind == UA101_F_VOLUME && r.sent[before].field.group == IN &&
           r.sent[before].field.index == 2 && r.sent[before].value == 0x0ABCDE);
    EXPECT(ua101_eng_busy(&r.eng));
    /* Inputs aren't linked on this unit: the neighbour is left alone. */
    EXPECT(unit_value(&r, UA101_F_VOLUME, IN, 3) == UNITY);

    /* No read yet just before the quiet time is over; one read just after. */
    advance(&r, r.eng.cfg.verify_quiet_us - 1);
    EXPECT(r.sent_count == before + 1);
    advance(&r, 1);
    EXPECT(r.sent_count == before + 2 && r.sent[before + 1].kind == UA101_MSG_READ_BLOCK);
    settle(&r);
    EXPECT(!ua101_eng_busy(&r.eng) && r.eng.disagreements == 0);
    EXPECT(r.sent_count == before + 2);

    /* The same value again sends nothing. */
    EXPECT(ua101_eng_set_volume(&r.eng, IN, 2, 0x0ABCDE, r.now) == UA101_ENG_OK);
    settle(&r);
    EXPECT(r.sent_count == before + 2);

    /* Pan, on/off, link and patch each reach the unit. */
    EXPECT(ua101_eng_set_pan(&r.eng, IN, 2, 0x4000, r.now) == UA101_ENG_OK);
    EXPECT(ua101_eng_set_monitor(&r.eng, IN, 2, 0, r.now) == UA101_ENG_OK);
    EXPECT(ua101_eng_set_link(&r.eng, IN, 4, 1, r.now) == UA101_ENG_OK);
    EXPECT(ua101_eng_set_patch(&r.eng, 4, 1, r.now) == UA101_ENG_OK);
    EXPECT(ua101_eng_monitor_on(&r.eng, IN, 2) == 0);
    settle(&r);
    EXPECT(unit_value(&r, UA101_F_PAN, IN, 2) == 0x4000);
    EXPECT(unit_value(&r, UA101_F_ON, IN, 2) == 1); /* assumed: the bit set means muted */
    EXPECT(unit_value(&r, UA101_F_LINK, IN, 4) == 1);
    EXPECT(unit_value(&r, UA101_F_PATCH, 0, 4) == 1);
    EXPECT(memcmp(r.eng.want, r.sim.block, 256) == 0 && r.eng.disagreements == 0);

    /* Refusals. */
    EXPECT(ua101_eng_set_pan(&r.eng, MON, 0, 0, r.now) == UA101_ENG_ERR_NO_CHANNEL);
    EXPECT(ua101_eng_set_pan(&r.eng, IN, 0, 0x8001, r.now) == UA101_ENG_ERR_VALUE);
    EXPECT(ua101_eng_set_volume(&r.eng, IN, 10, 0, r.now) == UA101_ENG_ERR_NO_CHANNEL);
    EXPECT(ua101_eng_set_volume(&r.eng, 3, 0, 0, r.now) == UA101_ENG_ERR_NO_CHANNEL);
    EXPECT(ua101_eng_set_volume(&r.eng, IN, 0, 0x800000, r.now) == UA101_ENG_ERR_VALUE);
    EXPECT(ua101_eng_set_volume(&r.eng, IN, 256, 0, r.now) == UA101_ENG_ERR_NO_CHANNEL);
    EXPECT(ua101_eng_set_patch(&r.eng, 5, 2, r.now) == UA101_ENG_ERR_NO_CHANNEL);
    EXPECT(ua101_eng_set_patch(&r.eng, 0, 0, r.now) == UA101_ENG_ERR_VALUE);
    EXPECT(ua101_eng_set_patch(&r.eng, 0, 12, r.now) == UA101_ENG_ERR_VALUE);
    EXPECT(ua101_eng_set_solo(&r.eng, MON, 0, r.now) == UA101_ENG_ERR_NO_CHANNEL);
    EXPECT(ua101_eng_set_solo(&r.eng, 3, 0, r.now) == UA101_ENG_ERR_NO_CHANNEL);
    EXPECT(ua101_eng_set_solo(&r.eng, IN, 10, r.now) == UA101_ENG_ERR_NO_CHANNEL);
    EXPECT(ua101_eng_set_solo(&r.eng, IN, -2, r.now) == UA101_ENG_ERR_NO_CHANNEL);
    EXPECT(ua101_eng_toggle_solo(&r.eng, OUT, 10, r.now) == UA101_ENG_ERR_NO_CHANNEL);
    settle(&r);
    EXPECT(memcmp(r.eng.want, r.sim.block, 256) == 0);
    rig_check(&r);
}

static void test_volume_ceiling(void)
{
    rig r;

    rig_ready(&r, NULL);
    /* As delivered (since 2026-11-27) the ceiling is the top of the range: up to +12 dB goes
       as it is, and past the top is no volume at all. */
    EXPECT(r.eng.cfg.volume_ceiling == UA101_MAX_VOLUME);
    EXPECT(ua101_eng_set_volume(&r.eng, IN, 0, UNITY + 1, r.now) == UA101_ENG_OK);
    EXPECT(ua101_eng_set_volume(&r.eng, IN, 1, UA101_MAX_VOLUME, r.now) == UA101_ENG_OK);
    EXPECT(ua101_eng_set_volume(&r.eng, IN, 2, UA101_MAX_VOLUME + 1, r.now) == UA101_ENG_ERR_VALUE);
    settle(&r);
    EXPECT(unit_value(&r, UA101_F_VOLUME, IN, 0) == UNITY + 1);
    EXPECT(unit_value(&r, UA101_F_VOLUME, IN, 1) == UA101_MAX_VOLUME);
    /* A lower ceiling holds a higher volume there, and the caller is told. */
    ua101_eng_set_volume_ceiling(&r.eng, UNITY);
    EXPECT(ua101_eng_set_volume(&r.eng, IN, 0, 0x100000, r.now) == UA101_ENG_OK);
    EXPECT(ua101_eng_set_volume(&r.eng, IN, 0, UNITY + 1, r.now) == UA101_ENG_CLAMPED);
    EXPECT(ua101_eng_set_volume(&r.eng, IN, 1, UA101_MAX_VOLUME, r.now) == UA101_ENG_CLAMPED);
    settle(&r);
    EXPECT(unit_value(&r, UA101_F_VOLUME, IN, 0) == UNITY);
    EXPECT(unit_value(&r, UA101_F_VOLUME, IN, 1) == UNITY);
    ua101_eng_set_volume_ceiling(&r.eng, 0xFFFFFFFFu);
    EXPECT(r.eng.cfg.volume_ceiling == UA101_MAX_VOLUME);
    rig_check(&r);
}

static void test_link(void)
{
    unsigned before;
    rig r;

    rig_ready(&r, NULL);
    before = r.sent_count;
    /* Outputs 3-4 are linked on this unit: volume and on/off go to both, pan to one. */
    EXPECT(ua101_eng_set_volume(&r.eng, OUT, 3, 0x080000, r.now) == UA101_ENG_OK);
    EXPECT(ua101_eng_set_monitor(&r.eng, OUT, 2, 0, r.now) == UA101_ENG_OK);
    EXPECT(ua101_eng_set_pan(&r.eng, OUT, 2, 0x2000, r.now) == UA101_ENG_OK);
    EXPECT(ua101_eng_set_volume(&r.eng, MON, 0, 0x040000, r.now) == UA101_ENG_OK);
    settle(&r);
    EXPECT(unit_value(&r, UA101_F_VOLUME, OUT, 2) == 0x080000);
    EXPECT(unit_value(&r, UA101_F_VOLUME, OUT, 3) == 0x080000);
    EXPECT(unit_value(&r, UA101_F_ON, OUT, 2) == 1 && unit_value(&r, UA101_F_ON, OUT, 3) == 1);
    EXPECT(unit_value(&r, UA101_F_PAN, OUT, 2) == 0x2000);
    EXPECT(unit_value(&r, UA101_F_PAN, OUT, 3) == 0x8000);
    EXPECT(unit_value(&r, UA101_F_VOLUME, MON, 1) == 0x040000);
    EXPECT(unit_value(&r, UA101_F_VOLUME, OUT, 4) == UNITY);
    EXPECT(count_kind(&r, UA101_MSG_FIELD, before) == 7);

    /* Unlinked, only the one channel moves. */
    EXPECT(ua101_eng_set_link(&r.eng, OUT, 1, 0, r.now) == UA101_ENG_OK);
    EXPECT(ua101_eng_set_volume(&r.eng, OUT, 3, 0x010000, r.now) == UA101_ENG_OK);
    settle(&r);
    EXPECT(unit_value(&r, UA101_F_VOLUME, OUT, 2) == 0x080000);
    EXPECT(unit_value(&r, UA101_F_VOLUME, OUT, 3) == 0x010000);
    EXPECT(r.eng.disagreements == 0);
    rig_check(&r);
}

/* Solo, as Roland's own program does it (read from its code on 2026-11-16): one number a
   group, 0 for none or the soloed channel's number 1 to 10; a click on a lit SOLO puts it
   out; a linked partner is shown lit too; always one message. (Not inlined: with the address
   checker every case inlined into main keeps its own rig there, and one more overflowed the
   stack.) */
__attribute__((noinline)) static void test_solo(void)
{
    static const uint8_t in3[13] = {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00,
                                    0x04, 0x00, 0x00, 0x03, 0x79, 0xF7};
    static const uint8_t out10[13] = {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00,
                                      0x05, 0x00, 0x00, 0x0A, 0x71, 0xF7};
    static const uint8_t none[13] = {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00,
                                     0x04, 0x00, 0x00, 0x00, 0x7C, 0xF7};
    uint8_t b[UA101_CTLMSG_MAX_BYTES];
    unsigned before, c;
    ua101_msg m;
    rig r;

    /* The bytes: 00 <group+3> 00 00, one byte, by hand (the checksum counted down from 0x80). */
    memset(&m, 0, sizeof(m));
    m.kind = UA101_MSG_FIELD;
    m.field = fld(UA101_F_SOLO, IN, 0);
    m.value = 3;
    EXPECT(ua101_ctlmsg_build(&m, b) == 13 && !memcmp(b, in3, 13));
    m.value = 0;
    EXPECT(ua101_ctlmsg_build(&m, b) == 13 && !memcmp(b, none, 13));
    m.field = fld(UA101_F_SOLO, OUT, 0);
    m.value = 10;
    EXPECT(ua101_ctlmsg_build(&m, b) == 13 && !memcmp(b, out10, 13));
    m.value = 11; /* no such channel: the builder makes nothing, the parser knows no such message */
    EXPECT(ua101_ctlmsg_build(&m, b) == 0);
    memcpy(b, out10, 13);
    b[10] = 0x0B;
    b[11] = 0x70;
    EXPECT(ua101_ctlmsg_parse(b, 13, &m) == UA101_MSG_UNKNOWN &&
           ua101_ctlmsg_risk(b, 13) == UA101_RISK_NEVER);
    EXPECT(ua101_ctlmsg_parse(out10, 13, &m) == UA101_MSG_FIELD && m.field.kind == UA101_F_SOLO &&
           m.field.group == OUT && m.field.index == 0 && m.value == 10);
    memcpy(b, in3, 13);
    b[8] = 0x01; /* a channel byte other than 00: not a solo */
    b[11] = 0x78;
    EXPECT(ua101_ctlmsg_parse(b, 13, &m) == UA101_MSG_UNKNOWN);

    rig_ready(&r, NULL);
    EXPECT(unit_value(&r, UA101_F_SOLO, IN, 0) == 0 && unit_value(&r, UA101_F_SOLO, OUT, 0) == 0);
    for (c = 0; c < 10; c++)
        EXPECT(!ua101_eng_soloed(&r.eng, IN, c) && !ua101_eng_soloed(&r.eng, OUT, c));
    EXPECT(!ua101_eng_soloed(&r.eng, MON, 0) && !ua101_eng_soloed(&r.eng, IN, 10));

    /* INPUT 3 (the inputs are not linked): one message, the unit holds 3, one lamp. */
    before = r.sent_count;
    EXPECT(ua101_eng_toggle_solo(&r.eng, IN, 2, r.now) == UA101_ENG_OK);
    settle(&r);
    EXPECT(count_kind(&r, UA101_MSG_FIELD, before) == 1 && r.sent[before].field.kind == UA101_F_SOLO &&
           r.sent[before].field.group == IN && r.sent[before].value == 3);
    EXPECT(unit_value(&r, UA101_F_SOLO, IN, 0) == 3 && unit_value(&r, UA101_F_SOLO, OUT, 0) == 0);
    for (c = 0; c < 10; c++)
        EXPECT(ua101_eng_soloed(&r.eng, IN, c) == (c == 2) && !ua101_eng_soloed(&r.eng, OUT, c));
    /* Another channel of the group takes the solo over: still one number. */
    EXPECT(ua101_eng_toggle_solo(&r.eng, IN, 7, r.now) == UA101_ENG_OK);
    settle(&r);
    EXPECT(unit_value(&r, UA101_F_SOLO, IN, 0) == 8);
    for (c = 0; c < 10; c++)
        EXPECT(ua101_eng_soloed(&r.eng, IN, c) == (c == 7));
    /* A click on the lit one puts the solo out. */
    EXPECT(ua101_eng_toggle_solo(&r.eng, IN, 7, r.now) == UA101_ENG_OK);
    settle(&r);
    EXPECT(unit_value(&r, UA101_F_SOLO, IN, 0) == 0);

    /* OUTPUT 3-4 is linked on this unit: a solo of OUTPUT 4 is one message with the number 4,
       both lamps of the pair are lit, and a click on either puts it out. */
    before = r.sent_count;
    EXPECT(ua101_eng_toggle_solo(&r.eng, OUT, 3, r.now) == UA101_ENG_OK);
    settle(&r);
    EXPECT(count_kind(&r, UA101_MSG_FIELD, before) == 1 && unit_value(&r, UA101_F_SOLO, OUT, 0) == 4);
    for (c = 0; c < 10; c++)
        EXPECT(ua101_eng_soloed(&r.eng, OUT, c) == (c == 2 || c == 3) && !ua101_eng_soloed(&r.eng, IN, c));
    EXPECT(ua101_eng_toggle_solo(&r.eng, OUT, 2, r.now) == UA101_ENG_OK); /* the partner's lamp */
    settle(&r);
    EXPECT(unit_value(&r, UA101_F_SOLO, OUT, 0) == 0);
    /* The two groups are apart; "none" for a group with none sends nothing. */
    EXPECT(ua101_eng_set_solo(&r.eng, IN, 0, r.now) == UA101_ENG_OK);
    EXPECT(ua101_eng_set_solo(&r.eng, OUT, 9, r.now) == UA101_ENG_OK);
    settle(&r);
    EXPECT(unit_value(&r, UA101_F_SOLO, IN, 0) == 1 && unit_value(&r, UA101_F_SOLO, OUT, 0) == 10);
    EXPECT(ua101_eng_set_solo(&r.eng, IN, UA101_ENG_SOLO_NONE, r.now) == UA101_ENG_OK);
    EXPECT(ua101_eng_set_solo(&r.eng, OUT, UA101_ENG_SOLO_NONE, r.now) == UA101_ENG_OK);
    settle(&r);
    before = r.sent_count;
    EXPECT(ua101_eng_set_solo(&r.eng, IN, UA101_ENG_SOLO_NONE, r.now) == UA101_ENG_OK);
    settle(&r);
    EXPECT(count_kind(&r, UA101_MSG_FIELD, before) == 0);
    EXPECT(unit_value(&r, UA101_F_SOLO, IN, 0) == 0 && unit_value(&r, UA101_F_SOLO, OUT, 0) == 0);
    /* No solo for MONITOR OUT or a channel the mixer doesn't have; nothing was sent for them. */
    before = r.sent_count;
    EXPECT(ua101_eng_toggle_solo(&r.eng, MON, 0, r.now) == UA101_ENG_ERR_NO_CHANNEL);
    EXPECT(ua101_eng_set_solo(&r.eng, OUT, 10, r.now) == UA101_ENG_ERR_NO_CHANNEL);
    settle(&r);
    EXPECT(count_kind(&r, UA101_MSG_FIELD, before) == 0);
    EXPECT(r.eng.disagreements == 0 && memcmp(r.eng.want, r.sim.block, 256) == 0);
    rig_check(&r);

    /* Read-only, and Soft Control out: refused like every other mixer control. */
    rig_init(&r, NULL);
    ua101_eng_transport(&r.eng, 1, r.now);
    settle(&r);
    EXPECT(ua101_eng_toggle_solo(&r.eng, IN, 0, r.now) == UA101_ENG_ERR_READ_ONLY);
    ua101_eng_allow_writes(&r.eng, 1);
    EXPECT(ua101_eng_toggle_solo(&r.eng, IN, 0, r.now) == UA101_ENG_ERR_SOFT_CTRL);
    settle(&r);
    EXPECT(r.risk[UA101_RISK_SETTING] == 0 && unit_value(&r, UA101_F_SOLO, IN, 0) == 0);
    rig_check(&r);
}

static void test_drag(void)
{
    unsigned before, i, last;
    uint64_t saves;
    rig r;

    rig_ready(&r, NULL);
    before = r.sent_count;
    saves = r.sim.saves;

    /* A drag: 300 mouse moves 2 ms apart on an unlinked input. */
    ua101_eng_drag_begin(&r.eng, r.now);
    EXPECT(r.sent_count == before); /* nothing until something changes */
    for (i = 1; i <= 300; i++) {
        EXPECT(ua101_eng_set_volume(&r.eng, IN, 0, i * 0x1000, r.now) == UA101_ENG_OK);
        /* The window shows each step at once, whatever has been sent. */
        EXPECT(ua101_eng_settings(&r.eng)->channel[IN][0].volume == i * 0x1000);
        advance(&r, 2000);
    }
    /* No check and no resume while the mouse is down. */
    advance(&r, 2000000);
    EXPECT(count_kind(&r, UA101_MSG_READ_BLOCK, before) == 0);
    EXPECT(r.sim.saving_paused == 1 && r.sim.saves == saves);
    ua101_eng_drag_end(&r.eng, r.now);
    settle(&r);

    /* First the pause, last the resume, then one read; the unit saved once. */
    last = r.sent_count - 1;
    EXPECT(r.sent[before].kind == UA101_MSG_AUTOSAVE && r.sent[before].value == 0);
    EXPECT(r.sent[last].kind == UA101_MSG_READ_BLOCK);
    EXPECT(r.sent[last - 1].kind == UA101_MSG_AUTOSAVE && r.sent[last - 1].value == 1);
    EXPECT(count_kind(&r, UA101_MSG_AUTOSAVE, before) == 2);
    EXPECT(count_kind(&r, UA101_MSG_READ_BLOCK, before) == 1);
    EXPECT(r.sim.saves == saves + 1 && r.sim.saving_paused == 0);
    EXPECT(unit_value(&r, UA101_F_VOLUME, IN, 0) == 300 * 0x1000);
    /* 300 moves in 600 ms with 5 ms between messages: at most 121 went out. */
    EXPECT(count_kind(&r, UA101_MSG_FIELD, before) <= 121);
    EXPECT(count_kind(&r, UA101_MSG_FIELD, before) >= 100);
    EXPECT(!ua101_eng_busy(&r.eng) && r.eng.disagreements == 0);

    /* Two drags at once (two fingers): one pause, one resume. */
    before = r.sent_count;
    ua101_eng_drag_begin(&r.eng, r.now);
    ua101_eng_drag_begin(&r.eng, r.now);
    ua101_eng_set_pan(&r.eng, IN, 0, 0x1000, r.now);
    advance(&r, 50000);
    ua101_eng_drag_end(&r.eng, r.now);
    ua101_eng_set_pan(&r.eng, IN, 1, 0x1000, r.now);
    advance(&r, 50000);
    EXPECT(count_kind(&r, UA101_MSG_AUTOSAVE, before) == 1);
    ua101_eng_drag_end(&r.eng, r.now);
    ua101_eng_drag_end(&r.eng, r.now); /* one too many does no harm */
    settle(&r);
    EXPECT(count_kind(&r, UA101_MSG_AUTOSAVE, before) == 2 && r.sim.saving_paused == 0);

    /* A drag that changes nothing sends nothing. */
    before = r.sent_count;
    ua101_eng_drag_begin(&r.eng, r.now);
    ua101_eng_drag_end(&r.eng, r.now);
    settle(&r);
    EXPECT(r.sent_count == before);

    /* Without a drag (a click), each change is saved by the unit: no pause. */
    saves = r.sim.saves;
    ua101_eng_set_patch(&r.eng, 0, 3, r.now);
    settle(&r);
    EXPECT(count_kind(&r, UA101_MSG_AUTOSAVE, before) == 0 && r.sim.saves == saves + 1);

    /* Quitting in the middle of a drag: what waits goes out, and saving resumes. */
    ua101_eng_drag_begin(&r.eng, r.now);
    ua101_eng_set_volume(&r.eng, IN, 5, 0x3000, r.now);
    ua101_eng_set_volume(&r.eng, IN, 6, 0x3000, r.now);
    ua101_eng_set_volume(&r.eng, IN, 7, 0x3000, r.now);
    EXPECT(r.sim.saving_paused == 1 && unit_value(&r, UA101_F_VOLUME, IN, 7) == UNITY);
    ua101_eng_finish(&r.eng, r.now);
    EXPECT(r.sim.saving_paused == 0 && unit_value(&r, UA101_F_VOLUME, IN, 7) == 0x3000);
    EXPECT(unit_value(&r, UA101_F_VOLUME, IN, 5) == 0x3000);
    rig_check(&r);
}

/* The unit's saving paused and resumed by itself: the first write planned for a real unit. */
static void test_saving_alone(void)
{
    unsigned before;
    uint64_t blocks, sets;
    rig r;

    rig_init(&r, NULL);
    EXPECT(ua101_eng_pause_saving(&r.eng, r.now) == UA101_ENG_ERR_NOT_READY);
    ua101_eng_transport(&r.eng, 1, r.now);
    settle(&r);
    EXPECT(ua101_eng_pause_saving(&r.eng, r.now) == UA101_ENG_ERR_READ_ONLY);
    EXPECT(ua101_eng_resume_saving(&r.eng, r.now) == UA101_ENG_ERR_VALUE);
    ua101_eng_allow_writes(&r.eng, 1);
    before = r.sent_count;
    blocks = r.eng.blocks_received;
    sets = r.sim.sets_applied;

    /* Pause: one message at once, one read after the quiet time, then nothing for an hour. */
    EXPECT(ua101_eng_pause_saving(&r.eng, r.now) == UA101_ENG_OK);
    EXPECT(r.sent_count == before + 1 && r.sent[before].kind == UA101_MSG_AUTOSAVE &&
           r.sent[before].value == 0);
    EXPECT(ua101_eng_saving_held(&r.eng) && ua101_eng_busy(&r.eng));
    EXPECT(ua101_eng_pause_saving(&r.eng, r.now) == UA101_ENG_ERR_BUSY);
    advance(&r, r.eng.cfg.verify_quiet_us - 1);
    EXPECT(r.sent_count == before + 1);
    settle(&r);
    EXPECT(r.sent_count == before + 2 && r.sent[before + 1].kind == UA101_MSG_READ_BLOCK);
    EXPECT(r.eng.blocks_received == blocks + 1 && r.sim.saving_paused == 1);
    advance(&r, 3600000000ull);
    EXPECT(r.sent_count == before + 2 && r.sim.saving_paused == 1);
    EXPECT(ua101_eng_next_deadline(&r.eng) == UINT64_MAX);

    /* Resume: one message, one read. */
    EXPECT(ua101_eng_resume_saving(&r.eng, r.now) == UA101_ENG_OK);
    settle(&r);
    EXPECT(r.sent_count == before + 4 && r.sent[before + 2].kind == UA101_MSG_AUTOSAVE &&
           r.sent[before + 2].value == 1 && r.sent[before + 3].kind == UA101_MSG_READ_BLOCK);
    EXPECT(r.sim.saving_paused == 0 && !ua101_eng_saving_held(&r.eng) && !ua101_eng_busy(&r.eng));
    EXPECT(r.eng.blocks_received == blocks + 2 && r.sim.sets_applied == sets);
    EXPECT(r.eng.disagreements == 0 && r.eng.guard_blocks == 0);
    EXPECT(ua101_eng_next_deadline(&r.eng) == UINT64_MAX);

    /* The way to the unit drops while paused: the resume is owed, and sent when it is back. */
    EXPECT(ua101_eng_pause_saving(&r.eng, r.now) == UA101_ENG_OK);
    settle(&r);
    ua101_eng_transport(&r.eng, 0, r.now);
    EXPECT(!ua101_eng_saving_held(&r.eng) && r.sim.saving_paused == 1);
    advance(&r, 5000000);
    ua101_eng_transport(&r.eng, 1, r.now);
    settle(&r);
    EXPECT(r.sim.saving_paused == 0 && !ua101_eng_busy(&r.eng));

    /* Quitting while paused resumes first. */
    EXPECT(ua101_eng_pause_saving(&r.eng, r.now) == UA101_ENG_OK);
    settle(&r);
    ua101_eng_finish(&r.eng, r.now);
    EXPECT(r.sim.saving_paused == 0);
    settle(&r);

    /* The way refused a change: read-only, nothing owed, no spinning, one fresh read. */
    EXPECT(ua101_eng_pause_saving(&r.eng, r.now) == UA101_ENG_OK);
    settle(&r);
    before = r.sent_count;
    ua101_eng_writes_refused(&r.eng, r.now);
    settle(&r);
    EXPECT(!r.eng.allow_writes && !r.eng.allow_memory && !ua101_eng_busy(&r.eng));
    EXPECT(r.sent_count == before + 1 && r.sent[before].kind == UA101_MSG_READ_BLOCK);
    advance(&r, 3600000000ull);
    EXPECT(r.sent_count == before + 1 && ua101_eng_next_deadline(&r.eng) == UINT64_MAX);
    EXPECT(r.eng.guard_blocks == 0);
    EXPECT(ua101_eng_pause_saving(&r.eng, r.now) == UA101_ENG_ERR_READ_ONLY);
}

static void test_coalescing_keeps_order_and_last_value(void)
{
    unsigned before, i;
    rig r;

    rig_ready(&r, NULL);
    before = r.sent_count;
    /* A burst at one instant on three controls: one message each, latest value. */
    for (i = 0; i < 50; i++) {
        ua101_eng_set_volume(&r.eng, IN, 0, 0x1000 + i, r.now);
        ua101_eng_set_pan(&r.eng, IN, 1, 0x100 + i, r.now);
        ua101_eng_set_patch(&r.eng, 2, 1 + i % 11, r.now);
    }
    settle(&r);
    /* The first volume went at once; pan and patch waited for their turn. */
    EXPECT(count_kind(&r, UA101_MSG_FIELD, before) == 4);
    EXPECT(r.sent[before].field.kind == UA101_F_VOLUME && r.sent[before].value == 0x1000);
    EXPECT(r.sent[before + 1].field.kind == UA101_F_PAN && r.sent[before + 1].value == 0x100 + 49);
    EXPECT(r.sent[before + 2].field.kind == UA101_F_PATCH);
    EXPECT(r.sent[before + 3].field.kind == UA101_F_VOLUME && r.sent[before + 3].value == 0x1000 + 49);
    EXPECT(memcmp(r.eng.want, r.sim.block, 256) == 0 && r.eng.disagreements == 0);
    rig_check(&r);
}

static void test_disagreement(void)
{
    rig r;

    rig_ready(&r, NULL);
    /* The unit takes pan but not volume (as a real one might, for all we know). */
    r.sim.ignore_kind_plus_1 = UA101_F_VOLUME + 1;
    r.events = 0;
    ua101_eng_set_volume(&r.eng, IN, 4, 0x012345, r.now);
    ua101_eng_set_pan(&r.eng, IN, 4, 0x1111, r.now);
    EXPECT(ua101_eng_settings(&r.eng)->channel[IN][4].volume == 0x012345);
    settle(&r);
    /* The unit's value wins, and the disagreement is reported with both values. */
    EXPECT(r.eng.disagreements == 1 && (r.events & UA101_EV_DISAGREE));
    EXPECT(r.eng.last_disagree_field.kind == UA101_F_VOLUME &&
           r.eng.last_disagree_field.group == IN && r.eng.last_disagree_field.index == 4);
    EXPECT(r.eng.last_disagree_sent == 0x012345 && r.eng.last_disagree_unit == UNITY);
    EXPECT(ua101_eng_settings(&r.eng)->channel[IN][4].volume == UNITY);
    EXPECT(ua101_eng_settings(&r.eng)->channel[IN][4].pan == 0x1111);
    EXPECT(!ua101_eng_busy(&r.eng));
    EXPECT(memcmp(r.eng.want, r.sim.block, 256) == 0);

    /* A unit that drops mixer changes while Soft Control is out. */
    r.sim.ignore_kind_plus_1 = 0;
    r.sim.ignore_mixer_without_soft_control = 1;
    ua101_eng_set_volume(&r.eng, IN, 4, 0x1000, r.now);
    settle(&r);
    EXPECT(r.eng.disagreements == 1 && unit_value(&r, UA101_F_VOLUME, IN, 4) == 0x1000);
    rig_check(&r);
}

static void test_stale_reply(void)
{
    unsigned before;
    rig r;

    rig_ready(&r, NULL);
    before = r.sent_count;
    /* A read is on its way back when the user changes something. */
    r.hold_replies = 1;
    ua101_eng_refresh(&r.eng, r.now);
    EXPECT(r.sim.out_count == 1);
    advance(&r, 10000);
    ua101_eng_set_volume(&r.eng, IN, 0, 0x5000, r.now);
    ua101_eng_set_pan(&r.eng, IN, 0, 0x5000, r.now); /* this one is still waiting to go */
    r.events = 0;
    r.hold_replies = 0;
    deliver(&r);
    /* The old block must not undo the new values, and isn't a disagreement. */
    EXPECT(ua101_eng_settings(&r.eng)->channel[IN][0].volume == 0x5000);
    EXPECT(ua101_eng_settings(&r.eng)->channel[IN][0].pan == 0x5000);
    EXPECT(r.eng.disagreements == 0 && ua101_eng_busy(&r.eng));
    settle(&r);
    /* They are checked by a later read. */
    EXPECT(!ua101_eng_busy(&r.eng) && r.eng.disagreements == 0);
    EXPECT(count_kind(&r, UA101_MSG_READ_BLOCK, before) == 2);
    EXPECT(memcmp(r.eng.want, r.sim.block, 256) == 0);

    /* A change made while the checking read is out: checked again, not lost. */
    ua101_eng_set_volume(&r.eng, IN, 1, 0x6000, r.now);
    r.hold_replies = 1;
    advance(&r, r.eng.cfg.verify_quiet_us + 1000);
    EXPECT(r.sim.out_count == 1);
    ua101_eng_set_volume(&r.eng, IN, 1, 0x7000, r.now);
    r.hold_replies = 0;
    settle(&r);
    EXPECT(ua101_eng_settings(&r.eng)->channel[IN][1].volume == 0x7000);
    EXPECT(unit_value(&r, UA101_F_VOLUME, IN, 1) == 0x7000 && r.eng.disagreements == 0);
    rig_check(&r);
}

static void test_switch_bursts(void)
{
    unsigned before, i;
    const uint8_t *msg;
    size_t len;
    rig r;

    rig_init(&r, NULL);
    ua101_eng_transport(&r.eng, 1, r.now);
    settle(&r);
    before = r.sent_count;

    /* Soft Control pressed: 7 messages over 377 ms, as measured on the unit.
     * Hand them over one at a time. */
    r.hold_replies = 1;
    ua101_sim_switch_move(&r.sim, UA101_SIM_SWITCH_SOFT, 1, 7);
    r.events = 0;
    for (i = 0; i < 7; i++) {
        EXPECT(ua101_sim_next(&r.sim, &msg, &len));
        over_usb(&r.to_host, msg, len, host_gets, &r);
        /* Nothing is believed, and nothing asked, during the burst. */
        EXPECT(ua101_eng_settings(&r.eng)->soft_control == 0);
        EXPECT(r.sent_count == before);
        if (i < 6)
            advance(&r, 377000 / 6);
    }
    r.hold_replies = 0;
    EXPECT(r.events & UA101_EV_SETTLING);
    /* 500 ms after the last message: one read, and the block is believed. */
    advance(&r, r.eng.cfg.settle_us - 1);
    EXPECT(r.sent_count == before);
    advance(&r, 1);
    EXPECT(r.sent_count == before + 1 && r.sent[before].kind == UA101_MSG_READ_BLOCK);
    settle(&r);
    EXPECT(ua101_eng_settings(&r.eng)->soft_control == 1 && r.sent_count == before + 1);

    /* Mono and the digital input switch show up the same way. */
    ua101_sim_switch_move(&r.sim, UA101_SIM_SWITCH_MONO, 1, 3);
    ua101_sim_switch_move(&r.sim, UA101_SIM_SWITCH_DIGITAL, 1, 3);
    settle(&r);
    EXPECT(ua101_eng_settings(&r.eng)->mono == 1);
    EXPECT(ua101_eng_settings(&r.eng)->clock_source == 1);
    EXPECT(ua101_eng_settings(&r.eng)->clock_status == 1);
    EXPECT(r.sent_count == before + 2);

    /* Old switch messages the unit held back arrive right at the start. */
    rig_init(&r, NULL);
    ua101_sim_switch_move(&r.sim, UA101_SIM_SWITCH_MONO, 1, 1);
    ua101_sim_switch_move(&r.sim, UA101_SIM_SWITCH_MONO, 0, 2);
    ua101_eng_transport(&r.eng, 1, r.now);
    settle(&r);
    EXPECT(r.eng.state == UA101_ENG_READY && ua101_eng_settings(&r.eng)->mono == 0);
    EXPECT(r.sent_count == 2); /* the first read, and one more after the quiet */
    rig_check(&r);
}

static void test_soft_control_released_mid_change(void)
{
    rig r;

    rig_ready(&r, NULL);
    ua101_eng_set_volume(&r.eng, IN, 0, 0x2000, r.now);
    /* Soft Control is released; once the block says so, the mixer is refused. */
    ua101_sim_switch_move(&r.sim, UA101_SIM_SWITCH_SOFT, 0, 5);
    settle(&r);
    EXPECT(ua101_eng_settings(&r.eng)->soft_control == 0);
    EXPECT(ua101_eng_set_volume(&r.eng, IN, 0, 0x3000, r.now) == UA101_ENG_ERR_SOFT_CTRL);
    EXPECT(!ua101_eng_busy(&r.eng) && memcmp(r.eng.want, r.sim.block, 256) == 0);
    rig_check(&r);
}

static void test_silent_unit(void)
{
    uint64_t start;
    rig r;

    rig_init(&r, NULL);
    r.sim.silent = 1;
    start = r.now;
    ua101_eng_transport(&r.eng, 1, r.now);
    /* Three tries a second apart, then SILENT. */
    advance(&r, 2999999);
    EXPECT(r.eng.state == UA101_ENG_SYNCING && r.sent_count == 3);
    advance(&r, 1);
    EXPECT(r.eng.state == UA101_ENG_SILENT && r.sent_count == 3);
    EXPECT(r.now == start + 3000000);
    ua101_eng_allow_writes(&r.eng, 1);
    EXPECT(ua101_eng_set_patch(&r.eng, 0, 2, r.now) == UA101_ENG_ERR_NOT_READY);
    /* It keeps asking, slowly: a try every 3 s, each given up after 3 more. */
    advance(&r, 3000000);
    EXPECT(r.sent_count == 4);
    /* The unit wakes up. */
    r.sim.silent = 0;
    advance(&r, 1000000);
    settle(&r);
    EXPECT(r.eng.state == UA101_ENG_READY);
    EXPECT(ua101_eng_set_patch(&r.eng, 0, 2, r.now) == UA101_ENG_OK);
    settle(&r);
    EXPECT(unit_value(&r, UA101_F_PATCH, 0, 0) == 2);

    /* It goes quiet with changes on their way: they are dropped, not kept for later. */
    ua101_sim_switch_move(&r.sim, UA101_SIM_SWITCH_SOFT, 1, 0);
    ua101_eng_refresh(&r.eng, r.now);
    settle(&r);
    r.sim.silent = 1;
    ua101_eng_drag_begin(&r.eng, r.now);
    ua101_eng_set_volume(&r.eng, IN, 0, 0x2000, r.now);
    ua101_eng_set_volume(&r.eng, IN, 1, 0x2000, r.now);
    ua101_eng_refresh(&r.eng, r.now);
    advance(&r, 20000000);
    EXPECT(r.eng.state == UA101_ENG_SILENT);
    EXPECT(r.eng.fifo_count == 0 && r.eng.unverified_count == 0);
    EXPECT(r.sim.saving_paused == 1);
    r.sim.silent = 0;
    advance(&r, 10000000);
    settle(&r);
    EXPECT(r.eng.state == UA101_ENG_READY && !ua101_eng_busy(&r.eng));
    /* The unit had been told to pause its saving: it is told to resume. */
    EXPECT(r.sim.saving_paused == 0);
    /* The pretend unit took the change before going quiet; the picture follows the unit. */
    EXPECT(memcmp(r.eng.want, r.sim.block, 256) == 0);
    rig_check(&r);
}

static void test_bad_reply(void)
{
    rig r;

    rig_init(&r, NULL);
    r.sim.corrupt_next_reply = 1;
    ua101_eng_transport(&r.eng, 1, r.now);
    advance(&r, 500000);
    /* A block with a wrong checksum is not believed. */
    EXPECT(r.eng.state == UA101_ENG_SYNCING && r.eng.bad_messages == 1);
    settle(&r);
    EXPECT(r.eng.state == UA101_ENG_READY && r.sent_count == 2);
    /* Rubbish from the cable. */
    {
        static const uint8_t junk[] = {0xF0, 0x7E, 0x00, 0x06, 0x02, 0x41, 0xF7};

        ua101_eng_receive(&r.eng, junk, sizeof(junk), r.now);
        ua101_eng_receive(&r.eng, junk, 0, r.now);
        EXPECT(r.eng.bad_messages == 3 && r.eng.state == UA101_ENG_READY);
    }
    rig_check(&r);
}

static void test_transport_loss(void)
{
    unsigned before;
    rig r;

    rig_ready(&r, NULL);
    ua101_eng_drag_begin(&r.eng, r.now);
    ua101_eng_set_volume(&r.eng, IN, 0, 0x2000, r.now);
    ua101_eng_set_volume(&r.eng, IN, 1, 0x2000, r.now);
    /* Unplugged: nothing more is sent, not even at the next deadline. */
    ua101_eng_transport(&r.eng, 0, r.now);
    before = r.sent_count;
    EXPECT(r.eng.state == UA101_ENG_OFFLINE && ua101_eng_next_deadline(&r.eng) == UINT64_MAX);
    advance(&r, 10000000);
    ua101_eng_drag_end(&r.eng, r.now);
    ua101_eng_finish(&r.eng, r.now);
    EXPECT(ua101_eng_set_patch(&r.eng, 0, 2, r.now) == UA101_ENG_ERR_NOT_READY);
    EXPECT(r.sent_count == before);
    /* Back: it reads first, and writes stay allowed (the user's choice stands). */
    ua101_sim_init(&r.sim);
    ua101_eng_transport(&r.eng, 1, r.now);
    settle(&r);
    EXPECT(r.eng.state == UA101_ENG_READY && !ua101_eng_busy(&r.eng));
    EXPECT(memcmp(r.eng.want, r.sim.block, 256) == 0);
    EXPECT(count_kind(&r, UA101_MSG_FIELD, before) == 0);

    /* A way to the unit that refuses messages: no spinning, and it recovers. */
    r.wire_refuses = 1;
    ua101_eng_set_patch(&r.eng, 0, 2, r.now);
    r.ticks = 0;
    advance(&r, 1000000);
    EXPECT(r.ticks <= 12 && r.eng.send_failures > 0);
    r.wire_refuses = 0;
    settle(&r);
    EXPECT(unit_value(&r, UA101_F_PATCH, 0, 0) == 2 && !ua101_eng_busy(&r.eng));
    EXPECT(r.eng.guard_blocks == 0);
}

static void test_192k(void)
{
    rig r;

    rig_init(&r, NULL);
    ua101_sim_set_rate_code(&r.sim, 4);
    ua101_sim_switch_move(&r.sim, UA101_SIM_SWITCH_SOFT, 1, 0);
    ua101_eng_transport(&r.eng, 1, r.now);
    settle(&r);
    ua101_eng_allow_writes(&r.eng, 1);
    EXPECT(ua101_ctl_rate_hz(ua101_eng_settings(&r.eng)->rate_code) == 192000);
    /* Six channels each way at 192 kHz: 7 to 10 are refused, as Roland's app does. */
    EXPECT(ua101_eng_set_volume(&r.eng, IN, 5, 0x1000, r.now) == UA101_ENG_OK);
    EXPECT(ua101_eng_set_volume(&r.eng, IN, 6, 0x1000, r.now) == UA101_ENG_ERR_NO_CHANNEL);
    EXPECT(ua101_eng_set_pan(&r.eng, OUT, 9, 0x1000, r.now) == UA101_ENG_ERR_NO_CHANNEL);
    EXPECT(ua101_eng_set_monitor(&r.eng, OUT, 6, 0, r.now) == UA101_ENG_ERR_NO_CHANNEL);
    EXPECT(ua101_eng_set_link(&r.eng, IN, 2, 1, r.now) == UA101_ENG_OK);
    EXPECT(ua101_eng_set_link(&r.eng, IN, 3, 1, r.now) == UA101_ENG_ERR_NO_CHANNEL);
    EXPECT(ua101_eng_set_solo(&r.eng, OUT, 5, r.now) == UA101_ENG_OK);
    EXPECT(ua101_eng_set_solo(&r.eng, OUT, 6, r.now) == UA101_ENG_ERR_NO_CHANNEL);
    EXPECT(ua101_eng_toggle_solo(&r.eng, IN, 9, r.now) == UA101_ENG_ERR_NO_CHANNEL);
    EXPECT(ua101_eng_set_volume(&r.eng, MON, 1, 0x1000, r.now) == UA101_ENG_OK);
    settle(&r);
    EXPECT(memcmp(r.eng.want, r.sim.block, 256) == 0);
    rig_check(&r);
}

static void test_patch_presets(void)
{
    unsigned i;
    rig r;

    rig_init(&r, NULL);
    ua101_eng_transport(&r.eng, 1, r.now);
    settle(&r);
    ua101_eng_allow_writes(&r.eng, 1);
    /* Hard Thru and Initial Setting, as five ordinary changes: no memory command. */
    EXPECT(ua101_eng_patch_preset(&r.eng, 1, r.now) == UA101_ENG_OK);
    settle(&r);
    for (i = 0; i < 5; i++)
        EXPECT(unit_value(&r, UA101_F_PATCH, 0, (uint8_t)i) == 2 + i);
    EXPECT(count_kind(&r, UA101_MSG_FIELD, 0) == 5);
    EXPECT(ua101_eng_patch_preset(&r.eng, 0, r.now) == UA101_ENG_OK);
    settle(&r);
    for (i = 0; i < 5; i++)
        EXPECT(unit_value(&r, UA101_F_PATCH, 0, (uint8_t)i) == 7 + i);
    EXPECT(r.risk[UA101_RISK_MEMORY] == 0 && r.sim.memory_commands == 0);
    EXPECT(r.eng.disagreements == 0);
    rig_check(&r);
}

static void test_memory_commands(void)
{
    uint32_t ticket, old;
    ua101_msg m;
    rig r;

    rig_ready(&r, NULL);
    memset(&m, 0, sizeof(m));
    m.kind = UA101_MSG_STORE;
    m.memory = 2;
    m.sections[3] = 1;

    /* Writes alone don't allow it. */
    EXPECT(ua101_eng_memory_prepare(&r.eng, &m, r.now, &ticket) == UA101_ENG_ERR_READ_ONLY);
    EXPECT(ticket == 0);
    ua101_eng_allow_memory(&r.eng, 1);

    /* Only memory commands go this way, and only well-formed ones. */
    m.kind = UA101_MSG_FIELD;
    EXPECT(ua101_eng_memory_prepare(&r.eng, &m, r.now, &ticket) == UA101_ENG_ERR_VALUE);
    m.kind = UA101_MSG_STORE;
    m.memory = 5;
    EXPECT(ua101_eng_memory_prepare(&r.eng, &m, r.now, &ticket) == UA101_ENG_ERR_VALUE);
    m.memory = 2;
    m.sections[3] = 0;
    EXPECT(ua101_eng_memory_prepare(&r.eng, &m, r.now, &ticket) == UA101_ENG_ERR_VALUE);
    m.sections[3] = 1;

    /* Prepare sends nothing. */
    ua101_eng_set_volume(&r.eng, IN, 0, 0x1000, r.now);
    EXPECT(ua101_eng_memory_prepare(&r.eng, &m, r.now, &ticket) == UA101_ENG_OK && ticket != 0);
    EXPECT(r.risk[UA101_RISK_MEMORY] == 0);
    /* A wrong ticket voids the right one too. */
    EXPECT(ua101_eng_memory_confirm(&r.eng, ticket + 1, r.now) == UA101_ENG_ERR_TICKET);
    EXPECT(ua101_eng_memory_confirm(&r.eng, ticket, r.now) == UA101_ENG_ERR_TICKET);
    /* While a change is still being checked: wait, and the ticket is kept. */
    EXPECT(ua101_eng_memory_prepare(&r.eng, &m, r.now, &ticket) == UA101_ENG_OK);
    EXPECT(ua101_eng_memory_confirm(&r.eng, ticket, r.now) == UA101_ENG_ERR_BUSY);
    settle(&r);
    /* A ticket expires after 30 s. */
    advance(&r, r.eng.cfg.ticket_life_us);
    EXPECT(ua101_eng_memory_confirm(&r.eng, ticket, r.now) == UA101_ENG_ERR_TICKET);
    /* A new prepare replaces the old ticket. */
    EXPECT(ua101_eng_memory_prepare(&r.eng, &m, r.now, &old) == UA101_ENG_OK);
    EXPECT(ua101_eng_memory_prepare(&r.eng, &m, r.now, &ticket) == UA101_ENG_OK);
    EXPECT(ua101_eng_memory_confirm(&r.eng, old, r.now) == UA101_ENG_ERR_TICKET);
    EXPECT(ua101_eng_memory_prepare(&r.eng, &m, r.now, &ticket) == UA101_ENG_OK);
    /* Cancelled. */
    ua101_eng_memory_cancel(&r.eng);
    EXPECT(ua101_eng_memory_confirm(&r.eng, ticket, r.now) == UA101_ENG_ERR_TICKET);
    /* Permission withdrawn between the two steps. */
    EXPECT(ua101_eng_memory_prepare(&r.eng, &m, r.now, &ticket) == UA101_ENG_OK);
    ua101_eng_allow_memory(&r.eng, 0);
    ua101_eng_allow_memory(&r.eng, 1);
    EXPECT(ua101_eng_memory_confirm(&r.eng, ticket, r.now) == UA101_ENG_ERR_TICKET);
    EXPECT(r.risk[UA101_RISK_MEMORY] == 0 && r.sim.memory_commands == 0);

    /* Done properly: Store the mixer into Memory 3. One message, one use. */
    EXPECT(ua101_eng_memory_prepare(&r.eng, &m, r.now, &ticket) == UA101_ENG_OK);
    EXPECT(ua101_eng_memory_confirm(&r.eng, ticket, r.now) == UA101_ENG_OK);
    EXPECT(ua101_eng_memory_confirm(&r.eng, ticket, r.now) == UA101_ENG_ERR_TICKET);
    EXPECT(r.risk[UA101_RISK_MEMORY] == 1 && r.sim.memory_commands == 1);
    EXPECT(r.sim.last_memory_command.kind == UA101_MSG_STORE &&
           r.sim.last_memory_command.memory == 2 && r.sim.last_memory_command.sections[3] == 1 &&
           r.sim.last_memory_command.sections[2] == 0);
    settle(&r);

    /* Change things, then Load it back: the picture follows the unit. */
    ua101_eng_set_volume(&r.eng, IN, 0, 0x2000, r.now);
    ua101_eng_set_patch(&r.eng, 0, 2, r.now);
    settle(&r);
    m.kind = UA101_MSG_LOAD;
    EXPECT(ua101_eng_memory_prepare(&r.eng, &m, r.now, &ticket) == UA101_ENG_OK);
    EXPECT(ua101_eng_memory_confirm(&r.eng, ticket, r.now) == UA101_ENG_OK);
    settle(&r);
    EXPECT(ua101_eng_settings(&r.eng)->channel[IN][0].volume == 0x1000);
    EXPECT(ua101_eng_settings(&r.eng)->patch[1][0] == 2); /* the patch bay wasn't chosen */
    EXPECT(memcmp(r.eng.want, r.sim.block, 256) == 0 && r.eng.disagreements == 0);

    /* Reset all, then the preset command. */
    m.kind = UA101_MSG_RESET_ALL;
    EXPECT(ua101_eng_memory_prepare(&r.eng, &m, r.now, &ticket) == UA101_ENG_OK);
    EXPECT(ua101_eng_memory_confirm(&r.eng, ticket, r.now) == UA101_ENG_OK);
    settle(&r);
    EXPECT(ua101_eng_settings(&r.eng)->channel[IN][0].volume == UNITY);
    EXPECT(ua101_eng_settings(&r.eng)->patch[1][0] == 7);
    EXPECT(ua101_eng_settings(&r.eng)->soft_control == 1); /* a switch isn't a setting */
    EXPECT(r.risk[UA101_RISK_MEMORY] == 3 && r.eng.memory_sent == 3);

    /* Reset of the settings in use: the patch bay and the mixer go back, the memories stay. */
    ua101_eng_set_volume(&r.eng, IN, 0, 0x3000, r.now);
    ua101_eng_set_patch(&r.eng, 0, 2, r.now);
    settle(&r);
    m.kind = UA101_MSG_STORE;
    m.memory = 4;
    m.sections[2] = m.sections[3] = 1;
    EXPECT(ua101_eng_memory_prepare(&r.eng, &m, r.now, &ticket) == UA101_ENG_OK);
    EXPECT(ua101_eng_memory_confirm(&r.eng, ticket, r.now) == UA101_ENG_OK);
    settle(&r);
    memset(&m, 0, sizeof(m));
    m.kind = UA101_MSG_RESET_CURRENT;
    EXPECT(ua101_eng_memory_prepare(&r.eng, &m, r.now, &ticket) == UA101_ENG_OK);
    EXPECT(ua101_eng_memory_confirm(&r.eng, ticket, r.now) == UA101_ENG_OK);
    settle(&r);
    EXPECT(r.sim.last_memory_command.kind == UA101_MSG_RESET_CURRENT);
    EXPECT(ua101_eng_settings(&r.eng)->channel[IN][0].volume == UNITY);
    EXPECT(ua101_eng_settings(&r.eng)->patch[1][0] == 7);
    EXPECT(ua101_eng_settings(&r.eng)->soft_control == 1);
    EXPECT(memcmp(r.eng.want, r.sim.block, 256) == 0 && r.eng.disagreements == 0);
    {
        ua101_field f = {UA101_F_VOLUME, IN, 0}, p = {UA101_F_PATCH, 0, 0};

        EXPECT(ua101_block_get(r.sim.memory[4], f) == 0x3000 && ua101_block_get(r.sim.memory[4], p) == 2);
    }
    EXPECT(r.risk[UA101_RISK_MEMORY] == 5 && r.eng.memory_sent == 5);

    /* The guard itself: a memory message can't get out by the ordinary door. */
    ua101_eng_allow_memory(&r.eng, 0);
    EXPECT(ua101_eng_memory_prepare(&r.eng, &m, r.now, &ticket) == UA101_ENG_ERR_READ_ONLY);
    EXPECT(r.risk[UA101_RISK_MEMORY] == 5);
    rig_check(&r);
}

int ua101_eng_test_emit(ua101_engine *e, const uint8_t *msg, size_t len, uint64_t now);

/* The guard by itself: bytes handed straight to the way out, as a bug in the
 * engine might do. Only what is allowed at that moment gets through. */
static void test_guard(void)
{
    uint8_t read[UA101_CTLMSG_MAX_BYTES], set[UA101_CTLMSG_MAX_BYTES], mem[UA101_CTLMSG_MAX_BYTES];
    uint8_t avoid[13];
    size_t read_len, set_len, mem_len;
    unsigned before;
    ua101_msg m;
    rig r;

    memset(&m, 0, sizeof(m));
    m.kind = UA101_MSG_READ_BLOCK;
    read_len = ua101_ctlmsg_build(&m, read);
    m.kind = UA101_MSG_FIELD;
    m.field = fld(UA101_F_PATCH, 0, 0);
    m.value = 2;
    set_len = ua101_ctlmsg_build(&m, set);
    m.kind = UA101_MSG_RESET_ALL;
    mem_len = ua101_ctlmsg_build(&m, mem);
    raw_set(avoid, 0, 0, 0, 1, 0);

    rig_init(&r, NULL);
    /* Not connected: nothing at all. */
    EXPECT(!ua101_eng_test_emit(&r.eng, read, read_len, r.now));
    ua101_eng_transport(&r.eng, 1, r.now);
    settle(&r);
    before = r.sent_count;
    /* Read-only: a read passes, a change and a memory command don't. */
    EXPECT(ua101_eng_test_emit(&r.eng, read, read_len, r.now));
    EXPECT(!ua101_eng_test_emit(&r.eng, set, set_len, r.now));
    EXPECT(!ua101_eng_test_emit(&r.eng, mem, mem_len, r.now));
    /* Writes allowed: a change passes; a memory command still doesn't. */
    ua101_eng_allow_writes(&r.eng, 1);
    EXPECT(ua101_eng_test_emit(&r.eng, set, set_len, r.now));
    EXPECT(!ua101_eng_test_emit(&r.eng, mem, mem_len, r.now));
    /* Memory allowed too: still not without a confirmed ticket. */
    ua101_eng_allow_memory(&r.eng, 1);
    EXPECT(!ua101_eng_test_emit(&r.eng, mem, mem_len, r.now));
    /* Memory allowed but writes not: no. */
    ua101_eng_allow_writes(&r.eng, 0);
    EXPECT(!ua101_eng_test_emit(&r.eng, mem, mem_len, r.now));
    EXPECT(!ua101_eng_test_emit(&r.eng, set, set_len, r.now));
    /* Never: the notes' "commands to avoid", damaged bytes, nothing. */
    ua101_eng_allow_writes(&r.eng, 1);
    EXPECT(!ua101_eng_test_emit(&r.eng, avoid, sizeof(avoid), r.now));
    set[11] ^= 1;
    EXPECT(!ua101_eng_test_emit(&r.eng, set, set_len, r.now));
    EXPECT(!ua101_eng_test_emit(&r.eng, set, 0, r.now));
    EXPECT(r.eng.guard_blocks == 10);
    EXPECT(r.sent_count == before + 2 && r.risk[UA101_RISK_MEMORY] == 0 &&
           r.risk[UA101_RISK_NEVER] == 0 && r.sim.memory_commands == 0);
    EXPECT(unit_value(&r, UA101_F_PATCH, 0, 0) == 2);
}

/* A long random run. Whatever is asked in whatever order, the rules hold, and
 * once things go quiet the picture and the unit are the same. */
static unsigned g_seed;
static unsigned g_memory_commands;

static unsigned rnd(unsigned n)
{
    g_seed = g_seed * 1664525u + 1013904223u;
    return (g_seed >> 8) % n;
}

static void random_run(unsigned seed, int writes, int memory_allowed, unsigned steps)
{
    static const ua101_sim_switch k_switch[] = {UA101_SIM_SWITCH_SOFT, UA101_SIM_SWITCH_MONO,
                                                UA101_SIM_SWITCH_DIGITAL};
    ua101_eng_config cfg;
    unsigned i, drags = 0;
    uint32_t ticket = 0;
    ua101_msg m;
    rig r;

    g_seed = seed;
    ua101_eng_default_config(&cfg);
    rig_init(&r, &cfg);
    ua101_sim_switch_move(&r.sim, UA101_SIM_SWITCH_SOFT, 1, 0);
    ua101_eng_transport(&r.eng, 1, r.now);
    ua101_eng_allow_writes(&r.eng, writes);
    ua101_eng_allow_memory(&r.eng, memory_allowed);

    for (i = 0; i < steps; i++) {
        unsigned g = rnd(4), c = rnd(11);

        switch (rnd(16)) {
        case 0:
        case 1:
        case 2:
            ua101_eng_set_volume(&r.eng, g, c, rnd(4) ? rnd(UNITY + 1) : rnd(0x900000), r.now);
            break;
        case 3:
        case 4:
            ua101_eng_set_pan(&r.eng, g, c, rnd(0x8003), r.now);
            break;
        case 5:
            ua101_eng_set_monitor(&r.eng, g, c, (int)rnd(2), r.now);
            break;
        case 6:
            ua101_eng_set_link(&r.eng, g, rnd(6), (int)rnd(2), r.now);
            break;
        case 7:
            ua101_eng_set_patch(&r.eng, rnd(6), rnd(13), r.now);
            break;
        case 8:
            if (rnd(2))
                ua101_eng_set_solo(&r.eng, g, (int)rnd(12) - 1, r.now);
            else
                ua101_eng_patch_preset(&r.eng, (int)rnd(2), r.now);
            break;
        case 9:
            if (drags < 3 && rnd(2)) {
                ua101_eng_drag_begin(&r.eng, r.now);
                drags++;
            } else {
                ua101_eng_drag_end(&r.eng, r.now);
                drags -= drags > 0;
            }
            break;
        case 10:
            if (!rnd(6))
                ua101_sim_switch_move(&r.sim, k_switch[rnd(3)], (int)rnd(2), rnd(8));
            else
                ua101_eng_refresh(&r.eng, r.now);
            break;
        case 11:
            r.hold_replies = (int)rnd(2);
            break;
        case 12:
            memset(&m, 0, sizeof(m));
            m.kind = (ua101_msg_kind)(UA101_MSG_RESET_ALL + rnd(6));
            m.memory = (uint8_t)rnd(6);
            m.value = 3 + rnd(2);
            m.sections[1 + rnd(3)] = 1;
            ua101_eng_memory_prepare(&r.eng, &m, r.now, &ticket);
            break;
        case 13:
            /* Never confirmed in this run unless memory is allowed. */
            ua101_eng_memory_confirm(&r.eng, rnd(3) ? ticket : ticket + 1, r.now);
            break;
        default:
            advance(&r, rnd(3) ? rnd(3000) : rnd(900000));
            break;
        }
    }
    while (drags--)
        ua101_eng_drag_end(&r.eng, r.now);
    r.hold_replies = 0;
    settle(&r);

    EXPECT(r.eng.state == UA101_ENG_READY && !ua101_eng_busy(&r.eng));
    EXPECT(memcmp(r.eng.want, r.sim.block, 256) == 0);
    EXPECT(memcmp(r.eng.unit, r.sim.block, 256) == 0);
    EXPECT(r.eng.disagreements == 0);
    EXPECT(r.sim.saving_paused == 0);
    EXPECT(r.sim.sets_ignored == 0);
    if (!writes) {
        EXPECT(r.risk[UA101_RISK_SETTING] == 0);
        EXPECT(memcmp(r.sim.block + 7, ua101_sim_factory_block() + 7, 249) == 0);
    } else {
        EXPECT(r.risk[UA101_RISK_SETTING] > steps / 20);
    }
    if (!writes || !memory_allowed)
        EXPECT(r.risk[UA101_RISK_MEMORY] == 0 && r.sim.memory_commands == 0);
    else
        g_memory_commands += r.risk[UA101_RISK_MEMORY];
    EXPECT(r.risk[UA101_RISK_MEMORY] == r.sim.memory_commands);
    /* Never above the ceiling (the top of the range), whatever was asked. */
    for (i = 0; i < 22; i++)
        EXPECT(ua101_block_get(r.sim.block, ua101_field_from_id(i)) <= UA101_MAX_VOLUME);
    rig_check(&r);
}

static void test_random(void)
{
    unsigned seed;

    for (seed = 1; seed <= 40; seed++) {
        random_run(seed, 0, 0, 3000);
        random_run(seed + 100, 1, 0, 3000);
        random_run(seed + 200, 1, 1, 3000);
        random_run(seed + 300, 0, 1, 1000); /* memory allowed without writes: still nothing */
    }
    EXPECT(g_memory_commands > 20); /* the runs that may use them did */
}

int main(void)
{
    test_examples();
    test_fields();
    test_risk();
    test_sim();
    test_connect_and_read_only();
    test_soft_control_gate();
    test_write_and_verify();
    test_volume_ceiling();
    test_link();
    test_solo();
    test_drag();
    test_saving_alone();
    test_coalescing_keeps_order_and_last_value();
    test_disagreement();
    test_stale_reply();
    test_switch_bursts();
    test_soft_control_released_mid_change();
    test_silent_unit();
    test_bad_reply();
    test_transport_loss();
    test_192k();
    test_patch_presets();
    test_memory_commands();
    test_guard();
    test_random();

    printf("%d checks, %d failed\n", g_tests, g_failures);
    return g_failures ? 1 : 0;
}
