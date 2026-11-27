/*
 * The control app's way to the unit, without a unit: the app's real engine (through
 * app/panel_shim.h), its real end of the link (src/ua101_ctllink.c) and the helper's real
 * service (src/ua101_service.c) in one process.
 *
 * What stands in for hardware: the unit's MIDI interface (src/ua101_midiport.h) is a pretend
 * one here with the pretend UA-101 (src/ua101_ctlsim.c) behind it, and it keeps a list of every
 * message that would have gone down the USB cable. The USB audio engine and the bus watcher are
 * pretend too, as in tests/test_service.c.
 *
 * The point of most cases: whatever the app does, with its own guards switched off or without
 * its engine at all, the only message that reaches the cable is the request for the settings;
 * and, since link version 5, "pause saving" and
 * "resume saving", since version 6 also the patch bay source of OUTPUT 1-2, since version 7
 * of all five output pairs and the volume of INPUT 1, since version 8 at any value up to 0 dB
 * (0x200000, the first digit sent being the highest), since version 9 also the pan of INPUT 1,
 * since version 10 at any value from 0x0000 to 0x8000 (the first digit sent being the
 * highest), since version 11 also the on/off of INPUT 1 (0 or 1), since version 13 those
 * three controls of every channel of the mixer (MONITOR OUT L and R without a pan, INPUT 1 to
 * 10, OUTPUT 1 to 10), since version 14 also the stereo link (0 or 1) of every pair and the
 * solo (0 for none, or a channel 1 to 10) of the INPUT and the OUTPUT group, and since version
 * 17, by rules of their own, 37 exact Load, Store
 * and Reset messages (test_memory_rules, test_memory), and still nothing
 * else. Up to link version 17 each of those changes also needed the user's go-ahead file;
 * since version 18 the helper looks for none: nothing here makes one, and the helper is
 * given no place to look.
 *
 * Since version 12 a MIDI client is here too, with its real end of the link
 * (src/ua101_midilink.c), beside the control app: what arrives at the MIDI IN socket (cable 0)
 * reaches MIDI clients only, what a MIDI client sends reaches the MIDI OUT socket (cable 0)
 * only, at the wire's speed, never the control cable and never as SysEx addressed to the
 * UA-101 itself; and the interface is held while either kind is attached.
 */
#include <math.h>
#include <dispatch/dispatch.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <xpc/xpc.h>

#include "../app/panel_shim.h"
#include "../src/ua101_control.h"
#include "../src/ua101_ctllink.h"
#include "../src/ua101_ctlsim.h"
#include "../src/ua101_engine.h"
#include "../src/ua101_link.h"
#include "../src/ua101_midi.h"
#include "../src/ua101_midilink.h"
#include "../src/ua101_midiport.h"
#include "../src/ua101_service.h"
#include "../src/ua101_shared.h"
#include "../src/ua101_watch.h"

/* Built only into this test (-DUA101_SERVICE_TESTING): leaves out one of the helper's value
   or channel checks, to show each keeps to the rule alone (src/ua101_service.c). */
extern unsigned ua101_service_test_skip;
int ua101_service_test_volume(const uint8_t *msg, size_t len, const ua101_msg *m);
int ua101_service_test_pan(const uint8_t *msg, size_t len, const ua101_msg *m);
int ua101_service_test_on(const uint8_t *msg, size_t len, const ua101_msg *m);
extern unsigned ua101_service_test_links, ua101_service_test_solos; /* the two lists, see there */
int ua101_service_test_link(const uint8_t *msg, size_t len, const ua101_msg *m);
int ua101_service_test_solo(const uint8_t *msg, size_t len, const ua101_msg *m);
/* Load, Store and Reset (link version 17): their list, the check by itself, and time added to
   the helper's clock, so that 5 and 30 seconds pass without waiting. */
extern uint64_t ua101_service_test_memory, ua101_service_test_clock_us;
uint64_t ua101_service_test_memory_command(const uint8_t *msg, size_t len, const ua101_msg *m);

static int failures, checks;

#define CHECK(cond, ...)                                                                           \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(cond)) {                                                                             \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                            \
            printf(__VA_ARGS__);                                                                   \
            printf("\n");                                                                          \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

static double g_tick_s;

static double now_s(void)
{
    return (double)mach_absolute_time() * g_tick_s;
}

static uint64_t now_us(void)
{
    return (uint64_t)(now_s() * 1e6);
}

static int wait_until(int (^cond)(void), double seconds)
{
    double limit = now_s() + seconds;

    while (now_s() < limit) {
        if (cond())
            return 1;
        usleep(2000);
    }
    return cond();
}

#define WAIT(cond, seconds) wait_until(^{ return (int)(cond); }, seconds)

/* --- The pretend unit: the bus and the Mac's sleep. --- */

static _Atomic int g_on_bus;

static struct {
    dispatch_queue_t queue; /* the service's */
    ua101_watch_events events;
} W;

int ua101_watch_start(dispatch_queue_t queue, const ua101_watch_events *events, int *units)
{
    W.queue = queue;
    W.events = *events;
    *units = atomic_load(&g_on_bus);
    return 1;
}

/* --- The pretend unit: USB audio. Its stream is a fixed stretch of sound, written once. --- */

#define TONE_FRAMES 2400

struct ua101_engine {
    ua101_shared *sh;
    pthread_t thread;
    int thread_running;
    atomic_int stop_request;
    double started, stopped;
};

static _Atomic int g_streaming;

ua101_engine *ua101_engine_open(const ua101_engine_options *opt, ua101_engine_format *format,
                                FILE *log, char *why, size_t why_bytes)
{
    (void)opt;
    (void)log;
    if (!atomic_load(&g_on_bus)) {
        snprintf(why, why_bytes, "no UA-101 is connected (pretend)");
        return NULL;
    }
    format->rate_hz = 48000;
    format->capture_channels = 12;
    format->playback_channels = 10;
    return calloc(1, sizeof(ua101_engine));
}

static void *pretend_stream(void *arg)
{
    ua101_engine *e = arg;

    while (!atomic_load(&e->stop_request)) {
        usleep(5000);
        atomic_fetch_add(&e->sh->heartbeat, 1);
    }
    return NULL;
}

int ua101_engine_start(ua101_engine *e, ua101_shared *shared, size_t shared_bytes)
{
    static int32_t in[TONE_FRAMES * 12], out[TONE_FRAMES * 10];
    int f, c;

    (void)shared_bytes;
    e->sh = shared;
    atomic_store(&e->stop_request, 0);
    ua101_frame_ring_reset(&shared->capture);
    ua101_frame_ring_reset(&shared->playback);
    /* Channel c holds a steady (c + 1) / 128 of full scale; channel 3 is negative; one sample of
       channel 0 is the most negative there is. Playback is quieter. */
    for (f = 0; f < TONE_FRAMES; f++) {
        for (c = 0; c < 12; c++)
            in[f * 12 + c] = (c == 3 ? -1 : 1) * (int32_t)((c + 1) << 24);
        for (c = 0; c < 10; c++)
            out[f * 10 + c] = (int32_t)((c + 1) << 16);
    }
    in[100 * 12 + 0] = INT32_MIN;
    ua101_frame_ring_write(&shared->capture, 0, in, TONE_FRAMES);
    ua101_frame_ring_write(&shared->playback, 0, out, TONE_FRAMES);
    ua101_shared_new_seed(&shared->clock);
    ua101_shared_publish(&shared->clock, 0, (double)mach_absolute_time(), 1000.0);
    atomic_store(&shared->state, UA101_STATE_RUNNING);
    e->started = now_s();
    if (pthread_create(&e->thread, NULL, pretend_stream, e) != 0)
        return 0;
    e->thread_running = 1;
    atomic_store(&g_streaming, 1);
    return 1;
}

const char *ua101_engine_failure(const ua101_engine *e)
{
    (void)e;
    return NULL;
}

void ua101_engine_stop(ua101_engine *e)
{
    if (!e->thread_running)
        return;
    atomic_store(&e->stop_request, 1);
    pthread_join(e->thread, NULL);
    e->thread_running = 0;
    e->stopped = now_s();
    atomic_store(&e->sh->state, UA101_STATE_STOPPED);
    atomic_store(&g_streaming, 0);
}

void ua101_engine_summarize(const ua101_engine *e, ua101_engine_summary *out)
{
    memset(out, 0, sizeof(*out));
    out->seconds = (e->stopped > 0.0 ? e->stopped : now_s()) - e->started;
}

void ua101_engine_close(ua101_engine *e)
{
    if (e == NULL)
        return;
    ua101_engine_stop(e);
    free(e);
}

/* --- The pretend unit: its MIDI interface, with the pretend UA-101 behind it. --- */

#define WIRE_LOG 256

static struct {
    /* Used on the service's queue only. */
    ua101_sim sim;
    ua101_sysex_rx rx; /* puts together what the helper sends down the cable */
    ua101_midiport_events events;
    dispatch_queue_t queue;
    int open;
    /* Read by the test. */
    _Atomic int can_open;       /* 0: another program has the interface */
    _Atomic int opens, closes;
    _Atomic int wire_messages;  /* whole messages that reached the cable */
    _Atomic int wire_reads;     /* of those, the settings request, byte for byte */
    _Atomic int wire_saving;    /* of those, "pause saving" or "resume saving", byte for byte */
    _Atomic int wire_patch;     /* of those, an output pair's patch bay source, 1 to 11, byte for byte */
    _Atomic int wire_volume;    /* of those, a passed channel's volume, at most 0 dB */
    _Atomic int wire_pan;       /* of those, a passed channel's pan, 0x0000 to 0x8000 */
    _Atomic int wire_on;        /* of those, a passed channel's on/off, 0 or 1 */
    _Atomic int wire_link;      /* of those, a passed pair's stereo link, 0 or 1 */
    _Atomic int wire_solo;      /* of those, a passed group's solo, 0 to 10 */
    _Atomic int wire_memory;    /* of those, one of the 37 Load, Store and Reset messages */
    _Atomic int wire_memory_reads; /* of those, one of the five memory reads (link version 20) */
    _Atomic int wire_other;     /* packets on a cable the unit doesn't have (neither the control
                                   cable nor the MIDI sockets'), or that made no sense */
    /* What reached the MIDI OUT socket (cable 0), as MIDI bytes in order. */
    uint8_t midi[1 << 17];      /* on the service's queue only */
    size_t midi_len;
    _Atomic int wire_midi_bytes;
    _Atomic int wire_midi_most; /* the most MIDI bytes in any one write */
    _Atomic int midi_stall;     /* 1: the unit takes no write for MIDI OUT */
    _Atomic int midi_stalled_writes;
} P;

struct ua101_midiport {
    int unused;
};
static struct ua101_midiport g_port;

/* The patch bay message for one output pair and source, from the app's builder. */
static size_t patch_message(unsigned output, unsigned source, uint8_t out[UA101_CTLMSG_MAX_BYTES])
{
    ua101_msg m;

    memset(&m, 0, sizeof(m));
    m.kind = UA101_MSG_FIELD;
    m.field.kind = UA101_F_PATCH;
    m.field.index = (uint8_t)output;
    m.value = source;
    return ua101_ctlmsg_build(&m, out);
}

/* 1 only for the patch bay changes link version 9 passes on: an output pair 0 to 4, source 1
   to 11. */
static int is_passed_patch(const uint8_t *msg, size_t len)
{
    uint8_t want[UA101_CTLMSG_MAX_BYTES];
    unsigned output, source;

    for (output = 0; output < 5; output++)
        for (source = 1; source <= 11; source++)
            if (patch_message(output, source, want) == len && memcmp(msg, want, len) == 0)
                return 1;
    return 0;
}

/*
 * WHICH CHANNELS LINK VERSION 13 PASSES ON, said here a second time and in other words than
 * the helper's (three bit masks in src/ua101_link.h): by group and channel, in plain
 * comparisons. Group 0 is MONITOR OUT (channel 0 L, 1 R), group 1 INPUT (channel 0 to 9 are
 * INPUT 1 to 10; 8 and 9 are OPTICAL IN L and R), group 2 OUTPUT (channel 0 to 9 are OUTPUT 1
 * to 10, the computer's channels in the monitor mix).
 */
static int t_volume(unsigned group, unsigned channel)
{
    return (group == 0 && channel < 2) || ((group == 1 || group == 2) && channel < 10); /* all 22 */
}

static int t_pan(unsigned group, unsigned channel)
{
    return (group == 1 || group == 2) && channel < 10; /* INPUT 1 to 10, OUTPUT 1 to 10 */
}

static int t_on(unsigned group, unsigned channel)
{
    return (group == 0 && channel < 2) || ((group == 1 || group == 2) && channel < 10); /* all 22 */
}

/* How many channels that is, counted by hand, and the bits the helper must name them by
   (bit 0 and 1 MONITOR OUT L and R, 2 to 11 INPUT 1 to 10, 12 to 21 OUTPUT 1 to 10). */
#define T_VOLUMES 22
#define T_PANS 20
#define T_ONS 22
#define T_VOLUME_BITS 0x3FFFFFull
#define T_PAN_BITS 0x3FFFFCull
#define T_ON_BITS 0x3FFFFFull

/*
 * WHICH PAIRS' LINK AND WHICH GROUPS' SOLO LINK VERSION 14 PASSES ON, said here a second time
 * and in other words than the helper's (two bit masks in src/ua101_link.h). A pair is named by
 * its group and its number in the group: MONITOR OUT has the one pair L-R; INPUT and OUTPUT
 * have five each, 1-2 to 9-10. A solo is one number a group: 0 for none, 1 to 10 for the one
 * soloed channel; MONITOR OUT has none.
 */
static int t_link(unsigned group, unsigned pair)
{
    return (group == 0 && pair == 0) || ((group == 1 || group == 2) && pair < 5); /* all 11 */
}

static int t_solo(unsigned group)
{
    return group == 1 || group == 2; /* the INPUT group and the OUTPUT group */
}

/* How many that is, counted by hand, and the bits the helper must name them by (pairs: bit 0
   MONITOR OUT L-R, 1 to 5 INPUT 1-2 to 9-10, 6 to 10 OUTPUT 1-2 to 9-10; groups: bit 0 INPUT,
   bit 1 OUTPUT). A solo takes eleven values: 0, and 1 to 10. */
#define T_LINKS 11
#define T_SOLOS 2
#define T_SOLO_VALUES 11
#define T_LINK_BITS 0x7FFull
#define T_SOLO_BITS 0x3ull

/* The volume message for one channel, from the app's builder. */
static size_t volume_message(unsigned group, unsigned channel, uint32_t value,
                             uint8_t out[UA101_CTLMSG_MAX_BYTES])
{
    ua101_msg m;

    memset(&m, 0, sizeof(m));
    m.kind = UA101_MSG_FIELD;
    m.field.kind = UA101_F_VOLUME;
    m.field.group = (uint8_t)group;
    m.field.index = (uint8_t)channel;
    m.value = value;
    return ua101_ctlmsg_build(&m, out);
}

/* A volume message written out by hand, as docs/control-protocol.md gives it: any address,
   six digits with the highest first, a right checksum. */
static void hand_volume(uint8_t out[18], unsigned a1, unsigned a2, unsigned a3, uint32_t value)
{
    static const uint8_t head[7] = {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00};
    unsigned i;

    memcpy(out, head, 7);
    out[7] = (uint8_t)a1;
    out[8] = (uint8_t)a2;
    out[9] = (uint8_t)a3;
    for (i = 0; i < 6; i++)
        out[10 + i] = (uint8_t)((value >> (20 - 4 * i)) & 0xFu);
    out[16] = ua101_ctl_checksum(out + 6, 10);
    out[17] = 0xF7;
}

/* The highest volume passed on since link version 19: the top of the unit's range, about
   +12 dB (up to version 18, 0x200000: 0 dB). */
#define T_VOLUME_TOP 0x7FFFFFu

/* The test's own reading of the rule, written a third way: the six digits, first sent first,
   compared one by one with those of the top (7 F F F F F), as words are put in order. 1 if the
   value comes before the top or is it. */
static int volume_at_most_top(uint32_t value)
{
    static const unsigned top[6] = {7, 15, 15, 15, 15, 15};
    unsigned i, digit;

    if (value > 0xFFFFFFu)
        return 0;
    for (i = 0; i < 6; i++) {
        digit = (value >> (20 - 4 * i)) & 0xFu;
        if (digit != top[i])
            return digit < top[i];
    }
    return 1;
}

/* 1 only for a volume change this link version passes on: a channel of t_volume, at most the
   top (0 dB up to version 18). */
static int is_passed_volume(const uint8_t *msg, size_t len)
{
    uint8_t want[18];
    uint32_t value = 0;
    unsigned i;

    if (len != 18 || msg[7] < 3 || msg[7] > 5 || !t_volume(msg[7] - 3u, msg[8]))
        return 0;
    for (i = 0; i < 6; i++)
        value = (value << 4) | (msg[10 + i] & 0xFu);
    hand_volume(want, msg[7], msg[8], 0x07, value);
    return memcmp(msg, want, 18) == 0 && volume_at_most_top(value);
}

/* The pan message for one channel, from the app's builder. */
static size_t pan_message(unsigned group, unsigned channel, uint32_t value,
                          uint8_t out[UA101_CTLMSG_MAX_BYTES])
{
    ua101_msg m;

    memset(&m, 0, sizeof(m));
    m.kind = UA101_MSG_FIELD;
    m.field.kind = UA101_F_PAN;
    m.field.group = (uint8_t)group;
    m.field.index = (uint8_t)channel;
    m.value = value;
    return ua101_ctlmsg_build(&m, out);
}

/* A pan message written out by hand, as docs/control-protocol.md gives it: any address, four
   digits with the highest first, a right checksum. */
static void hand_pan(uint8_t out[16], unsigned a1, unsigned a2, unsigned a3, uint32_t value)
{
    static const uint8_t head[7] = {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00};
    unsigned i;

    memcpy(out, head, 7);
    out[7] = (uint8_t)a1;
    out[8] = (uint8_t)a2;
    out[9] = (uint8_t)a3;
    for (i = 0; i < 4; i++)
        out[10 + i] = (uint8_t)((value >> (12 - 4 * i)) & 0xFu);
    out[14] = ua101_ctl_checksum(out + 6, 8);
    out[15] = 0xF7;
}

/* The test's own reading of the pan rule, written a third way: the four digits, the first
   sent being the highest (proven on the unit, 2026-11-12), are compared with the top of the
   range (8 0 0 0) digit by digit, as words are put in order. 1 if they don't come after it. */
static int pan_in_range(uint32_t value)
{
    static const unsigned top[4] = {8, 0, 0, 0};
    unsigned i;

    if (value > 0xFFFFu)
        return 0;
    for (i = 0; i < 4; i++) {
        unsigned digit = (value >> (12 - 4 * i)) & 0xFu;

        if (digit > top[i])
            return 0;
        if (digit < top[i])
            break;
    }
    return 1;
}

/* 1 only for a pan change link version 13 passes on: a channel of t_pan, 0x0000 to 0x8000. */
static int is_passed_pan(const uint8_t *msg, size_t len)
{
    uint8_t want[16];
    uint32_t value = 0;
    unsigned i;

    if (len != 16 || msg[7] < 3 || msg[7] > 5 || !t_pan(msg[7] - 3u, msg[8]))
        return 0;
    for (i = 0; i < 4; i++)
        value = (value << 4) | (msg[10 + i] & 0xFu);
    hand_pan(want, msg[7], msg[8], 0x03, value);
    return memcmp(msg, want, 16) == 0 && pan_in_range(value);
}

/* The on/off message for one channel, from the app's builder. */
static size_t on_message(unsigned group, unsigned channel, uint32_t value,
                         uint8_t out[UA101_CTLMSG_MAX_BYTES])
{
    ua101_msg m;

    memset(&m, 0, sizeof(m));
    m.kind = UA101_MSG_FIELD;
    m.field.kind = UA101_F_ON;
    m.field.group = (uint8_t)group;
    m.field.index = (uint8_t)channel;
    m.value = value;
    return ua101_ctlmsg_build(&m, out);
}

/* An on/off message written out by hand, as docs/control-protocol.md gives it: any address,
   one byte, a right checksum. */
static void hand_on(uint8_t out[13], unsigned a1, unsigned a2, unsigned a3, unsigned value)
{
    static const uint8_t head[7] = {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00};

    memcpy(out, head, 7);
    out[7] = (uint8_t)a1;
    out[8] = (uint8_t)a2;
    out[9] = (uint8_t)a3;
    out[10] = (uint8_t)value;
    out[11] = ua101_ctl_checksum(out + 6, 5);
    out[12] = 0xF7;
}

/* 1 only for an on/off change link version 13 passes on: a channel of t_on, the byte 0 or 1.
   Written a third way: the address and the byte compared one by one, the checksum worked out
   here by counting down from 0x80 (for INPUT 1 that is 7A and 79). */
static int is_passed_on(const uint8_t *msg, size_t len)
{
    unsigned sum;

    if (len != 13 || msg[0] != 0xF0 || msg[1] != 0x41 || msg[2] != 0x10 || msg[3] != 0x00 ||
        msg[4] != 0x69 || msg[5] != 0x12 || msg[6] != 0x00 || msg[7] < 3 || msg[7] > 5 ||
        !t_on(msg[7] - 3u, msg[8]) || msg[9] != 0x02 || msg[10] > 1 || msg[12] != 0xF7)
        return 0;
    sum = (unsigned)msg[7] + msg[8] + 2u + msg[10];
    return msg[11] == (uint8_t)((0x80u - (sum & 0x7Fu)) & 0x7Fu);
}

/* The link message for one pair, from the app's builder. */
static size_t link_message(unsigned group, unsigned pair, uint32_t value,
                           uint8_t out[UA101_CTLMSG_MAX_BYTES])
{
    ua101_msg m;

    memset(&m, 0, sizeof(m));
    m.kind = UA101_MSG_FIELD;
    m.field.kind = UA101_F_LINK;
    m.field.group = (uint8_t)group;
    m.field.index = (uint8_t)pair;
    m.value = value;
    return ua101_ctlmsg_build(&m, out);
}

/* 1 only for a link change link version 14 passes on: a pair of t_link, the byte 0 or 1. The
   pair's first channel is in the address (00, 02, 04, 06, 08), the control is 01. Written a
   third way: the address and the byte compared one by one, the checksum worked out here by
   counting down from 0x80 (for INPUT 1-2 that is 7B and 7A). hand_on makes such a message by
   hand: it writes any 13-byte message with one byte of payload. */
static int is_passed_link(const uint8_t *msg, size_t len)
{
    unsigned sum;

    if (len != 13 || msg[0] != 0xF0 || msg[1] != 0x41 || msg[2] != 0x10 || msg[3] != 0x00 ||
        msg[4] != 0x69 || msg[5] != 0x12 || msg[6] != 0x00 || msg[7] < 3 || msg[7] > 5 ||
        (msg[8] % 2u) != 0 || !t_link(msg[7] - 3u, msg[8] / 2u) || msg[9] != 0x01 || msg[10] > 1 ||
        msg[12] != 0xF7)
        return 0;
    sum = (unsigned)msg[7] + msg[8] + 1u + msg[10];
    return msg[11] == (uint8_t)((0x80u - (sum & 0x7Fu)) & 0x7Fu);
}

/* The solo message for one group, from the app's builder. */
static size_t solo_message(unsigned group, uint32_t value, uint8_t out[UA101_CTLMSG_MAX_BYTES])
{
    ua101_msg m;

    memset(&m, 0, sizeof(m));
    m.kind = UA101_MSG_FIELD;
    m.field.kind = UA101_F_SOLO;
    m.field.group = (uint8_t)group;
    m.value = value;
    return ua101_ctlmsg_build(&m, out);
}

/* 1 only for a solo change link version 14 passes on: a group of t_solo, the byte 0 to 10, at
   the address 00 <group+3> 00 00. Written a third way, as is_passed_link is. */
static int is_passed_solo(const uint8_t *msg, size_t len)
{
    unsigned sum;

    if (len != 13 || msg[0] != 0xF0 || msg[1] != 0x41 || msg[2] != 0x10 || msg[3] != 0x00 ||
        msg[4] != 0x69 || msg[5] != 0x12 || msg[6] != 0x00 || msg[7] < 3 || msg[7] > 5 ||
        !t_solo(msg[7] - 3u) || msg[8] != 0x00 || msg[9] != 0x00 || msg[10] >= T_SOLO_VALUES ||
        msg[12] != 0xF7)
        return 0;
    sum = (unsigned)msg[7] + msg[10];
    return msg[11] == (uint8_t)((0x80u - (sum & 0x7Fu)) & 0x7Fu);
}

/* A Load, Store or Reset message from the app's builder: kind is a UA101_MSG_ kind, memory 0
   to 4, sections the four section bytes (switches, patch row 0, patch bay, mixer), each 0 or 1. */
static size_t memory_message(unsigned kind, unsigned memory, const uint8_t sections[4],
                             uint8_t out[UA101_CTLMSG_MAX_BYTES])
{
    ua101_msg m;

    memset(&m, 0, sizeof(m));
    m.kind = (ua101_msg_kind)kind;
    m.memory = (uint8_t)memory;
    if (sections != NULL)
        memcpy(m.sections, sections, 4);
    return ua101_ctlmsg_build(&m, out);
}

/* A 16-byte message at 00 00 <a2> <a3> with four bytes, written out by hand with a right
   checksum: what a Load or a Store looks like, and their neighbours. */
static void hand_memory(uint8_t out[16], unsigned a2, unsigned a3, const uint8_t d[4])
{
    static const uint8_t head[8] = {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x00};

    memcpy(out, head, 8);
    out[8] = (uint8_t)a2;
    out[9] = (uint8_t)a3;
    memcpy(out + 10, d, 4);
    out[14] = ua101_ctl_checksum(out + 6, 8);
    out[15] = 0xF7;
}

/* WHICH LOAD, STORE AND RESET MESSAGES LINK VERSION 17 PASSES ON, said here a third way, in
   plain comparisons of the bytes at their places, the checksum worked out by counting down
   from 0x80: 37 messages.
     Load:  16 bytes, address 00 00 <03 to 07> 08, then 00 00 <00 or 01> <00 or 01>, not both 00.
     Store: 16 bytes, address 00 00 <03 to 07> 04, then 00 00 <00 or 02> <00 or 02>, not both 00.
     Reset: 13 bytes, one byte 00, at 00 00 00 03 (the settings in use), 00 00 <03 to 07> 03
            (Memory 1 to 5), or 00 00 00 02 (everything). */
#define T_MEMORY_MESSAGES 37
/* The memory reads test_memory_reads sends through the engine (link version 20): each of the
   five once, and Memory 1 once more. */
#define T_MEMORY_READS 6
static int is_passed_memory(const uint8_t *msg, size_t len)
{
    unsigned sum, i;

    if ((len != 13 && len != 16) || msg[0] != 0xF0 || msg[1] != 0x41 || msg[2] != 0x10 ||
        msg[3] != 0x00 || msg[4] != 0x69 || msg[5] != 0x12 || msg[6] != 0x00 || msg[7] != 0x00 ||
        msg[len - 1] != 0xF7)
        return 0;
    for (sum = 0, i = 6; i < len - 2; i++)
        sum += msg[i];
    if (msg[len - 2] != (uint8_t)((0x80u - (sum & 0x7Fu)) & 0x7Fu))
        return 0;
    if (len == 13) {
        if (msg[10] != 0x00)
            return 0;
        if (msg[8] == 0x00)
            return msg[9] == 0x03 || msg[9] == 0x02;
        return msg[8] >= 0x03 && msg[8] <= 0x07 && msg[9] == 0x03;
    }
    if (msg[8] < 0x03 || msg[8] > 0x07 || msg[10] != 0x00 || msg[11] != 0x00)
        return 0;
    if (msg[9] == 0x08)
        return msg[12] <= 1 && msg[13] <= 1 && (msg[12] | msg[13]) != 0;
    if (msg[9] == 0x04)
        return (msg[12] == 0 || msg[12] == 2) && (msg[13] == 0 || msg[13] == 2) &&
               (msg[12] | msg[13]) != 0;
    return 0;
}

/* The 37, one after another, from the app's builder: n 0 to 36. Returns the length. */
static size_t memory_nth(unsigned n, uint8_t out[UA101_CTLMSG_MAX_BYTES])
{
    static const uint8_t parts[3][4] = {{0, 0, 1, 0}, {0, 0, 0, 1}, {0, 0, 1, 1}};

    if (n < 15)
        return memory_message(UA101_MSG_LOAD, n / 3u, parts[n % 3u], out);
    if (n < 30)
        return memory_message(UA101_MSG_STORE, (n - 15u) / 3u, parts[n % 3u], out);
    if (n < 35)
        return memory_message(UA101_MSG_RESET_MEMORY, n - 30u, NULL, out);
    return memory_message(n == 35 ? UA101_MSG_RESET_CURRENT : UA101_MSG_RESET_ALL, 0, NULL, out);
}

/* The unit's answers, as USB would bring them: 64 bytes a read, with a packet from the MIDI IN
   socket (cable 0) between the reads. */
static void port_deliver(void)
{
    const uint8_t *msg;
    size_t len;

    while (ua101_sim_next(&P.sim, &msg, &len)) {
        uint8_t packets[4 * ((UA101_CTL_BLOCK_MESSAGE_BYTES + 2) / 3)];
        size_t packed = ua101_ctl_pack_sysex(msg, len, UA101_CTL_CABLE, packets, sizeof(packets));
        size_t at;

        for (at = 0; at < packed; at += 64) {
            static const uint8_t note[4] = {0x09, 0x90, 0x3C, 0x40}; /* cable 0: note on */
            size_t n = packed - at < 64 ? packed - at : 64;

            P.events.packets(NULL, packets + at, n, mach_absolute_time());
            if (!P.open)
                return;
            P.events.packets(NULL, note, sizeof(note), mach_absolute_time());
        }
    }
}

ua101_midiport *ua101_midiport_open(dispatch_queue_t queue, const ua101_midiport_events *events,
                                    void *context, FILE *log, char *why, size_t why_bytes)
{
    (void)context;
    (void)log;
    if (!atomic_load(&g_on_bus)) {
        snprintf(why, why_bytes, "no UA-101 in Hi-Speed mode was found (pretend)");
        return NULL;
    }
    if (!atomic_load(&P.can_open)) {
        snprintf(why, why_bytes, "couldn't open the MIDI interface (pretend): another program "
                                 "is using it");
        return NULL;
    }
    P.queue = queue;
    P.events = *events;
    P.open = 1;
    ua101_sysex_rx_init(&P.rx);
    atomic_fetch_add(&P.opens, 1);
    return &g_port;
}

int ua101_midiport_send(ua101_midiport *p, const uint8_t *packets, size_t bytes)
{
    size_t i;
    int midi_bytes = 0;

    if (p != &g_port || !P.open || bytes % 4 != 0)
        return 0;
    /* A write for the MIDI OUT socket that the unit doesn't take in time (pretend). */
    if (atomic_load(&P.midi_stall) && bytes != 0 && (packets[0] >> 4) == UA101_MIDI_CABLE) {
        atomic_fetch_add(&P.midi_stalled_writes, 1);
        return 0;
    }
    for (i = 0; i < bytes; i += 4) {
        const uint8_t *msg = NULL;
        size_t len = 0;

        if ((packets[i] >> 4) == UA101_MIDI_CABLE) {
            uint8_t cable, b[3];
            int malformed;
            size_t n = ua101_usbmidi_unpack(packets + i, &cable, b, &malformed);

            if (malformed || n == 0) {
                atomic_fetch_add(&P.wire_other, 1);
            } else if (P.midi_len + n <= sizeof(P.midi)) {
                memcpy(P.midi + P.midi_len, b, n);
                P.midi_len += n;
                midi_bytes += (int)n;
            }
            continue;
        }
        if ((packets[i] >> 4) != UA101_CTL_CABLE)
            atomic_fetch_add(&P.wire_other, 1);
        if (ua101_sysex_rx_feed(&P.rx, packets + i, &msg, &len)) {
            atomic_fetch_add(&P.wire_messages, 1);
            if (ua101_ctl_is_read_request(msg, len))
                atomic_fetch_add(&P.wire_reads, 1);
            else if (panel_is_saving_message(msg, len))
                atomic_fetch_add(&P.wire_saving, 1);
            else if (is_passed_patch(msg, len))
                atomic_fetch_add(&P.wire_patch, 1);
            else if (is_passed_volume(msg, len))
                atomic_fetch_add(&P.wire_volume, 1);
            else if (is_passed_pan(msg, len))
                atomic_fetch_add(&P.wire_pan, 1);
            else if (is_passed_on(msg, len))
                atomic_fetch_add(&P.wire_on, 1);
            else if (is_passed_link(msg, len))
                atomic_fetch_add(&P.wire_link, 1);
            else if (is_passed_solo(msg, len))
                atomic_fetch_add(&P.wire_solo, 1);
            else if (is_passed_memory(msg, len))
                atomic_fetch_add(&P.wire_memory, 1);
            else if (ua101_ctl_memory_read_request(msg, len) >= 0)
                atomic_fetch_add(&P.wire_memory_reads, 1);
            ua101_sim_receive(&P.sim, msg, len);
        }
    }
    if (midi_bytes != 0) {
        if (midi_bytes > atomic_load(&P.wire_midi_most))
            atomic_store(&P.wire_midi_most, midi_bytes);
        atomic_fetch_add(&P.wire_midi_bytes, midi_bytes);
    }
    /* The answer comes on a later turn, as from a real unit. */
    dispatch_async(P.queue, ^{
      if (P.open)
          port_deliver();
    });
    return 1;
}

void ua101_midiport_close(ua101_midiport *p)
{
    if (p != &g_port || !P.open)
        return;
    P.open = 0;
    atomic_fetch_add(&P.closes, 1);
}

/* The test's hand on the pretend unit, on the service's queue as IOKit's callbacks would be. */
static void on_service_queue(void (^work)(void))
{
    dispatch_sync(W.queue, work);
}

static void unit_goes(void)
{
    atomic_store(&g_on_bus, 0);
    on_service_queue(^{
      W.events.unit_left();
    });
}

static void unit_comes(void)
{
    atomic_store(&g_on_bus, 1);
    on_service_queue(^{
      W.events.unit_arrived();
    });
}

/* --- The app: its engine and its end of the link, on one queue as in the real app. --- */

typedef struct {
    ua101_ctllink *link;
    panel_engine *engine; /* NULL for an app without one */
    dispatch_source_t timer;
    int up;
    _Atomic int attached_events, last_status, last_watching, last_streaming;
    _Atomic int messages, changed, audio_events, audio_now, refused, levels_events;
    _Atomic int passes_events;           /* what the helper says it passes on, with each attach */
    _Atomic uint64_t passes, passes_on;
    _Atomic uint64_t memory_passes;      /* the Load, Store and Reset commands the helper names */
    _Atomic uint64_t read_passes;        /* the memories it reads on request (link version 20) */
    _Atomic int lamp_events, lamp_in, lamp_out; /* "midi_lamps", and how many said in, out */
    ua101_ctl_levels levels; /* on the app's queue */
    char why[256];
    char refused_why[256];   /* the reason with the last refusal; read on the app's queue */
} app;

static dispatch_queue_t A; /* the app's queue */

static void app_attached(void *ctx, int status, int streaming, int watching, const char *why)
{
    app *a = ctx;

    atomic_store(&a->last_status, status);
    atomic_store(&a->last_watching, watching);
    atomic_store(&a->last_streaming, streaming);
    snprintf(a->why, sizeof(a->why), "%s", why);
    a->up = status == UA101_LINK_OK;
    if (a->engine != NULL)
        panel_transport(a->engine, a->up, now_us());
    atomic_fetch_add(&a->attached_events, 1);
}

static void app_message(void *ctx, const uint8_t *msg, size_t len)
{
    app *a = ctx;

    if (a->engine != NULL)
        panel_receive(a->engine, msg, len, now_us());
    atomic_fetch_add(&a->messages, 1);
}

static void app_changed(void *ctx)
{
    app *a = ctx;

    a->up = 0;
    if (a->engine != NULL)
        panel_transport(a->engine, 0, now_us());
    atomic_fetch_add(&a->changed, 1);
}

static void app_audio(void *ctx, int streaming)
{
    app *a = ctx;

    atomic_store(&a->audio_now, streaming);
    atomic_fetch_add(&a->audio_events, 1);
}

static void app_levels(void *ctx, const ua101_ctl_levels *levels)
{
    app *a = ctx;

    a->levels = *levels;
    atomic_fetch_add(&a->levels_events, 1);
}

static void app_refused(void *ctx, int status, const char *why)
{
    app *a = ctx;

    (void)status;
    snprintf(a->refused_why, sizeof(a->refused_why), "%s", why != NULL ? why : "");
    atomic_fetch_add(&a->refused, 1);
}

static void app_passes(void *ctx, uint64_t passes, uint64_t passes_on, uint64_t passes_memory,
                       uint64_t passes_read)
{
    app *a = ctx;

    atomic_store(&a->read_passes, passes_read);
    atomic_store(&a->passes, passes);
    atomic_store(&a->passes_on, passes_on);
    atomic_store(&a->memory_passes, passes_memory);
    atomic_fetch_add(&a->passes_events, 1);
}

static void app_midi(void *ctx, int in, int out)
{
    app *a = ctx;

    atomic_fetch_add(&a->lamp_in, in != 0);
    atomic_fetch_add(&a->lamp_out, out != 0);
    atomic_fetch_add(&a->lamp_events, 1);
}

static int app_send(void *ctx, const uint8_t *msg, size_t len)
{
    app *a = ctx;

    return a->up && ua101_ctllink_send(a->link, msg, len);
}

static void app_event(void *ctx, unsigned events)
{
    (void)ctx;
    (void)events;
}

static app *app_new(int with_engine)
{
    static const ua101_ctllink_events events = {app_attached, app_message, app_changed,
                                                app_audio,    app_levels,  app_refused,
                                                app_passes,   app_midi};
    app *a = calloc(1, sizeof(*a));

    dispatch_sync(A, ^{
      a->link = ua101_ctllink_open(A, &events, a);
      if (with_engine) {
          a->engine = panel_engine_new(app_send, app_event, a);
          /* The engine keeps no timer: this stands in for the app's booked wake-ups. */
          a->timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, A);
          dispatch_source_set_timer(a->timer, DISPATCH_TIME_NOW, 2 * NSEC_PER_MSEC, NSEC_PER_MSEC);
          dispatch_source_set_event_handler(a->timer, ^{
            if (panel_next_deadline(a->engine) <= now_us())
                panel_tick(a->engine, now_us());
          });
          dispatch_resume(a->timer);
      }
    });
    return a;
}

static void app_free(app *a)
{
    dispatch_sync(A, ^{
      if (a->timer != NULL) {
          dispatch_source_cancel(a->timer);
          dispatch_release(a->timer);
      }
      ua101_ctllink_close(a->link);
      if (a->engine != NULL)
          panel_engine_free(a->engine);
    });
    /* Whatever was already on its way to the queue has run by now and found the link closed. */
    dispatch_sync(A, ^{
    });
    free(a);
}

static void on_app(void (^work)(void))
{
    dispatch_sync(A, work);
}

static int app_int(int (^read)(void))
{
    __block int v;

    dispatch_sync(A, ^{
      v = read();
    });
    return v;
}

#define APP(expr) app_int(^{ return (int)(expr); })

/* The reason the last attach gave, copied on the app's queue. */
static const char *app_why(app *a)
{
    static char copy[256];

    dispatch_sync(A, ^{
      snprintf(copy, sizeof(copy), "%s", a->why);
    });
    return copy;
}

/* The reason the last refusal gave, copied on the app's queue. */
static const char *refused_why(app *a)
{
    static char copy[256];

    dispatch_sync(A, ^{
      snprintf(copy, sizeof(copy), "%s", a->refused_why);
    });
    return copy;
}

/* Attaches and waits for the answer. Returns its status. */
static int attach(app *a)
{
    int before = atomic_load(&a->attached_events);

    on_app(^{
      ua101_ctllink_attach(a->link);
    });
    CHECK(WAIT(atomic_load(&a->attached_events) > before, 8.0), "the attach was answered");
    return atomic_load(&a->last_status);
}

static ua101_service_counts counts(void)
{
    ua101_service_counts c;

    ua101_service_get_counts(&c);
    return c;
}

static uint32_t sim_value(int kind, unsigned group, unsigned index)
{
    __block uint32_t v;
    ua101_field f = {(uint8_t)kind, (uint8_t)group, (uint8_t)index};

    on_service_queue(^{
      v = ua101_block_get(P.sim.block, f);
    });
    return v;
}

/* A plug-in's requests, sent by hand: this test has no plug-in. */
static xpc_endpoint_t g_endpoint;

static int64_t plugin_ask(xpc_connection_t c, const char *op, uint64_t version)
{
    xpc_object_t m = xpc_dictionary_create(NULL, NULL, 0), reply;
    int64_t status = -1;

    xpc_dictionary_set_string(m, UA101_LINK_OP, op);
    xpc_dictionary_set_uint64(m, UA101_LINK_KEY_VERSION, version);
    reply = xpc_connection_send_message_with_reply_sync(c, m);
    if (xpc_get_type(reply) == XPC_TYPE_DICTIONARY)
        status = xpc_dictionary_get_int64(reply, UA101_LINK_STATUS);
    xpc_release(reply);
    xpc_release(m);
    return status;
}

static xpc_connection_t plugin_connect(void)
{
    xpc_connection_t c = xpc_connection_create_from_endpoint(g_endpoint);

    xpc_connection_set_event_handler(c, ^(xpc_object_t event) {
      (void)event;
    });
    xpc_connection_resume(c);
    return c;
}

/* --- The cases. --- */

static void test_who_may_ask(void)
{
    const uid_t coreaudio = 202, owner = 501, other = 502;

    CHECK(ua101_service_allows(coreaudio, 0, coreaudio, owner, -1), "Core Audio may ask for audio");
    CHECK(!ua101_service_allows(coreaudio, 1, coreaudio, owner, -1),
          "Core Audio's user may not use the control side");
    CHECK(ua101_service_allows(owner, 1, coreaudio, owner, -1),
          "the user the helper runs as may use the control side");
    CHECK(!ua101_service_allows(owner, 0, coreaudio, owner, -1),
          "but can't get at the audio: no hello, start or stop");
    CHECK(ua101_service_allows(other, 1, coreaudio, owner, -1) &&
              !ua101_service_allows(other, 0, coreaudio, owner, -1),
          "another person's account may use the control side and MIDI, but not the audio");
    CHECK(!ua101_service_allows(500, 1, coreaudio, owner, -1) &&
              !ua101_service_allows(201, 1, coreaudio, owner, -1) &&
              !ua101_service_allows(1, 1, coreaudio, owner, -1) &&
              !ua101_service_allows((uid_t)-2, 1, coreaudio, owner, -1),
          "a system account (below 501, or nobody) may not use the control side");
    CHECK(ua101_service_person(501) && ua101_service_person(502) && ua101_service_person(70000) &&
              !ua101_service_person(500) && !ua101_service_person(202) &&
              !ua101_service_person((uid_t)-2) && !ua101_service_person(0),
          "accounts a person logs in to start at 501");
    CHECK(!ua101_service_allows(other, 1, other, owner, -1),
          "Core Audio's user stays out of the control side even with a person's number");
    CHECK(ua101_service_allows(0, 0, coreaudio, owner, -1) &&
              ua101_service_allows(0, 1, coreaudio, owner, -1),
          "root may ask for both");
    CHECK(!ua101_service_allows(coreaudio, 1, coreaudio, coreaudio, -1),
          "a helper run as Core Audio's user still gives it no control side");
    CHECK(ua101_service_allows(other, 0, coreaudio, owner, (int)other) &&
              ua101_service_allows(other, 1, coreaudio, owner, (int)other),
          "a test's own user may ask for both");
    /* Which requests count as the user's side, so Core Audio's user can't make them. */
    CHECK(ua101_service_owner_request("midi_open") && ua101_service_owner_request("midi_send") &&
              ua101_service_owner_request("midi_close") && ua101_service_owner_request("ctl_open") &&
              ua101_service_owner_request("ctl_send") && ua101_service_owner_request("levels"),
          "a MIDI client's requests are the user's, like the control app's");
    CHECK(!ua101_service_owner_request("hello") && !ua101_service_owner_request("start") &&
              !ua101_service_owner_request("stop") && !ua101_service_owner_request(NULL),
          "the plug-in's are not");
}

/* The unit is off: the app is told, and told again when it appears. Nothing is opened. */
static void test_no_unit(app *a)
{
    CHECK(attach(a) == UA101_LINK_ERR_ABSENT, "no unit: the attach says so (%d)",
          atomic_load(&a->last_status));
    CHECK(atomic_load(&a->last_watching) == 1, "and that an event will come when one appears");
    CHECK(atomic_load(&P.opens) == 0 && !counts().port_open, "nothing was opened");
    CHECK(APP(panel_state(a->engine)) == PANEL_STATE_OFFLINE, "the engine is offline");

    unit_comes();
    CHECK(WAIT(atomic_load(&a->changed) >= 1, 3.0), "the unit appeared: the app is told");
    atomic_store(&P.can_open, 0);
    CHECK(attach(a) == UA101_LINK_ERR_ABSENT && atomic_load(&a->last_watching) == 0,
          "another program holds the MIDI interface: absent, and no event will come for that");
    atomic_store(&P.can_open, 1);
}

static void helper_clock_forward(double seconds);
static int log_lines_with(const char *a, const char *b);
static int send_raw(app *a, const uint8_t *msg, size_t len);

/* The memory preview (link version 20): the app's engine reads each of the five memories
   through the helper; each read reaches the cable, byte for byte one of the five, and its
   block comes back to the engine as that memory's, never as the settings in use. No other
   block is passed on, 02 0D to 02 0F above all (they hung the real unit's control port).
   A memory's block is no fresh read of the settings for a Load, Store or Reset. */
static void test_memory_reads(app *a)
{
    int wire = atomic_load(&P.wire_messages), reads = atomic_load(&P.wire_memory_reads);
    int refused = atomic_load(&a->refused);
    uint64_t changes = counts().ctl_changes, sent = counts().ctl_sent;
    static uint8_t settings_bytes[256], before_bytes[256];
    uint8_t *settings = settings_bytes, *before = before_bytes;
    unsigned mem, b, ok = 0;

    CHECK(atomic_load(&a->read_passes) == UA101_CTL_PASSES_READ,
          "with the attach the helper named the five memories it reads (%llx)",
          (unsigned long long)atomic_load(&a->read_passes));
    on_app(^{
      panel_unit_block(a->engine, before);
    });
    on_service_queue(^{
      unsigned m;

      for (m = 0; m < UA101_MEMORIES; m++)
          P.sim.memory[m][0x80] = (uint8_t)(m + 1); /* INPUT 1's volume, other in each */
    });
    for (mem = 0; mem < UA101_MEMORIES; mem++) {
        static uint8_t got_bytes[256], held_bytes[256];
        uint8_t *got = got_bytes, *held = held_bytes;
        __block int has = 0;
        unsigned m = mem;

        CHECK(APP(panel_read_memory(a->engine, m, now_us())) == PANEL_OK, "Memory %u is asked for", m + 1);
        CHECK(WAIT(APP(panel_memory_reading(a->engine, m)) == 0, 3.0), "and its block came");
        on_app(^{
          has = panel_memory_block(a->engine, m, got);
        });
        on_service_queue(^{
          memcpy(held, P.sim.memory[m], 256);
        });
        ok += has && memcmp(got, held, 256) == 0 && got[0x80] == m + 1;
    }
    CHECK(ok == UA101_MEMORIES, "each of the five came back as the unit holds it (%u)", ok);
    CHECK(atomic_load(&P.wire_memory_reads) == reads + UA101_MEMORIES &&
              atomic_load(&P.wire_messages) == wire + UA101_MEMORIES,
          "five messages reached the cable, the five memory reads");
    CHECK(counts().ctl_memory_reads == UA101_MEMORIES && counts().ctl_sent == sent + UA101_MEMORIES &&
              counts().ctl_changes == changes,
          "the helper counted them as requests, not as changes");
    on_app(^{
      panel_unit_block(a->engine, settings);
    });
    CHECK(memcmp(settings, before, 256) == 0 && APP(panel_volume(a->engine, 1, 0)) == PANEL_UNITY_VOLUME,
          "the settings in use are as before: no memory was taken for them");
    CHECK(log_lines_with("a read of Memory 3", "changes nothing") == 1,
          "the helper's log has each read, in words");

    /* Every other block, handed straight to the link: refused, nothing on the cable. */
    wire = atomic_load(&P.wire_messages);
    for (b = 1; b < UA101_CTL_BLOCKS; b++) {
        uint8_t req[UA101_CTL_READ_REQUEST_BYTES];

        if (b >= 3 && b <= 7)
            continue;
        ua101_ctl_block_request(b, req);
        CHECK(send_raw(a, req, sizeof(req)) == 1, "block 02 %02X is handed over", b);
    }
    CHECK(WAIT(atomic_load(&a->refused) == refused + 10, 3.0),
          "the helper refused all ten: 02 01, 02 02 and 02 08 to 02 0F (%d)",
          atomic_load(&a->refused) - refused);
    CHECK(strstr(refused_why(a), "02 03 to 02 07") != NULL, "saying why: %s", refused_why(a));
    usleep(100000);
    CHECK(atomic_load(&P.wire_messages) == wire, "and nothing of them reached the cable");

    /* A memory's block is no fresh read: with the settings read 40 s ago, a memory read just
       now leaves a Load refused by its own rule. */
    helper_clock_forward(40);
    CHECK(APP(panel_read_memory(a->engine, 0, now_us())) == PANEL_OK &&
              WAIT(APP(panel_memory_reading(a->engine, 0)) == 0 && APP(panel_memory_block(a->engine, 0, settings)), 3.0),
          "Memory 1 is read once more");
    {
        ua101_msg m;
        uint8_t load[UA101_CTLMSG_MAX_BYTES];
        size_t len;
        int memory_wire = atomic_load(&P.wire_memory);

        memset(&m, 0, sizeof(m));
        m.kind = UA101_MSG_LOAD;
        m.memory = 0;
        m.sections[3] = 1;
        len = ua101_ctlmsg_build(&m, load);
        refused = atomic_load(&a->refused);
        CHECK(send_raw(a, load, len) == 1 && WAIT(atomic_load(&a->refused) == refused + 1, 3.0),
              "a Load of Memory 1 right after it is refused");
        CHECK(strstr(refused_why(a), "weren't read") != NULL && atomic_load(&P.wire_memory) == memory_wire,
              "as the settings weren't read lately: %s", refused_why(a));
    }
}

/* The first read: the app's engine gets the settings through the helper. One message went down
   the cable, and it is the settings request. */
static void test_read(app *a)
{
    CHECK(attach(a) == UA101_LINK_OK, "attached (%d: %s)", atomic_load(&a->last_status), app_why(a));
    CHECK(atomic_load(&P.opens) == 1 && counts().port_open, "the helper holds the MIDI interface");
    CHECK(WAIT(APP(panel_state(a->engine)) == PANEL_STATE_READY, 3.0), "the settings arrived");
    CHECK(APP(panel_rate_hz(a->engine)) == 48000, "the sample rate reads 48 kHz");
    CHECK(APP(panel_patch(a->engine, 0)) == 7 && APP(panel_patch(a->engine, 4)) == 11,
          "the patch bay reads Initial Setting");
    CHECK(APP(panel_volume(a->engine, 1, 0)) == PANEL_UNITY_VOLUME, "INPUT 1 reads 0 dB");
    CHECK(!APP(panel_soft_control(a->engine)), "SOFT CTRL reads out");
    CHECK(!APP(panel_writes_allowed(a->engine)), "the engine is read-only");
    CHECK(atomic_load(&P.wire_messages) == 1 && atomic_load(&P.wire_reads) == 1,
          "one message reached the cable, the settings request (%d, %d)",
          atomic_load(&P.wire_messages), atomic_load(&P.wire_reads));
    CHECK(counts().ctl_sent == 1 && counts().ctl_refused == 0, "the helper counted one request");
    CHECK(counts().midi_packets > 0, "packets from the MIDI IN socket were counted");
    CHECK(atomic_load(&a->messages) == 1,
          "and not passed to the app: it got one message, the block (%d)",
          atomic_load(&a->messages));
    CHECK(atomic_load(&a->last_streaming) == 0, "no audio is streaming");
}

/* A front-panel switch: the unit's burst reaches the engine, which waits and reads again. */
static void test_switch(app *a)
{
    int reads = atomic_load(&P.wire_reads);

    on_service_queue(^{
      ua101_sim_switch_move(&P.sim, UA101_SIM_SWITCH_SOFT, 1, 3);
      port_deliver();
    });
    CHECK(WAIT(APP(panel_soft_control(a->engine)), 3.0), "SOFT CTRL pressed in on the unit shows");
    CHECK(WAIT(atomic_load(&P.wire_reads) == reads + 1 && !APP(panel_settling(a->engine)), 3.0),
          "after the burst the engine read the block once more");
    CHECK(atomic_load(&P.wire_messages) == atomic_load(&P.wire_reads), "still only reads on the cable");
}

static int send_raw(app *a, const uint8_t *msg, size_t len)
{
    __block int taken;

    on_app(^{
      taken = ua101_ctllink_send(a->link, msg, len);
    });
    return taken;
}

/* Every kind of message that isn't the settings request, handed straight to the link as a
   broken or hostile app would. With `saving`, the ones a helper passes on
   are among them (pause saving, resume saving, the five patch bay sources, the volume, pan
   and on/off of the passed channels, the link of the passed pairs and the solo of the passed
   groups); without, they are left out. Returns how many were handed over. */
#define UA101_VOLUME_NEAR 13
#define UA101_PAN_NEAR 12
#define UA101_ON_NEAR 13
#define UA101_LINK_NEAR 11
#define UA101_SOLO_NEAR 10
static int send_everything(app *a, int saving)
{
    int sent = 0;
    uint8_t bytes[UA101_CTLMSG_MAX_BYTES], big[300];
    ua101_msg m;
    size_t len;
    unsigned kind, id;

    /* All 82 settings, each with a value the unit would accept. */
    for (id = 0; id < UA101_FIELD_COUNT; id++) {
        memset(&m, 0, sizeof(m));
        m.kind = UA101_MSG_FIELD;
        m.field = ua101_field_from_id(id);
        m.value = m.field.kind == UA101_F_PATCH ? 2 : 1;
        len = ua101_ctlmsg_build(&m, bytes);
        CHECK(len != 0, "setting %u builds", id);
        if (!saving && (is_passed_patch(bytes, len) || is_passed_volume(bytes, len) ||
                        is_passed_pan(bytes, len) || is_passed_on(bytes, len) ||
                        is_passed_link(bytes, len) || is_passed_solo(bytes, len)))
            continue;
        sent += send_raw(a, bytes, len);
    }
    /* Pause and resume saving, and everything that touches the stored memories. */
    for (kind = UA101_MSG_AUTOSAVE; kind <= UA101_MSG_PRESET + 1; kind++) {
        int resume = kind == UA101_MSG_PRESET + 1; /* one more round: "resume saving" */

        if ((kind == UA101_MSG_AUTOSAVE || resume) && !saving)
            continue;
        memset(&m, 0, sizeof(m));
        m.kind = resume ? UA101_MSG_AUTOSAVE : (ua101_msg_kind)kind;
        m.memory = 1;
        m.value = kind == UA101_MSG_PRESET ? 4 : resume ? 1 : 0;
        m.sections[2] = m.sections[3] = 1;
        len = ua101_ctlmsg_build(&m, bytes);
        CHECK(len != 0, "message kind %u builds", kind);
        CHECK(ua101_ctlmsg_risk(bytes, len) != UA101_RISK_READ, "and isn't a read");
        sent += send_raw(a, bytes, len);
    }
    /* Requests the notes say to avoid, a read with one bit wrong, and rubbish. */
    ua101_ctl_read_request(bytes);
    bytes[7] = 0x01;
    bytes[14] = ua101_ctl_checksum(bytes + 6, 8);
    sent += send_raw(a, bytes, UA101_CTL_READ_REQUEST_BYTES);
    ua101_ctl_read_request(bytes);
    bytes[6] = 0x01;
    bytes[14] = ua101_ctl_checksum(bytes + 6, 8);
    sent += send_raw(a, bytes, UA101_CTL_READ_REQUEST_BYTES);
    ua101_ctl_read_request(bytes);
    bytes[5] = 0x12;
    sent += send_raw(a, bytes, UA101_CTL_READ_REQUEST_BYTES);
    ua101_ctl_read_request(bytes);
    sent += send_raw(a, bytes, UA101_CTL_READ_REQUEST_BYTES - 1);
    memset(big, 0x55, sizeof(big));
    big[0] = 0xF0;
    big[sizeof(big) - 1] = 0xF7;
    sent += send_raw(a, big, sizeof(big));
    sent += send_raw(a, big, 1);
    /* A note for the MIDI OUT socket: app data never goes out through here. */
    {
        static const uint8_t note[3] = {0x90, 0x3C, 0x40};

        sent += send_raw(a, note, sizeof(note));
    }
    /* The saving messages' neighbours: one byte away from "pause saving". */
    {
        static const uint8_t near[][13] = {
            /* reset all */
            {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x00, 0x00, 0x02, 0x00, 0x7E, 0xF7},
            /* set 00 00 00 01, listed under "commands to avoid" */
            {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x00, 0x00, 0x01, 0x00, 0x7F, 0xF7},
            /* the saving address with a value that is neither pause nor resume */
            {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x00, 0x00, 0x00, 0x02, 0x7E, 0xF7},
            /* "resume saving" with a wrong checksum */
            {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0xF7},
            /* "pause saving" for another device ID */
            {0xF0, 0x41, 0x11, 0x00, 0x69, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF7},
            /* The patch bay change's neighbours. OUTPUT 1-2 to source 0, and to source 12 */
            {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x02, 0x01, 0x00, 0x00, 0x7D, 0xF7},
            {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x02, 0x01, 0x00, 0x0C, 0x71, 0xF7},
            /* an output pair that doesn't exist */
            {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x02, 0x01, 0x05, 0x02, 0x76, 0xF7},
            /* rows 0 and 2 of the block's patch bay, which the UA-101's dialog doesn't use */
            {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x02, 0x00, 0x00, 0x02, 0x7C, 0xF7},
            {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x02, 0x02, 0x00, 0x02, 0x7A, 0xF7},
            /* OUTPUT 1-2 to INPUT 1-2 with a wrong checksum, and for another device ID */
            {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x02, 0x01, 0x00, 0x02, 0x7A, 0xF7},
            {0xF0, 0x41, 0x11, 0x00, 0x69, 0x12, 0x00, 0x02, 0x01, 0x00, 0x02, 0x7B, 0xF7},
            /* and as a request (11) in place of a set (12) */
            {0xF0, 0x41, 0x10, 0x00, 0x69, 0x11, 0x00, 0x02, 0x01, 0x00, 0x02, 0x7B, 0xF7}};
        unsigned i;

        for (i = 0; i < sizeof(near) / sizeof(near[0]); i++)
            sent += send_raw(a, near[i], sizeof(near[i]));
    }
    /* The volume change's neighbours: INPUT 1 one step past the top of the unit's range
       (0x7FFFFF, about +12 dB), and four more values past it (since link version 19 a volume
       above 0 dB up to the top passes); the same harmless value for three channels
       the mixer doesn't have (an INPUT 11, an OUTPUT 11, a third MONITOR OUT); INPUT 1's pan
       address; a wrong checksum, another device ID, a request in place of a set, and a digit
       that isn't one. */
    {
        uint8_t v[UA101_VOLUME_NEAR][18];
        unsigned i;

        hand_volume(v[0], 0x04, 0x00, 0x07, 0x800000u);
        hand_volume(v[1], 0x04, 0x00, 0x07, 0x800001u);
        hand_volume(v[2], 0x04, 0x00, 0x07, 0x900000u);
        hand_volume(v[3], 0x04, 0x00, 0x07, 0xFFFFFFu);
        hand_volume(v[4], 0x04, 0x00, 0x07, 0x8F0000u);
        hand_volume(v[5], 0x04, 0x0A, 0x07, 0x100001u);
        hand_volume(v[6], 0x05, 0x0A, 0x07, 0x100001u);
        hand_volume(v[7], 0x03, 0x02, 0x07, 0x100001u);
        hand_volume(v[8], 0x04, 0x00, 0x03, 0x100001u);
        hand_volume(v[9], 0x04, 0x00, 0x07, 0x100001u);
        v[9][16] ^= 0x01;
        hand_volume(v[10], 0x04, 0x00, 0x07, 0x100001u);
        v[10][2] = 0x11;
        hand_volume(v[11], 0x04, 0x00, 0x07, 0x100001u);
        v[11][5] = 0x11;
        hand_volume(v[12], 0x04, 0x00, 0x07, 0x100001u);
        v[12][12] = 0x10;
        v[12][16] = ua101_ctl_checksum(v[12] + 6, 10);
        for (i = 0; i < UA101_VOLUME_NEAR; i++)
            sent += send_raw(a, v[i], 18);
    }
    /* The pan change's neighbours: INPUT 1 one step past the top of the range and at the top
       of what four digits spell; three more above the range (0x9000, 0x8800, 0x8008: the
       first digit 9, or 8 with another digit not 0); the middle for an INPUT 11 and an OUTPUT
       11, which the mixer doesn't have, and at MONITOR OUT L's address, which has no pan; a
       wrong checksum, another device ID, a request in place of a set, and a digit that isn't
       one. */
    {
        uint8_t v[UA101_PAN_NEAR][16];
        unsigned i;

        hand_pan(v[0], 0x04, 0x00, 0x03, 0x8001u);
        hand_pan(v[1], 0x04, 0x00, 0x03, 0xFFFFu);
        hand_pan(v[2], 0x04, 0x00, 0x03, 0x9000u);
        hand_pan(v[3], 0x04, 0x00, 0x03, 0x8800u);
        hand_pan(v[4], 0x04, 0x00, 0x03, 0x8008u);
        hand_pan(v[5], 0x04, 0x0A, 0x03, 0x4000u);
        hand_pan(v[6], 0x05, 0x0A, 0x03, 0x4000u);
        hand_pan(v[7], 0x03, 0x00, 0x03, 0x4000u);
        hand_pan(v[8], 0x04, 0x00, 0x03, 0x4000u);
        v[8][14] ^= 0x01;
        hand_pan(v[9], 0x04, 0x00, 0x03, 0x4000u);
        v[9][2] = 0x11;
        hand_pan(v[10], 0x04, 0x00, 0x03, 0x4000u);
        v[10][5] = 0x11;
        hand_pan(v[11], 0x04, 0x00, 0x03, 0x4000u);
        v[11][12] = 0x10;
        v[11][14] = ua101_ctl_checksum(v[11] + 6, 8);
        for (i = 0; i < UA101_PAN_NEAR; i++)
            sent += send_raw(a, v[i], 16);
    }
    /* The on/off change's neighbours: INPUT 1 with the byte 2, 0x7F and 0x80; the byte 1 for
       four channels the mixer doesn't have (an INPUT 11 and 16, an OUTPUT 11, a third MONITOR
       OUT); INPUT 1's address with two controls nothing uses (04, 06) and 1; a wrong checksum,
       another device ID, a request in place of a set, and the byte 1 with a second byte after
       it. */
    {
        uint8_t v[UA101_ON_NEAR][14];
        unsigned i;

        hand_on(v[0], 0x04, 0x00, 0x02, 2);
        hand_on(v[1], 0x04, 0x00, 0x02, 0x7F);
        hand_on(v[2], 0x04, 0x00, 0x02, 0x80);
        hand_on(v[3], 0x04, 0x0A, 0x02, 1);
        hand_on(v[4], 0x04, 0x0F, 0x02, 1);
        hand_on(v[5], 0x05, 0x0A, 0x02, 1);
        hand_on(v[6], 0x03, 0x02, 0x02, 1);
        hand_on(v[7], 0x04, 0x00, 0x04, 1);
        hand_on(v[8], 0x04, 0x00, 0x06, 1);
        hand_on(v[9], 0x04, 0x00, 0x02, 1);
        v[9][11] ^= 0x01;
        hand_on(v[10], 0x04, 0x00, 0x02, 1);
        v[10][2] = 0x11;
        hand_on(v[11], 0x04, 0x00, 0x02, 1);
        v[11][5] = 0x11;
        hand_on(v[12], 0x04, 0x00, 0x02, 1);
        v[12][11] = 0x00;
        v[12][12] = ua101_ctl_checksum(v[12] + 6, 6);
        v[12][13] = 0xF7;
        for (i = 0; i < UA101_ON_NEAR; i++)
            sent += send_raw(a, v[i], i == 12 ? 14 : 13);
    }
    /* The link change's neighbours: INPUT 1-2 with the byte 2 and 0x7F; the byte 1 at an odd
       channel (INPUT 2's address, which starts no pair), for a sixth INPUT pair, a second
       MONITOR OUT pair, a sixth OUTPUT pair and a fourth group; a wrong checksum, another
       device ID, a request in place of a set, and the byte 1 with a second byte after it. */
    {
        uint8_t v[UA101_LINK_NEAR][14];
        unsigned i;

        hand_on(v[0], 0x04, 0x00, 0x01, 2);
        hand_on(v[1], 0x04, 0x00, 0x01, 0x7F);
        hand_on(v[2], 0x04, 0x01, 0x01, 1);
        hand_on(v[3], 0x04, 0x0A, 0x01, 1);
        hand_on(v[4], 0x03, 0x02, 0x01, 1);
        hand_on(v[5], 0x05, 0x0A, 0x01, 1);
        hand_on(v[6], 0x06, 0x00, 0x01, 1);
        hand_on(v[7], 0x04, 0x00, 0x01, 1);
        v[7][11] ^= 0x01;
        hand_on(v[8], 0x04, 0x00, 0x01, 1);
        v[8][2] = 0x11;
        hand_on(v[9], 0x04, 0x00, 0x01, 1);
        v[9][5] = 0x11;
        hand_on(v[10], 0x04, 0x00, 0x01, 1);
        v[10][11] = 0x00;
        v[10][12] = ua101_ctl_checksum(v[10] + 6, 6);
        v[10][13] = 0xF7;
        for (i = 0; i < UA101_LINK_NEAR; i++)
            sent += send_raw(a, v[i], i == 10 ? 14 : 13);
    }
    /* The solo change's neighbours: the INPUT group with 11 and 0x7F, the OUTPUT group with
       11 (channels the mixer doesn't have); the number 1 at MONITOR OUT's address, which has
       no solo, at a fourth group's, and with a channel byte that isn't 00; a wrong checksum,
       another device ID, a request in place of a set, and the number 1 with a second byte
       after it. */
    {
        uint8_t v[UA101_SOLO_NEAR][14];
        unsigned i;

        hand_on(v[0], 0x04, 0x00, 0x00, 11);
        hand_on(v[1], 0x04, 0x00, 0x00, 0x7F);
        hand_on(v[2], 0x05, 0x00, 0x00, 11);
        hand_on(v[3], 0x03, 0x00, 0x00, 1);
        hand_on(v[4], 0x06, 0x00, 0x00, 1);
        hand_on(v[5], 0x04, 0x01, 0x00, 1);
        hand_on(v[6], 0x04, 0x00, 0x00, 1);
        v[6][11] ^= 0x01;
        hand_on(v[7], 0x04, 0x00, 0x00, 1);
        v[7][2] = 0x11;
        hand_on(v[8], 0x04, 0x00, 0x00, 1);
        v[8][5] = 0x11;
        hand_on(v[9], 0x04, 0x00, 0x00, 1);
        v[9][11] = 0x00;
        v[9][12] = ua101_ctl_checksum(v[9] + 6, 6);
        v[9][13] = 0xF7;
        for (i = 0; i < UA101_SOLO_NEAR; i++)
            sent += send_raw(a, v[i], i == 9 ? 14 : 13);
    }
    return sent;
}

/* The settings the helper lets through: 5 patch sources, the passed channels' volumes, pans
   and on/offs, the passed pairs' links and the passed groups' solos. In version 14 that is
   every one of the 82. */
#define PASSED_SETTINGS (5 + T_VOLUMES + T_PANS + T_ONS + T_LINKS + T_SOLOS)
/* Without those and the two saving messages: the rest of the 82 settings (none in version
   14), 6 memory commands, 7 odd ones, 13 neighbours of saving and patch, 13 of the volume, 12
   of the pan, 13 of the on/off, 11 of the link, 10 of the solo. */
#define EVERYTHING \
    (82 - PASSED_SETTINGS + 6 + 7 + 13 + UA101_VOLUME_NEAR + UA101_PAN_NEAR + UA101_ON_NEAR + \
     UA101_LINK_NEAR + UA101_SOLO_NEAR)

static void helper_clock_forward(double seconds);

/* Everything but the changes on the helper's list (those have tests of their own below): the
   helper passes none of it on. Among it are a Load, a Store and the Resets, which since link
   version 18 no go-ahead stops; here the last read of the settings is made 40 seconds old
   first, so their own rules refuse them (test_memory has them at a good moment). */
static void test_helper_refuses(app *a)
{
    int wire = atomic_load(&P.wire_messages), refused = atomic_load(&a->refused), sent;
    uint64_t helper_refused = counts().ctl_refused;
    uint8_t big[4] = {0xF0, 0, 0, 0xF7};

    helper_clock_forward(40);
    sent = send_everything(a, 0);
    CHECK(sent == EVERYTHING,
          "everything but the saving messages, the patch changes, the passed channels' "
          "volumes, pans and on/offs, and the passed links and solos was handed to the helper (%d)",
          sent);
    CHECK(WAIT(atomic_load(&a->refused) == refused + sent, 5.0),
          "the helper refused every one (%d of %d)", atomic_load(&a->refused) - refused, sent);
    CHECK(counts().ctl_refused == helper_refused + (uint64_t)sent, "and counted them");
    CHECK(atomic_load(&P.wire_messages) == wire && atomic_load(&P.wire_other) == 0,
          "nothing reached the cable");
    {
        __block uint64_t sets, memory, unknown, saves;

        on_service_queue(^{
          sets = P.sim.sets_applied + P.sim.sets_ignored;
          memory = P.sim.memory_commands;
          unknown = P.sim.unknown_messages + P.sim.bad_messages;
          saves = P.sim.saves;
        });
        CHECK(sets == 0 && memory == 0 && unknown == 0 && saves == 0,
              "the pretend unit saw no change, no memory command and nothing odd");
    }
    CHECK(send_raw(a, big, 0) == 0, "an empty message isn't even handed over");
}

/* (Up to link version 17 a test here let the app's engine loose with its guards off, and the
   helper, without the go-ahead, refused its volume change. Since version 18 every setting the
   engine can name is passed on within its range, so the helper's refusal of what the engine
   can't make, and the engine's answer to it, is shown with a message made by hand: test_patch,
   INPUT 1 one step past the top of the range.) */

static const char *g_log_path;

/* How many lines of the helper's log hold all of `a` and `b` (b may be NULL). */
static int log_lines_with(const char *a, const char *b)
{
    char line[512];
    FILE *f = fopen(g_log_path, "r");
    int n = 0;

    if (f == NULL)
        return -1;
    while (fgets(line, sizeof(line), f) != NULL)
        n += strstr(line, a) != NULL && (b == NULL || strstr(line, b) != NULL);
    fclose(f);
    return n;
}

static int sim_paused(void)
{
    __block int paused;

    on_service_queue(^{
      paused = P.sim.saving_paused;
    });
    return paused;
}

/* Sends one saving message and says whether it reached the cable. */
static int saving_reaches(app *a, int resume)
{
    uint8_t bytes[13];
    int wire = atomic_load(&P.wire_saving), refused = atomic_load(&a->refused);

    panel_saving_message(resume, bytes);
    send_raw(a, bytes, sizeof(bytes));
    WAIT(atomic_load(&P.wire_saving) > wire || atomic_load(&a->refused) > refused, 3.0);
    return atomic_load(&P.wire_saving) > wire;
}

/* The rule by itself, with no service running: which message passes. (Up to link version 17
   it also took the user's go-ahead; there is none any more.) */
static void test_rules(void)
{
    static const uint8_t pause[13] = {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00,
                                      0x00, 0x00, 0x00, 0x00, 0x00, 0xF7};
    static const uint8_t resume[13] = {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00,
                                       0x00, 0x00, 0x00, 0x01, 0x7F, 0xF7};
    uint8_t bytes[UA101_CTLMSG_MAX_BYTES], built[13];
    const char *why = NULL;
    ua101_msg m;
    size_t len;
    unsigned id, kind, passed = 0, tried = 0;

    panel_saving_message(0, built);
    CHECK(memcmp(built, pause, 13) == 0, "the helper's \"pause saving\" is the app's, byte for byte");
    panel_saving_message(1, built);
    CHECK(memcmp(built, resume, 13) == 0, "and so is \"resume saving\"");
    ua101_ctl_read_request(bytes);
    CHECK(ua101_service_passes(bytes, UA101_CTL_READ_REQUEST_BYTES, &why) &&
              ua101_service_passes(bytes, UA101_CTL_READ_REQUEST_BYTES, NULL),
          "the settings request passes");
    /* Link version 20: the five memory reads, as the app's builder makes them, pass; no other
       block does, and no read one byte away from them. */
    {
        unsigned b, i, near = 0, near_passed = 0;
        uint8_t req[UA101_CTL_READ_REQUEST_BYTES];

        for (b = 0; b < UA101_CTL_BLOCKS; b++) {
            ua101_ctl_block_request(b, req);
            why = NULL;
            CHECK(ua101_service_passes(req, sizeof(req), &why) == (b == 0 || (b >= 3 && b <= 7)),
                  "the request for block 02 %02X %s", b,
                  b == 0 || (b >= 3 && b <= 7) ? "passes" : "never passes");
            if (b == 1 || b == 2 || b >= 8)
                CHECK(why != NULL && strstr(why, "02 03 to 02 07") != NULL, "and says why: %s",
                      why != NULL ? why : "(nothing)");
        }
        for (b = 0; b < UA101_MEMORIES; b++) {
            memset(&m, 0, sizeof(m));
            m.kind = UA101_MSG_READ_MEMORY;
            m.memory = (uint8_t)b;
            len = ua101_ctlmsg_build(&m, bytes);
            CHECK(len == UA101_CTL_READ_REQUEST_BYTES && ua101_service_passes(bytes, len, &why),
                  "Memory %u's read, as the app builds it, passes", b + 1);
            CHECK(!ua101_service_passes(bytes, len - 1, &why), "cut short it doesn't");
            for (i = 0; i < len; i++) {
                unsigned v;

                for (v = 0; v < 256; v += 1) {
                    if (v == bytes[i])
                        continue;
                    memcpy(req, bytes, len);
                    req[i] = (uint8_t)v;
                    near++;
                    /* A changed byte that makes another memory's read or the settings request,
                       with its checksum, can't happen with one byte; so none passes. */
                    near_passed += (unsigned)ua101_service_passes(req, len, &why);
                }
            }
        }
        CHECK(near == 5 * 16 * 255 && near_passed == 0,
              "of %u reads one byte away from the five, %u pass", near, near_passed);
        CHECK(UA101_CTL_PASSES_READ == 0x1Full && panel_helper_passes_read() == 0x1Full &&
                  UA101_CTL_PASS_READ_MEMORY(4) == 0x10ull,
              "the helper names the five memories it reads, a bit each");
        CHECK(UA101_LINK_VERSION == 20 && UA101_LINK_VERSION_OLDER_HELPER == 19,
              "link version 20, and the app still speaks 19 to an older helper");
    }
    CHECK(ua101_service_passes(pause, 13, &why) && ua101_service_passes(resume, 13, &why),
          "\"pause saving\" and \"resume saving\" both pass");
    CHECK(!ua101_service_passes(pause, 12, &why) && !ua101_service_passes(NULL, 13, &why),
          "a cut-off or missing message never passes");
    /* Every message the app's builder can make: of these, only the five
       patch bay sources, the passed channels' volumes, pans and on/offs, the passed pairs'
       links and the passed groups' solos pass. */
    {
        for (id = 0; id < UA101_FIELD_COUNT; id++) {
            uint32_t values[3] = {0, 1, 0x100001u};
            unsigned v;

            for (v = 0; v < 3; v++) {
                memset(&m, 0, sizeof(m));
                m.kind = UA101_MSG_FIELD;
                m.field = ua101_field_from_id(id);
                m.value = m.field.kind == UA101_F_PATCH ? 2 + v : values[v];
                if ((len = ua101_ctlmsg_build(&m, bytes)) == 0)
                    continue;
                tried++;
                passed += (unsigned)ua101_service_passes(bytes, len, &why);
            }
        }
        for (kind = UA101_MSG_RESET_ALL; kind <= UA101_MSG_PRESET; kind++) {
            unsigned memory, sections;

            for (memory = 0; memory < UA101_MEMORIES; memory++)
                for (sections = 1; sections < 16; sections++) {
                    memset(&m, 0, sizeof(m));
                    m.kind = (ua101_msg_kind)kind;
                    m.memory = (uint8_t)memory;
                    m.value = 3 + (sections & 1);
                    m.sections[0] = sections & 1;
                    m.sections[1] = (sections >> 1) & 1;
                    m.sections[2] = (sections >> 2) & 1;
                    m.sections[3] = (sections >> 3) & 1;
                    if ((len = ua101_ctlmsg_build(&m, bytes)) == 0)
                        continue;
                    tried++;
                    passed += (unsigned)ua101_service_passes(bytes, len, &why);
                }
        }
    }
    CHECK(tried > 82 + 5 &&
              passed == 15 + 3 * T_VOLUMES + 2 * T_PANS + 2 * T_ONS + 2 * T_LINKS + 2 * T_SOLOS,
          "of %u settings, Loads, Stores, Resets and presets, only the fifteen patch changes "
          "pass and, of each passed channel, three "
          "volumes, two pans and two on/offs, of each passed pair two links and of each passed "
          "group two solos (%u)",
          tried, passed);
    /* The patch bay change, version 7: the notes' own bytes, then every output and source. */
    {
        static const uint8_t wave[13] = {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00,
                                         0x02, 0x01, 0x00, 0x07, 0x76, 0xF7};
        static const uint8_t input[13] = {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00,
                                          0x02, 0x01, 0x00, 0x02, 0x7B, 0xF7};
        unsigned output, source, a1, a2, a3, value, ok = 0, count = 0, right = 0;

        CHECK(patch_message(0, 7, bytes) == 13 && memcmp(bytes, wave, 13) == 0,
              "OUTPUT 1-2 to WAVE OUT 1-2 is the notes' example, byte for byte");
        CHECK(patch_message(0, 2, bytes) == 13 && memcmp(bytes, input, 13) == 0,
              "and OUTPUT 1-2 to INPUT 1-2 is F0 41 10 00 69 12 00 02 01 00 02 7B F7");
        CHECK(ua101_service_passes(wave, 13, &why) && ua101_service_passes(input, 13, &why),
              "both pass");
        CHECK(!ua101_service_passes(input, 12, &why), "but not cut off");
        for (output = 0; output < 6; output++)
            for (source = 0; source < 14; source++) {
                int want = output < 5 && source >= 1 && source <= 11;

                len = patch_message(output, source, bytes);
                if (len == 0) { /* the builder won't make it: by hand */
                    memcpy(bytes, input, 13);
                    bytes[9] = (uint8_t)output;
                    bytes[10] = (uint8_t)source;
                    bytes[11] = ua101_ctl_checksum(bytes + 6, 5);
                    len = 13;
                }
                right += ua101_service_passes(bytes, len, &why) == want;
            }
        CHECK(right == 6 * 14,
              "of the patch bay's outputs and sources only OUTPUT 1-2 to 9-10 with a source 1 to "
              "11 pass (%u of %u right)", right, 6 * 14);
        CHECK(UA101_PATCH_OUTPUTS_PASSED == 0x1Fu, "this version passes on all five output pairs");
        /* By brute force: every 13-byte set at 00 02 <a2> <a3>, and at 00 <a1> 01 <a3>, with
           every payload byte and a right checksum. */
        {
            for (a2 = 0; a2 < 128; a2++)
                for (a3 = 0; a3 < 128; a3++)
                    for (value = 0; value < 128; value++) {
                        memcpy(bytes, input, 13);
                        bytes[8] = (uint8_t)a2;
                        bytes[9] = (uint8_t)a3;
                        bytes[10] = (uint8_t)value;
                        bytes[11] = ua101_ctl_checksum(bytes + 6, 5);
                        count++;
                        ok += (unsigned)ua101_service_passes(bytes, 13, &why);
                    }
            for (a1 = 0; a1 < 128; a1++)
                for (a3 = 0; a3 < 128; a3++)
                    for (value = 0; value < 128; value++) {
                        if (a1 == 2)
                            continue; /* counted above */
                        memcpy(bytes, input, 13);
                        bytes[7] = (uint8_t)a1;
                        bytes[9] = (uint8_t)a3;
                        bytes[10] = (uint8_t)value;
                        bytes[11] = ua101_ctl_checksum(bytes + 6, 5);
                        count++;
                        ok += (unsigned)ua101_service_passes(bytes, 13, &why);
                    }
        }
        /* The second loop also meets 00 03 01 02, 00 04 01 02 and 00 05 01 02: the on/off of
           MONITOR OUT R, INPUT 2 and OUTPUT 2, two values each where that channel is passed. */
        CHECK(ok == 55u + 2u * (unsigned)(t_on(0, 1) + t_on(1, 1) + t_on(2, 1)),
              "of %u messages around the patch bay's address, exactly the fifty-five patch "
              "changes and the on/offs that lie there pass (%u)", count, ok);
        /* The right bytes with one of them changed, any one, to any value: only a change of
           the source to another source 1 to 11, with its checksum, could pass, and that takes
           two bytes. */
        {
            unsigned at, v, slipped = 0;

            for (at = 0; at < 13; at++)
                for (v = 0; v < 256; v++) {
                    memcpy(bytes, input, 13);
                    if (bytes[at] == (uint8_t)v)
                        continue;
                    bytes[at] = (uint8_t)v;
                    slipped += (unsigned)ua101_service_passes(bytes, 13, &why);
                }
            CHECK(slipped == 0, "with any one byte of it changed, it no longer passes (%u)", slipped);
        }
    }
    /* The volume change, version 8: the notes' own bytes, every value, every channel, and the
       addresses around it. */
    {
        static const uint8_t first[18] = {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x04, 0x00,
                                          0x07, 0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x73, 0xF7};
        static const uint8_t zero_db[18] = {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x04, 0x00,
                                            0x07, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x73, 0xF7};
        static const uint8_t minus_3[18] = {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x04, 0x00,
                                            0x07, 0x01, 0x06, 0x0A, 0x00, 0x09, 0x0E, 0x4D, 0xF7};
        static const uint8_t mon_l[18] = {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x03, 0x00,
                                          0x07, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x74, 0xF7};
        static const uint8_t out10[18] = {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x05, 0x09,
                                          0x07, 0x01, 0x06, 0x0A, 0x00, 0x09, 0x0E, 0x43, 0xF7};
        uint8_t v[20], shim[18];
        uint32_t value, wrong = 0, above = 0, safe = 0, app_differs = 0;
        unsigned group, channel, a1, a2, a3, at, b, ok = 0, count = 0, slipped = 0;

        CHECK(volume_message(0, 0, 0x200000u, bytes) == 18 && memcmp(bytes, mon_l, 18) == 0 &&
                  volume_message(2, 9, 0x16A09Eu, bytes) == 18 && memcmp(bytes, out10, 18) == 0,
              "MONITOR OUT L's volume to 0 dB is F0 41 10 00 69 12 00 03 00 07 02 00 00 00 00 00 74 "
              "F7, and OUTPUT 10's to 0x16A09E is ... 05 09 07 01 06 0A 00 09 0E 43 F7, by the builder");
        CHECK(ua101_service_passes(mon_l, 18, &why) == t_volume(0, 0) &&
                  ua101_service_passes(out10, 18, &why) == t_volume(2, 9),
              "written out by hand they pass as this version's list says");
        CHECK(volume_message(1, 0, 0x100001u, bytes) == 18 && memcmp(bytes, first, 18) == 0,
              "INPUT 1's volume to 0x100001 is F0 41 10 00 69 12 00 04 00 07 01 00 00 00 00 01 73 F7");
        CHECK(volume_message(1, 0, 0x200000u, bytes) == 18 && memcmp(bytes, zero_db, 18) == 0,
              "and to 0 dB it is the notes' example, byte for byte");
        hand_volume(v, 0x04, 0x00, 0x07, 0x100001u);
        panel_volume_message(1, 0, 0x100001u, shim);
        CHECK(memcmp(v, first, 18) == 0 && memcmp(shim, first, 18) == 0,
              "written by hand, and by the app, it is the same 18 bytes");
        CHECK(ua101_service_passes(first, 18, &why) && ua101_service_passes(zero_db, 18, &why),
              "both pass");
        CHECK(!ua101_service_passes(first, 17, &why), "nor cut off");
        hand_volume(v, 0x04, 0x00, 0x07, 0x200001u);
        hand_volume(shim, 0x04, 0x00, 0x07, T_VOLUME_TOP);
        CHECK(ua101_service_passes(v, 18, &why) && ua101_service_passes(shim, 18, &why),
              "since link version 19, one step above 0 dB and the top of the range (0x7FFFFF, "
              "about +12 dB) pass");
        hand_volume(v, 0x04, 0x00, 0x07, T_VOLUME_TOP + 1u);
        CHECK(!ua101_service_passes(v, 18, &why) && strstr(why, "not sent") != NULL,
              "one step past the top: refused, and the reason says nothing was sent (%s)", why);
        CHECK(strstr(why, "order") == NULL, "and the reason no longer speaks of other orders of the digits");
        hand_volume(v, 0x04, 0x00, 0x07, 0x16A09Eu);
        panel_volume_message(1, 0, 0x16A09Eu, shim);
        CHECK(volume_message(1, 0, 0x16A09Eu, bytes) == 18 && memcmp(bytes, minus_3, 18) == 0 &&
                  memcmp(v, minus_3, 18) == 0 && memcmp(shim, minus_3, 18) == 0,
              "INPUT 1's volume to 0x16A09E (the menu's new value) is F0 41 10 00 69 12 00 04 00 07 "
              "01 06 0A 00 09 0E 4D F7, by the builder, by hand and by the app");
        CHECK(ua101_service_passes(minus_3, 18, &why),
              "it passes, which version 7 refused");
        hand_volume(v, 0x04, 0x00, 0x07, 0x1FFFFFu);
        hand_volume(shim, 0x04, 0x00, 0x07, 0x00000Fu);
        CHECK(ua101_service_passes(v, 18, &why) && ua101_service_passes(shim, 18, &why),
              "one step below 0 dB (0x1FFFFF) and 0x00000F pass: version 7 refused both");

        /* Every value six digits can spell, 16,777,216 of them, for INPUT 1. */
        for (value = 0; value <= 0xFFFFFFu; value++) {
            int want = volume_at_most_top(value), got;

            hand_volume(v, 0x04, 0x00, 0x07, value);
            got = ua101_service_passes(v, 18, &why);
            wrong += got != want;
            above += got && value > T_VOLUME_TOP;
            safe += (uint32_t)got;
            app_differs += (panel_volume_message_channel(v, 18) == 2) != got ||
                           (panel_volume_sendable(value) != 0) != got;
        }
        CHECK(above == 0, "of all 16,777,216 values for INPUT 1, none past the top (0x7FFFFF) passes (%u)", above);
        CHECK(wrong == 0 && safe == T_VOLUME_TOP + 1u,
              "exactly the %u from 0x000000 to 0x7FFFFF pass, every one of them (%u wrong)", safe,
              wrong);
        CHECK(app_differs == 0, "the app's own rule picks the same values (%u differ)", app_differs);
        for (value = 0; value <= 0x7Fu; value++) /* a digit that is more than a digit */
            for (at = 10; at < 16; at++) {
                hand_volume(v, 0x04, 0x00, 0x07, 0);
                v[at] = (uint8_t)(0x10u + value);
                v[16] = ua101_ctl_checksum(v + 6, 10);
                slipped += (unsigned)ua101_service_passes(v, 18, &why);
            }
        CHECK(slipped == 0, "a byte above 0F among the six digits never passes (%u)", slipped);

        /* Every channel there is and isn't, with four harmless values: the passed ones only. */
        for (group = 0; group < 3; group++)
            for (channel = 0; channel < 16; channel++) {
                static const uint32_t harmless[4] = {0, 1, 0x100001u, 0x200000u};
                int want = t_volume(group, channel);

                for (b = 0; b < 4; b++) {
                    hand_volume(v, 3 + group, channel, 0x07, harmless[b]);
                    count++;
                    ok += ua101_service_passes(v, 18, &why) == want &&
                          (panel_volume_message_channel(v, 18) >= 0) ==
                              (group == 0 ? channel < 2 : channel < 10);
                }
            }
        CHECK(ok == count, "of every channel's volume only the passed channels' pass (%u of %u right)", ok, count);
        CHECK(UA101_VOLUMES_PASSED == T_VOLUME_BITS && UA101_VOLUME_PASSED_MAX == T_VOLUME_TOP,
              "this version passes on the volume of the channels the test names, up to the top "
              "of the range (about +12 dB)");
        /* Every channel there is and isn't, with values all along the six digits (every 257th,
           and the ones around 0 dB and the top): at most the top for a passed channel, nothing
           for another. */
        {
            static const uint32_t edge[8] = {0x1FFFFFu, 0x200000u, 0x200001u, 0x7FFFFEu,
                                             0x7FFFFFu, 0x800000u, 0x800001u, 0xFFFFFFu};
            uint32_t bad = 0, over = 0, tried = 0;

            for (group = 0; group < 3; group++)
                for (channel = 0; channel < 16; channel++) {
                    for (value = 0; value <= 0xFFFFFFu; value += 0x101u) {
                        int got;

                        hand_volume(v, 3 + group, channel, 0x07, value);
                        got = ua101_service_passes(v, 18, &why);
                        bad += got != (t_volume(group, channel) && volume_at_most_top(value));
                        over += got && value > T_VOLUME_TOP;
                        tried++;
                    }
                    for (b = 0; b < 8; b++) {
                        int got;

                        hand_volume(v, 3 + group, channel, 0x07, edge[b]);
                        got = ua101_service_passes(v, 18, &why);
                        bad += got != (t_volume(group, channel) && edge[b] <= T_VOLUME_TOP);
                        over += got && edge[b] > T_VOLUME_TOP;
                        tried++;
                    }
                }
            CHECK(over == 0, "of %u volumes over all 48 channel addresses, none past the top passes (%u)",
                  tried, over);
            CHECK(bad == 0, "and exactly those at most the top of a passed channel pass (%u wrong)", bad);
        }
        /* Each of the helper's checks alone (this test's build can leave one out): the two
           value checks and the two channel checks. The parser already stops a volume above
           0x7FFFFF, so the value checks are also called directly, with a parse made by hand
           that carries the number the digits spell, whatever it is. */
        {
            static const unsigned skip[4] = {512u, 256u, 128u, 64u};
            static const char *const alone[4] = {
                "the first value check (the parser's number)", "the second value check (the digits one by one)",
                "the first channel check (the parser's field)", "the second channel check (bytes 7 and 8)"};
            unsigned k;

            for (k = 0; k < 4; k++) {
                uint32_t bad = 0, passed = 0, direct = 0;

                ua101_service_test_skip = skip[k];
                for (value = 0; value <= 0xFFFFFFu; value += 7u) {
                    ua101_msg m = {.kind = UA101_MSG_FIELD, .value = value};
                    int got;

                    m.field.kind = UA101_F_VOLUME;
                    m.field.group = 1;
                    m.field.index = 0;
                    hand_volume(v, 0x04, 0x00, 0x07, value);
                    got = ua101_service_passes(v, 18, &why);
                    bad += got != volume_at_most_top(value);
                    passed += (uint32_t)got;
                    got = ua101_service_test_volume(v, 18, &m);
                    bad += got != (value <= T_VOLUME_TOP ? 1 : -1);
                    direct += got == 1;
                }
                for (value = 0x7FFFF0u; value <= 0x800010u; value++) { /* and around the top, every one */
                    ua101_msg m = {.kind = UA101_MSG_FIELD, .value = value};

                    m.field.kind = UA101_F_VOLUME;
                    m.field.group = 1;
                    hand_volume(v, 0x04, 0x00, 0x07, value);
                    bad += ua101_service_passes(v, 18, &why) != (value <= T_VOLUME_TOP);
                    bad += ua101_service_test_volume(v, 18, &m) != (value <= T_VOLUME_TOP ? 1 : -1);
                }
                for (group = 0; group < 3; group++)
                    for (channel = 0; channel < 16; channel++) {
                        ua101_msg m = {.kind = UA101_MSG_FIELD, .value = 0x100001u};

                        hand_volume(v, 3 + group, channel, 0x07, 0x100001u);
                        bad += ua101_service_passes(v, 18, &why) != t_volume(group, channel);
                        /* Past the parser: a parse and bytes that agree on this channel,
                           whether the mixer has it or not. */
                        m.field.kind = UA101_F_VOLUME;
                        m.field.group = (uint8_t)group;
                        m.field.index = (uint8_t)channel;
                        bad += ua101_service_test_volume(v, 18, &m) != t_volume(group, channel);
                    }
                ua101_service_test_skip = 0;
                CHECK(bad == 0 && passed == T_VOLUME_TOP / 7u + 1u && direct == passed,
                      "%s alone: exactly the values up to the top pass for INPUT 1, also past the "
                      "parser, and no channel's volume but a passed one's (%u wrong)", alone[k], bad);
            }
        }
        /* Past the parser, with nothing left out: a parse and bytes that name a channel the
           mixer doesn't have, or that don't name the same channel, never pass. */
        {
            ua101_msg m = {.kind = UA101_MSG_FIELD, .value = 0x100001u};
            unsigned bad = 0;

            m.field.kind = UA101_F_VOLUME;
            m.field.group = 1;
            m.field.index = 10; /* an INPUT 11 */
            hand_volume(v, 0x04, 0x0A, 0x07, 0x100001u);
            bad += ua101_service_test_volume(v, 18, &m) != 0;
            m.field.group = 0;
            m.field.index = 2; /* a third MONITOR OUT */
            hand_volume(v, 0x03, 0x02, 0x07, 0x100001u);
            bad += ua101_service_test_volume(v, 18, &m) != 0;
            m.field.group = 1;
            m.field.index = 0; /* the parse says INPUT 1, the bytes INPUT 2, then OUTPUT 1 */
            hand_volume(v, 0x04, 0x01, 0x07, 0x100001u);
            bad += ua101_service_test_volume(v, 18, &m) != 0;
            hand_volume(v, 0x05, 0x00, 0x07, 0x100001u);
            bad += ua101_service_test_volume(v, 18, &m) != 0;
            hand_volume(v, 0x04, 0x00, 0x07, 0x100001u); /* and when they agree on INPUT 1 */
            bad += ua101_service_test_volume(v, 18, &m) != 1;
            CHECK(bad == 0, "a channel that isn't there, or a parse and bytes that disagree: never (%u wrong)", bad);
        }
        /* By brute force: every 18-byte set at 00 <a1> <a2> <a3> with the first test's value. */
        ok = count = 0;
        for (a1 = 0; a1 < 128; a1++)
            for (a2 = 0; a2 < 128; a2++)
                for (a3 = 0; a3 < 128; a3++) {
                    hand_volume(v, a1, a2, a3, 0x100001u);
                    count++;
                    ok += (unsigned)ua101_service_passes(v, 18, &why);
                }
        for (a1 = 1; a1 < 128; a1++) { /* and with another first address byte */
            hand_volume(v, 0x04, 0x00, 0x07, 0x100001u);
            v[6] = (uint8_t)a1;
            v[16] = ua101_ctl_checksum(v + 6, 10);
            count++;
            ok += (unsigned)ua101_service_passes(v, 18, &why);
        }
        CHECK(ok == T_VOLUMES, "of %u messages around the volume's address, exactly one for each "
                               "passed channel passes (%u)", count, ok);
        /* The right bytes with any one of them changed to any value. */
        slipped = 0;
        for (at = 0; at < 18; at++)
            for (b = 0; b < 256; b++) {
                memcpy(v, first, 18);
                if (v[at] == (uint8_t)b)
                    continue;
                v[at] = (uint8_t)b;
                slipped += (unsigned)ua101_service_passes(v, 18, &why);
            }
        CHECK(slipped == 0, "with any one byte of it changed, it no longer passes (%u)", slipped);
        /* A seventh digit, or five. */
        memcpy(v, first, 16);
        v[16] = 0x00;
        v[17] = ua101_ctl_checksum(v + 6, 11);
        v[18] = 0xF7;
        CHECK(!ua101_service_passes(v, 19, &why), "seven digits: refused");
        memcpy(v, first, 15);
        v[15] = ua101_ctl_checksum(v + 6, 9);
        v[16] = 0xF7;
        CHECK(!ua101_service_passes(v, 17, &why), "five digits: refused");
    }
    /* The pan change, version 10: the bytes, every value four digits can spell, every channel,
       the addresses around it, and each of the helper's two value checks and two channel
       checks alone. */
    {
        static const uint8_t middle[16] = {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x04,
                                           0x00, 0x03, 0x04, 0x00, 0x00, 0x00, 0x75, 0xF7};
        static const uint8_t left[16] = {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x04,
                                         0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x79, 0xF7};
        static const uint8_t digits[16] = {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x04,
                                           0x00, 0x03, 0x01, 0x02, 0x03, 0x04, 0x6F, 0xF7};
        static const uint8_t out1[16] = {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x05,
                                         0x00, 0x03, 0x04, 0x00, 0x00, 0x00, 0x74, 0xF7};
        uint8_t v[18], shim[16];
        uint32_t value, wrong = 0, outside = 0, safe = 0, app_differs = 0;
        unsigned group, channel, a1, a2, a3, at, b, ok = 0, count = 0, slipped = 0;

        CHECK(pan_message(1, 0, 0x4000u, bytes) == 16 && memcmp(bytes, middle, 16) == 0,
              "INPUT 1's pan to the middle is F0 41 10 00 69 12 00 04 00 03 04 00 00 00 75 F7");
        CHECK(pan_message(1, 0, 0, bytes) == 16 && memcmp(bytes, left, 16) == 0,
              "and to 0x0000 it is F0 41 10 00 69 12 00 04 00 03 00 00 00 00 79 F7");
        CHECK(pan_message(2, 0, 0x4000u, bytes) == 16 && memcmp(bytes, out1, 16) == 0,
              "OUTPUT 1's pan to the middle is the notes' example, byte for byte");
        hand_pan(v, 0x04, 0x00, 0x03, 0x4000u);
        panel_pan_message(1, 0, 0x4000u, shim);
        CHECK(memcmp(v, middle, 16) == 0 && memcmp(shim, middle, 16) == 0,
              "written by hand, and by the app, the middle is the same 16 bytes");
        hand_pan(v, 0x04, 0x00, 0x03, 0x1234u);
        panel_pan_message(1, 0, 0x1234u, shim);
        CHECK(pan_message(1, 0, 0x1234u, bytes) == 16 && memcmp(bytes, digits, 16) == 0 &&
                  memcmp(v, digits, 16) == 0 && memcmp(shim, digits, 16) == 0,
              "INPUT 1's pan to 0x1234 is F0 41 10 00 69 12 00 04 00 03 01 02 03 04 6F F7, by the "
              "builder, by hand and by the app");
        CHECK(ua101_service_passes(middle, 16, &why) && ua101_service_passes(left, 16, &why) &&
                  ua101_service_passes(digits, 16, &why),
              "all three pass");
        CHECK(!ua101_service_passes(middle, 15, &why), "nor cut off");
        CHECK(ua101_service_passes(out1, 16, &why) == t_pan(2, 0),
              "OUTPUT 1's pan to the middle passes as this version's list says");
        hand_pan(v, 0x03, 0x00, 0x03, 0x4000u);
        CHECK(!ua101_service_passes(v, 16, &why) && strstr(why, "not sent") != NULL,
              "a pan at MONITOR OUT L's address, which has none: refused (%s)", why);
        hand_on(v, 0x04, 0x00, 0x01, 1);
        CHECK(ua101_service_passes(v, 13, &why) == t_link(1, 0),
              "INPUT 1-2's link, refused up to version 13, passes as this version's list says");
        hand_pan(v, 0x04, 0x00, 0x03, 0x8001u);
        CHECK(!ua101_service_passes(v, 16, &why) && strstr(why, "not sent") != NULL,
              "0x8001, one past the end: refused (%s)", why);
        hand_pan(v, 0x04, 0x00, 0x03, 0x8000u);
        hand_pan(shim, 0x04, 0x00, 0x03, 0x7FFFu);
        CHECK(ua101_service_passes(v, 16, &why) && ua101_service_passes(shim, 16, &why),
              "the far end of the range (0x8000) and 0x7FFF pass");
        hand_pan(v, 0x04, 0x00, 0x03, 0x0009u);
        hand_pan(shim, 0x04, 0x00, 0x03, 0x4800u);
        CHECK(ua101_service_passes(v, 16, &why) && ua101_service_passes(shim, 16, &why),
              "0x0009 and 0x4800, which version 9 refused (out of range in some other order of "
              "the digits), pass now");

        /* Every value four digits can spell, 65,536 of them, for INPUT 1. */
        for (value = 0; value <= 0xFFFFu; value++) {
            int want = pan_in_range(value), got;

            hand_pan(v, 0x04, 0x00, 0x03, value);
            got = ua101_service_passes(v, 16, &why);
            wrong += got != want;
            outside += got && value > 0x8000u;
            safe += (uint32_t)got;
            app_differs += (panel_pan_message_channel(v, 16) == 2) != got ||
                           (panel_pan_sendable(value) != 0) != got;
        }
        CHECK(outside == 0, "of all 65,536 values for INPUT 1's pan, none above 0x8000 passes (%u)", outside);
        CHECK(wrong == 0 && safe == 0x8001u,
              "exactly %u pass: every one from 0x0000 to 0x8000 (%u wrong)", safe, wrong);
        CHECK(app_differs == 0, "the app's own rule picks the same values (%u differ)", app_differs);

        /* Each of the helper's checks alone (this test's build can leave one out): each must
           keep to the rule by itself, so taking either out of src/ua101_service.c fails here.
           The parser already stops a pan above 0x8000 (ua101_field_value_ok), so the value
           checks are also called directly, with a parse made by hand that carries the number
           the digits spell, whatever it is. */
        {
            static const unsigned skip[4] = {2u, 1u, 8u, 4u};
            static const char *const alone[4] = {
                "the first value check (the parser's number)", "the second value check (the digits one by one)",
                "the first channel check (the parser's field)", "the second channel check (bytes 7 and 8)"};
            unsigned k;

            for (k = 0; k < 4; k++) {
                uint32_t bad = 0, passed = 0, direct = 0;

                ua101_service_test_skip = skip[k];
                for (value = 0; value <= 0xFFFFu; value++) {
                    ua101_msg m = {.kind = UA101_MSG_FIELD, .value = value};
                    int got;

                    m.field.kind = UA101_F_PAN;
                    m.field.group = 1;
                    m.field.index = 0;
                    hand_pan(v, 0x04, 0x00, 0x03, value);
                    got = ua101_service_passes(v, 16, &why);
                    bad += got != pan_in_range(value);
                    passed += (uint32_t)got;
                    got = ua101_service_test_pan(v, 16, &m);
                    bad += got != (value <= 0x8000u ? 1 : -1);
                    direct += got == 1;
                }
                for (group = 0; group < 3; group++)
                    for (channel = 0; channel < 16; channel++) {
                        ua101_msg m = {.kind = UA101_MSG_FIELD, .value = 0x4000u};

                        hand_pan(v, 3 + group, channel, 0x03, 0x4000u);
                        bad += ua101_service_passes(v, 16, &why) != t_pan(group, channel);
                        /* Past the parser: a parse and bytes that agree on this channel,
                           whether it has a pan or not (MONITOR OUT has none). */
                        m.field.kind = UA101_F_PAN;
                        m.field.group = (uint8_t)group;
                        m.field.index = (uint8_t)channel;
                        bad += ua101_service_test_pan(v, 16, &m) != t_pan(group, channel);
                    }
                ua101_service_test_skip = 0;
                CHECK(bad == 0 && passed == 0x8001u && direct == 0x8001u,
                      "%s alone: exactly 0x0000 to 0x8000 pass for INPUT 1, also past the parser, "
                      "and no channel's pan but a passed one's (%u wrong)", alone[k], bad);
            }
        }
        for (value = 0; value <= 0x7Fu; value++) /* a digit that is more than a digit */
            for (at = 10; at < 14; at++) {
                hand_pan(v, 0x04, 0x00, 0x03, 0);
                v[at] = (uint8_t)(0x10u + value);
                v[14] = ua101_ctl_checksum(v + 6, 8);
                slipped += (unsigned)ua101_service_passes(v, 16, &why);
            }
        CHECK(slipped == 0, "a byte above 0F among the four digits never passes (%u)", slipped);

        /* Every channel there is and isn't, with four harmless values: the passed ones only. */
        for (group = 0; group < 3; group++)
            for (channel = 0; channel < 16; channel++) {
                static const uint32_t harmless[4] = {0, 1, 0x4000u, 0x8000u};
                int want = t_pan(group, channel);

                for (b = 0; b < 4; b++) {
                    hand_pan(v, 3 + group, channel, 0x03, harmless[b]);
                    count++;
                    ok += ua101_service_passes(v, 16, &why) == want &&
                          (panel_pan_message_channel(v, 16) >= 0) == (group != 0 && channel < 10);
                }
            }
        CHECK(ok == count, "of every channel's pan only the passed channels' pass (%u of %u right)", ok, count);
        CHECK(UA101_PANS_PASSED == T_PAN_BITS && UA101_PAN_PASSED_MAX == 0x8000u &&
                  (UA101_PANS_PASSED & 3u) == 0,
              "this version passes on the pan of the channels the test names, 0x0000 to 0x8000, "
              "and never one for MONITOR OUT");
        /* Every channel there is and isn't, with every value four digits can spell. */
        {
            uint32_t bad = 0, over = 0, tried = 0;

            for (group = 0; group < 3; group++)
                for (channel = 0; channel < 16; channel++)
                    for (value = 0; value <= 0xFFFFu; value++) {
                        int got;

                        hand_pan(v, 3 + group, channel, 0x03, value);
                        got = ua101_service_passes(v, 16, &why);
                        bad += got != (t_pan(group, channel) && pan_in_range(value));
                        over += got && value > 0x8000u;
                        tried++;
                    }
            CHECK(over == 0, "of %u pans over all 48 channel addresses, none above 0x8000 passes (%u)",
                  tried, over);
            CHECK(bad == 0, "and exactly 0x0000 to 0x8000 of a passed channel pass (%u wrong)", bad);
        }
        /* By brute force: every 16-byte set at 00 <a1> <a2> <a3> with the middle. */
        ok = count = 0;
        for (a1 = 0; a1 < 128; a1++)
            for (a2 = 0; a2 < 128; a2++)
                for (a3 = 0; a3 < 128; a3++) {
                    hand_pan(v, a1, a2, a3, 0x4000u);
                    count++;
                    ok += (unsigned)ua101_service_passes(v, 16, &why);
                }
        for (a1 = 1; a1 < 128; a1++) { /* and with another first address byte */
            hand_pan(v, 0x04, 0x00, 0x03, 0x4000u);
            v[6] = (uint8_t)a1;
            v[14] = ua101_ctl_checksum(v + 6, 8);
            count++;
            ok += (unsigned)ua101_service_passes(v, 16, &why);
        }
        CHECK(ok == T_PANS, "of %u messages around the pan's address, exactly one for each passed "
                            "channel passes (%u)", count, ok);
        /* The right bytes with any one of them changed to any value. */
        slipped = 0;
        for (at = 0; at < 16; at++)
            for (b = 0; b < 256; b++) {
                memcpy(v, middle, 16);
                if (v[at] == (uint8_t)b)
                    continue;
                v[at] = (uint8_t)b;
                slipped += (unsigned)ua101_service_passes(v, 16, &why);
            }
        CHECK(slipped == 0, "with any one byte of it changed, it no longer passes (%u)", slipped);
        /* A fifth digit, or three. */
        memcpy(v, middle, 14);
        v[14] = 0x00;
        v[15] = ua101_ctl_checksum(v + 6, 9);
        v[16] = 0xF7;
        CHECK(!ua101_service_passes(v, 17, &why), "five digits: refused");
        memcpy(v, middle, 13);
        v[13] = ua101_ctl_checksum(v + 6, 7);
        v[14] = 0xF7;
        CHECK(!ua101_service_passes(v, 15, &why), "three digits: refused");
    }
    /* The on/off change, version 11: the bytes, both values and every other byte value,
       every channel, the addresses around it, and each of the helper's two checks alone. */
    {
        static const uint8_t zero[13] = {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00,
                                         0x04, 0x00, 0x02, 0x00, 0x7A, 0xF7};
        static const uint8_t one[13] = {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00,
                                        0x04, 0x00, 0x02, 0x01, 0x79, 0xF7};
        uint8_t v[16], shim[13];
        unsigned group, channel, a1, a2, a3, at, b, value, ok = 0, count = 0, slipped = 0;

        CHECK(on_message(1, 0, 0, bytes) == 13 && memcmp(bytes, zero, 13) == 0,
              "INPUT 1's on/off bit to 0 is F0 41 10 00 69 12 00 04 00 02 00 7A F7");
        CHECK(on_message(1, 0, 1, bytes) == 13 && memcmp(bytes, one, 13) == 0,
              "and to 1 it is F0 41 10 00 69 12 00 04 00 02 01 79 F7");
        CHECK(on_message(1, 0, 2, bytes) == 0, "the app's builder makes no on/off with the byte 2");
        hand_on(v, 0x04, 0x00, 0x02, 1);
        panel_on_message(1, 0, 1, shim);
        CHECK(memcmp(v, one, 13) == 0 && memcmp(shim, one, 13) == 0,
              "written by hand, and by the app, the bit 1 is the same 13 bytes");
        panel_on_message(1, 0, 0, shim);
        CHECK(memcmp(shim, zero, 13) == 0 && is_passed_on(zero, 13) && is_passed_on(one, 13),
              "and so is the bit 0; the test's own reading takes both");
        CHECK(ua101_service_passes(zero, 13, &why) && ua101_service_passes(one, 13, &why),
              "both pass");
        CHECK(!ua101_service_passes(one, 12, &why), "nor cut off");
        hand_on(v, 0x04, 0x0A, 0x02, 1);
        CHECK(!ua101_service_passes(v, 13, &why) && strstr(why, "not sent") != NULL,
              "the on/off of an INPUT 11, which the mixer doesn't have: refused (%s)", why);
        hand_on(v, 0x04, 0x00, 0x00, 1);
        CHECK(ua101_service_passes(v, 13, &why) == t_solo(1),
              "the INPUT group's solo of channel 1, refused up to version 13, passes as this "
              "version's list says");
        /* Every passed channel's two messages: the app's builder, a message made by hand and
           the helper's own rows (which a message passes only by being equal to) agree. */
        {
            unsigned bad = 0, rows = 0;

            for (group = 0; group < 3; group++)
                for (channel = 0; channel < 16; channel++)
                    for (b = 0; b < 2; b++) {
                        size_t n = on_message(group, channel, b, bytes);

                        hand_on(v, 3 + group, channel, 0x02, b);
                        if (!t_on(group, channel)) {
                            bad += (unsigned)ua101_service_passes(v, 13, &why);
                            continue;
                        }
                        rows++;
                        panel_on_message(group, channel, b, shim);
                        bad += n != 13 || memcmp(bytes, v, 13) != 0 || memcmp(shim, v, 13) != 0 ||
                               !ua101_service_passes(bytes, 13, &why) || !is_passed_on(bytes, 13);
                    }
            CHECK(bad == 0 && rows == 2 * T_ONS,
                  "the %u on/off messages of the passed channels are the same by the builder, by "
                  "hand and by the app, and pass (%u wrong)", rows, bad);
        }

        /* Every byte value for INPUT 1: 0 and 1 only. */
        for (value = 0; value < 256; value++) {
            int want = value <= 1;

            hand_on(v, 0x04, 0x00, 0x02, value);
            count++;
            ok += ua101_service_passes(v, 13, &why) == want &&
                  (panel_on_message_channel(v, 13) == 2) == want;
        }
        CHECK(ok == count, "of the 256 values of the byte, only 00 and 01 pass for INPUT 1, and "
                           "the app's own rule says the same (%u of %u right)", ok, count);

        /* Every channel there is and isn't, with every byte value: 0 and 1 of the passed ones. */
        ok = count = 0;
        for (group = 0; group < 3; group++)
            for (channel = 0; channel < 16; channel++)
                for (b = 0; b < 256; b++) {
                    int want = t_on(group, channel) && b <= 1;

                    hand_on(v, 3 + group, channel, 0x02, b);
                    count++;
                    ok += ua101_service_passes(v, 13, &why) == want &&
                          (panel_on_message_channel(v, 13) >= 0) ==
                              (b <= 1 && (group == 0 ? channel < 2 : channel < 10));
                }
        CHECK(ok == count, "of every channel's on/off only the passed channels' 00 and 01 pass (%u of %u right)", ok, count);
        CHECK(UA101_ONS_PASSED == T_ON_BITS && (UA101_CTL_PASSES_ON & 0xFFFFFFFFull) == T_ON_BITS &&
                  (UA101_CTL_PASSES & UA101_CTL_PASS_ON) != 0,
              "this version passes on the on/off of the channels the test names, and says so");

        /* Each of the helper's two checks alone (this test's build can leave one out): each
           must keep to the rule by itself. The parser already stops a byte above 1, so the
           check is also called directly, with a parse made by hand that says what the test
           wants it to say, right or wrong. */
        {
            static const unsigned skip[2] = {32u, 16u};
            static const char *const alone[2] = {"the first check (the parser's field and value)",
                                                 "the second check (the 13 bytes, whole)"};
            unsigned k;

            for (k = 0; k < 2; k++) {
                unsigned bad = 0, passed = 0;

                ua101_service_test_skip = skip[k];
                for (group = 0; group < 3; group++)
                    for (channel = 0; channel < 16; channel++)
                        for (value = 0; value < 4; value++) {
                            int want = t_on(group, channel) && value <= 1;

                            hand_on(v, 3 + group, channel, 0x02, value);
                            b = (unsigned)ua101_service_passes(v, 13, &why);
                            bad += b != (unsigned)want;
                            passed += b;
                        }
                /* Past the parser: the parse says one thing, the bytes another. With the
                   first check alone, a parse naming a channel that isn't passed, or the value
                   2, is refused; with the second alone, bytes naming such a channel, or the
                   byte 2, are. */
                {
                    ua101_msg m;

                    memset(&m, 0, sizeof(m));
                    m.kind = UA101_MSG_FIELD;
                    m.field.kind = UA101_F_ON;
                    m.field.group = 1;
                    if (k == 0) { /* the first check alone: lie with the bytes */
                        m.value = 1;
                        bad += ua101_service_test_on(one, 13, &m) != 1;
                        for (group = 0; group < 3; group++)
                            for (channel = 0; channel < 16; channel++) {
                                if (t_on(group, channel))
                                    continue;
                                m.field.group = (uint8_t)group;
                                m.field.index = (uint8_t)channel;
                                bad += ua101_service_test_on(one, 13, &m) != 0;
                            }
                        m.field.group = 1;
                        m.field.index = 0;
                        m.value = 2;
                        bad += ua101_service_test_on(one, 13, &m) != 0;
                    } else { /* the second check alone: lie with the parse */
                        m.value = 1;
                        for (group = 0; group < 3; group++)
                            for (channel = 0; channel < 16; channel++) {
                                if (t_on(group, channel))
                                    continue;
                                hand_on(v, 3 + group, channel, 0x02, 1);
                                bad += ua101_service_test_on(v, 13, &m) != 0;
                            }
                        hand_on(v, 0x04, 0x00, 0x02, 2);
                        bad += ua101_service_test_on(v, 13, &m) != 0;
                        bad += ua101_service_test_on(one, 13, &m) != 1;
                    }
                }
                ua101_service_test_skip = 0;
                CHECK(bad == 0 && passed == 2 * T_ONS,
                      "%s alone: only a passed channel with 0 or 1 passes, also past the parser (%u wrong)",
                      alone[k], bad);
            }
        }

        /* By brute force: every 13-byte set at 00 <a1> <a2> 02, and at 00 04 00 <a3>, with
           every payload byte and a right checksum. Left out: 00 02 01 02, which is OUTPUT
           5-6's patch bay source, passed on and counted with the patch bay above. */
        ok = count = 0;
        for (a1 = 0; a1 < 128; a1++)
            for (a2 = 0; a2 < 128; a2++)
                for (value = 0; value < 128; value++) {
                    if (a1 == 0x02 && a2 == 0x01)
                        continue;
                    hand_on(v, a1, a2, 0x02, value);
                    count++;
                    ok += (unsigned)ua101_service_passes(v, 13, &why);
                }
        for (a3 = 0; a3 < 128; a3++)
            for (value = 0; value < 128; value++) {
                if (a3 == 0x02)
                    continue; /* counted above */
                hand_on(v, 0x04, 0x00, a3, value);
                count++;
                ok += (unsigned)ua101_service_passes(v, 13, &why);
            }
        /* The second loop also meets 00 04 00 01, INPUT 1-2's link (two values where that pair
           is passed), and 00 04 00 00, the INPUT group's solo (eleven where it is). */
        CHECK(ok == 2 * T_ONS + 2u * (unsigned)t_link(1, 0) + (unsigned)T_SOLO_VALUES * (unsigned)t_solo(1),
              "of %u messages around the on/off's address, exactly two for each passed channel "
              "pass, and the link and the solo that lie there (%u)", count, ok);
        /* The right bytes with any one of them changed to any value: only the byte itself,
           changed to the other value with its checksum, could pass, and that takes two. */
        for (at = 0; at < 13; at++)
            for (b = 0; b < 256; b++) {
                memcpy(v, one, 13);
                if (v[at] == (uint8_t)b)
                    continue;
                v[at] = (uint8_t)b;
                slipped += (unsigned)ua101_service_passes(v, 13, &why);
            }
        CHECK(slipped == 0, "with any one byte of it changed, it no longer passes (%u)", slipped);
        memcpy(v, one, 11);
        v[11] = 0x00;
        v[12] = ua101_ctl_checksum(v + 6, 6);
        v[13] = 0xF7;
        CHECK(!ua101_service_passes(v, 14, &why), "two bytes of payload: refused");
    }
    /* The stereo link, version 14: the bytes, both values and every other byte value, every
       pair there is and isn't, the addresses around it, and each of the helper's two checks
       alone. */
    {
        static const uint8_t off[13] = {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00,
                                        0x04, 0x00, 0x01, 0x00, 0x7B, 0xF7};
        static const uint8_t on[13] = {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00,
                                       0x04, 0x00, 0x01, 0x01, 0x7A, 0xF7};
        static const uint8_t out910[13] = {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00,
                                           0x05, 0x08, 0x01, 0x01, 0x71, 0xF7};
        static const uint8_t monitor[13] = {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00,
                                            0x03, 0x00, 0x01, 0x00, 0x7C, 0xF7};
        uint8_t v[16], shim[13];
        unsigned group, pair, chan, a1, a2, at, b, value, ok = 0, count = 0, slipped = 0;

        CHECK(link_message(1, 0, 0, bytes) == 13 && memcmp(bytes, off, 13) == 0,
              "INPUT 1-2's link to 0 is F0 41 10 00 69 12 00 04 00 01 00 7B F7");
        CHECK(link_message(1, 0, 1, bytes) == 13 && memcmp(bytes, on, 13) == 0,
              "and to 1 it is F0 41 10 00 69 12 00 04 00 01 01 7A F7");
        CHECK(link_message(2, 4, 1, bytes) == 13 && memcmp(bytes, out910, 13) == 0,
              "OUTPUT 9-10's link to 1 is F0 41 10 00 69 12 00 05 08 01 01 71 F7: the pair's first "
              "channel is in the address");
        CHECK(link_message(0, 0, 0, bytes) == 13 && memcmp(bytes, monitor, 13) == 0,
              "MONITOR OUT L-R's link to 0 is F0 41 10 00 69 12 00 03 00 01 00 7C F7");
        CHECK(link_message(1, 0, 2, bytes) == 0 && link_message(1, 5, 1, bytes) == 0 &&
                  link_message(0, 1, 1, bytes) == 0,
              "the app's builder makes no link with the byte 2, and none for a pair the mixer doesn't have");
        hand_on(v, 0x04, 0x00, 0x01, 1);
        panel_link_message(1, 0, 1, shim);
        CHECK(memcmp(v, on, 13) == 0 && memcmp(shim, on, 13) == 0,
              "written by hand, and by the app, the link to 1 is the same 13 bytes");
        panel_link_message(1, 0, 0, shim);
        CHECK(memcmp(shim, off, 13) == 0 && is_passed_link(off, 13) && is_passed_link(on, 13) &&
                  is_passed_link(out910, 13) && is_passed_link(monitor, 13),
              "and so is the link to 0; the test's own reading takes all four");
        CHECK(ua101_service_passes(off, 13, &why) && ua101_service_passes(on, 13, &why) &&
                  ua101_service_passes(out910, 13, &why) && ua101_service_passes(monitor, 13, &why),
              "all four pass");
        CHECK(!ua101_service_passes(on, 12, &why), "nor cut off");
        hand_on(v, 0x04, 0x0A, 0x01, 1);
        CHECK(!ua101_service_passes(v, 13, &why) && strstr(why, "not sent") != NULL,
              "the link of a sixth INPUT pair, which the mixer doesn't have: refused (%s)", why);
        hand_on(v, 0x04, 0x01, 0x01, 1);
        CHECK(!ua101_service_passes(v, 13, &why) && strstr(why, "not sent") != NULL,
              "a link at INPUT 2's address, which starts no pair: refused (%s)", why);
        /* Every passed pair's two messages: the app's builder, a message made by hand and the
           helper's own rows (which a message passes only by being equal to) agree. */
        {
            unsigned bad = 0, rows = 0;

            for (group = 0; group < 3; group++)
                for (pair = 0; pair < 8; pair++)
                    for (b = 0; b < 2; b++) {
                        size_t n = link_message(group, pair, b, bytes);

                        hand_on(v, 3 + group, 2 * pair, 0x01, b);
                        if (!t_link(group, pair)) {
                            bad += (unsigned)ua101_service_passes(v, 13, &why);
                            continue;
                        }
                        rows++;
                        panel_link_message(group, pair, b, shim);
                        bad += n != 13 || memcmp(bytes, v, 13) != 0 || memcmp(shim, v, 13) != 0 ||
                               !ua101_service_passes(bytes, 13, &why) || !is_passed_link(bytes, 13);
                    }
            CHECK(bad == 0 && rows == 2 * T_LINKS,
                  "the %u link messages of the passed pairs are the same by the builder, by hand "
                  "and by the app, and pass (%u wrong)", rows, bad);
        }

        /* Every byte value for INPUT 1-2: 0 and 1 only. */
        for (value = 0; value < 256; value++) {
            int want = value <= 1;

            hand_on(v, 0x04, 0x00, 0x01, value);
            count++;
            ok += ua101_service_passes(v, 13, &why) == want &&
                  (panel_link_message_pair(v, 13) == 1) == want;
        }
        CHECK(ok == count, "of the 256 values of the byte, only 00 and 01 pass for INPUT 1-2, and "
                           "the app's own rule says the same (%u of %u right)", ok, count);

        /* Every channel byte of every group, even and odd, with every byte value: 0 and 1 of
           the passed pairs, at the pair's first channel. */
        ok = count = 0;
        for (group = 0; group < 3; group++)
            for (chan = 0; chan < 16; chan++)
                for (b = 0; b < 256; b++) {
                    int starts = chan % 2u == 0;
                    int want = starts && t_link(group, chan / 2u) && b <= 1;

                    hand_on(v, 3 + group, chan, 0x01, b);
                    count++;
                    ok += ua101_service_passes(v, 13, &why) == want &&
                          (panel_link_message_pair(v, 13) >= 0) ==
                              (b <= 1 && starts && (group == 0 ? chan < 2 : chan < 10));
                }
        CHECK(ok == count, "of every pair's link only the passed pairs' 00 and 01 pass (%u of %u right)", ok, count);
        CHECK(UA101_LINKS_PASSED == T_LINK_BITS && ((UA101_CTL_PASSES_ON >> 32) & 0xFFFFull) == T_LINK_BITS &&
                  (UA101_CTL_PASSES & UA101_CTL_PASS_LINK) != 0 &&
                  UA101_CTL_PASS_LINK_PAIR(0) == 0x100000000ull && UA101_CTL_PASS_LINK_PAIR(10) == 0x40000000000ull,
              "this version passes on the link of the pairs the test names, and says so");

        /* Each of the helper's two checks alone (this test's build can leave one out): each
           must keep to the rule by itself. The parser already stops a byte above 1 and an odd
           channel, so the check is also called directly, with a parse made by hand that says
           what the test wants it to say, right or wrong. */
        {
            static const unsigned skip[2] = {2048u, 1024u};
            static const char *const alone[2] = {"the first check (the parser's field and value)",
                                                 "the second check (the 13 bytes, whole)"};
            unsigned k;

            for (k = 0; k < 2; k++) {
                unsigned bad = 0, passed = 0;

                ua101_service_test_skip = skip[k];
                for (group = 0; group < 3; group++)
                    for (chan = 0; chan < 16; chan++)
                        for (value = 0; value < 4; value++) {
                            int want = chan % 2u == 0 && t_link(group, chan / 2u) && value <= 1;

                            hand_on(v, 3 + group, chan, 0x01, value);
                            b = (unsigned)ua101_service_passes(v, 13, &why);
                            bad += b != (unsigned)want;
                            passed += b;
                        }
                {
                    ua101_msg m;

                    memset(&m, 0, sizeof(m));
                    m.kind = UA101_MSG_FIELD;
                    m.field.kind = UA101_F_LINK;
                    m.field.group = 1;
                    if (k == 0) { /* the first check alone: lie with the bytes */
                        m.value = 1;
                        bad += ua101_service_test_link(on, 13, &m) != 1;
                        for (group = 0; group < 4; group++)
                            for (pair = 0; pair < 8; pair++) {
                                if (group < 3 && t_link(group, pair))
                                    continue;
                                m.field.group = (uint8_t)group;
                                m.field.index = (uint8_t)pair;
                                bad += ua101_service_test_link(on, 13, &m) != 0;
                            }
                        m.field.group = 1;
                        m.field.index = 0;
                        m.value = 2;
                        bad += ua101_service_test_link(on, 13, &m) != 0;
                    } else { /* the second check alone: lie with the parse */
                        m.value = 1;
                        for (group = 0; group < 4; group++)
                            for (chan = 0; chan < 16; chan++) {
                                if (group < 3 && chan % 2u == 0 && t_link(group, chan / 2u))
                                    continue;
                                hand_on(v, 3 + group, chan, 0x01, 1);
                                bad += ua101_service_test_link(v, 13, &m) != 0;
                            }
                        hand_on(v, 0x04, 0x00, 0x01, 2);
                        bad += ua101_service_test_link(v, 13, &m) != 0;
                        bad += ua101_service_test_link(on, 13, &m) != 1;
                    }
                }
                ua101_service_test_skip = 0;
                CHECK(bad == 0 && passed == 2 * T_LINKS,
                      "the link, %s alone: only a passed pair with 0 or 1 passes, also past the parser (%u wrong)",
                      alone[k], bad);
            }
        }

        /* The list itself. With every pair on it, a check that forgot to ask its list would
           look the same as one that asks. So for a moment the list is made shorter (this test's
           build only): INPUT 1-2 and OUTPUT 9-10. Both checks together, and each alone, then
           pass those two pairs only; and a link that is on no list is refused with the
           sentence that names what is passed on. */
        {
            static const unsigned skip[3] = {0u, 2048u, 1024u};
            unsigned k, bad = 0, passed = 0;

            ua101_service_test_links = (1u << 1) | (1u << 10);
            for (k = 0; k < 3; k++) {
                ua101_service_test_skip = skip[k];
                for (group = 0; group < 3; group++)
                    for (pair = 0; pair < 8; pair++)
                        for (value = 0; value < 2; value++) {
                            int want = (group == 1 && pair == 0) || (group == 2 && pair == 4);

                            hand_on(v, 3 + group, 2 * pair, 0x01, value);
                            b = (unsigned)ua101_service_passes(v, 13, &why);
                            bad += b != (unsigned)want;
                            passed += b;
                        }
            }
            ua101_service_test_skip = 0;
            hand_on(v, 0x04, 0x02, 0x01, 1);
            CHECK(!ua101_service_passes(v, 13, &why) && strstr(why, "seven changes only") != NULL &&
                      strstr(why, "not sent") != NULL,
                  "INPUT 3-4's link, on no list for a moment: refused, and the reason names what is passed on (%s)", why);
            ua101_service_test_links = UA101_LINKS_PASSED;
            CHECK(bad == 0 && passed == 3 * 4,
                  "with the list made shorter for a moment (INPUT 1-2 and OUTPUT 9-10), both checks "
                  "together and each alone pass those two pairs only (%u wrong)", bad);
            CHECK(ua101_service_passes(v, 13, &why), "with the whole list back, INPUT 3-4's link passes again");
        }

        /* By brute force: every 13-byte set at 00 <a1> <a2> 01, with every payload byte and a
           right checksum. Left out: 00 02 01 01, which is OUTPUT 3-4's patch bay source, passed
           on and counted with the patch bay above. (00 04 00 <a3> is counted with the on/off.) */
        ok = count = 0;
        for (a1 = 0; a1 < 128; a1++)
            for (a2 = 0; a2 < 128; a2++)
                for (value = 0; value < 128; value++) {
                    if (a1 == 0x02 && a2 == 0x01)
                        continue;
                    hand_on(v, a1, a2, 0x01, value);
                    count++;
                    ok += (unsigned)ua101_service_passes(v, 13, &why);
                }
        CHECK(ok == 2 * T_LINKS, "of %u messages around the link's address, exactly two for each "
                                 "passed pair pass (%u)", count, ok);
        /* The right bytes with any one of them changed to any value: nothing passes. */
        for (at = 0; at < 13; at++)
            for (b = 0; b < 256; b++) {
                memcpy(v, on, 13);
                if (v[at] == (uint8_t)b)
                    continue;
                v[at] = (uint8_t)b;
                slipped += (unsigned)ua101_service_passes(v, 13, &why);
            }
        CHECK(slipped == 0, "with any one byte of it changed, it no longer passes (%u)", slipped);
        memcpy(v, on, 11);
        v[11] = 0x00;
        v[12] = ua101_ctl_checksum(v + 6, 6);
        v[13] = 0xF7;
        CHECK(!ua101_service_passes(v, 14, &why), "two bytes of payload: refused");
    }
    /* The solo, version 14: one number a group, 0 for none or the soloed channel 1 to 10 (read
       from Roland's program, 2026-11-16). The bytes, every value, every group and channel
       byte, the addresses around it, and each of the helper's two checks alone. */
    {
        static const uint8_t none[13] = {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00,
                                         0x04, 0x00, 0x00, 0x00, 0x7C, 0xF7};
        static const uint8_t in3[13] = {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00,
                                        0x04, 0x00, 0x00, 0x03, 0x79, 0xF7};
        static const uint8_t out10[13] = {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00,
                                          0x05, 0x00, 0x00, 0x0A, 0x71, 0xF7};
        uint8_t v[16], shim[13];
        unsigned group, chan, a1, a2, at, b, value, ok = 0, count = 0, slipped = 0;

        CHECK(solo_message(1, 0, bytes) == 13 && memcmp(bytes, none, 13) == 0,
              "the INPUT group's solo to 0, none, is F0 41 10 00 69 12 00 04 00 00 00 7C F7");
        CHECK(solo_message(1, 3, bytes) == 13 && memcmp(bytes, in3, 13) == 0,
              "INPUT 3 alone is F0 41 10 00 69 12 00 04 00 00 03 79 F7");
        CHECK(solo_message(2, 10, bytes) == 13 && memcmp(bytes, out10, 13) == 0,
              "OUTPUT 10 alone is F0 41 10 00 69 12 00 05 00 00 0A 71 F7");
        CHECK(solo_message(1, 11, bytes) == 0 && solo_message(0, 1, bytes) == 0 && solo_message(2, 0x7F, bytes) == 0,
              "the app's builder makes no solo above 10, and none for MONITOR OUT");
        hand_on(v, 0x04, 0x00, 0x00, 3);
        panel_solo_message(1, 3, shim);
        CHECK(memcmp(v, in3, 13) == 0 && memcmp(shim, in3, 13) == 0,
              "written by hand, and by the app, INPUT 3 alone is the same 13 bytes");
        panel_solo_message(2, 10, shim);
        CHECK(memcmp(shim, out10, 13) == 0 && is_passed_solo(none, 13) && is_passed_solo(in3, 13) &&
                  is_passed_solo(out10, 13),
              "and so is OUTPUT 10 alone; the test's own reading takes all three");
        CHECK(ua101_service_passes(none, 13, &why) && ua101_service_passes(in3, 13, &why) &&
                  ua101_service_passes(out10, 13, &why),
              "all three pass");
        CHECK(!ua101_service_passes(in3, 12, &why), "nor cut off");
        hand_on(v, 0x04, 0x00, 0x00, 11);
        CHECK(!ua101_service_passes(v, 13, &why) && strstr(why, "not sent") != NULL,
              "a solo of an INPUT 11, which the mixer doesn't have: refused (%s)", why);
        hand_on(v, 0x03, 0x00, 0x00, 1);
        CHECK(!ua101_service_passes(v, 13, &why) && strstr(why, "not sent") != NULL,
              "a solo at MONITOR OUT's address, which has none: refused (%s)", why);
        /* Every passed group's eleven messages: the app's builder, a message made by hand and
           the helper's own rows agree. */
        {
            unsigned bad = 0, rows = 0;

            for (group = 0; group < 4; group++)
                for (value = 0; value < 16; value++) {
                    size_t n = group < 3 ? solo_message(group, value, bytes) : 0;

                    hand_on(v, 3 + group, 0x00, 0x00, value);
                    if (group > 2 || !t_solo(group) || value >= T_SOLO_VALUES) {
                        bad += (unsigned)ua101_service_passes(v, 13, &why) + (n != 0);
                        continue;
                    }
                    rows++;
                    panel_solo_message(group, value, shim);
                    bad += n != 13 || memcmp(bytes, v, 13) != 0 || memcmp(shim, v, 13) != 0 ||
                           !ua101_service_passes(bytes, 13, &why) || !is_passed_solo(bytes, 13);
                }
            CHECK(bad == 0 && rows == T_SOLOS * T_SOLO_VALUES,
                  "the %u solo messages of the passed groups are the same by the builder, by hand "
                  "and by the app, and pass (%u wrong)", rows, bad);
        }

        /* Every byte value for the INPUT group: 0 to 10 only. */
        for (value = 0; value < 256; value++) {
            int want = value <= 10;

            hand_on(v, 0x04, 0x00, 0x00, value);
            count++;
            ok += ua101_service_passes(v, 13, &why) == want &&
                  (panel_solo_message_group(v, 13) == 1) == want;
        }
        CHECK(ok == count, "of the 256 values of the byte, only 00 to 0A pass for the INPUT group, "
                           "and the app's own rule says the same (%u of %u right)", ok, count);

        /* Every group and every channel byte, with every byte value: 0 to 10 of the passed
           groups, at the channel byte 00. */
        ok = count = 0;
        for (group = 0; group < 3; group++)
            for (chan = 0; chan < 16; chan++)
                for (b = 0; b < 256; b++) {
                    int want = t_solo(group) && chan == 0 && b <= 10;

                    hand_on(v, 3 + group, chan, 0x00, b);
                    count++;
                    ok += ua101_service_passes(v, 13, &why) == want &&
                          (panel_solo_message_group(v, 13) >= 0) == (group != 0 && chan == 0 && b <= 10);
                }
        CHECK(ok == count, "of every group's solo only the passed groups' 00 to 0A pass (%u of %u right)", ok, count);
        CHECK(UA101_SOLOS_PASSED == T_SOLO_BITS && UA101_SOLO_PASSED_MAX == T_SOLO_VALUES - 1 &&
                  (UA101_CTL_PASSES_ON >> 48) == T_SOLO_BITS && (UA101_CTL_PASSES & UA101_CTL_PASS_SOLO) != 0 &&
                  UA101_CTL_PASS_SOLO_GROUP(1) == 0x1000000000000ull &&
                  UA101_CTL_PASS_SOLO_GROUP(2) == 0x2000000000000ull,
              "this version passes on the solo of the groups the test names, up to channel 10, and says so");

        /* Each of the helper's two checks alone. The parser already stops a solo above 10, one
           for MONITOR OUT and a channel byte that isn't 00, so the check is also called
           directly, with a parse made by hand. */
        {
            static const unsigned skip[2] = {8192u, 4096u};
            static const char *const alone[2] = {"the first check (the parser's field and value)",
                                                 "the second check (the 13 bytes, whole)"};
            unsigned k;

            for (k = 0; k < 2; k++) {
                unsigned bad = 0, passed = 0;

                ua101_service_test_skip = skip[k];
                for (group = 0; group < 3; group++)
                    for (chan = 0; chan < 4; chan++)
                        for (value = 0; value < 14; value++) {
                            int want = t_solo(group) && chan == 0 && value <= 10;

                            hand_on(v, 3 + group, chan, 0x00, value);
                            b = (unsigned)ua101_service_passes(v, 13, &why);
                            bad += b != (unsigned)want;
                            passed += b;
                        }
                {
                    ua101_msg m;

                    memset(&m, 0, sizeof(m));
                    m.kind = UA101_MSG_FIELD;
                    m.field.kind = UA101_F_SOLO;
                    m.field.group = 1;
                    m.value = 3;
                    if (k == 0) { /* the first check alone: lie with the bytes */
                        bad += ua101_service_test_solo(in3, 13, &m) != 1;
                        for (group = 0; group < 5; group++) {
                            if (group < 3 && t_solo(group))
                                continue;
                            m.field.group = (uint8_t)group;
                            bad += ua101_service_test_solo(in3, 13, &m) != 0;
                        }
                        m.field.group = 1;
                        m.field.index = 1;
                        bad += ua101_service_test_solo(in3, 13, &m) != 0;
                        m.field.index = 0;
                        for (value = 11; value < 130; value++) {
                            m.value = value;
                            bad += ua101_service_test_solo(in3, 13, &m) != 0;
                        }
                    } else { /* the second check alone: lie with the parse */
                        for (group = 0; group < 4; group++)
                            for (chan = 0; chan < 4; chan++)
                                for (value = 0; value < 130; value++) {
                                    if (group < 3 && t_solo(group) && chan == 0 && value <= 10)
                                        continue;
                                    hand_on(v, 3 + group, chan, 0x00, value);
                                    bad += ua101_service_test_solo(v, 13, &m) != 0;
                                }
                        bad += ua101_service_test_solo(in3, 13, &m) != 1;
                    }
                }
                ua101_service_test_skip = 0;
                CHECK(bad == 0 && passed == T_SOLOS * T_SOLO_VALUES,
                      "the solo, %s alone: only a passed group with 0 to 10 passes, also past the parser (%u wrong)",
                      alone[k], bad);
            }
        }

        /* The list itself, made shorter for a moment as the link's was: the OUTPUT group only.
           Both checks together, and each alone, then pass the OUTPUT group's eleven numbers
           only; the INPUT group's solo, on no list, is refused with the sentence that names
           what is passed on. */
        {
            static const unsigned skip[3] = {0u, 8192u, 4096u};
            unsigned k, bad = 0, passed = 0;

            ua101_service_test_solos = 1u << 1;
            for (k = 0; k < 3; k++) {
                ua101_service_test_skip = skip[k];
                for (group = 0; group < 3; group++)
                    for (value = 0; value < 12; value++) {
                        int want = group == 2 && value <= 10;

                        hand_on(v, 3 + group, 0x00, 0x00, value);
                        b = (unsigned)ua101_service_passes(v, 13, &why);
                        bad += b != (unsigned)want;
                        passed += b;
                    }
            }
            ua101_service_test_skip = 0;
            CHECK(!ua101_service_passes(in3, 13, &why) && strstr(why, "seven changes only") != NULL &&
                      strstr(why, "not sent") != NULL,
                  "the INPUT group's solo, on no list for a moment: refused, and the reason names what is passed on (%s)", why);
            ua101_service_test_solos = UA101_SOLOS_PASSED;
            CHECK(bad == 0 && passed == 3 * T_SOLO_VALUES,
                  "with the list made shorter for a moment (the OUTPUT group), both checks together "
                  "and each alone pass that group's eleven numbers only (%u wrong)", bad);
            CHECK(ua101_service_passes(in3, 13, &why), "with the whole list back, INPUT 3 alone passes again");
        }

        /* By brute force: every 13-byte set at 00 <a1> <a2> 00, with every payload byte and a
           right checksum. Left out: 00 00 00 00, the saving (counted below), and 00 02 01 00,
           OUTPUT 1-2's patch bay source (counted with the patch bay above). */
        ok = count = 0;
        for (a1 = 0; a1 < 128; a1++)
            for (a2 = 0; a2 < 128; a2++)
                for (value = 0; value < 128; value++) {
                    if ((a1 == 0x00 && a2 == 0x00) || (a1 == 0x02 && a2 == 0x01))
                        continue;
                    hand_on(v, a1, a2, 0x00, value);
                    count++;
                    ok += (unsigned)ua101_service_passes(v, 13, &why);
                }
        CHECK(ok == T_SOLOS * T_SOLO_VALUES, "of %u messages around the solo's address, exactly "
                                             "eleven for each passed group pass (%u)", count, ok);
        /* The right bytes with any one of them changed to any value: nothing passes. */
        for (at = 0; at < 13; at++)
            for (b = 0; b < 256; b++) {
                memcpy(v, in3, 13);
                if (v[at] == (uint8_t)b)
                    continue;
                v[at] = (uint8_t)b;
                slipped += (unsigned)ua101_service_passes(v, 13, &why);
            }
        CHECK(slipped == 0, "with any one byte of it changed, it no longer passes (%u)", slipped);
        memcpy(v, in3, 11);
        v[11] = 0x00;
        v[12] = ua101_ctl_checksum(v + 6, 6);
        v[13] = 0xF7;
        CHECK(!ua101_service_passes(v, 14, &why), "two bytes of payload: refused");
    }
    /* What is left to refuse. Since version 14 every one of the 82 settings the app's builder
       can name is passed on, each within its range; what the helper still
       never passes on is a volume above 0 dB, Roland's one-message preset, and whatever the
       parser doesn't know; and by this function, no Load, Store or Reset (since
       version 17 those have rules of their own: test_memory_rules). */
    {
        unsigned kind, wrong = 0, memory = 0;

        for (kind = UA101_MSG_RESET_ALL; kind <= UA101_MSG_PRESET; kind++) {
            ua101_msg m;

            memset(&m, 0, sizeof(m));
            m.kind = (ua101_msg_kind)kind;
            m.memory = 2;
            m.value = 4;
            m.sections[2] = m.sections[3] = 1;
            len = ua101_ctlmsg_build(&m, bytes);
            memory += len != 0;
            wrong += len == 0 || ua101_service_passes(bytes, len, &why) ||
                     strstr(why, "Load, Store and Reset") == NULL ||
                     strstr(why, "not sent") == NULL;
        }
        CHECK(wrong == 0 && memory == 6,
              "Reset all, Reset of the settings in use, Reset one, Store, Load and Roland's "
              "preset: never by this function, and the reason says so (%u wrong)", wrong);
    }
    /* The log's stamp: local time to the millisecond. */
    {
        char stamp[40], whole[40];
        time_t t = 1794477757; /* some second */
        struct tm tm;

        localtime_r(&t, &tm);
        strftime(whole, sizeof(whole), "%Y-%m-%d %H:%M:%S", &tm);
        ua101_service_stamp(stamp, sizeof(stamp), t, 364999);
        CHECK(strlen(stamp) == 23 && strncmp(stamp, whole, 19) == 0 && strcmp(stamp + 19, ".364") == 0,
              "a log stamp reads %s: the second, a point, and three digits of milliseconds", stamp);
        ua101_service_stamp(stamp, sizeof(stamp), t, 0);
        CHECK(strcmp(stamp + 19, ".000") == 0, "at the full second it ends .000");
        ua101_service_stamp(stamp, sizeof(stamp), t, 999999);
        CHECK(strncmp(stamp, whole, 19) == 0 && strcmp(stamp + 19, ".999") == 0,
              "and just before the next it ends .999, in the same second");
    }
    /* Every 13-byte message at the saving address, and its neighbours, by brute force: all
       values of the last address byte and of the payload byte, with a right checksum. */
    {
        unsigned a3, value, ok = 0, count = 0;

        for (a3 = 0; a3 < 128; a3++)
            for (value = 0; value < 128; value++) {
                memcpy(bytes, pause, 13);
                bytes[9] = (uint8_t)a3;
                bytes[10] = (uint8_t)value;
                bytes[11] = ua101_ctl_checksum(bytes + 6, 5);
                count++;
                ok += (unsigned)ua101_service_passes(bytes, 13, &why);
            }
        CHECK(ok == 2, "of %u messages around the saving address, exactly two pass (%u)",
              count, ok);
    }
}

/* What the helper says it passes on, and the first of it, the unit's saving, through the real
   link and service. No go-ahead file exists anywhere, and none is needed (link version 18). */
static void test_saving(app *a)
{
    int wire = atomic_load(&P.wire_messages), refused, sent;
    uint64_t changes = counts().ctl_changes;

    CHECK(atomic_load(&a->passes_events) >= 1 &&
              atomic_load(&a->passes) ==
                  ((uint64_t)UA101_CTL_PASS_SAVING | UA101_CTL_PASS_PATCH | UA101_CTL_PASS_PATCH_OUTPUT(0) |
                   UA101_CTL_PASS_PATCH_OUTPUT(1) | UA101_CTL_PASS_PATCH_OUTPUT(2) |
                   UA101_CTL_PASS_PATCH_OUTPUT(3) | UA101_CTL_PASS_PATCH_OUTPUT(4) |
                   UA101_CTL_PASS_VOLUME | (T_VOLUME_BITS << 16) |
                   UA101_CTL_PASS_PAN | (T_PAN_BITS << 40) | UA101_CTL_PASS_ON |
                   UA101_CTL_PASS_LINK | UA101_CTL_PASS_SOLO | UA101_CTL_PASS_MEMORY) &&
              atomic_load(&a->passes) == UA101_CTL_PASSES &&
              UA101_CTL_PASS_VOLUME_CHANNEL(2) == 0x40000ull &&
              UA101_CTL_PASS_PAN_CHANNEL(2) == 0x40000000000ull &&
              atomic_load(&a->passes_on) == (T_ON_BITS | (T_LINK_BITS << 32) | (T_SOLO_BITS << 48)) &&
              UA101_CTL_PASS_ON_CHANNEL(2) == 4u &&
              atomic_load(&a->memory_passes) == UA101_CTL_PASSES_MEMORY,
          "with the attach the helper said what it passes on: the channels, pairs and groups "
          "the test names, a bit each, and the Load, Store and Reset commands");
    {
        /* The old question is gone: a helper of this version answers it as any request it
           doesn't know, and nothing is sent for it. */
        xpc_connection_t c = xpc_connection_create_from_endpoint(g_endpoint);
        xpc_object_t m = xpc_dictionary_create(NULL, NULL, 0), r;

        xpc_connection_set_event_handler(c, ^(xpc_object_t e) {
          (void)e;
        });
        xpc_connection_resume(c);
        xpc_dictionary_set_string(m, UA101_LINK_OP, "ctl_goahead");
        xpc_dictionary_set_uint64(m, UA101_LINK_KEY_VERSION, UA101_LINK_VERSION);
        r = xpc_connection_send_message_with_reply_sync(c, m);
        CHECK(xpc_get_type(r) == XPC_TYPE_DICTIONARY &&
                  xpc_dictionary_get_int64(r, UA101_LINK_STATUS) == UA101_LINK_ERR_REQUEST &&
                  xpc_dictionary_get_value(r, "ctl_writes") == NULL &&
                  xpc_dictionary_get_value(r, "ctl_memory") == NULL,
              "the old request \"ctl_goahead\" is answered \"not understood\", with no go-ahead in the answer");
        xpc_release(r);
        xpc_release(m);
        xpc_connection_cancel(c);
        xpc_release(c);
    }
    CHECK(atomic_load(&P.wire_messages) == wire, "nothing of that reached the cable");
    refused = atomic_load(&a->refused);
    helper_clock_forward(40); /* as in test_helper_refuses: no read of the settings is fresh */
    sent = send_everything(a, 0);
    CHECK(sent == EVERYTHING,
          "everything but the saving messages, the patch changes, the passed channels' "
          "volumes, pans and on/offs, and the passed links and solos, handed over again (%d)",
          sent);
    CHECK(WAIT(atomic_load(&a->refused) == refused + sent, 5.0),
          "the helper refused every one (%d of %d)",
          atomic_load(&a->refused) - refused, sent);
    CHECK(atomic_load(&P.wire_messages) == wire && atomic_load(&P.wire_other) == 0,
          "nothing reached the cable");
    CHECK(saving_reaches(a, 0) && WAIT(sim_paused(), 3.0), "\"pause saving\" reaches the unit, with no file anywhere");
    CHECK(counts().ctl_changes == changes + 1, "the helper counted one change");
    CHECK(saving_reaches(a, 1) && WAIT(!sim_paused(), 3.0), "\"resume saving\" reaches it too");
    CHECK(atomic_load(&P.wire_messages) == wire + 2 && atomic_load(&P.wire_saving) == 2,
          "two messages were added on the cable, both about saving");

    /* The app's engine, allowed to write: pause, read back, resume, read back. */
    {
        __block int result;
        __block uint64_t blocks;
        int reads = atomic_load(&P.wire_reads);

        on_app(^{
          panel_allow_writes(a->engine, 1);
          blocks = panel_blocks_received(a->engine);
          result = panel_pause_saving(a->engine, now_us());
        });
        CHECK(result == PANEL_OK, "the engine pauses the unit's saving (%d)", result);
        CHECK(WAIT(sim_paused() && APP(panel_blocks_received(a->engine) > blocks), 3.0),
              "the unit is paused and the settings were read back");
        CHECK(APP(panel_saving_held(a->engine)), "the engine holds the pause");
        usleep(300000);
        CHECK(sim_paused() && atomic_load(&P.wire_saving) == 3, "and doesn't resume by itself");
        on_app(^{
          blocks = panel_blocks_received(a->engine);
          result = panel_resume_saving(a->engine, now_us());
        });
        CHECK(result == PANEL_OK, "the engine resumes it");
        CHECK(WAIT(!sim_paused() && APP(panel_blocks_received(a->engine) > blocks), 3.0),
              "the unit saves again and the settings were read back");
        CHECK(WAIT(!APP(panel_busy(a->engine)), 3.0), "nothing is left waiting");
        CHECK(atomic_load(&P.wire_saving) == 4 && atomic_load(&P.wire_reads) == reads + 2,
              "on the cable: a pause, a read, a resume, a read");

        /* Paused again, then the app is told of a refusal (as after one by the helper):
           read-only, nothing waiting. The pause is still on the unit, and a resume by hand
           mends it. */
        /* Asked again until the read-back of the resume is in: a pause goes out by itself. */
        CHECK(WAIT(APP(panel_pause_saving(a->engine, now_us())) == PANEL_OK, 3.0) &&
                  WAIT(sim_paused(), 3.0),
              "paused again");
        CHECK(atomic_load(&P.wire_saving) == 5, "five saving messages on the cable so far");
        on_app(^{
          panel_writes_refused(a->engine, now_us());
        });
        CHECK(!APP(panel_writes_allowed(a->engine)) && WAIT(!APP(panel_busy(a->engine)), 3.0),
              "the engine, told of a refusal, is read-only again with nothing waiting");
        CHECK(sim_paused() && saving_reaches(a, 1) && WAIT(!sim_paused(), 3.0),
              "the unit was still paused; a resume by hand passes and it saves again");
    }
    {
        __block uint64_t sets, memory, unknown;

        on_service_queue(^{
          sets = P.sim.sets_applied + P.sim.sets_ignored;
          memory = P.sim.memory_commands;
          unknown = P.sim.unknown_messages + P.sim.bad_messages;
        });
        CHECK(sets == 0 && memory == 0 && unknown == 0,
              "the pretend unit saw no setting changed, no memory command and nothing odd");
    }
}

/* Two apps: both hear the unit; the interface is given back when the last one lets go. */
/* The helper passes on 20 messages in one second at most. Up to link version 17 the questions
   about the go-ahead and the refusals without it spread a test's messages over time; without
   them a test waits here, so that its own count starts in a second of its own. */
static void fresh_second(void)
{
    usleep(1100000);
}

/* Sends OUTPUT <output>'s patch bay source by hand and says whether the helper passed it on
   (the engine's own reads may be on the cable at the same time, so the cable isn't counted). */
static int patch_reaches(app *a, unsigned output, unsigned source)
{
    uint8_t bytes[UA101_CTLMSG_MAX_BYTES];
    uint64_t changes = counts().ctl_changes;
    int refused = atomic_load(&a->refused);
    size_t len = patch_message(output, source, bytes);

    send_raw(a, bytes, len);
    WAIT(counts().ctl_changes > changes || atomic_load(&a->refused) > refused, 3.0);
    return counts().ctl_changes > changes;
}

/* Link version 7: the patch bay source of every output pair. */
static void test_patch(app *a)
{
    int patch = atomic_load(&P.wire_patch), saving = atomic_load(&P.wire_saving);
    int reads, refused;
    uint64_t changes = counts().ctl_changes;
    __block int result;
    __block uint64_t blocks, disagreements;
    unsigned output;

    fresh_second();
    CHECK(sim_value(UA101_F_PATCH, 0, 0) == 7, "the pretend unit still has WAVE OUT 1-2 there");
    CHECK(patch_reaches(a, 0, 2) && WAIT(sim_value(UA101_F_PATCH, 0, 0) == 2, 3.0),
          "OUTPUT 1-2 to INPUT 1-2 reaches the unit");
    CHECK(patch_reaches(a, 0, 7) && WAIT(sim_value(UA101_F_PATCH, 0, 0) == 7, 3.0), "and back to WAVE OUT 1-2");
    CHECK(counts().ctl_changes == changes + 2 && atomic_load(&P.wire_patch) == patch + 2,
          "the helper counted two changes; two patch messages on the cable");
    for (output = 1; output < 5; output++)
        CHECK(patch_reaches(a, output, 2 + output) &&
                  WAIT(sim_value(UA101_F_PATCH, 0, output) == 2 + output, 3.0) &&
                  patch_reaches(a, output, 7 + output) &&
                  WAIT(sim_value(UA101_F_PATCH, 0, output) == 7 + output, 3.0),
              "OUTPUT %u-%u's patch change reaches the unit too, and back", 2 * output + 1, 2 * output + 2);
    CHECK(!patch_reaches(a, 5, 2) && !patch_reaches(a, 0, 12) && !patch_reaches(a, 0, 0),
          "an output pair or a source that doesn't exist is refused");

    /* The app's engine: one patch change, read back, and back again. No pause of the saving
       goes with it: a single change isn't a drag. */
    fresh_second();
    reads = atomic_load(&P.wire_reads);
    on_app(^{
      panel_allow_writes(a->engine, 1);
      blocks = panel_blocks_received(a->engine);
      disagreements = panel_disagreements(a->engine);
      result = panel_set_patch(a->engine, 0, 2, now_us());
    });
    CHECK(result == PANEL_OK, "the engine, allowed to write, changes OUTPUT 1-2 to INPUT 1-2 (%d)", result);
    CHECK(WAIT(sim_value(UA101_F_PATCH, 0, 0) == 2 && APP(panel_blocks_received(a->engine) > blocks) &&
                   !APP(panel_busy(a->engine)), 3.0),
          "the unit took it and the settings were read back");
    CHECK(APP(panel_patch(a->engine, 0)) == 2 && (uint64_t)APP(panel_disagreements(a->engine)) == disagreements,
          "the engine shows INPUT 1-2 and found no disagreement");
    {
        uint8_t block[256], unit[256];
        uint8_t *bp = block, *up = unit;
        unsigned i, differ = 0;

        on_app(^{
          panel_unit_block(a->engine, bp);
        });
        on_service_queue(^{
          memcpy(up, P.sim.block, 256);
        });
        CHECK(memcmp(block, unit, 256) == 0 && block[12] == 2, "the block read back has d[12] = 02");
        unit[12] = 7;
        for (i = 0; i < 256; i++)
            differ += block[i] != unit[i];
        CHECK(differ == 1, "and d[12] is the only byte the change moved");
    }
    on_app(^{
      blocks = panel_blocks_received(a->engine);
      result = panel_set_patch(a->engine, 0, 7, now_us());
    });
    CHECK(result == PANEL_OK &&
              WAIT(sim_value(UA101_F_PATCH, 0, 0) == 7 && APP(panel_blocks_received(a->engine) > blocks) &&
                       !APP(panel_busy(a->engine)), 3.0),
          "and back to WAVE OUT 1-2, read back");
    CHECK(atomic_load(&P.wire_patch) == patch + 12 && atomic_load(&P.wire_reads) == reads + 2 &&
              atomic_load(&P.wire_saving) == saving,
          "on the cable: a patch change, a read, a patch change, a read; no pause or resume of saving");

    /* A refusal. Up to version 13 this was the engine let loose on
       the link of INPUT 1-2; since version 14 every setting the engine can name is passed on
       within its range, and the engine itself makes no volume past the top of the range. So
       the message is made by hand: INPUT 1 one step past the top (0x800000; up to version 18,
       one step above 0 dB). The helper refuses it, and the app's answer to a
       refusal (read-only again, read the unit) leaves the engine showing what the unit has. */
    refused = atomic_load(&a->refused);
    CHECK(sim_value(UA101_F_VOLUME, 1, 0) == UA101_CTL_UNITY_VOLUME, "the pretend unit has INPUT 1 at 0 dB, as the real one reads");
    {
        uint8_t over[18];

        hand_volume(over, 0x04, 0x00, 0x07, 0x800000u);
        send_raw(a, over, 18);
    }
    CHECK(WAIT(atomic_load(&a->refused) == refused + 1, 3.0) && strstr(refused_why(a), "not sent") != NULL,
          "INPUT 1 to 0x800000, by hand: the helper refuses it (%s)", refused_why(a));
    on_app(^{
      blocks = panel_blocks_received(a->engine);
      panel_writes_refused(a->engine, now_us());
    });
    CHECK(WAIT(APP(panel_blocks_received(a->engine) > blocks) && !APP(panel_busy(a->engine)), 3.0) &&
              sim_value(UA101_F_VOLUME, 1, 0) == UA101_CTL_UNITY_VOLUME &&
              APP(panel_volume(a->engine, 1, 0)) == PANEL_UNITY_VOLUME && !APP(panel_writes_allowed(a->engine)),
          "the unit keeps 0 dB, and the engine, read-only again, shows it");

    {
        __block uint64_t applied, ignored, memory, unknown;

        on_service_queue(^{
          applied = P.sim.sets_applied;
          ignored = P.sim.sets_ignored;
          memory = P.sim.memory_commands;
          unknown = P.sim.unknown_messages + P.sim.bad_messages;
        });
        CHECK(applied == 12 && ignored == 0 && memory == 0 && unknown == 0,
              "the pretend unit saw twelve settings changed, all patch bay sources, and nothing odd");
    }
}

/* Sends one volume by hand and says whether the helper passed it on. */
static int volume_reaches(app *a, unsigned group, unsigned channel, uint32_t value)
{
    uint8_t bytes[18];
    uint64_t changes = counts().ctl_changes;
    int refused = atomic_load(&a->refused);

    hand_volume(bytes, 3 + group, channel, 0x07, value);
    send_raw(a, bytes, 18);
    WAIT(counts().ctl_changes > changes || atomic_load(&a->refused) > refused, 3.0);
    return counts().ctl_changes > changes;
}

/* Link version 8: INPUT 1's volume at any value up to 0 dB, never above,
   and no other channel's. Since version 19 up to the top of the range (about +12 dB). */
static void test_volume(app *a)
{
    int volume = atomic_load(&P.wire_volume), saving = atomic_load(&P.wire_saving);
    int reads, refused, i;
    uint64_t changes = counts().ctl_changes;
    __block int result;
    __block uint64_t blocks, disagreements;
    uint8_t before[256], after[256];
    uint8_t *bp = before, *ap = after;
    unsigned differ = 0;

    fresh_second();
    CHECK(sim_value(UA101_F_VOLUME, 1, 0) == 0x200000u, "the pretend unit still has 0 dB there");
    CHECK(volume_reaches(a, 1, 0, 0x100001u) && WAIT(sim_value(UA101_F_VOLUME, 1, 0) == 0x100001u, 3.0),
          "INPUT 1's volume 0x100001 reaches the unit");
    CHECK(volume_reaches(a, 1, 0, 0x200000u) && WAIT(sim_value(UA101_F_VOLUME, 1, 0) == 0x200000u, 3.0), "and back to 0 dB");
    CHECK(counts().ctl_changes == changes + 2 && atomic_load(&P.wire_volume) == volume + 2,
          "the helper counted two changes; two volume messages on the cable");
    CHECK(volume_reaches(a, 1, 0, 0x1F0000u) && volume_reaches(a, 1, 0, 0x00000Fu) &&
              volume_reaches(a, 1, 0, 0x16A09Eu) && WAIT(sim_value(UA101_F_VOLUME, 1, 0) == 0x16A09Eu, 3.0) &&
              volume_reaches(a, 1, 0, 0x200000u) && WAIT(sim_value(UA101_F_VOLUME, 1, 0) == 0x200000u, 3.0) &&
              counts().ctl_changes == changes + 6 && atomic_load(&P.wire_volume) == volume + 6,
          "three values version 7 refused (0x1F0000, 0x00000F, 0x16A09E) reach the unit, and 0 dB again");
    CHECK(volume_reaches(a, 1, 0, 0x200001u) && WAIT(sim_value(UA101_F_VOLUME, 1, 0) == 0x200001u, 3.0) &&
              volume_reaches(a, 1, 0, T_VOLUME_TOP) && WAIT(sim_value(UA101_F_VOLUME, 1, 0) == T_VOLUME_TOP, 3.0) &&
              volume_reaches(a, 1, 0, 0x200000u) && WAIT(sim_value(UA101_F_VOLUME, 1, 0) == 0x200000u, 3.0) &&
              counts().ctl_changes == changes + 9 && atomic_load(&P.wire_volume) == volume + 9,
          "since version 19 above 0 dB too: one step above (0x200001) and the top of the range "
          "(0x7FFFFF, about +12 dB) reach the unit, and 0 dB again");
    volume += 7;
    CHECK(!volume_reaches(a, 1, 0, 0x800000u) && !volume_reaches(a, 1, 0, 0x800010u) &&
              !volume_reaches(a, 1, 0, 0x900000u) && !volume_reaches(a, 1, 0, 0xFFFFFFu),
          "INPUT 1 past the top of the range: refused");
    CHECK(!volume_reaches(a, 1, 10, 0x100001u) && !volume_reaches(a, 2, 10, 0x100001u) &&
              !volume_reaches(a, 0, 2, 0x100001u) && !volume_reaches(a, 3, 0, 0),
          "an INPUT 11, an OUTPUT 11, a third MONITOR OUT and a fourth group, none of which "
          "the mixer has: refused");
    CHECK(sim_value(UA101_F_VOLUME, 1, 0) == 0x200000u && sim_value(UA101_F_VOLUME, 1, 1) == 0x200000u &&
              atomic_load(&P.wire_volume) == volume + 2,
          "nothing of that reached the cable");

    /* The app's engine: the first test's value by itself, read back. No pause of the saving
       goes with a single change. */
    reads = atomic_load(&P.wire_reads);
    on_app(^{
      panel_set_min_gap(a->engine, 60000); /* as the app does for the UA-101 */
      panel_allow_writes(a->engine, 1);
      blocks = panel_blocks_received(a->engine);
      disagreements = panel_disagreements(a->engine);
      panel_unit_block(a->engine, bp);
      result = panel_set_volume(a->engine, 1, 0, 0x100001u, now_us());
    });
    CHECK(result == PANEL_OK, "the engine, allowed to write, sets INPUT 1 to 0x100001 (%d)", result);
    CHECK(WAIT(sim_value(UA101_F_VOLUME, 1, 0) == 0x100001u && APP(panel_blocks_received(a->engine) > blocks) &&
                   !APP(panel_busy(a->engine)), 3.0),
          "the unit took it and the settings were read back");
    CHECK(APP(panel_volume(a->engine, 1, 0)) == 0x100001 && (uint64_t)APP(panel_disagreements(a->engine)) == disagreements,
          "the engine shows it and found no disagreement");
    on_app(^{
      panel_unit_block(a->engine, ap);
    });
    for (i = 0; i < 256; i++)
        differ += before[i] != after[i];
    CHECK(differ == 2 && before[128] == 2 && after[128] == 1 && after[133] == 1 &&
              after[129] + after[130] + after[131] + after[132] == 0,
          "in the block read back, d[128] to d[133] are 01 00 00 00 00 01, and nothing else moved");
    CHECK(atomic_load(&P.wire_volume) == volume + 3 && atomic_load(&P.wire_reads) == reads + 1 &&
              atomic_load(&P.wire_saving) == saving,
          "on the cable: one volume and one read; no pause or resume of saving");

    /* A drag: forty values in 0.4 s. Paused, spaced out, resumed, read once. */
    reads = atomic_load(&P.wire_reads);
    refused = atomic_load(&a->refused);
    on_app(^{
      panel_drag_begin(a->engine, now_us());
    });
    for (i = 0; i < 40; i++) {
        on_app(^{
          panel_set_volume(a->engine, 1, 0, 0x10FF00u - 0x100u * (uint32_t)i, now_us());
        });
        usleep(10000);
    }
    on_app(^{
      blocks = panel_blocks_received(a->engine);
      panel_drag_end(a->engine, now_us());
    });
    CHECK(WAIT(sim_value(UA101_F_VOLUME, 1, 0) == 0x10FF00u - 0x100u * 39u && !sim_paused() &&
                   APP(panel_blocks_received(a->engine) > blocks) && !APP(panel_busy(a->engine)), 5.0),
          "a drag of forty values: the unit has the last, saves again, and was read back");
    CHECK(atomic_load(&P.wire_saving) == saving + 2 && atomic_load(&a->refused) == refused &&
              atomic_load(&P.wire_volume) - volume - 3 >= 2 && atomic_load(&P.wire_volume) - volume - 3 <= 12 &&
              atomic_load(&P.wire_reads) == reads + 1,
          "on the cable: a pause, %d volumes for the forty asked (never more than the helper's 20 a "
          "second, none refused), a resume, one read",
          atomic_load(&P.wire_volume) - volume - 3);
    CHECK((uint64_t)APP(panel_disagreements(a->engine)) == disagreements, "no disagreement");

    /* Back to 0 dB. */
    on_app(^{
      blocks = panel_blocks_received(a->engine);
      result = panel_set_volume(a->engine, 1, 0, 0x200000u, now_us());
    });
    CHECK(result == PANEL_OK && WAIT(sim_value(UA101_F_VOLUME, 1, 0) == 0x200000u &&
                                         APP(panel_blocks_received(a->engine) > blocks) && !APP(panel_busy(a->engine)), 3.0),
          "and back to 0 dB, read back");
    on_app(^{
      panel_allow_writes(a->engine, 0);
      panel_set_min_gap(a->engine, 5000);
    });
}

/* Sends one pan by hand and says whether the helper passed it on. */
static int pan_reaches(app *a, unsigned group, unsigned channel, uint32_t value)
{
    uint8_t bytes[16];
    uint64_t changes = counts().ctl_changes;
    int refused = atomic_load(&a->refused);

    hand_pan(bytes, 3 + group, channel, 0x03, value);
    send_raw(a, bytes, 16);
    WAIT(counts().ctl_changes > changes || atomic_load(&a->refused) > refused, 3.0);
    return counts().ctl_changes > changes;
}

/* The lines of the helper's log that say a pan was passed on, and whether every one of them,
   and every other stamped line, starts "YYYY-MM-DD HH:MM:SS.mmm ua101d: ". The spacing of two
   lines can then be read to the millisecond: `last_gap_ms` gets the time between the last two
   pan lines. */
static int log_pan_lines(int *stamps_ok, long *last_gap_ms)
{
    char line[512];
    FILE *f = fopen(g_log_path, "r");
    int n = 0;
    long at = -1, before = -1;

    *stamps_ok = 1;
    *last_gap_ms = -1;
    if (f == NULL)
        return -1;
    while (fgets(line, sizeof(line), f) != NULL) {
        int h, m, sec, ms;
        const char *name = strstr(line, " ua101d: ");

        if (name == NULL)
            continue; /* the pretend parts' own lines carry no stamp */
        if (name - line != 23 || sscanf(line + 11, "%2d:%2d:%2d.%3d", &h, &m, &sec, &ms) != 4 ||
            line[4] != '-' || line[7] != '-' || line[10] != ' ' || line[19] != '.')
            *stamps_ok = 0;
        else if (strstr(line, "passed on to the unit") != NULL && strstr(line, ": pan, INPUT 1 to 0x") != NULL) {
            n++;
            before = at;
            at = ((h * 60L + m) * 60L + sec) * 1000L + ms;
        }
    }
    fclose(f);
    if (before >= 0)
        *last_gap_ms = (at - before + 86400000L) % 86400000L;
    return n;
}

/* Link version 10: INPUT 1's pan at any value from 0x0000 to 0x8000, and no
   other channel's. */
static void test_pan(app *a)
{
    int pan = atomic_load(&P.wire_pan), saving = atomic_load(&P.wire_saving);
    int volume = atomic_load(&P.wire_volume), patch = atomic_load(&P.wire_patch);
    int reads, refused, i, stamps_ok, lines;
    long gap_ms;
    uint64_t changes = counts().ctl_changes;
    __block int result;
    __block uint64_t blocks, disagreements;
    uint8_t before[256], after[256];
    uint8_t *bp = before, *ap = after;
    unsigned differ = 0;

    fresh_second();
    CHECK(sim_value(UA101_F_PAN, 1, 0) == 0 && sim_value(UA101_F_PAN, 1, 1) == 0x8000u,
          "the pretend unit starts as the real one reads: INPUT 1's pan 0x0000, INPUT 2's 0x8000");
    CHECK(sim_value(UA101_F_PAN, 1, 0) == 0, "the pretend unit still has 0x0000 there");
    CHECK(pan_reaches(a, 1, 0, 0x4000u) && WAIT(sim_value(UA101_F_PAN, 1, 0) == 0x4000u, 3.0),
          "INPUT 1's pan 0x4000 reaches the unit");
    usleep(30000);
    CHECK(pan_reaches(a, 1, 0, 0) && WAIT(sim_value(UA101_F_PAN, 1, 0) == 0, 3.0), "and back to 0x0000");
    CHECK(counts().ctl_changes == changes + 2 && atomic_load(&P.wire_pan) == pan + 2,
          "the helper counted two changes; two pan messages on the cable");
    lines = log_pan_lines(&stamps_ok, &gap_ms);
    CHECK(lines == 2 && stamps_ok, "the helper's log has a line for each, and every stamped line "
                                   "of the log carries milliseconds (%d lines)", lines);
    CHECK(gap_ms >= 30 && gap_ms < 3000,
          "the time between the two can be read from the log's stamps: %ld ms (they were sent at "
          "least 30 ms apart)", gap_ms);
    CHECK(pan_reaches(a, 1, 0, 0x8000u) && pan_reaches(a, 1, 0, 0x7FFFu) && pan_reaches(a, 1, 0, 0x0009u) &&
              pan_reaches(a, 1, 0, 0x4ABCu) && WAIT(sim_value(UA101_F_PAN, 1, 0) == 0x4ABCu, 3.0) &&
              pan_reaches(a, 1, 0, 0) && WAIT(sim_value(UA101_F_PAN, 1, 0) == 0, 3.0) &&
              counts().ctl_changes == changes + 7 && atomic_load(&P.wire_pan) == pan + 7,
          "the far end (0x8000), 0x7FFF, 0x0009 and 0x4ABC reach the unit, and 0x0000 again");
    CHECK(!pan_reaches(a, 1, 0, 0x8001u) && !pan_reaches(a, 1, 0, 0xFFFFu) && !pan_reaches(a, 1, 0, 0x9000u) &&
              !pan_reaches(a, 1, 0, 0x8800u) && !pan_reaches(a, 1, 0, 0x8008u) && !pan_reaches(a, 1, 0, 0xF000u),
          "INPUT 1's pan above 0x8000: refused");
    CHECK(!pan_reaches(a, 1, 10, 0x4000u) && !pan_reaches(a, 2, 10, 0x4000u) && !pan_reaches(a, 0, 0, 0x4000u) &&
              !pan_reaches(a, 0, 1, 0) && !pan_reaches(a, 3, 0, 0),
          "an INPUT 11, an OUTPUT 11, MONITOR OUT L's and R's addresses (they have no pan) and a "
          "fourth group: refused");
    CHECK(sim_value(UA101_F_PAN, 1, 0) == 0 && sim_value(UA101_F_PAN, 1, 1) == 0x8000u &&
              sim_value(UA101_F_PAN, 2, 0) == 0 && atomic_load(&P.wire_pan) == pan + 7,
          "nothing of that reached the cable");

    /* The app's engine: the middle by itself, read back. No pause of the saving goes with a
       single change. */
    reads = atomic_load(&P.wire_reads);
    on_app(^{
      panel_set_min_gap(a->engine, 60000); /* as the app does for the UA-101 */
      panel_allow_writes(a->engine, 1);
      blocks = panel_blocks_received(a->engine);
      disagreements = panel_disagreements(a->engine);
      panel_unit_block(a->engine, bp);
      result = panel_set_pan(a->engine, 1, 0, 0x4000u, now_us());
    });
    CHECK(result == PANEL_OK, "the engine, allowed to write, sets INPUT 1's pan to 0x4000 (%d)", result);
    CHECK(WAIT(sim_value(UA101_F_PAN, 1, 0) == 0x4000u && APP(panel_blocks_received(a->engine) > blocks) &&
                   !APP(panel_busy(a->engine)), 3.0),
          "the unit took it and the settings were read back");
    CHECK(APP(panel_pan(a->engine, 1, 0)) == 0x4000 && (uint64_t)APP(panel_disagreements(a->engine)) == disagreements,
          "the engine shows it and found no disagreement");
    on_app(^{
      panel_unit_block(a->engine, ap);
    });
    for (i = 0; i < 256; i++)
        differ += before[i] != after[i];
    CHECK(differ == 1 && before[36] == 0 && after[36] == 4 && after[37] + after[38] + after[39] == 0,
          "in the block read back, d[36] to d[39] are 04 00 00 00, and nothing else moved");
    CHECK(atomic_load(&P.wire_pan) == pan + 8 && atomic_load(&P.wire_reads) == reads + 1 &&
              atomic_load(&P.wire_saving) == saving,
          "on the cable: one pan and one read; no pause or resume of saving");

    /* A drag of the knob: forty values in 0.4 s, none of them moved to a step. Paused, spaced
       out, resumed, read once. */
    reads = atomic_load(&P.wire_reads);
    refused = atomic_load(&a->refused);
    on_app(^{
      panel_drag_begin(a->engine, now_us());
    });
    for (i = 0; i < 40; i++) {
        on_app(^{
          panel_set_pan(a->engine, 1, 0, 0x4000u + 0x9Bu * (uint32_t)i, now_us());
        });
        usleep(10000);
    }
    on_app(^{
      blocks = panel_blocks_received(a->engine);
      panel_drag_end(a->engine, now_us());
    });
    CHECK(WAIT(sim_value(UA101_F_PAN, 1, 0) == 0x4000u + 0x9Bu * 39u && !sim_paused() &&
                   APP(panel_blocks_received(a->engine) > blocks) && !APP(panel_busy(a->engine)), 5.0),
          "a drag of forty values: the unit has the last, saves again, and was read back");
    CHECK(atomic_load(&P.wire_saving) == saving + 2 && atomic_load(&a->refused) == refused &&
              atomic_load(&P.wire_pan) - pan - 8 >= 2 && atomic_load(&P.wire_pan) - pan - 8 <= 12 &&
              atomic_load(&P.wire_reads) == reads + 1,
          "on the cable: a pause, %d pans for the forty asked (never more than the helper's 20 a "
          "second, none refused), a resume, one read",
          atomic_load(&P.wire_pan) - pan - 8);
    CHECK((uint64_t)APP(panel_disagreements(a->engine)) == disagreements, "no disagreement");
    lines = log_pan_lines(&stamps_ok, &gap_ms);
    CHECK(stamps_ok && gap_ms >= 50 && gap_ms <= 1000,
          "the helper's log shows the spacing of the drag's last two pans: %ld ms (the app sends "
          "60 ms apart)", gap_ms);

    /* The engine on INPUT 2's pan, which version 12 refused: it reaches the unit, and back. */
    {
        uint64_t helper_refused = counts().ctl_refused;

        on_app(^{
          blocks = panel_blocks_received(a->engine);
          result = panel_set_pan(a->engine, 1, 1, 0x4000u, now_us());
        });
        CHECK(result == PANEL_OK &&
                  WAIT(sim_value(UA101_F_PAN, 1, 1) == 0x4000u && APP(panel_blocks_received(a->engine) > blocks) &&
                           !APP(panel_busy(a->engine)), 3.0) &&
                  APP(panel_pan(a->engine, 1, 1)) == 0x4000,
              "INPUT 2's pan through the engine: the unit takes the middle, and it is read back");
        on_app(^{
          blocks = panel_blocks_received(a->engine);
          result = panel_set_pan(a->engine, 1, 1, 0x8000u, now_us());
        });
        CHECK(result == PANEL_OK &&
                  WAIT(sim_value(UA101_F_PAN, 1, 1) == 0x8000u && APP(panel_blocks_received(a->engine) > blocks) &&
                           !APP(panel_busy(a->engine)), 3.0) &&
                  sim_value(UA101_F_PAN, 1, 0) == 0x4000u + 0x9Bu * 39u &&
                  counts().ctl_refused == helper_refused &&
                  (uint64_t)APP(panel_disagreements(a->engine)) == disagreements,
              "and back to 0x8000; INPUT 1's pan didn't move, nothing was refused, no disagreement");
    }

    /* Back to 0x0000. */
    on_app(^{
      blocks = panel_blocks_received(a->engine);
      result = panel_set_pan(a->engine, 1, 0, 0, now_us());
    });
    CHECK(result == PANEL_OK && WAIT(sim_value(UA101_F_PAN, 1, 0) == 0 &&
                                         APP(panel_blocks_received(a->engine) > blocks) && !APP(panel_busy(a->engine)), 3.0),
          "and back to 0x0000, read back");
    CHECK(atomic_load(&P.wire_volume) == volume && atomic_load(&P.wire_patch) == patch,
          "no volume and no patch bay message went with any of it");
    on_app(^{
      panel_allow_writes(a->engine, 0);
      panel_set_min_gap(a->engine, 5000);
    });
}

static int on_reaches(app *a, unsigned group, unsigned channel, unsigned value)
{
    uint8_t bytes[13];
    uint64_t changes = counts().ctl_changes;
    int refused = atomic_load(&a->refused);

    hand_on(bytes, 3 + group, channel, 0x02, value);
    send_raw(a, bytes, 13);
    WAIT(counts().ctl_changes > changes || atomic_load(&a->refused) > refused, 3.0);
    return counts().ctl_changes > changes;
}

/* Link version 11: INPUT 1's on/off, 0 or 1, and no other channel's. */
static void test_on(app *a)
{
    int on = atomic_load(&P.wire_on), saving = atomic_load(&P.wire_saving);
    int volume = atomic_load(&P.wire_volume), pan = atomic_load(&P.wire_pan);
    int patch = atomic_load(&P.wire_patch), reads, i;
    uint64_t changes = counts().ctl_changes;
    __block int result;
    __block uint64_t blocks, disagreements;
    uint8_t before[256], after[256];
    uint8_t *bp = before, *ap = after;
    unsigned differ = 0;

    fresh_second();
    CHECK(sim_value(UA101_F_ON, 1, 0) == 0 && sim_value(UA101_F_ON, 1, 1) == 0,
          "the pretend unit starts as the real one reads: every on/off bit 0");
    CHECK(sim_value(UA101_F_ON, 1, 0) == 0, "the pretend unit still has 0 there");
    {
        int named1 = log_lines_with("on/off, INPUT 1 to bit 1", "F0 41 10 00 69 12 00 04 00 02 01 79 F7");
        int named0 = log_lines_with("on/off, INPUT 1 to bit 0", "F0 41 10 00 69 12 00 04 00 02 00 7A F7");
        int as_saving = log_lines_with("saving (F0 41 10 00 69 12 00 04 00 02", NULL);

        CHECK(on_reaches(a, 1, 0, 1) && WAIT(sim_value(UA101_F_ON, 1, 0) == 1, 3.0),
              "INPUT 1's bit 1 reaches the unit");
        CHECK(on_reaches(a, 1, 0, 0) && WAIT(sim_value(UA101_F_ON, 1, 0) == 0, 3.0), "and back to 0");
        CHECK(log_lines_with("on/off, INPUT 1 to bit 1", "F0 41 10 00 69 12 00 04 00 02 01 79 F7") == named1 + 1 &&
                  log_lines_with("on/off, INPUT 1 to bit 0", "F0 41 10 00 69 12 00 04 00 02 00 7A F7") == named0 + 1 &&
                  log_lines_with("saving (F0 41 10 00 69 12 00 04 00 02", NULL) == as_saving && as_saving == 0,
              "the helper's log names each as an on/off of INPUT 1, never as the saving (on the "
              "unit, 2026-11-13, they were logged as \"resume saving\" and \"pause saving\")");
    }
    CHECK(counts().ctl_changes == changes + 2 && atomic_load(&P.wire_on) == on + 2,
          "the helper counted two changes; two on/off messages on the cable");
    CHECK(!on_reaches(a, 1, 0, 2) && !on_reaches(a, 1, 0, 0x7F),
          "INPUT 1 with the byte 2 or 0x7F: refused");
    CHECK(!on_reaches(a, 1, 10, 1) && !on_reaches(a, 1, 15, 1) && !on_reaches(a, 2, 10, 1) &&
              !on_reaches(a, 0, 2, 1) && !on_reaches(a, 3, 0, 1),
          "an INPUT 11 and 16, an OUTPUT 11, a third MONITOR OUT and a fourth group: refused");
    CHECK(sim_value(UA101_F_ON, 1, 1) == 0 && sim_value(UA101_F_ON, 1, 4) == 0 &&
              sim_value(UA101_F_ON, 2, 0) == 0 && sim_value(UA101_F_ON, 0, 0) == 0 &&
              atomic_load(&P.wire_on) == on + 2,
          "nothing of that reached the cable");

    /* The app's engine: INPUT 1 "off" (bit 1, as the engine reads the bit; not proven), by
       itself, read back. No pause of the saving goes with it. */
    reads = atomic_load(&P.wire_reads);
    on_app(^{
      panel_set_min_gap(a->engine, 60000);
      panel_allow_writes(a->engine, 1);
      blocks = panel_blocks_received(a->engine);
      disagreements = panel_disagreements(a->engine);
      panel_unit_block(a->engine, bp);
      result = panel_set_on_bit(a->engine, 1, 0, 1, now_us());
    });
    CHECK(result == PANEL_OK, "the engine, allowed to write, sets INPUT 1's bit to 1 (%d)", result);
    CHECK(WAIT(sim_value(UA101_F_ON, 1, 0) == 1 && APP(panel_blocks_received(a->engine) > blocks) &&
                   !APP(panel_busy(a->engine)), 3.0),
          "the unit took it and the settings were read back");
    CHECK(APP(panel_on_bit(a->engine, 1, 0)) == 1 && APP(panel_monitor_on(a->engine, 1, 0)) == 0 &&
              (uint64_t)APP(panel_disagreements(a->engine)) == disagreements,
          "the engine shows the bit 1, which it reads as \"off\", and found no disagreement");
    on_app(^{
      panel_unit_block(a->engine, ap);
    });
    for (i = 0; i < 256; i++)
        differ += before[i] != after[i];
    CHECK(differ == 1 && before[30] == 0 && after[30] == 1,
          "in the block read back, d[30] went 00 to 01 (bit 0), and nothing else moved");
    CHECK(atomic_load(&P.wire_on) == on + 3 && atomic_load(&P.wire_reads) == reads + 1 &&
              atomic_load(&P.wire_saving) == saving,
          "on the cable: one on/off and one read; no pause or resume of saving");

    /* The engine on INPUT 2's on/off, which version 12 refused: it reaches the unit, and back. */
    {
        uint64_t helper_refused = counts().ctl_refused;

        on_app(^{
          blocks = panel_blocks_received(a->engine);
          result = panel_set_on_bit(a->engine, 1, 1, 1, now_us());
        });
        CHECK(result == PANEL_OK &&
                  WAIT(sim_value(UA101_F_ON, 1, 1) == 1 && APP(panel_blocks_received(a->engine) > blocks) &&
                           !APP(panel_busy(a->engine)), 3.0) &&
                  APP(panel_on_bit(a->engine, 1, 1)) == 1,
              "INPUT 2's on/off through the engine: the unit takes the bit 1, and it is read back");
        on_app(^{
          blocks = panel_blocks_received(a->engine);
          result = panel_set_on_bit(a->engine, 1, 1, 0, now_us());
        });
        CHECK(result == PANEL_OK &&
                  WAIT(sim_value(UA101_F_ON, 1, 1) == 0 && APP(panel_blocks_received(a->engine) > blocks) &&
                           !APP(panel_busy(a->engine)), 3.0) &&
                  sim_value(UA101_F_ON, 1, 0) == 1 && counts().ctl_refused == helper_refused &&
                  (uint64_t)APP(panel_disagreements(a->engine)) == disagreements,
              "and back to 0; INPUT 1's bit didn't move, nothing was refused, no disagreement");
    }

    /* Back to 0. */
    on_app(^{
      blocks = panel_blocks_received(a->engine);
      result = panel_set_on_bit(a->engine, 1, 0, 0, now_us());
    });
    CHECK(result == PANEL_OK && WAIT(sim_value(UA101_F_ON, 1, 0) == 0 &&
                                         APP(panel_blocks_received(a->engine) > blocks) && !APP(panel_busy(a->engine)), 3.0),
          "and back to 0, read back");
    CHECK(atomic_load(&P.wire_volume) == volume && atomic_load(&P.wire_pan) == pan &&
              atomic_load(&P.wire_patch) == patch,
          "no volume, pan or patch bay message went with any of it");
    on_app(^{
      panel_allow_writes(a->engine, 0);
      panel_set_min_gap(a->engine, 5000);
    });
}

/* The helper passes on 20 changes in a second at most: the loops below stay under that. */
static void pace(void)
{
    usleep(60000);
}

/* Forces a pair's link in the pretend unit, as the real unit has OUTPUT 1-2 to 9-10 and
   MONITOR OUT linked, and has the engine read the settings again. No message does this here,
   so that test_all_channels counts only volumes, pans and on/offs on the cable (the link's
   own message is tried in test_link_solo). */
static void sim_set_link(app *a, unsigned group, unsigned pair, unsigned on)
{
    __block uint64_t blocks;
    ua101_field f = {UA101_F_LINK, (uint8_t)group, (uint8_t)pair};

    on_service_queue(^{
      ua101_block_put(P.sim.block, f, on);
    });
    on_app(^{
      blocks = panel_blocks_received(a->engine);
      panel_refresh(a->engine, now_us());
    });
    CHECK(WAIT(APP(panel_blocks_received(a->engine) > blocks) && !APP(panel_busy(a->engine)), 3.0) &&
              APP(panel_link(a->engine, group, pair)) == (int)on,
          "the pretend unit's link of group %u pair %u is %u, and the engine has read it", group, pair, on);
}

/* Link version 13: the volume (never above 0 dB), the pan and the on/off
   of every channel the test's list names, and of no other; through the engine a linked pair
   moves together and an unlinked one doesn't. */
static void test_all_channels(app *a)
{
    static const char *const group_name[4] = {"MONITOR OUT", "INPUT", "OUTPUT", "a fourth group"};
    unsigned group, channel, wrong = 0, passed = 0;
    int volume, pan, on;
    uint64_t changes, refused_before;
    __block int result;
    __block uint64_t blocks, disagreements;

    fresh_second();
    /* Each passed channel takes a volume, never one above 0 dB, a pan, never one past
       the end, and the bit 1 and 0, never the byte 2; every other address takes nothing. Each
       is put back as it was. */
    volume = atomic_load(&P.wire_volume);
    pan = atomic_load(&P.wire_pan);
    on = atomic_load(&P.wire_on);
    changes = counts().ctl_changes;
    for (group = 0; group < 4; group++)
        for (channel = 0; channel < 12; channel++) {
            int exists = group < 3 && channel < (group == 0 ? 2u : 10u);
            int want_volume = group < 3 && t_volume(group, channel);
            int want_pan = group < 3 && t_pan(group, channel);
            int want_on = group < 3 && t_on(group, channel);
            unsigned before = wrong;

            if (volume_reaches(a, group, channel, 0x100001u) != want_volume)
                wrong++;
            else if (want_volume) {
                pace();
                wrong += !WAIT(sim_value(UA101_F_VOLUME, group, channel) == 0x100001u, 3.0);
                wrong += (unsigned)volume_reaches(a, group, channel, 0x800000u);
                wrong += (unsigned)volume_reaches(a, group, channel, 0xFFFFFFu);
                wrong += !volume_reaches(a, group, channel, 0x200000u);
                pace();
                wrong += !WAIT(sim_value(UA101_F_VOLUME, group, channel) == 0x200000u, 3.0);
                passed++;
            } else if (exists) {
                wrong += sim_value(UA101_F_VOLUME, group, channel) != 0x200000u;
            }
            if (pan_reaches(a, group, channel, 0x4000u) != want_pan)
                wrong++;
            else if (want_pan) {
                uint32_t was = (channel & 1u) ? 0x8000u : 0x0000u; /* odd channels left, even right */

                pace();
                wrong += !WAIT(sim_value(UA101_F_PAN, group, channel) == 0x4000u, 3.0);
                wrong += (unsigned)pan_reaches(a, group, channel, 0x8001u);
                wrong += (unsigned)pan_reaches(a, group, channel, 0xFFFFu);
                wrong += !pan_reaches(a, group, channel, was);
                pace();
                wrong += !WAIT(sim_value(UA101_F_PAN, group, channel) == was, 3.0);
                passed++;
            } else if (exists && group != 0) {
                wrong += sim_value(UA101_F_PAN, group, channel) != ((channel & 1u) ? 0x8000u : 0x0000u);
            }
            if (on_reaches(a, group, channel, 1) != want_on)
                wrong++;
            else if (want_on) {
                pace();
                wrong += !WAIT(sim_value(UA101_F_ON, group, channel) == 1, 3.0);
                wrong += (unsigned)on_reaches(a, group, channel, 2);
                wrong += !on_reaches(a, group, channel, 0);
                pace();
                wrong += !WAIT(sim_value(UA101_F_ON, group, channel) == 0, 3.0);
                passed++;
            } else if (exists) {
                wrong += sim_value(UA101_F_ON, group, channel) != 0;
            }
            if (wrong != before)
                printf("   note  %s %u: %u wrong\n", group_name[group], channel + 1, wrong - before);
        }
    CHECK(wrong == 0 && passed == T_VOLUMES + T_PANS + T_ONS,
          "over 48 channel addresses: the %u volumes, pans and on/offs of the passed "
          "channels reach the unit and go back, none past the top of the range, none past a pan's end, no byte "
          "2; nothing else does (%u wrong)", passed, wrong);
    CHECK(atomic_load(&P.wire_volume) == volume + 2 * T_VOLUMES && atomic_load(&P.wire_pan) == pan + 2 * T_PANS &&
              atomic_load(&P.wire_on) == on + 2 * T_ONS &&
              counts().ctl_changes == changes + 2u * (T_VOLUMES + T_PANS + T_ONS),
          "on the cable: two volumes, two pans and two on/offs for each passed channel, and the "
          "helper counted them");
    {
        int named = log_lines_with("passed on to the unit", "MONITOR OUT 1") +
                    log_lines_with("passed on to the unit", "MONITOR OUT 2");

        CHECK(named == 0 && (log_lines_with("volume, MONITOR OUT L to 0x100001", NULL) == 1) == t_volume(0, 0) &&
                  (log_lines_with("on/off, MONITOR OUT R to bit 1", NULL) == 1) == t_on(0, 1) &&
                  (log_lines_with("pan, OUTPUT 10 to 0x4000", NULL) == 1) == t_pan(2, 9) &&
                  (log_lines_with("on/off, INPUT 9 to bit 1", NULL) == 1) == t_on(1, 8),
              "the helper's log names each channel as the user knows it (MONITOR OUT L and R, "
              "INPUT 9, OUTPUT 10), where it is passed");
    }

    /* The app's engine. An unlinked pair: INPUT 3 moves alone. */
    on_app(^{
      panel_set_min_gap(a->engine, 60000);
      panel_allow_writes(a->engine, 1);
      blocks = panel_blocks_received(a->engine);
      disagreements = panel_disagreements(a->engine);
      result = panel_set_volume(a->engine, 1, 2, 0x16A09Eu, now_us());
    });
    refused_before = counts().ctl_refused;
    if (t_volume(1, 2)) {
        CHECK(result == PANEL_OK &&
                  WAIT(sim_value(UA101_F_VOLUME, 1, 2) == 0x16A09Eu && APP(panel_blocks_received(a->engine) > blocks) &&
                           !APP(panel_busy(a->engine)), 3.0) &&
                  sim_value(UA101_F_VOLUME, 1, 3) == 0x200000u && APP(panel_volume(a->engine, 1, 2)) == 0x16A09E,
              "the engine sets INPUT 3 to -3 dB: the unit takes it, INPUT 4 stays at 0 dB (the pair isn't linked)");
        on_app(^{
          blocks = panel_blocks_received(a->engine);
          result = panel_set_volume(a->engine, 1, 2, 0x200000u, now_us());
        });
        CHECK(result == PANEL_OK &&
                  WAIT(sim_value(UA101_F_VOLUME, 1, 2) == 0x200000u && APP(panel_blocks_received(a->engine) > blocks) &&
                           !APP(panel_busy(a->engine)), 3.0),
              "and back to 0 dB");
    }
    /* A linked pair, as the real unit has the computer's channels and MONITOR OUT: both
       channels' messages must pass, or the helper refuses the partner and the app turns
       read-only. A volume and an on/off go to both; a pan to the one. */
    for (group = 0; group < 3; group += 2) {
        unsigned first = group == 0 ? 0u : 4u; /* MONITOR OUT L-R; OUTPUT 5-6 */
        unsigned pair = first / 2u;

        if (!t_volume(group, first) || !t_volume(group, first + 1) || !t_on(group, first) || !t_on(group, first + 1))
            continue;
        sim_set_link(a, group, pair, 1);
        on_app(^{
          blocks = panel_blocks_received(a->engine);
          result = panel_set_volume(a->engine, group, first, 0x16A09Eu, now_us());
        });
        CHECK(result == PANEL_OK &&
                  WAIT(sim_value(UA101_F_VOLUME, group, first) == 0x16A09Eu &&
                           sim_value(UA101_F_VOLUME, group, first + 1) == 0x16A09Eu &&
                           APP(panel_blocks_received(a->engine) > blocks) && !APP(panel_busy(a->engine)), 3.0),
              "%s %u-%u linked: the engine sets one to -3 dB and the unit takes it for both",
              group_name[group], first + 1, first + 2);
        on_app(^{
          blocks = panel_blocks_received(a->engine);
          result = panel_set_on_bit(a->engine, group, first + 1, 1, now_us());
        });
        CHECK(result == PANEL_OK &&
                  WAIT(sim_value(UA101_F_ON, group, first) == 1 && sim_value(UA101_F_ON, group, first + 1) == 1 &&
                           APP(panel_blocks_received(a->engine) > blocks) && !APP(panel_busy(a->engine)), 3.0),
              "its on/off bit set to 1 on the other: the unit takes it for both");
        if (t_pan(group, first)) {
            on_app(^{
              blocks = panel_blocks_received(a->engine);
              result = panel_set_pan(a->engine, group, first, 0x4000u, now_us());
            });
            CHECK(result == PANEL_OK &&
                      WAIT(sim_value(UA101_F_PAN, group, first) == 0x4000u &&
                               APP(panel_blocks_received(a->engine) > blocks) && !APP(panel_busy(a->engine)), 3.0) &&
                      sim_value(UA101_F_PAN, group, first + 1) == 0x8000u,
                  "a pan goes to the one channel only: its partner stays hard right");
            on_app(^{
              blocks = panel_blocks_received(a->engine);
              result = panel_set_pan(a->engine, group, first, 0, now_us());
            });
            CHECK(result == PANEL_OK &&
                      WAIT(sim_value(UA101_F_PAN, group, first) == 0 &&
                               APP(panel_blocks_received(a->engine) > blocks) && !APP(panel_busy(a->engine)), 3.0),
                  "and back hard left");
        }
        on_app(^{
          blocks = panel_blocks_received(a->engine);
          panel_set_on_bit(a->engine, group, first, 0, now_us());
          result = panel_set_volume(a->engine, group, first + 1, 0x200000u, now_us());
        });
        CHECK(result == PANEL_OK &&
                  WAIT(sim_value(UA101_F_VOLUME, group, first) == 0x200000u &&
                           sim_value(UA101_F_VOLUME, group, first + 1) == 0x200000u &&
                           sim_value(UA101_F_ON, group, first) == 0 && sim_value(UA101_F_ON, group, first + 1) == 0 &&
                           APP(panel_blocks_received(a->engine) > blocks) && !APP(panel_busy(a->engine)), 5.0),
              "both back to 0 dB and the bit 0");
        CHECK(APP(panel_writes_allowed(a->engine)), "the engine may still write: nothing of the pair was refused");
        sim_set_link(a, group, pair, 0);
    }
    CHECK(counts().ctl_refused == refused_before &&
              (uint64_t)APP(panel_disagreements(a->engine)) == disagreements,
          "in all of the engine's changes the helper refused nothing and the engine found no disagreement");
    on_app(^{
      panel_allow_writes(a->engine, 0);
      panel_set_min_gap(a->engine, 5000);
    });
}

/* Sends one 13-byte change by hand (any address, one byte) and says whether the helper passed
   it on. */
static int byte_reaches(app *a, unsigned a1, unsigned a2, unsigned a3, unsigned value)
{
    uint8_t bytes[13];
    uint64_t changes = counts().ctl_changes;
    int refused = atomic_load(&a->refused);

    hand_on(bytes, a1, a2, a3, value);
    send_raw(a, bytes, 13);
    WAIT(counts().ctl_changes > changes || atomic_load(&a->refused) > refused, 3.0);
    return counts().ctl_changes > changes;
}

/* Has the engine do one thing and waits until the unit's settings were read back after it. */
static int engine_does(app *a, int (^change)(void))
{
    __block uint64_t blocks;
    __block int result;

    on_app(^{
      blocks = panel_blocks_received(a->engine);
      result = change();
    });
    return result == PANEL_OK &&
           WAIT(APP(panel_blocks_received(a->engine) > blocks) && !APP(panel_busy(a->engine)), 3.0);
}

/* Link version 14: the link (0 or 1) of every pair the test's list names
   and the solo (0 to 10) of every group it names, and of no other. Through the engine a link
   is one message that moves no volume; the pair then moves together; a solo is one number a
   group, also for a linked pair. */
static void test_link_solo(app *a)
{
    static const char *const group_name[4] = {"MONITOR OUT", "INPUT", "OUTPUT", "a fourth group"};
    unsigned group, pair, value, i, differ, wrong = 0, links = 0, solos = 0;
    int link, solo, volume, pan, on, patch, saving, reads;
    uint64_t changes, refused_before;
    __block uint64_t disagreements;
    uint8_t before[256], after[256];
    uint8_t *bp = before, *ap = after;

    fresh_second();
    /* Each passed pair takes the other value and its own again, never the byte 2 and
       never at the pair's second channel; each passed group takes 1 to 10 and then 0, never
       11 or 0x7F and never with a channel byte; every other address takes nothing. */
    link = atomic_load(&P.wire_link);
    solo = atomic_load(&P.wire_solo);
    volume = atomic_load(&P.wire_volume);
    pan = atomic_load(&P.wire_pan);
    on = atomic_load(&P.wire_on);
    patch = atomic_load(&P.wire_patch);
    saving = atomic_load(&P.wire_saving);
    changes = counts().ctl_changes;
    for (group = 0; group < 4; group++)
        for (pair = 0; pair < 6; pair++) {
            int exists = group < 3 && pair < (group == 0 ? 1u : 5u);
            int want = group < 3 && t_link(group, pair);
            unsigned was = exists ? sim_value(UA101_F_LINK, group, pair) : 0;
            unsigned start = wrong;

            if (byte_reaches(a, 3 + group, 2 * pair, 0x01, !was) != want)
                wrong++;
            else if (want) {
                pace();
                wrong += !WAIT(sim_value(UA101_F_LINK, group, pair) == !was, 3.0);
                wrong += (unsigned)byte_reaches(a, 3 + group, 2 * pair, 0x01, 2);
                wrong += (unsigned)byte_reaches(a, 3 + group, 2 * pair + 1, 0x01, was);
                wrong += !byte_reaches(a, 3 + group, 2 * pair, 0x01, was);
                pace();
                wrong += !WAIT(sim_value(UA101_F_LINK, group, pair) == was, 3.0);
                links++;
            } else if (exists) {
                wrong += sim_value(UA101_F_LINK, group, pair) != was;
            }
            if (wrong != start)
                printf("   note  the link of %s, pair %u: %u wrong\n", group_name[group], pair + 1, wrong - start);
        }
    for (group = 0; group < 4; group++) {
        int want = group < 3 && t_solo(group);
        unsigned start = wrong;

        for (value = 1; value < 14; value++) {
            int w = want && value <= 10;

            if (byte_reaches(a, 3 + group, 0x00, 0x00, value) != w)
                wrong++;
            else if (w) {
                pace();
                wrong += !WAIT(sim_value(UA101_F_SOLO, group, 0) == value, 3.0);
                solos++;
            }
        }
        wrong += (unsigned)byte_reaches(a, 3 + group, 0x00, 0x00, 0x7F);
        wrong += (unsigned)byte_reaches(a, 3 + group, 0x01, 0x00, 1);
        if (byte_reaches(a, 3 + group, 0x00, 0x00, 0) != want)
            wrong++;
        else if (want) {
            pace();
            wrong += !WAIT(sim_value(UA101_F_SOLO, group, 0) == 0, 3.0);
            solos++;
        }
        if (wrong != start)
            printf("   note  the solo of %s: %u wrong\n", group_name[group], wrong - start);
    }
    CHECK(wrong == 0 && links == T_LINKS && solos == T_SOLOS * T_SOLO_VALUES,
          "with it: the %u passed pairs' links reach the unit and go back, no byte 2, none at a "
          "pair's second channel; the %u solo numbers of the passed groups reach it, none above "
          "10; nothing else does (%u wrong)", links, solos, wrong);
    CHECK(atomic_load(&P.wire_link) == link + 2 * T_LINKS &&
              atomic_load(&P.wire_solo) == solo + T_SOLOS * T_SOLO_VALUES &&
              counts().ctl_changes == changes + 2u * T_LINKS + (unsigned)(T_SOLOS * T_SOLO_VALUES) &&
              atomic_load(&P.wire_volume) == volume && atomic_load(&P.wire_pan) == pan &&
              atomic_load(&P.wire_on) == on && atomic_load(&P.wire_patch) == patch &&
              atomic_load(&P.wire_saving) == saving,
          "on the cable: two links for each passed pair and eleven solos for each passed group, "
          "counted by the helper, and no volume, pan, on/off, patch bay or saving message");
    CHECK((log_lines_with("link, INPUT 1-2 to 1", "F0 41 10 00 69 12 00 04 00 01 01 7A F7") == 1) == t_link(1, 0) &&
              (log_lines_with("link, INPUT 1-2 to 0", "F0 41 10 00 69 12 00 04 00 01 00 7B F7") == 1) == t_link(1, 0) &&
              (log_lines_with("link, MONITOR OUT L-R to ", NULL) == 2) == t_link(0, 0) &&
              (log_lines_with("link, OUTPUT 9-10 to ", "F0 41 10 00 69 12 00 05 08 01") == 2) == t_link(2, 4) &&
              (log_lines_with("solo, INPUT group to 3 (INPUT 3 alone)", "F0 41 10 00 69 12 00 04 00 00 03 79 F7") == 1) == t_solo(1) &&
              (log_lines_with("solo, OUTPUT group to 10 (OUTPUT 10 alone)", "F0 41 10 00 69 12 00 05 00 00 0A 71 F7") == 1) == t_solo(2) &&
              (log_lines_with("solo, OUTPUT group to 0 (no solo)", "F0 41 10 00 69 12 00 05 00 00 00 7B F7") == 1) == t_solo(2) &&
              log_lines_with("unit: a change (", NULL) == 0 &&
              log_lines_with("saving (F0 41 10 00 69 12 00 04", NULL) == 0 &&
              log_lines_with("saving (F0 41 10 00 69 12 00 05", NULL) == 0,
          "the helper's log names each as a link of its pair or a solo of its group, with the "
          "bytes, never as the saving or as \"a change\"");

    /* The app's engine. A link is one message, and it moves nothing but its own bit: INPUT
       3-4, with INPUT 3 at -3 dB and INPUT 4 at 0 dB. */
    on_app(^{
      panel_set_min_gap(a->engine, 60000);
      panel_allow_writes(a->engine, 1);
      disagreements = panel_disagreements(a->engine);
    });
    refused_before = counts().ctl_refused;
    CHECK(engine_does(a, ^{ return panel_set_volume(a->engine, 1, 2, 0x16A09Eu, now_us()); }) &&
              sim_value(UA101_F_VOLUME, 1, 2) == 0x16A09Eu && sim_value(UA101_F_VOLUME, 1, 3) == 0x200000u &&
              sim_value(UA101_F_LINK, 1, 1) == 0,
          "INPUT 3-4 is not linked: the engine sets INPUT 3 to -3 dB, INPUT 4 stays at 0 dB");
    link = atomic_load(&P.wire_link);
    volume = atomic_load(&P.wire_volume);
    on = atomic_load(&P.wire_on);
    saving = atomic_load(&P.wire_saving);
    reads = atomic_load(&P.wire_reads);
    on_app(^{
      panel_unit_block(a->engine, bp);
    });
    CHECK(engine_does(a, ^{ return panel_set_link(a->engine, 1, 1, 1, now_us()); }) &&
              sim_value(UA101_F_LINK, 1, 1) == 1 && APP(panel_link(a->engine, 1, 1)) == 1,
          "the engine links INPUT 3-4: the unit takes it, and it is read back");
    on_app(^{
      panel_unit_block(a->engine, ap);
    });
    for (i = 0, differ = 0; i < 256; i++)
        differ += before[i] != after[i];
    CHECK(differ == 1 && before[25] == 0x00 && after[25] == 0x02,
          "in the block read back, d[25] went 00 to 02 (bit 1, the INPUT group's second pair), and nothing else moved");
    CHECK(atomic_load(&P.wire_link) == link + 1 && atomic_load(&P.wire_volume) == volume &&
              atomic_load(&P.wire_on) == on && atomic_load(&P.wire_saving) == saving &&
              atomic_load(&P.wire_reads) == reads + 1,
          "on the cable: one link message and one read; no volume, no on/off, no pause or resume of saving");
    CHECK(sim_value(UA101_F_VOLUME, 1, 2) == 0x16A09Eu && sim_value(UA101_F_VOLUME, 1, 3) == 0x200000u &&
              APP(panel_volume(a->engine, 1, 2)) == 0x16A09E && APP(panel_volume(a->engine, 1, 3)) == 0x200000,
          "the two channels keep their different volumes, in the unit and in the engine");
    CHECK(engine_does(a, ^{ return panel_set_volume(a->engine, 1, 3, 0x100001u, now_us()); }) &&
              sim_value(UA101_F_VOLUME, 1, 2) == 0x100001u && sim_value(UA101_F_VOLUME, 1, 3) == 0x100001u &&
              atomic_load(&P.wire_volume) == volume + 2,
          "the next move of either fader (INPUT 4 to -6 dB) goes to both: two volume messages, both channels equal");
    CHECK(engine_does(a, ^{ return panel_set_pan(a->engine, 1, 2, 0x4000u, now_us()); }) &&
              sim_value(UA101_F_PAN, 1, 2) == 0x4000u && sim_value(UA101_F_PAN, 1, 3) == 0x8000u &&
              engine_does(a, ^{ return panel_set_pan(a->engine, 1, 2, 0u, now_us()); }) &&
              sim_value(UA101_F_PAN, 1, 2) == 0u,
          "a pan still goes to the one channel, and back");
    CHECK(engine_does(a, ^{ return panel_set_link(a->engine, 1, 1, 0, now_us()); }) &&
              sim_value(UA101_F_LINK, 1, 1) == 0 && atomic_load(&P.wire_link) == link + 2 &&
              sim_value(UA101_F_VOLUME, 1, 2) == 0x100001u && sim_value(UA101_F_VOLUME, 1, 3) == 0x100001u,
          "unlinked again: one message, the volumes stay");
    CHECK(engine_does(a, ^{ return panel_set_volume(a->engine, 1, 2, 0x200000u, now_us()); }) &&
              sim_value(UA101_F_VOLUME, 1, 2) == 0x200000u && sim_value(UA101_F_VOLUME, 1, 3) == 0x100001u &&
              engine_does(a, ^{ return panel_set_volume(a->engine, 1, 3, 0x200000u, now_us()); }) &&
              sim_value(UA101_F_VOLUME, 1, 3) == 0x200000u,
          "INPUT 3 goes back to 0 dB alone, then INPUT 4");
    /* MONITOR OUT L-R and OUTPUT 9-10, the first and the last pair of the list. */
    for (group = 0; group < 3; group += 2) {
        unsigned p = group == 0 ? 0u : 4u;
        unsigned was = sim_value(UA101_F_LINK, group, p);
        unsigned at = group == 0 ? 24u : 28u; /* d[0x18], and d[0x1B + 1]; bit 0 in both */

        on_app(^{
          panel_unit_block(a->engine, bp);
        });
        CHECK(engine_does(a, ^{ return panel_set_link(a->engine, group, p, !was, now_us()); }) &&
                  sim_value(UA101_F_LINK, group, p) == !was,
              "the engine sets the link of %s, pair %u, to %u", group_name[group], p + 1, !was);
        on_app(^{
          panel_unit_block(a->engine, ap);
        });
        for (i = 0, differ = 0; i < 256; i++)
            differ += before[i] != after[i];
        CHECK(differ == 1 && (before[at] ^ after[at]) == 0x01,
              "in the block read back, bit 0 of d[%u] and nothing else moved", at);
        CHECK(engine_does(a, ^{ return panel_set_link(a->engine, group, p, (int)was, now_us()); }) &&
                  sim_value(UA101_F_LINK, group, p) == was,
              "and back");
    }

    /* A solo is one number a group. INPUT 3, then INPUT 8 takes it over, then out. */
    solo = atomic_load(&P.wire_solo);
    on_app(^{
      panel_unit_block(a->engine, bp);
    });
    CHECK(engine_does(a, ^{ return panel_toggle_solo(a->engine, 1, 2, now_us()); }) &&
              sim_value(UA101_F_SOLO, 1, 0) == 3 && sim_value(UA101_F_SOLO, 2, 0) == 0,
          "a click on INPUT 3's SOLO through the engine: the unit holds 3 for the INPUT group");
    on_app(^{
      panel_unit_block(a->engine, ap);
    });
    for (i = 0, differ = 0; i < 256; i++)
        differ += before[i] != after[i];
    CHECK(differ == 1 && before[22] == 0x00 && after[22] == 0x03,
          "in the block read back, d[22] went 00 to 03, and nothing else moved");
    CHECK(APP(panel_soloed(a->engine, 1, 2)) == 1 && APP(panel_soloed(a->engine, 1, 3)) == 0 &&
              APP(panel_soloed(a->engine, 2, 2)) == 0 && APP(panel_solo_raw(a->engine, 1)) == 3 &&
              APP(panel_solo_raw(a->engine, 2)) == 0,
          "the engine lights INPUT 3's lamp and no other");
    CHECK(engine_does(a, ^{ return panel_toggle_solo(a->engine, 1, 7, now_us()); }) &&
              sim_value(UA101_F_SOLO, 1, 0) == 8 && APP(panel_soloed(a->engine, 1, 2)) == 0 &&
              APP(panel_soloed(a->engine, 1, 7)) == 1,
          "a click on INPUT 8's: the unit holds 8, and only that lamp is lit");
    CHECK(engine_does(a, ^{ return panel_toggle_solo(a->engine, 1, 7, now_us()); }) &&
              sim_value(UA101_F_SOLO, 1, 0) == 0 && APP(panel_soloed(a->engine, 1, 7)) == 0,
          "a click on the lit one: the unit holds 0, no solo");
    /* A linked pair, OUTPUT 5-6: one number, two lamps, and either lamp puts it out. */
    {
        unsigned was = sim_value(UA101_F_LINK, 2, 2);

        CHECK(engine_does(a, ^{ return panel_set_link(a->engine, 2, 2, 1, now_us()); }) || was == 1,
              "OUTPUT 5-6 is linked");
        CHECK(engine_does(a, ^{ return panel_toggle_solo(a->engine, 2, 5, now_us()); }) &&
                  sim_value(UA101_F_SOLO, 2, 0) == 6 && sim_value(UA101_F_SOLO, 1, 0) == 0 &&
                  APP(panel_soloed(a->engine, 2, 4)) == 1 && APP(panel_soloed(a->engine, 2, 5)) == 1 &&
                  APP(panel_soloed(a->engine, 2, 3)) == 0 && APP(panel_soloed(a->engine, 2, 6)) == 0,
              "a click on OUTPUT 6's SOLO: one message, the unit holds 6, both lamps of the pair are lit");
        CHECK(engine_does(a, ^{ return panel_toggle_solo(a->engine, 2, 4, now_us()); }) &&
                  sim_value(UA101_F_SOLO, 2, 0) == 0,
              "a click on OUTPUT 5's lit lamp puts the solo out");
        if (was == 0)
            CHECK(engine_does(a, ^{ return panel_set_link(a->engine, 2, 2, 0, now_us()); }) &&
                      sim_value(UA101_F_LINK, 2, 2) == 0,
                  "OUTPUT 5-6 unlinked again, as it was");
    }
    CHECK(atomic_load(&P.wire_solo) == solo + 5, "five solo messages in all of that, one a click (%d)",
          atomic_load(&P.wire_solo) - solo);
    CHECK(counts().ctl_refused == refused_before &&
              (uint64_t)APP(panel_disagreements(a->engine)) == disagreements && APP(panel_writes_allowed(a->engine)),
          "in all of the engine's changes the helper refused nothing and the engine found no disagreement");
    on_app(^{
      panel_allow_writes(a->engine, 0);
      panel_set_min_gap(a->engine, 5000);
    });
}

static void test_two_apps(app *a)
{
    app *b = app_new(1);
    int closes = atomic_load(&P.closes), opens = atomic_load(&P.opens);
    int a_messages = atomic_load(&a->messages);

    CHECK(attach(b) == UA101_LINK_OK, "a second app attaches");
    CHECK(atomic_load(&P.opens) == opens, "to the interface already open");
    CHECK(WAIT(APP(panel_state(b->engine)) == PANEL_STATE_READY, 3.0), "and reads the settings");
    CHECK(WAIT(atomic_load(&a->messages) > a_messages, 3.0), "the first app hears the unit too");
    on_app(^{
      ua101_ctllink_detach(b->link);
    });
    usleep(100000);
    CHECK(atomic_load(&P.closes) == closes && counts().port_open,
          "one app let go: the interface stays open for the other");
    app_free(b);
    on_app(^{
      ua101_ctllink_detach(a->link);
    });
    CHECK(WAIT(atomic_load(&P.closes) == closes + 1 && !counts().port_open, 3.0),
          "the last app let go: the interface is given back");
    on_app(^{
      a->up = 0;
      panel_transport(a->engine, 0, now_us());
    });
    CHECK(attach(a) == UA101_LINK_OK, "attached again");
    CHECK(WAIT(APP(panel_state(a->engine)) == PANEL_STATE_READY, 3.0), "and in step again");
}

static int ask_levels(app *a)
{
    int before = atomic_load(&a->levels_events);

    on_app(^{
      ua101_ctllink_ask_levels(a->link);
    });
    return WAIT(atomic_load(&a->levels_events) > before, 3.0);
}

/* Levels come from the audio the helper streams anyway, and only while it streams. */
static void test_levels(app *a)
{
    xpc_connection_t plugin = plugin_connect();
    int events = atomic_load(&a->audio_events), wire = atomic_load(&P.wire_messages);
    __block ua101_ctl_levels l;

    CHECK(ask_levels(a), "levels answered");
    CHECK(!APP(a->levels.streaming), "nothing plays or records: no levels");

    CHECK(plugin_ask(plugin, "hello", 3) == UA101_LINK_ERR_VERSION,
          "a plug-in from the build before this one is told the versions differ");
    CHECK(plugin_ask(plugin, "hello", UA101_LINK_VERSION) == UA101_LINK_OK, "a plug-in says hello");
    CHECK(plugin_ask(plugin, "start", UA101_LINK_VERSION) == UA101_LINK_OK, "and starts streaming");
    CHECK(WAIT(atomic_load(&a->audio_events) > events && atomic_load(&a->audio_now) == 1, 3.0),
          "the app is told that audio is streaming");
    CHECK(ask_levels(a), "levels answered");
    on_app(^{
      l = a->levels;
    });
    CHECK(l.streaming && l.rate_hz == 48000 && l.capture_channels == 12 && l.playback_channels == 10,
          "streaming at 48 kHz, 12 channels in and 10 out (%d, %.0f, %u, %u)", l.streaming,
          l.rate_hz, l.capture_channels, l.playback_channels);
    CHECK(l.capture[0] == 0x80000000u, "INPUT 1 touched full scale once (0x%08x)", l.capture[0]);
    CHECK(l.capture[1] == 2u << 24 && l.capture[9] == 10u << 24, "the steady inputs read their level");
    CHECK(l.capture[3] == 4u << 24, "a negative level counts as much as a positive one");
    CHECK(l.capture[10] == 11u << 24 && l.capture[11] == 12u << 24, "inputs 11 and 12: the monitor mix");
    CHECK(l.playback[0] == 1u << 16 && l.playback[9] == 10u << 16, "what the computer plays reads too");
    CHECK(ask_levels(a), "levels answered");
    on_app(^{
      l = a->levels;
    });
    CHECK(l.streaming && l.capture[0] == 0 && l.capture[11] == 0 && l.playback[9] == 0,
          "asked again with no new audio: nothing is counted twice");
    CHECK(atomic_load(&P.wire_messages) == wire, "levels send nothing to the unit");
    CHECK(counts().port_open, "and streaming leaves the MIDI interface alone");

    events = atomic_load(&a->audio_events);
    CHECK(plugin_ask(plugin, "stop", UA101_LINK_VERSION) == UA101_LINK_OK, "the plug-in stops");
    CHECK(WAIT(atomic_load(&a->audio_events) > events && atomic_load(&a->audio_now) == 0, 3.0),
          "the app is told that audio stopped");
    CHECK(ask_levels(a) && !APP(a->levels.streaming), "and there are no levels again");
    xpc_connection_cancel(plugin);
    xpc_release(plugin);
}

/* A run of a plug-in that stops with these counts. `peak` below 0 leaves the peak out, as a
   plug-in from before it did; `cut` below 0 leaves the correction's two out, as a run without
   correction does. */
static int plugin_run(xpc_connection_t c, uint64_t clipped, double peak, int64_t cut,
                      double cut_peak)
{
    xpc_object_t m = xpc_dictionary_create(NULL, NULL, 0), reply;
    int ok = plugin_ask(c, "start", UA101_LINK_VERSION) == UA101_LINK_OK;

    xpc_dictionary_set_string(m, UA101_LINK_OP, "stop");
    xpc_dictionary_set_uint64(m, UA101_LINK_KEY_VERSION, UA101_LINK_VERSION);
    xpc_dictionary_set_uint64(m, UA101_LINK_MISSING, 0);
    xpc_dictionary_set_uint64(m, UA101_LINK_CLIPPED, clipped);
    if (peak >= 0.0)
        xpc_dictionary_set_double(m, UA101_LINK_PEAK, peak);
    if (cut >= 0) {
        xpc_dictionary_set_uint64(m, UA101_LINK_CUT, (uint64_t)cut);
        xpc_dictionary_set_double(m, UA101_LINK_CUT_PEAK, cut_peak);
    }
    reply = xpc_connection_send_message_with_reply_sync(c, m);
    ok = ok && xpc_get_type(reply) == XPC_TYPE_DICTIONARY &&
         xpc_dictionary_get_int64(reply, UA101_LINK_STATUS) == UA101_LINK_OK;
    xpc_release(reply);
    xpc_release(m);
    return ok;
}

/* The line a run leaves in the log says what was over full scale, and whose it was: the apps'
   own samples, or the rate correction's result between them. Neither fails the run. */
static void test_verdict_says_what_was_over(void)
{
    xpc_connection_t plugin = plugin_connect();

    CHECK(plugin_ask(plugin, "hello", UA101_LINK_VERSION) == UA101_LINK_OK, "a plug-in says hello");

    CHECK(plugin_run(plugin, 3, 1.0233, 12, 1.135), "a corrected run with both kinds");
    CHECK(log_lines_with("streaming PASS",
                         "the apps sent 3 samples at or over full scale (highest +0.20 dB); the "
                         "rate correction's own result went over at 12 samples (highest +1.10 "
                         "dB)") == 1,
          "the line names both, each with how far over, and the run still passes");

    CHECK(plugin_run(plugin, 0, 0.0, 0, 0.0), "a corrected run with nothing over");
    CHECK(log_lines_with("the apps sent 0 samples at or over full scale; the rate correction's "
                         "own result went over at 0 samples",
                         NULL) == 1,
          "the line says 0 for both");
    CHECK(log_lines_with("the apps sent 0 samples", "highest") == 0, "and no \"highest\"");

    CHECK(plugin_run(plugin, 7, 1.5, -1, 0.0), "a run without correction");
    CHECK(log_lines_with("the apps sent 7 samples at or over full scale (highest +3.52 dB)",
                         NULL) == 1,
          "the line says what the apps sent");
    CHECK(log_lines_with("the apps sent 7 samples", "rate correction's") == 0,
          "and nothing about a correction that wasn't on");

    CHECK(plugin_run(plugin, 4, -1.0, -1, 0.0), "a plug-in from before these were measured");
    CHECK(log_lines_with("the apps sent 4 samples at or over full scale", NULL) == 1,
          "the line has its count");
    CHECK(log_lines_with("the apps sent 4 samples", "highest") == 0 &&
              log_lines_with("the apps sent 4 samples", "rate correction's") == 0,
          "and says nothing it wasn't told");

    CHECK(plugin_run(plugin, 1, (double)INFINITY, 5, 1.0), "an infinite sample from an app");
    CHECK(log_lines_with("the apps sent 1 samples at or over full scale (highest: not a finite "
                         "number); the rate correction's own result went over at 5 samples "
                         "(highest +0.00 dB)",
                         NULL) == 1,
          "the line says so in words");
    xpc_connection_cancel(plugin);
    xpc_release(plugin);
}

/* Unplugged, switched on again, the Mac sleeping and waking, the interface failing. */
static void test_coming_and_going(app *a)
{
    int changed = atomic_load(&a->changed), closes = atomic_load(&P.closes);

    unit_goes();
    CHECK(WAIT(atomic_load(&a->changed) == changed + 1, 3.0), "unplugged: the app is told");
    CHECK(atomic_load(&P.closes) == closes + 1 && !counts().port_open, "the interface is closed");
    CHECK(attach(a) == UA101_LINK_ERR_ABSENT && atomic_load(&a->last_watching) == 1,
          "attaching says there is no unit");
    unit_comes();
    CHECK(WAIT(atomic_load(&a->changed) == changed + 2, 3.0), "plugged in: the app is told");
    CHECK(attach(a) == UA101_LINK_OK, "attached again");
    CHECK(WAIT(APP(panel_state(a->engine)) == PANEL_STATE_READY, 3.0), "settings read again");

    changed = atomic_load(&a->changed);
    closes = atomic_load(&P.closes);
    on_service_queue(^{
      W.events.will_sleep();
    });
    CHECK(atomic_load(&P.closes) == closes + 1 && !counts().port_open,
          "the Mac sleeps: the interface is given back first");
    CHECK(atomic_load(&a->changed) == changed, "the app isn't told until the Mac wakes");
    CHECK(attach(a) == UA101_LINK_ERR_ASLEEP, "attaching meanwhile is answered \"asleep\"");
    CHECK(!counts().port_open, "and opens nothing");
    on_service_queue(^{
      W.events.did_wake();
    });
    CHECK(WAIT(atomic_load(&a->changed) == changed + 1, 3.0), "the Mac woke: the app is told");
    CHECK(attach(a) == UA101_LINK_OK, "attached again");

    changed = atomic_load(&a->changed);
    closes = atomic_load(&P.closes);
    on_service_queue(^{
      P.events.lost(NULL, "the pipe stalled (pretend)");
    });
    CHECK(WAIT(atomic_load(&a->changed) == changed + 1, 3.0), "the interface failed: the app is told");
    CHECK(atomic_load(&P.closes) == closes + 1 && !counts().port_open, "and it is closed");
    CHECK(attach(a) == UA101_LINK_OK, "attached again");
    CHECK(WAIT(APP(panel_state(a->engine)) == PANEL_STATE_READY, 3.0), "settings read again");
}

/* More reads than any app needs: the helper stops passing them on. */
static void test_flood(app *a)
{
    uint8_t request[UA101_CTL_READ_REQUEST_BYTES];
    int refused = atomic_load(&a->refused), reads = atomic_load(&P.wire_reads), i;

    ua101_ctl_read_request(request);
    for (i = 0; i < 45; i++)
        send_raw(a, request, sizeof(request));
    CHECK(WAIT(atomic_load(&a->refused) - refused + atomic_load(&P.wire_reads) - reads == 45, 5.0),
          "all 45 were answered");
    CHECK(atomic_load(&a->refused) - refused >= 5 && atomic_load(&P.wire_reads) - reads <= 40,
          "at most 20 a second were passed on (%d refused)", atomic_load(&a->refused) - refused);
    sleep(1); /* a new second, so later cases start clean */
}

/* A helper that isn't there, or an older one that turns the app away. */
static void test_no_helper(void)
{
    xpc_connection_t deaf = xpc_connection_create(NULL, NULL);
    xpc_endpoint_t ep;
    app *a;

    xpc_connection_set_event_handler(deaf, ^(xpc_object_t event) {
      if (xpc_get_type(event) == XPC_TYPE_CONNECTION)
          xpc_connection_cancel((xpc_connection_t)event);
    });
    xpc_connection_resume(deaf);
    ep = xpc_endpoint_create(deaf);
    ua101_ctllink_use_endpoint(ep);
    a = app_new(1);
    CHECK(attach(a) == UA101_LINK_ERR_NO_HELPER, "turned away: the app is told there is no helper (%d)",
          atomic_load(&a->last_status));
    CHECK(strstr(app_why(a), "older") != NULL, "with the likely reason in words: %s", app_why(a));
    CHECK(APP(panel_state(a->engine)) == PANEL_STATE_OFFLINE, "the engine stays offline");
    CHECK(attach(a) == UA101_LINK_ERR_NO_HELPER, "trying again gives the same answer, not a hang");
    app_free(a);
    ua101_ctllink_use_endpoint(g_endpoint);
    xpc_release(ep);
    xpc_connection_cancel(deaf);
    xpc_release(deaf);
}

/* A helper one version older (19, before the memory preview): the app's link asks again in
   version 19, is attached, reads no memory, and every later request carries 19. Any older
   helper stays an error. The helpers here are stand-ins that answer by the version alone. */
static void test_older_helper(void)
{
    unsigned kind;

    for (kind = 0; kind < 2; kind++) {
        const uint64_t speaks = kind == 0 ? UA101_LINK_VERSION_OLDER_HELPER : 18;
        xpc_connection_t old = xpc_connection_create(NULL, NULL);
        __block _Atomic int asked20 = 0, asked_older = 0, asked_other = 0, sends_older = 0;
        xpc_endpoint_t ep;
        uint8_t request[UA101_CTL_READ_REQUEST_BYTES];
        app *a;

        xpc_connection_set_event_handler(old, ^(xpc_object_t peer) {
          if (xpc_get_type(peer) != XPC_TYPE_CONNECTION)
              return;
          xpc_connection_set_event_handler((xpc_connection_t)peer, ^(xpc_object_t m) {
            xpc_object_t reply;
            uint64_t v;
            const char *op;

            if (xpc_get_type(m) != XPC_TYPE_DICTIONARY || (reply = xpc_dictionary_create_reply(m)) == NULL)
                return;
            v = xpc_dictionary_get_uint64(m, UA101_LINK_KEY_VERSION);
            op = xpc_dictionary_get_string(m, UA101_LINK_OP);
            if (v == UA101_LINK_VERSION)
                atomic_fetch_add(&asked20, 1);
            else if (v == speaks)
                atomic_fetch_add(&asked_older, 1);
            else
                atomic_fetch_add(&asked_other, 1);
            if (v != speaks) {
                xpc_dictionary_set_int64(reply, UA101_LINK_STATUS, UA101_LINK_ERR_VERSION);
                xpc_dictionary_set_string(reply, UA101_LINK_WHY, "different builds");
            } else {
                xpc_dictionary_set_int64(reply, UA101_LINK_STATUS, UA101_LINK_OK);
                xpc_dictionary_set_bool(reply, UA101_LINK_PRESENT, true);
                xpc_dictionary_set_uint64(reply, UA101_LINK_CTL_PASSES, UA101_CTL_PASS_SAVING);
                /* Never sent by a real helper of 19; the app mustn't believe it from one. */
                xpc_dictionary_set_uint64(reply, UA101_LINK_CTL_PASSES_READ, 0x1F);
                if (op != NULL && strcmp(op, "ctl_send") == 0)
                    atomic_fetch_add(&sends_older, 1);
            }
            xpc_connection_send_message((xpc_connection_t)peer, reply);
            xpc_release(reply);
          });
          xpc_connection_resume((xpc_connection_t)peer);
        });
        xpc_connection_resume(old);
        ep = xpc_endpoint_create(old);
        ua101_ctllink_use_endpoint(ep);
        a = app_new(0);
        if (kind == 0) {
            CHECK(attach(a) == UA101_LINK_OK, "a helper of link version 19 is attached to (%d: %s)",
                  atomic_load(&a->last_status), app_why(a));
            CHECK(atomic_load(&asked20) == 1 && atomic_load(&asked_older) == 1 && atomic_load(&asked_other) == 0,
                  "asked once in version 20, then once in 19 (%d, %d)", atomic_load(&asked20),
                  atomic_load(&asked_older));
            CHECK(APP(ua101_ctllink_version(a->link)) == UA101_LINK_VERSION_OLDER_HELPER,
                  "the link says it speaks 19");
            CHECK(atomic_load(&a->passes) == UA101_CTL_PASS_SAVING && atomic_load(&a->read_passes) == 0,
                  "what that helper passes on is taken, and no memory read: no preview");
            ua101_ctl_read_request(request);
            CHECK(send_raw(a, request, sizeof(request)) == 1 && WAIT(atomic_load(&sends_older) == 1, 3.0),
                  "a request afterwards goes in version 19 too");
            CHECK(attach(a) == UA101_LINK_OK && atomic_load(&asked20) == 2,
                  "each attach tries 20 first, in case the helper was updated");
        } else {
            CHECK(attach(a) == UA101_LINK_ERR_VERSION, "a helper of link version 18 is an error (%d)",
                  atomic_load(&a->last_status));
            CHECK(atomic_load(&asked20) == 1 && atomic_load(&asked_other) == 1 && atomic_load(&asked_older) == 0,
                  "after one try in 20 and one in 19, no more (%d, %d)", atomic_load(&asked20),
                  atomic_load(&asked_other));
        }
        app_free(a);
        ua101_ctllink_use_endpoint(g_endpoint);
        xpc_release(ep);
        xpc_connection_cancel(old);
        xpc_release(old);
    }
}

/* Closing with things in flight: no event arrives afterwards (the address checker watches). */
static void test_close_in_flight(void)
{
    app *a = app_new(0);
    static uint8_t request[UA101_CTL_READ_REQUEST_BYTES];

    ua101_ctl_read_request(request);
    on_app(^{
      ua101_ctllink_attach(a->link);
      ua101_ctllink_ask_levels(a->link);
    });
    app_free(a);
    a = app_new(0);
    CHECK(attach(a) == UA101_LINK_OK, "an app without an engine attaches");
    on_app(^{
      a->up = 1;
      ua101_ctllink_send(a->link, request, sizeof(request));
      ua101_ctllink_ask_levels(a->link);
    });
    app_free(a);
    usleep(200000);
    CHECK(WAIT(!counts().port_open, 3.0), "and its interface was given back when it closed");
}

/* --- MIDI clients (link version 12): the MIDI IN and OUT sockets, beside the control app. --- */

typedef struct {
    ua101_midilink *link;
    _Atomic int attached_events, last_status, last_watching, changed;
    _Atomic int got_bytes, got_events;
    _Atomic uint64_t last_time;
    uint8_t got[8192]; /* on the app's queue */
} mclient;

static void m_attached(void *ctx, int status, int watching, const char *why)
{
    mclient *m = ctx;

    (void)why;
    atomic_store(&m->last_status, status);
    atomic_store(&m->last_watching, watching);
    atomic_fetch_add(&m->attached_events, 1);
}

static void m_midi(void *ctx, const uint8_t *bytes, size_t len, uint64_t host_time)
{
    mclient *m = ctx;
    size_t have = (size_t)atomic_load(&m->got_bytes);

    if (have + len <= sizeof(m->got))
        memcpy(m->got + have, bytes, len);
    atomic_store(&m->last_time, host_time);
    atomic_fetch_add(&m->got_bytes, (int)len);
    atomic_fetch_add(&m->got_events, 1);
}

static void m_changed(void *ctx)
{
    mclient *m = ctx;

    atomic_fetch_add(&m->changed, 1);
}

static mclient *m_new(void)
{
    static const ua101_midilink_events events = {m_attached, m_midi, m_changed};
    mclient *m = calloc(1, sizeof(*m));

    dispatch_sync(A, ^{
      m->link = ua101_midilink_open(A, &events, m);
    });
    return m;
}

static void m_free(mclient *m)
{
    dispatch_sync(A, ^{
      ua101_midilink_close(m->link);
    });
    dispatch_sync(A, ^{
    });
    free(m);
}

/* Attaches and waits for the answer. Returns its status. */
static int m_attach(mclient *m)
{
    int before = atomic_load(&m->attached_events);

    on_app(^{
      ua101_midilink_attach(m->link);
    });
    CHECK(WAIT(atomic_load(&m->attached_events) > before, 8.0), "the MIDI client's attach was answered");
    return atomic_load(&m->last_status);
}

static void m_send(mclient *m, const uint8_t *bytes, size_t len)
{
    on_app(^{
      ua101_midilink_send(m->link, bytes, len);
    });
}

static void m_detach(mclient *m)
{
    on_app(^{
      ua101_midilink_detach(m->link);
    });
}

/* Whether the last `len` bytes the client got are `want`. */
static int m_got_ends(mclient *m, const uint8_t *want, size_t len)
{
    __block int same;

    on_app(^{
      size_t have = (size_t)atomic_load(&m->got_bytes);

      same = have >= len && have <= sizeof(m->got) && memcmp(m->got + have - len, want, len) == 0;
    });
    return same;
}

static int wire_midi(void)
{
    return atomic_load(&P.wire_midi_bytes);
}

/* Whether the last `len` MIDI bytes that reached the MIDI OUT socket are `want`. */
static int wire_midi_ends(const uint8_t *want, size_t len)
{
    __block int same;

    on_service_queue(^{
      same = P.midi_len >= len && memcmp(P.midi + P.midi_len - len, want, len) == 0;
    });
    return same;
}

/* The pretend unit sends a read of packets, of any cables, stamped `host_time`. */
static void unit_sends(const uint8_t *packets, size_t bytes, uint64_t host_time)
{
    on_service_queue(^{
      if (P.open)
          P.events.packets(NULL, packets, bytes, host_time);
    });
}

/* A connection of the test's own that counts the events the helper sends it, by kind: the
   links above ignore events that aren't theirs, so only this shows what was really sent. */
typedef struct {
    _Atomic int midi, ctl;
    /* "midi_lamps": how many, how many said in and out, and when each came (seconds). */
    _Atomic int lamps, lamp_in, lamp_out;
    double lamp_at[4096];
} heard;

static xpc_connection_t raw_listener(heard *h)
{
    xpc_connection_t c = xpc_connection_create_from_endpoint(g_endpoint);

    xpc_connection_set_event_handler(c, ^(xpc_object_t event) {
      if (xpc_get_type(event) == XPC_TYPE_DICTIONARY) {
          const char *what = xpc_dictionary_get_string(event, UA101_LINK_EVENT);

          if (what != NULL && strcmp(what, "midi") == 0)
              atomic_fetch_add(&h->midi, 1);
          if (what != NULL && strcmp(what, "ctl") == 0)
              atomic_fetch_add(&h->ctl, 1);
          if (what != NULL && strcmp(what, "midi_lamps") == 0) {
              int n = atomic_load(&h->lamps);

              if (n < (int)(sizeof(h->lamp_at) / sizeof(h->lamp_at[0])))
                  h->lamp_at[n] = now_s();
              atomic_fetch_add(&h->lamp_in, xpc_dictionary_get_bool(event, UA101_LINK_MIDI_IN));
              atomic_fetch_add(&h->lamp_out, xpc_dictionary_get_bool(event, UA101_LINK_MIDI_OUT));
              atomic_fetch_add(&h->lamps, 1);
          }
      }
    });
    xpc_connection_resume(c);
    return c;
}

static const uint8_t k_note_on[] = {0x90, 0x3C, 0x40};
static const uint8_t k_note_off[] = {0x80, 0x3C, 0x00};

/* A message by hand over a connection of the test's own: what the answer's status is. */
static int64_t raw_midi_send(xpc_connection_t c, const uint8_t *bytes, size_t len)
{
    xpc_object_t m = xpc_dictionary_create(NULL, NULL, 0), reply;
    int64_t status = -1;

    xpc_dictionary_set_string(m, UA101_LINK_OP, "midi_send");
    xpc_dictionary_set_uint64(m, UA101_LINK_KEY_VERSION, UA101_LINK_VERSION);
    xpc_dictionary_set_data(m, UA101_LINK_DATA, bytes, len);
    reply = xpc_connection_send_message_with_reply_sync(c, m);
    if (xpc_get_type(reply) == XPC_TYPE_DICTIONARY)
        status = xpc_dictionary_get_int64(reply, UA101_LINK_STATUS);
    xpc_release(reply);
    xpc_release(m);
    return status;
}

/* What arrives at the MIDI IN socket reaches MIDI clients, with the read's time; the other
   cables' packets in the same read don't. */
static void test_midi_in(app *a, mclient *m)
{
    static const uint8_t alone[] = {0x09, 0x90, 0x3C, 0x40};
    static const uint8_t change[13] = {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00,
                                       0x01, 0x00, 0x00, 0x01, 0x7E, 0xF7};
    static const uint8_t want[] = {0x90, 0x3C, 0x40, 0xF8, 0x80, 0x3C, 0x00};
    static uint8_t read[64];
    size_t used = 0;
    static heard as_control, as_midi;
    uint64_t events = counts().midi_in_events;
    int opens = atomic_load(&P.opens), a_messages, got;
    xpc_connection_t old = plugin_connect(), raw_control, raw_midi;

    CHECK(plugin_ask(old, "midi_open", UA101_LINK_VERSION - 1) == UA101_LINK_ERR_VERSION,
          "a MIDI client from another build is told so");
    xpc_connection_cancel(old);
    xpc_release(old);

    unit_sends(alone, sizeof(alone), 111);
    CHECK(counts().midi_in_events == events && counts().midi_clients == 0,
          "with no MIDI client attached, what arrives at MIDI IN goes nowhere");

    CHECK(m_attach(m) == UA101_LINK_OK, "a MIDI client attaches (%d)", atomic_load(&m->last_status));
    CHECK(counts().midi_clients == 1 && atomic_load(&P.opens) == opens,
          "to the interface already open for the control app");
    raw_control = raw_listener(&as_control);
    raw_midi = raw_listener(&as_midi);
    CHECK(plugin_ask(raw_control, "ctl_open", UA101_LINK_VERSION) == UA101_LINK_OK &&
              plugin_ask(raw_midi, "midi_open", UA101_LINK_VERSION) == UA101_LINK_OK,
          "two listeners by hand attach too, one as a control app and one as a MIDI client");

    /* One read with three cables in it: a note on (cable 0), a whole control message
       (cable 1), a note on a cable the unit doesn't have, a clock byte and a note off. */
    memcpy(read + used, alone, 4);
    used += 4;
    used += ua101_ctl_pack_sysex(change, sizeof(change), UA101_CTL_CABLE, read + used,
                                 sizeof(read) - used);
    memcpy(read + used, (const uint8_t[]){0x29, 0x90, 0x40, 0x40}, 4);
    used += 4;
    memcpy(read + used, (const uint8_t[]){0x0F, 0xF8, 0x00, 0x00}, 4);
    used += 4;
    memcpy(read + used, (const uint8_t[]){0x08, 0x80, 0x3C, 0x00}, 4);
    used += 4;
    a_messages = atomic_load(&a->messages);
    got = atomic_load(&m->got_bytes);
    unit_sends(read, used, 987654321);
    CHECK(WAIT(atomic_load(&m->got_bytes) == got + (int)sizeof(want), 3.0) &&
              m_got_ends(m, want, sizeof(want)),
          "a read with three cables in it: the MIDI client gets cable 0's bytes, in order, and "
          "nothing else (%d bytes)",
          atomic_load(&m->got_bytes) - got);
    CHECK(atomic_load(&m->last_time) == 987654321,
          "stamped with the time the read completed, as the port gave it");
    CHECK(WAIT(atomic_load(&a->messages) > a_messages, 3.0),
          "the control app gets the control cable's message from the same read");
    CHECK(WAIT(APP(panel_state(a->engine)) == PANEL_STATE_READY, 3.0), "and is in step again");
    usleep(50000);
    CHECK(atomic_load(&m->got_bytes) == got + (int)sizeof(want),
          "what the unit then answered the control app stayed off the MIDI client");
    CHECK(atomic_load(&as_control.ctl) >= 1 && atomic_load(&as_control.midi) == 0,
          "the helper sent the control app's listener control messages (%d) and nothing of MIDI "
          "IN (%d)",
          atomic_load(&as_control.ctl), atomic_load(&as_control.midi));
    CHECK(atomic_load(&as_midi.midi) >= 1 && atomic_load(&as_midi.ctl) == 0,
          "and the MIDI client's listener MIDI IN (%d) and nothing of the control cable (%d)",
          atomic_load(&as_midi.midi), atomic_load(&as_midi.ctl));
    CHECK(WAIT(atomic_load(&as_control.lamps) >= 1, 0.5) && atomic_load(&as_midi.lamps) == 0,
          "the MIDI lamps' event went to the control app's listener (%d) and never to the MIDI "
          "client's (%d)",
          atomic_load(&as_control.lamps), atomic_load(&as_midi.lamps));
    xpc_connection_cancel(raw_control);
    xpc_release(raw_control);
    xpc_connection_cancel(raw_midi);
    xpc_release(raw_midi);
    CHECK(WAIT(counts().midi_clients == 1, 3.0), "the listeners are gone again");
}

/* What a MIDI client sends reaches the MIDI OUT socket, cable 0, and nothing else; SysEx
   addressed to the UA-101 itself reaches nothing. */
static void test_midi_out(mclient *m)
{
    static const uint8_t pause[13] = {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00,
                                      0x00, 0x00, 0x00, 0x00, 0x00, 0xF7};
    static const uint8_t reset_all[13] = {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00,
                                          0x00, 0x00, 0x00, 0x02, 0x7E, 0xF7};
    static const uint8_t other[] = {0xF0, 0x7D, 0x01, 0x02, 0x03, 0x04, 0x05, 0xF7};
    static const uint8_t gs_reset[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00,
                                       0x7F, 0x00, 0x41, 0xF7};
    /* What a control message looks like as USB packets for the control cable. Handed over as
       MIDI bytes it is only that: bytes for MIDI OUT. */
    static const uint8_t packets[] = {0x14, 0xF0, 0x41, 0x10, 0x14, 0x00,
                                      0x69, 0x12, 0x15, 0xF7, 0x00, 0x00};
    static const uint8_t packets_as_midi[] = {0xF0, 0x41, 0x10, 0x14, 0x00, 0x69, 0x12, 0x15, 0xF7};
    uint8_t request[UA101_CTL_READ_REQUEST_BYTES], volume[UA101_CTLMSG_MAX_BYTES],
        patch[UA101_CTLMSG_MAX_BYTES];
    size_t volume_len = volume_message(1, 0, 0x100000, volume), patch_len = patch_message(0, 3, patch);
    int wm = wire_midi(), messages = atomic_load(&P.wire_messages);
    uint64_t own = counts().gate.own_sysex;
    uint64_t sim_sets, sim_now;
    __block uint64_t sets;

    on_service_queue(^{
      sets = P.sim.sets_applied + P.sim.sets_ignored + P.sim.memory_commands +
             P.sim.unknown_messages + P.sim.bad_messages;
    });
    sim_sets = sets;
    ua101_ctl_read_request(request);

    m_send(m, k_note_on, sizeof(k_note_on));
    CHECK(WAIT(wire_midi() == wm + 3, 3.0) && wire_midi_ends(k_note_on, sizeof(k_note_on)),
          "a note from a MIDI client reaches the MIDI OUT socket");
    CHECK(atomic_load(&P.wire_messages) == messages && atomic_load(&P.wire_other) == 0,
          "on cable 0, and nothing on any other");

    /* Everything a control app could say, sent as MIDI: none of it reaches any cable. */
    m_send(m, request, sizeof(request));
    m_send(m, pause, sizeof(pause));
    m_send(m, reset_all, sizeof(reset_all));
    m_send(m, patch, patch_len);
    m_send(m, volume, volume_len);
    m_send(m, k_note_off, sizeof(k_note_off));
    CHECK(WAIT(wire_midi() == wm + 6, 3.0) && wire_midi_ends(k_note_off, sizeof(k_note_off)),
          "the settings request, \"pause saving\", \"reset all\", a patch bay change and a "
          "volume change from a MIDI client: nothing of them reaches MIDI OUT; the note after "
          "them does (%d bytes)",
          wire_midi() - wm);
    CHECK(counts().gate.own_sysex == own + 5 && counts().gate.gate_own_sysex == 0,
          "the helper dropped all five as SysEx addressed to the UA-101 itself (%llu)",
          (unsigned long long)(counts().gate.own_sysex - own));
    on_service_queue(^{
      sets = P.sim.sets_applied + P.sim.sets_ignored + P.sim.memory_commands +
             P.sim.unknown_messages + P.sim.bad_messages;
    });
    sim_now = sets;
    CHECK(atomic_load(&P.wire_messages) == messages && sim_now == sim_sets,
          "nothing reached the control cable, and the pretend unit was asked nothing");
    CHECK(log_lines_with("SysEx addressed to the UA-101 itself", "said once") == 1,
          "and the helper's log says so once");

    m_send(m, other, sizeof(other));
    CHECK(WAIT(wire_midi() == wm + 6 + (int)sizeof(other), 3.0) && wire_midi_ends(other, sizeof(other)),
          "SysEx of another maker passes whole");
    m_send(m, gs_reset, sizeof(gs_reset));
    CHECK(WAIT(wire_midi() == wm + 6 + (int)(sizeof(other) + sizeof(gs_reset)), 3.0) &&
              wire_midi_ends(gs_reset, sizeof(gs_reset)),
          "so does Roland SysEx for another model");

    wm = wire_midi();
    m_send(m, packets, sizeof(packets));
    CHECK(WAIT(wire_midi() == wm + (int)sizeof(packets_as_midi), 3.0) &&
              wire_midi_ends(packets_as_midi, sizeof(packets_as_midi)),
          "bytes shaped like USB packets for the control cable are taken as MIDI bytes: they "
          "leave by MIDI OUT (%d)",
          wire_midi() - wm);
    CHECK(atomic_load(&P.wire_messages) == messages && atomic_load(&P.wire_other) == 0,
          "a MIDI client can't name a cable: still nothing on the control cable");
}

/* A long SysEx leaves at the wire's speed, a little at a time, and the control app isn't kept
   waiting behind it. */
static void test_midi_pacing(app *a, mclient *m)
{
    enum { LONG = 3000 };
    static uint8_t sysex[LONG];
    uint8_t request[UA101_CTL_READ_REQUEST_BYTES];
    int wm = wire_midi(), reads, i, early;
    double t0, took;

    sysex[0] = 0xF0;
    sysex[1] = 0x7D;
    for (i = 2; i < LONG - 1; i++)
        sysex[i] = (uint8_t)(i & 0x7F);
    sysex[LONG - 1] = 0xF7;
    ua101_ctl_read_request(request);
    atomic_store(&P.wire_midi_most, 0);

    t0 = now_s();
    m_send(m, sysex, LONG);
    usleep(400000);
    early = wire_midi() - wm;
    CHECK(early >= 300 && early <= UA101_MIDIGATE_BURST_BYTES + 3125 * (now_s() - t0) + 40,
          "0.4 s after a 3,000 byte SysEx was handed over, %d bytes have left: the wire's speed",
          early);
    reads = atomic_load(&P.wire_reads);
    send_raw(a, request, sizeof(request));
    CHECK(WAIT(atomic_load(&P.wire_reads) == reads + 1, 0.3),
          "while it is still leaving, the control app's request reaches the unit at once");
    CHECK(wire_midi() - wm < LONG, "(the SysEx was indeed still leaving: %d)", wire_midi() - wm);
    CHECK(WAIT(wire_midi() == wm + LONG, 4.0) && wire_midi_ends(sysex, LONG),
          "all of it arrives, in order (%d)", wire_midi() - wm);
    took = now_s() - t0;
    CHECK(took >= (LONG - UA101_MIDIGATE_BURST_BYTES) / 3125.0 - 0.02,
          "it took the wire's time: %.2f s for 3,000 bytes", took);
    printf("   note  a 3,000 byte SysEx took %.2f s to leave by MIDI OUT (the wire: %.2f s); %d "
           "bytes had left after 0.4 s; the largest write was %d MIDI bytes\n",
           took, LONG / 3125.0, early, atomic_load(&P.wire_midi_most));
    CHECK(atomic_load(&P.wire_midi_most) <= (int)UA101_MIDIGATE_BURST_BYTES,
          "no write carried more than %u bytes (%d)", UA101_MIDIGATE_BURST_BYTES,
          atomic_load(&P.wire_midi_most));
    CHECK(WAIT(APP(panel_state(a->engine)) == PANEL_STATE_READY, 3.0), "the control app is in step");
}

/* More than the queue holds; then the unit unplugged, switched on again, the Mac sleeping and
   waking, the interface failing: closed for both kinds of client, and both told. */
static void test_midi_coming_and_going(app *a, mclient *m)
{
    enum { HUGE_BYTES = 70000 };
    static uint8_t huge[HUGE_BYTES];
    uint8_t request[UA101_CTL_READ_REQUEST_BYTES];
    uint64_t overflow = counts().gate.overflow_messages, flushed, refused;
    int a_changed, m_changed, closes, reads, wm, i;

    huge[0] = 0xF0;
    huge[1] = 0x7D;
    for (i = 2; i < HUGE_BYTES; i++)
        huge[i] = 0x22;
    ua101_ctl_read_request(request);

    m_send(m, huge, HUGE_BYTES);
    CHECK(WAIT(counts().gate.overflow_messages == overflow + 1, 3.0),
          "a SysEx longer than the queue: cut where the queue was full, counted once (%llu)",
          (unsigned long long)(counts().gate.overflow_messages - overflow));
    CHECK(log_lines_with("more than the MIDI wire takes", "said once") == 1,
          "and the helper's log says so once");
    reads = atomic_load(&P.wire_reads);
    send_raw(a, request, sizeof(request));
    CHECK(WAIT(atomic_load(&P.wire_reads) == reads + 1, 0.3),
          "with the queue full the control app's request still reaches the unit at once");

    a_changed = atomic_load(&a->changed);
    m_changed = atomic_load(&m->changed);
    closes = atomic_load(&P.closes);
    flushed = counts().gate.flushed_packets;
    unit_goes();
    CHECK(WAIT(atomic_load(&a->changed) == a_changed + 1 && atomic_load(&m->changed) == m_changed + 1,
               3.0),
          "unplugged: the control app and the MIDI client are both told");
    CHECK(atomic_load(&P.closes) == closes + 1 && !counts().port_open && counts().midi_clients == 0,
          "the interface is closed, for both");
    CHECK(counts().gate.flushed_packets > flushed,
          "and what was still waiting for MIDI OUT is thrown away (%llu packets)",
          (unsigned long long)(counts().gate.flushed_packets - flushed));
    wm = wire_midi();
    refused = counts().midi_refused;
    m_send(m, k_note_on, sizeof(k_note_on));
    CHECK(WAIT(counts().midi_refused == refused + 1, 3.0) && wire_midi() == wm,
          "a message a MIDI client sends meanwhile goes nowhere, and is counted");
    CHECK(m_attach(m) == UA101_LINK_ERR_ABSENT && atomic_load(&m->last_watching) == 1,
          "attaching says there is no unit, and that word will come when one appears");
    unit_comes();
    CHECK(WAIT(atomic_load(&a->changed) == a_changed + 2 && atomic_load(&m->changed) == m_changed + 2,
               3.0),
          "plugged in: both are told (app %d, MIDI client %d)", atomic_load(&a->changed) - a_changed,
          atomic_load(&m->changed) - m_changed);
    CHECK(m_attach(m) == UA101_LINK_OK && attach(a) == UA101_LINK_OK, "both attach again");
    CHECK(WAIT(APP(panel_state(a->engine)) == PANEL_STATE_READY, 3.0), "settings read again");
    m_send(m, k_note_on, sizeof(k_note_on));
    CHECK(WAIT(wire_midi() == wm + 3, 3.0) && wire_midi_ends(k_note_on, sizeof(k_note_on)),
          "and a note passes: the SysEx that was cut off is forgotten");

    a_changed = atomic_load(&a->changed);
    m_changed = atomic_load(&m->changed);
    closes = atomic_load(&P.closes);
    on_service_queue(^{
      W.events.will_sleep();
    });
    CHECK(atomic_load(&P.closes) == closes + 1 && !counts().port_open && counts().midi_clients == 0,
          "the Mac sleeps: the interface is given back first, for both");
    CHECK(atomic_load(&m->changed) == m_changed, "the MIDI client isn't told until the Mac wakes");
    CHECK(m_attach(m) == UA101_LINK_ERR_ASLEEP && !counts().port_open,
          "attaching meanwhile is answered \"asleep\" and opens nothing");
    on_service_queue(^{
      W.events.did_wake();
    });
    CHECK(WAIT(atomic_load(&a->changed) == a_changed + 1 && atomic_load(&m->changed) == m_changed + 1,
               3.0),
          "the Mac woke: both are told (app %d, MIDI client %d)", atomic_load(&a->changed) - a_changed,
          atomic_load(&m->changed) - m_changed);
    CHECK(m_attach(m) == UA101_LINK_OK && attach(a) == UA101_LINK_OK, "both attach again");

    a_changed = atomic_load(&a->changed);
    m_changed = atomic_load(&m->changed);
    closes = atomic_load(&P.closes);
    on_service_queue(^{
      P.events.lost(NULL, "the pipe stalled (pretend)");
    });
    CHECK(WAIT(atomic_load(&a->changed) == a_changed + 1 && atomic_load(&m->changed) == m_changed + 1,
               3.0),
          "the interface failed: both are told");
    CHECK(atomic_load(&P.closes) == closes + 1 && !counts().port_open && counts().midi_clients == 0,
          "and it is closed");
    CHECK(m_attach(m) == UA101_LINK_OK && attach(a) == UA101_LINK_OK, "both attach again");
    CHECK(WAIT(APP(panel_state(a->engine)) == PANEL_STATE_READY, 3.0), "settings read again");
}

/* The interface is held while a control app or a MIDI client is attached, whichever. */
static void test_midi_held(app *a, mclient *m)
{
    static const uint8_t in[] = {0x09, 0x90, 0x45, 0x50};
    int closes = atomic_load(&P.closes), opens = atomic_load(&P.opens), wm = wire_midi(), got;

    on_app(^{
      ua101_ctllink_detach(a->link);
    });
    usleep(100000);
    CHECK(atomic_load(&P.closes) == closes && counts().port_open,
          "the control app let go: the interface stays open for the MIDI client");
    m_send(m, k_note_on, sizeof(k_note_on));
    CHECK(WAIT(wire_midi() == wm + 3, 3.0), "whose notes still pass");
    m_detach(m);
    CHECK(WAIT(atomic_load(&P.closes) == closes + 1 && !counts().port_open, 3.0),
          "the MIDI client let go too: the interface is given back");
    CHECK(log_lines_with("MIDI clients:", "handed over for MIDI OUT") >= 1,
          "and the helper's log has a line of what passed and what didn't");

    CHECK(m_attach(m) == UA101_LINK_OK && atomic_load(&P.opens) == opens + 1 && counts().port_open,
          "a MIDI client alone opens the interface");
    m_send(m, k_note_off, sizeof(k_note_off));
    CHECK(WAIT(wire_midi() == wm + 6, 3.0) && wire_midi_ends(k_note_off, sizeof(k_note_off)),
          "its notes pass with no control app there");
    got = atomic_load(&m->got_bytes);
    unit_sends(in, sizeof(in), 5);
    CHECK(WAIT(atomic_load(&m->got_bytes) == got + 3, 3.0), "and it gets what arrives at MIDI IN");

    on_app(^{
      a->up = 0;
      panel_transport(a->engine, 0, now_us());
    });
    CHECK(attach(a) == UA101_LINK_OK && atomic_load(&P.opens) == opens + 1,
          "the control app attaches to the interface already open");
    CHECK(WAIT(APP(panel_state(a->engine)) == PANEL_STATE_READY, 3.0), "and reads the settings");
    m_detach(m);
    usleep(100000);
    CHECK(atomic_load(&P.closes) == closes + 1 && counts().port_open && counts().midi_clients == 0,
          "the MIDI client let go: the interface stays open for the control app");
    CHECK(m_attach(m) == UA101_LINK_OK, "attached again");
}

/* Two MIDI clients: both hear MIDI IN; one's SysEx isn't broken by the other's messages; a
   client that goes away in the middle doesn't hold the other back. */
static void test_midi_two(mclient *m)
{
    static const uint8_t in[] = {0x09, 0x90, 0x48, 0x33};
    static const uint8_t start[] = {0xF0, 0x7D, 0x01, 0x02, 0x03, 0x04};
    mclient *m2 = m_new();
    uint8_t oversize[16] = {0};
    xpc_connection_t raw = plugin_connect();
    static uint8_t too_long[UA101_LINK_MIDI_SEND_MAX + 1];
    uint64_t busy = counts().gate.busy_messages, refused;
    int got = atomic_load(&m->got_bytes), got2, wm;

    CHECK(m_attach(m2) == UA101_LINK_OK && counts().midi_clients == 2, "a second MIDI client attaches");
    got2 = atomic_load(&m2->got_bytes);
    unit_sends(in, sizeof(in), 7);
    CHECK(WAIT(atomic_load(&m->got_bytes) == got + 3 && atomic_load(&m2->got_bytes) == got2 + 3, 3.0),
          "both get what arrives at MIDI IN");

    wm = wire_midi();
    m_send(m2, start, sizeof(start));
    CHECK(WAIT(wire_midi() == wm + 6, 3.0), "the second begins a SysEx");
    m_send(m, k_note_on, sizeof(k_note_on));
    CHECK(WAIT(counts().gate.busy_messages == busy + 1, 3.0) && wire_midi() == wm + 6,
          "the first one's note meanwhile is dropped and counted, not put into the SysEx");
    m_free(m2);
    CHECK(WAIT(counts().midi_clients == 1, 3.0), "the second goes away with its SysEx half sent");
    m_send(m, k_note_on, sizeof(k_note_on));
    CHECK(WAIT(wire_midi() == wm + 9, 3.0) && wire_midi_ends(k_note_on, sizeof(k_note_on)),
          "the first one's next note passes");
    CHECK(log_lines_with("a MIDI client disconnected", NULL) >= 1, "the log names it a MIDI client");

    /* By hand: a client that isn't attached, and a message longer than one may be. */
    refused = counts().midi_refused;
    CHECK(raw_midi_send(raw, oversize, sizeof(oversize)) == UA101_LINK_ERR_ABSENT,
          "midi_send from a client that never attached is refused");
    CHECK(plugin_ask(raw, "midi_open", UA101_LINK_VERSION) == UA101_LINK_OK, "it attaches");
    CHECK(raw_midi_send(raw, too_long, sizeof(too_long)) == UA101_LINK_ERR_REQUEST,
          "more than %d bytes in one midi_send is refused", UA101_LINK_MIDI_SEND_MAX);
    CHECK(counts().midi_refused == refused + 2 && wire_midi() == wm + 9, "both counted, nothing sent");
    xpc_connection_cancel(raw);
    xpc_release(raw);
    CHECK(WAIT(counts().midi_clients == 1, 3.0), "and it is gone again");
}

/* The unit doesn't take a write for MIDI OUT: nothing more is tried for a second, and the
   control app is served meanwhile. */
static void test_midi_stall(app *a, mclient *m)
{
    uint8_t request[UA101_CTL_READ_REQUEST_BYTES];
    uint64_t write_failures = counts().midi_write_failures;
    int wm = wire_midi(), reads, stalled = atomic_load(&P.midi_stalled_writes), i, lamp_out;

    ua101_ctl_read_request(request);
    usleep(1100000); /* a new second for the control side's count, and no hold left over */
    lamp_out = atomic_load(&a->lamp_out);
    atomic_store(&P.midi_stall, 1);
    m_send(m, k_note_on, sizeof(k_note_on));
    CHECK(WAIT(counts().midi_write_failures == write_failures + 1, 3.0),
          "the unit doesn't take a write for MIDI OUT: counted");
    for (i = 0; i < 5; i++)
        m_send(m, k_note_on, sizeof(k_note_on));
    reads = atomic_load(&P.wire_reads);
    send_raw(a, request, sizeof(request));
    CHECK(WAIT(atomic_load(&P.wire_reads) == reads + 1, 0.5),
          "the control app's request reaches the unit all the same");
    usleep(200000);
    CHECK(atomic_load(&P.midi_stalled_writes) == stalled + 1 && wire_midi() == wm,
          "for a second no further write for MIDI OUT is tried (%d)",
          atomic_load(&P.midi_stalled_writes) - stalled);
    CHECK(atomic_load(&a->lamp_out) == lamp_out,
          "notes the unit didn't take leave the control app's OUT lamp dark (link version 15)");
    atomic_store(&P.midi_stall, 0);
    usleep(1000000);
    m_send(m, k_note_off, sizeof(k_note_off));
    CHECK(WAIT(wire_midi() == wm + 3, 3.0) && wire_midi_ends(k_note_off, sizeof(k_note_off)),
          "after it MIDI OUT works again, and what was dropped stays dropped");
    CHECK(WAIT(atomic_load(&a->lamp_out) == lamp_out + 1, 0.5), "the note off that left lights OUT");
    CHECK(log_lines_with("didn't take", "said once") == 1, "the helper's log says it once");
    CHECK(WAIT(APP(panel_state(a->engine)) == PANEL_STATE_READY, 3.0), "the control app is in step");
}


/* No "midi_lamps" event and no wake-up booked for `seconds`: MIDI is quiet, so nothing runs. */
static int lamps_quiet(heard *h, double seconds)
{
    int before = atomic_load(&h->lamps);
    uint64_t wakeups = counts().midi_lamp_wakeups;

    usleep((useconds_t)(seconds * 1e6));
    return atomic_load(&h->lamps) == before && counts().midi_lamp_wakeups == wakeups &&
           !counts().midi_lamp_timer_on;
}

/* Waits out the gap after earlier MIDI, and for the one wake-up after it, if any. */
static void lamps_settle(void)
{
    usleep(150000);
    CHECK(WAIT(!counts().midi_lamp_timer_on, 1.0), "the lamps' wake-up is over");
}

/*
 * Link version 15: the control app's MIDI IN and MIDI OUT lamps (src/ua101_midilamp.h). The
 * helper tells each attached control app when MIDI arrived at MIDI IN or really left through
 * MIDI OUT: at most 10 times a second, never while MIDI is quiet, never for active sensing
 * alone, never for bytes the gate dropped or the unit didn't take; and while MIDI is quiet no
 * wake-up stays booked.
 */
static void test_midi_lamps(app *a, mclient *m)
{
    static const uint8_t note[] = {0x09, 0x90, 0x3C, 0x40};
    static const uint8_t sensing[] = {0x0F, 0xFE, 0x00, 0x00};
    static const uint8_t control_only[] = {0x14, 0xF0, 0x41, 0x10};
    static const uint8_t pause[13] = {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00,
                                      0x00, 0x00, 0x00, 0x00, 0x00, 0xF7};
    static const uint8_t sense_byte[] = {0xFE};
    static heard h;
    xpc_connection_t raw = raw_listener(&h);
    int n, i, wm, app_events, app_in, app_out, most_in_a_second, in_before, out_before;
    double t0, gap, smallest_gap = 1e9;
    uint64_t events;

    CHECK(plugin_ask(raw, "ctl_open", UA101_LINK_VERSION) == UA101_LINK_OK,
          "a listener by hand attaches as a control app");
    lamps_settle();

    /* Quiet: nothing is told and nothing is booked. (The pretend unit puts a note at MIDI IN
       between the reads of each answer to the app's engine, and the engine reads again 0.5 s
       after the unit's own change in test_midi_in: that is let pass first.) */
    usleep(800000);
    CHECK(WAIT(APP(panel_state(a->engine)) == PANEL_STATE_READY, 3.0), "the control app is in step");
    lamps_settle();
    CHECK(lamps_quiet(&h, 0.5), "with MIDI quiet for 0.5 s: no lamp event, no wake-up, no timer");

    /* One note at MIDI IN: told at once, as IN only. */
    app_events = atomic_load(&a->lamp_events);
    app_in = atomic_load(&a->lamp_in);
    n = atomic_load(&h.lamps);
    in_before = atomic_load(&h.lamp_in);
    out_before = atomic_load(&h.lamp_out);
    t0 = now_s();
    unit_sends(note, sizeof(note), 1);
    CHECK(WAIT(atomic_load(&h.lamps) == n + 1, 0.5) && atomic_load(&h.lamp_in) == in_before + 1 &&
              atomic_load(&h.lamp_out) == out_before,
          "one note at MIDI IN: one event, IN lit, OUT not (%d events, %d in, %d out)",
          atomic_load(&h.lamps) - n, atomic_load(&h.lamp_in) - in_before,
          atomic_load(&h.lamp_out) - out_before);
    CHECK(atomic_load(&h.lamps) > n && h.lamp_at[n] - t0 < 0.05,
          "told at once, not after the gap (%.3f s)", h.lamp_at[n] - t0);
    CHECK(WAIT(atomic_load(&a->lamp_events) == app_events + 1, 0.5) &&
              atomic_load(&a->lamp_in) == app_in + 1,
          "the control app's own link (src/ua101_ctllink.c) hands it on as IN");
    CHECK(!counts().midi_lamp_timer_on, "a single note books no wake-up");
    CHECK(lamps_quiet(&h, 0.4), "and after it nothing more comes and nothing runs");

    /* Active sensing alone, and the control cable alone: neither is MIDI to show. */
    n = atomic_load(&h.lamps);
    for (i = 0; i < 10; i++) {
        unit_sends(sensing, sizeof(sensing), 1);
        unit_sends(control_only, sizeof(control_only), 1);
        usleep(30000);
    }
    CHECK(atomic_load(&h.lamps) == n && lamps_quiet(&h, 0.2),
          "active sensing at MIDI IN, and control-cable packets, light nothing and book nothing");

    /* A flood at MIDI IN: a note every 2 ms for a second. */
    lamps_settle();
    n = atomic_load(&h.lamps);
    events = counts().midi_lamp_events;
    t0 = now_s();
    while (now_s() - t0 < 1.0) {
        unit_sends(note, sizeof(note), 1);
        usleep(2000);
    }
    CHECK(WAIT(!counts().midi_lamp_timer_on, 0.3),
          "within 0.3 s of the flood's end no wake-up is booked any more");
    usleep(50000);
    for (i = n + 1; i < atomic_load(&h.lamps); i++) {
        gap = h.lamp_at[i] - h.lamp_at[i - 1];
        if (gap < smallest_gap)
            smallest_gap = gap;
    }
    most_in_a_second = 0;
    for (i = n; i < atomic_load(&h.lamps); i++) {
        int j, within = 0;

        for (j = i; j < atomic_load(&h.lamps) && h.lamp_at[j] - h.lamp_at[i] < 1.0; j++)
            within++;
        if (within > most_in_a_second)
            most_in_a_second = within;
    }
    printf("   note  a second of notes every 2 ms at MIDI IN: %d lamp events (%llu told), at "
           "most %d in any second, the smallest gap %.3f s\n",
           atomic_load(&h.lamps) - n, (unsigned long long)(counts().midi_lamp_events - events),
           most_in_a_second, smallest_gap);
    CHECK(most_in_a_second <= 11 && atomic_load(&h.lamps) - n <= 12,
          "a flood is told at most about 10 times a second (%d in a second, %d in all)",
          most_in_a_second, atomic_load(&h.lamps) - n);
    CHECK(atomic_load(&h.lamps) - n >= 8, "and keeps being told while it lasts (%d)",
          atomic_load(&h.lamps) - n);
    CHECK(smallest_gap >= 0.08, "no two events closer than the gap (%.3f s)", smallest_gap);
    CHECK(lamps_quiet(&h, 0.5), "after the flood: nothing more, no wake-up, no timer");

    /* MIDI OUT: a note that leaves lights OUT. */
    lamps_settle();
    n = atomic_load(&h.lamps);
    out_before = atomic_load(&h.lamp_out);
    app_out = atomic_load(&a->lamp_out);
    wm = wire_midi();
    m_send(m, k_note_on, sizeof(k_note_on));
    CHECK(WAIT(wire_midi() == wm + 3, 1.0), "a note from a MIDI client leaves by MIDI OUT");
    CHECK(WAIT(atomic_load(&h.lamps) == n + 1, 0.5) && atomic_load(&h.lamp_out) == out_before + 1,
          "and lights OUT (%d events)", atomic_load(&h.lamps) - n);
    CHECK(WAIT(atomic_load(&a->lamp_out) == app_out + 1, 0.5), "the control app hears OUT");
    m_send(m, k_note_off, sizeof(k_note_off));
    CHECK(WAIT(wire_midi() == wm + 6, 1.0), "the note off leaves too");
    lamps_settle();

    /* What the gate drops lights nothing: SysEx addressed to the UA-101, and active sensing
       alone (which does leave). */
    n = atomic_load(&h.lamps);
    wm = wire_midi();
    m_send(m, pause, sizeof(pause));
    usleep(150000);
    CHECK(wire_midi() == wm && atomic_load(&h.lamps) == n,
          "SysEx addressed to the UA-101, dropped by the gate: nothing leaves and OUT stays dark");
    m_send(m, sense_byte, sizeof(sense_byte));
    CHECK(WAIT(wire_midi() == wm + 1, 1.0), "active sensing from an app leaves by MIDI OUT");
    CHECK(lamps_quiet(&h, 0.2) && atomic_load(&h.lamps) == n, "but lights nothing");

    /* What the unit didn't take lights nothing: in test_midi_stall. */

    /* With the control apps gone, nothing is told or booked. */
    n = atomic_load(&h.lamps);
    xpc_connection_cancel(raw);
    xpc_release(raw);
    on_app(^{
      ua101_ctllink_detach(a->link);
    });
    usleep(100000);
    events = counts().midi_lamp_events;
    for (i = 0; i < 20; i++) {
        unit_sends(note, sizeof(note), 1);
        usleep(5000);
    }
    usleep(50000);
    CHECK(counts().midi_lamp_events == events && !counts().midi_lamp_timer_on,
          "with no control app attached, MIDI at MIDI IN is told to nobody and books nothing");
    on_app(^{
      a->up = 0;
      panel_transport(a->engine, 0, now_us());
    });
    CHECK(attach(a) == UA101_LINK_OK, "the control app attaches again");
    CHECK(WAIT(APP(panel_state(a->engine)) == PANEL_STATE_READY, 3.0), "and is in step");
    lamps_settle();
}

static void test_midi(app *a)
{
    mclient *m = m_new();

    test_midi_in(a, m);
    test_midi_lamps(a, m);
    test_midi_out(m);
    test_midi_pacing(a, m);
    test_midi_coming_and_going(a, m);
    test_midi_held(a, m);
    test_midi_two(m);
    test_midi_stall(a, m);
    m_free(m);
    CHECK(WAIT(counts().midi_clients == 0, 3.0) && counts().port_open,
          "the MIDI client is gone; the interface stays open for the control app");
    CHECK(counts().gate.gate_wrong_cable == 0 && counts().gate.gate_own_sysex == 0 &&
              counts().gate.gate_malformed == 0,
          "in the whole run the last check before the port had nothing to stop");
}

/* --- Load, Store and Reset (link version 17). --- */

/* A moment at which a Load, Store or Reset passes: no sound, the settings
   read 5 s ago, after the last change (10 s ago) and the last memory command (20 s ago). */
static ua101_service_moment good_moment(void)
{
    ua101_service_moment at;

    memset(&at, 0, sizeof(at));
    at.now_us = 100000000u;
    at.block_us = 95000000u;
    at.change_us = 90000000u;
    at.memory_us = 80000000u;
    return at;
}

/* The rules for Load, Store and Reset by themselves, with no service running and no waiting:
   which messages, and at which moments. */
static void test_memory_rules(void)
{
    const ua101_service_moment good = good_moment();
    uint8_t bytes[UA101_CTLMSG_MAX_BYTES], v[16];
    const char *why = "";
    unsigned n, ok, count;
    size_t len;

    /* The 37, each from the app's builder: every one passes at a good moment, is one the
       test's own rule names, and is refused by the function for ordinary changes. */
    for (n = ok = 0; n < T_MEMORY_MESSAGES; n++) {
        len = memory_nth(n, bytes);
        ok += len != 0 && is_passed_memory(bytes, len) &&
              ua101_service_memory_passes(bytes, len, &good, &why) && why[0] == '\0' &&
              !ua101_service_passes(bytes, len, &why) && panel_memory_message_bit(bytes, len) != 0;
    }
    CHECK(ok == T_MEMORY_MESSAGES,
          "all 37 Load, Store and Reset messages pass at a good moment, and the app's own rule "
          "knows each (%u of 37)", ok);
    /* No two of them are the same, and each has a bit of its own kind. */
    {
        uint8_t other[UA101_CTLMSG_MAX_BYTES];
        unsigned m, same = 0;
        uint64_t bits = 0;

        for (n = 0; n < T_MEMORY_MESSAGES; n++) {
            len = memory_nth(n, bytes);
            bits |= panel_memory_message_bit(bytes, len);
            for (m = n + 1; m < T_MEMORY_MESSAGES; m++)
                same += memory_nth(m, other) == len && memcmp(bytes, other, len) == 0;
        }
        CHECK(same == 0 && bits == UA101_MEMORY_PASSED && UA101_CTL_PASSES_MEMORY == 0x31F1F1Full &&
                  (UA101_CTL_PASSES & UA101_CTL_PASS_MEMORY) != 0 && panel_helper_passes_memory() == bits,
              "they are 37 different messages, and their 17 commands are the ones the helper "
              "says it passes on (0x%llx)", (unsigned long long)bits);
    }
    /* Some of them written out by hand, as docs/control-protocol.md gives them. */
    {
        static const uint8_t load3[16] = {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x00,
                                          0x05, 0x08, 0x00, 0x00, 0x01, 0x01, 0x71, 0xF7};
        static const uint8_t store5_mixer[16] = {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x00,
                                                 0x07, 0x04, 0x00, 0x00, 0x00, 0x02, 0x73, 0xF7};
        static const uint8_t reset_current[13] = {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00,
                                                  0x00, 0x00, 0x03, 0x00, 0x7D, 0xF7};
        static const uint8_t reset5[13] = {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00,
                                           0x00, 0x07, 0x03, 0x00, 0x76, 0xF7};
        static const uint8_t reset_all[13] = {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00,
                                              0x00, 0x00, 0x02, 0x00, 0x7E, 0xF7};
        static const uint8_t both[4] = {0, 0, 1, 1}, mixer[4] = {0, 0, 0, 1};

        CHECK(memory_message(UA101_MSG_LOAD, 2, both, bytes) == 16 && memcmp(bytes, load3, 16) == 0 &&
                  ua101_service_memory_passes(load3, 16, &good, &why),
              "Load Memory 3, patch bay and mixer, is F0 41 10 00 69 12 00 00 05 08 00 00 01 01 71 F7");
        CHECK(memory_message(UA101_MSG_STORE, 4, mixer, bytes) == 16 && memcmp(bytes, store5_mixer, 16) == 0 &&
                  ua101_service_memory_passes(store5_mixer, 16, &good, &why),
              "Store into Memory 5, mixer only, is ... 07 04 00 00 00 02 73 F7");
        CHECK(memory_message(UA101_MSG_RESET_CURRENT, 0, NULL, bytes) == 13 && memcmp(bytes, reset_current, 13) == 0 &&
                  ua101_service_memory_passes(reset_current, 13, &good, &why),
              "Reset of the settings in use is ... 00 00 00 03 00 7D F7");
        CHECK(memory_message(UA101_MSG_RESET_MEMORY, 4, NULL, bytes) == 13 && memcmp(bytes, reset5, 13) == 0 &&
                  ua101_service_memory_passes(reset5, 13, &good, &why),
              "Reset of Memory 5 is ... 00 00 07 03 00 76 F7");
        CHECK(memory_message(UA101_MSG_RESET_ALL, 0, NULL, bytes) == 13 && memcmp(bytes, reset_all, 13) == 0 &&
                  ua101_service_memory_passes(reset_all, 13, &good, &why),
              "Reset of everything is ... 00 00 00 02 00 7E F7");
    }

    /* By brute force, every 16-byte message at 00 00 <a2> <a3> with each of the four bytes 0
       to 3 and a right checksum: only the 30 Loads and Stores pass. */
    {
        unsigned a2, a3, d, wrong = 0;

        ok = count = 0;
        for (a2 = 0; a2 < 12; a2++)
            for (a3 = 0; a3 < 16; a3++)
                for (d = 0; d < 256; d++) {
                    const uint8_t four[4] = {(uint8_t)(d & 3u), (uint8_t)((d >> 2) & 3u),
                                             (uint8_t)((d >> 4) & 3u), (uint8_t)((d >> 6) & 3u)};
                    int passes;

                    hand_memory(v, a2, a3, four);
                    passes = ua101_service_memory_passes(v, 16, &good, &why);
                    count++;
                    ok += (unsigned)passes;
                    wrong += passes != is_passed_memory(v, 16) ||
                             (panel_memory_message_bit(v, 16) != 0) != passes ||
                             ua101_service_passes(v, 16, NULL);
                }
        CHECK(ok == 30 && wrong == 0,
              "of %u 16-byte messages around Load and Store, exactly 30 pass, the ones the test "
              "names, and the app's own rule agrees (%u pass, %u wrong)", count, ok, wrong);
    }
    /* And every 13-byte message at 00 00 <a2> <a3> with every payload byte: only the 7 Resets
       pass here ("pause saving" and "resume saving" are ordinary changes). */
    {
        unsigned a2, a3, value, wrong = 0;

        ok = count = 0;
        for (a2 = 0; a2 < 128; a2++)
            for (a3 = 0; a3 < 128; a3++)
                for (value = 0; value < 128; value += (a2 < 9 && a3 < 9) ? 1 : 31) {
                    int passes;

                    hand_on(v, 0x00, a2, a3, value);
                    passes = ua101_service_memory_passes(v, 13, &good, &why);
                    count++;
                    ok += (unsigned)passes;
                    wrong += passes != is_passed_memory(v, 13) ||
                             (panel_memory_message_bit(v, 13) != 0) != passes;
                }
        CHECK(ok == 7 && wrong == 0,
              "of %u 13-byte messages around Reset, exactly 7 pass, the ones the test names (%u "
              "pass, %u wrong)", count, ok, wrong);
    }
    /* Each of the 37 with any one byte changed to any value: nothing new passes. */
    {
        unsigned at, b, wrong = 0;

        for (n = 0; n < T_MEMORY_MESSAGES; n++) {
            len = memory_nth(n, bytes);
            for (at = 0; at < len; at++)
                for (b = 0; b < 256; b++) {
                    memcpy(v, bytes, len);
                    v[at] = (uint8_t)b;
                    wrong += ua101_service_memory_passes(v, len, &good, &why) != is_passed_memory(v, len);
                }
            /* One byte more or less: never. */
            wrong += ua101_service_memory_passes(bytes, len - 1, &good, &why) != 0;
            memcpy(v, bytes, len);
            wrong += len < sizeof(v) && ua101_service_memory_passes(v, len + 1, &good, &why) != 0;
        }
        CHECK(wrong == 0, "any one byte of any of the 37 changed, or one byte more or less: "
                          "passes only if it is another of the 37 (%u wrong)", wrong);
    }
    /* Their neighbours by name. */
    {
        static const uint8_t switches[4] = {1, 0, 1, 1}, row0[4] = {0, 1, 0, 1};
        ua101_msg m;

        memset(&m, 0, sizeof(m));
        m.kind = UA101_MSG_PRESET;
        m.value = 4;
        len = ua101_ctlmsg_build(&m, bytes);
        CHECK(len == 16 && !ua101_service_memory_passes(bytes, len, &good, &why) &&
                  strstr(why, "preset") != NULL && strstr(why, "not sent") != NULL,
              "Roland's one-message Hard Thru: never, and the reason names it (%s)", why);
        m.value = 3;
        len = ua101_ctlmsg_build(&m, bytes);
        CHECK(len == 16 && !ua101_service_memory_passes(bytes, len, &good, &why) && strstr(why, "preset") != NULL,
              "nor its Initial Setting");
        len = memory_message(UA101_MSG_LOAD, 0, switches, bytes);
        CHECK(len == 16 && ua101_ctlmsg_risk(bytes, len) == UA101_RISK_MEMORY &&
                  !ua101_service_memory_passes(bytes, len, &good, &why) && strstr(why, "37 exact messages") != NULL,
              "a Load that also names the switches' part: refused, the reason says what passes (%s)", why);
        len = memory_message(UA101_MSG_STORE, 0, row0, bytes);
        CHECK(len == 16 && !ua101_service_memory_passes(bytes, len, &good, &why) && strstr(why, "37 exact messages") != NULL,
              "a Store that names patch bay row 0: refused");
        panel_saving_message(0, bytes);
        CHECK(!ua101_service_memory_passes(bytes, 13, &good, &why) && strstr(why, "not a Load") != NULL,
              "\"pause saving\" is not this function's to pass");
        ua101_ctl_read_request(bytes);
        CHECK(!ua101_service_memory_passes(bytes, UA101_CTL_READ_REQUEST_BYTES, &good, &why),
              "nor is the settings request");
        len = memory_nth(0, bytes);
        CHECK(!ua101_service_memory_passes(NULL, len, &good, &why) &&
                  !ua101_service_memory_passes(bytes, len, NULL, &why) &&
                  !ua101_service_memory_passes(bytes, 0, &good, &why),
              "no message, no moment, no length: refused");
        bytes[len - 2] ^= 0x01;
        CHECK(!ua101_service_memory_passes(bytes, len, &good, &why), "a wrong checksum: refused");
    }

    /* The moment. Each rule by itself, with all the others met, for every one of the 37. */
    {
        struct {
            const char *what, *says;
            ua101_service_moment at;
            int want;
        } cases[32];
        unsigned c = 0, k, wrong;

#define MOMENT(text, words, expect, change)                                                        \
    do {                                                                                           \
        cases[c].what = text;                                                                      \
        cases[c].says = words;                                                                     \
        cases[c].want = expect;                                                                    \
        cases[c].at = good;                                                                        \
        { ua101_service_moment *at = &cases[c].at; change; }                                       \
        c++;                                                                                       \
    } while (0)
        MOMENT("sound is streaming", "sound is being played or recorded", 0, at->streaming = 1);
        MOMENT("the one before was 4.999999 s ago", "less than 5 seconds ago", 0,
               at->memory_us = at->now_us - 4999999u; at->block_us = at->now_us - 1u);
        MOMENT("the one before was exactly 5 s ago", "", 1,
               at->memory_us = at->now_us - 5000000u; at->block_us = at->now_us - 1u);
        MOMENT("the one before is later than now (the clock went back)", "less than 5 seconds ago", 0,
               at->memory_us = at->now_us + 1u);
        MOMENT("there never was one before", "", 1, at->memory_us = 0);
        MOMENT("the settings were never read", "weren't read in the 30 seconds before", 0, at->block_us = 0);
        MOMENT("the settings were read 30.000001 s ago", "weren't read in the 30 seconds before", 0,
               at->block_us = at->now_us - 30000001u; at->change_us = 0; at->memory_us = 0);
        MOMENT("the settings were read exactly 30 s ago", "", 1,
               at->block_us = at->now_us - 30000000u; at->change_us = 0; at->memory_us = 0);
        MOMENT("the read is later than now (the clock went back)", "weren't read in the 30 seconds before", 0,
               at->block_us = at->now_us + 1u);
        MOMENT("a change was passed on after the read", "weren't read again after the last change", 0,
               at->change_us = at->block_us + 1u);
        MOMENT("a change was passed on in the very microsecond of the read", "weren't read again after the last change", 0,
               at->change_us = at->block_us);
        MOMENT("the last change was a microsecond before the read", "", 1, at->change_us = at->block_us - 1u);
        MOMENT("there never was a change", "", 1, at->change_us = 0);
        MOMENT("the read is older than the Load, Store or Reset before (6 s ago)", "weren't read again after the last change", 0,
               at->memory_us = at->now_us - 6000000u; at->block_us = at->now_us - 7000000u);
        MOMENT("the read came in the very microsecond of the one before", "weren't read again after the last change", 0,
               at->memory_us = at->now_us - 6000000u; at->block_us = at->memory_us);
        MOMENT("sound is streaming and nothing was read", "sound is being played or recorded", 0,
               at->streaming = 1; at->block_us = 0);
        MOMENT("three seconds after the Mac started, the settings read a second ago, nothing ever sent", "", 1,
               at->now_us = 3000000u; at->block_us = 2000000u; at->change_us = 0; at->memory_us = 0);
        MOMENT("twenty seconds after the Mac started, the settings never read", "weren't read in the 30 seconds before", 0,
               at->now_us = 20000000u; at->block_us = 0; at->change_us = 0; at->memory_us = 0);
#undef MOMENT
        for (k = 0; k < c; k++) {
            wrong = 0;
            for (n = 0; n < T_MEMORY_MESSAGES; n++) {
                len = memory_nth(n, bytes);
                wrong += ua101_service_memory_passes(bytes, len, &cases[k].at, &why) != cases[k].want ||
                         strstr(why, cases[k].says) == NULL ||
                         (!cases[k].want && strstr(why, "not sent") == NULL);
            }
            CHECK(wrong == 0, "%s: all 37 are %s%s%s (%u wrong; the last reason: %s)", cases[k].what,
                  cases[k].want ? "passed on" : "refused", cases[k].want ? "" : ", and the reason says: ",
                  cases[k].says, wrong, why);
        }
        CHECK(UA101_MEMORY_GAP_SECONDS == 5 && UA101_MEMORY_FRESH_SECONDS == 30,
              "the times are 5 seconds and 30 seconds");
    }

    /* Each of the helper's two checks alone (this test's build can leave one out): each must
       keep to the rule by itself. */
    {
        static const unsigned skip[2] = {32768u, 16384u};
        static const char *const alone[2] = {"the first check (the parser's kind, memory and parts)",
                                             "the second check (the bytes, whole)"};
        unsigned k;

        for (k = 0; k < 2; k++) {
            unsigned a2, a3, d, bad = 0, passed = 0;
            ua101_msg m;

            ua101_service_test_skip = skip[k];
            for (a2 = 0; a2 < 10; a2++)
                for (a3 = 0; a3 < 12; a3++) {
                    for (d = 0; d < 256; d++) {
                        const uint8_t four[4] = {(uint8_t)(d & 3u), (uint8_t)((d >> 2) & 3u),
                                                 (uint8_t)((d >> 4) & 3u), (uint8_t)((d >> 6) & 3u)};
                        int passes;

                        hand_memory(v, a2, a3, four);
                        passes = ua101_service_memory_passes(v, 16, &good, &why);
                        bad += passes != is_passed_memory(v, 16);
                        passed += (unsigned)passes;
                    }
                    for (d = 0; d < 4; d++) {
                        int passes;

                        hand_on(v, 0x00, a2, a3, d);
                        passes = ua101_service_memory_passes(v, 13, &good, &why);
                        bad += passes != is_passed_memory(v, 13);
                        passed += (unsigned)passes;
                    }
                }
            /* Past the parser: the check called directly, with a parse or bytes that lie. */
            len = memory_nth(2, bytes); /* Load Memory 1, both */
            memset(&m, 0, sizeof(m));
            if (k == 0) { /* the first check alone: any bytes, the parse says what it likes */
                m.kind = UA101_MSG_LOAD;
                m.sections[2] = m.sections[3] = 1;
                bad += ua101_service_test_memory_command(bytes, len, &m) != UA101_CTL_PASS_MEM_LOAD(0);
                m.memory = 5;
                bad += ua101_service_test_memory_command(bytes, len, &m) != 0; /* Memory 6 */
                m.memory = 0;
                m.sections[0] = 1;
                bad += ua101_service_test_memory_command(bytes, len, &m) != 0; /* the switches' part */
                m.sections[0] = 0;
                m.sections[1] = 1;
                bad += ua101_service_test_memory_command(bytes, len, &m) != 0; /* patch bay row 0 */
                m.sections[1] = 0;
                m.sections[2] = 3;
                bad += ua101_service_test_memory_command(bytes, len, &m) != 0; /* "section byte 3" */
                m.sections[2] = m.sections[3] = 0;
                bad += ua101_service_test_memory_command(bytes, len, &m) != 0; /* nothing chosen */
                m.sections[3] = 1;
                m.kind = UA101_MSG_STORE;
                m.memory = 4;
                bad += ua101_service_test_memory_command(bytes, len, &m) != UA101_CTL_PASS_MEM_STORE(4);
                m.kind = UA101_MSG_RESET_MEMORY;
                m.memory = 7;
                bad += ua101_service_test_memory_command(bytes, len, &m) != 0;
                m.kind = UA101_MSG_PRESET;
                m.memory = 0;
                bad += ua101_service_test_memory_command(bytes, len, &m) != 0;
                m.kind = UA101_MSG_FIELD;
                bad += ua101_service_test_memory_command(bytes, len, &m) != 0;
            } else { /* the second check alone: a parse that says "Load Memory 1", any bytes */
                static const uint8_t three[4] = {0, 0, 3, 1}, sw[4] = {1, 0, 1, 1};

                m.kind = UA101_MSG_LOAD;
                m.sections[2] = m.sections[3] = 1;
                bad += ua101_service_test_memory_command(bytes, len, &m) != UA101_CTL_PASS_MEM_LOAD(0);
                hand_memory(v, 0x08, 0x08, m.sections); /* Memory 6 */
                bad += ua101_service_test_memory_command(v, 16, &m) != 0;
                hand_memory(v, 0x03, 0x08, three); /* section byte 3 */
                bad += ua101_service_test_memory_command(v, 16, &m) != 0;
                hand_memory(v, 0x03, 0x08, sw); /* the switches' part */
                bad += ua101_service_test_memory_command(v, 16, &m) != 0;
                hand_memory(v, 0x00, 0x08, m.sections); /* no memory at all */
                bad += ua101_service_test_memory_command(v, 16, &m) != 0;
                hand_on(v, 0x00, 0x08, 0x03, 0); /* Reset of "Memory 6" */
                bad += ua101_service_test_memory_command(v, 13, &m) != 0;
                hand_on(v, 0x00, 0x00, 0x01, 0); /* 00 00 00 01: unknown, between saving and reset all */
                bad += ua101_service_test_memory_command(v, 13, &m) != 0;
                hand_on(v, 0x00, 0x00, 0x03, 1); /* reset of the settings in use, payload 01 */
                bad += ua101_service_test_memory_command(v, 13, &m) != 0;
            }
            ua101_service_test_skip = 0;
            CHECK(bad == 0 && passed == T_MEMORY_MESSAGES,
                  "Load, Store and Reset, %s alone: only the 37 pass, also past the parser (%u wrong, %u passed)",
                  alone[k], bad, passed);
        }
    }
    /* Both checks at work, past the parser: they must also name the same command. */
    {
        static const uint8_t both[4] = {0, 0, 1, 1};
        ua101_msg m;
        unsigned bad = 0;

        len = memory_message(UA101_MSG_LOAD, 0, both, bytes);
        memset(&m, 0, sizeof(m));
        m.kind = UA101_MSG_LOAD;
        m.sections[2] = m.sections[3] = 1;
        bad += ua101_service_test_memory_command(bytes, len, &m) != UA101_CTL_PASS_MEM_LOAD(0);
        m.memory = 1; /* the bytes say Memory 1, the parse Memory 2 */
        bad += ua101_service_test_memory_command(bytes, len, &m) != 0;
        m.memory = 0;
        m.kind = UA101_MSG_STORE; /* the bytes say Load, the parse Store */
        bad += ua101_service_test_memory_command(bytes, len, &m) != 0;
        m.kind = UA101_MSG_LOAD;
        m.sections[0] = 1; /* the parse names the switches' part, the bytes don't */
        bad += ua101_service_test_memory_command(bytes, len, &m) != 0;
        m.sections[0] = 0;
        bytes[10] = 1; /* and the other way round */
        bytes[14] = ua101_ctl_checksum(bytes + 6, 8);
        bad += ua101_service_test_memory_command(bytes, len, &m) != 0;
        CHECK(bad == 0, "with both checks at work a message passes only if both name it, and the same command (%u wrong)", bad);
    }
    /* The list itself: made shorter for a moment (this test's build only), to Load Memory 3
       and Reset of everything. Both checks together, and each alone, then pass those four
       messages only; the others are refused with the sentence that names what is passed on. */
    {
        static const unsigned skip[3] = {0u, 32768u, 16384u};
        unsigned k, bad = 0, passed = 0;

        ua101_service_test_memory = UA101_CTL_PASS_MEM_LOAD(2) | UA101_CTL_PASS_MEM_RESET_ALL;
        for (k = 0; k < 3; k++) {
            ua101_service_test_skip = skip[k];
            for (n = 0; n < T_MEMORY_MESSAGES; n++) {
                int want = (n >= 6 && n < 9) || n == 36, passes;

                len = memory_nth(n, bytes);
                passes = ua101_service_memory_passes(bytes, len, &good, &why);
                bad += passes != want;
                passed += (unsigned)passes;
            }
        }
        ua101_service_test_skip = 0;
        len = memory_nth(35, bytes);
        CHECK(!ua101_service_memory_passes(bytes, len, &good, &why) && strstr(why, "37 exact messages") != NULL,
              "Reset of the settings in use, on no list for a moment: refused (%s)", why);
        ua101_service_test_memory = UA101_MEMORY_PASSED;
        CHECK(bad == 0 && passed == 3 * 4,
              "with the list made shorter for a moment (Load Memory 3, Reset of everything), both "
              "checks together and each alone pass those four messages only (%u wrong)", bad);
        CHECK(ua101_service_memory_passes(bytes, len, &good, &why), "with the whole list back, it passes again");
    }
}

/* Moves the helper's clock for the memory rules forward (this test's build only). */
static void helper_clock_forward(double seconds)
{
    on_service_queue(^{
      ua101_service_test_clock_us += (uint64_t)(seconds * 1e6);
    });
}

/* Asks the unit for its settings through `a`'s link, by hand, and waits for the block. */
static int read_settings(app *a)
{
    uint8_t request[UA101_CTL_READ_REQUEST_BYTES];
    int before = atomic_load(&a->messages);

    ua101_ctl_read_request(request);
    pace();
    send_raw(a, request, sizeof(request));
    return WAIT(atomic_load(&a->messages) > before, 3.0);
}

/* Sends one message and says whether it reached the cable as one of the 37. */
static int memory_reaches(app *a, const uint8_t *bytes, size_t len)
{
    int wire = atomic_load(&P.wire_memory), refused = atomic_load(&a->refused);

    pace();
    send_raw(a, bytes, len);
    WAIT(atomic_load(&P.wire_memory) > wire || atomic_load(&a->refused) > refused, 3.0);
    return atomic_load(&P.wire_memory) > wire;
}

static uint64_t sim_memory_commands(void)
{
    __block uint64_t n;

    on_service_queue(^{
      n = P.sim.memory_commands;
    });
    return n;
}

/* The last line of the helper's log that holds `a`, copied; "" if none. */
static const char *log_last_line_with(const char *a)
{
    static char found[1400];
    char line[1400];
    FILE *f = fopen(g_log_path, "r");

    found[0] = '\0';
    if (f == NULL)
        return found;
    while (fgets(line, sizeof(line), f) != NULL)
        if (strstr(line, a) != NULL)
            snprintf(found, sizeof(found), "%s", line);
    fclose(f);
    return found;
}

/* Load, Store and Reset through the real link and service, with no go-ahead file anywhere,
   the helper's own clock (moved forward by hand), and the pretend unit on the cable. */
static void test_memory(app *a)
{
    static const uint8_t both[4] = {0, 0, 1, 1}, mixer[4] = {0, 0, 0, 1}, patch[4] = {0, 0, 1, 0};
    uint8_t load3[UA101_CTLMSG_MAX_BYTES], store5[UA101_CTLMSG_MAX_BYTES], bytes[UA101_CTLMSG_MAX_BYTES];
    size_t load3_len = memory_message(UA101_MSG_LOAD, 2, both, load3);
    size_t store5_len = memory_message(UA101_MSG_STORE, 4, mixer, store5);
    int wire = atomic_load(&P.wire_messages), wire_memory = atomic_load(&P.wire_memory);
    uint64_t in_sim = sim_memory_commands(), counted = counts().ctl_memory;
    int closes = atomic_load(&P.closes);
    app *b;

    /* First the app's own engine, all its guards open, through the real link: one Load. */
    CHECK(atomic_load(&a->memory_passes) == UA101_MEMORY_PASSED,
          "with the attach the helper named the 17 commands");
    {
        __block int prepared, confirmed;
        __block uint32_t ticket;
        __block size_t n;
        static uint8_t shown[18]; /* static: a block can't hold an array of its own */

        helper_clock_forward(40);
        on_app(^{
          panel_allow_writes(a->engine, 1);
          panel_allow_memory(a->engine, 1);
          panel_refresh(a->engine, now_us());
        });
        usleep(300000);
        on_app(^{
          prepared = panel_memory_prepare(a->engine, PANEL_MEM_LOAD, 2, 1, 1, now_us(), &ticket, shown, &n);
          confirmed = panel_memory_confirm(a->engine, ticket, now_us());
        });
        CHECK(prepared == PANEL_OK && confirmed == PANEL_OK && n == load3_len && memcmp(shown, load3, n) == 0,
              "the app's engine prepares and confirms Load Memory 3 (%d, %d)", prepared, confirmed);
        CHECK(WAIT(atomic_load(&P.wire_memory) == wire_memory + 1, 3.0) && sim_memory_commands() == in_sim + 1,
              "after a fresh read it reaches the unit, with no go-ahead file anywhere");
        CHECK(WAIT(APP(panel_busy(a->engine)) == 0, 3.0), "and the engine reads the settings back");
        on_app(^{
          panel_allow_memory(a->engine, 0);
          panel_allow_writes(a->engine, 0);
        });
        usleep(700000); /* its read-back, 0.5 s after the command */
    }
    /* From here on an app with no engine, so that no read is sent that the test doesn't send. */
    on_app(^{
      ua101_ctllink_detach(a->link);
      a->up = 0;
      panel_transport(a->engine, 0, now_us());
    });
    CHECK(WAIT(atomic_load(&P.closes) == closes + 1 && !counts().port_open, 3.0), "the app let go of the unit");
    b = app_new(0);
    CHECK(attach(b) == UA101_LINK_OK, "an app without an engine attaches: it reads nothing by itself");
    wire = atomic_load(&P.wire_messages);
    wire_memory = atomic_load(&P.wire_memory);
    helper_clock_forward(10);

    /* An ordinary change after the read. */
    CHECK(read_settings(b), "the settings are read");
    CHECK(atomic_load(&b->memory_passes) == UA101_MEMORY_PASSED, "this app too was told the 17 commands");
    CHECK(saving_reaches(b, 0) && saving_reaches(b, 1), "an ordinary change passes as before (a pause and a resume of the saving)");
    CHECK(atomic_load(&P.wire_memory) == wire_memory && sim_memory_commands() == in_sim + 1,
          "no Load, Store or Reset has reached the cable since the engine's");

    /* The moment decides. */
    CHECK(!memory_reaches(b, load3, load3_len) &&
              strstr(refused_why(b), "weren't read again after the last change") != NULL,
          "the saving was paused and resumed since the read: refused until the settings are read again (%s)",
          refused_why(b));
    CHECK(read_settings(b), "read again");
    {
        int said = log_lines_with("a Load, Store or Reset follows", NULL);
        static char first_bytes[64]; /* static: a block can't hold an array of its own */

        on_service_queue(^{
          snprintf(first_bytes, sizeof(first_bytes), "%02X %02X %02X %02X %02X %02X %02X %02X", P.sim.block[0],
                   P.sim.block[1], P.sim.block[2], P.sim.block[3], P.sim.block[4], P.sim.block[5],
                   P.sim.block[6], P.sim.block[7]);
        });
        CHECK(memory_reaches(b, load3, load3_len), "then Load Memory 3 reaches the unit");
        CHECK(log_lines_with("a Load, Store or Reset follows", NULL) == said + 1 &&
                  strstr(log_last_line_with("a Load, Store or Reset follows"), first_bytes) != NULL &&
                  strlen(strstr(log_last_line_with("a Load, Store or Reset follows"), "(256 bytes): ")) >=
                      13 + 256 * 3 - 1,
              "the helper's log holds the settings from before, all 256 bytes, written before the command");
        CHECK(strstr(log_last_line_with("passed on to the unit: "),
                     "Load Memory 3, the patch bay and the mixer (F0 41 10 00 69 12 00 00 05 08 00 00 01 01 71 F7)") != NULL &&
                  log_lines_with("go-ahead", NULL) == 0,
              "and names what was passed on, and nowhere in the whole log is a go-ahead named (%s)",
              log_last_line_with("passed on to the unit: "));
    }
    CHECK(counts().ctl_memory == counted + 2, "the helper counted it");

    /* One at a time. */
    CHECK(!memory_reaches(b, store5, store5_len) && strstr(refused_why(b), "less than 5 seconds ago") != NULL,
          "a Store right after it: refused, too soon (%s)", refused_why(b));
    CHECK(read_settings(b) && !memory_reaches(b, store5, store5_len) &&
              strstr(refused_why(b), "less than 5 seconds ago") != NULL,
          "also with the settings read again");
    helper_clock_forward(4.5);
    CHECK(!memory_reaches(b, store5, store5_len) && strstr(refused_why(b), "less than 5 seconds ago") != NULL,
          "and 4.5 seconds later");
    helper_clock_forward(0.6);
    CHECK(memory_reaches(b, store5, store5_len), "5.1 seconds after the Load the Store reaches the unit");
    CHECK(strstr(log_last_line_with("passed on to the unit: "), "Store into Memory 5, the mixer (") != NULL,
          "the log names it: Store into Memory 5, the mixer");
    helper_clock_forward(6);
    CHECK(!memory_reaches(b, load3, load3_len) &&
              strstr(refused_why(b), "weren't read again after the last change") != NULL,
          "6 seconds later, with no read since the Store: refused (%s)", refused_why(b));

    /* Only after a fresh read. */
    CHECK(read_settings(b), "read");
    helper_clock_forward(31);
    CHECK(!memory_reaches(b, load3, load3_len) &&
              strstr(refused_why(b), "weren't read in the 30 seconds before") != NULL,
          "31 seconds after the read: refused, the read is too old (%s)", refused_why(b));
    CHECK(read_settings(b), "read");
    helper_clock_forward(29);
    {
        uint8_t change[13];

        CHECK(patch_message(0, 2, change) == 13, "a patch bay change");
        send_raw(b, change, 13);
        CHECK(WAIT(sim_value(UA101_F_PATCH, 0, 0) == 2, 3.0), "an ordinary change passes (OUTPUT 1-2 to INPUT 1-2)");
        CHECK(!memory_reaches(b, load3, load3_len) &&
                  strstr(refused_why(b), "weren't read again after the last change") != NULL,
              "a Load after it: refused, the settings changed since the read");
        patch_message(0, 7, change);
        send_raw(b, change, 13);
        CHECK(WAIT(sim_value(UA101_F_PATCH, 0, 0) == 7, 3.0), "the patch bay back");
    }
    CHECK(read_settings(b) && memory_reaches(b, load3, load3_len), "read again: the Load reaches the unit");
    /* A switch on the unit moved since the read. */
    helper_clock_forward(6);
    CHECK(read_settings(b), "read");
    {
        int before = atomic_load(&b->messages);

        on_service_queue(^{
          ua101_sim_switch_move(&P.sim, UA101_SIM_SWITCH_MONO, 1, 1);
          port_deliver();
        });
        CHECK(WAIT(atomic_load(&b->messages) > before, 3.0), "the MONO switch on the unit moves");
        CHECK(!memory_reaches(b, load3, load3_len) &&
                  strstr(refused_why(b), "a switch on the unit moved since") != NULL,
              "a Load after it: refused until the settings are read again (%s)", refused_why(b));
        before = atomic_load(&b->messages);
        on_service_queue(^{
          ua101_sim_switch_move(&P.sim, UA101_SIM_SWITCH_MONO, 0, 1);
          port_deliver();
        });
        WAIT(atomic_load(&b->messages) > before, 3.0);
        CHECK(read_settings(b) && memory_reaches(b, load3, load3_len), "read again: it reaches the unit");
    }

    /* Never while sound plays. */
    {
        xpc_connection_t plugin = plugin_connect();
        int events = atomic_load(&b->audio_events);

        helper_clock_forward(6);
        CHECK(plugin_ask(plugin, "hello", UA101_LINK_VERSION) == UA101_LINK_OK &&
                  plugin_ask(plugin, "start", UA101_LINK_VERSION) == UA101_LINK_OK &&
                  WAIT(atomic_load(&b->audio_events) > events && atomic_load(&b->audio_now) == 1, 3.0),
              "an app starts to play");
        CHECK(read_settings(b) && !memory_reaches(b, load3, load3_len) &&
                  strstr(refused_why(b), "sound is being played or recorded") != NULL,
              "a Load while sound streams: refused (%s)", refused_why(b));
        CHECK(!memory_reaches(b, store5, store5_len) && strstr(refused_why(b), "sound is being played") != NULL,
              "and a Store");
        CHECK(saving_reaches(b, 0) && saving_reaches(b, 1), "an ordinary change passes while it streams, as before");
        events = atomic_load(&b->audio_events);
        CHECK(plugin_ask(plugin, "stop", UA101_LINK_VERSION) == UA101_LINK_OK &&
                  WAIT(atomic_load(&b->audio_events) > events && atomic_load(&b->audio_now) == 0, 3.0),
              "the sound stops");
        CHECK(read_settings(b) && memory_reaches(b, load3, load3_len), "then, after a read, the Load reaches the unit (%s)", refused_why(b));
        xpc_connection_cancel(plugin);
        xpc_release(plugin);
    }

    /* All 37, one after another, each after a read and 5 seconds; and their neighbours. */
    {
        unsigned n, reached = 0;
        uint64_t before = sim_memory_commands();
        int wire_before = atomic_load(&P.wire_memory);
        static const uint8_t switches[4] = {1, 0, 1, 1};
        ua101_msg m;
        size_t len;

        for (n = 0; n < T_MEMORY_MESSAGES; n++) {
            len = memory_nth(n, bytes);
            helper_clock_forward(5.5);
            reached += read_settings(b) && memory_reaches(b, bytes, len);
        }
        CHECK(reached == T_MEMORY_MESSAGES && sim_memory_commands() == before + T_MEMORY_MESSAGES &&
                  atomic_load(&P.wire_memory) == wire_before + T_MEMORY_MESSAGES,
              "each of the 37 reaches the unit after a read and five seconds (%u)", reached);
        CHECK(strstr(log_last_line_with("passed on to the unit: "),
                     "Reset of everything (the settings in use and all five memories) (F0 41 10 00 69 12 00 00 00 02 00 7E F7)") != NULL &&
                  log_lines_with("passed on to the unit: Reset of the settings in use", "00 00 00 03 00 7D F7") == 1 &&
                  log_lines_with("passed on to the unit: Reset of Memory 5", "00 00 07 03 00 76 F7") == 1 &&
                  log_lines_with("passed on to the unit: Load Memory 1, the patch bay (", NULL) == 1,
              "the log names each");
        helper_clock_forward(6);
        memset(&m, 0, sizeof(m));
        m.kind = UA101_MSG_PRESET;
        m.value = 4;
        len = ua101_ctlmsg_build(&m, bytes);
        CHECK(read_settings(b) && !memory_reaches(b, bytes, len) && strstr(refused_why(b), "preset") != NULL,
              "at the same good moment Roland's one-message Hard Thru is refused (%s)", refused_why(b));
        len = memory_message(UA101_MSG_LOAD, 0, switches, bytes);
        CHECK(!memory_reaches(b, bytes, len) && strstr(refused_why(b), "37 exact messages") != NULL,
              "and a Load that also names the switches' part");
        len = memory_message(UA101_MSG_STORE, 0, patch, bytes);
        bytes[8] = 0x08; /* "Memory 6" */
        bytes[14] = ua101_ctl_checksum(bytes + 6, 8);
        CHECK(!memory_reaches(b, bytes, len), "and a Store into a sixth memory");
        CHECK(sim_memory_commands() == before + T_MEMORY_MESSAGES, "none of those reached the unit");
    }

    /* The interface given back and opened again: what was read before no longer counts. */
    {
        int opens = atomic_load(&P.opens);

        helper_clock_forward(6);
        CHECK(read_settings(b), "read");
        on_app(^{
          ua101_ctllink_detach(b->link);
          b->up = 0;
        });
        CHECK(WAIT(!counts().port_open, 3.0), "the app lets go: the interface is given back");
        CHECK(attach(b) == UA101_LINK_OK && atomic_load(&P.opens) == opens + 1, "and attaches again");
        CHECK(!memory_reaches(b, load3, load3_len) &&
                  strstr(refused_why(b), "weren't read in the 30 seconds before") != NULL,
              "a Load before any read on the new connection: refused (%s)", refused_why(b));
        CHECK(read_settings(b) && memory_reaches(b, load3, load3_len), "after a read it reaches the unit");
    }
    CHECK(atomic_load(&P.wire_other) == 0, "nothing odd reached the cable");
    app_free(b);
    (void)wire;
}

int main(void)
{
    mach_timebase_info_data_t tb;
    ua101_service_options so;
    char log_path[64];
    FILE *log;
    app *a;
    int fd;

    setvbuf(stdout, NULL, _IOLBF, 0);
    mach_timebase_info(&tb);
    g_tick_s = (double)tb.numer / (double)tb.denom / 1e9;

    snprintf(log_path, sizeof(log_path), "/tmp/ua101-test-ctlhelper-XXXXXX");
    fd = mkstemp(log_path);
    log = fd >= 0 ? fdopen(fd, "w") : NULL;
    g_log_path = log_path;
    if (log == NULL) {
        printf("FAIL: no log file\n");
        return 1;
    }
    ua101_sim_init(&P.sim);
    atomic_store(&P.can_open, 1);

    memset(&so, 0, sizeof(so));
    so.input_offset = -1;
    so.output_offset = -1;
    so.watch = 1;
    so.allow_uid = (int)getuid();
    so.log = log;
    g_endpoint = ua101_service_start_anonymous(&so);
    if (g_endpoint == NULL) {
        printf("FAIL: the service didn't start\n");
        return 1;
    }
    ua101_ctllink_use_endpoint(g_endpoint);
    ua101_midilink_use_endpoint(g_endpoint);
    A = dispatch_queue_create("test.app", DISPATCH_QUEUE_SERIAL);

    test_who_may_ask();
    test_rules();
    test_memory_rules();
    a = app_new(1);
    test_no_unit(a);
    test_read(a);
    test_switch(a);
    test_memory_reads(a);
    test_helper_refuses(a);
    test_saving(a);
    test_patch(a);
    test_volume(a);
    test_pan(a);
    test_on(a);
    test_all_channels(a);
    test_link_solo(a);
    test_two_apps(a);
    test_levels(a);
    test_verdict_says_what_was_over();
    test_coming_and_going(a);
    test_flood(a);
    test_midi(a);
    CHECK(atomic_load(&P.wire_memory) == 0 && sim_memory_commands() == 0,
          "up to here, with none asked for, no Load, Store or Reset reached the cable");
    test_memory(a);
    CHECK(atomic_load(&P.wire_messages) ==
                  atomic_load(&P.wire_reads) + atomic_load(&P.wire_saving) + atomic_load(&P.wire_patch) +
                      atomic_load(&P.wire_volume) + atomic_load(&P.wire_pan) + atomic_load(&P.wire_on) +
                      atomic_load(&P.wire_link) + atomic_load(&P.wire_solo) + atomic_load(&P.wire_memory) +
                      atomic_load(&P.wire_memory_reads) &&
              atomic_load(&P.wire_memory_reads) == T_MEMORY_READS &&
              atomic_load(&P.wire_saving) == 14 && atomic_load(&P.wire_patch) == 14 &&
              atomic_load(&P.wire_other) == 0,
          "in the whole run, every message on the cable was the settings request (%d) or "
          "a pause or resume of saving (%d), a patch bay source (%d), a passed "
          "channel's volume at a value that is at most the top of the range (%d), a passed channel's pan from "
          "0x0000 to 0x8000 (%d), a passed channel's on/off, 0 or 1 (%d), a passed pair's link, "
          "0 or 1 (%d), a passed group's solo, 0 to 10 (%d), one of the "
          "37 Load, Store and Reset messages (%d), or one of the five memory reads (%d), of %d",
          atomic_load(&P.wire_reads), atomic_load(&P.wire_saving), atomic_load(&P.wire_patch),
          atomic_load(&P.wire_volume), atomic_load(&P.wire_pan), atomic_load(&P.wire_on),
          atomic_load(&P.wire_link), atomic_load(&P.wire_solo), atomic_load(&P.wire_memory),
          atomic_load(&P.wire_memory_reads), atomic_load(&P.wire_messages));
    {
        __block uint64_t touched;

        on_service_queue(^{
          touched = (P.sim.sets_applied - 14 - (uint64_t)atomic_load(&P.wire_volume) -
                     (uint64_t)atomic_load(&P.wire_pan) - (uint64_t)atomic_load(&P.wire_on) -
                     (uint64_t)atomic_load(&P.wire_link) - (uint64_t)atomic_load(&P.wire_solo)) +
                    P.sim.sets_ignored +
                    (P.sim.memory_commands - (uint64_t)atomic_load(&P.wire_memory)) +
                    P.sim.unknown_messages + P.sim.bad_messages +
                    (uint64_t)P.sim.saving_paused;
        });
        CHECK(touched == 0, "and the pretend unit was asked to change nothing but patch bay "
                            "sources, fourteen times, the passed channels' volumes, pans and "
                            "on/offs, the passed pairs' links and the passed groups' solos, "
                            "saves, and the Load, Store and Reset messages counted on the cable");
    }
    app_free(a);
    test_no_helper();
    test_older_helper();
    test_close_in_flight();

    ua101_service_shutdown();
    fclose(log);
    if (failures != 0) {
        char command[128];

        printf("The helper's log:\n");
        snprintf(command, sizeof(command), "cat %s", log_path);
        if (system(command) != 0)
            printf("(not readable)\n");
    }
    unlink(log_path);
    printf("test_ctlhelper: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
