/*
 * The CoreMIDI driver plug-in (src/midiplugin/ua101_midiplugin.c)
 * with no unit and no MIDI server: the plug-in's real code and its real end of the link, the
 * helper's real service, a pretend unit behind a pretend MIDI interface, and a pretend MIDI
 * server around the plug-in, all in one process.
 *
 * The pretend MIDI server is this file's own versions of the calls only a driver inside the real
 * server can make (MIDIDeviceCreate, MIDISetupAddDevice, MIDIReceivedEventList and the rest):
 * defined here, they are the ones the plug-in's code is linked to. A thread with a run loop plays
 * the server's main thread, where Start and Stop are called and where the device must be
 * changed; this test's main thread plays the server's I/O thread, which calls SendPackets.
 *
 * Nothing here reaches the installed helper, the MIDI server or any MIDI device: the link is
 * pointed at the helper's service started in this process before anything else.
 */
#include <CoreFoundation/CFPlugInCOM.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>
#include <CoreMIDI/MIDIDriver.h>

#include <dispatch/dispatch.h>
#include <mach/mach_time.h>
#include <os/lock.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>
#include <xpc/xpc.h>

#include "../src/midiplugin/ua101_midiplugin.h"
#include "../src/ua101_engine.h"
#include "../src/ua101_link.h"
#include "../src/ua101_midi.h"
#include "../src/ua101_midilink.h"
#include "../src/ua101_midiport.h"
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

static double cpu_seconds(void)
{
    struct rusage u;

    getrusage(RUSAGE_SELF, &u);
    return (double)(u.ru_utime.tv_sec + u.ru_stime.tv_sec) +
           (double)(u.ru_utime.tv_usec + u.ru_stime.tv_usec) / 1e6;
}

/* --- The pretend unit: the bus, no audio, and its MIDI interface (as tests/test_midiagent.c). */

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
    uint8_t midi[8192]; /* what reached the MIDI OUT socket, as MIDI bytes */
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

static int wire_bytes(void)
{
    return atomic_load(&P.wire_midi_bytes);
}

static ua101_service_counts service(void)
{
    ua101_service_counts c;

    ua101_service_get_counts(&c);
    return c;
}

/* --- The pretend MIDI server's main thread: a thread with a run loop. --- */

static pthread_t g_server_thread;
static CFRunLoopRef g_server_loop;

static void *server_main(void *arg)
{
    dispatch_semaphore_t ready = arg;
    CFRunLoopTimerRef keep = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + 1e9, 1e9, 0,
                                                  0, NULL, NULL);

    g_server_loop = CFRunLoopGetCurrent();
    CFRetain(g_server_loop);
    CFRunLoopAddTimer(g_server_loop, keep, kCFRunLoopCommonModes);
    dispatch_semaphore_signal(ready);
    CFRunLoopRun();
    return NULL;
}

static void on_server(void (^work)(void))
{
    dispatch_semaphore_t done = dispatch_semaphore_create(0);

    CFRunLoopPerformBlock(g_server_loop, kCFRunLoopCommonModes, ^{
      work();
      dispatch_semaphore_signal(done);
    });
    CFRunLoopWakeUp(g_server_loop);
    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
    dispatch_release(done);
}

/* --- The pretend MIDI server's objects, and the calls a driver makes on it. --- */

enum { KIND_NONE, KIND_DEVICE, KIND_ENTITY, KIND_SOURCE, KIND_DEST };
#define MAX_OBJECTS 32
#define REF(i) ((MIDIObjectRef)(0x5000 + (i)))
#define INDEX(ref) ((int)(ref) - 0x5000)
#define MAX_WORDS 512

static struct {
    os_unfair_lock lock;
    struct {
        int kind;
        int parent;         /* the device of an entity, the entity of an endpoint */
        int children[2];    /* an entity's source and destination; a device's entity */
        int in_setup;       /* a device added with MIDISetupAddDevice */
        int disposed;
        int offline;        /* -1 never set */
        int offline_changes;
        int32_t unique_id;
        char name[64], manufacturer[64], model[64];
        void *ref1;
    } o[MAX_OBJECTS];
    int count;
    int creates, adds, disposes;
    int off_thread;      /* device changes made off the server's main thread */
    int32_t taken_id;    /* a unique ID another object already has */
    /* What the plug-in gave the server from MIDI IN, word by word. */
    uint32_t words[MAX_WORDS];
    MIDITimeStamp stamps[MAX_WORDS];
    size_t word_count;
    int received_from_other;  /* MIDIReceivedEventList on anything but our source */
    OSStatus receive_status;  /* what MIDIReceivedEventList answers */
} S = {.lock = OS_UNFAIR_LOCK_INIT};

static int valid(MIDIObjectRef ref, int kind)
{
    int i = INDEX(ref);

    return i >= 0 && i < S.count && S.o[i].kind == kind && !S.o[i].disposed;
}

static int new_object(int kind, int parent, const char *name)
{
    int i = S.count++;

    memset(&S.o[i], 0, sizeof(S.o[i]));
    S.o[i].kind = kind;
    S.o[i].parent = parent;
    S.o[i].offline = -1;
    S.o[i].children[0] = S.o[i].children[1] = -1;
    S.o[i].unique_id = 0x7000 + i;
    snprintf(S.o[i].name, sizeof(S.o[i].name), "%s", name);
    return i;
}

static void server_thread_only(void)
{
    if (!pthread_equal(pthread_self(), g_server_thread))
        S.off_thread++;
}

static void cstr(CFStringRef s, char *out, size_t size)
{
    out[0] = 0;
    if (s != NULL)
        CFStringGetCString(s, out, (CFIndex)size, kCFStringEncodingUTF8);
}

OSStatus MIDIDeviceCreate(MIDIDriverRef owner, CFStringRef name, CFStringRef manufacturer,
                          CFStringRef model, MIDIDeviceRef *outDevice)
{
    int i;

    (void)owner;
    os_unfair_lock_lock(&S.lock);
    server_thread_only();
    i = new_object(KIND_DEVICE, -1, "");
    cstr(name, S.o[i].name, sizeof(S.o[i].name));
    cstr(manufacturer, S.o[i].manufacturer, sizeof(S.o[i].manufacturer));
    cstr(model, S.o[i].model, sizeof(S.o[i].model));
    S.creates++;
    os_unfair_lock_unlock(&S.lock);
    *outDevice = REF(i);
    return noErr;
}

OSStatus MIDIDeviceDispose(MIDIDeviceRef device)
{
    os_unfair_lock_lock(&S.lock);
    server_thread_only();
    if (valid(device, KIND_DEVICE))
        S.o[INDEX(device)].disposed = 1;
    S.disposes++;
    os_unfair_lock_unlock(&S.lock);
    return noErr;
}

OSStatus MIDIDeviceNewEntity(MIDIDeviceRef device, CFStringRef name, MIDIProtocolID protocol,
                             Boolean embedded, ItemCount numSourceEndpoints,
                             ItemCount numDestinationEndpoints, MIDIEntityRef *newEntity)
{
    char text[64];
    int d = INDEX(device), e, s, t;

    (void)embedded;
    os_unfair_lock_lock(&S.lock);
    server_thread_only();
    if (!valid(device, KIND_DEVICE) || protocol != kMIDIProtocol_1_0 || numSourceEndpoints != 1 ||
        numDestinationEndpoints != 1 || S.o[d].children[0] >= 0) {
        os_unfair_lock_unlock(&S.lock);
        return kMIDIUnknownError;
    }
    cstr(name, text, sizeof(text));
    e = new_object(KIND_ENTITY, d, text);
    s = new_object(KIND_SOURCE, e, "");
    t = new_object(KIND_DEST, e, "");
    S.o[d].children[0] = e;
    S.o[e].children[0] = s;
    S.o[e].children[1] = t;
    os_unfair_lock_unlock(&S.lock);
    *newEntity = REF(e);
    return noErr;
}

ItemCount MIDIDeviceGetNumberOfEntities(MIDIDeviceRef device)
{
    ItemCount n;

    os_unfair_lock_lock(&S.lock);
    n = valid(device, KIND_DEVICE) && S.o[INDEX(device)].children[0] >= 0 ? 1 : 0;
    os_unfair_lock_unlock(&S.lock);
    return n;
}

MIDIEntityRef MIDIDeviceGetEntity(MIDIDeviceRef device, ItemCount index0)
{
    MIDIEntityRef e = 0;

    os_unfair_lock_lock(&S.lock);
    if (valid(device, KIND_DEVICE) && index0 == 0 && S.o[INDEX(device)].children[0] >= 0)
        e = REF(S.o[INDEX(device)].children[0]);
    os_unfair_lock_unlock(&S.lock);
    return e;
}

static ItemCount endpoint_count(MIDIEntityRef entity, int which)
{
    ItemCount n;

    os_unfair_lock_lock(&S.lock);
    n = valid(entity, KIND_ENTITY) && S.o[INDEX(entity)].children[which] >= 0 ? 1 : 0;
    os_unfair_lock_unlock(&S.lock);
    return n;
}

static MIDIEndpointRef endpoint(MIDIEntityRef entity, ItemCount index0, int which)
{
    MIDIEndpointRef r = 0;

    os_unfair_lock_lock(&S.lock);
    if (valid(entity, KIND_ENTITY) && index0 == 0 && S.o[INDEX(entity)].children[which] >= 0)
        r = REF(S.o[INDEX(entity)].children[which]);
    os_unfair_lock_unlock(&S.lock);
    return r;
}

ItemCount MIDIEntityGetNumberOfSources(MIDIEntityRef entity)
{
    return endpoint_count(entity, 0);
}

ItemCount MIDIEntityGetNumberOfDestinations(MIDIEntityRef entity)
{
    return endpoint_count(entity, 1);
}

MIDIEndpointRef MIDIEntityGetSource(MIDIEntityRef entity, ItemCount sourceIndex0)
{
    return endpoint(entity, sourceIndex0, 0);
}

MIDIEndpointRef MIDIEntityGetDestination(MIDIEntityRef entity, ItemCount destIndex0)
{
    return endpoint(entity, destIndex0, 1);
}

OSStatus MIDISetupAddDevice(MIDIDeviceRef device)
{
    OSStatus status = kMIDIObjectNotFound;

    os_unfair_lock_lock(&S.lock);
    server_thread_only();
    if (valid(device, KIND_DEVICE)) {
        S.o[INDEX(device)].in_setup = 1;
        S.adds++;
        status = noErr;
    }
    os_unfair_lock_unlock(&S.lock);
    return status;
}

OSStatus MIDIEndpointSetRefCons(MIDIEndpointRef endpt, void *ref1, void *ref2)
{
    (void)ref2;
    os_unfair_lock_lock(&S.lock);
    server_thread_only();
    if (valid(endpt, KIND_DEST) || valid(endpt, KIND_SOURCE))
        S.o[INDEX(endpt)].ref1 = ref1;
    os_unfair_lock_unlock(&S.lock);
    return noErr;
}

OSStatus MIDIObjectSetIntegerProperty(MIDIObjectRef obj, CFStringRef propertyID, SInt32 value)
{
    int i = INDEX(obj);
    OSStatus status = noErr;

    os_unfair_lock_lock(&S.lock);
    server_thread_only();
    if (i < 0 || i >= S.count) {
        status = kMIDIObjectNotFound;
    } else if (CFEqual(propertyID, kMIDIPropertyOffline)) {
        if (S.o[i].offline != value)
            S.o[i].offline_changes++;
        S.o[i].offline = value;
    } else if (CFEqual(propertyID, kMIDIPropertyUniqueID)) {
        int k;

        for (k = 0; k < S.count; k++)
            if (k != i && S.o[k].unique_id == value)
                status = kMIDIIDNotUnique;
        if (value == S.taken_id)
            status = kMIDIIDNotUnique;
        if (status == noErr)
            S.o[i].unique_id = value;
    }
    os_unfair_lock_unlock(&S.lock);
    return status;
}

OSStatus MIDIObjectSetStringProperty(MIDIObjectRef obj, CFStringRef propertyID, CFStringRef str)
{
    int i = INDEX(obj);

    os_unfair_lock_lock(&S.lock);
    server_thread_only();
    if (i >= 0 && i < S.count && CFEqual(propertyID, kMIDIPropertyName))
        cstr(str, S.o[i].name, sizeof(S.o[i].name));
    os_unfair_lock_unlock(&S.lock);
    return noErr;
}

OSStatus MIDIReceivedEventList(MIDIEndpointRef src, const MIDIEventList *evtlist)
{
    const MIDIEventPacket *packet = &evtlist->packet[0];
    UInt32 p, w;
    OSStatus status;

    os_unfair_lock_lock(&S.lock);
    status = S.receive_status;
    if (!valid(src, KIND_SOURCE))
        S.received_from_other++;
    else if (status == noErr)
        for (p = 0; p < evtlist->numPackets; p++) {
            for (w = 0; w < packet->wordCount && S.word_count < MAX_WORDS; w++) {
                S.stamps[S.word_count] = packet->timeStamp;
                S.words[S.word_count++] = packet->words[w];
            }
            packet = MIDIEventPacketNext(packet);
        }
    os_unfair_lock_unlock(&S.lock);
    return status;
}

/* The lists the server hands Start: an empty one, or one holding given devices. */
#define LIST_EMPTY ((MIDIDeviceListRef)0x9000)
#define LIST_OF(n) ((MIDIDeviceListRef)(0x9100 + (n)))
static int g_list[4];

ItemCount MIDIDeviceListGetNumberOfDevices(MIDIDeviceListRef devList)
{
    if (devList == LIST_EMPTY)
        return 0;
    return (ItemCount)((uintptr_t)devList - 0x9100);
}

MIDIDeviceRef MIDIDeviceListGetDevice(MIDIDeviceListRef devList, ItemCount index0)
{
    if (devList == LIST_EMPTY || index0 >= MIDIDeviceListGetNumberOfDevices(devList))
        return 0;
    return REF(g_list[index0]);
}

/* --- Reading the pretend server --- */

static int offline_of(int device)
{
    int v;

    os_unfair_lock_lock(&S.lock);
    v = S.o[device].offline;
    os_unfair_lock_unlock(&S.lock);
    return v;
}

static int offline_changes_of(int device)
{
    int v;

    os_unfair_lock_lock(&S.lock);
    v = S.o[device].offline_changes;
    os_unfair_lock_unlock(&S.lock);
    return v;
}

static size_t got_count(void)
{
    size_t n;

    os_unfair_lock_lock(&S.lock);
    n = S.word_count;
    os_unfair_lock_unlock(&S.lock);
    return n;
}

static void got_reset(void)
{
    os_unfair_lock_lock(&S.lock);
    S.word_count = 0;
    os_unfair_lock_unlock(&S.lock);
}

/* --- The plug-in, as the server holds it --- */

static void *g_driver;
static MIDIDriverInterface **g_d;
static int g_device = -1, g_source = -1, g_dest = -1;
static int g_ref2; /* the server passes the second refCon as it is; ours is unused */
static FILE *g_log;

static void test_note(const char *text)
{
    if (g_log != NULL) {
        fprintf(g_log, "plug-in: %s\n", text);
        fflush(g_log);
    }
}

static ua101_midiplugin_counts plugin(void)
{
    ua101_midiplugin_counts c;

    ua101_midiplugin_get_counts(g_driver, &c);
    return c;
}

static OSStatus start(MIDIDeviceListRef list)
{
    __block OSStatus status;

    on_server(^{
      status = (*g_d)->Start((MIDIDriverRef)g_driver, list);
    });
    return status;
}

static OSStatus stop(void)
{
    __block OSStatus status;

    on_server(^{
      status = (*g_d)->Stop((MIDIDriverRef)g_driver);
    });
    return status;
}

/* Sends MIDI bytes as the server does to a version 3 driver: universal packets, one message per
   list, from its I/O thread (this one), with the destination's refCon. */
static int app_sends(const uint8_t *bytes, size_t len)
{
    static const ua101_midi_event cut_event = {UA101_MIDI_SYSEX_CUT, {0}, 0};
    ua101_midi_parser parser;
    ua101_ump_packer packer;
    void *ref1 = S.o[g_dest].ref1;
    size_t i;
    int ok = 1;

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
            ok = packet != NULL &&
                 (*g_d)->SendPackets((MIDIDriverRef)g_driver, &list, ref1, &g_ref2) == noErr;
        }
    }
    return ok;
}

/* As the server sends to a version 2 driver: plain MIDI bytes, the SysEx in pieces. */
static int app_sends_v2(const uint8_t *bytes, size_t len, size_t piece)
{
    Byte buffer[1024];
    MIDIPacketList *list = (MIDIPacketList *)buffer;
    MIDIPacket *packet = MIDIPacketListInit(list);
    size_t i;

    for (i = 0; i < len && packet != NULL; i += piece) {
        size_t n = len - i < piece ? len - i : piece;

        packet = MIDIPacketListAdd(list, sizeof(buffer), packet, 0, n, bytes + i);
    }
    return packet != NULL &&
           (*g_d)->Send((MIDIDriverRef)g_driver, list, S.o[g_dest].ref1, &g_ref2) == noErr;
}

/* --- The cases --- */

static void test_the_com_object(void)
{
    CFUUIDRef wrong = CFUUIDCreateFromString(NULL, CFSTR("00000000-0000-0000-0000-000000000001"));
    void *out = NULL;
    HRESULT r;

    CHECK(UA101MIDI_Create(NULL, wrong) == NULL, "the factory makes nothing for another type");
    CFRelease(wrong);
    g_driver = UA101MIDI_Create(NULL, kMIDIDriverTypeID);
    g_d = g_driver;
    CHECK(g_driver != NULL && *g_d != NULL, "the factory makes a driver for the MIDI driver type");
    if (g_driver == NULL)
        exit(1);
    r = (*g_d)->QueryInterface(g_driver, CFUUIDGetUUIDBytes(kMIDIDriverInterfaceID), &out);
    CHECK(r == E_NOINTERFACE && out == NULL, "version 1 (FindDevices) is not offered");
    r = (*g_d)->QueryInterface(g_driver, CFUUIDGetUUIDBytes(IUnknownUUID), &out);
    CHECK(r == S_OK && out == g_driver, "IUnknown is");
    r = (*g_d)->QueryInterface(g_driver, CFUUIDGetUUIDBytes(kMIDIDriverInterface2ID), &out);
    CHECK(r == S_OK && out == g_driver && plugin().version == 2, "version 2 is");
    r = (*g_d)->QueryInterface(g_driver, CFUUIDGetUUIDBytes(kMIDIDriverInterface3ID), &out);
    CHECK(r == S_OK && out == g_driver && plugin().version == 3, "version 3 is, and is remembered");
    CHECK(plugin().refs == 4, "each answer counted a reference (%u)", plugin().refs);
    CHECK((*g_d)->Release(g_driver) == 3 && (*g_d)->Release(g_driver) == 2 &&
              (*g_d)->Release(g_driver) == 1,
          "and Release gives them back");
    CHECK((*g_d)->AddRef(g_driver) == 2 && (*g_d)->Release(g_driver) == 1, "AddRef and Release pair");
}

static void test_start_makes_the_device(void)
{
    int e;
    double cpu;
    uint64_t attaches;

    S.taken_id = 0;
    CHECK(start(LIST_EMPTY) == noErr, "Start, with no device kept from before");
    CHECK(S.creates == 1 && S.adds == 1 && S.count == 4,
          "one device made and added to the setup, with one entity and two endpoints");
    g_device = 0;
    e = S.o[g_device].children[0];
    g_source = S.o[e].children[0];
    g_dest = S.o[e].children[1];
    CHECK(strcmp(S.o[g_device].name, "UA-101") == 0 &&
              strcmp(S.o[g_device].manufacturer, "EDIROL") == 0 &&
              strcmp(S.o[g_device].model, "UA-101") == 0,
          "the device is \"%s\" by \"%s\", model \"%s\"", S.o[g_device].name,
          S.o[g_device].manufacturer, S.o[g_device].model);
    CHECK(strcmp(S.o[e].name, "MIDI") == 0 && strcmp(S.o[g_source].name, "MIDI") == 0 &&
              strcmp(S.o[g_dest].name, "MIDI") == 0,
          "its entity, MIDI IN and MIDI OUT are \"MIDI\": apps show \"UA-101 MIDI\", as the agent's");
    CHECK(S.o[g_source].unique_id == ua101_midi_port_id("UA-101 MIDI", 0, 0) &&
              S.o[g_dest].unique_id == ua101_midi_port_id("UA-101 MIDI", 1, 0),
          "with the agent's fixed IDs, so apps find the port they remember");
    CHECK(S.o[g_dest].ref1 == g_driver, "MIDI OUT carries the driver as its refCon");
    CHECK(offline_of(g_device) == 1, "no unit: the device is offline");
    CHECK(WAIT(plugin().attaches >= 1, 8.0), "the plug-in asked the helper for the unit");
    CHECK(plugin().running && !plugin().attached, "running, not attached");
    attaches = plugin().attaches;
    cpu = cpu_seconds();
    usleep(1000000);
    cpu = cpu_seconds() - cpu;
    CHECK(plugin().attaches == attaches, "and it doesn't keep asking: the helper will say");
    CHECK(cpu < 0.05, "a second of waiting for a unit used %.3f s of processor time", cpu);
    CHECK(offline_of(g_device) == 1, "still offline");
}

static void test_unit_comes(void)
{
    unit_comes();
    CHECK(WAIT(offline_of(g_device) == 0, 5.0), "the unit appears: the device goes online");
    CHECK(plugin().attached && atomic_load(&P.opens) == 1,
          "the helper holds the unit's MIDI interface for it, opened once");
}

static void test_app_to_unit(void)
{
    static const uint8_t note[] = {0x90, 0x3C, 0x40};
    static const uint8_t sysex[] = {0xF0, 0x7D, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0xF7};
    static const uint8_t own[] = {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x04, 0x00,
                                  0x07, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x71, 0xF7};
    static const uint8_t off[] = {0x80, 0x3C, 0x00};
    static const uint8_t gs_reset[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00,
                                       0x7F, 0x00, 0x41, 0xF7};
    static const uint8_t cc[] = {0xB0, 0x07, 0x64};
    int before = wire_bytes();
    uint64_t own_before = service().gate.own_sysex;

    CHECK(app_sends(note, sizeof(note)), "an app sends a note to \"UA-101 MIDI\"");
    CHECK(WAIT(wire_bytes() == before + 3, 3.0) && wire_ends(note, sizeof(note)),
          "it reaches the unit's MIDI OUT socket");
    CHECK(app_sends(sysex, sizeof(sysex)), "an app sends SysEx of another maker");
    CHECK(WAIT(wire_bytes() == before + 3 + (int)sizeof(sysex), 3.0) && wire_ends(sysex, sizeof(sysex)),
          "it arrives whole, in order");
    CHECK(app_sends(own, sizeof(own)) && app_sends(off, sizeof(off)),
          "an app sends a volume change addressed to the UA-101 itself, then a note off");
    CHECK(WAIT(wire_bytes() == before + 6 + (int)sizeof(sysex), 3.0) && wire_ends(off, sizeof(off)),
          "only the note off arrives (%d bytes in all)", wire_bytes() - before);
    CHECK(service().gate.own_sysex == own_before + 1 && atomic_load(&P.wire_other_packets) == 0,
          "the helper dropped the one for the UA-101, and nothing went to any other cable");

    /* As a version 2 driver: bytes in packets, a SysEx cut across them. */
    before = wire_bytes();
    CHECK(app_sends_v2(gs_reset, sizeof(gs_reset), 4) && app_sends_v2(cc, sizeof(cc), 3),
          "version 2: a GS reset in pieces of 4 bytes (Roland, another model), then a control change");
    CHECK(WAIT(wire_bytes() == before + (int)sizeof(gs_reset) + 3, 3.0) && wire_ends(cc, sizeof(cc)),
          "both arrive whole (%d bytes)", wire_bytes() - before);
    before = wire_bytes();
    CHECK(app_sends_v2(own, sizeof(own), 5) && app_sends_v2(off, sizeof(off), 3),
          "version 2: the message for the UA-101 in pieces of 5 bytes, then a note off");
    CHECK(WAIT(wire_bytes() == before + 3, 3.0) && wire_ends(off, sizeof(off)),
          "only the note off arrives (%d bytes)", wire_bytes() - before);
    CHECK(service().gate.own_sysex == own_before + 2, "the helper dropped the second one too");
    CHECK(plugin().out_refused == 0 && plugin().out_unattached == 0, "the plug-in refused nothing");
}

static void test_app_sends_what_midi_1_cannot_carry(void)
{
    MIDIEventList list;
    MIDIEventPacket *packet = MIDIEventListInit(&list, kMIDIProtocol_2_0);
    /* A MIDI 1.0 note in a MIDI 2.0 list: it would pass if the list's protocol were not read. */
    uint32_t words[2] = {0x20903C40, 0};
    int before = wire_bytes();
    uint64_t refused = plugin().out_refused;

    packet = MIDIEventListAdd(&list, sizeof(list), packet, 0, 1, words);
    CHECK(packet != NULL &&
              (*g_d)->SendPackets((MIDIDriverRef)g_driver, &list, S.o[g_dest].ref1, &g_ref2) == noErr,
          "the server hands over a MIDI 2.0 list");
    CHECK(WAIT(plugin().out_refused == refused + 1, 2.0), "it is refused and counted");
    /* Zeros after the packet, so a read past its end would find a SysEx that passes. */
    memset(&list, 0, sizeof(list));
    packet = MIDIEventListInit(&list, kMIDIProtocol_1_0);
    words[0] = 0x30160102; /* a SysEx7 packet of two words, cut to one */
    packet = MIDIEventListAdd(&list, sizeof(list), packet, 0, 1, words);
    CHECK(packet != NULL &&
              (*g_d)->SendPackets((MIDIDriverRef)g_driver, &list, S.o[g_dest].ref1, &g_ref2) == noErr,
          "a packet shorter than its message");
    CHECK(WAIT(plugin().out_refused == refused + 2, 2.0), "is refused and counted too");
    packet = MIDIEventListInit(&list, kMIDIProtocol_1_0);
    words[0] = 0x20903C40; /* a whole note on */
    packet = MIDIEventListAdd(&list, sizeof(list), packet, 0, 1, words);
    CHECK(packet != NULL &&
              (*g_d)->SendPackets((MIDIDriverRef)g_driver, &list, (void *)0x1234, &g_ref2) == noErr,
          "a note for a destination that isn't ours");
    usleep(200000);
    CHECK(wire_bytes() == before, "nothing of them reached MIDI OUT");
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

    got_reset();
    unit_sends(read, sizeof(read), stamp);
    CHECK(WAIT(got_count() >= 4, 3.0), "what arrives at MIDI IN reaches the server (%zu words)",
          got_count());
    usleep(100000);
    os_unfair_lock_lock(&S.lock);
    for (i = 0; i < S.word_count && i < 4; i++) {
        same += S.words[i] == want[i];
        stamped += S.stamps[i] == stamp;
    }
    CHECK(S.word_count == 4 && same == 4,
          "a note, a clock byte and a SysEx, and nothing of the control cable (%zu words, %zu "
          "right)",
          S.word_count, same);
    CHECK(stamped == 4, "each stamped with the time the USB read completed (%zu of 4)", stamped);
    CHECK(S.received_from_other == 0, "all on the device's own MIDI IN");
    os_unfair_lock_unlock(&S.lock);
    CHECK(plugin().in_errors == 0, "the server took every message");

    /* A server that refuses a message: counted, and the next one still goes. */
    os_unfair_lock_lock(&S.lock);
    S.receive_status = kMIDIUnknownError;
    os_unfair_lock_unlock(&S.lock);
    unit_sends(read, 4, 0);
    CHECK(WAIT(plugin().in_errors == 1, 2.0), "a message the server refuses is counted");
    os_unfair_lock_lock(&S.lock);
    S.receive_status = noErr;
    os_unfair_lock_unlock(&S.lock);
    got_reset();
    unit_sends(read, 4, 0);
    CHECK(WAIT(got_count() == 1, 2.0), "and the next one goes");
}

static void test_unit_goes_and_comes(void)
{
    static const uint8_t note[] = {0x90, 0x40, 0x41};
    static const uint8_t read[] = {0x08, 0x80, 0x40, 0x00};
    int before, changes;
    double cpu;

    cpu = cpu_seconds();
    usleep(1000000);
    cpu = cpu_seconds() - cpu;
    CHECK(cpu < 0.05, "a second attached with nothing to do used %.3f s of processor time", cpu);

    unit_goes();
    CHECK(WAIT(offline_of(g_device) == 1, 5.0), "the unit goes: the device is offline");
    CHECK(WAIT(!plugin().attached && atomic_load(&P.closes) == 1, 3.0),
          "and the helper gave the interface back");
    before = wire_bytes();
    CHECK(app_sends(note, sizeof(note)), "an app still sends to the offline device");
    CHECK(WAIT(plugin().out_unattached == 3, 2.0) && wire_bytes() == before,
          "it is dropped and counted, nothing reaches the unit");

    unit_comes();
    CHECK(WAIT(offline_of(g_device) == 0, 5.0), "the unit comes back: online again");
    CHECK(S.creates == 1, "the same device: none made again");
    before = wire_bytes();
    CHECK(app_sends(note, sizeof(note)) && WAIT(wire_bytes() == before + 3, 3.0),
          "and notes pass again");
    got_reset();
    unit_sends(read, sizeof(read), 0);
    CHECK(WAIT(got_count() == 1, 3.0), "and MIDI IN reaches the server again");

    /* The Mac sleeps and wakes with the unit on: the device stays online throughout. */
    changes = offline_changes_of(g_device);
    {
        uint64_t attaches = plugin().attaches;

        on_service_queue(^{
          W.events.will_sleep();
        });
        on_service_queue(^{
          W.events.did_wake();
        });
        CHECK(WAIT(plugin().attaches == attaches + 1 && plugin().attached, 5.0),
              "after a sleep and wake the plug-in asked again and is attached");
        usleep(100000);
        CHECK(offline_changes_of(g_device) == changes && offline_of(g_device) == 0,
              "and the device never went offline: apps never saw it go");
    }
}

static void test_stop_and_start_again(void)
{
    static const uint8_t note[] = {0x90, 0x41, 0x42};
    int before;

    CHECK(stop() == noErr, "Stop");
    CHECK(offline_of(g_device) == 1, "the device is offline at once");
    CHECK(WAIT(service().midi_clients == 0 && !service().port_open, 3.0),
          "the helper has no MIDI client left and gave the interface back");
    CHECK(!plugin().running && !plugin().attached, "not running");
    before = wire_bytes();
    app_sends(note, sizeof(note));
    usleep(200000);
    CHECK(wire_bytes() == before, "a send after Stop reaches nothing");
    CHECK(stop() == noErr, "a second Stop does nothing");

    /* The server starts the driver again with the device it kept, here named as the first
       version (the trial of 2026-11-17) named it. */
    {
        int e = S.o[g_device].children[0];
        int32_t in_id = S.o[g_source].unique_id, out_id = S.o[g_dest].unique_id;

        os_unfair_lock_lock(&S.lock);
        snprintf(S.o[e].name, sizeof(S.o[e].name), "UA-101 MIDI");
        snprintf(S.o[g_source].name, sizeof(S.o[g_source].name), "UA-101 MIDI");
        snprintf(S.o[g_dest].name, sizeof(S.o[g_dest].name), "UA-101 MIDI");
        os_unfair_lock_unlock(&S.lock);
        g_list[0] = g_device;
        CHECK(start(LIST_OF(1)) == noErr, "Start again, given the device kept from before");
        CHECK(S.creates == 1 && S.adds == 1, "no device made or added again");
        CHECK(strcmp(S.o[e].name, "MIDI") == 0 && strcmp(S.o[g_source].name, "MIDI") == 0 &&
                  strcmp(S.o[g_dest].name, "MIDI") == 0,
              "the kept device's entity, MIDI IN and MIDI OUT are named \"MIDI\" again");
        CHECK(S.o[g_source].unique_id == in_id && S.o[g_dest].unique_id == out_id,
              "with their IDs unchanged");
    }
    CHECK(WAIT(offline_of(g_device) == 0 && plugin().attached, 5.0),
          "the unit is on: online again, attached");
    before = wire_bytes();
    CHECK(app_sends(note, sizeof(note)) && WAIT(wire_bytes() == before + 3, 3.0), "notes pass");
    CHECK(start(LIST_OF(1)) == noErr && S.creates == 1, "a second Start does nothing");
}

/* A second driver made while the agent's IDs are taken: it takes the next ones. Also a kept
   device with no endpoints is left offline and a whole one made. */
static void test_ids_taken_and_a_broken_device(void)
{
    void *second = UA101MIDI_Create(NULL, kMIDIDriverTypeID);
    MIDIDriverInterface **d2 = second;
    int broken, e;
    __block OSStatus status;

    os_unfair_lock_lock(&S.lock);
    broken = new_object(KIND_DEVICE, -1, "UA-101");
    os_unfair_lock_unlock(&S.lock);
    g_list[0] = broken;
    stop();
    on_server(^{
      status = (*d2)->Start((MIDIDriverRef)second, LIST_OF(1));
    });
    CHECK(status == noErr, "a second driver starts, given a kept device without MIDI IN and OUT");
    CHECK(S.creates == 2 && offline_of(broken) == 1,
          "it makes a whole device and leaves the broken one offline");
    /* Made last: the device, its entity, MIDI IN, MIDI OUT. */
    e = S.count - 3;
    CHECK(S.o[S.count - 4].kind == KIND_DEVICE && S.o[S.count - 4].children[0] == e &&
              S.o[e].children[0] == S.count - 2 && S.o[e].children[1] == S.count - 1,
          "the new device is whole");
    CHECK(S.o[S.count - 2].unique_id == ua101_midi_port_id("UA-101 MIDI", 0, 1) &&
              S.o[S.count - 1].unique_id == ua101_midi_port_id("UA-101 MIDI", 1, 1),
          "the fixed IDs being taken by the first device, it took the next ones");
    CHECK(WAIT(offline_of(S.count - 4) == 0, 5.0), "and goes online: the unit is on");
    on_server(^{
      (*d2)->Stop((MIDIDriverRef)second);
    });
    CHECK(offline_of(S.count - 4) == 1, "stopped: offline");
}

/* CoreMIDI fills in its property names only when it starts up for a client or inside the MIDI
   server (measured 2026-11-17: all NULL in a program that has made no MIDI call). The plug-in
   runs inside the server, where they are set; here, with no server, they are set by hand. */
static void name_the_properties(void)
{
    static const struct {
        const CFStringRef *name;
        CFStringRef text;
    } names[] = {
        {&kMIDIPropertyName, CFSTR("name")},
        {&kMIDIPropertyOffline, CFSTR("offline")},
        {&kMIDIPropertyUniqueID, CFSTR("uniqueID")},
        {&kMIDIPropertyImage, CFSTR("image")},
    };
    size_t i;

    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (*names[i].name == NULL)
            /* Volatile: a plain store to a const object may be dropped by the compiler. */
            *(CFStringRef volatile *)(uintptr_t)names[i].name = names[i].text;
}

int main(void)
{
    mach_timebase_info_data_t tb;
    ua101_service_options so;
    xpc_endpoint_t endpoint;
    dispatch_semaphore_t ready = dispatch_semaphore_create(0);
    char log_path[64];
    int fd;

    setvbuf(stdout, NULL, _IOLBF, 0);
    mach_timebase_info(&tb);
    g_tick_s = (double)tb.numer / (double)tb.denom / 1e9;

    snprintf(log_path, sizeof(log_path), "/tmp/ua101-test-midiplugin-XXXXXX");
    fd = mkstemp(log_path);
    g_log = fd >= 0 ? fdopen(fd, "w") : NULL;
    if (g_log == NULL) {
        printf("FAIL: no log file\n");
        return 1;
    }
    /* The helper's service, in this process, before anything could reach the installed one. */
    memset(&so, 0, sizeof(so));
    so.input_offset = -1;
    so.output_offset = -1;
    so.watch = 1;
    so.allow_uid = (int)getuid();
    so.log = g_log;
    endpoint = ua101_service_start_anonymous(&so);
    if (endpoint == NULL) {
        printf("FAIL: the service didn't start\n");
        return 1;
    }
    ua101_midilink_use_endpoint(endpoint);
    ua101_midiplugin_test_note = test_note;
    name_the_properties();

    pthread_create(&g_server_thread, NULL, server_main, ready);
    dispatch_semaphore_wait(ready, DISPATCH_TIME_FOREVER);

    test_the_com_object();
    test_start_makes_the_device();
    test_unit_comes();
    test_app_to_unit();
    test_app_sends_what_midi_1_cannot_carry();
    test_unit_to_app();
    test_unit_goes_and_comes();
    test_stop_and_start_again();
    test_ids_taken_and_a_broken_device();

    CHECK(S.off_thread == 0, "the device was changed only on the server's main thread (%d not)",
          S.off_thread);
    CHECK(WAIT(service().midi_clients == 0 && !service().port_open, 3.0),
          "at the end the helper has no MIDI client and gave the interface back");

    ua101_service_shutdown();
    fclose(g_log);
    if (failures != 0) {
        char command[128];

        printf("The helper's and the plug-in's log:\n");
        snprintf(command, sizeof(command), "cat %s", log_path);
        if (system(command) != 0)
            printf("(not readable)\n");
    }
    unlink(log_path);
    printf("test_midiplugin: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
