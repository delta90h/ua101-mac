/*
 * MIDI for the UA-101's MIDI IN and OUT ports, with no Apple APIs, so it can be unit-tested.
 *
 * The unit speaks standard USB-MIDI on interface 2: 4-byte event packets, whose first byte holds
 * a cable number and a code index number (CIN) that gives the message's length. Cable 0 is the
 * MIDI port. Cable 1 carries Roland's control messages (see ua101_control.h), which can change
 * the unit's mixer and erase its stored settings, so nothing an app sends may ever reach it:
 * the packer below can only write cable 0.
 *
 * CoreMIDI's current API passes Universal MIDI Packets (UMP) of 32-bit words, so both directions
 * also convert to and from UMP, using MIDI 1.0 messages only:
 *   app -> unit: UMP words -> MIDI bytes -> parser -> USB-MIDI packets on cable 0
 *   unit -> app: USB-MIDI packets -> MIDI bytes (cable 0 only) -> parser -> UMP words
 */
#ifndef UA101_MIDI_H
#define UA101_MIDI_H

#include <stddef.h>
#include <stdint.h>

#define UA101_MIDI_CABLE 0
#define UA101_MIDI_PACKET_BYTES 4

/* What the parser found in a MIDI 1.0 byte stream. */
typedef enum {
    UA101_MIDI_NONE,        /* nothing complete yet, or a byte was dropped */
    UA101_MIDI_SHORT,       /* a complete message of 1 to 3 bytes, with its status byte */
    UA101_MIDI_SYSEX_START, /* F0 */
    UA101_MIDI_SYSEX_BYTE,  /* one data byte inside SysEx */
    UA101_MIDI_SYSEX_END,   /* F7 */
    UA101_MIDI_SYSEX_CUT,   /* SysEx ended by another status byte, so it's incomplete */
} ua101_midi_kind;

typedef struct {
    ua101_midi_kind kind;
    uint8_t bytes[3]; /* the message for SHORT, the data byte for SYSEX_BYTE */
    uint8_t len;
} ua101_midi_event;

/*
 * Turns a byte stream into messages. It expands running status, lets real-time bytes through
 * at any point without disturbing the message around them, and drops what MIDI 1.0 doesn't
 * allow: data bytes with no status, undefined status bytes (F4 F5 F9 FD), and a lone F7.
 * A status byte inside SysEx ends it as cut, then starts its own message.
 */
typedef struct {
    uint8_t status;  /* current or running status, 0 for none */
    uint8_t data[2];
    uint8_t have;    /* data bytes collected */
    uint8_t need;    /* data bytes the status needs */
    uint8_t pending; /* 1 while a status byte waits for its first complete message */
    uint8_t in_sysex;
    uint64_t dropped;     /* bytes thrown away */
    uint64_t sysex_cut;   /* SysEx messages ended without F7 */
} ua101_midi_parser;

void ua101_midi_parser_init(ua101_midi_parser *p);
/*
 * Feeds one byte and returns what it completed, also filled into ev. When a status byte ends
 * SysEx without F7, *cut is set to 1: pass a SYSEX_CUT event on first, then ev, because the
 * same byte may also complete a message of its own (F6), or start one (F0).
 */
ua101_midi_kind ua101_midi_parse(ua101_midi_parser *p, uint8_t byte, ua101_midi_event *ev,
                                 int *cut);

/* Data bytes a status byte needs, or -1 if it isn't a valid status byte to send. */
int ua101_midi_data_bytes(uint8_t status);

/*
 * Packs messages into USB-MIDI packets for cable 0. SysEx goes in groups of 3 bytes, F0 and
 * F7 included. A cut SysEx's last 1 or 2 bytes are dropped, as Linux does for this unit.
 */
typedef struct {
    uint8_t sysex[3];
    uint8_t have;
    uint64_t dropped; /* bytes of cut SysEx messages left unsent */
} ua101_usbmidi_packer;

void ua101_usbmidi_packer_init(ua101_usbmidi_packer *k);
/* Returns 1 and fills packet when the event completes one. */
int ua101_usbmidi_pack(ua101_usbmidi_packer *k, const ua101_midi_event *ev,
                       uint8_t packet[UA101_MIDI_PACKET_BYTES]);

/*
 * Reads one USB-MIDI packet from the unit. Returns the number of MIDI bytes in it (0 to 3),
 * copied to bytes, and sets *cable. Packets whose code doesn't match their status byte, and the
 * reserved codes 0 and 1 (which includes the zero padding at the end of a transfer), give 0.
 * *malformed is set to 1 for a packet that is neither valid nor all zero.
 */
size_t ua101_usbmidi_unpack(const uint8_t packet[UA101_MIDI_PACKET_BYTES], uint8_t *cable,
                            uint8_t bytes[3], int *malformed);

/* Where a cable's messages go. Only cable 0 is ever given to apps. */
typedef enum { UA101_ROUTE_APPS, UA101_ROUTE_CONTROL, UA101_ROUTE_DROP } ua101_midi_route;
ua101_midi_route ua101_midi_route_for(uint8_t cable);

/*
 * Packs messages into UMP words, using group 0: real time and system common as message type 1,
 * channel voice as type 2, and SysEx as type 3, in 64-bit packets of up to 6 data bytes.
 * A cut SysEx that has already sent a start packet is closed with an end packet, and one that
 * hasn't is dropped, so an app never sees a cut message presented as complete.
 */
typedef struct {
    uint8_t sysex[6];
    uint8_t have;
    uint8_t started;
    uint64_t dropped; /* SysEx bytes not passed on */
    uint64_t cut;     /* SysEx messages closed early */
} ua101_ump_packer;

void ua101_ump_packer_init(ua101_ump_packer *k);
/* Returns the number of words written to words (0, 1 or 2). */
size_t ua101_ump_pack(ua101_ump_packer *k, const ua101_midi_event *ev, uint32_t words[2]);

/* The number of 32-bit words in a UMP packet of this message type (the top 4 bits). */
size_t ua101_ump_words(uint32_t first_word);

/*
 * Turns one UMP packet from an app back into MIDI 1.0 bytes, whatever its group, since the unit
 * has one MIDI port. Takes the packet's words (count from ua101_ump_words) and returns the
 * number of bytes written to bytes (up to 8). It returns 0 for utility messages (type 0, such
 * as timestamps), and 0 with *refused set to 1 for anything else it can't pass on: MIDI 2.0
 * and other types MIDI 1.0 can't carry, and malformed packets.
 */
size_t ua101_ump_to_bytes(const uint32_t *words, size_t count, uint8_t bytes[8], int *refused);

/*
 * A fixed CoreMIDI unique ID for a published port, worked out from its name and direction, so
 * apps that remember ports by ID find it again on the next run instead of a stranger. CoreMIDI
 * gives a random one otherwise. If another object already has it, try the next attempt number.
 * Always positive, so never 0, which CoreMIDI uses for no ID.
 */
int32_t ua101_midi_port_id(const char *port_name, int is_destination, unsigned attempt);

#endif
