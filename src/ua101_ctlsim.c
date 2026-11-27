/* See ua101_ctlsim.h, above all for what this does and doesn't prove. */
#include "ua101_ctlsim.h"

#include <string.h>

/* The block the real unit returned (settings-output-48k.txt, "Raw settings block"). */
static const uint8_t k_factory[UA101_CTL_BLOCK_BYTES] = {
    0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a,
    0x0b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x0f, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x08, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

/* The block's sections, as the four flags at its start name them. */
#define SWITCHES_FIRST 4
#define SWITCHES_END 7
#define PATCH0_END 12
#define PATCH12_END 22
#define MIXER_END 248

const uint8_t *ua101_sim_factory_block(void)
{
    return k_factory;
}

void ua101_sim_init(ua101_sim *s)
{
    unsigned m;

    memset(s, 0, sizeof(*s));
    memcpy(s->block, k_factory, sizeof(s->block));
    for (m = 0; m < UA101_MEMORIES; m++)
        memcpy(s->memory[m], k_factory, sizeof(s->block));
}

static void queue(ua101_sim *s, const uint8_t *msg, size_t len)
{
    unsigned slot;

    if (s->silent)
        return;
    if (s->out_count == UA101_SIM_QUEUE || len > sizeof(s->out[0])) {
        s->dropped++;
        return;
    }
    slot = (s->out_head + s->out_count++) % UA101_SIM_QUEUE;
    memcpy(s->out[slot], msg, len);
    s->out_len[slot] = len;
}

int ua101_sim_next(ua101_sim *s, const uint8_t **msg, size_t *len)
{
    if (!s->out_count)
        return 0;
    *msg = s->out[s->out_head];
    *len = s->out_len[s->out_head];
    s->out_head = (s->out_head + 1) % UA101_SIM_QUEUE;
    s->out_count--;
    return 1;
}

static void reply(ua101_sim *s, const uint8_t address[4], const uint8_t *data, size_t len)
{
    uint8_t msg[UA101_CTL_BLOCK_MESSAGE_BYTES] = {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12};

    memcpy(msg + 6, address, 4);
    memcpy(msg + 10, data, len);
    msg[10 + len] = ua101_ctl_checksum(msg + 6, 4 + len);
    if (len == UA101_CTL_BLOCK_BYTES && s->corrupt_next_reply) {
        s->corrupt_next_reply = 0;
        msg[10 + len] ^= 0x01;
    }
    msg[11 + len] = 0xF7;
    queue(s, msg, 12 + len);
}

/* Copies the chosen sections from one block to another. Never the switches. */
static void copy_sections(uint8_t *to, const uint8_t *from, const uint8_t sections[4])
{
    if (sections[1])
        memcpy(to + SWITCHES_END, from + SWITCHES_END, PATCH0_END - SWITCHES_END);
    if (sections[2])
        memcpy(to + PATCH0_END, from + PATCH0_END, PATCH12_END - PATCH0_END);
    if (sections[3])
        memcpy(to + PATCH12_END, from + PATCH12_END, MIXER_END - PATCH12_END);
}

static void saved(ua101_sim *s)
{
    if (s->saving_paused)
        s->unsaved = 1;
    else
        s->saves++;
}

void ua101_sim_receive(ua101_sim *s, const uint8_t *msg, size_t len)
{
    static const uint8_t k_block_address[4] = {0x02, 0, 0, 0};
    static const uint8_t k_all[4] = {1, 1, 1, 1};
    ua101_msg m;
    unsigned i;

    switch (ua101_ctlmsg_parse(msg, len, &m)) {
    case UA101_MSG_INVALID:
        s->bad_messages++;
        return;
    case UA101_MSG_READ_BLOCK:
        s->reads++;
        reply(s, k_block_address, s->block, sizeof(s->block));
        return;
    case UA101_MSG_READ_MEMORY: {
        /* As the real unit answers (box test 2026-11-27): the memory's 256 bytes, from
           02 03+m 00 00, in the settings block's layout. */
        const uint8_t address[4] = {0x02, (uint8_t)(UA101_CTL_MEMORY_BLOCK_FIRST + m.memory), 0, 0};

        s->memory_reads++;
        reply(s, address, s->memory[m.memory], sizeof(s->block));
        return;
    }
    case UA101_MSG_FIELD:
        if (s->ignore_kind_plus_1 == m.field.kind + 1 ||
            (s->ignore_mixer_without_soft_control && ua101_field_is_mixer(m.field) &&
             !(s->block[6] & 0x04))) {
            s->sets_ignored++;
            return;
        }
        ua101_block_put(s->block, m.field, m.value);
        s->sets_applied++;
        saved(s);
        return;
    case UA101_MSG_AUTOSAVE:
        s->saving_paused = m.value == 0;
        if (!s->saving_paused && s->unsaved) {
            s->unsaved = 0;
            s->saves++;
        }
        return;
    case UA101_MSG_RESET_ALL:
        copy_sections(s->block, k_factory, k_all);
        for (i = 0; i < UA101_MEMORIES; i++)
            memcpy(s->memory[i], k_factory, sizeof(s->block));
        break;
    case UA101_MSG_RESET_CURRENT:
        /* A guess, as the rest: the settings in use go back to the factory ones, the
         * memories stay. */
        copy_sections(s->block, k_factory, k_all);
        break;
    case UA101_MSG_RESET_MEMORY:
        memcpy(s->memory[m.memory], k_factory, sizeof(s->block));
        break;
    case UA101_MSG_STORE:
        copy_sections(s->memory[m.memory], s->block, m.sections);
        break;
    case UA101_MSG_LOAD:
        copy_sections(s->block, s->memory[m.memory], m.sections);
        break;
    case UA101_MSG_PRESET:
        for (i = 0; i < UA101_CTL_PATCH_OUTPUTS; i++)
            s->block[12 + i] = (uint8_t)(i + (m.value == 4 ? UA101_PATCH_HARD_THRU_FIRST
                                                            : UA101_PATCH_INITIAL_FIRST));
        break;
    default:
        s->unknown_messages++;
        return;
    }
    s->memory_commands++;
    s->last_memory_command = m;
    s->saves++;
}

void ua101_sim_switch_move(ua101_sim *s, ua101_sim_switch which, int in, unsigned messages)
{
    uint8_t address[4] = {0x00, 0x01, 0x00, (uint8_t)which};
    uint8_t on_value = which == UA101_SIM_SWITCH_DIGITAL ? 0x09 : 0x01;
    unsigned i;

    in = in != 0;
    if (which == UA101_SIM_SWITCH_DIGITAL)
        s->block[5] = in ? 0x09 : 0x00; /* switch in, no source: as seen on the unit */
    else if (which == UA101_SIM_SWITCH_MONO)
        s->block[6] = (uint8_t)((s->block[6] & ~0x02) | (in << 1));
    else
        s->block[6] = (uint8_t)((s->block[6] & ~0x04) | (in << 2));

    for (i = 0; i < messages; i++) {
        int value = ((messages - 1 - i) & 1) ? !in : in;
        uint8_t byte = value ? on_value : 0x00;

        reply(s, address, &byte, 1);
    }
}

void ua101_sim_set_rate_code(ua101_sim *s, unsigned code)
{
    s->block[4] = (uint8_t)((s->block[4] & ~0x07) | (code & 0x07));
    s->out_head = s->out_count = 0;
    s->saving_paused = s->unsaved = 0;
}
