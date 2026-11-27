/*
 * The UA-101's control protocol, read side only: SysEx on USB-MIDI cable 1.
 * See docs/control-protocol.md. Contains no Apple APIs, so it can be
 * unit-tested on any computer.
 *
 * There is deliberately nothing here that builds a message which sets anything.
 */
#ifndef UA101_CONTROL_H
#define UA101_CONTROL_H

#include <stddef.h>
#include <stdint.h>

#define UA101_CTL_CABLE 1
#define UA101_CTL_BLOCK_BYTES 256
/* F0 41 10 00 69, the command, 4 address bytes, the block, the checksum and F7. */
#define UA101_CTL_BLOCK_MESSAGE_BYTES (10 + UA101_CTL_BLOCK_BYTES + 2)
#define UA101_CTL_READ_REQUEST_BYTES 16
/* Six USB-MIDI packets: five carry 3 bytes each and the last carries F7. */
#define UA101_CTL_READ_PACKET_BYTES 24
#define UA101_CTL_MAX_MESSAGE_BYTES 300

#define UA101_CTL_GROUPS 3
#define UA101_CTL_MAX_CHANNELS 10
#define UA101_CTL_PATCH_ROWS 3
#define UA101_CTL_PATCH_OUTPUTS 5
#define UA101_CTL_UNITY_VOLUME 0x200000u

uint8_t ua101_ctl_checksum(const uint8_t *bytes, size_t count);

/* Writes the one request this project sends: read the whole settings block. */
void ua101_ctl_read_request(uint8_t out[UA101_CTL_READ_REQUEST_BYTES]);
/* For the developer's read-only test of the unit's other blocks (ua101settings --all-blocks,
   2026-11-27): the request for block 02 <block> 00 00, 256 bytes. Block 0 is the settings in
   use; on the sister UA-1000, 03 to 07 seem to be the five memories. Returns 0, and writes
   nothing, for a block above 0x0F. The helper passes none of these but block 0 on
   (ua101_ctl_is_read_request below). */
#define UA101_CTL_BLOCKS 16
int ua101_ctl_block_request(unsigned block, uint8_t out[UA101_CTL_READ_REQUEST_BYTES]);
/* Returns 1 only for a well-formed message that asks for data and sets nothing. */
int ua101_ctl_is_request_only(const uint8_t *msg, size_t len);
/* Returns 1 only for exactly the one request above, byte for byte. The helper passes on nothing
   else (src/ua101_service.c): ua101_ctl_is_request_only would also let through the requests
   listed under "Commands to avoid" in docs/control-protocol.md. */
int ua101_ctl_is_read_request(const uint8_t *msg, size_t len);
/* Packs a SysEx message into USB-MIDI packets. Returns the bytes written, or 0 if it won't fit. */
size_t ua101_ctl_pack_sysex(const uint8_t *msg, size_t len, uint8_t cable, uint8_t *out,
                            size_t capacity);

/* Reassembles SysEx messages from USB-MIDI packets, keeping only the control cable. */
typedef struct {
    uint8_t msg[UA101_CTL_MAX_MESSAGE_BYTES];
    size_t len;
    int active;
    uint64_t other_cable_packets;
    uint64_t ignored_packets;
    uint64_t overflows;
} ua101_sysex_rx;

void ua101_sysex_rx_init(ua101_sysex_rx *rx);
/* Returns 1 and sets *msg and *len when this packet completes a message. */
int ua101_sysex_rx_feed(ua101_sysex_rx *rx, const uint8_t packet[4], const uint8_t **msg,
                        size_t *len);

typedef enum {
    UA101_CTL_BAD_FORMAT = 0, /* not a complete message from this model */
    UA101_CTL_BAD_CHECKSUM,
    UA101_CTL_SETTINGS, /* the settings block */
    UA101_CTL_CHANGE,   /* a one-byte value change at 00 01 00 n */
    UA101_CTL_OTHER     /* valid, but not one the protocol notes describe */
} ua101_ctl_kind;

typedef struct {
    uint8_t command;
    uint8_t address[4];
    const uint8_t *data;
    size_t data_len;
} ua101_ctl_view;

ua101_ctl_kind ua101_ctl_classify(const uint8_t *msg, size_t len, ua101_ctl_view *view);
/* Returns 1 and sets *block when a classified message is a whole 256-byte block from
   02 <block> 00 00, block 0 to 0x0F. */
int ua101_ctl_block_reply(const ua101_ctl_view *view, unsigned *block);

typedef struct {
    uint8_t on;   /* raw bit: which value means on isn't known yet */
    uint16_t pan; /* 0 to 0x8000, 0x4000 is center */
    uint32_t volume;
} ua101_ctl_channel;

typedef struct {
    uint8_t section_flags[4]; /* switches, patch row 0, patch rows 1-2, mixer */

    uint8_t rate_code;
    uint8_t d4_bit3; /* meaning unknown */
    uint8_t clock_source;
    uint8_t clock_status;
    uint8_t d6_bit0; /* meaning unknown */
    uint8_t mono;
    uint8_t soft_control;

    uint8_t patch[UA101_CTL_PATCH_ROWS][UA101_CTL_PATCH_OUTPUTS];
    uint8_t solo[2]; /* groups 1 and 2 */

    /* Group 0 is MONITOR OUT with 2 channels, 1 is INPUT and 2 is computer OUTPUT. */
    uint8_t link[UA101_CTL_GROUPS][UA101_CTL_MAX_CHANNELS / 2];
    ua101_ctl_channel channel[UA101_CTL_GROUPS][UA101_CTL_MAX_CHANNELS];

    /* Bytes in a nibble field above 0x0F, which the notes say can't happen. */
    unsigned bad_nibbles;
} ua101_settings;

void ua101_settings_parse(const uint8_t block[UA101_CTL_BLOCK_BYTES], ua101_settings *out);
unsigned ua101_ctl_group_channels(unsigned group);

/* These return NULL or 0 for a value the notes don't describe. */
uint32_t ua101_ctl_rate_hz(uint8_t rate_code);
const char *ua101_ctl_patch_source(uint8_t value);
/* Returns the level in dB. A volume of 0 is silence and returns -INFINITY. */
double ua101_ctl_volume_db(uint32_t volume);

#endif
