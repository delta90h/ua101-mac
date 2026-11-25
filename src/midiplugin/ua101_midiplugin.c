/*
 * UA101MIDI.plugin: the UA-101's MIDI IN and OUT sockets as a MIDI device, "UA-101" by EDIROL,
 * in Audio MIDI Setup's MIDI Studio and in every MIDI app (option (B)).
 *
 * macOS's MIDI server (one per logged-in user, running as that user) loads this bundle from
 * /Library/Audio/MIDI Drivers and calls it through the CoreMIDI driver interface
 * (CoreMIDI/MIDIDriver.h). It replaces the per-user agent's two virtual ports
 * (src/ua101_midiports.c) and does the same job the same way: it is a MIDI client of the
 * helper (src/ua101_midilink.h), which owns the unit and decides what may reach it
 * (src/ua101_midigate.h). It has no USB code at all (the Makefile checks IOKit isn't linked and
 * that none of the helper's port or gate code is in it), and it decides nothing about what may
 * be sent: it turns the server's packets into MIDI bytes and the helper's bytes into packets.
 *
 * The device: one entity, "UA-101 MIDI", with one source (MIDI IN) and one destination (MIDI
 * OUT). It is made once and the MIDI server keeps it from then on; it is shown offline (grey)
 * while the helper says there is no unit, and online while the helper holds the unit for us.
 * Its two endpoints carry the agent's fixed IDs (ua101_midi_port_id), so an app that remembered
 * "UA-101 MIDI" finds it again.
 *
 * Threads. The server calls Start, Stop and the COM functions on its main thread, and Send or
 * SendPackets on its own I/O thread. Everything about the link happens on one serial queue of
 * ours; the device is changed only on the server's main run loop, as MIDIDriver.h asks. A fault
 * here would stop all MIDI for the user until the server restarts, so the code is kept small and
 * nothing is freed that another thread might still hold: the driver's own block is never freed.
 */
#include <CoreFoundation/CFPlugInCOM.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>
#include <CoreMIDI/MIDIDriver.h>

#include <dispatch/dispatch.h>
#include <mach/mach_time.h>
#include <os/log.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../ua101_link.h"
#include "../ua101_midi.h"
#include "../ua101_midilink.h"
#include "ua101_midiplugin.h"

/* The same waits as the agent's when the helper can't say when to ask again. */
#define RETRY_SOON_SECONDS 10
#define RETRY_LATER_SECONDS 60
#define RETRIES_SOON 6
/* MIDI bytes from apps are handed to the helper in pieces of at most this. */
#define OUT_CHUNK_BYTES 1024

typedef struct plugin {
    /* First: a MIDIDriverRef points at this field. */
    MIDIDriverInterface *interface;
    _Atomic uint32_t refs;
    _Atomic int version; /* the driver interface the server asked for: 2 or 3 (0 not yet) */

    /* The server's main thread only. */
    CFRunLoopRef runloop;
    MIDIDeviceRef device;
    int device_running; /* between Start and Stop; blocks on the run loop check it */
    int offline_set;    /* the value last given to the device, -1 for none yet */

    /* Written at Start before the link opens, read on the queue. */
    MIDIEndpointRef source, dest;

    dispatch_queue_t queue; /* the link and everything marked Q */
    ua101_midilink *link;   /* Q */
    int running, attached, gave_up;  /* Q */
    unsigned retries, retry_serial;  /* Q */
    char said[200];                  /* Q: the last state logged, so it is logged once */
    ua101_midi_parser rx_parser;     /* Q */
    ua101_ump_packer rx_packer;      /* Q */
    ua101_midiplugin_counts counts;  /* Q, but for the atomics below */
    atomic_ullong out_refused;       /* counted on the server's I/O thread */
} plugin;

static os_log_t g_log;

#ifdef UA101_MIDIPLUGIN_TESTING
void (*ua101_midiplugin_test_note)(const char *text);
#endif

static void note(plugin *p, const char *format, ...) __attribute__((format(printf, 2, 3)));

static void note(plugin *p, const char *format, ...)
{
    char text[sizeof(p->said)];
    va_list args;

    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    /* Lines come from the server's main thread and from our queue: the log is made once. */
    static dispatch_once_t once;
    dispatch_once(&once, ^{
      g_log = os_log_create("io.github.delta90h.ua101midi", "plugin");
    });
    os_log(g_log, "%{public}s", text);
#ifdef UA101_MIDIPLUGIN_TESTING
    if (ua101_midiplugin_test_note != NULL)
        ua101_midiplugin_test_note(text);
#endif
}

/* A state worth a line, written only when it differs from the last one written. On the queue. */
static void note_state(plugin *p, const char *format, ...) __attribute__((format(printf, 2, 3)));

static void note_state(plugin *p, const char *format, ...)
{
    char text[sizeof(p->said)];
    va_list args;

    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    if (strcmp(text, p->said) == 0)
        return;
    snprintf(p->said, sizeof(p->said), "%s", text);
    note(p, "%s", text);
}

/* ---- The device, on the server's main run loop ---- */

static void set_offline_now(plugin *p, int offline)
{
    if (!p->device_running || p->device == 0 || p->offline_set == offline)
        return;
    if (MIDIObjectSetIntegerProperty(p->device, kMIDIPropertyOffline, offline) == noErr)
        p->offline_set = offline;
}

/* From the queue: the device shows the unit's state, on the server's run loop. */
static void show_offline(plugin *p, int offline)
{
    CFRunLoopRef loop = p->runloop;

    if (loop == NULL)
        return;
    CFRunLoopPerformBlock(loop, kCFRunLoopCommonModes, ^{
      set_offline_now(p, offline);
    });
    CFRunLoopWakeUp(loop);
}

/* Gives a new endpoint the agent's fixed ID, so apps that remember "UA-101 MIDI" find it. If
   that ID is taken it tries a few others; failing all, it keeps the server's own. */
static void set_endpoint_id(plugin *p, MIDIEndpointRef endpoint, int is_destination)
{
    unsigned attempt;

    for (attempt = 0; attempt < 8; attempt++) {
        int32_t id = ua101_midi_port_id(UA101_MIDIPLUGIN_PORT_NAME, is_destination, attempt);
        OSStatus status = MIDIObjectSetIntegerProperty(endpoint, kMIDIPropertyUniqueID, id);

        if (status == noErr) {
            if (attempt != 0)
                note(p, "the %s's usual ID was taken by another MIDI object: it has ID %d now",
                     is_destination ? "MIDI OUT" : "MIDI IN", (int)id);
            return;
        }
        if (status != kMIDIIDNotUnique)
            break;
    }
    note(p, "the %s keeps the MIDI server's own ID: the fixed one couldn't be set",
         is_destination ? "MIDI OUT" : "MIDI IN");
}

/* The device's picture: a file in this bundle, if the bundle is found. */
static void set_picture(MIDIDeviceRef device)
{
    CFBundleRef bundle = CFBundleGetBundleWithIdentifier(CFSTR(UA101_MIDIPLUGIN_BUNDLE_ID));
    CFURLRef url = bundle != NULL ? CFBundleCopyResourceURL(bundle, CFSTR("UA-101"), CFSTR("png"),
                                                            NULL)
                                  : NULL;
    CFStringRef path;

    if (url == NULL)
        return;
    path = CFURLCopyFileSystemPath(url, kCFURLPOSIXPathStyle);
    if (path != NULL) {
        MIDIObjectSetStringProperty(device, kMIDIPropertyImage, path);
        CFRelease(path);
    }
    CFRelease(url);
}

/* An entity of ours with one source and one destination at least. */
static MIDIEntityRef usable_entity(MIDIDeviceRef device)
{
    MIDIEntityRef entity;

    if (device == 0 || MIDIDeviceGetNumberOfEntities(device) < 1)
        return 0;
    entity = MIDIDeviceGetEntity(device, 0);
    if (entity == 0 || MIDIEntityGetNumberOfSources(entity) < 1 ||
        MIDIEntityGetNumberOfDestinations(entity) < 1)
        return 0;
    return entity;
}

/* The entity's and endpoints' names, on a new device and on one kept from an earlier version. */
static void name_ports(MIDIEntityRef entity)
{
    CFStringRef name = CFSTR(UA101_MIDIPLUGIN_ENDPOINT_NAME);

    MIDIObjectSetStringProperty(entity, kMIDIPropertyName, name);
    MIDIObjectSetStringProperty(MIDIEntityGetSource(entity, 0), kMIDIPropertyName, name);
    MIDIObjectSetStringProperty(MIDIEntityGetDestination(entity, 0), kMIDIPropertyName, name);
}

/* Finds the device the server kept from an earlier run, or makes it. Returns its entity. */
static MIDIEntityRef find_or_make_device(plugin *p, MIDIDeviceListRef list)
{
    ItemCount i, n = list != 0 ? MIDIDeviceListGetNumberOfDevices(list) : 0;
    MIDIDeviceRef device = 0;
    MIDIEntityRef entity = 0;
    OSStatus status;

    /* The list holds only this driver's devices. The first whole one is used; any other is
       left as it is, offline. */
    for (i = 0; i < n; i++) {
        MIDIDeviceRef d = MIDIDeviceListGetDevice(list, i);
        MIDIEntityRef e = usable_entity(d);

        if (entity == 0 && e != 0) {
            device = d;
            entity = e;
        } else if (d != 0) {
            MIDIObjectSetIntegerProperty(d, kMIDIPropertyOffline, 1);
        }
    }
    if (entity != 0) {
        name_ports(entity);
        p->device = device;
        return entity;
    }

    status = MIDIDeviceCreate((MIDIDriverRef)p, CFSTR(UA101_MIDIPLUGIN_DEVICE_NAME),
                              CFSTR("EDIROL"), CFSTR("UA-101"), &device);
    if (status != noErr) {
        note(p, "couldn't make the MIDI device (error %d)", (int)status);
        return 0;
    }
    status = MIDIDeviceNewEntity(device, CFSTR(UA101_MIDIPLUGIN_ENDPOINT_NAME), kMIDIProtocol_1_0,
                                 false, 1, 1, &entity);
    if (status != noErr || usable_entity(device) == 0) {
        note(p, "couldn't give the MIDI device its MIDI IN and OUT (error %d)", (int)status);
        MIDIDeviceDispose(device);
        return 0;
    }
    entity = usable_entity(device);
    name_ports(entity);
    set_endpoint_id(p, MIDIEntityGetSource(entity, 0), 0);
    set_endpoint_id(p, MIDIEntityGetDestination(entity, 0), 1);
    status = MIDISetupAddDevice(device);
    if (status != noErr) {
        note(p, "the MIDI server didn't take the device (error %d)", (int)status);
        MIDIDeviceDispose(device);
        return 0;
    }
    note(p, "made the MIDI device \"%s\" with MIDI IN and OUT \"%s\"", UA101_MIDIPLUGIN_DEVICE_NAME,
         UA101_MIDIPLUGIN_PORT_NAME);
    p->device = device;
    return entity;
}

/* ---- Unit to apps, on the queue ---- */

/* One message per event list: SysEx must travel in packets of its own. */
static void deliver(plugin *p, const uint32_t *words, size_t count, uint64_t host_time)
{
    MIDIEventList list;
    MIDIEventPacket *packet;
    OSStatus status;

    if (count == 0 || p->source == 0)
        return;
    packet = MIDIEventListInit(&list, kMIDIProtocol_1_0);
    packet = MIDIEventListAdd(&list, sizeof(list), packet, host_time, count, words);
    status = packet != NULL ? MIDIReceivedEventList(p->source, &list) : kMIDIUnknownError;
    if (status == noErr) {
        p->counts.in_messages++;
        return;
    }
    if (p->counts.in_errors++ == 0)
        note(p, "the MIDI server didn't take a message from MIDI IN (error %d)", (int)status);
}

static void from_unit(plugin *p, const uint8_t *bytes, size_t len, uint64_t host_time)
{
    static const ua101_midi_event cut_event = {UA101_MIDI_SYSEX_CUT, {0}, 0};
    size_t i;

    if (host_time == 0)
        host_time = mach_absolute_time();
    p->counts.in_bytes += len;
    for (i = 0; i < len; i++) {
        ua101_midi_event ev;
        uint32_t words[2];
        int cut;

        ua101_midi_parse(&p->rx_parser, bytes[i], &ev, &cut);
        if (cut)
            deliver(p, words, ua101_ump_pack(&p->rx_packer, &cut_event, words), host_time);
        deliver(p, words, ua101_ump_pack(&p->rx_packer, &ev, words), host_time);
    }
}

/* ---- Apps to unit ---- */

/* On the server's I/O thread: the bytes step onto the queue, and from there to the helper. */
static void hand_over(plugin *p, const uint8_t *bytes, size_t len)
{
    uint8_t *copy;

    if (len == 0 || (copy = malloc(len)) == NULL)
        return;
    memcpy(copy, bytes, len);
    dispatch_async(p->queue, ^{
      if (p->running && p->attached && p->link != NULL && ua101_midilink_send(p->link, copy, len))
          p->counts.out_bytes += len;
      else
          p->counts.out_unattached += len;
      free(copy);
    });
}

/* Version 3: universal packets. MIDI 1.0 only; turned into MIDI bytes and nothing more. */
static void from_event_list(plugin *p, const MIDIEventList *list)
{
    uint8_t bytes[OUT_CHUNK_BYTES];
    const MIDIEventPacket *packet = &list->packet[0];
    size_t used = 0;
    UInt32 i;

    if (list->protocol != kMIDIProtocol_1_0) {
        atomic_fetch_add(&p->out_refused, list->numPackets);
        return;
    }
    for (i = 0; i < list->numPackets; i++) {
        UInt32 w = 0;

        while (w < packet->wordCount) {
            size_t count = ua101_ump_words(packet->words[w]), n;
            uint8_t message[8];
            int refused;

            if (w + count > packet->wordCount) {
                atomic_fetch_add(&p->out_refused, 1);
                break;
            }
            n = ua101_ump_to_bytes(&packet->words[w], count, message, &refused);
            if (refused)
                atomic_fetch_add(&p->out_refused, 1);
            if (used + n > sizeof(bytes)) {
                hand_over(p, bytes, used);
                used = 0;
            }
            memcpy(bytes + used, message, n);
            used += n;
            w += (UInt32)count;
        }
        packet = MIDIEventPacketNext(packet);
    }
    hand_over(p, bytes, used);
}

/* Version 2: packets of MIDI bytes already. */
static void from_packet_list(plugin *p, const MIDIPacketList *list)
{
    const MIDIPacket *packet = &list->packet[0];
    UInt32 i;

    for (i = 0; i < list->numPackets; i++) {
        hand_over(p, packet->data, packet->length);
        packet = MIDIPacketNext(packet);
    }
}

/* ---- Following the helper, on the queue ---- */

static void ask_later(plugin *p)
{
    unsigned serial = ++p->retry_serial;
    int seconds = p->retries++ < RETRIES_SOON ? RETRY_SOON_SECONDS : RETRY_LATER_SECONDS;

    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)seconds * (int64_t)NSEC_PER_SEC),
                   p->queue, ^{
                     if (p->running && !p->gave_up && !p->attached && serial == p->retry_serial)
                         ua101_midilink_attach(p->link);
                   });
}

static void on_attached(void *context, int status, int watching, const char *why)
{
    plugin *p = context;

    p->counts.attaches++;
    if (!p->running)
        return;
    if (status == UA101_LINK_OK) {
        p->attached = 1;
        p->retries = 0;
        ua101_midi_parser_init(&p->rx_parser);
        ua101_ump_packer_init(&p->rx_packer);
        show_offline(p, 0);
        note_state(p, "the UA-101 is there: \"%s\" is online, in and out",
                   UA101_MIDIPLUGIN_PORT_NAME);
        return;
    }
    p->attached = 0;
    show_offline(p, 1);
    if (status == UA101_LINK_ERR_DENIED) {
        /* Another user of this Mac than the one the helper serves: nothing to do, ever. */
        p->gave_up = 1;
        note_state(p, "this user may not use the UA-101 (%s): the device stays offline", why);
        return;
    }
    if (status == UA101_LINK_ERR_ABSENT && watching) {
        /* No unit on the bus: the helper says when one appears. Nothing runs meanwhile. */
        note_state(p, "no UA-101 is connected: the device is offline until it is");
        return;
    }
    note_state(p, "the UA-101's MIDI can't be had now (%s): offline, asking again later", why);
    ask_later(p);
}

static void on_midi(void *context, const uint8_t *bytes, size_t len, uint64_t host_time)
{
    plugin *p = context;

    if (p->running && p->attached)
        from_unit(p, bytes, len, host_time);
}

/* The unit came, went or was lost, the Mac woke, or the helper started again. The device stays
   as it is until the helper has answered: if the unit is still there, apps never see it go. */
static void on_changed(void *context)
{
    plugin *p = context;

    if (!p->running || p->gave_up)
        return;
    p->attached = 0;
    p->retry_serial++;
    ua101_midilink_attach(p->link);
}

/* ---- The driver interface ---- */

static plugin *self_of(void *self)
{
    return (plugin *)self;
}

static ULONG plugin_add_ref(void *self)
{
    return atomic_fetch_add(&self_of(self)->refs, 1) + 1;
}

/* The block is never freed: the server's threads may still be on their way into it, and it is
   one small block for the life of the server. */
static ULONG plugin_release(void *self)
{
    plugin *p = self_of(self);
    uint32_t refs = atomic_load(&p->refs);

    while (refs != 0 && !atomic_compare_exchange_weak(&p->refs, &refs, refs - 1)) {
    }
    return refs != 0 ? refs - 1 : 0;
}

static HRESULT plugin_query_interface(void *self, REFIID iid, LPVOID *out)
{
    plugin *p = self_of(self);
    CFUUIDRef want = CFUUIDCreateFromUUIDBytes(NULL, iid);
    int version = 0;

    if (out == NULL) {
        CFRelease(want);
        return E_POINTER;
    }
    if (CFEqual(want, kMIDIDriverInterface3ID))
        version = 3;
    else if (CFEqual(want, kMIDIDriverInterface2ID))
        version = 2;
    else if (CFEqual(want, IUnknownUUID))
        version = -1;
    CFRelease(want);
    if (version == 0) {
        /* Version 1 (FindDevices) is not offered. */
        *out = NULL;
        return E_NOINTERFACE;
    }
    if (version > 0)
        atomic_store(&p->version, version);
    plugin_add_ref(self);
    *out = self;
    return S_OK;
}

static OSStatus plugin_find_devices(MIDIDriverRef self, MIDIDeviceListRef list)
{
    (void)self;
    (void)list;
    return noErr;
}

static OSStatus plugin_start(MIDIDriverRef self, MIDIDeviceListRef list)
{
    plugin *p = self_of(self);
    static const ua101_midilink_events events = {on_attached, on_midi, on_changed};
    MIDIEntityRef entity;

    if (p->device_running)
        return noErr;
    if (p->runloop == NULL) {
        p->runloop = CFRunLoopGetCurrent();
        CFRetain(p->runloop);
    }
    entity = find_or_make_device(p, list);
    if (entity == 0)
        return kMIDIUnknownError;
    p->source = MIDIEntityGetSource(entity, 0);
    p->dest = MIDIEntityGetDestination(entity, 0);
    MIDIEndpointSetRefCons(p->dest, p, NULL);
    set_picture(p->device);
    p->device_running = 1;
    p->offline_set = -1;
    set_offline_now(p, 1); /* until the helper says the unit is there */

    dispatch_sync(p->queue, ^{
      p->running = 1;
      p->attached = 0;
      p->gave_up = 0;
      p->retries = 0;
      p->retry_serial++;
      p->said[0] = 0;
      p->link = ua101_midilink_open(p->queue, &events, p);
      if (p->link == NULL) {
          p->running = 0;
          return;
      }
      note(p, "started (driver interface %d): asking the helper for the UA-101",
           atomic_load(&p->version));
      ua101_midilink_attach(p->link);
    });
    return p->running ? noErr : kMIDIUnknownError;
}

static OSStatus plugin_stop(MIDIDriverRef self)
{
    plugin *p = self_of(self);

    if (!p->device_running)
        return noErr;
    dispatch_sync(p->queue, ^{
      p->running = 0;
      p->attached = 0;
      p->retry_serial++;
      if (p->link != NULL)
          ua101_midilink_close(p->link);
      p->link = NULL;
      note(p, "stopped: the helper has the UA-101's MIDI back");
    });
    set_offline_now(p, 1);
    p->device_running = 0;
    return noErr;
}

static OSStatus plugin_configure(MIDIDriverRef self, MIDIDeviceRef device)
{
    (void)self;
    (void)device;
    return noErr;
}

static OSStatus plugin_send(MIDIDriverRef self, const MIDIPacketList *list, void *ref1, void *ref2)
{
    plugin *p = self_of(self);

    (void)ref2;
    if (list == NULL || ref1 != p)
        return noErr;
    from_packet_list(p, list);
    return noErr;
}

static OSStatus plugin_send_packets(MIDIDriverRef self, const MIDIEventList *list, void *ref1,
                                    void *ref2)
{
    plugin *p = self_of(self);

    (void)ref2;
    if (list == NULL || ref1 != p)
        return noErr;
    from_event_list(p, list);
    return noErr;
}

static OSStatus plugin_enable_source(MIDIDriverRef self, MIDIEndpointRef source, Boolean enabled)
{
    (void)self;
    (void)source;
    (void)enabled;
    return noErr;
}

/* Nothing is scheduled here: the server keeps every message until it is due (no advance
   schedule time is set), and what the helper queues it paces itself. */
static OSStatus plugin_flush(MIDIDriverRef self, MIDIEndpointRef dest, void *ref1, void *ref2)
{
    (void)self;
    (void)dest;
    (void)ref1;
    (void)ref2;
    return noErr;
}

static OSStatus plugin_monitor(MIDIDriverRef self, MIDIEndpointRef dest, const MIDIPacketList *list)
{
    (void)self;
    (void)dest;
    (void)list;
    return noErr;
}

static OSStatus plugin_monitor_events(MIDIDriverRef self, MIDIEndpointRef dest,
                                      const MIDIEventList *list)
{
    (void)self;
    (void)dest;
    (void)list;
    return noErr;
}

static MIDIDriverInterface g_interface = {
    NULL, /* _reserved */
    plugin_query_interface,
    plugin_add_ref,
    plugin_release,
    plugin_find_devices,
    plugin_start,
    plugin_stop,
    plugin_configure,
    plugin_send,
    plugin_enable_source,
    plugin_flush,
    plugin_monitor,
    plugin_send_packets,
    plugin_monitor_events,
};

__attribute__((visibility("default"))) void *UA101MIDI_Create(CFAllocatorRef allocator,
                                                              CFUUIDRef type)
{
    plugin *p;

    (void)allocator;
    if (type == NULL || !CFEqual(type, kMIDIDriverTypeID))
        return NULL;
    p = calloc(1, sizeof(*p));
    if (p == NULL)
        return NULL;
    p->interface = &g_interface;
    atomic_store(&p->refs, 1);
    p->offline_set = -1;
    p->queue = dispatch_queue_create("io.github.delta90h.ua101midi", DISPATCH_QUEUE_SERIAL);
    /* Counted once and never taken back, as the block is never freed: CFPlugIn then never
       unloads this code under the server's threads. */
    {
        CFUUIDRef factory = CFUUIDCreateFromString(NULL, CFSTR(UA101_MIDIPLUGIN_FACTORY));

        if (factory != NULL) {
            CFPlugInAddInstanceForFactory(factory);
            CFRelease(factory);
        }
    }
    return p;
}

#ifdef UA101_MIDIPLUGIN_TESTING
void ua101_midiplugin_get_counts(void *driver, ua101_midiplugin_counts *out)
{
    plugin *p = self_of(driver);

    dispatch_sync(p->queue, ^{
      *out = p->counts;
      out->attached = p->attached;
      out->running = p->running;
    });
    out->out_refused = atomic_load(&p->out_refused);
    out->version = atomic_load(&p->version);
    out->refs = atomic_load(&p->refs);
}
#endif
