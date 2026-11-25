/*
 * The UA-101's control protocol, write side: the messages that change a
 * setting, and where each setting lives in the 256-byte settings block.
 * See docs/control-protocol.md. Contains no Apple APIs.
 *
 * NOTHING HERE HAS BEEN TRIED ON A REAL UNIT. The codes come from reading
 * Roland's driver. This file only builds and reads bytes; it sends nothing.
 * What may be sent, and when, is decided in ua101_ctlengine.c.
 *
 * It is kept apart from ua101_control.c on purpose: the read-only tools
 * (ua101settings, ua101midi) don't link this file, so they still can't build
 * a message that sets anything.
 */
#ifndef UA101_CTLMSG_H
#define UA101_CTLMSG_H

#include <stddef.h>
#include <stdint.h>

#include "ua101_control.h"

/* Groups, as in ua101_control.h: 0 MONITOR OUT (2 channels), 1 INPUT, 2 OUTPUT. */
#define UA101_GROUP_MONITOR 0
#define UA101_GROUP_INPUT 1
#define UA101_GROUP_OUTPUT 2

#define UA101_MAX_VOLUME 0x7FFFFFu /* about +12 dB */
#define UA101_MAX_PAN 0x8000u
#define UA101_CENTER_PAN 0x4000u
#define UA101_MAX_SOLO 10u /* a group's solo: 0 for none, or channel 1 to 10 of the group */
#define UA101_PATCH_SOURCE_MIN 1u
#define UA101_PATCH_SOURCE_MAX 11u
#define UA101_PATCH_HARD_THRU_FIRST 2u /* INPUT 1-2 */
#define UA101_PATCH_INITIAL_FIRST 7u   /* WAVE OUT 1-2 */
#define UA101_MEMORIES 5

/* One setting that an ordinary message can change. */
typedef enum {
    UA101_F_VOLUME = 0, /* index: channel */
    UA101_F_PAN,        /* index: channel; groups 1 and 2 only */
    UA101_F_ON,         /* index: channel; the raw bit, see the engine about its meaning */
    UA101_F_LINK,       /* index: pair */
    UA101_F_SOLO,       /* index: 0; groups 1 and 2 only; the value: 0 for no solo, or the one
                           soloed channel's number, 1 to UA101_MAX_SOLO (tried on the unit) */
    UA101_F_PATCH       /* group 0; index: output pair 0 to 4 (row 1 of the block) */
} ua101_field_kind;

typedef struct {
    uint8_t kind;
    uint8_t group;
    uint8_t index;
} ua101_field;

/* 22 volumes, 20 pans, 22 on bits, 11 links, 2 solos, 5 patch sources. */
#define UA101_FIELD_COUNT 82

/* Returns 0 to UA101_FIELD_COUNT-1, or -1 for a field that doesn't exist. */
int ua101_field_id(ua101_field f);
ua101_field ua101_field_from_id(unsigned id);
/* 1 for the direct monitor mixer's fields, 0 for the patch bay. */
int ua101_field_is_mixer(ua101_field f);
/* 1 if the value is one the notes allow for this field. */
int ua101_field_value_ok(ua101_field f, uint32_t value);

uint32_t ua101_block_get(const uint8_t block[UA101_CTL_BLOCK_BYTES], ua101_field f);
void ua101_block_put(uint8_t block[UA101_CTL_BLOCK_BYTES], ua101_field f, uint32_t value);

typedef enum {
    UA101_MSG_INVALID = 0, /* not a well-formed message for this model */
    UA101_MSG_READ_BLOCK,  /* the one request: send the whole settings block */
    UA101_MSG_FIELD,       /* set one field */
    UA101_MSG_AUTOSAVE,    /* value 0: suspend, 1: resume */
    /* These change or erase the unit's stored memories. */
    UA101_MSG_RESET_ALL,
    UA101_MSG_RESET_CURRENT, /* the settings in use, no memory: Roland's "Current Setting"
                                (its "reset one" with the number 0, tried on the unit) */
    UA101_MSG_RESET_MEMORY, /* memory 0 to 4 */
    UA101_MSG_STORE,        /* memory 0 to 4; sections[i] is 0 or 1 here */
    UA101_MSG_LOAD,         /* memory 0 to 4; sections[i] is 0 or 1 */
    UA101_MSG_PRESET,       /* value 4: Hard Thru, 3: Initial Setting */
    UA101_MSG_UNKNOWN       /* well-formed, but not one of the above */
} ua101_msg_kind;

typedef struct {
    ua101_msg_kind kind;
    ua101_field field;
    uint32_t value;
    uint8_t memory;
    uint8_t sections[4]; /* switches, patch row 0, patch rows 1-2, mixer */
} ua101_msg;

#define UA101_CTLMSG_MAX_BYTES 18

/* 1 if a Load or Store names a memory that exists and at least one section. */
int ua101_ctlmsg_sections_ok(const ua101_msg *m);

/* Writes the message. Returns its length, or 0 for anything out of range.
 * In ua101_ctlbuild.c, which the helper doesn't link. */
size_t ua101_ctlmsg_build(const ua101_msg *m, uint8_t out[UA101_CTLMSG_MAX_BYTES]);
/* Reads a message a computer would send. Out-of-range values give UNKNOWN. */
ua101_msg_kind ua101_ctlmsg_parse(const uint8_t *bytes, size_t len, ua101_msg *out);

/* How much harm a message could do if it were sent to the unit. */
typedef enum {
    UA101_RISK_READ = 0, /* asks for the settings; changes nothing */
    UA101_RISK_SETTING,  /* changes one current setting, or pauses saving */
    UA101_RISK_MEMORY,   /* replaces settings or overwrites stored memories */
    UA101_RISK_NEVER     /* unknown, malformed, or listed under "commands to avoid" */
} ua101_risk;

ua101_risk ua101_ctlmsg_risk(const uint8_t *bytes, size_t len);

#endif
