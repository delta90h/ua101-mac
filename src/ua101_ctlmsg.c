/* See ua101_ctlmsg.h. Nothing here has been tried on a real unit.
 *
 * This file reads and classes messages and knows the settings block. The code
 * that MAKES a message is in ua101_ctlbuild.c, so the helper (ua101d), which
 * classes what a control app hands it, can link this file and still holds no
 * code that builds a message that sets something. */
#include "ua101_ctlmsg.h"

#include <string.h>

#define CMD_SET 0x12

/* Where each group's fields start in the block. Group 0 has no pan and no solo.
 * tests/test_ctlengine.c checks these against the parser in ua101_control.c. */
static const uint8_t k_link_base[UA101_CTL_GROUPS] = {0x18, 0x19, 0x1B};
static const uint8_t k_on_base[UA101_CTL_GROUPS] = {0x1D, 0x1E, 0x21};
static const uint8_t k_pan_base[UA101_CTL_GROUPS] = {0, 0x24, 0x4C};
static const uint8_t k_volume_base[UA101_CTL_GROUPS] = {0x74, 0x80, 0xBC};
#define SOLO_BASE 0x16
#define PATCH_ROW1_BASE 12

static int field_exists(ua101_field f)
{
    switch (f.kind) {
    case UA101_F_VOLUME:
    case UA101_F_ON:
        return f.group < UA101_CTL_GROUPS && f.index < ua101_ctl_group_channels(f.group);
    case UA101_F_PAN:
        return (f.group == 1 || f.group == 2) && f.index < UA101_CTL_MAX_CHANNELS;
    case UA101_F_LINK:
        return f.group < UA101_CTL_GROUPS && f.index < ua101_ctl_group_channels(f.group) / 2;
    case UA101_F_SOLO:
        return (f.group == 1 || f.group == 2) && f.index == 0;
    case UA101_F_PATCH:
        return f.group == 0 && f.index < UA101_CTL_PATCH_OUTPUTS;
    default:
        return 0;
    }
}

/* Channels of all groups in one row: 2 monitor, 10 input, 10 output. */
static unsigned flat_channel(ua101_field f)
{
    return f.group == 0 ? f.index : 2u + (f.group - 1u) * 10u + f.index;
}

int ua101_field_id(ua101_field f)
{
    if (!field_exists(f))
        return -1;
    switch (f.kind) {
    case UA101_F_VOLUME:
        return (int)flat_channel(f);
    case UA101_F_PAN:
        return 22 + (int)((f.group - 1u) * 10u + f.index);
    case UA101_F_ON:
        return 42 + (int)flat_channel(f);
    case UA101_F_LINK:
        return 64 + (int)(f.group == 0 ? 0u : 1u + (f.group - 1u) * 5u + f.index);
    case UA101_F_SOLO:
        return 75 + (int)(f.group - 1u);
    default:
        return 77 + (int)f.index;
    }
}

ua101_field ua101_field_from_id(unsigned id)
{
    ua101_field f = {0, 0, 0};

    if (id < 22 || (id >= 42 && id < 64)) {
        unsigned c = id < 22 ? id : id - 42;

        f.kind = id < 22 ? UA101_F_VOLUME : UA101_F_ON;
        f.group = (uint8_t)(c < 2 ? 0 : 1 + (c - 2) / 10);
        f.index = (uint8_t)(c < 2 ? c : (c - 2) % 10);
    } else if (id < 42) {
        f.kind = UA101_F_PAN;
        f.group = (uint8_t)(1 + (id - 22) / 10);
        f.index = (uint8_t)((id - 22) % 10);
    } else if (id < 75) {
        unsigned p = id - 64;

        f.kind = UA101_F_LINK;
        f.group = (uint8_t)(p == 0 ? 0 : 1 + (p - 1) / 5);
        f.index = (uint8_t)(p == 0 ? 0 : (p - 1) % 5);
    } else if (id < 77) {
        f.kind = UA101_F_SOLO;
        f.group = (uint8_t)(1 + (id - 75));
    } else {
        f.kind = UA101_F_PATCH;
        f.index = (uint8_t)(id - 77);
    }
    return f;
}

int ua101_field_is_mixer(ua101_field f)
{
    return f.kind != UA101_F_PATCH;
}

int ua101_field_value_ok(ua101_field f, uint32_t value)
{
    if (!field_exists(f))
        return 0;
    switch (f.kind) {
    case UA101_F_VOLUME:
        return value <= UA101_MAX_VOLUME;
    case UA101_F_PAN:
        return value <= UA101_MAX_PAN;
    case UA101_F_ON:
    case UA101_F_LINK:
        return value <= 1;
    case UA101_F_SOLO:
        return value <= UA101_MAX_SOLO;
    default:
        return value >= UA101_PATCH_SOURCE_MIN && value <= UA101_PATCH_SOURCE_MAX;
    }
}

static uint32_t get_nibbles(const uint8_t *b, unsigned count)
{
    uint32_t v = 0;
    unsigned i;

    for (i = 0; i < count; i++)
        v = (v << 4) | (b[i] & 0x0Fu);
    return v;
}

static void put_nibbles(uint8_t *b, unsigned count, uint32_t v)
{
    unsigned i;

    for (i = 0; i < count; i++)
        b[i] = (uint8_t)((v >> (4 * (count - 1 - i))) & 0x0Fu);
}

uint32_t ua101_block_get(const uint8_t block[UA101_CTL_BLOCK_BYTES], ua101_field f)
{
    if (!field_exists(f))
        return 0;
    switch (f.kind) {
    case UA101_F_VOLUME:
        return get_nibbles(block + k_volume_base[f.group] + f.index * 6u, 6);
    case UA101_F_PAN:
        return get_nibbles(block + k_pan_base[f.group] + f.index * 4u, 4);
    case UA101_F_ON:
        return (block[k_on_base[f.group] + f.index / 4u] >> (f.index & 3u)) & 1u;
    case UA101_F_LINK:
        return (block[k_link_base[f.group] + f.index / 4u] >> (f.index & 3u)) & 1u;
    case UA101_F_SOLO:
        return block[SOLO_BASE + f.group - 1u];
    default:
        return block[PATCH_ROW1_BASE + f.index];
    }
}

static void put_bit(uint8_t *byte, unsigned bit, uint32_t value)
{
    *byte = (uint8_t)((*byte & ~(1u << bit)) | ((value & 1u) << bit));
}

void ua101_block_put(uint8_t block[UA101_CTL_BLOCK_BYTES], ua101_field f, uint32_t value)
{
    if (!field_exists(f))
        return;
    switch (f.kind) {
    case UA101_F_VOLUME:
        put_nibbles(block + k_volume_base[f.group] + f.index * 6u, 6, value);
        break;
    case UA101_F_PAN:
        put_nibbles(block + k_pan_base[f.group] + f.index * 4u, 4, value);
        break;
    case UA101_F_ON:
        put_bit(&block[k_on_base[f.group] + f.index / 4u], f.index & 3u, value);
        break;
    case UA101_F_LINK:
        put_bit(&block[k_link_base[f.group] + f.index / 4u], f.index & 3u, value);
        break;
    case UA101_F_SOLO:
        block[SOLO_BASE + f.group - 1u] = (uint8_t)value;
        break;
    default:
        block[PATCH_ROW1_BASE + f.index] = (uint8_t)value;
        break;
    }
}

int ua101_ctlmsg_sections_ok(const ua101_msg *m)
{
    unsigned i, any = 0;

    if (m->memory >= UA101_MEMORIES)
        return 0;
    for (i = 0; i < 4; i++) {
        if (m->sections[i] > 1)
            return 0;
        any |= m->sections[i];
    }
    return any != 0;
}

static int nibbles_ok(const uint8_t *b, size_t count)
{
    size_t i;

    for (i = 0; i < count; i++)
        if (b[i] > 0x0F)
            return 0;
    return 1;
}

static ua101_msg_kind parse_set(const ua101_ctl_view *v, ua101_msg *out)
{
    const uint8_t *a = v->address, *d = v->data;
    size_t n = v->data_len;
    unsigned i;

    if (a[0] != 0)
        return UA101_MSG_UNKNOWN;

    if (a[1] == 0) { /* saving and the stored memories */
        if (a[2] == 0 && a[3] == 0 && n == 1 && d[0] <= 1) {
            out->value = d[0];
            return UA101_MSG_AUTOSAVE;
        }
        if (a[2] == 0 && a[3] == 2 && n == 1 && d[0] == 0)
            return UA101_MSG_RESET_ALL;
        /* Roland's "reset one" with the number 0 in place of a memory's 3 to 7: the settings
         * in use ("Current Setting" in its Reset dialog). */
        if (a[2] == 0 && a[3] == 3 && n == 1 && d[0] == 0)
            return UA101_MSG_RESET_CURRENT;
        if (a[2] >= 3 && a[2] < 3 + UA101_MEMORIES) {
            out->memory = (uint8_t)(a[2] - 3);
            if (a[3] == 3 && n == 1 && d[0] == 0)
                return UA101_MSG_RESET_MEMORY;
            if ((a[3] == 4 || a[3] == 8) && n == 4) {
                uint8_t chosen = a[3] == 4 ? 2 : 1;

                for (i = 0; i < 4; i++) {
                    if (d[i] != 0 && d[i] != chosen)
                        return UA101_MSG_UNKNOWN;
                    out->sections[i] = d[i] != 0;
                }
                if (!ua101_ctlmsg_sections_ok(out))
                    return UA101_MSG_UNKNOWN;
                return a[3] == 4 ? UA101_MSG_STORE : UA101_MSG_LOAD;
            }
        }
        if (a[2] == 0 && a[3] == 8 && n == 4 && d[0] == 0 && d[1] == 0 && d[3] == 0 &&
            (d[2] == 3 || d[2] == 4)) {
            out->value = d[2];
            return UA101_MSG_PRESET;
        }
        return UA101_MSG_UNKNOWN;
    }

    if (a[1] == 2 && a[2] == 1) {
        out->field.kind = UA101_F_PATCH;
        out->field.group = 0;
        out->field.index = a[3];
        out->value = n == 1 ? d[0] : 0xFFFFFFFFu;
    } else if (a[1] >= 3 && a[1] <= 5) {
        out->field.group = (uint8_t)(a[1] - 3);
        out->field.index = a[2];
        switch (a[3]) {
        case 0x07:
            if (n != 6 || !nibbles_ok(d, 6))
                return UA101_MSG_UNKNOWN;
            out->field.kind = UA101_F_VOLUME;
            out->value = get_nibbles(d, 6);
            break;
        case 0x03:
            if (n != 4 || !nibbles_ok(d, 4))
                return UA101_MSG_UNKNOWN;
            out->field.kind = UA101_F_PAN;
            out->value = get_nibbles(d, 4);
            break;
        case 0x02:
        case 0x01:
        case 0x00:
            if (n != 1 || (a[3] == 0x01 && (a[2] & 1)))
                return UA101_MSG_UNKNOWN;
            out->field.kind = a[3] == 2 ? UA101_F_ON : a[3] == 1 ? UA101_F_LINK : UA101_F_SOLO;
            if (a[3] == 0x01)
                out->field.index = (uint8_t)(a[2] / 2);
            out->value = d[0];
            break;
        default:
            return UA101_MSG_UNKNOWN;
        }
    } else {
        return UA101_MSG_UNKNOWN;
    }
    return ua101_field_value_ok(out->field, out->value) ? UA101_MSG_FIELD : UA101_MSG_UNKNOWN;
}

ua101_msg_kind ua101_ctlmsg_parse(const uint8_t *bytes, size_t len, ua101_msg *out)
{
    uint8_t request[UA101_CTL_READ_REQUEST_BYTES];
    ua101_ctl_view view;
    ua101_ctl_kind kind;

    memset(out, 0, sizeof(*out));
    ua101_ctl_read_request(request);
    if (len == sizeof(request) && memcmp(bytes, request, len) == 0) {
        out->kind = UA101_MSG_READ_BLOCK;
        return out->kind;
    }
    kind = ua101_ctl_classify(bytes, len, &view);
    if (kind == UA101_CTL_BAD_FORMAT || kind == UA101_CTL_BAD_CHECKSUM)
        return UA101_MSG_INVALID;
    out->kind = view.command == CMD_SET ? parse_set(&view, out) : UA101_MSG_UNKNOWN;
    return out->kind;
}

ua101_risk ua101_ctlmsg_risk(const uint8_t *bytes, size_t len)
{
    ua101_msg m;

    switch (ua101_ctlmsg_parse(bytes, len, &m)) {
    case UA101_MSG_READ_BLOCK:
        return UA101_RISK_READ;
    case UA101_MSG_FIELD:
    case UA101_MSG_AUTOSAVE:
        return UA101_RISK_SETTING;
    case UA101_MSG_RESET_ALL:
    case UA101_MSG_RESET_CURRENT:
    case UA101_MSG_RESET_MEMORY:
    case UA101_MSG_STORE:
    case UA101_MSG_LOAD:
    case UA101_MSG_PRESET:
        return UA101_RISK_MEMORY;
    default:
        return UA101_RISK_NEVER;
    }
}
