/*
 * The per-user agent's CoreMIDI side (src/ua101_midiports.c) with no unit: the agent's real
 * code and its real end of the link, the helper's real service, and a pretend unit behind a
 * pretend MIDI interface, all in one process. A MIDI app is played by this test through
 * CoreMIDI itself, so it needs the logged-in user's MIDI server: `make agent-test`, not part of
 * `make test`.
 *
 * The ports made here are private (hidden from every other app) and carry a name of their
 * own, so nothing on the Mac sees them, and nothing here can reach a real MIDI device.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>

#include <dispatch/dispatch.h>
#include <mach/mach_time.h>
#include <os/lock.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>
#include <xpc/xpc.h>

#include "../src/ua101_engine.h"
#include "../src/ua101_link.h"
#include "../src/ua101_midi.h"
#include "../src/ua101_midilink.h"
#include "../src/ua101_midiport.h"
#include "../src/ua101_midiports.h"
#include "../src/ua101_service.h"
#include "../src/ua101_watch.h"

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

/* --- The pretend unit: the bus, no audio, and its MIDI interface. --- */

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

struct ua101_engine {
    int unused;
};

ua101_engine *ua101_engine_open(const ua101_engine_options *opt, ua101_engine_format *format,
                                FILE *log, char *why, size_t why_bytes)
{
    (void)opt;
    (void)format;
    (void)log;
    snprintf(why, why_bytes, "no audio in this test");
    return NULL;
}

int ua101_engine_start(ua101_engine *e, ua101_shared *shared, size_t shared_bytes)
{
    (void)e;
    (void)shared;
    (void)shared_bytes;
    return 0;
}

const char *ua101_engine_failure(const ua101_engine *e)
{
    (void)e;
    return NULL;
}

void ua101_engine_stop(ua101_engine *e)
{
    (void)e;
}

void ua101_engine_summarize(const ua101_engine *e, ua101_engine_summary *out)
{
    (void)e;
    memset(out, 0, sizeof(*out));
}

void ua101_engine_close(ua101_engine *e)
{
    (void)e;
}

static struct {
    ua101_midiport_events events; /* on the service's queue */
    int open;
    uint8_t midi[4096]; /* what reached the MIDI OUT socket, as MIDI bytes */
    size_t midi_len;
    _Atomic int wire_midi_bytes, wire_other_packets, opens, closes;
} P;

struct ua101_midiport {
    int unused;
};
static struct ua101_midiport g_port;

ua101_midiport *ua101_midiport_open(dispatch_queue_t queue, const ua101_midiport_events *events,
                                    void *context, FILE *log, char *why, size_t why_bytes)
{
    (void)queue;
    (void)context;
    (void)log;
    if (!atomic_load(&g_on_bus)) {
        snprintf(why, why_bytes, "no UA-101 in Hi-Speed mode was found (pretend)");
        return NULL;
    }
    P.events = *events;
    P.open = 1;
    atomic_fetch_add(&P.opens, 1);
    return &g_port;
}

int ua101_midiport_send(ua101_midiport *p, const uint8_t *packets, size_t bytes)
{
    size_t i;

    if (p != &g_port || !P.open || bytes % 4 != 0)
        return 0;
    for (i = 0; i < bytes; i += 4) {
        uint8_t cable, b[3];
        int malformed;
        size_t n = ua101_usbmidi_unpack(packets + i, &cable, b, &malformed);

        if (cable != UA101_MIDI_CABLE || malformed || n == 0) {
            atomic_fetch_add(&P.wire_other_packets, 1);
        } else if (P.midi_len + n <= sizeof(P.midi)) {
            memcpy(P.midi + P.midi_len, b, n);
            P.midi_len += n;
            atomic_fetch_add(&P.wire_midi_bytes, (int)n);
        }
    }
    return 1;
}

void ua101_midiport_close(ua101_midiport *p)
{
    if (p != &g_port || !P.open)
        return;
    P.open = 0;
    atomic_fetch_add(&P.closes, 1);
}

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

static void unit_sends(const uint8_t *packets, size_t bytes, uint64_t host_time)
{
    on_service_queue(^{
      if (P.open)
          P.events.packets(NULL, packets, bytes, host_time);
    });
}

static int wire_ends(const uint8_t *want, size_t len)
{
    __block int same;

    on_service_queue(^{
      same = P.midi_len >= len && memcmp(P.midi + P.midi_len - len, want, len) == 0;
    });
    return same;
}

/* --- The agent, on a queue of its own as on the real program's main queue. --- */

static dispatch_queue_t Q;
static ua101_midiports *g_agent;
static _Atomic int g_broken;

static void agent_broken(void *context, const char *why)
{
    (void)context;
    (void)why;
    atomic_fetch_add(&g_broken, 1);
}

static ua101_midiports_counts agent(void)
{
    __block ua101_midiports_counts c;

    dispatch_sync(Q, ^{
      ua101_midiports_get_counts(g_agent, &c);
    });
    return c;
}

static ua101_service_counts service(void)
{
    ua101_service_counts c;

    ua101_service_get_counts(&c);
    return c;
}

/* --- The MIDI app: a CoreMIDI client of this test's own. --- */

#define MAX_WORDS 256

static struct {
    os_unfair_lock lock;
    uint32_t words[MAX_WORDS];
    MIDITimeStamp stamps[MAX_WORDS]; /* the stamp of the packet each word came in */
    size_t count;
} g_got;

static MIDIClientRef g_client;
static MIDIPortRef g_in_port, g_out_port;
static MIDIEndpointRef g_listening;

static int app_start(void)
{
    g_got.lock = OS_UNFAIR_LOCK_INIT;
    if (MIDIClientCreateWithBlock(CFSTR("ua101 agent test"), &g_client, NULL) != noErr)
        return 0;
    if (MIDIInputPortCreateWithProtocol(
            g_client, CFSTR("in"), kMIDIProtocol_1_0, &g_in_port,
            ^(const MIDIEventList *list, void *ref) {
              const MIDIEventPacket *packet = &list->packet[0];
              UInt32 p, w;

              (void)ref;
              os_unfair_lock_lock(&g_got.lock);
              for (p = 0; p < list->numPackets; p++) {
                  for (w = 0; w < packet->wordCount && g_got.count < MAX_WORDS; w++) {
                      g_got.stamps[g_got.count] = packet->timeStamp;
                      g_got.words[g_got.count++] = packet->words[w];
                  }
                  packet = MIDIEventPacketNext(packet);
              }
              os_unfair_lock_unlock(&g_got.lock);
            }) != noErr)
        return 0;
    return MIDIOutputPortCreate(g_client, CFSTR("out"), &g_out_port) == noErr;
}

/* Listens to the agent's source as it is now. */
static int app_listen(void)
{
    MIDIEndpointRef source = (MIDIEndpointRef)agent().source;

    if (g_listening != 0)
        MIDIPortDisconnectSource(g_in_port, g_listening);
    g_listening = source;
    return source != 0 && MIDIPortConnectSource(g_in_port, source, NULL) == noErr;
}

static size_t got_count(void)
{
    size_t n;

    os_unfair_lock_lock(&g_got.lock);
    n = g_got.count;
    os_unfair_lock_unlock(&g_got.lock);
    return n;
}

static void got_reset(void)
{
    os_unfair_lock_lock(&g_got.lock);
    g_got.count = 0;
    os_unfair_lock_unlock(&g_got.lock);
}

/* Sends MIDI bytes to the agent's destination as an app would: as UMP, a packet at a time. */
static int app_send(const uint8_t *bytes, size_t len)
{
    static const ua101_midi_event cut_event = {UA101_MIDI_SYSEX_CUT, {0}, 0};
    MIDIEndpointRef dest = (MIDIEndpointRef)agent().destination;
    ua101_midi_parser parser;
    ua101_ump_packer packer;
    size_t i;
    int ok = dest != 0;

    ua101_midi_parser_init(&parser);
    ua101_ump_packer_init(&packer);
    for (i = 0; i < len && ok; i++) {
        ua101_midi_event ev[2];
        int cut, k, n = 0;

        ua101_midi_parse(&parser, bytes[i], &ev[1], &cut);
        if (cut)
            ev[n++] = cut_event;
        ev[n++] = ev[1];
        for (k = 0; k < n && ok; k++) {
            uint32_t words[2];
            size_t count = ua101_ump_pack(&packer, &ev[k], words);
            MIDIEventList list;
            MIDIEventPacket *packet = MIDIEventListInit(&list, kMIDIProtocol_1_0);

            if (count == 0)
                continue;
            packet = MIDIEventListAdd(&list, sizeof(list), packet, 0, count, words);
            ok = packet != NULL && MIDISendEventList(g_out_port, dest, &list) == noErr;
        }
    }
    return ok;
}

static double cpu_seconds(void)
{
    struct rusage u;

    getrusage(RUSAGE_SELF, &u);
    return (double)(u.ru_utime.tv_sec + u.ru_stime.tv_sec) +
           (double)(u.ru_utime.tv_usec + u.ru_stime.tv_usec) / 1e6;
}

/* --- The cases. --- */

static void test_ports_follow_the_unit(void)
{
    ua101_midiports_counts c;
    double cpu;

    CHECK(WAIT(agent().attaches >= 1, 8.0), "the agent asked the helper for the unit");
    c = agent();
    CHECK(!c.ports_up && !c.attached, "no unit: no MIDI ports");
    cpu = cpu_seconds();
    usleep(1000000);
    CHECK(agent().attaches == c.attaches,
          "and it doesn't keep asking: the helper will say when a unit appears");
    cpu = cpu_seconds() - cpu;
    CHECK(cpu < 0.05, "a second of waiting for a unit used %.3f s of processor time", cpu);

    unit_comes();
    CHECK(WAIT(agent().ports_up, 5.0), "the unit appears: the ports are made");
    c = agent();
    CHECK(c.attached && c.source != 0 && c.destination != 0 && c.source != c.destination,
          "a source and a destination, and the helper holds the unit's MIDI interface");
    CHECK(atomic_load(&P.opens) == 1, "opened once");
}

static void test_app_to_unit(void)
{
    static const uint8_t note[] = {0x90, 0x3C, 0x40};
    static const uint8_t sysex[] = {0xF0, 0x7D, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0xF7};
    static const uint8_t own[] = {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x04, 0x00,
                                  0x07, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x71, 0xF7};
    static const uint8_t off[] = {0x80, 0x3C, 0x00};
    int before = atomic_load(&P.wire_midi_bytes);

    CHECK(app_send(note, sizeof(note)), "an app sends a note to \"UA-101 MIDI\"");
    CHECK(WAIT(atomic_load(&P.wire_midi_bytes) == before + 3, 3.0) && wire_ends(note, sizeof(note)),
          "it reaches the unit's MIDI OUT socket");
    CHECK(app_send(sysex, sizeof(sysex)), "an app sends SysEx of another maker");
    CHECK(WAIT(atomic_load(&P.wire_midi_bytes) == before + 3 + (int)sizeof(sysex), 3.0) &&
              wire_ends(sysex, sizeof(sysex)),
          "it arrives whole, in order");
    CHECK(app_send(own, sizeof(own)) && app_send(off, sizeof(off)),
          "an app sends a volume change addressed to the UA-101 itself, then a note off");
    CHECK(WAIT(atomic_load(&P.wire_midi_bytes) == before + 6 + (int)sizeof(sysex), 3.0) &&
              wire_ends(off, sizeof(off)),
          "only the note off arrives (%d bytes in all)", atomic_load(&P.wire_midi_bytes) - before);
    CHECK(service().gate.own_sysex == 1 && atomic_load(&P.wire_other_packets) == 0,
          "the helper dropped the one for the UA-101, and nothing went to any other cable");
    CHECK(agent().out_refused == 0 && agent().out_unattached == 0, "the agent refused nothing");
}

static void test_unit_to_app(void)
{
    static const uint8_t read[] = {0x09, 0x90, 0x3C, 0x40, /* note on */
                                   0x0F, 0xF8, 0x00, 0x00, /* clock */
                                   0x14, 0xF0, 0x41, 0x10, /* the control cable: not for apps */
                                   0x04, 0xF0, 0x7D, 0x01, /* SysEx */
                                   0x07, 0x02, 0x03, 0xF7};
    static const uint32_t want[] = {0x20903C40, 0x10F80000, 0x30047D01, 0x02030000};
    uint64_t stamp = mach_absolute_time();
    size_t i, same = 0, stamped = 0;

    CHECK(app_listen(), "an app listens to \"UA-101 MIDI\"");
    got_reset();
    unit_sends(read, sizeof(read), stamp);
    CHECK(WAIT(got_count() >= 4, 3.0), "what arrives at MIDI IN reaches the app (%zu words)",
          got_count());
    usleep(100000);
    os_unfair_lock_lock(&g_got.lock);
    for (i = 0; i < g_got.count && i < 4; i++) {
        same += g_got.words[i] == want[i];
        stamped += g_got.stamps[i] == stamp;
    }
    CHECK(g_got.count == 4 && same == 4,
          "a note, a clock byte and a SysEx, and nothing of the control cable (%zu words, %zu "
          "right)",
          g_got.count, same);
    CHECK(stamped == 4, "each stamped with the time the USB read completed (%zu of 4)", stamped);
    os_unfair_lock_unlock(&g_got.lock);
    CHECK(agent().in_errors == 0, "CoreMIDI took every message");
}

static void test_unit_goes_and_comes(void)
{
    static const uint8_t note[] = {0x90, 0x40, 0x41};
    static const uint8_t read[] = {0x08, 0x80, 0x40, 0x00};
    uint32_t old_source;
    int before;
    double cpu;

    cpu = cpu_seconds();
    usleep(1000000);
    cpu = cpu_seconds() - cpu;
    CHECK(cpu < 0.05, "a second attached with nothing to do used %.3f s of processor time", cpu);

    unit_goes();
    CHECK(WAIT(!agent().ports_up, 5.0), "the unit goes: the ports are removed");
    CHECK(!agent().attached && atomic_load(&P.closes) == 1, "and the helper gave the interface back");
    CHECK(!app_send(note, sizeof(note)), "an app has nothing to send to");

    unit_comes();
    CHECK(WAIT(agent().ports_up, 5.0), "the unit comes back: the ports are made again");
    before = atomic_load(&P.wire_midi_bytes);
    CHECK(app_send(note, sizeof(note)) &&
              WAIT(atomic_load(&P.wire_midi_bytes) == before + 3, 3.0),
          "and notes pass again");
    CHECK(app_listen(), "the app listens to the new source");
    got_reset();
    unit_sends(read, sizeof(read), 0);
    CHECK(WAIT(got_count() == 1, 3.0), "and hears MIDI IN again");

    /* The Mac sleeps and wakes with the unit on: the ports stay as they are for apps. */
    old_source = agent().source;
    {
        uint64_t attaches = agent().attaches;

        on_service_queue(^{
          W.events.will_sleep();
        });
        on_service_queue(^{
          W.events.did_wake();
        });
        CHECK(WAIT(agent().attaches == attaches + 1 && agent().attached, 5.0),
              "after a sleep and wake the agent asked again and is attached");
        CHECK(agent().source == old_source && agent().ports_up,
              "and the ports are the same ones: apps never saw them go");
    }
}

int main(void)
{
    mach_timebase_info_data_t tb;
    ua101_service_options so;
    ua101_midiports_options ao;
    xpc_endpoint_t endpoint;
    char log_path[64], name[64];
    FILE *log;
    int fd;

    setvbuf(stdout, NULL, _IOLBF, 0);
    mach_timebase_info(&tb);
    g_tick_s = (double)tb.numer / (double)tb.denom / 1e9;

    snprintf(log_path, sizeof(log_path), "/tmp/ua101-test-midiagent-XXXXXX");
    fd = mkstemp(log_path);
    log = fd >= 0 ? fdopen(fd, "w") : NULL;
    if (log == NULL) {
        printf("FAIL: no log file\n");
        return 1;
    }
    memset(&so, 0, sizeof(so));
    so.input_offset = -1;
    so.output_offset = -1;
    so.watch = 1;
    so.allow_uid = (int)getuid();
    so.log = log;
    endpoint = ua101_service_start_anonymous(&so);
    if (endpoint == NULL) {
        printf("FAIL: the service didn't start\n");
        return 1;
    }
    ua101_midilink_use_endpoint(endpoint);

    if (!app_start()) {
        printf("FAIL: no CoreMIDI client could be made: is a user logged in?\n");
        return 1;
    }
    Q = dispatch_queue_create("test.agent", DISPATCH_QUEUE_SERIAL);
    snprintf(name, sizeof(name), "UA-101 MIDI test %d", (int)getpid());
    memset(&ao, 0, sizeof(ao));
    ao.name = name;
    ao.private_ports = 1;
    ao.log = log;
    ao.broken = agent_broken;
    g_agent = ua101_midiports_start(Q, &ao);
    if (g_agent == NULL) {
        printf("FAIL: the agent didn't start\n");
        return 1;
    }

    test_ports_follow_the_unit();
    test_app_to_unit();
    test_unit_to_app();
    test_unit_goes_and_comes();

    dispatch_sync(Q, ^{
      ua101_midiports_stop(g_agent);
    });
    dispatch_sync(Q, ^{
    });
    CHECK(WAIT(service().midi_clients == 0 && !service().port_open, 3.0),
          "stopped: the helper has no MIDI client left and gave the interface back");
    CHECK(atomic_load(&g_broken) == 0, "CoreMIDI never lost the ports");

    ua101_service_shutdown();
    fclose(log);
    if (failures != 0) {
        char command[128];

        printf("The helper's and the agent's log:\n");
        snprintf(command, sizeof(command), "cat %s", log_path);
        if (system(command) != 0)
            printf("(not readable)\n");
    }
    unlink(log_path);
    printf("test_midiagent: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
