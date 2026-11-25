/*
 * USB descriptor parsing and validation for the Edirol/Roland UA-101.
 * Contains no Apple APIs, so it can be unit-tested on any computer.
 */
#ifndef UA101_DESC_H
#define UA101_DESC_H

#include <stddef.h>
#include <stdint.h>

#define UA101_VENDOR_ID 0x0582
#define UA101_PRODUCT_HIGH_SPEED 0x007D
#define UA101_PRODUCT_FULL_SPEED 0x008D

#define UA101_IF_PLAYBACK 0
#define UA101_IF_CAPTURE 1
#define UA101_IF_MIDI 2
#define UA101_NUM_INTERFACES 3

/* The UA-101 has six alternate settings, so these leave plenty of room. */
#define UA101_MAX_ALTS 16
#define UA101_MAX_ENDPOINTS 4

typedef struct {
    uint8_t address;    /* bit 7 set means IN */
    uint8_t attributes; /* bits 0-1 are the transfer type */
    uint16_t max_packet; /* bytes per packet, without the multiplier bits */
    uint8_t interval;
} ua101_endpoint;

typedef struct {
    uint8_t number;
    uint8_t alt;
    uint8_t iface_class;
    uint8_t iface_subclass;
    uint8_t iface_protocol;
    uint8_t declared_endpoints;

    uint8_t num_endpoints;
    ua101_endpoint endpoints[UA101_MAX_ENDPOINTS];

    /* From the class-specific format descriptor, when has_format is set. */
    uint8_t has_format;
    uint8_t format_type;
    uint8_t channels;
    uint8_t subframe_bytes;
    uint8_t bit_resolution;
    uint8_t rate_count; /* bSamFreqType: number of discrete rates */
    uint32_t rate_hz;   /* first listed rate */
} ua101_alt;

typedef struct {
    uint16_t total_length;
    uint8_t num_interfaces;
    uint8_t config_value;
    uint8_t attributes;
    uint8_t truncated; /* fewer bytes were supplied than total_length says */

    uint8_t num_alts;
    ua101_alt alts[UA101_MAX_ALTS];
} ua101_config;

typedef enum {
    UA101_PARSE_OK = 0,
    UA101_PARSE_BAD_HEADER,
    UA101_PARSE_BAD_DESCRIPTOR,
    UA101_PARSE_TOO_MANY
} ua101_parse_result;

/* Called once per check. The message is only valid during the call. */
typedef void (*ua101_report_fn)(void *ctx, int ok, const char *message);

ua101_parse_result ua101_parse_config(const uint8_t *data, size_t len,
                                      ua101_config *out);
const char *ua101_parse_result_string(ua101_parse_result result);

/* Returns NULL when that interface and alternate setting don't exist. */
const ua101_alt *ua101_find_alt(const ua101_config *cfg, uint8_t number,
                                uint8_t alt);

int ua101_endpoint_is_in(const ua101_endpoint *ep);
int ua101_endpoint_is_isochronous(const ua101_endpoint *ep);
int ua101_endpoint_is_bulk(const ua101_endpoint *ep);

/*
 * Checks the structure the driver depends on. Reports each result through
 * report, which may be NULL, and returns the number of failed checks.
 */
int ua101_check_config(const ua101_config *cfg, ua101_report_fn report,
                       void *ctx);

#endif
