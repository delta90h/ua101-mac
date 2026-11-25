#include "ua101_midi.h"

#include <string.h>

/* Apps' messages must never reach cable 1, Roland's control channel. */
_Static_assert(UA101_MIDI_CABLE == 0, "app MIDI goes on cable 0 only");

#define CONTROL_CABLE 1

/* USB-MIDI 1.0, table 4-1: MIDI bytes carried by each code index number. 0 and 1 are reserved. */
static const uint8_t k_cin_bytes[16] = {0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1};

/* UMP 1.1: words in a packet of each message type. */
static const uint8_t k_ump_words[16] = {1, 1, 1, 2, 2, 4, 1, 1, 2, 2, 2, 3, 3, 4, 4, 4};

#define UMP_SYSEX_COMPLETE 0
#define UMP_SYSEX_START 1
#define UMP_SYSEX_CONTINUE 2
#define UMP_SYSEX_END 3

int ua101_midi_data_bytes(uint8_t status)
{
    if (status < 0x80)
        return -1;
    if (status < 0xF0) {
        uint8_t kind = status & 0xF0;

        return kind == 0xC0 || kind == 0xD0 ? 1 : 2;
    }
    switch (status) {
    case 0xF1: /* time code quarter frame */
    case 0xF3: /* song select */
        return 1;
    case 0xF2: /* song position */
        return 2;
    case 0xF6: /* tune request */
    case 0xF8:
    case 0xFA:
    case 0xFB:
    case 0xFC:
    case 0xFE:
    case 0xFF:
        return 0;
    default: /* F0 and F7 belong to SysEx; F4, F5, F9 and FD are undefined */
        return -1;
    }
}

void ua101_midi_parser_init(ua101_midi_parser *p)
{
    memset(p, 0, sizeof(*p));
}

static ua101_midi_kind give(ua101_midi_event *ev, ua101_midi_kind kind, const uint8_t *bytes,
                            uint8_t len)
{
    ev->kind = kind;
    ev->len = len;
    if (len != 0)
        memcpy(ev->bytes, bytes, len);
    return kind;
}

ua101_midi_kind ua101_midi_parse(ua101_midi_parser *p, uint8_t byte, ua101_midi_event *ev,
                                 int *cut)
{
    int need;

    *cut = 0;
    ev->kind = UA101_MIDI_NONE;
    ev->len = 0;

    /* Real time can come between any two bytes, even inside SysEx, and changes nothing. */
    if (byte >= 0xF8) {
        if (ua101_midi_data_bytes(byte) != 0) {
            p->dropped++;
            return UA101_MIDI_NONE;
        }
        return give(ev, UA101_MIDI_SHORT, &byte, 1);
    }

    if (byte < 0x80) {
        if (p->in_sysex)
            return give(ev, UA101_MIDI_SYSEX_BYTE, &byte, 1);
        if (p->status == 0) {
            p->dropped++;
            return UA101_MIDI_NONE;
        }
        p->data[p->have++] = byte;
        if (p->have < p->need)
            return UA101_MIDI_NONE;
        {
            uint8_t msg[3] = {p->status, p->data[0], p->data[1]};

            p->have = 0;
            p->pending = 0;
            /* Only channel messages leave a running status behind. */
            if (p->status >= 0xF0)
                p->status = 0;
            return give(ev, UA101_MIDI_SHORT, msg, (uint8_t)(1 + p->need));
        }
    }

    /* Any other status byte ends what came before it. */
    if (p->in_sysex) {
        p->in_sysex = 0;
        if (byte == 0xF7)
            return give(ev, UA101_MIDI_SYSEX_END, &byte, 1);
        p->sysex_cut++;
        *cut = 1;
    } else {
        p->dropped += p->pending + p->have;
    }
    p->status = 0;
    p->have = 0;
    p->pending = 0;

    if (byte == 0xF0) {
        p->in_sysex = 1;
        return give(ev, UA101_MIDI_SYSEX_START, &byte, 1);
    }
    need = ua101_midi_data_bytes(byte);
    if (need < 0) { /* a lone F7, or an undefined status */
        p->dropped++;
        return UA101_MIDI_NONE;
    }
    if (need == 0) /* tune request */
        return give(ev, UA101_MIDI_SHORT, &byte, 1);
    p->status = byte;
    p->need = (uint8_t)need;
    p->pending = 1;
    return UA101_MIDI_NONE;
}

void ua101_usbmidi_packer_init(ua101_usbmidi_packer *k)
{
    memset(k, 0, sizeof(*k));
}

static int usb_packet(uint8_t cin, const uint8_t *bytes, uint8_t len,
                      uint8_t packet[UA101_MIDI_PACKET_BYTES])
{
    packet[0] = (uint8_t)((UA101_MIDI_CABLE << 4) | cin);
    packet[1] = bytes[0];
    packet[2] = len > 1 ? bytes[1] : 0;
    packet[3] = len > 2 ? bytes[2] : 0;
    return 1;
}

int ua101_usbmidi_pack(ua101_usbmidi_packer *k, const ua101_midi_event *ev,
                       uint8_t packet[UA101_MIDI_PACKET_BYTES])
{
    uint8_t status, cin;

    switch (ev->kind) {
    case UA101_MIDI_SHORT:
        status = ev->bytes[0];
        if (status >= 0xF8)
            cin = 0xF;
        else if (status == 0xF6)
            cin = 0x5;
        else if (status == 0xF2)
            cin = 0x3;
        else if (status >= 0xF0)
            cin = 0x2;
        else
            cin = status >> 4;
        return usb_packet(cin, ev->bytes, ev->len, packet);
    case UA101_MIDI_SYSEX_START:
        k->sysex[0] = 0xF0;
        k->have = 1;
        return 0;
    case UA101_MIDI_SYSEX_BYTE:
        k->sysex[k->have++] = ev->bytes[0];
        if (k->have < 3)
            return 0;
        k->have = 0;
        return usb_packet(0x4, k->sysex, 3, packet); /* starts or continues */
    case UA101_MIDI_SYSEX_END:
        k->sysex[k->have++] = 0xF7;
        cin = (uint8_t)(0x4 + k->have); /* 5, 6 or 7: ends with 1, 2 or 3 bytes */
        usb_packet(cin, k->sysex, k->have, packet);
        k->have = 0;
        return 1;
    case UA101_MIDI_SYSEX_CUT:
        k->dropped += k->have;
        k->have = 0;
        return 0;
    default:
        return 0;
    }
}

static int is_data(uint8_t byte)
{
    return byte < 0x80;
}

/* Checks that a packet's bytes are what its code says, so a damaged one can't merge with the
   next message. */
static int usb_packet_valid(uint8_t cin, const uint8_t *b)
{
    switch (cin) {
    case 0x2:
        return (b[0] == 0xF1 || b[0] == 0xF3) && is_data(b[1]);
    case 0x3:
        return b[0] == 0xF2 && is_data(b[1]) && is_data(b[2]);
    case 0x4: /* SysEx starts or continues */
        return (b[0] == 0xF0 || is_data(b[0])) && is_data(b[1]) && is_data(b[2]);
    case 0x5: /* tune request, or SysEx ends with F7 alone */
        return b[0] == 0xF6 || b[0] == 0xF7;
    case 0x6:
        return (b[0] == 0xF0 || is_data(b[0])) && b[1] == 0xF7;
    case 0x7:
        return (b[0] == 0xF0 || is_data(b[0])) && is_data(b[1]) && b[2] == 0xF7;
    case 0xF: /* one byte, as it comes */
        return 1;
    default: /* channel messages: the code repeats the status byte's top half */
        return (b[0] >> 4) == cin && is_data(b[1]) && (k_cin_bytes[cin] < 3 || is_data(b[2]));
    }
}

size_t ua101_usbmidi_unpack(const uint8_t packet[UA101_MIDI_PACKET_BYTES], uint8_t *cable,
                            uint8_t bytes[3], int *malformed)
{
    uint8_t cin = packet[0] & 0x0F;
    size_t len = k_cin_bytes[cin];

    *cable = packet[0] >> 4;
    *malformed = 0;
    if (len == 0) {
        *malformed = (packet[0] | packet[1] | packet[2] | packet[3]) != 0;
        return 0;
    }
    if (!usb_packet_valid(cin, packet + 1)) {
        *malformed = 1;
        return 0;
    }
    memcpy(bytes, packet + 1, len);
    return len;
}

ua101_midi_route ua101_midi_route_for(uint8_t cable)
{
    if (cable == UA101_MIDI_CABLE)
        return UA101_ROUTE_APPS;
    if (cable == CONTROL_CABLE)
        return UA101_ROUTE_CONTROL;
    return UA101_ROUTE_DROP;
}

void ua101_ump_packer_init(ua101_ump_packer *k)
{
    memset(k, 0, sizeof(*k));
}

static size_t ump_sysex(ua101_ump_packer *k, unsigned status, uint32_t words[2])
{
    uint8_t b[6] = {0};

    memcpy(b, k->sysex, k->have);
    words[0] = (0x3u << 28) | (status << 20) | ((uint32_t)k->have << 16) | ((uint32_t)b[0] << 8) |
               b[1];
    words[1] = ((uint32_t)b[2] << 24) | ((uint32_t)b[3] << 16) | ((uint32_t)b[4] << 8) | b[5];
    k->have = 0;
    return 2;
}

size_t ua101_ump_pack(ua101_ump_packer *k, const ua101_midi_event *ev, uint32_t words[2])
{
    size_t n = 0;

    switch (ev->kind) {
    case UA101_MIDI_SHORT: {
        uint32_t status = ev->bytes[0];
        uint32_t d1 = ev->len > 1 ? ev->bytes[1] : 0;
        uint32_t d2 = ev->len > 2 ? ev->bytes[2] : 0;
        uint32_t type = status >= 0xF0 ? 0x1 : 0x2;

        words[0] = (type << 28) | (status << 16) | (d1 << 8) | d2;
        return 1;
    }
    case UA101_MIDI_SYSEX_START:
        k->have = 0;
        k->started = 0;
        return 0;
    case UA101_MIDI_SYSEX_BYTE:
        /* A full packet waits for one more byte, so the last one can be marked as the end. */
        if (k->have == sizeof(k->sysex)) {
            n = ump_sysex(k, k->started ? UMP_SYSEX_CONTINUE : UMP_SYSEX_START, words);
            k->started = 1;
        }
        k->sysex[k->have++] = ev->bytes[0];
        return n;
    case UA101_MIDI_SYSEX_END:
        n = ump_sysex(k, k->started ? UMP_SYSEX_END : UMP_SYSEX_COMPLETE, words);
        k->started = 0;
        return n;
    case UA101_MIDI_SYSEX_CUT:
        if (k->started) {
            k->cut++;
            n = ump_sysex(k, UMP_SYSEX_END, words);
        } else {
            k->dropped += k->have;
            k->have = 0;
        }
        k->started = 0;
        return n;
    default:
        return 0;
    }
}

size_t ua101_ump_words(uint32_t first_word)
{
    return k_ump_words[first_word >> 28];
}

size_t ua101_ump_to_bytes(const uint32_t *words, size_t count, uint8_t bytes[8], int *refused)
{
    uint32_t w = words[0];
    uint32_t type = w >> 28;
    uint8_t status = (uint8_t)(w >> 16);
    uint8_t d1 = (uint8_t)(w >> 8), d2 = (uint8_t)w;
    int need;

    *refused = 0;
    if (count != ua101_ump_words(w)) {
        *refused = 1;
        return 0;
    }
    switch (type) {
    case 0x0: /* utility: no-op and timestamps */
        return 0;
    case 0x1: /* system common and real time */
    case 0x2: /* MIDI 1.0 channel voice */
        need = ua101_midi_data_bytes(status);
        if (need < 0 || (type == 0x1) != (status >= 0xF0) || (need > 0 && !is_data(d1)) ||
            (need > 1 && !is_data(d2))) {
            *refused = 1;
            return 0;
        }
        bytes[0] = status;
        bytes[1] = d1;
        bytes[2] = d2;
        return (size_t)(1 + need);
    case 0x3: { /* 7-bit SysEx, up to 6 bytes a packet */
        unsigned form = (w >> 20) & 0xF, n = (w >> 16) & 0xF, i;
        uint8_t data[6] = {d1, d2, (uint8_t)(words[1] >> 24), (uint8_t)(words[1] >> 16),
                           (uint8_t)(words[1] >> 8), (uint8_t)words[1]};
        size_t len = 0;

        if (form > UMP_SYSEX_END || n > 6) {
            *refused = 1;
            return 0;
        }
        for (i = 0; i < n; i++) {
            if (!is_data(data[i])) {
                *refused = 1;
                return 0;
            }
        }
        if (form == UMP_SYSEX_COMPLETE || form == UMP_SYSEX_START)
            bytes[len++] = 0xF0;
        memcpy(bytes + len, data, n);
        len += n;
        if (form == UMP_SYSEX_COMPLETE || form == UMP_SYSEX_END)
            bytes[len++] = 0xF7;
        return len;
    }
    default: /* MIDI 2.0 and other types that MIDI 1.0 can't carry */
        *refused = 1;
        return 0;
    }
}

/* 32-bit FNV-1a over the name, the direction and the attempt: stable across runs and Macs. */
int32_t ua101_midi_port_id(const char *port_name, int is_destination, unsigned attempt)
{
    const uint8_t *p = (const uint8_t *)port_name;
    uint32_t hash = 2166136261u;
    uint32_t id;
    unsigned i;

    for (; *p != 0; p++)
        hash = (hash ^ *p) * 16777619u;
    hash = (hash ^ (is_destination ? 'd' : 's')) * 16777619u;
    for (i = 0; i < 4; i++)
        hash = (hash ^ ((attempt >> (8 * i)) & 0xFFu)) * 16777619u;
    id = hash & 0x7FFFFFFFu;
    return (int32_t)(id == 0 ? 1 : id);
}
