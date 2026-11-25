#include "ua101_control.h"

#include <math.h>
#include <string.h>

#define CMD_REQUEST 0x11
#define CMD_SET 0x12
#define HEADER_BYTES 6
#define ADDRESS_BYTES 4

static const uint8_t k_header[HEADER_BYTES - 1] = {0xF0, 0x41, 0x10, 0x00, 0x69};

uint8_t ua101_ctl_checksum(const uint8_t *bytes, size_t count)
{
    unsigned sum = 0;
    size_t i;

    for (i = 0; i < count; i++)
        sum += bytes[i];
    return (uint8_t)((0u - sum) & 0x7F);
}

void ua101_ctl_read_request(uint8_t out[UA101_CTL_READ_REQUEST_BYTES])
{
    /* Address 02 00 00 00, size 00 00 02 00, which is 256 in 7-bit bytes. */
    static const uint8_t k_body[8] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00};

    memcpy(out, k_header, sizeof(k_header));
    out[5] = CMD_REQUEST;
    memcpy(out + HEADER_BYTES, k_body, sizeof(k_body));
    out[14] = ua101_ctl_checksum(k_body, sizeof(k_body));
    out[15] = 0xF7;
}

int ua101_ctl_is_request_only(const uint8_t *msg, size_t len)
{
    /* The header, 4 address bytes, 4 size bytes, the checksum and F7. */
    if (len != UA101_CTL_READ_REQUEST_BYTES)
        return 0;
    if (memcmp(msg, k_header, sizeof(k_header)) != 0 || msg[5] != CMD_REQUEST || msg[15] != 0xF7)
        return 0;
    return ua101_ctl_checksum(msg + HEADER_BYTES, 8) == msg[14];
}

int ua101_ctl_is_read_request(const uint8_t *msg, size_t len)
{
    uint8_t want[UA101_CTL_READ_REQUEST_BYTES];

    if (msg == NULL || len != sizeof(want))
        return 0;
    ua101_ctl_read_request(want);
    return memcmp(msg, want, sizeof(want)) == 0 && ua101_ctl_is_request_only(msg, len);
}

size_t ua101_ctl_pack_sysex(const uint8_t *msg, size_t len, uint8_t cable, uint8_t *out,
                            size_t capacity)
{
    size_t packets, i, written = 0;

    if (len == 0 || cable > 15)
        return 0;
    packets = (len + 2) / 3;
    if (packets * 4 > capacity)
        return 0;

    for (i = 0; i < len; i += 3) {
        size_t left = len - i;
        size_t take = left < 3 ? left : 3;
        /* CIN 4 continues a message. CIN 5, 6 and 7 end it with 1, 2 or 3 bytes. */
        uint8_t cin = left > 3 ? 4 : (uint8_t)(4 + take);

        out[written] = (uint8_t)((cable << 4) | cin);
        out[written + 1] = msg[i];
        out[written + 2] = take > 1 ? msg[i + 1] : 0;
        out[written + 3] = take > 2 ? msg[i + 2] : 0;
        written += 4;
    }
    return written;
}

void ua101_sysex_rx_init(ua101_sysex_rx *rx)
{
    memset(rx, 0, sizeof(*rx));
}

/* Returns 1 when this byte completes a message. */
static int rx_byte(ua101_sysex_rx *rx, uint8_t byte)
{
    if (byte >= 0xF8) /* real-time messages can appear anywhere */
        return 0;
    if (byte == 0xF0) {
        rx->active = 1;
        rx->msg[0] = byte;
        rx->len = 1;
        return 0;
    }
    if (!rx->active)
        return 0;
    if (byte >= 0x80 && byte != 0xF7) {
        /* Any other status byte means the message was cut short. */
        rx->active = 0;
        rx->len = 0;
        return 0;
    }
    if (rx->len >= sizeof(rx->msg)) {
        rx->overflows++;
        rx->active = 0;
        rx->len = 0;
        return 0;
    }
    rx->msg[rx->len++] = byte;
    if (byte == 0xF7) {
        rx->active = 0;
        return 1;
    }
    return 0;
}

int ua101_sysex_rx_feed(ua101_sysex_rx *rx, const uint8_t packet[4], const uint8_t **msg,
                        size_t *len)
{
    uint8_t cin = packet[0] & 0x0F;
    unsigned count, i;
    int complete = 0;

    /* Normal MIDI on the other cable is none of this code's business. */
    if ((packet[0] >> 4) != UA101_CTL_CABLE) {
        rx->other_cable_packets++;
        return 0;
    }
    switch (cin) {
    case 0x4:
    case 0x7:
        count = 3;
        break;
    case 0x6:
        count = 2;
        break;
    case 0x5:
        count = 1;
        break;
    default:
        rx->ignored_packets++;
        return 0;
    }
    for (i = 0; i < count && !complete; i++)
        complete = rx_byte(rx, packet[1 + i]);
    if (complete) {
        *msg = rx->msg;
        *len = rx->len;
    }
    return complete;
}

ua101_ctl_kind ua101_ctl_classify(const uint8_t *msg, size_t len, ua101_ctl_view *view)
{
    /* Even the shortest message has the header, an address, a checksum and F7. */
    if (len < HEADER_BYTES + ADDRESS_BYTES + 2)
        return UA101_CTL_BAD_FORMAT;
    if (memcmp(msg, k_header, sizeof(k_header)) != 0 || msg[len - 1] != 0xF7)
        return UA101_CTL_BAD_FORMAT;

    view->command = msg[5];
    memcpy(view->address, msg + HEADER_BYTES, ADDRESS_BYTES);
    view->data = msg + HEADER_BYTES + ADDRESS_BYTES;
    view->data_len = len - (HEADER_BYTES + ADDRESS_BYTES + 2);

    if (ua101_ctl_checksum(msg + HEADER_BYTES, len - HEADER_BYTES - 2) != msg[len - 2])
        return UA101_CTL_BAD_CHECKSUM;

    if (view->command == CMD_SET) {
        const uint8_t *a = view->address;

        if (a[0] == 0x02 && a[1] == 0 && a[2] == 0 && a[3] == 0 &&
            view->data_len == UA101_CTL_BLOCK_BYTES)
            return UA101_CTL_SETTINGS;
        if (a[0] == 0x00 && a[1] == 0x01 && a[2] == 0 && view->data_len == 1)
            return UA101_CTL_CHANGE;
    }
    return UA101_CTL_OTHER;
}

static uint32_t read_nibbles(const uint8_t *bytes, unsigned count, unsigned *bad)
{
    uint32_t value = 0;
    unsigned i;

    for (i = 0; i < count; i++) {
        if (bytes[i] > 0x0F)
            (*bad)++;
        value = (value << 4) | (bytes[i] & 0x0F);
    }
    return value;
}

unsigned ua101_ctl_group_channels(unsigned group)
{
    return group == 0 ? 2 : UA101_CTL_MAX_CHANNELS;
}

void ua101_settings_parse(const uint8_t block[UA101_CTL_BLOCK_BYTES], ua101_settings *out)
{
    /* Where each group's fields start. Group 0 has no pan, and always reads as center. */
    static const unsigned k_link_base[UA101_CTL_GROUPS] = {0x18, 0x19, 0x1B};
    static const unsigned k_on_base[UA101_CTL_GROUPS] = {0x1D, 0x1E, 0x21};
    static const unsigned k_pan_base[UA101_CTL_GROUPS] = {0, 0x24, 0x4C};
    static const unsigned k_volume_base[UA101_CTL_GROUPS] = {0x74, 0x80, 0xBC};
    unsigned group, c, row, output;

    memset(out, 0, sizeof(*out));
    memcpy(out->section_flags, block, sizeof(out->section_flags));

    out->rate_code = block[4] & 0x07;
    out->d4_bit3 = (block[4] >> 3) & 1;
    out->clock_source = block[5] & 0x07;
    out->clock_status = (block[5] >> 3) & 1;
    out->d6_bit0 = block[6] & 1;
    out->mono = (block[6] >> 1) & 1;
    out->soft_control = (block[6] >> 2) & 1;

    for (row = 0; row < UA101_CTL_PATCH_ROWS; row++)
        for (output = 0; output < UA101_CTL_PATCH_OUTPUTS; output++)
            out->patch[row][output] = block[7 + row * UA101_CTL_PATCH_OUTPUTS + output];
    out->solo[0] = block[0x16];
    out->solo[1] = block[0x17];

    for (group = 0; group < UA101_CTL_GROUPS; group++) {
        unsigned channels = ua101_ctl_group_channels(group);

        for (c = 0; c < channels / 2; c++)
            out->link[group][c] = (block[k_link_base[group] + c / 4] >> (c & 3)) & 1;

        for (c = 0; c < channels; c++) {
            ua101_ctl_channel *ch = &out->channel[group][c];

            ch->on = (block[k_on_base[group] + c / 4] >> (c & 3)) & 1;
            ch->pan = group == 0 ? 0x4000
                                 : (uint16_t)read_nibbles(block + k_pan_base[group] + c * 4, 4,
                                                          &out->bad_nibbles);
            ch->volume =
                read_nibbles(block + k_volume_base[group] + c * 6, 6, &out->bad_nibbles);
        }
    }
}

uint32_t ua101_ctl_rate_hz(uint8_t rate_code)
{
    static const uint32_t k_rates[] = {44100, 48000, 88200, 96000, 192000};

    return rate_code < sizeof(k_rates) / sizeof(k_rates[0]) ? k_rates[rate_code] : 0;
}

const char *ua101_ctl_patch_source(uint8_t value)
{
    static const char *const k_sources[] = {
        NULL,          "MONITOR OUT",  "INPUT 1-2",    "INPUT 3-4",
        "INPUT 5-6",   "INPUT 7-8",    "INPUT 9-10",   "WAVE OUT 1-2",
        "WAVE OUT 3-4", "WAVE OUT 5-6", "WAVE OUT 7-8", "WAVE OUT 9-10"};

    return value < sizeof(k_sources) / sizeof(k_sources[0]) ? k_sources[value] : NULL;
}

double ua101_ctl_volume_db(uint32_t volume)
{
    if (volume == 0)
        return -INFINITY;
    return 20.0 * log10((double)volume / (double)UA101_CTL_UNITY_VOLUME);
}
