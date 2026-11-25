#include "ua101_desc.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define DT_CONFIG 0x02
#define DT_INTERFACE 0x04
#define DT_ENDPOINT 0x05
#define DT_CS_INTERFACE 0x24
#define AS_FORMAT_TYPE 0x02

#define TRANSFER_ISOCHRONOUS 1
#define TRANSFER_BULK 2

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t read_le24(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

ua101_parse_result ua101_parse_config(const uint8_t *data, size_t len,
                                      ua101_config *out)
{
    size_t limit, pos;
    ua101_alt *current = NULL;

    memset(out, 0, sizeof(*out));

    if (data == NULL || len < 9 || data[0] < 9 || data[1] != DT_CONFIG)
        return UA101_PARSE_BAD_HEADER;

    out->total_length = read_le16(data + 2);
    out->num_interfaces = data[4];
    out->config_value = data[5];
    out->attributes = data[7];

    limit = out->total_length;
    if (limit > len) {
        out->truncated = 1;
        limit = len;
    }
    pos = data[0];
    if (pos > limit)
        return UA101_PARSE_BAD_HEADER;

    while (pos < limit) {
        const uint8_t *d = data + pos;
        size_t remaining = limit - pos;
        uint8_t dlen, type;

        if (remaining < 2)
            return UA101_PARSE_BAD_DESCRIPTOR;
        dlen = d[0];
        type = d[1];
        if (dlen < 2 || dlen > remaining)
            return UA101_PARSE_BAD_DESCRIPTOR;

        switch (type) {
        case DT_INTERFACE:
            if (dlen < 9)
                return UA101_PARSE_BAD_DESCRIPTOR;
            if (out->num_alts >= UA101_MAX_ALTS)
                return UA101_PARSE_TOO_MANY;
            current = &out->alts[out->num_alts++];
            current->number = d[2];
            current->alt = d[3];
            current->declared_endpoints = d[4];
            current->iface_class = d[5];
            current->iface_subclass = d[6];
            current->iface_protocol = d[7];
            break;

        case DT_ENDPOINT:
            if (dlen < 7 || current == NULL)
                return UA101_PARSE_BAD_DESCRIPTOR;
            if (current->num_endpoints >= UA101_MAX_ENDPOINTS)
                return UA101_PARSE_TOO_MANY;
            {
                ua101_endpoint *ep =
                    &current->endpoints[current->num_endpoints++];
                ep->address = d[2];
                ep->attributes = d[3];
                ep->max_packet = (uint16_t)(read_le16(d + 4) & 0x07FF);
                ep->interval = d[6];
            }
            break;

        case DT_CS_INTERFACE:
            /* Format descriptor: type, channels, subframe, bits, rate list. */
            if (current != NULL && dlen >= 8 && d[2] == AS_FORMAT_TYPE) {
                current->has_format = 1;
                current->format_type = d[3];
                current->channels = d[4];
                current->subframe_bytes = d[5];
                current->bit_resolution = d[6];
                current->rate_count = d[7];
                current->rate_hz = 0;
                if (d[7] >= 1 && dlen >= 11)
                    current->rate_hz = read_le24(d + 8);
            }
            break;

        default:
            /* Other descriptors, such as class-specific endpoints, are unused. */
            break;
        }
        pos += dlen;
    }
    return UA101_PARSE_OK;
}

const char *ua101_parse_result_string(ua101_parse_result result)
{
    switch (result) {
    case UA101_PARSE_OK:
        return "ok";
    case UA101_PARSE_BAD_HEADER:
        return "bad configuration header";
    case UA101_PARSE_BAD_DESCRIPTOR:
        return "malformed descriptor";
    case UA101_PARSE_TOO_MANY:
        return "more interfaces or endpoints than expected";
    }
    return "unknown";
}

const ua101_alt *ua101_find_alt(const ua101_config *cfg, uint8_t number,
                                uint8_t alt)
{
    uint8_t i;

    for (i = 0; i < cfg->num_alts; i++) {
        if (cfg->alts[i].number == number && cfg->alts[i].alt == alt)
            return &cfg->alts[i];
    }
    return NULL;
}

int ua101_endpoint_is_in(const ua101_endpoint *ep)
{
    return (ep->address & 0x80) != 0;
}

int ua101_endpoint_is_isochronous(const ua101_endpoint *ep)
{
    return (ep->attributes & 0x03) == TRANSFER_ISOCHRONOUS;
}

int ua101_endpoint_is_bulk(const ua101_endpoint *ep)
{
    return (ep->attributes & 0x03) == TRANSFER_BULK;
}

typedef struct {
    ua101_report_fn report;
    void *ctx;
    int failures;
} checker;

#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 3, 4)))
#endif
static int check(checker *c, int ok, const char *fmt, ...)
{
    char message[160];
    va_list args;

    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    if (!ok)
        c->failures++;
    if (c->report != NULL)
        c->report(c->ctx, ok, message);
    return ok;
}

/* Returns the streaming alternate setting, or NULL if it isn't usable. */
static const ua101_alt *check_audio_interface(const ua101_config *cfg,
                                              checker *c, uint8_t number,
                                              const char *name, int want_in)
{
    const ua101_alt *idle = ua101_find_alt(cfg, number, 0);
    const ua101_alt *streaming = ua101_find_alt(cfg, number, 1);
    const ua101_endpoint *ep;

    check(c, idle != NULL && idle->num_endpoints == 0,
          "%s: alt 0 exists with no endpoints", name);

    if (!check(c, streaming != NULL && streaming->num_endpoints == 1,
               "%s: alt 1 exists with one endpoint", name))
        return NULL;

    ep = &streaming->endpoints[0];
    check(c, ua101_endpoint_is_isochronous(ep), "%s: endpoint is isochronous",
          name);
    check(c, ua101_endpoint_is_in(ep) == want_in, "%s: endpoint direction is %s",
          name, want_in ? "IN" : "OUT");
    check(c, ep->max_packet > 0, "%s: endpoint max packet is not zero", name);

    if (!check(c, streaming->has_format, "%s: format descriptor present", name))
        return NULL;

    check(c, streaming->format_type == 1 && streaming->rate_count == 1,
          "%s: PCM with one fixed sample rate", name);
    check(c, streaming->channels > 0, "%s: channel count is not zero", name);
    check(c, streaming->subframe_bytes == 3 || streaming->subframe_bytes == 4,
          "%s: sample container is 3 or 4 bytes", name);
    check(c, streaming->bit_resolution == 24, "%s: 24-bit resolution", name);
    check(c, streaming->rate_hz > 0, "%s: sample rate is not zero", name);
    return streaming;
}

static void check_midi_interface(const ua101_config *cfg, checker *c)
{
    const ua101_alt *alt = ua101_find_alt(cfg, UA101_IF_MIDI, 0);
    int bulk_in = 0, bulk_out = 0;
    uint8_t i;

    if (!check(c, alt != NULL, "MIDI: alt 0 exists"))
        return;
    for (i = 0; i < alt->num_endpoints; i++) {
        const ua101_endpoint *ep = &alt->endpoints[i];
        if (ua101_endpoint_is_bulk(ep)) {
            if (ua101_endpoint_is_in(ep))
                bulk_in = 1;
            else
                bulk_out = 1;
        }
    }
    check(c, bulk_in && bulk_out, "MIDI: alt 0 has bulk IN and bulk OUT");
}

int ua101_check_config(const ua101_config *cfg, ua101_report_fn report,
                       void *ctx)
{
    checker c;
    const ua101_alt *playback, *capture;

    c.report = report;
    c.ctx = ctx;
    c.failures = 0;

    check(&c, !cfg->truncated, "configuration descriptor is complete");
    check(&c, cfg->num_interfaces == UA101_NUM_INTERFACES,
          "configuration has %d interfaces (found %d)", UA101_NUM_INTERFACES,
          cfg->num_interfaces);

    playback = check_audio_interface(cfg, &c, UA101_IF_PLAYBACK, "Playback", 0);
    capture = check_audio_interface(cfg, &c, UA101_IF_CAPTURE, "Capture", 1);
    check_midi_interface(cfg, &c);

    if (playback != NULL && capture != NULL && playback->has_format &&
        capture->has_format) {
        check(&c, playback->rate_hz == capture->rate_hz,
              "Playback and capture sample rates match (%u and %u Hz)",
              (unsigned)playback->rate_hz, (unsigned)capture->rate_hz);
        check(&c, playback->subframe_bytes == capture->subframe_bytes,
              "Playback and capture sample containers match");

        /*
         * Playback copies capture's packet sizes in frames, so the largest
         * capture packet must also fit in a playback packet.
         */
        {
            unsigned capture_frame_bytes =
                (unsigned)capture->channels * capture->subframe_bytes;
            unsigned playback_frame_bytes =
                (unsigned)playback->channels * playback->subframe_bytes;

            if (capture_frame_bytes > 0 && playback_frame_bytes > 0) {
                unsigned max_frames =
                    capture->endpoints[0].max_packet / capture_frame_bytes;
                check(&c,
                      max_frames * playback_frame_bytes <=
                          playback->endpoints[0].max_packet,
                      "Largest capture packet (%u frames) fits in a playback packet",
                      max_frames);
            }
        }
    }
    return c.failures;
}
