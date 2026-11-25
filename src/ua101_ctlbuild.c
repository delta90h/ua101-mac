/* The write side's builder: ua101_ctlmsg_build. See ua101_ctlmsg.h.
 * Nothing here has been tried on a real unit.
 *
 * Kept apart from ua101_ctlmsg.c on purpose: the helper (ua101d) links that
 * file to class messages, and must not link this one (the Makefile checks). */
#include "ua101_ctlmsg.h"

#include <string.h>

#define CMD_SET 0x12

static void put_nibbles(uint8_t *b, unsigned count, uint32_t v)
{
    unsigned i;

    for (i = 0; i < count; i++)
        b[i] = (uint8_t)((v >> (4 * (count - 1 - i))) & 0x0Fu);
}

static size_t finish(uint8_t *out, const uint8_t address[4], const uint8_t *payload,
                     size_t payload_len)
{
    static const uint8_t k_header[] = {0xF0, 0x41, 0x10, 0x00, 0x69, CMD_SET};
    size_t n = sizeof(k_header);

    memcpy(out, k_header, n);
    memcpy(out + n, address, 4);
    memcpy(out + n + 4, payload, payload_len);
    out[n + 4 + payload_len] = ua101_ctl_checksum(out + n, 4 + payload_len);
    out[n + 5 + payload_len] = 0xF7;
    return n + 6 + payload_len;
}

size_t ua101_ctlmsg_build(const ua101_msg *m, uint8_t out[UA101_CTLMSG_MAX_BYTES])
{
    uint8_t a[4] = {0, 0, 0, 0};
    uint8_t p[6] = {0, 0, 0, 0, 0, 0};
    unsigned i;

    switch (m->kind) {
    case UA101_MSG_READ_BLOCK:
        ua101_ctl_read_request(out);
        return UA101_CTL_READ_REQUEST_BYTES;
    case UA101_MSG_FIELD:
        if (!ua101_field_value_ok(m->field, m->value))
            return 0;
        a[1] = (uint8_t)(m->field.group + 3);
        a[2] = m->field.index;
        switch (m->field.kind) {
        case UA101_F_VOLUME:
            a[3] = 0x07;
            put_nibbles(p, 6, m->value);
            return finish(out, a, p, 6);
        case UA101_F_PAN:
            a[3] = 0x03;
            put_nibbles(p, 4, m->value);
            return finish(out, a, p, 4);
        case UA101_F_ON:
            a[3] = 0x02;
            break;
        case UA101_F_LINK:
            a[2] = (uint8_t)(m->field.index * 2);
            a[3] = 0x01;
            break;
        case UA101_F_SOLO:
            a[2] = 0;
            a[3] = 0x00;
            break;
        default:
            a[1] = 0x02;
            a[2] = 0x01;
            a[3] = m->field.index;
            break;
        }
        p[0] = (uint8_t)m->value;
        return finish(out, a, p, 1);
    case UA101_MSG_AUTOSAVE:
        if (m->value > 1)
            return 0;
        p[0] = (uint8_t)m->value;
        return finish(out, a, p, 1);
    case UA101_MSG_RESET_ALL:
        a[3] = 0x02;
        return finish(out, a, p, 1);
    case UA101_MSG_RESET_CURRENT:
        a[3] = 0x03;
        return finish(out, a, p, 1);
    case UA101_MSG_RESET_MEMORY:
        if (m->memory >= UA101_MEMORIES)
            return 0;
        a[2] = (uint8_t)(m->memory + 3);
        a[3] = 0x03;
        return finish(out, a, p, 1);
    case UA101_MSG_STORE:
    case UA101_MSG_LOAD:
        if (!ua101_ctlmsg_sections_ok(m))
            return 0;
        a[2] = (uint8_t)(m->memory + 3);
        a[3] = m->kind == UA101_MSG_STORE ? 0x04 : 0x08;
        /* A chosen section is 2 in a store and 1 in a load. */
        for (i = 0; i < 4; i++)
            p[i] = (uint8_t)(m->sections[i] * (m->kind == UA101_MSG_STORE ? 2 : 1));
        return finish(out, a, p, 4);
    case UA101_MSG_PRESET:
        if (m->value != 3 && m->value != 4)
            return 0;
        a[3] = 0x08;
        p[2] = (uint8_t)m->value;
        return finish(out, a, p, 4);
    default:
        return 0;
    }
}
