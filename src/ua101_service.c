#include "ua101_service.h"

#include <dispatch/dispatch.h>
#include <fcntl.h>
#include <math.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "ua101_control.h"
#include "ua101_ctlmsg.h"
#include "ua101_engine.h"
#include "ua101_link.h"
#include "ua101_midilamp.h"
#include "ua101_midiport.h"
#include "ua101_shared.h"
#include "ua101_watch.h"

/* How often a streaming engine is checked for having stopped by itself. */
#define WATCH_MS 200
/* The USB thread bumps the block's heartbeat every 10 ms and stops by itself after 1 s without
   a transfer. If the heartbeat stands still this long and it has not stopped, the thread is
   stuck inside a call and can't be joined: the helper exits, launchd starts it again on the
   plug-in's next message, and the plug-in sees a new block and restarts I/O. */
#define STUCK_SECONDS 4.0
/* The limit the clock is reported against. It is the same in every test; streaming is judged
   on its own (a design choice, 2026-11-07). */
#define CLOCK_LIMIT_PPM 100.0
/* A stream that fails by itself this soon after starting, this many times in a row, isn't
   tried again until the unit leaves the bus and comes back (switched off and on, replugged) or
   the Mac sleeps and wakes. Without this, a unit that is on the bus but won't stream (Roland
   says theirs needs switching off and on after the Mac slept during playback) would be opened,
   started and lost about once a second for ever, because each failure makes the plug-in ask
   again. Only with the bus watched, since the unit coming back is what ends it. */
#define SHORT_RUN_SECONDS 10.0
#define GIVE_UP_FAILURES 3

/* Levels: at most this much audio is looked at for one answer, in chunks of this many frames. */
#define LEVEL_WINDOW_SECONDS 0.25
#define LEVEL_CHUNK_FRAMES 1024
/* Messages passed on to the unit in any one second, at most. The app's engine sends a read
   when it attaches, one after a front-panel switch moved and one when asked to read again;
   a patch bay change or a pause of the saving is rarer still. A volume drag is the one
   thing that sends many: the app spaces its messages so that it stays under this
   (PanelModel.unitGapMicros in app/PanelModel.swift). */
#define CTL_SENDS_PER_SECOND 20
/* A write for the MIDI OUT socket that the unit didn't take in time (the port waits a quarter
   of a second at most): what was waiting is thrown away and nothing is tried for this long,
   so a unit that takes nothing can't hold up the service queue write after write. */
#define MIDI_WRITE_HOLD_US 1000000u

typedef struct peer {
    xpc_connection_t conn;
    uid_t uid;
    int started;
    /* The control app's side. */
    int control;  /* it has sent a control request: it is told "changed" and "audio" its own way */
    int attached; /* between ctl_open and ctl_close: it is sent what the unit says */
    uint64_t level_block, capture_seen, playback_seen; /* where its last levels ended */
    /* The MIDI client's side. */
    int midi;          /* it has sent a MIDI request: it is told "changed" */
    int midi_attached; /* between midi_open and midi_close: it is sent what arrives at MIDI IN */
    ua101_midifeed feed; /* where its byte stream for MIDI OUT stands */
    struct peer *next;
} peer;

static struct {
    ua101_service_options opt;
    dispatch_queue_t queue;
    xpc_connection_t listener;
    peer *peers;
    uid_t coreaudio_uid;

    ua101_engine *engine;
    ua101_engine_format format;
    ua101_shared *shared;
    size_t shared_bytes;
    xpc_object_t shmem;
    uint64_t block_id, opens;
    uint32_t input_offset, output_offset; /* frames, at the open unit's rate */
    int streaming;
    dispatch_source_t watch;
    uint32_t watch_heartbeat;
    unsigned watch_still; /* watches in a row in which the heartbeat didn't move */
    unsigned idle_generation;
    unsigned close_generation;
    /* What the plug-in counted in the run it is stopping, when it said. */
    int plugin_counts;
    uint64_t plugin_missing, plugin_clipped;
    /* From a plug-in that measures them: the largest clipped sample, and what the rate
       correction's own result went over at. Sizes where full scale is 1.0. */
    int plugin_has_peak, plugin_has_cut;
    double plugin_peak, plugin_cut_peak;
    uint64_t plugin_cut;
    /* And the safety offsets its run used, with the least room recording had, in frames. */
    int plugin_has_offsets, plugin_has_spare;
    uint32_t plugin_input_offset, plugin_output_offset;
    int64_t plugin_capture_spare;

    /* The bus and the Mac's sleep (src/ua101_watch.h). */
    int watching;  /* the bus is watched: an event will come when a unit appears or goes */
    int units;     /* UA-101s on the bus, while watching */
    int announced; /* the last hello was answered "present", and nothing was said since */
    int asleep;    /* between "will sleep" and "woke": the unit stays closed */
    int resume;    /* streaming was on when the Mac went to sleep */
    unsigned short_failures; /* streams in a row that failed within SHORT_RUN_SECONDS */

    /* The unit's MIDI interface, held while a control app is attached (src/ua101_midiport.h). */
    uid_t own_uid;
    ua101_midiport *port;
    ua101_sysex_rx rx;       /* the control cable's messages, put back together */
    ua101_frame_view capture_view, playback_view; /* for levels; valid while views_ok */
    int views_ok;
    time_t ctl_second;       /* the second ctl_sends_now counts */
    /* For Load, Store and Reset (link version 17): the last whole settings block the unit
       sent, and when (0: none since the interface was opened, or a switch
       on the unit moved since); when a change was last passed on; when a Load, Store or Reset
       last was. On the helper's own clock (memory_now_us). */
    uint8_t block[UA101_CTL_BLOCK_BYTES];
    uint64_t block_us, change_us, memory_us;
    unsigned ctl_sends_now;
    ua101_service_counts counts;

    /* MIDI clients: the way to the MIDI OUT socket (src/ua101_midigate.h). Its counts are for
       the time the interface is held; `counts.gate` has those of the times before. */
    ua101_midigate gate;
    dispatch_source_t midi_timer; /* comes back every tick while packets wait */
    int midi_timer_on;
    int midi_session;             /* a MIDI client attached while the interface was held */
    unsigned midi_said;           /* what the log has said once, this time */
    uint64_t midi_hold_until_us;  /* after a write the unit didn't take */
    uint64_t midi_in_session;     /* bytes from MIDI IN passed on, this time */

    /* The control app's MIDI lamps (src/ua101_midilamp.h): a one-shot wake-up, booked only
       while MIDI seen within the last 0.1 s waits to be told. */
    ua101_midilamp lamp;
    dispatch_source_t lamp_timer;
    int lamp_timer_on;
    uint64_t lamp_wake_us; /* when it is booked for, while lamp_timer_on */
} S;

/* Levels are worked out here, a chunk at a time. Only used on the service queue. */
static int32_t g_level_frames[LEVEL_CHUNK_FRAMES * UA101_LINK_MAX_CHANNELS];

static void tell_control(const char *event, const uint8_t *data, size_t bytes);
static uint64_t memory_now_us(void);
static void midi_timer_set(int on);
static void lamp_step(unsigned flags);
static void lamp_timer_set(uint64_t wake_at_us);

void ua101_service_stamp(char *out, size_t bytes, time_t seconds, long microseconds)
{
    char when[32];
    struct tm tm;

    localtime_r(&seconds, &tm);
    strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tm);
    snprintf(out, bytes, "%s.%03ld", when, microseconds < 0 ? 0 : (microseconds / 1000) % 1000);
}

long long ua101_service_trim_log(int fd, long long limit, long long keep)
{
    struct stat st;
    char *tail, *start, *newline;
    long long removed;
    ssize_t got;
    int flags;

    if (fd < 0 || keep <= 0 || keep >= limit || fstat(fd, &st) != 0)
        return -1;
    if (!S_ISREG(st.st_mode) || (long long)st.st_size <= limit)
        return 0;
    tail = malloc((size_t)keep);
    if (tail == NULL)
        return -1;
    got = pread(fd, tail, (size_t)keep, st.st_size - keep);
    if (got != (ssize_t)keep) {
        free(tail);
        return -1;
    }
    /* Start after the first line break, so the kept part begins with a whole line. */
    newline = memchr(tail, '\n', (size_t)got);
    start = newline != NULL ? newline + 1 : tail;
    got -= start - tail;
    if (ftruncate(fd, 0) != 0 || pwrite(fd, start, (size_t)got, 0) != got) {
        free(tail);
        return -1;
    }
    free(tail);
    removed = (long long)st.st_size - (long long)got;
    lseek(fd, 0, SEEK_END);
    flags = fcntl(fd, F_GETFL);
    if (flags != -1)
        fcntl(fd, F_SETFL, flags | O_APPEND);
    return removed;
}

/* Every line carries the time to the millisecond, so the spacing of the messages of a drag
   can be read from the log (the 2026-11-12 drag couldn't show it: whole seconds). */
static void say(const char *format, ...)
{
    char when[40];
    struct timeval now;
    va_list args;

    if (S.opt.log == NULL)
        return;
    gettimeofday(&now, NULL);
    ua101_service_stamp(when, sizeof(when), now.tv_sec, (long)now.tv_usec);
    fprintf(S.opt.log, "%s ua101d: ", when);
    va_start(args, format);
    vfprintf(S.opt.log, format, args);
    va_end(args);
    fputc('\n', S.opt.log);
    fflush(S.opt.log);
}

/*
 * What the run that just stopped lost, so a real app's session leaves a verdict in the log.
 * Two lines: streaming, judged on its own, and the unit's clock against the unchanged limit.
 */
/* " (highest +0.21 dB)" for a size where full scale is 1.0, or nothing if nothing was over. */
static void highest(char *text, size_t bytes, uint64_t count, double peak)
{
    text[0] = '\0';
    if (count == 0 || !(peak > 0.0))
        return;
    if (isfinite(peak))
        snprintf(text, bytes, " (highest %+.2f dB)", 20.0 * log10(peak));
    else
        snprintf(text, bytes, " (highest: not a finite number)");
}

static double say_verdict(void)
{
    ua101_engine_summary s;
    const char *failure = ua101_engine_failure(S.engine);
    char plugin[400];
    int pass;

    ua101_engine_summarize(S.engine, &s);
    if (S.plugin_counts) {
        char peak[48], cut[160] = "", cut_peak[48];

        /* Neither is a fault of the stream. The first are the apps' own samples, which no
           24-bit unit can take; the second is where the wave the rate correction worked out
           passed over full scale between samples that were within it. */
        highest(peak, sizeof(peak), S.plugin_has_peak ? S.plugin_clipped : 0, S.plugin_peak);
        if (S.plugin_has_cut) {
            highest(cut_peak, sizeof(cut_peak), S.plugin_cut, S.plugin_cut_peak);
            snprintf(cut, sizeof(cut), "; the rate correction's own result went over at %llu "
                                       "samples%s",
                     (unsigned long long)S.plugin_cut, cut_peak);
        }
        snprintf(plugin, sizeof(plugin), "the plug-in found %llu capture frames missing; the "
                                         "apps sent %llu samples at or over full scale%s%s",
                 (unsigned long long)S.plugin_missing, (unsigned long long)S.plugin_clipped,
                 peak, cut);
    } else {
        snprintf(plugin, sizeof(plugin), "the plug-in gave no counts");
    }
    pass = failure == NULL && s.errors == 0 && s.capture_lost_frames == 0 &&
           s.playback_late_frames == 0 && (!S.plugin_counts || S.plugin_missing == 0);
    say("streaming %s: %.1f s; capture %llu frames, %llu lost in %llu gaps; playback %llu "
        "frames, %llu late; %llu USB errors, %llu resyncs; %s%s%s",
        pass ? "PASS" : "FAIL", s.seconds, (unsigned long long)s.capture_frames,
        (unsigned long long)s.capture_lost_frames, (unsigned long long)s.capture_gaps,
        (unsigned long long)s.playback_frames, (unsigned long long)s.playback_late_frames,
        (unsigned long long)s.errors, (unsigned long long)s.resyncs, plugin,
        failure != NULL ? "; stopped because " : "", failure != NULL ? failure : "");
    if (s.clock_seconds > 0.0)
        say("clock: the unit ran %+.1f ppm against the Mac's clock over %.0f s (limit %.0f "
            "ppm: %s)",
            s.clock_ppm, s.clock_seconds, CLOCK_LIMIT_PPM,
            fabs(s.clock_ppm) <= CLOCK_LIMIT_PPM ? "within" : "outside");
    else
        say("clock: not measured, the run was too short");
    /* The room the two delays left at the run's worst moment, for the choice of them
       (src/ua101_drvset.h). Not part of the verdict: a run with no room left has lost frames,
       and those are counted above. */
    if (S.plugin_counts && S.plugin_has_offsets && s.rate_hz > 0.0) {
        char recording[64] = "not measured", playback[64] = "not measured";

        if (S.plugin_has_spare)
            snprintf(recording, sizeof(recording), "%+.2f ms",
                     (double)S.plugin_capture_spare * 1000.0 / s.rate_hz);
        if (s.playback_spare_known)
            snprintf(playback, sizeof(playback), "%+.2f ms",
                     (double)s.playback_spare_frames * 1000.0 / s.rate_hz);
        say("room: recording read %u frames behind (%.2f ms), playback written %u ahead (%.2f "
            "ms); the least room left: recording %s, playback %s",
            S.plugin_input_offset, S.plugin_input_offset * 1000.0 / s.rate_hz,
            S.plugin_output_offset, S.plugin_output_offset * 1000.0 / s.rate_hz, recording,
            playback);
    }
    S.plugin_counts = 0;
    return s.seconds;
}

static int given_up(void)
{
    return S.watching && S.short_failures >= GIVE_UP_FAILURES;
}

static void stop_streaming(void)
{
    double seconds;

    if (!S.streaming)
        return;
    if (S.watch != NULL) {
        dispatch_source_cancel(S.watch);
        dispatch_release(S.watch);
        S.watch = NULL;
    }
    ua101_engine_stop(S.engine);
    S.streaming = 0;
    tell_control("audio", NULL, 0);
    say("streaming stopped");
    seconds = say_verdict();
    if (ua101_engine_failure(S.engine) != NULL && seconds < SHORT_RUN_SECONDS) {
        if (++S.short_failures == GIVE_UP_FAILURES && S.watching)
            say("streaming failed %d times in a row, each within %.0f s of starting: not trying "
                "again until the unit is switched off and on, or the Mac sleeps and wakes",
                GIVE_UP_FAILURES, SHORT_RUN_SECONDS);
    } else {
        S.short_failures = 0;
    }
}

/* Gives back the unit and the block. A plug-in still holding the block keeps its own mapping. */
static void close_unit(void)
{
    peer *p;

    stop_streaming();
    S.views_ok = 0;
    for (p = S.peers; p != NULL; p = p->next)
        p->started = 0;
    if (S.engine != NULL) {
        ua101_engine_close(S.engine);
        S.engine = NULL;
        say("unit closed");
    }
    if (S.shmem != NULL) {
        xpc_release(S.shmem);
        S.shmem = NULL;
    }
    if (S.shared != NULL) {
        munlock(S.shared, S.shared_bytes);
        munmap(S.shared, S.shared_bytes);
        S.shared = NULL;
    }
}

static int open_unit(char *why, size_t why_bytes)
{
    ua101_engine_options eopt;
    uint64_t ring_frames;
    size_t bytes;
    void *block;
    long page = sysconf(_SC_PAGESIZE);

    if (S.engine != NULL)
        return 1;
    if (given_up()) {
        snprintf(why, why_bytes, "the unit is connected but streaming failed %d times in a row: "
                                 "switch it off and on",
                 GIVE_UP_FAILURES);
        return 0;
    }
    memset(&eopt, 0, sizeof(eopt));
    eopt.capture_queue = S.opt.capture_queue;
    eopt.playback_queue = S.opt.playback_queue;
    S.engine = ua101_engine_open(&eopt, &S.format, S.opt.log, why, why_bytes);
    if (S.engine == NULL)
        return 0;
    if (S.format.playback_channels == 0) {
        snprintf(why, why_bytes, "the unit has no playback at this rate");
        close_unit();
        return 0;
    }
    ring_frames = ua101_shared_ring_frames(S.format.rate_hz);
    bytes = ua101_shared_bytes(S.format.capture_channels * 4, S.format.playback_channels * 4,
                               ring_frames, (size_t)page);
    block = bytes != 0 ? mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_ANON | MAP_SHARED, -1, 0)
                       : MAP_FAILED;
    if (block == MAP_FAILED) {
        snprintf(why, why_bytes, "no memory for the shared block");
        close_unit();
        return 0;
    }
    S.shared = block;
    S.shared_bytes = bytes;
    if (!ua101_shared_init(S.shared, bytes, S.format.rate_hz, S.format.capture_channels,
                           S.format.playback_channels, ring_frames, (size_t)page)) {
        snprintf(why, why_bytes, "the shared block couldn't be set up");
        close_unit();
        return 0;
    }
    mlock(S.shared, bytes);
    S.shmem = xpc_shmem_create(S.shared, bytes);
    if (S.shmem == NULL) {
        snprintf(why, why_bytes, "the shared block couldn't be shared");
        close_unit();
        return 0;
    }
    S.block_id = ((uint64_t)getpid() << 32) | (++S.opens & 0xFFFFFFFFu);
    /* For levels: the helper's own checked views, as the engine takes for itself, since the
       plug-in can write to the block too. */
    S.views_ok = S.format.capture_channels <= UA101_LINK_MAX_CHANNELS &&
                 S.format.playback_channels <= UA101_LINK_MAX_CHANNELS &&
                 ua101_shared_views(S.shared, bytes, S.format.capture_channels,
                                    S.format.playback_channels, &S.capture_view,
                                    &S.playback_view);
    S.input_offset = S.opt.input_offset >= 0
                         ? (uint32_t)S.opt.input_offset
                         : ua101_offset_frames(UA101_INPUT_OFFSET_SECONDS, S.format.rate_hz);
    S.output_offset = S.opt.output_offset >= 0
                          ? (uint32_t)S.opt.output_offset
                          : ua101_offset_frames(UA101_OUTPUT_OFFSET_SECONDS, S.format.rate_hz);
    /* The plug-in may use others, by the user's settings (src/ua101_drvset.h): each run's
       "room" line says which it used. */
    say("unit open: %.0f Hz, %u in, %u out; shared block %zu KB; the offsets to start from: "
        "input %u frames (%.2f ms), output %u frames (%.2f ms)",
        S.format.rate_hz, S.format.capture_channels, S.format.playback_channels, bytes / 1024,
        S.input_offset, S.input_offset * 1000.0 / S.format.rate_hz, S.output_offset,
        S.output_offset * 1000.0 / S.format.rate_hz);
    return 1;
}

/* To the plug-ins. A control app is told in its own words (tell_control), and a MIDI client
   in its own (tell_midi_changed). */
static void tell_all(const char *event)
{
    peer *p;

    for (p = S.peers; p != NULL; p = p->next) {
        xpc_object_t m;

        if (p->control || p->midi)
            continue;
        m = xpc_dictionary_create(NULL, NULL, 0);
        xpc_dictionary_set_string(m, UA101_LINK_EVENT, event);
        xpc_connection_send_message(p->conn, m);
        xpc_release(m);
    }
}

/* --- The control app's side: the unit's MIDI interface, and levels. --- */

/* To every control app; with `data`, only to those attached. */
static void tell_control(const char *event, const uint8_t *data, size_t bytes)
{
    peer *p;

    for (p = S.peers; p != NULL; p = p->next) {
        xpc_object_t m;

        if (!p->control || (data != NULL && !p->attached))
            continue;
        m = xpc_dictionary_create(NULL, NULL, 0);
        xpc_dictionary_set_string(m, UA101_LINK_EVENT, event);
        if (data != NULL)
            xpc_dictionary_set_data(m, UA101_LINK_DATA, data, bytes);
        xpc_dictionary_set_bool(m, UA101_LINK_STREAMING, S.streaming);
        xpc_connection_send_message(p->conn, m);
        xpc_release(m);
    }
}

/* To every MIDI client: something changed (the unit came, went or was lost, or the Mac woke),
   so it is no longer attached and asks again. */
static void tell_midi_changed(void)
{
    peer *p;

    for (p = S.peers; p != NULL; p = p->next) {
        xpc_object_t m;

        if (!p->midi)
            continue;
        m = xpc_dictionary_create(NULL, NULL, 0);
        xpc_dictionary_set_string(m, UA101_LINK_EVENT, "changed");
        xpc_connection_send_message(p->conn, m);
        xpc_release(m);
    }
}

/* What arrived at the MIDI IN socket, to the MIDI clients attached now, and to nobody else. */
static void tell_midi(const uint8_t *midi, size_t bytes, uint64_t host_time)
{
    peer *p;
    int any = 0;

    for (p = S.peers; p != NULL; p = p->next) {
        xpc_object_t m;

        if (!p->midi_attached)
            continue;
        m = xpc_dictionary_create(NULL, NULL, 0);
        xpc_dictionary_set_string(m, UA101_LINK_EVENT, "midi");
        xpc_dictionary_set_data(m, UA101_LINK_DATA, midi, bytes);
        xpc_dictionary_set_uint64(m, UA101_LINK_TIME, host_time);
        xpc_connection_send_message(p->conn, m);
        xpc_release(m);
        any = 1;
    }
    if (any) {
        S.counts.midi_in_events++;
        S.counts.midi_in_bytes += bytes;
        S.midi_in_session += bytes;
    }
}

/* A control app or a MIDI client: the interface is held while either is attached. */
static int anyone_attached(void)
{
    peer *p;

    for (p = S.peers; p != NULL; p = p->next)
        if (p->attached || p->midi_attached)
            return 1;
    return 0;
}

static void add_gate_counts(ua101_midigate_counts *to, const ua101_midigate_counts *from)
{
    to->bytes_in += from->bytes_in;
    to->invalid_bytes += from->invalid_bytes;
    to->sysex_cut += from->sysex_cut;
    to->own_sysex += from->own_sysex;
    to->overflow_messages += from->overflow_messages;
    to->busy_messages += from->busy_messages;
    to->packets_queued += from->packets_queued;
    to->packets_sent += from->packets_sent;
    to->bytes_sent += from->bytes_sent;
    to->gate_wrong_cable += from->gate_wrong_cable;
    to->gate_own_sysex += from->gate_own_sysex;
    to->gate_malformed += from->gate_malformed;
    to->flushed_packets += from->flushed_packets;
}

/* One line for the time the interface was held with a MIDI client: what passed, what didn't. */
static void say_midi_totals(void)
{
    const ua101_midigate_counts *c = &S.gate.counts;
    char not_sent[400] = "";
    size_t at = 0;

#define NOT_SENT(count, ...)                                                                      \
    do {                                                                                          \
        if ((count) != 0 && at < sizeof(not_sent)) {                                              \
            at += (size_t)snprintf(not_sent + at, sizeof(not_sent) - at, "%s%llu ",               \
                                   at == 0 ? "; not sent: " : ", ", (unsigned long long)(count)); \
            if (at < sizeof(not_sent))                                                            \
                at += (size_t)snprintf(not_sent + at, sizeof(not_sent) - at, __VA_ARGS__);        \
        }                                                                                         \
    } while (0)
    NOT_SENT(c->own_sysex, "SysEx addressed to the UA-101 itself");
    NOT_SENT(c->overflow_messages, "messages with the queue full");
    NOT_SENT(c->busy_messages, "messages during another client's SysEx");
    NOT_SENT(c->invalid_bytes, "bytes MIDI doesn't allow");
    NOT_SENT(c->flushed_packets, "packets still waiting when the unit stopped taking them");
    NOT_SENT(c->gate_wrong_cable + c->gate_own_sysex + c->gate_malformed,
             "packets stopped by the last check");
#undef NOT_SENT
    say("MIDI clients: %llu bytes from MIDI IN passed on; %llu bytes handed over for MIDI OUT, "
        "%llu sent%s",
        (unsigned long long)S.midi_in_session, (unsigned long long)c->bytes_in,
        (unsigned long long)c->bytes_sent, not_sent);
}

static void close_port(void)
{
    if (S.port == NULL)
        return;
    midi_timer_set(0);
    lamp_timer_set(0);
    ua101_midilamp_init(&S.lamp);
    ua101_midigate_flush(&S.gate);
    ua101_midiport_close(S.port);
    S.port = NULL;
    say("MIDI interface given back");
    if (S.midi_session)
        say_midi_totals();
    add_gate_counts(&S.counts.gate, &S.gate.counts);
    ua101_midigate_init(&S.gate);
    S.midi_session = 0;
    S.midi_said = 0;
    S.midi_hold_until_us = 0;
    S.midi_in_session = 0;
}

static void midi_detach(peer *p)
{
    if (!p->midi_attached)
        return;
    p->midi_attached = 0;
    ua101_midigate_release(&S.gate, &p->feed);
}

/* The interface is gone, or going: nobody is attached any more, and each control app and each
   MIDI client is told to ask again. */
static void detach_all(int tell)
{
    peer *p;
    int any = 0, any_midi = 0;

    for (p = S.peers; p != NULL; p = p->next) {
        any |= p->attached;
        p->attached = 0;
        any_midi |= p->midi_attached;
        midi_detach(p);
    }
    close_port();
    if (tell && any)
        tell_control("changed", NULL, 0);
    if (tell && any_midi)
        tell_midi_changed();
}

/* What the unit sent, on the service queue, with the time the USB read completed. The
   control cable's messages go to control apps only (ua101_sysex_rx_feed keeps cable 1 alone);
   the MIDI IN socket's bytes, cable 0, go to MIDI clients only (ua101_midisplit gives out
   cable 0 alone); any other cable goes nowhere. */
static void port_packets(void *context, const uint8_t *data, size_t bytes, uint64_t host_time)
{
    enum { SLICE_PACKETS = 128 };
    uint8_t midi[3 * SLICE_PACKETS];
    size_t i;
    int in_lamp = 0;

    (void)context;
    for (i = 0; i + 4 <= bytes; i += 4) {
        const uint8_t *msg = NULL;
        size_t len = 0;

        if ((data[i] >> 4) == 0 && (data[i] & 0x0F) >= 2)
            S.counts.midi_packets++;
        if (ua101_sysex_rx_feed(&S.rx, data + i, &msg, &len)) {
            ua101_ctl_view view;
            ua101_ctl_kind kind = ua101_ctl_classify(msg, len, &view);

            S.counts.ctl_received++;
            tell_control("ctl", msg, len);
            /* For Load, Store and Reset: the whole settings block, as the unit sent it to the
               control app that asked, is kept with its time (the "fresh read" of
               ua101_service_memory_passes, and the record written to the log before such a
               command goes out). A switch that moved on the unit makes the block kept here an
               old one. */
            if (kind == UA101_CTL_SETTINGS && view.data_len == UA101_CTL_BLOCK_BYTES) {
                memcpy(S.block, view.data, sizeof(S.block));
                S.block_us = memory_now_us();
            } else if (kind == UA101_CTL_CHANGE) {
                S.block_us = 0;
            }
        }
    }
    for (i = 0; i + 4 <= bytes; i += 4 * SLICE_PACKETS) {
        size_t n = bytes - i < 4 * SLICE_PACKETS ? bytes - i : 4 * SLICE_PACKETS;
        size_t got = ua101_midisplit(data + i, n, midi, sizeof(midi), &S.counts.split);

        if (got != 0) {
            tell_midi(midi, got, host_time);
            in_lamp |= ua101_midilamp_bytes_count(midi, got);
        }
    }
    if (in_lamp)
        lamp_step(UA101_MIDILAMP_IN);
}

static void port_lost(void *context, const char *why)
{
    (void)context;
    say("the MIDI interface stopped answering (%s)", why);
    detach_all(1);
}

static int open_port(char *why, size_t why_bytes)
{
    static const ua101_midiport_events events = {port_packets, port_lost};

    if (S.port != NULL)
        return 1;
    S.port = ua101_midiport_open(S.queue, &events, NULL, S.opt.log, why, why_bytes);
    if (S.port == NULL)
        return 0;
    ua101_sysex_rx_init(&S.rx);
    ua101_midigate_init(&S.gate);
    ua101_midilamp_init(&S.lamp);
    /* The unit may have been switched off and on since: what was read before doesn't count
       for a Load, Store or Reset. (When one was last passed on is kept.) */
    S.block_us = 0;
    say("MIDI interface open. From a control app these are passed on: the settings request, "
        "\"pause saving\", \"resume saving\", the patch bay sources, "
        "and of the mixer's channels (MONITOR OUT, INPUT 1 to 10, OUTPUT 1 to 10) the volume up "
        "to +12 dB, the pan (0x0000 to 0x8000) and the on/off (0 or 1), of its pairs the stereo "
        "link (0 or 1), and of the INPUT and the OUTPUT group the solo (0 for none, or a "
        "channel 1 to 10); also "
        "Load and Store of Memory 1 to 5 and Reset (the settings in use, a memory, or "
        "everything): 37 exact messages, never while sound plays, only after a fresh read of "
        "the settings, at most one every %d seconds; and nothing else. From a "
        "MIDI client: MIDI for the MIDI OUT socket "
        "only, never the control cable, and no SysEx addressed to the UA-101 itself",
        UA101_MEMORY_GAP_SECONDS);
    return 1;
}

/* --- MIDI clients: the MIDI IN and OUT sockets (cable 0). --- */

static uint64_t now_us(void)
{
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000u;
}

/* Things worth a line in the log, each said once for the time the interface is held: the
   counts are in the line written when it is given back. */
static void midi_say_once(void)
{
    const ua101_midigate_counts *c = &S.gate.counts;

    if (c->own_sysex != 0 && !(S.midi_said & 1u)) {
        S.midi_said |= 1u;
        say("a MIDI app sent SysEx addressed to the UA-101 itself (F0 41 .. 00 69): from MIDI "
            "apps such messages are never passed on, so it was dropped, as later ones will be "
            "(said once)");
    }
    if (c->overflow_messages != 0 && !(S.midi_said & 2u)) {
        S.midi_said |= 2u;
        say("MIDI OUT: an app sent more than the MIDI wire takes (3,125 bytes a second) and "
            "the queue (%u KB) is full: what doesn't fit is dropped (said once)",
            (unsigned)(sizeof(S.gate.ring) / 1024));
    }
    if (c->gate_wrong_cable + c->gate_own_sysex + c->gate_malformed != 0 && !(S.midi_said & 4u)) {
        S.midi_said |= 4u;
        say("MIDI OUT: the last check before the port stopped packets that should never have "
            "got that far (%llu for another cable, %llu of SysEx for the UA-101 itself, %llu "
            "unsound); they were not sent (said once)",
            (unsigned long long)c->gate_wrong_cable, (unsigned long long)c->gate_own_sysex,
            (unsigned long long)c->gate_malformed);
    }
    if (c->busy_messages != 0 && !(S.midi_said & 16u)) {
        S.midi_said |= 16u;
        say("MIDI OUT: two MIDI clients sent at once; while one's SysEx is on its way the "
            "other's messages are dropped (said once)");
    }
}

/*
 * The second sender, beside send_to_unit and sharing nothing with it: what the gate lets out
 * for the MIDI OUT socket goes to the port here, and nowhere else. The gate gives out only
 * cable 0 packets it checked again, a few at a time and no faster than the MIDI wire, so the
 * write doesn't wait and neither do the service queue or a control app's messages.
 */
static void midi_pump(void)
{
    uint8_t packets[UA101_MIDI_PACKET_BYTES * UA101_MIDIGATE_BURST_BYTES];
    uint64_t now = now_us();
    size_t n;

    if (S.port == NULL)
        return;
    if (now < S.midi_hold_until_us) {
        ua101_midigate_flush(&S.gate);
        midi_timer_set(0);
        return;
    }
    n = ua101_midigate_take(&S.gate, now, packets, sizeof(packets));
    if (n != 0 && ua101_midiport_send(S.port, packets, n)) {
        /* Only what the unit took lights the OUT lamp: never bytes the gate dropped. */
        if (ua101_midilamp_packets_count(packets, n))
            lamp_step(UA101_MIDILAMP_OUT);
    } else if (n != 0) {
        S.counts.midi_write_failures++;
        ua101_midigate_flush(&S.gate);
        S.midi_hold_until_us = now_us() + MIDI_WRITE_HOLD_US;
        if (!(S.midi_said & 8u)) {
            S.midi_said |= 8u;
            say("MIDI OUT: the unit didn't take %zu bytes in time; what was waiting is dropped "
                "and nothing is sent for a second (said once)",
                n);
        }
    }
    midi_say_once();
    midi_timer_set(ua101_midigate_pending(&S.gate));
}

/* While packets wait the pump comes back every tick; otherwise no timer runs at all. */
static void midi_timer_set(int on)
{
    if (S.midi_timer == NULL) {
        if (!on)
            return;
        S.midi_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, S.queue);
        dispatch_source_set_timer(S.midi_timer, DISPATCH_TIME_FOREVER, DISPATCH_TIME_FOREVER, 0);
        dispatch_source_set_event_handler(S.midi_timer, ^{
          midi_pump();
        });
        dispatch_resume(S.midi_timer);
    }
    if (on == S.midi_timer_on)
        return;
    S.midi_timer_on = on;
    if (on)
        dispatch_source_set_timer(S.midi_timer,
                                  dispatch_time(DISPATCH_TIME_NOW,
                                                UA101_MIDIGATE_TICK_US * NSEC_PER_USEC),
                                  UA101_MIDIGATE_TICK_US * NSEC_PER_USEC, NSEC_PER_MSEC);
    else
        dispatch_source_set_timer(S.midi_timer, DISPATCH_TIME_FOREVER, DISPATCH_TIME_FOREVER, 0);
}

/*
 * The one way a MIDI client's bytes set out for the unit. They are MIDI bytes, not packets:
 * the gate parses them, packs them for cable 0, drops SysEx addressed to the UA-101 itself and
 * queues the rest (src/ua101_midigate.h). Nothing here goes near send_to_unit or the control
 * cable, and a control app's messages never come this way.
 */
static void midi_from_client(peer *p, const uint8_t *bytes, size_t len)
{
    ua101_midigate_feed(&S.gate, &p->feed, bytes, len);
    midi_pump();
}

/* --- The control app's MIDI IN and OUT lamps (link version 15, src/ua101_midilamp.h). --- */

/* To every attached control app: MIDI passed MIDI IN (`in`) or left through MIDI OUT (`out`).
   Only two flags: no MIDI bytes go to a control app this way. */
static void lamp_tell(unsigned flags)
{
    peer *p;
    int any = 0;

    for (p = S.peers; p != NULL; p = p->next) {
        xpc_object_t m;

        if (!p->control || !p->attached)
            continue;
        m = xpc_dictionary_create(NULL, NULL, 0);
        xpc_dictionary_set_string(m, UA101_LINK_EVENT, "midi_lamps");
        xpc_dictionary_set_bool(m, UA101_LINK_MIDI_IN, (flags & UA101_MIDILAMP_IN) != 0);
        xpc_dictionary_set_bool(m, UA101_LINK_MIDI_OUT, (flags & UA101_MIDILAMP_OUT) != 0);
        xpc_connection_send_message(p->conn, m);
        xpc_release(m);
        any = 1;
    }
    if (any)
        S.counts.midi_lamp_events++;
}

static int control_attached(void)
{
    peer *p;

    for (p = S.peers; p != NULL; p = p->next)
        if (p->control && p->attached)
            return 1;
    return 0;
}

/* MIDI passed (`flags`), or the wake-up came (0): tell now, or book the one wake-up. With no
   control app attached nothing new is remembered, so nothing is booked for it. */
static void lamp_step(unsigned flags)
{
    uint64_t wake_at;
    unsigned tell;

    if (flags != 0 && !control_attached())
        return;
    tell = ua101_midilamp_step(&S.lamp, flags, now_us(), &wake_at);
    if (tell != 0)
        lamp_tell(tell);
    lamp_timer_set(wake_at);
}

/* One one-shot wake-up at `wake_at_us`, or none (0). Never repeats by itself: while MIDI is
   quiet nothing is booked. */
static void lamp_timer_set(uint64_t wake_at_us)
{
    if (wake_at_us == 0) {
        if (S.lamp_timer_on) {
            S.lamp_timer_on = 0;
            dispatch_source_set_timer(S.lamp_timer, DISPATCH_TIME_FOREVER, DISPATCH_TIME_FOREVER, 0);
        }
        return;
    }
    if (S.lamp_timer == NULL) {
        S.lamp_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, S.queue);
        dispatch_source_set_timer(S.lamp_timer, DISPATCH_TIME_FOREVER, DISPATCH_TIME_FOREVER, 0);
        dispatch_source_set_event_handler(S.lamp_timer, ^{
          S.lamp_timer_on = 0;
          dispatch_source_set_timer(S.lamp_timer, DISPATCH_TIME_FOREVER, DISPATCH_TIME_FOREVER, 0);
          S.counts.midi_lamp_wakeups++;
          lamp_step(0);
        });
        dispatch_resume(S.lamp_timer);
    }
    if (S.lamp_timer_on && S.lamp_wake_us == wake_at_us)
        return;
    {
        uint64_t now = now_us();
        uint64_t wait = wake_at_us > now ? wake_at_us - now : 0;

        S.lamp_timer_on = 1;
        S.lamp_wake_us = wake_at_us;
        dispatch_source_set_timer(S.lamp_timer,
                                  dispatch_time(DISPATCH_TIME_NOW, (int64_t)(wait * NSEC_PER_USEC)),
                                  DISPATCH_TIME_FOREVER, 5 * NSEC_PER_MSEC);
    }
}

/* The two messages of the one control this version passes on, as docs/control-protocol.md
   gives them ("Suspend auto-save", "Resume auto-save"). Written out here because the helper
   holds no code that builds a message; tests/test_ctlhelper.c checks them against the app's
   builder. Their neighbours at the same address (00 00 00 01 and 00 00 00 02, which is
   "reset all") differ in one byte, so the match is byte for byte. */
static const uint8_t k_pause_saving[UA101_SAVING_MESSAGE_BYTES] = {
    0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF7};
static const uint8_t k_resume_saving[UA101_SAVING_MESSAGE_BYTES] = {
    0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x00, 0x00, 0x00, 0x01, 0x7F, 0xF7};

/* A patch bay source change this version passes on, judged twice without shared code: by
   what the parser says the message is, and by the 13 bytes themselves as
   docs/control-protocol.md gives them (F0 41 10 00 69 12, address 00 02 01 <output>, the
   source, the checksum, F7). The output pair must be one of UA101_PATCH_OUTPUTS_PASSED. */
static int patch_change_passes(const uint8_t *msg, size_t len, const ua101_msg *m)
{
    static const uint8_t head[9] = {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x02, 0x01};
    unsigned output, source;

    if (m->kind != UA101_MSG_FIELD || m->field.kind != UA101_F_PATCH || m->field.group != 0 ||
        m->field.index > 4 || !((UA101_PATCH_OUTPUTS_PASSED >> m->field.index) & 1u) ||
        m->value < 1 || m->value > 11)
        return 0;
    if (len != 13 || memcmp(msg, head, sizeof(head)) != 0 || msg[12] != 0xF7)
        return 0;
    output = msg[9];
    source = msg[10];
    if (output > 4 || !((UA101_PATCH_OUTPUTS_PASSED >> output) & 1u) || source < 1 || source > 11)
        return 0;
    if (output != m->field.index || source != m->value)
        return 0;
    return msg[11] == ((0x80u - ((0x02u + 0x01u + output + source) & 0x7Fu)) & 0x7Fu);
}

#ifdef UA101_SERVICE_TESTING
/* The helper test only (tests/test_ctlhelper.c): it leaves out one of the two value
   checks or one of the two channel checks of a volume (256, 512; 64, 128) or of a pan (1, 2;
   4, 8), or one of the two on/off checks (16, 32), link checks (1024, 2048) or solo checks
   (4096, 8192), to show that each alone keeps to the rule;
   so taking either out of this file makes the test fail. Never built into the helper. */
unsigned ua101_service_test_skip;
#define SKIPPED(bit) (ua101_service_test_skip & (bit))
/* The helper test only: the link's and the solo's lists, which the test makes shorter for a
   moment. With every pair and both groups on them, a check that forgot to ask its list would
   look the same as one that asks; with a shorter list it shows, and so does what happens to a
   link or a solo that is on no list (the general refusal). In the helper they are the
   constants of src/ua101_link.h. */
unsigned ua101_service_test_links = UA101_LINKS_PASSED, ua101_service_test_solos = UA101_SOLOS_PASSED;
#define LINKS_PASSED ua101_service_test_links
#define SOLOS_PASSED ua101_service_test_solos
/* The same for the list of Load, Store and Reset commands (bits 16384 and 32768 of the skips
   above leave out one of their two checks), and time for the helper's memory rules: the test
   adds to the helper's clock so that 5 and 30 seconds pass without waiting. */
uint64_t ua101_service_test_memory = UA101_MEMORY_PASSED;
uint64_t ua101_service_test_clock_us;
#define MEMORY_PASSED ua101_service_test_memory
#define CLOCK_SKEW_US ua101_service_test_clock_us
#else
#define SKIPPED(bit) 0
#define LINKS_PASSED UA101_LINKS_PASSED
#define SOLOS_PASSED UA101_SOLOS_PASSED
#define MEMORY_PASSED ((uint64_t)UA101_MEMORY_PASSED)
#define CLOCK_SKEW_US 0u
#endif

/* 1 if the six digits, the first one sent being the highest (proven on the unit, 2026-11-12:
  ), spell a volume that is at most the top of the unit's range (0x7FFFFF, about +12 dB):
   the first digit is at most 7 (the caller has seen that each digit is 0 to F). Up to link
   version 18 the top was 0 dB (0x200000). */
static int volume_digits_ok(const uint8_t d[6])
{
    return d[0] <= 7;
}

/* A volume change this version passes on, judged twice without shared code, as the patch
   bay change is: by what the parser says the message is, and by the 18 bytes themselves as
   docs/control-protocol.md gives them (F0 41 10 00 69 12, address 00 <group+3> <channel> 07,
   six digits each 0 to F, the checksum, F7). The channel must be one of UA101_VOLUMES_PASSED:
   the first check takes the parser's field, the second bytes 7 and 8. The value must be at
   most UA101_VOLUME_PASSED_MAX (about +12 dB), the first digit sent being the highest: the first check takes the parser's
   number, the second the digits one by one. The order of the digits is proven on the unit for
   INPUT 1 (2026-11-12); the other channels' volumes lie in the block in the same six digits
   (docs/control-protocol.md), and each is tried on the unit by the user before it is used
  . Returns 1 if it passes, 0 if it is no such message, -1 if it is one
   for a passed channel but the value is too high. */
static int volume_change_passes(const uint8_t *msg, size_t len, const ua101_msg *m)
{
    static const uint8_t head[7] = {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00};
    static const uint8_t first[3] = {0, 2, 12}, count[3] = {2, 10, 10};
    unsigned group, channel, number, sum = 0, i;
    const uint8_t *d;

    if (m->kind != UA101_MSG_FIELD || m->field.kind != UA101_F_VOLUME)
        return 0;
    /* The first channel check, by the parser's field: a channel the mixer has (MONITOR OUT L
       and R, INPUT 1 to 10, OUTPUT 1 to 10), and one of UA101_VOLUMES_PASSED. */
    if (!SKIPPED(64u)) {
        if (m->field.group > 2 || m->field.index >= (m->field.group == 0 ? 2u : 10u))
            return 0;
        number = (m->field.group == 0 ? 0u : m->field.group == 1 ? 2u : 12u) + m->field.index;
        if (!((UA101_VOLUMES_PASSED >> number) & 1u))
            return 0;
    }
    if (len != UA101_VOLUME_MESSAGE_BYTES || memcmp(msg, head, sizeof(head)) != 0 ||
        msg[9] != 0x07 || msg[17] != 0xF7 || msg[7] < 3 || msg[7] > 5)
        return 0;
    group = msg[7] - 3u;
    channel = msg[8];
    /* The second channel check, by bytes 7 and 8: the same two questions, from the tables. */
    if (!SKIPPED(128u) &&
        (channel >= count[group] || !((UA101_VOLUMES_PASSED >> (first[group] + channel)) & 1u)))
        return 0;
    /* And the two must name the same channel. */
    if (group != m->field.group || channel != m->field.index)
        return 0;
    d = msg + 10;
    for (i = 0; i < 6; i++) {
        if (d[i] > 0x0F)
            return 0;
        sum += d[i];
    }
    if (msg[16] != ((0x80u - ((msg[7] + msg[8] + 0x07u + sum) & 0x7Fu)) & 0x7Fu))
        return 0;
    if (!SKIPPED(256u) && m->value > UA101_VOLUME_PASSED_MAX)
        return -1;
    if (!SKIPPED(512u) && !volume_digits_ok(d))
        return -1;
    return 1;
}

#ifdef UA101_SERVICE_TESTING
/* The helper test only: the volume check by itself, with a parse the test makes by hand, so
   that each of the two channel checks and each of the two value checks can be shown to keep
   the rule alone. Never built into the helper. */
int ua101_service_test_volume(const uint8_t *msg, size_t len, const ua101_msg *m)
{
    return volume_change_passes(msg, len, m);
}
#endif

/* 1 if the four digits, the first one sent being the highest (proven on the unit for a pan,
   2026-11-12: 0x4000 sent as 04 00 00 00 read back as 04 00 00 00 in d[36..39],
  ), spell a pan within 0x0000 to 0x8000: the first digit is 0 to 7, or it
   is 8 and the other three are all 0. No number is formed here. */
static int pan_digits_ok(const uint8_t d[4])
{
    unsigned i;

    if (d[0] < 8)
        return 1;
    if (d[0] > 8)
        return 0;
    for (i = 1; i < 4; i++)
        if (d[i] != 0)
            return 0;
    return 1;
}

/* A pan change this version passes on, judged twice without shared code, as the volume is:
   by what the parser says the message is, and by the 16 bytes themselves as
   docs/control-protocol.md gives them (F0 41 10 00 69 12, address 00 <group+3> <channel> 03,
   four digits each 0 to F, the checksum, F7). The channel must be one of UA101_PANS_PASSED
   (INPUT 1 to 10 and OUTPUT 1 to 10; MONITOR OUT has no pan): the first check takes the
   parser's field, the second bytes 7 and 8. The value must lie within 0x0000 to 0x8000, the
   first digit sent being the highest: the first check takes the parser's number, the second
   the digits one by one. Returns 1 if it passes, 0 if it is no such message, -1 if it is one
   for a passed channel but the value is past the end of the range. */
static int pan_change_passes(const uint8_t *msg, size_t len, const ua101_msg *m)
{
    static const uint8_t head[7] = {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00};
    unsigned group, channel, sum = 0, i;
    const uint8_t *d;

    if (m->kind != UA101_MSG_FIELD || m->field.kind != UA101_F_PAN)
        return 0;
    /* The first channel check, by the parser's field: a channel that has a pan (INPUT 1 to
       10, OUTPUT 1 to 10; not MONITOR OUT), and one of UA101_PANS_PASSED. */
    if (!SKIPPED(4u) &&
        (m->field.group < 1 || m->field.group > 2 || m->field.index >= 10 ||
         !((UA101_PANS_PASSED >> ((m->field.group == 1 ? 2u : 12u) + m->field.index)) & 1u)))
        return 0;
    if (len != UA101_PAN_MESSAGE_BYTES || memcmp(msg, head, sizeof(head)) != 0 ||
        msg[9] != 0x03 || msg[15] != 0xF7 || msg[7] < 3 || msg[7] > 5)
        return 0;
    group = msg[7] - 3u;
    channel = msg[8];
    /* The second channel check, by bytes 7 and 8: the same two questions. */
    if (!SKIPPED(8u) &&
        (group < 1 || channel >= 10 || !((UA101_PANS_PASSED >> (10u * group - 8u + channel)) & 1u)))
        return 0;
    /* And the two must name the same channel. */
    if (group != m->field.group || channel != m->field.index)
        return 0;
    d = msg + 10;
    for (i = 0; i < 4; i++) {
        if (d[i] > 0x0F)
            return 0;
        sum += d[i];
    }
    if (msg[14] != ((0x80u - ((msg[7] + msg[8] + 0x03u + sum) & 0x7Fu)) & 0x7Fu))
        return 0;
    if (!SKIPPED(1u) && m->value > UA101_PAN_PASSED_MAX)
        return -1;
    if (!SKIPPED(2u) && !pan_digits_ok(d))
        return -1;
    return 1;
}

#ifdef UA101_SERVICE_TESTING
/* The helper test only: the pan check by itself, with a parse the test makes by hand. The
   parser stops any pan above 0x8000 before this check is reached (ua101_field_value_ok), so
   only this way can the test show that each of the two value checks keeps to the rule
   alone. Never built into the helper. */
int ua101_service_test_pan(const uint8_t *msg, size_t len, const ua101_msg *m)
{
    return pan_change_passes(msg, len, m);
}
#endif

/* The on/off of every mixer channel, as docs/control-protocol.md gives it ("Monitor on/off",
   address 00 <group+3> <channel> 02, one byte), with the only two values the settings block
   can hold for it (one bit): 00 and 01. A row for each channel, numbered as in
   UA101_CTL_PASS_ON_CHANNEL, the message for 00 and then the one for 01. Written out byte by
   byte, checksums included, because the helper holds no code that builds a message;
   tests/test_ctlhelper.c checks every row against the app's builder and against a message
   made by hand. On INPUT 1 the bit 1 mutes and the bit 0 lets the sound through (proven on
   the unit by ear, 2026-11-14); for the other channels that is the user's check at the box
  . Either value is harmless to the volume rule: neither raises a level
   above what the fader allows. */
#define ON_CHANNELS 22
static const uint8_t k_on_messages[ON_CHANNELS][2][UA101_ON_MESSAGE_BYTES] = {
    /* MONITOR OUT L */
    {{0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x03, 0x00, 0x02, 0x00, 0x7B, 0xF7},
     {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x03, 0x00, 0x02, 0x01, 0x7A, 0xF7}},
    /* MONITOR OUT R */
    {{0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x03, 0x01, 0x02, 0x00, 0x7A, 0xF7},
     {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x03, 0x01, 0x02, 0x01, 0x79, 0xF7}},
    /* INPUT 1 */
    {{0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x04, 0x00, 0x02, 0x00, 0x7A, 0xF7},
     {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x04, 0x00, 0x02, 0x01, 0x79, 0xF7}},
    /* INPUT 2 */
    {{0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x04, 0x01, 0x02, 0x00, 0x79, 0xF7},
     {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x04, 0x01, 0x02, 0x01, 0x78, 0xF7}},
    /* INPUT 3 */
    {{0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x04, 0x02, 0x02, 0x00, 0x78, 0xF7},
     {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x04, 0x02, 0x02, 0x01, 0x77, 0xF7}},
    /* INPUT 4 */
    {{0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x04, 0x03, 0x02, 0x00, 0x77, 0xF7},
     {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x04, 0x03, 0x02, 0x01, 0x76, 0xF7}},
    /* INPUT 5 */
    {{0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x04, 0x04, 0x02, 0x00, 0x76, 0xF7},
     {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x04, 0x04, 0x02, 0x01, 0x75, 0xF7}},
    /* INPUT 6 */
    {{0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x04, 0x05, 0x02, 0x00, 0x75, 0xF7},
     {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x04, 0x05, 0x02, 0x01, 0x74, 0xF7}},
    /* INPUT 7 */
    {{0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x04, 0x06, 0x02, 0x00, 0x74, 0xF7},
     {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x04, 0x06, 0x02, 0x01, 0x73, 0xF7}},
    /* INPUT 8 */
    {{0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x04, 0x07, 0x02, 0x00, 0x73, 0xF7},
     {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x04, 0x07, 0x02, 0x01, 0x72, 0xF7}},
    /* INPUT 9 */
    {{0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x04, 0x08, 0x02, 0x00, 0x72, 0xF7},
     {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x04, 0x08, 0x02, 0x01, 0x71, 0xF7}},
    /* INPUT 10 */
    {{0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x04, 0x09, 0x02, 0x00, 0x71, 0xF7},
     {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x04, 0x09, 0x02, 0x01, 0x70, 0xF7}},
    /* OUTPUT 1 */
    {{0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x05, 0x00, 0x02, 0x00, 0x79, 0xF7},
     {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x05, 0x00, 0x02, 0x01, 0x78, 0xF7}},
    /* OUTPUT 2 */
    {{0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x05, 0x01, 0x02, 0x00, 0x78, 0xF7},
     {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x05, 0x01, 0x02, 0x01, 0x77, 0xF7}},
    /* OUTPUT 3 */
    {{0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x05, 0x02, 0x02, 0x00, 0x77, 0xF7},
     {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x05, 0x02, 0x02, 0x01, 0x76, 0xF7}},
    /* OUTPUT 4 */
    {{0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x05, 0x03, 0x02, 0x00, 0x76, 0xF7},
     {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x05, 0x03, 0x02, 0x01, 0x75, 0xF7}},
    /* OUTPUT 5 */
    {{0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x05, 0x04, 0x02, 0x00, 0x75, 0xF7},
     {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x05, 0x04, 0x02, 0x01, 0x74, 0xF7}},
    /* OUTPUT 6 */
    {{0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x05, 0x05, 0x02, 0x00, 0x74, 0xF7},
     {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x05, 0x05, 0x02, 0x01, 0x73, 0xF7}},
    /* OUTPUT 7 */
    {{0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x05, 0x06, 0x02, 0x00, 0x73, 0xF7},
     {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x05, 0x06, 0x02, 0x01, 0x72, 0xF7}},
    /* OUTPUT 8 */
    {{0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x05, 0x07, 0x02, 0x00, 0x72, 0xF7},
     {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x05, 0x07, 0x02, 0x01, 0x71, 0xF7}},
    /* OUTPUT 9 */
    {{0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x05, 0x08, 0x02, 0x00, 0x71, 0xF7},
     {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x05, 0x08, 0x02, 0x01, 0x70, 0xF7}},
    /* OUTPUT 10 */
    {{0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x05, 0x09, 0x02, 0x00, 0x70, 0xF7},
     {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x05, 0x09, 0x02, 0x01, 0x6F, 0xF7}},
};

/* An on/off change this version passes on, judged twice without shared code: by what the
   parser says the message is (an on/off, of a channel in UA101_ONS_PASSED, the value 0 or 1),
   and by the 13 bytes themselves, whole, against the rows above of the channels in
   UA101_ONS_PASSED. Each check alone names both the channel and the value. Returns 1 if it
   passes, 0 if not. */
static int on_change_passes(const uint8_t *msg, size_t len, const ua101_msg *m)
{
    static const uint8_t first[3] = {0, 2, 12}, count[3] = {2, 10, 10};
    unsigned n, v;
    int same = 0;

    if (m->kind != UA101_MSG_FIELD || m->field.kind != UA101_F_ON)
        return 0;
    /* The first check, by the parser's field and value: a channel the mixer has, one of
       UA101_ONS_PASSED, and the value 0 or 1. */
    if (!SKIPPED(16u) &&
        (m->field.group > 2 || m->field.index >= count[m->field.group] ||
         !((UA101_ONS_PASSED >> (first[m->field.group] + m->field.index)) & 1u) || m->value > 1))
        return 0;
    if (len != UA101_ON_MESSAGE_BYTES)
        return 0;
    /* The second check, by the 13 bytes, whole: one of the rows of a passed channel. */
    for (n = 0; n < ON_CHANNELS; n++)
        if ((UA101_ONS_PASSED >> n) & 1u)
            for (v = 0; v < 2; v++)
                if (memcmp(msg, k_on_messages[n][v], UA101_ON_MESSAGE_BYTES) == 0)
                    same = 1;
    if (!SKIPPED(32u) && !same)
        return 0;
    return 1;
}

#ifdef UA101_SERVICE_TESTING
/* The helper test only: the on/off check by itself, with a parse the test makes by hand, so
   that each of the two checks can be shown to keep the rule alone. Never built into the
   helper. */
int ua101_service_test_on(const uint8_t *msg, size_t len, const ua101_msg *m)
{
    return on_change_passes(msg, len, m);
}
#endif

/* The stereo link of every pair of the mixer, as docs/control-protocol.md gives it ("Stereo
   link", address 00 <group+3> <the pair's first channel> 01, one byte), with the only two
   values the settings block can hold for it (one bit): 00 and 01. A row for each pair,
   numbered as in UA101_CTL_PASS_LINK_PAIR, the message for 00 and then the one for 01. Written
   out byte by byte, checksums included, because the helper holds no code that builds a
   message; tests/test_ctlhelper.c checks every row against the app's builder and against a
   message made by hand. Roland's own program sends exactly this one message for a click on
   LINK and nothing with it: the link changes no level by
   itself as far as that program shows; what the unit does with it is the user's check at the
   box. Either value is harmless to the volume rule: neither raises a level above what a fader
   allows. */
#define LINK_PAIRS 11
static const uint8_t k_link_messages[LINK_PAIRS][2][UA101_LINK_MESSAGE_BYTES] = {
    /* MONITOR OUT L-R */
    {{0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x03, 0x00, 0x01, 0x00, 0x7C, 0xF7},
     {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x03, 0x00, 0x01, 0x01, 0x7B, 0xF7}},
    /* INPUT 1-2 */
    {{0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x04, 0x00, 0x01, 0x00, 0x7B, 0xF7},
     {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x04, 0x00, 0x01, 0x01, 0x7A, 0xF7}},
    /* INPUT 3-4 */
    {{0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x04, 0x02, 0x01, 0x00, 0x79, 0xF7},
     {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x04, 0x02, 0x01, 0x01, 0x78, 0xF7}},
    /* INPUT 5-6 */
    {{0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x04, 0x04, 0x01, 0x00, 0x77, 0xF7},
     {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x04, 0x04, 0x01, 0x01, 0x76, 0xF7}},
    /* INPUT 7-8 */
    {{0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x04, 0x06, 0x01, 0x00, 0x75, 0xF7},
     {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x04, 0x06, 0x01, 0x01, 0x74, 0xF7}},
    /* INPUT 9-10 */
    {{0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x04, 0x08, 0x01, 0x00, 0x73, 0xF7},
     {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x04, 0x08, 0x01, 0x01, 0x72, 0xF7}},
    /* OUTPUT 1-2 */
    {{0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x05, 0x00, 0x01, 0x00, 0x7A, 0xF7},
     {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x05, 0x00, 0x01, 0x01, 0x79, 0xF7}},
    /* OUTPUT 3-4 */
    {{0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x05, 0x02, 0x01, 0x00, 0x78, 0xF7},
     {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x05, 0x02, 0x01, 0x01, 0x77, 0xF7}},
    /* OUTPUT 5-6 */
    {{0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x05, 0x04, 0x01, 0x00, 0x76, 0xF7},
     {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x05, 0x04, 0x01, 0x01, 0x75, 0xF7}},
    /* OUTPUT 7-8 */
    {{0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x05, 0x06, 0x01, 0x00, 0x74, 0xF7},
     {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x05, 0x06, 0x01, 0x01, 0x73, 0xF7}},
    /* OUTPUT 9-10 */
    {{0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x05, 0x08, 0x01, 0x00, 0x72, 0xF7},
     {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x05, 0x08, 0x01, 0x01, 0x71, 0xF7}},
};

/* A link change this version passes on, judged twice without shared code, as the on/off is:
   by what the parser says the message is (a link, of a pair in UA101_LINKS_PASSED, the value 0
   or 1), and by the 13 bytes themselves, whole, against the rows above of the pairs in
   UA101_LINKS_PASSED (LINKS_PASSED here: the same, but for the helper test). Each check alone
   names both the pair and the value. Returns 1 if it passes, 0 if not. */
static int link_change_passes(const uint8_t *msg, size_t len, const ua101_msg *m)
{
    static const uint8_t first[3] = {0, 1, 6}, pairs[3] = {1, 5, 5};
    unsigned n, v;
    int same = 0;

    if (m->kind != UA101_MSG_FIELD || m->field.kind != UA101_F_LINK)
        return 0;
    /* The first check, by the parser's field and value: a pair the mixer has, one of
       UA101_LINKS_PASSED, and the value 0 or 1. */
    if (!SKIPPED(1024u) &&
        (m->field.group > 2 || m->field.index >= pairs[m->field.group] ||
         !((LINKS_PASSED >> (first[m->field.group] + m->field.index)) & 1u) || m->value > 1))
        return 0;
    if (len != UA101_LINK_MESSAGE_BYTES)
        return 0;
    /* The second check, by the 13 bytes, whole: one of the rows of a passed pair. */
    for (n = 0; n < LINK_PAIRS; n++)
        if ((LINKS_PASSED >> n) & 1u)
            for (v = 0; v < 2; v++)
                if (memcmp(msg, k_link_messages[n][v], UA101_LINK_MESSAGE_BYTES) == 0)
                    same = 1;
    if (!SKIPPED(2048u) && !same)
        return 0;
    return 1;
}

#ifdef UA101_SERVICE_TESTING
/* The helper test only: the link check by itself, with a parse the test makes by hand, so
   that each of the two checks can be shown to keep the rule alone. Never built into the
   helper. */
int ua101_service_test_link(const uint8_t *msg, size_t len, const ua101_msg *m)
{
    return link_change_passes(msg, len, m);
}
#endif

/* The solo of the INPUT group and of the OUTPUT group, as docs/control-protocol.md gives it
   ("Solo", address 00 <group+3> 00 00, one byte). What the byte means was read from Roland's
   own program on 2026-11-16: one number a group, 00 for "no channel is
   soloed" and 01 to 0A for the one soloed channel, INPUT or OUTPUT 1 to 10; a click on SOLO
   sends the channel's number, a click on a lit SOLO sends 00. The settings block holds the
   same number (d[22] for INPUT, d[23] for OUTPUT; both read 00 on every read of this unit).
   MONITOR OUT has no solo. A row for each group, numbered as in UA101_SOLOS_PASSED, the eleven
   messages for 00 to 0A. Written out byte by byte, checksums included; tests/test_ctlhelper.c
   checks every row against the app's builder and against a message made by hand. A solo can
   silence channels; it raises no level above what a fader allows. What the unit does with it
   has never been heard: that is the user's check at the box. */
#define SOLO_GROUPS 2
#define SOLO_VALUES (UA101_SOLO_PASSED_MAX + 1u)
static const uint8_t k_solo_messages[SOLO_GROUPS][SOLO_VALUES][UA101_SOLO_MESSAGE_BYTES] = {
    /* the INPUT group: no solo, then channel 1 to 10 */
    {{0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x04, 0x00, 0x00, 0x00, 0x7C, 0xF7},
     {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x04, 0x00, 0x00, 0x01, 0x7B, 0xF7},
     {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x04, 0x00, 0x00, 0x02, 0x7A, 0xF7},
     {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x04, 0x00, 0x00, 0x03, 0x79, 0xF7},
     {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x04, 0x00, 0x00, 0x04, 0x78, 0xF7},
     {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x04, 0x00, 0x00, 0x05, 0x77, 0xF7},
     {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x04, 0x00, 0x00, 0x06, 0x76, 0xF7},
     {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x04, 0x00, 0x00, 0x07, 0x75, 0xF7},
     {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x04, 0x00, 0x00, 0x08, 0x74, 0xF7},
     {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x04, 0x00, 0x00, 0x09, 0x73, 0xF7},
     {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x04, 0x00, 0x00, 0x0A, 0x72, 0xF7}},
    /* the OUTPUT group: no solo, then channel 1 to 10 */
    {{0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x05, 0x00, 0x00, 0x00, 0x7B, 0xF7},
     {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x05, 0x00, 0x00, 0x01, 0x7A, 0xF7},
     {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x05, 0x00, 0x00, 0x02, 0x79, 0xF7},
     {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x05, 0x00, 0x00, 0x03, 0x78, 0xF7},
     {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x05, 0x00, 0x00, 0x04, 0x77, 0xF7},
     {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x05, 0x00, 0x00, 0x05, 0x76, 0xF7},
     {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x05, 0x00, 0x00, 0x06, 0x75, 0xF7},
     {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x05, 0x00, 0x00, 0x07, 0x74, 0xF7},
     {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x05, 0x00, 0x00, 0x08, 0x73, 0xF7},
     {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x05, 0x00, 0x00, 0x09, 0x72, 0xF7},
     {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x05, 0x00, 0x00, 0x0A, 0x71, 0xF7}},
};

/* A solo change this version passes on, judged twice without shared code, as the link is: by
   what the parser says the message is (a solo, of a group in UA101_SOLOS_PASSED, the value 0
   to UA101_SOLO_PASSED_MAX), and by the 13 bytes themselves, whole, against the rows above of
   the groups in UA101_SOLOS_PASSED. Each check alone names both the group and the value.
   Returns 1 if it passes, 0 if not. */
static int solo_change_passes(const uint8_t *msg, size_t len, const ua101_msg *m)
{
    unsigned g, v;
    int same = 0;

    if (m->kind != UA101_MSG_FIELD || m->field.kind != UA101_F_SOLO)
        return 0;
    /* The first check, by the parser's field and value: the INPUT or the OUTPUT group, one of
       UA101_SOLOS_PASSED, and a value that is "none" or a channel 1 to 10. */
    if (!SKIPPED(4096u) &&
        (m->field.group < 1 || m->field.group > 2 || m->field.index != 0 ||
         !((SOLOS_PASSED >> (m->field.group - 1u)) & 1u) || m->value > UA101_SOLO_PASSED_MAX))
        return 0;
    if (len != UA101_SOLO_MESSAGE_BYTES)
        return 0;
    /* The second check, by the 13 bytes, whole: one of the rows of a passed group. */
    for (g = 0; g < SOLO_GROUPS; g++)
        if ((SOLOS_PASSED >> g) & 1u)
            for (v = 0; v < SOLO_VALUES; v++)
                if (memcmp(msg, k_solo_messages[g][v], UA101_SOLO_MESSAGE_BYTES) == 0)
                    same = 1;
    if (!SKIPPED(8192u) && !same)
        return 0;
    return 1;
}

#ifdef UA101_SERVICE_TESTING
/* The helper test only: the solo check by itself, with a parse the test makes by hand. The
   parser stops any solo above 10 before this check is reached (ua101_field_value_ok), so only
   this way can the test show that each of the two checks keeps to the rule alone. Never built
   into the helper. */
int ua101_service_test_solo(const uint8_t *msg, size_t len, const ua101_msg *m)
{
    return solo_change_passes(msg, len, m);
}
#endif

/* --- Load, Store and Reset (link version 17). --- */

#define MEMORY_WORDS_(n) #n " seconds"
#define MEMORY_WORDS(n) MEMORY_WORDS_(n)
#define MEMORY_GAP_WORDS MEMORY_WORDS(UA101_MEMORY_GAP_SECONDS)
#define MEMORY_FRESH_WORDS MEMORY_WORDS(UA101_MEMORY_FRESH_SECONDS)

/* The helper's clock for the rules below: time since the Mac started, in microseconds. */
static uint64_t memory_now_us(void)
{
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000u + CLOCK_SKEW_US;
}

/* The 37 messages this version passes on, as docs/control-protocol.md gives them ("Commands
   that change or erase stored settings"), each written out byte by byte, checksums included,
   because the helper holds no code that builds a message; tests/test_ctlhelper.c checks every
   row against the app's builder and against a message made by hand.
   Load: address 00 00 <memory + 2> 08 (Memory 1 is 03), four bytes: the first two always 00,
   the third 01 for the patch bay, the fourth 01 for the mixer (read from Roland's own Load
   dialog, 2026-11-20: its two ticks are "OUTPUT Patch Bay" and "MONITOR"). Store: the same at
   00 00 <memory + 2> 04, with 02 where a Load has 01. Reset: one byte 00 at 00 00 <n> 03,
   where n is 00 for the settings in use (Roland's "Current Setting": its "reset one" with the
   number 0; in Roland's driver the number goes into the address unchanged, read 2026-11-21) and
   03 to 07 for Memory 1 to 5; and at 00 00 00 02 for everything. Their neighbours differ in
   one byte ("resume saving" is 00 00 00 00 with 01; Roland's patch bay preset is 00 00 00 08),
   so every match below is of the whole message. NONE HAS BEEN SENT TO THE UNIT YET. */
#define MEMORY_PARTS 3
#define RESET_ROWS 7
static const uint8_t k_load_messages[UA101_MEMORIES][MEMORY_PARTS][UA101_MEMORY_MESSAGE_BYTES] = {
    /* Memory 1: the patch bay, the mixer, both */
    {{0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x00, 0x03, 0x08, 0x00, 0x00, 0x01, 0x00, 0x74, 0xF7},
     {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x00, 0x03, 0x08, 0x00, 0x00, 0x00, 0x01, 0x74, 0xF7},
     {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x00, 0x03, 0x08, 0x00, 0x00, 0x01, 0x01, 0x73, 0xF7}},
    /* Memory 2: the patch bay, the mixer, both */
    {{0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x00, 0x04, 0x08, 0x00, 0x00, 0x01, 0x00, 0x73, 0xF7},
     {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x00, 0x04, 0x08, 0x00, 0x00, 0x00, 0x01, 0x73, 0xF7},
     {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x00, 0x04, 0x08, 0x00, 0x00, 0x01, 0x01, 0x72, 0xF7}},
    /* Memory 3: the patch bay, the mixer, both */
    {{0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x00, 0x05, 0x08, 0x00, 0x00, 0x01, 0x00, 0x72, 0xF7},
     {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x00, 0x05, 0x08, 0x00, 0x00, 0x00, 0x01, 0x72, 0xF7},
     {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x00, 0x05, 0x08, 0x00, 0x00, 0x01, 0x01, 0x71, 0xF7}},
    /* Memory 4: the patch bay, the mixer, both */
    {{0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x00, 0x06, 0x08, 0x00, 0x00, 0x01, 0x00, 0x71, 0xF7},
     {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x00, 0x06, 0x08, 0x00, 0x00, 0x00, 0x01, 0x71, 0xF7},
     {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x00, 0x06, 0x08, 0x00, 0x00, 0x01, 0x01, 0x70, 0xF7}},
    /* Memory 5: the patch bay, the mixer, both */
    {{0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x00, 0x07, 0x08, 0x00, 0x00, 0x01, 0x00, 0x70, 0xF7},
     {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x00, 0x07, 0x08, 0x00, 0x00, 0x00, 0x01, 0x70, 0xF7},
     {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x00, 0x07, 0x08, 0x00, 0x00, 0x01, 0x01, 0x6F, 0xF7}},
};
static const uint8_t k_store_messages[UA101_MEMORIES][MEMORY_PARTS][UA101_MEMORY_MESSAGE_BYTES] = {
    /* Memory 1: the patch bay, the mixer, both */
    {{0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x00, 0x03, 0x04, 0x00, 0x00, 0x02, 0x00, 0x77, 0xF7},
     {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x00, 0x03, 0x04, 0x00, 0x00, 0x00, 0x02, 0x77, 0xF7},
     {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x00, 0x03, 0x04, 0x00, 0x00, 0x02, 0x02, 0x75, 0xF7}},
    /* Memory 2: the patch bay, the mixer, both */
    {{0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x00, 0x04, 0x04, 0x00, 0x00, 0x02, 0x00, 0x76, 0xF7},
     {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x00, 0x04, 0x04, 0x00, 0x00, 0x00, 0x02, 0x76, 0xF7},
     {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x00, 0x04, 0x04, 0x00, 0x00, 0x02, 0x02, 0x74, 0xF7}},
    /* Memory 3: the patch bay, the mixer, both */
    {{0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x00, 0x05, 0x04, 0x00, 0x00, 0x02, 0x00, 0x75, 0xF7},
     {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x00, 0x05, 0x04, 0x00, 0x00, 0x00, 0x02, 0x75, 0xF7},
     {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x00, 0x05, 0x04, 0x00, 0x00, 0x02, 0x02, 0x73, 0xF7}},
    /* Memory 4: the patch bay, the mixer, both */
    {{0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x00, 0x06, 0x04, 0x00, 0x00, 0x02, 0x00, 0x74, 0xF7},
     {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x00, 0x06, 0x04, 0x00, 0x00, 0x00, 0x02, 0x74, 0xF7},
     {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x00, 0x06, 0x04, 0x00, 0x00, 0x02, 0x02, 0x72, 0xF7}},
    /* Memory 5: the patch bay, the mixer, both */
    {{0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x00, 0x07, 0x04, 0x00, 0x00, 0x02, 0x00, 0x73, 0xF7},
     {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x00, 0x07, 0x04, 0x00, 0x00, 0x00, 0x02, 0x73, 0xF7},
     {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x00, 0x07, 0x04, 0x00, 0x00, 0x02, 0x02, 0x71, 0xF7}},
};
static const uint8_t k_reset_messages[RESET_ROWS][UA101_RESET_MESSAGE_BYTES] = {
    /* the settings in use */
    {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x00, 0x00, 0x03, 0x00, 0x7D, 0xF7},
    /* Memory 1 */
    {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x00, 0x03, 0x03, 0x00, 0x7A, 0xF7},
    /* Memory 2 */
    {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x00, 0x04, 0x03, 0x00, 0x79, 0xF7},
    /* Memory 3 */
    {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x00, 0x05, 0x03, 0x00, 0x78, 0xF7},
    /* Memory 4 */
    {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x00, 0x06, 0x03, 0x00, 0x77, 0xF7},
    /* Memory 5 */
    {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x00, 0x07, 0x03, 0x00, 0x76, 0xF7},
    /* everything: the settings in use and all five memories */
    {0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x00, 0x00, 0x02, 0x00, 0x7E, 0xF7},
};

/* A Load, Store or Reset this version passes on, judged twice without shared code, as a link
   or a solo is: by what the parser says the message is (the kind, a memory 1 to 5, the first
   two section bytes 0 and the patch bay, the mixer or both chosen), and by the bytes
   themselves, whole, against the rows above; each check alone names the command, and both
   ask the list (UA101_MEMORY_PASSED; MEMORY_PASSED here: the same, but for the helper test).
   Returns the command's bit (UA101_CTL_PASS_MEM_...), or 0 if it doesn't pass. */
static uint64_t memory_command_passes(const uint8_t *msg, size_t len, const ua101_msg *m)
{
    uint64_t by_parse = 0, by_bytes = 0;
    unsigned memory, part, row;

    /* The first check, by the parser's kind, memory and sections. */
    switch (m->kind) {
    case UA101_MSG_LOAD:
    case UA101_MSG_STORE:
        if (m->memory < UA101_MEMORIES && m->sections[0] == 0 && m->sections[1] == 0 &&
            m->sections[2] <= 1 && m->sections[3] <= 1 && (m->sections[2] | m->sections[3]) != 0)
            by_parse = m->kind == UA101_MSG_LOAD ? UA101_CTL_PASS_MEM_LOAD(m->memory)
                                                 : UA101_CTL_PASS_MEM_STORE(m->memory);
        break;
    case UA101_MSG_RESET_MEMORY:
        if (m->memory < UA101_MEMORIES)
            by_parse = UA101_CTL_PASS_MEM_RESET(m->memory);
        break;
    case UA101_MSG_RESET_CURRENT:
        by_parse = UA101_CTL_PASS_MEM_RESET_CURRENT;
        break;
    case UA101_MSG_RESET_ALL:
        by_parse = UA101_CTL_PASS_MEM_RESET_ALL;
        break;
    default:
        return 0;
    }
    by_parse &= MEMORY_PASSED;
    /* The second check, by the bytes, whole: one of the rows of a command on the list. */
    if (len == UA101_MEMORY_MESSAGE_BYTES) {
        for (memory = 0; memory < UA101_MEMORIES; memory++)
            for (part = 0; part < MEMORY_PARTS; part++) {
                if ((MEMORY_PASSED & UA101_CTL_PASS_MEM_LOAD(memory)) != 0 &&
                    memcmp(msg, k_load_messages[memory][part], len) == 0)
                    by_bytes = UA101_CTL_PASS_MEM_LOAD(memory);
                if ((MEMORY_PASSED & UA101_CTL_PASS_MEM_STORE(memory)) != 0 &&
                    memcmp(msg, k_store_messages[memory][part], len) == 0)
                    by_bytes = UA101_CTL_PASS_MEM_STORE(memory);
            }
    } else if (len == UA101_RESET_MESSAGE_BYTES) {
        for (row = 0; row < RESET_ROWS; row++) {
            uint64_t bit = row == 0 ? UA101_CTL_PASS_MEM_RESET_CURRENT
                         : row == RESET_ROWS - 1 ? UA101_CTL_PASS_MEM_RESET_ALL
                                                 : UA101_CTL_PASS_MEM_RESET(row - 1u);

            if ((MEMORY_PASSED & bit) != 0 && memcmp(msg, k_reset_messages[row], len) == 0)
                by_bytes = bit;
        }
    }
    /* In the helper test only, one check can be left out: the other's answer is then the
       answer. In the helper both must name a command, and the same one. */
    if (SKIPPED(32768u))
        return by_parse;
    if (SKIPPED(16384u))
        return by_bytes;
    return by_parse == by_bytes ? by_bytes : 0;
}

#ifdef UA101_SERVICE_TESTING
/* The helper test only: the memory check by itself, with a parse the test makes by hand, so
   that each of the two checks can be shown to keep the rule alone. Never built into the
   helper. */
uint64_t ua101_service_test_memory_command(const uint8_t *msg, size_t len, const ua101_msg *m)
{
    return memory_command_passes(msg, len, m);
}
#endif

int ua101_service_memory_passes(const uint8_t *msg, size_t len, const ua101_service_moment *at,
                                const char **why)
{
    static const char *unused;
    const uint64_t fresh_us = (uint64_t)UA101_MEMORY_FRESH_SECONDS * 1000000u;
    const uint64_t gap_us = (uint64_t)UA101_MEMORY_GAP_SECONDS * 1000000u;
    ua101_msg m;

    if (why == NULL)
        why = &unused;
    *why = "";
    if (msg == NULL || at == NULL || ua101_ctlmsg_risk(msg, len) != UA101_RISK_MEMORY) {
        *why = "not a Load, Store or Reset; it was not sent";
        return 0;
    }
    (void)ua101_ctlmsg_parse(msg, len, &m);
    if (m.kind == UA101_MSG_PRESET) {
        *why = "Roland's one-message patch bay preset is never passed on (a preset is made of "
               "five ordinary patch bay changes); the message was not sent";
        return 0;
    }
    if (memory_command_passes(msg, len, &m) == 0) {
        *why = "of Load, Store and Reset this helper passes on 37 exact messages only: Load "
               "and Store of Memory 1 to 5 with the patch bay, the mixer or both, and Reset of "
               "the settings in use, of Memory 1 to 5, or of everything; the message was not "
               "sent";
        return 0;
    }
    /* Never while sound plays: a Load or a Reset can change what is heard at once, and nothing
       shows how the unit behaves while it streams. */
    if (at->streaming) {
        *why = "sound is being played or recorded through the unit, and a Load, Store or "
               "Reset is never passed on meanwhile; the message was not sent";
        return 0;
    }
    /* One at a time. A clock that went backwards counts as too soon. */
    if (at->memory_us != 0 && (at->now_us < at->memory_us || at->now_us - at->memory_us < gap_us)) {
        *why = "another Load, Store or Reset was passed on less than " MEMORY_GAP_WORDS
               " ago; the message was not sent";
        return 0;
    }
    /* Only after a fresh read: the unit sent its whole settings block a short while before,
       and nothing was changed since. */
    /* (A read dated after now counts as too old as well: the subtraction then wraps round.) */
    if (at->block_us == 0 || at->now_us - at->block_us > fresh_us) {
        *why = "the unit's settings weren't read in the " MEMORY_FRESH_WORDS " before it (or a "
               "switch on the unit moved since), so there would be no record of them; the "
               "message was not sent";
        return 0;
    }
    if (at->block_us <= at->change_us || at->block_us <= at->memory_us) {
        *why = "the unit's settings weren't read again after the last change, so there would "
               "be no record of them as they are now; the message was not sent";
        return 0;
    }
    return 1;
}

/* The five memory reads this version passes on (link version 20, for the app's memory
   preview): Memory 1 to 5 at 02 03 00 00 to 02 07 00 00, 256 bytes each, written out here
   from docs/control-protocol.md because the helper holds no code that builds a message;
   tests/test_ctlhelper.c checks them against the app's builder. Reading changes nothing on the
   unit (box test, 2026-11-27). The other blocks are never asked for: 02 0D to 02 0F hung the
   unit's control port until it was switched off and on. */
static const uint8_t k_memory_reads[UA101_MEMORIES][UA101_CTL_READ_REQUEST_BYTES] = {
    {0xF0, 0x41, 0x10, 0x00, 0x69, 0x11, 0x02, 0x03, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x79, 0xF7},
    {0xF0, 0x41, 0x10, 0x00, 0x69, 0x11, 0x02, 0x04, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x78, 0xF7},
    {0xF0, 0x41, 0x10, 0x00, 0x69, 0x11, 0x02, 0x05, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x77, 0xF7},
    {0xF0, 0x41, 0x10, 0x00, 0x69, 0x11, 0x02, 0x06, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x76, 0xF7},
    {0xF0, 0x41, 0x10, 0x00, 0x69, 0x11, 0x02, 0x07, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x75, 0xF7}};

/* The memory, 0 to 4, if this is one of the five reads by both checks and the memory is in
   UA101_MEMORY_READS_PASSED; -1 for anything else. */
static int memory_read_passes(const uint8_t *msg, size_t len)
{
    ua101_msg m;
    unsigned i;

    if (msg == NULL || len != UA101_CTL_READ_REQUEST_BYTES ||
        ua101_ctlmsg_parse(msg, len, &m) != UA101_MSG_READ_MEMORY || m.memory >= UA101_MEMORIES)
        return -1;
    for (i = 0; i < UA101_MEMORIES; i++)
        if (memcmp(msg, k_memory_reads[i], len) == 0)
            break;
    if (i != m.memory || (UA101_MEMORY_READS_PASSED & (1u << i)) == 0)
        return -1;
    return (int)i;
}

int ua101_service_passes(const uint8_t *msg, size_t len, const char **why)
{
    static const char *unused;
    ua101_msg m;
    ua101_risk risk = msg != NULL ? ua101_ctlmsg_risk(msg, len) : UA101_RISK_NEVER;
    int volume, pan, on, link, solo;

    if (why == NULL)
        why = &unused;
    *why = "";
    switch (risk) {
    case UA101_RISK_READ:
        /* The settings request, or one of the five memory reads (link version 20), judged
           twice: the parser's verdict, and the bytes themselves against the list below. */
        if (ua101_ctl_is_read_request(msg, len) || memory_read_passes(msg, len) >= 0)
            return 1;
        *why = "of the requests this helper passes on only the settings and the five memories "
               "(02 00 and 02 03 to 02 07); the message was not sent";
        return 0;
    case UA101_RISK_SETTING:
        /* Two checks that don't share code: the parser's verdict and the bytes themselves. */
        volume = 0;
        if (!((ua101_ctlmsg_parse(msg, len, &m) == UA101_MSG_AUTOSAVE &&
               len == sizeof(k_pause_saving) &&
               (memcmp(msg, k_pause_saving, len) == 0 || memcmp(msg, k_resume_saving, len) == 0)) ||
              patch_change_passes(msg, len, &m))) {
            volume = volume_change_passes(msg, len, &m);
            if (volume < 0) {
                *why = "a volume above the top of the unit's range (0x7FFFFF, about +12 dB) is never "
                       "passed on; the message was not sent";
                return 0;
            }
            pan = volume == 0 ? pan_change_passes(msg, len, &m) : 0;
            if (pan < 0) {
                *why = "a pan past the end of its range (above 0x8000) is never passed on; "
                       "the message was not sent";
                return 0;
            }
            on = volume == 0 && pan == 0 ? on_change_passes(msg, len, &m) : 0;
            link = volume == 0 && pan == 0 && on == 0 ? link_change_passes(msg, len, &m) : 0;
            solo = volume == 0 && pan == 0 && on == 0 && link == 0
                       ? solo_change_passes(msg, len, &m) : 0;
            if (volume == 0 && pan == 0 && on == 0 && link == 0 && solo == 0) {
                *why = "this helper passes on seven changes only: pausing and resuming the "
                       "unit's saving, the patch bay source of an output pair, the volume "
                       "(up to +12 dB), the pan and the on/off (0 or 1) of a mixer channel, the "
                       "stereo link (0 or 1) of a pair, and the solo of the INPUT or the OUTPUT "
                       "group (0 for none, or a channel 1 to 10); the message was not sent";
                return 0;
            }
        }
        return 1;
    case UA101_RISK_MEMORY:
        /* Load, Store and Reset go by rules of their own, which need to know the moment:
           ua101_service_memory_passes. By this function, never. */
        *why = "Load, Store and Reset are passed on only by their own rules; the message was "
               "not sent";
        return 0;
    default:
        /* A request for another block (02 0D to 02 0F hung the unit's control port). */
        *why = msg != NULL && ua101_ctl_is_request_only(msg, len)
                   ? "of the requests this helper passes on only the settings and the five "
                     "memories (02 00 and 02 03 to 02 07); the message was not sent"
                   : "not a message this helper knows; it was not sent";
        return 0;
    }
}

/*
 * The one way a control app's bytes can reach the unit, and the only caller of
 * ua101_midiport_send for the control cable (the other caller, midi_pump, sends what the gate
 * gives out for the MIDI OUT socket, which is cable 0 only). Whatever the app above it believes it may do, it lets through the
 * request for the settings block, "pause saving",
 * "resume saving", the patch bay source of an output pair, and of the mixer's channels the
 * volume up to +12 dB, the pan and the on/off, of its pairs the link, and of the INPUT and the
 * OUTPUT group the solo (ua101_service_passes); and a Load, a Store or a Reset by the rules
 * of ua101_service_memory_passes. No go-ahead file is looked for (link version 18). Returns a
 * UA101_LINK_ status.
 */
static int64_t send_to_unit(const uint8_t *msg, size_t len, char *why, size_t why_bytes)
{
    uint8_t packets[UA101_CTL_READ_PACKET_BYTES];
    time_t now = time(NULL);
    const char *reason = "";
    ua101_risk risk = msg != NULL ? ua101_ctlmsg_risk(msg, len) : UA101_RISK_NEVER;
    int change = msg != NULL && risk != UA101_RISK_READ;
    int memory = risk == UA101_RISK_MEMORY;
    ua101_service_moment at;
    size_t packed;
    int passes;

    if (memory) {
        /* Load, Store and Reset: their own rules, with everything they are judged by taken
           at this moment. */
        memset(&at, 0, sizeof(at));
        at.streaming = S.streaming;
        at.now_us = memory_now_us();
        at.block_us = S.block_us;
        at.change_us = S.change_us;
        at.memory_us = S.memory_us;
        passes = ua101_service_memory_passes(msg, len, &at, &reason);
    } else {
        passes = ua101_service_passes(msg, len, &reason);
    }
    if (!passes) {
        if (S.counts.ctl_refused++ < 20 || memory)
            say("refused a control message of %zu bytes: %s", len, reason);
        snprintf(why, why_bytes, "%s", reason);
        return UA101_LINK_ERR_REFUSED;
    }
    if (now != S.ctl_second) {
        S.ctl_second = now;
        S.ctl_sends_now = 0;
    }
    if (++S.ctl_sends_now > CTL_SENDS_PER_SECOND) {
        S.counts.ctl_refused++;
        snprintf(why, why_bytes, "too many requests in one second; not sent");
        return UA101_LINK_ERR_REFUSED;
    }
    if (memory) {
        /* The record first: the settings as the unit last sent them, into the log, before the
           command leaves. This is the backup the app can't forget. */
        char hex[3 * UA101_CTL_BLOCK_BYTES + 1];
        size_t i;

        for (i = 0; i < UA101_CTL_BLOCK_BYTES; i++)
            snprintf(hex + 3 * i, 4, "%02X ", S.block[i]);
        hex[3 * i - 1] = '\0';
        say("a Load, Store or Reset follows. The unit's settings as it sent them %.1f s ago "
            "(256 bytes): %s",
            (double)(at.now_us - at.block_us) / 1e6, hex);
    }
    packed = ua101_ctl_pack_sysex(msg, len, UA101_CTL_CABLE, packets, sizeof(packets));
    /* Whether or not the unit took it, what was read before no longer counts as fresh. */
    if (memory)
        S.memory_us = memory_now_us();
    else if (change)
        S.change_us = memory_now_us();
    if (packed == 0 || !ua101_midiport_send(S.port, packets, packed)) {
        if (memory)
            say("the unit didn't take the Load, Store or Reset");
        snprintf(why, why_bytes, "the unit didn't take the %s", change ? "message" : "request");
        return UA101_LINK_ERR_ABSENT;
    }
    if (memory) {
        /* What passed is one of the 37 (ua101_service_memory_passes): named from the parse. */
        static const char *const part[4] = {"nothing", "the mixer", "the patch bay",
                                            "the patch bay and the mixer"};
        char what[96], hex[3 * UA101_MEMORY_MESSAGE_BYTES + 1];
        ua101_msg m;
        size_t i;

        (void)ua101_ctlmsg_parse(msg, len, &m);
        S.counts.ctl_changes++;
        S.counts.ctl_memory++;
        if (m.kind == UA101_MSG_LOAD || m.kind == UA101_MSG_STORE)
            snprintf(what, sizeof(what), "%s Memory %u, %s",
                     m.kind == UA101_MSG_LOAD ? "Load" : "Store into", m.memory + 1u,
                     part[(m.sections[2] ? 2 : 0) + (m.sections[3] ? 1 : 0)]);
        else if (m.kind == UA101_MSG_RESET_MEMORY)
            snprintf(what, sizeof(what), "Reset of Memory %u", m.memory + 1u);
        else if (m.kind == UA101_MSG_RESET_CURRENT)
            snprintf(what, sizeof(what), "Reset of the settings in use");
        else
            snprintf(what, sizeof(what),
                     "Reset of everything (the settings in use and all five memories)");
        for (i = 0; i < len && i < UA101_MEMORY_MESSAGE_BYTES; i++)
            snprintf(hex + 3 * i, 4, "%02X ", msg[i]);
        hex[3 * i - 1] = '\0';
        say("passed on to the unit: %s (%s)", what, hex);
    } else if (change) {
        /* Every change is written down: they are rare, and the log is the user's record. */
        static const char *const group_name[3] = {"MONITOR OUT", "INPUT", "OUTPUT"};
        char what[64], channel[24], hex[3 * UA101_VOLUME_MESSAGE_BYTES + 1];
        size_t i;

        S.counts.ctl_changes++;
        /* What passed is a saving message, a patch bay change, a volume, a pan, an on/off, a
           link or a solo. The 13-byte ones are told apart by their address: 02 01 the patch
           bay; 03 to 05 with 02 at the end an on/off, with 01 a link, with 00 a solo (04 and
           05 only); 00 00 00 the saving (an on/off was once logged as the saving,
           2026-11-13). */
        /* The channel as the user knows it: MONITOR OUT L or R, INPUT 1 to 10, OUTPUT 1 to 10. */
        if (msg[7] == 0x03)
            snprintf(channel, sizeof(channel), "MONITOR OUT %s",
                     msg[8] == 0 ? "L" : msg[8] == 1 ? "R" : "?");
        else
            snprintf(channel, sizeof(channel), "%s %u", group_name[(msg[7] - 3u) % 3u],
                     msg[8] + 1u);
        if (len == UA101_PAN_MESSAGE_BYTES)
            snprintf(what, sizeof(what), "pan, %s to 0x%X%X%X%X", channel, msg[10], msg[11],
                     msg[12], msg[13]);
        else if (len == UA101_VOLUME_MESSAGE_BYTES)
            snprintf(what, sizeof(what), "volume, %s to 0x%X%X%X%X%X%X", channel, msg[10],
                     msg[11], msg[12], msg[13], msg[14], msg[15]);
        else if (msg[7] == 0x02)
            snprintf(what, sizeof(what), "patch bay, OUTPUT %u-%u to source %u", 2u * msg[9] + 1,
                     2u * msg[9] + 2, (unsigned)msg[10]);
        else if (msg[7] >= 0x03 && msg[7] <= 0x05 && msg[9] == 0x02)
            snprintf(what, sizeof(what), "on/off, %s to bit %u", channel, (unsigned)msg[10]);
        else if (msg[7] == 0x03 && msg[9] == 0x01)
            snprintf(what, sizeof(what), "link, MONITOR OUT L-R to %u", (unsigned)msg[10]);
        else if (msg[7] >= 0x04 && msg[7] <= 0x05 && msg[9] == 0x01)
            snprintf(what, sizeof(what), "link, %s %u-%u to %u", group_name[msg[7] - 3u],
                     msg[8] + 1u, msg[8] + 2u, (unsigned)msg[10]);
        else if (msg[7] >= 0x04 && msg[7] <= 0x05 && msg[9] == 0x00 && msg[10] == 0)
            snprintf(what, sizeof(what), "solo, %s group to 0 (no solo)", group_name[msg[7] - 3u]);
        else if (msg[7] >= 0x04 && msg[7] <= 0x05 && msg[9] == 0x00)
            snprintf(what, sizeof(what), "solo, %s group to %u (%s %u alone)",
                     group_name[msg[7] - 3u], (unsigned)msg[10], group_name[msg[7] - 3u],
                     (unsigned)msg[10]);
        else if (msg[7] == 0x00 && msg[8] == 0x00 && msg[9] == 0x00)
            snprintf(what, sizeof(what), "%s", msg[10] == 0 ? "pause saving" : "resume saving");
        else
            snprintf(what, sizeof(what), "a change");
        for (i = 0; i < len && i < UA101_VOLUME_MESSAGE_BYTES; i++)
            snprintf(hex + 3 * i, 4, "%02X ", msg[i]);
        hex[3 * i - 1] = '\0';
        say("passed on to the unit: %s (%s)", what, hex);
    } else {
        int read_of = memory_read_passes(msg, len);

        S.counts.ctl_sent++;
        /* A memory read is rare (a click in Recall Memory) and goes in the log: what was
           asked of the unit is the user's record, even when it changes nothing. */
        if (read_of >= 0) {
            S.counts.ctl_memory_reads++;
            say("passed on to the unit: a read of Memory %d (changes nothing)", read_of + 1);
        }
    }
    return UA101_LINK_OK;
}

/* The largest |sample| of each channel in the frames written since *seen, a quarter second at
   most. Reads the ring as the plug-in does, from the helper's own checked view. */
static void ring_peaks(const ua101_frame_view *v, uint32_t channels, uint64_t *seen,
                       uint32_t peaks[UA101_LINK_MAX_CHANNELS])
{
    uint64_t end = ua101_frame_view_end(v);
    uint64_t window = (uint64_t)(S.format.rate_hz * LEVEL_WINDOW_SECONDS);
    uint64_t at = *seen <= end ? *seen : 0; /* sample time started again from 0 */

    memset(peaks, 0, sizeof(uint32_t) * UA101_LINK_MAX_CHANNELS);
    if (end - at > window)
        at = end - window;
    while (at < end) {
        uint64_t n = end - at < LEVEL_CHUNK_FRAMES ? end - at : LEVEL_CHUNK_FRAMES;
        uint64_t i;
        uint32_t c;

        ua101_frame_view_read(v, at, g_level_frames, n, NULL);
        for (i = 0; i < n; i++)
            for (c = 0; c < channels; c++) {
                int32_t x = g_level_frames[i * channels + c];
                uint32_t a = x < 0 ? (uint32_t)0 - (uint32_t)x : (uint32_t)x;

                if (a > peaks[c])
                    peaks[c] = a;
            }
        at += n;
    }
    *seen = end;
}

static void fill_levels(peer *p, xpc_object_t reply)
{
    uint32_t peaks[UA101_LINK_MAX_CHANNELS];
    int live = S.streaming && S.views_ok &&
               atomic_load(&S.shared->state) == UA101_STATE_RUNNING;

    xpc_dictionary_set_bool(reply, UA101_LINK_STREAMING, live);
    if (!live)
        return;
    if (p->level_block != S.block_id) {
        p->level_block = S.block_id;
        p->capture_seen = p->playback_seen = 0;
    }
    xpc_dictionary_set_double(reply, UA101_LINK_RATE, S.format.rate_hz);
    ring_peaks(&S.capture_view, S.format.capture_channels, &p->capture_seen, peaks);
    xpc_dictionary_set_data(reply, UA101_LINK_CAPTURE_PEAKS, peaks,
                            sizeof(uint32_t) * S.format.capture_channels);
    ring_peaks(&S.playback_view, S.format.playback_channels, &p->playback_seen, peaks);
    xpc_dictionary_set_data(reply, UA101_LINK_PLAYBACK_PEAKS, peaks,
                            sizeof(uint32_t) * S.format.playback_channels);
    S.counts.levels_given++;
}

static void watch_engine(void)
{
    const char *failure;
    uint32_t heartbeat;

    if (!S.streaming)
        return;
    if ((failure = ua101_engine_failure(S.engine)) == NULL) {
        heartbeat = atomic_load(&S.shared->heartbeat);
        if (heartbeat != S.watch_heartbeat) {
            S.watch_heartbeat = heartbeat;
            S.watch_still = 0;
        } else if (++S.watch_still * (WATCH_MS / 1000.0) >= STUCK_SECONDS) {
            say("the USB thread is stuck: exiting, to be started again");
            _exit(1);
        }
        return;
    }
    say("streaming stopped by itself: %s", failure);
    close_unit();
    S.announced = 0;
    tell_all("changed");
}

/* --- The bus and the Mac's sleep. Each runs on the service queue. --- */

static void unit_arrived(void)
{
    S.units++;
    S.short_failures = 0;
    say("a UA-101 appeared on the bus");
    /* Plug-ins that think there is none say hello, which opens it. While asleep, waking tells. */
    if (!S.announced && !S.asleep)
        tell_all("changed");
    if (!S.asleep) {
        tell_control("changed", NULL, 0);
        tell_midi_changed();
    }
}

static void unit_left(void)
{
    int was_open = S.engine != NULL;

    if (S.units > 0)
        S.units--;
    say("a UA-101 left the bus: unplugged or switched off");
    if (S.units == 0) {
        /* The engine would notice by itself only while streaming, and up to a second later. */
        close_unit();
        detach_all(!S.asleep);
        if ((was_open || S.announced) && !S.asleep) {
            S.announced = 0;
            tell_all("changed");
        }
    }
    S.short_failures = 0;
}

/*
 * Before the Mac sleeps: streaming stops and the unit is given back, with its interfaces at
 * their idle setting, so nothing is left running into a sleeping bus, and the Mac's clock,
 * which stands still during sleep, is never used across it. Plug-ins aren't told and keep their
 * device; while asleep, hello and start are answered "asleep", which changes nothing for them.
 */
static void will_sleep(void)
{
    peer *p;

    S.resume = 0;
    for (p = S.peers; p != NULL; p = p->next)
        S.resume |= p->started;
    S.asleep = 1;
    say("the Mac is going to sleep%s", S.engine != NULL ? ": giving the unit back" : "");
    close_unit();
    detach_all(0); /* a control app is told when the Mac wakes */
}

/*
 * After waking: a plug-in whose stream was cut by the sleep is told, says hello, finds a new
 * block and has Core Audio restart its I/O, as after any stream that started over. One that
 * wasn't streaming is told only if the unit came or went meanwhile; otherwise it says hello by
 * itself at its next start. If the unit won't open or stream now, the usual paths apply.
 */
static void did_wake(void)
{
    if (!S.asleep)
        return;
    S.asleep = 0;
    S.short_failures = 0;
    say("the Mac woke%s", S.resume ? ": streaming was on, so the plug-in is told" : "");
    if (S.resume || (S.watching && S.announced != (S.units > 0))) {
        S.announced = 0;
        tell_all("changed");
    }
    S.resume = 0;
    tell_control("changed", NULL, 0);
    tell_midi_changed();
}

static int start_streaming(char *why, size_t why_bytes)
{
    if (S.streaming)
        return 1;
    S.watch_heartbeat = atomic_load(&S.shared->heartbeat);
    S.watch_still = 0;
    if (!ua101_engine_start(S.engine, S.shared, S.shared_bytes)) {
        snprintf(why, why_bytes, "the streaming thread couldn't start");
        return 0;
    }
    S.streaming = 1;
    S.watch = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, S.queue);
    dispatch_source_set_timer(S.watch, dispatch_time(DISPATCH_TIME_NOW, WATCH_MS * NSEC_PER_MSEC),
                              WATCH_MS * NSEC_PER_MSEC, 20 * NSEC_PER_MSEC);
    dispatch_source_set_event_handler(S.watch, ^{
      watch_engine();
    });
    dispatch_resume(S.watch);
    tell_control("audio", NULL, 0);
    say("streaming started");
    return 1;
}

static void arm_idle_exit(void)
{
    unsigned generation = ++S.idle_generation;

    if (S.opt.idle_exit_seconds <= 0.0)
        return;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,
                                 (int64_t)(S.opt.idle_exit_seconds * NSEC_PER_SEC)),
                   S.queue, ^{
                     if (S.peers == NULL && generation == S.idle_generation) {
                         say("no plug-in for %.0f s, exiting", S.opt.idle_exit_seconds);
                         detach_all(0);
                         close_unit();
                         exit(0);
                     }
                   });
}

/*
 * Gives the unit back once it has been open this long with no streaming and nothing asked of
 * the helper. Plug-ins aren't told: they keep the format they know, and say hello before each
 * start, which opens the unit again and gives them the new block.
 */
static void arm_idle_close(void)
{
    unsigned generation = ++S.close_generation;

    if (S.opt.idle_close_seconds <= 0.0 || S.engine == NULL || S.streaming)
        return;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,
                                 (int64_t)(S.opt.idle_close_seconds * NSEC_PER_SEC)),
                   S.queue, ^{
                     if (generation == S.close_generation && S.engine != NULL && !S.streaming) {
                         say("no streaming for %.0f s: giving the unit back",
                             S.opt.idle_close_seconds);
                         close_unit();
                     }
                   });
}

/* With ctl_open: what this helper passes on. */
static void say_passes(xpc_object_t reply)
{
    xpc_dictionary_set_uint64(reply, UA101_LINK_CTL_PASSES, UA101_CTL_PASSES);
    xpc_dictionary_set_uint64(reply, UA101_LINK_CTL_PASSES_ON, UA101_CTL_PASSES_ON);
    xpc_dictionary_set_uint64(reply, UA101_LINK_CTL_PASSES_MEMORY, UA101_CTL_PASSES_MEMORY);
    xpc_dictionary_set_uint64(reply, UA101_LINK_CTL_PASSES_READ, UA101_CTL_PASSES_READ);
}

static void handle_request(peer *p, xpc_object_t msg)
{
    xpc_object_t reply = xpc_dictionary_create_reply(msg);
    const char *op = xpc_dictionary_get_string(msg, UA101_LINK_OP);
    char why[256] = "";
    int64_t status = UA101_LINK_OK;
    int wants_reply = reply != NULL;
    /* The user's side: the control app's requests and a MIDI client's. */
    int is_control = ua101_service_owner_request(op);

    /* A message that waits for no reply is still obeyed: a plug-in that gave up waiting for a
       start sends its stop that way. */
    if (reply == NULL && (reply = xpc_dictionary_create(NULL, NULL, 0)) == NULL)
        return;
    if (op == NULL) {
        status = UA101_LINK_ERR_REQUEST;
        snprintf(why, sizeof(why), "no operation");
    } else if (xpc_dictionary_get_uint64(msg, UA101_LINK_KEY_VERSION) != UA101_LINK_VERSION) {
        status = UA101_LINK_ERR_VERSION;
        snprintf(why, sizeof(why), "the plug-in and the helper are from different builds");
    } else if (!ua101_service_allows(p->uid, is_control, S.coreaudio_uid, S.own_uid,
                                     S.opt.allow_uid)) {
        status = UA101_LINK_ERR_DENIED;
        S.counts.denied++;
        snprintf(why, sizeof(why), "user %d may not ask for \"%.20s\"", (int)p->uid, op);
    } else if (S.asleep && (strcmp(op, "hello") == 0 || strcmp(op, "start") == 0 ||
                            strcmp(op, "ctl_open") == 0 || strcmp(op, "midi_open") == 0)) {
        status = UA101_LINK_ERR_ASLEEP;
        snprintf(why, sizeof(why), "the Mac is going to sleep or waking");
    } else if (strcmp(op, "ctl_open") == 0) {
        p->control = 1;
        if (open_port(why, sizeof(why))) {
            p->attached = 1;
            xpc_dictionary_set_bool(reply, UA101_LINK_PRESENT, true);
        } else {
            status = UA101_LINK_ERR_ABSENT;
            xpc_dictionary_set_bool(reply, UA101_LINK_PRESENT, false);
            /* With no unit on the bus an event comes when one appears. One that is there but
               couldn't be opened (another program has its MIDI interface) is asked for again. */
            xpc_dictionary_set_bool(reply, UA101_LINK_WATCHING, S.watching && S.units == 0);
        }
        xpc_dictionary_set_bool(reply, UA101_LINK_STREAMING, S.streaming);
        say_passes(reply);
    } else if (strcmp(op, "ctl_send") == 0) {
        size_t len = 0;
        const uint8_t *data = xpc_dictionary_get_data(msg, UA101_LINK_DATA, &len);

        p->control = 1;
        if (!p->attached || S.port == NULL) {
            status = UA101_LINK_ERR_ABSENT;
            snprintf(why, sizeof(why), "not attached to the unit");
        } else {
            status = send_to_unit(data, len, why, sizeof(why));
        }
    } else if (strcmp(op, "ctl_close") == 0) {
        p->control = 1;
        p->attached = 0;
        if (!anyone_attached())
            close_port();
    } else if (strcmp(op, "levels") == 0) {
        p->control = 1;
        fill_levels(p, reply);
    } else if (strcmp(op, "midi_open") == 0) {
        p->midi = 1;
        if (open_port(why, sizeof(why))) {
            if (!p->midi_attached) {
                p->midi_attached = 1;
                ua101_midifeed_init(&p->feed);
            }
            if (!S.midi_session) {
                S.midi_session = 1;
                say("a MIDI client attached: what arrives at MIDI IN is passed to it, and what "
                    "it sends goes to MIDI OUT");
            }
            xpc_dictionary_set_bool(reply, UA101_LINK_PRESENT, true);
        } else {
            status = UA101_LINK_ERR_ABSENT;
            xpc_dictionary_set_bool(reply, UA101_LINK_PRESENT, false);
            /* As for a control app: with no unit on the bus an event comes when one appears. */
            xpc_dictionary_set_bool(reply, UA101_LINK_WATCHING, S.watching && S.units == 0);
        }
    } else if (strcmp(op, "midi_send") == 0) {
        size_t len = 0;
        const uint8_t *data = xpc_dictionary_get_data(msg, UA101_LINK_DATA, &len);

        p->midi = 1;
        if (!p->midi_attached || S.port == NULL) {
            status = UA101_LINK_ERR_ABSENT;
            S.counts.midi_refused++;
            snprintf(why, sizeof(why), "not attached to the unit");
        } else if (data == NULL || len == 0 || len > UA101_LINK_MIDI_SEND_MAX) {
            status = UA101_LINK_ERR_REQUEST;
            S.counts.midi_refused++;
            snprintf(why, sizeof(why), "no MIDI bytes, or more than %d at once",
                     UA101_LINK_MIDI_SEND_MAX);
        } else {
            midi_from_client(p, data, len);
        }
    } else if (strcmp(op, "midi_close") == 0) {
        p->midi = 1;
        midi_detach(p);
        if (!anyone_attached())
            close_port();
    } else if (strcmp(op, "hello") == 0) {
        if (open_unit(why, sizeof(why))) {
            xpc_dictionary_set_bool(reply, UA101_LINK_PRESENT, true);
            xpc_dictionary_set_double(reply, UA101_LINK_RATE, S.format.rate_hz);
            xpc_dictionary_set_uint64(reply, UA101_LINK_IN, S.format.capture_channels);
            xpc_dictionary_set_uint64(reply, UA101_LINK_OUT, S.format.playback_channels);
            xpc_dictionary_set_uint64(reply, UA101_LINK_INPUT_OFFSET, S.input_offset);
            xpc_dictionary_set_uint64(reply, UA101_LINK_OUTPUT_OFFSET, S.output_offset);
            xpc_dictionary_set_value(reply, UA101_LINK_SHMEM, S.shmem);
            xpc_dictionary_set_uint64(reply, UA101_LINK_BLOCK, S.block_id);
            S.announced = 1;
        } else {
            status = UA101_LINK_ERR_ABSENT;
            xpc_dictionary_set_bool(reply, UA101_LINK_PRESENT, false);
            /* With no unit on the bus, or one given up on, an event comes when that changes,
               so the plug-in needn't keep asking. One that is there but couldn't be opened
               (another program has it) is asked for again. */
            xpc_dictionary_set_bool(reply, UA101_LINK_WATCHING,
                                    S.watching && (S.units == 0 || given_up()));
            S.announced = 0;
        }
    } else if (strcmp(op, "start") == 0) {
        if (!open_unit(why, sizeof(why)))
            status = UA101_LINK_ERR_ABSENT;
        else if (!start_streaming(why, sizeof(why)))
            status = UA101_LINK_ERR_START;
        else
            p->started = 1;
    } else if (strcmp(op, "stop") == 0) {
        peer *q;
        int anyone = 0;

        p->started = 0;
        for (q = S.peers; q != NULL; q = q->next)
            anyone |= q->started;
        if (!anyone) {
            xpc_object_t missing = xpc_dictionary_get_value(msg, UA101_LINK_MISSING);

            S.plugin_counts = missing != NULL && xpc_get_type(missing) == XPC_TYPE_UINT64;
            S.plugin_missing = xpc_dictionary_get_uint64(msg, UA101_LINK_MISSING);
            S.plugin_clipped = xpc_dictionary_get_uint64(msg, UA101_LINK_CLIPPED);
            {
                xpc_object_t peak = xpc_dictionary_get_value(msg, UA101_LINK_PEAK);
                xpc_object_t cut = xpc_dictionary_get_value(msg, UA101_LINK_CUT);

                S.plugin_has_peak = peak != NULL && xpc_get_type(peak) == XPC_TYPE_DOUBLE;
                S.plugin_peak = xpc_dictionary_get_double(msg, UA101_LINK_PEAK);
                S.plugin_has_cut = cut != NULL && xpc_get_type(cut) == XPC_TYPE_UINT64;
                S.plugin_cut = xpc_dictionary_get_uint64(msg, UA101_LINK_CUT);
                S.plugin_cut_peak = xpc_dictionary_get_double(msg, UA101_LINK_CUT_PEAK);
            }
            {
                xpc_object_t in = xpc_dictionary_get_value(msg, UA101_LINK_INPUT_OFFSET);
                xpc_object_t out = xpc_dictionary_get_value(msg, UA101_LINK_OUTPUT_OFFSET);
                xpc_object_t spare = xpc_dictionary_get_value(msg, UA101_LINK_CAPTURE_SPARE);
                uint64_t in_frames = xpc_dictionary_get_uint64(msg, UA101_LINK_INPUT_OFFSET);
                uint64_t out_frames = xpc_dictionary_get_uint64(msg, UA101_LINK_OUTPUT_OFFSET);

                /* Only for the log. A number no offset can be is left out. */
                S.plugin_has_offsets = in != NULL && xpc_get_type(in) == XPC_TYPE_UINT64 &&
                                       out != NULL && xpc_get_type(out) == XPC_TYPE_UINT64 &&
                                       in_frames <= 1000000 && out_frames <= 1000000;
                S.plugin_input_offset = S.plugin_has_offsets ? (uint32_t)in_frames : 0;
                S.plugin_output_offset = S.plugin_has_offsets ? (uint32_t)out_frames : 0;
                S.plugin_has_spare = spare != NULL && xpc_get_type(spare) == XPC_TYPE_INT64;
                S.plugin_capture_spare = xpc_dictionary_get_int64(msg, UA101_LINK_CAPTURE_SPARE);
            }
            stop_streaming();
            S.plugin_counts = 0;
        }
    } else {
        status = UA101_LINK_ERR_REQUEST;
        snprintf(why, sizeof(why), "unknown operation \"%.40s\"", op);
    }
    xpc_dictionary_set_int64(reply, UA101_LINK_STATUS, status);
    if (status != UA101_LINK_OK) {
        xpc_dictionary_set_string(reply, UA101_LINK_WHY, why);
        /* A start waiting for the wake asks many times, and a refusal is logged where counted. */
        /* Nor is each MIDI message that came too late for a unit that has gone: counted. */
        if (status != UA101_LINK_ERR_ASLEEP && status != UA101_LINK_ERR_REFUSED &&
            (op == NULL || strcmp(op, "midi_send") != 0))
            say("%s: %s", op != NULL ? op : "?", why);
    }
    if (wants_reply)
        xpc_connection_send_message(p->conn, reply);
    xpc_release(reply);
    /* Counted from the last thing a plug-in asked; does nothing while streaming. What a control
       app asks doesn't keep the audio side open. */
    if (!is_control)
        arm_idle_close();
}

static void drop_peer(peer *p)
{
    peer **at;
    int anyone = 0;

    for (at = &S.peers; *at != NULL; at = &(*at)->next)
        if (*at == p) {
            *at = p->next;
            break;
        }
    say("%s disconnected", p->control ? "a control app" : p->midi ? "a MIDI client" : "a plug-in");
    midi_detach(p);
    xpc_release(p->conn);
    free(p);
    for (p = S.peers; p != NULL; p = p->next)
        anyone |= p->started;
    if (!anyone)
        stop_streaming();
    if (!anyone_attached())
        close_port();
    arm_idle_close();
    if (S.peers == NULL) {
        close_unit();
        arm_idle_exit();
    }
}

int ua101_service_owner_request(const char *op)
{
    return op != NULL && (strncmp(op, "ctl_", 4) == 0 || strcmp(op, "levels") == 0 ||
                          strncmp(op, "midi_", 5) == 0);
}

int ua101_service_allows(uid_t uid, int control_request, uid_t coreaudio_uid, uid_t own_uid,
                         int allow_uid)
{
    if (uid == 0 || (allow_uid >= 0 && uid == (uid_t)allow_uid))
        return 1;
    /* Audio: Core Audio only. Control: the user the helper runs as, or any account a person
       logs in to (macOS numbers those from 501), never Core Audio's. */
    if (!control_request)
        return uid == coreaudio_uid;
    if (uid == coreaudio_uid)
        return 0;
    return uid == own_uid || ua101_service_person(uid);
}

int ua101_service_person(uid_t uid)
{
    /* 501 is the first account macOS makes for a person; nobody (-2) and the other system
       accounts are below it or wrap round to the top. */
    return uid >= 501 && uid < 0x80000000u;
}

static int allowed(xpc_connection_t c)
{
    uid_t uid = xpc_connection_get_euid(c);

    return ua101_service_allows(uid, 0, S.coreaudio_uid, S.own_uid, S.opt.allow_uid) ||
           ua101_service_allows(uid, 1, S.coreaudio_uid, S.own_uid, S.opt.allow_uid);
}

static void accept_peer(xpc_connection_t c)
{
    peer *p;

    if (!allowed(c)) {
        say("refused a connection from user %d", (int)xpc_connection_get_euid(c));
        xpc_connection_cancel(c);
        return;
    }
    p = calloc(1, sizeof(*p));
    if (p == NULL) {
        xpc_connection_cancel(c);
        return;
    }
    p->conn = (xpc_connection_t)xpc_retain(c);
    p->uid = xpc_connection_get_euid(c);
    p->next = S.peers;
    S.peers = p;
    ++S.idle_generation; /* someone's here: no idle exit */
    /* Each connection delivers on a queue of its own, in order, and steps onto the service
       queue for the work. (Making the service queue the connection's target would serialize
       just as well, but the thread checker can't see through that, and this it can.) */
    xpc_connection_set_event_handler(c, ^(xpc_object_t event) {
      dispatch_sync(S.queue, ^{
        xpc_type_t type = xpc_get_type(event);

        if (type == XPC_TYPE_DICTIONARY)
            handle_request(p, event);
        else if (type == XPC_TYPE_ERROR && event == XPC_ERROR_CONNECTION_INVALID)
            drop_peer(p);
      });
    });
    xpc_connection_resume(c);
    say("%s connected (user %d)", p->uid == S.coreaudio_uid ? "a plug-in" : "a client",
        (int)p->uid);
}

static void setup(const ua101_service_options *opt)
{
    struct passwd *pw = getpwnam("_coreaudiod");

    S.opt = *opt;
    S.coreaudio_uid = pw != NULL ? pw->pw_uid : 202;
    S.own_uid = getuid();
    S.queue = dispatch_queue_create("ua101d.service", DISPATCH_QUEUE_SERIAL);
    if (opt->watch)
        dispatch_sync(S.queue, ^{
          static const ua101_watch_events events = {unit_arrived, unit_left, will_sleep,
                                                    did_wake};

          S.watching = ua101_watch_start(S.queue, &events, &S.units);
          if (S.watching)
              say("watching the bus: %d UA-101 there now", S.units);
          else
              say("the bus can't be watched: plug-ins keep asking for the unit");
        });
}

static void listen_on(xpc_connection_t listener)
{
    S.listener = listener;
    xpc_connection_set_event_handler(listener, ^(xpc_object_t event) {
      if (xpc_get_type(event) == XPC_TYPE_CONNECTION)
          dispatch_sync(S.queue, ^{
            accept_peer((xpc_connection_t)event);
          });
    });
    xpc_connection_resume(listener);
}

int ua101_service_run(const ua101_service_options *opt)
{
    dispatch_source_t term;
    long long trimmed = 0;

    /* Before anything else writes to it, and long before the unit is opened. */
    if (opt->log != NULL)
        trimmed = ua101_service_trim_log(fileno(opt->log), UA101_LOG_LIMIT_BYTES,
                                         UA101_LOG_KEEP_BYTES);
    setup(opt);
    if (trimmed > 0)
        say("the log was over %d KB: its oldest %lld bytes were removed", UA101_LOG_LIMIT_BYTES / 1024,
            trimmed);
    else if (trimmed < 0)
        say("the log is over its limit or couldn't be measured, and couldn't be trimmed");
    signal(SIGTERM, SIG_IGN);
    term = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGTERM, 0, S.queue);
    dispatch_source_set_event_handler(term, ^{
      say("asked to stop");
      detach_all(0);
      close_unit();
      exit(0);
    });
    dispatch_resume(term);

    listen_on(xpc_connection_create_mach_service(UA101_SERVICE_NAME, NULL,
                                                 XPC_CONNECTION_MACH_SERVICE_LISTENER));
    say("listening as %s", UA101_SERVICE_NAME);
    dispatch_async(S.queue, ^{
      arm_idle_exit(); /* launchd started it for a plug-in; if none comes, don't linger */
    });
    dispatch_main();
}

xpc_endpoint_t ua101_service_start_anonymous(const ua101_service_options *opt)
{
    xpc_connection_t listener;

    setup(opt);
    listener = xpc_connection_create(NULL, NULL);
    listen_on(listener);
    return xpc_endpoint_create(listener);
}

void ua101_service_shutdown(void)
{
    dispatch_sync(S.queue, ^{
      detach_all(0);
      close_unit();
    });
}

void ua101_service_get_counts(ua101_service_counts *out)
{
    dispatch_sync(S.queue, ^{
      peer *p;

      *out = S.counts;
      out->port_open = S.port != NULL;
      out->midi_lamp_timer_on = S.lamp_timer_on;
      add_gate_counts(&out->gate, &S.gate.counts);
      for (p = S.peers; p != NULL; p = p->next)
          out->midi_clients += p->midi_attached;
    });
}

const ua101_shared *ua101_service_block(void)
{
    __block const ua101_shared *block;

    dispatch_sync(S.queue, ^{
      block = S.shared;
    });
    return block;
}
