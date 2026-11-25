/* The CoreMIDI side of the per-user agent: see src/ua101_midiports.h. The port making, the
   fixed IDs and the conversion to and from UMP are src/ua101midi.c's, which passed the cable
   test on the unit (2026-11-08); the USB half of that program is not here. */
#include "ua101_midiports.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>

#include <mach/mach_time.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "ua101_link.h"
#include "ua101_midi.h"
#include "ua101_midilink.h"

/* When the helper can't say when to ask again (it isn't installed, it is asleep, or another
   program holds the unit's MIDI interface), ask again after this long: a few times soon, then
   seldom. With a unit simply absent nothing is asked: the helper says when one appears. */
#define RETRY_SOON_SECONDS 10
#define RETRY_LATER_SECONDS 60
#define RETRIES_SOON 6
/* MIDI bytes from apps are handed to the helper in pieces of at most this. */
#define OUT_CHUNK_BYTES 1024

struct ua101_midiports {
    dispatch_queue_t queue; /* everything is used on it, but for what is marked otherwise */
    ua101_midiports_options opt;
    char name[64];
    ua101_midilink *link;
    int attached, stopped, gave_up;
    unsigned retries, retry_serial;
    char said[200]; /* the last state written to the log, so it is written once */

    MIDIClientRef client; /* made at the first attach that succeeds, then kept */
    MIDIEndpointRef source, dest;
    ua101_midi_parser rx_parser;
    ua101_ump_packer rx_packer;
    ua101_midiports_counts counts;
    atomic_ullong out_refused; /* counted on CoreMIDI's thread */
};

static void note(ua101_midiports *a, const char *format, ...)
{
    char when[32];
    struct timeval now;
    struct tm tm;
    va_list args;

    if (a->opt.log == NULL)
        return;
    gettimeofday(&now, NULL);
    localtime_r(&now.tv_sec, &tm);
    strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tm);
    fprintf(a->opt.log, "%s.%03d ua101midiagent: ", when, (int)(now.tv_usec / 1000));
    va_start(args, format);
    vfprintf(a->opt.log, format, args);
    va_end(args);
    fputc('\n', a->opt.log);
    fflush(a->opt.log);
}

/* A state worth a line, written only when it differs from the last one written. */
static void note_state(ua101_midiports *a, const char *format, ...)
{
    char text[sizeof(a->said)];
    va_list args;

    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    if (strcmp(text, a->said) == 0)
        return;
    snprintf(a->said, sizeof(a->said), "%s", text);
    note(a, "%s", text);
}

/* ---- Unit to apps ---- */

/* One message per event list: SysEx must travel in packets of its own. */
static void deliver(ua101_midiports *a, const uint32_t *words, size_t count, uint64_t host_time)
{
    MIDIEventList list;
    MIDIEventPacket *packet;
    OSStatus status;

    if (count == 0 || a->source == 0)
        return;
    packet = MIDIEventListInit(&list, kMIDIProtocol_1_0);
    packet = MIDIEventListAdd(&list, sizeof(list), packet, host_time, count, words);
    status = packet != NULL ? MIDIReceivedEventList(a->source, &list) : kMIDIUnknownError;
    if (status == noErr) {
        a->counts.in_messages++;
        return;
    }
    if (a->counts.in_errors++ == 0)
        note(a, "CoreMIDI didn't take a message from MIDI IN (error %d)", (int)status);
    /* Its server went away under the ports: only a fresh start puts that right. */
    if ((status == kMIDIServerStartErr || status == kMIDIInvalidClient ||
         status == kMIDIObjectNotFound) &&
        a->opt.broken != NULL)
        a->opt.broken(a->opt.context, "CoreMIDI no longer knows the ports");
}

static void from_unit(ua101_midiports *a, const uint8_t *bytes, size_t len, uint64_t host_time)
{
    static const ua101_midi_event cut_event = {UA101_MIDI_SYSEX_CUT, {0}, 0};
    size_t i;

    if (host_time == 0)
        host_time = mach_absolute_time();
    a->counts.in_bytes += len;
    for (i = 0; i < len; i++) {
        ua101_midi_event ev;
        uint32_t words[2];
        int cut;

        ua101_midi_parse(&a->rx_parser, bytes[i], &ev, &cut);
        if (cut)
            deliver(a, words, ua101_ump_pack(&a->rx_packer, &cut_event, words), host_time);
        deliver(a, words, ua101_ump_pack(&a->rx_packer, &ev, words), host_time);
    }
}

/* ---- Apps to unit ---- */

/* On CoreMIDI's thread: the bytes step onto the queue, and from there to the helper. */
static void hand_over(ua101_midiports *a, const uint8_t *bytes, size_t len)
{
    uint8_t *copy;

    if (len == 0 || (copy = malloc(len)) == NULL)
        return;
    memcpy(copy, bytes, len);
    dispatch_async(a->queue, ^{
      if (!a->stopped && a->attached && ua101_midilink_send(a->link, copy, len))
          a->counts.out_bytes += len;
      else
          a->counts.out_unattached += len;
      free(copy);
    });
}

/* Runs on CoreMIDI's receive thread for everything apps send to the destination. It turns
   UMP into MIDI bytes and nothing more: what may reach the unit is the helper's decision. */
static void from_apps(ua101_midiports *a, const MIDIEventList *list)
{
    uint8_t bytes[OUT_CHUNK_BYTES];
    const MIDIEventPacket *packet = &list->packet[0];
    size_t used = 0;
    UInt32 p;

    if (list->protocol != kMIDIProtocol_1_0) {
        atomic_fetch_add(&a->out_refused, list->numPackets);
        return;
    }
    for (p = 0; p < list->numPackets; p++) {
        UInt32 w = 0;

        while (w < packet->wordCount) {
            size_t count = ua101_ump_words(packet->words[w]), n;
            uint8_t message[8];
            int refused;

            if (w + count > packet->wordCount) {
                atomic_fetch_add(&a->out_refused, 1);
                break;
            }
            n = ua101_ump_to_bytes(&packet->words[w], count, message, &refused);
            if (refused)
                atomic_fetch_add(&a->out_refused, 1);
            if (used + n > sizeof(bytes)) {
                hand_over(a, bytes, used);
                used = 0;
            }
            memcpy(bytes + used, message, n);
            used += n;
            w += (UInt32)count;
        }
        packet = MIDIEventPacketNext(packet);
    }
    hand_over(a, bytes, used);
}

/* ---- The ports ---- */

/* Gives a published port its fixed ID, so apps remember it from one run to the next. If the
   ID is taken, it tries a few others; failing all, the port keeps CoreMIDI's random one. */
static void set_port_id(ua101_midiports *a, MIDIEndpointRef endpoint, int is_destination)
{
    unsigned attempt;

    for (attempt = 0; attempt < 8; attempt++) {
        int32_t id = ua101_midi_port_id(a->name, is_destination, attempt);
        OSStatus status = MIDIObjectSetIntegerProperty(endpoint, kMIDIPropertyUniqueID, id);

        if (status == noErr) {
            if (attempt != 0)
                note(a, "the %s's usual ID was taken by another MIDI object: it has ID %d now",
                     is_destination ? "destination" : "source", (int)id);
            return;
        }
        if (status != kMIDIIDNotUnique)
            break;
    }
    note(a, "the %s keeps a random ID: its fixed one couldn't be set",
         is_destination ? "destination" : "source");
}

static void dress(ua101_midiports *a, MIDIEndpointRef endpoint, int is_destination)
{
    MIDIObjectSetStringProperty(endpoint, kMIDIPropertyManufacturer, CFSTR("EDIROL"));
    MIDIObjectSetStringProperty(endpoint, kMIDIPropertyModel, CFSTR("UA-101"));
    if (a->opt.private_ports)
        MIDIObjectSetIntegerProperty(endpoint, kMIDIPropertyPrivate, 1);
    else
        set_port_id(a, endpoint, is_destination);
}

static void remove_ports(ua101_midiports *a)
{
    /* The destination goes first, so nothing new arrives from apps while the rest goes. */
    if (a->dest != 0)
        MIDIEndpointDispose(a->dest);
    a->dest = 0;
    if (a->source != 0)
        MIDIEndpointDispose(a->source);
    a->source = 0;
    /* The client stays: CoreMIDI disposes it when the program ends. */
}

static int make_ports(ua101_midiports *a)
{
    CFStringRef name;
    OSStatus status;

    if (a->source != 0 && a->dest != 0)
        return 1;
    remove_ports(a);
    if (a->client == 0) {
        status = MIDIClientCreateWithBlock(CFSTR("UA-101 MIDI agent"), &a->client, NULL);
        if (status != noErr) {
            a->client = 0;
            note_state(a, "couldn't create a CoreMIDI client (error %d)", (int)status);
            return 0;
        }
    }
    name = CFStringCreateWithCString(NULL, a->name, kCFStringEncodingUTF8);
    if (name == NULL)
        return 0;
    ua101_midi_parser_init(&a->rx_parser);
    ua101_ump_packer_init(&a->rx_packer);
    status = MIDISourceCreateWithProtocol(a->client, name, kMIDIProtocol_1_0, &a->source);
    if (status == noErr) {
        dress(a, a->source, 0);
        status = MIDIDestinationCreateWithProtocol(a->client, name, kMIDIProtocol_1_0, &a->dest,
                                                   ^(const MIDIEventList *list, void *ref) {
                                                     (void)ref;
                                                     from_apps(a, list);
                                                   });
        if (status == noErr)
            dress(a, a->dest, 1);
    }
    CFRelease(name);
    if (status != noErr) {
        note_state(a, "couldn't create the MIDI ports (error %d)", (int)status);
        if (status != noErr && a->dest == 0 && a->source != 0) {
            MIDIEndpointDispose(a->source);
            a->source = 0;
        }
        return 0;
    }
    return 1;
}

/* ---- Following the helper ---- */

static void ask_later(ua101_midiports *a)
{
    unsigned serial = ++a->retry_serial;
    int seconds = a->retries++ < RETRIES_SOON ? RETRY_SOON_SECONDS : RETRY_LATER_SECONDS;

    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)seconds * (int64_t)NSEC_PER_SEC),
                   a->queue, ^{
                     if (!a->stopped && !a->gave_up && !a->attached && serial == a->retry_serial)
                         ua101_midilink_attach(a->link);
                   });
}

static void on_attached(void *context, int status, int watching, const char *why)
{
    ua101_midiports *a = context;

    a->counts.attaches++;
    if (a->stopped)
        return;
    if (status == UA101_LINK_OK) {
        if (make_ports(a)) {
            a->attached = 1;
            a->retries = 0;
            note_state(a, "the UA-101 is there: \"%s\" is published, in and out", a->name);
        } else {
            /* CoreMIDI wasn't ready: the unit is let go, and asked for again later. */
            a->attached = 0;
            ua101_midilink_detach(a->link);
            ask_later(a);
        }
        return;
    }
    a->attached = 0;
    remove_ports(a);
    if (status == UA101_LINK_ERR_DENIED) {
        /* A system account, or an older helper that serves only the account that installed
           it: nothing to do, ever. */
        a->gave_up = 1;
        note_state(a, "this user may not use the UA-101 (%s): no MIDI ports here", why);
        return;
    }
    if (status == UA101_LINK_ERR_ABSENT && watching) {
        /* No unit on the bus: the helper says when one appears. Nothing runs meanwhile. */
        note_state(a, "no UA-101 is connected: no MIDI ports until it is");
        return;
    }
    note_state(a, "the UA-101's MIDI can't be had now (%s): no MIDI ports, asking again later",
               why);
    ask_later(a);
}

static void on_midi(void *context, const uint8_t *bytes, size_t len, uint64_t host_time)
{
    ua101_midiports *a = context;

    if (!a->stopped && a->attached)
        from_unit(a, bytes, len, host_time);
}

/* The unit came, went or was lost, the Mac woke, or the helper started again. The ports stay
   as they are until the helper has answered: if the unit is still there, apps never see them
   go. */
static void on_changed(void *context)
{
    ua101_midiports *a = context;

    if (a->stopped || a->gave_up)
        return;
    a->attached = 0;
    a->retry_serial++;
    ua101_midilink_attach(a->link);
}

ua101_midiports *ua101_midiports_start(dispatch_queue_t queue,
                                       const ua101_midiports_options *options)
{
    static const ua101_midilink_events events = {on_attached, on_midi, on_changed};
    /* Never freed: CoreMIDI's thread may still be inside the destination's block when the
       ports are removed. One small block for the life of the program. */
    ua101_midiports *a = calloc(1, sizeof(*a));

    if (a == NULL)
        return NULL;
    a->queue = queue;
    dispatch_retain(queue);
    a->opt = *options;
    snprintf(a->name, sizeof(a->name), "%s",
             options->name != NULL ? options->name : UA101_MIDIPORTS_NAME);
    a->link = ua101_midilink_open(queue, &events, a);
    if (a->link == NULL)
        return NULL;
    dispatch_async(queue, ^{
      if (!a->stopped)
          ua101_midilink_attach(a->link);
    });
    return a;
}

void ua101_midiports_stop(ua101_midiports *a)
{
    if (a->stopped)
        return;
    a->stopped = 1;
    a->attached = 0;
    remove_ports(a);
    ua101_midilink_close(a->link);
}

void ua101_midiports_get_counts(ua101_midiports *a, ua101_midiports_counts *out)
{
    *out = a->counts;
    out->attached = a->attached;
    out->ports_up = a->source != 0 && a->dest != 0;
    out->source = (uint32_t)a->source;
    out->destination = (uint32_t)a->dest;
    out->out_refused = atomic_load(&a->out_refused);
}
