/*
 * The MIDI lamps' pacing (src/ua101_midilamp.h), with time moved by hand: never more than 10
 * events a second however much MIDI passes, nothing while MIDI is quiet, at most one wake-up
 * booked and none left after the last MIDI, nothing lost, and active sensing alone lights
 * nothing. The helper's use of it, with its real link and timer, is tested in
 * tests/test_ctlhelper.c (test_midi_lamps).
 */
#include <stdio.h>
#include <string.h>

#include "../src/ua101_midilamp.h"

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

/* A pretend helper: the lamp, its one booked wake-up, and what it told. */
typedef struct {
    ua101_midilamp lamp;
    uint64_t wake_at;  /* 0: nothing booked */
    unsigned told;     /* events */
    unsigned told_in, told_out;
    uint64_t told_at[100000];
    unsigned wakeups;  /* wake-ups that came */
    unsigned bookings; /* times a wake-up was booked anew */
} sim;

static void sim_init(sim *s)
{
    memset(s, 0, sizeof(*s));
    ua101_midilamp_init(&s->lamp);
}

static void sim_tell(sim *s, unsigned flags, uint64_t now)
{
    if (flags == 0)
        return;
    if (s->told < sizeof(s->told_at) / sizeof(s->told_at[0]))
        s->told_at[s->told] = now;
    s->told++;
    s->told_in += (flags & UA101_MIDILAMP_IN) != 0;
    s->told_out += (flags & UA101_MIDILAMP_OUT) != 0;
}

static void sim_step(sim *s, unsigned flags, uint64_t now)
{
    uint64_t wake;

    sim_tell(s, ua101_midilamp_step(&s->lamp, flags, now, &wake), now);
    if (wake != 0 && wake != s->wake_at)
        s->bookings++;
    s->wake_at = wake;
}

/* Time moves to `now`: a booked wake-up that is due comes first. */
static void sim_advance(sim *s, uint64_t now)
{
    while (s->wake_at != 0 && s->wake_at <= now) {
        uint64_t at = s->wake_at;

        s->wake_at = 0;
        s->wakeups++;
        sim_step(s, 0, at);
    }
}

/* The most events in any one second, and the smallest gap between two. */
static void sim_rate(const sim *s, unsigned *most, uint64_t *smallest_gap)
{
    unsigned i, j;

    *most = 0;
    *smallest_gap = UINT64_MAX;
    for (i = 0; i < s->told; i++) {
        for (j = i; j < s->told && s->told_at[j] - s->told_at[i] < 1000000; j++)
            ;
        if (j - i > *most)
            *most = j - i;
        if (i > 0 && s->told_at[i] - s->told_at[i - 1] < *smallest_gap)
            *smallest_gap = s->told_at[i] - s->told_at[i - 1];
    }
}

static void test_quiet(void)
{
    static sim s;
    uint64_t t;

    sim_init(&s);
    for (t = 1000; t < 60000000; t += 1000)
        sim_advance(&s, t);
    CHECK(s.told == 0 && s.wakeups == 0 && s.bookings == 0 && s.wake_at == 0,
          "a minute with no MIDI: nothing told, nothing booked (%u, %u, %u)", s.told, s.wakeups,
          s.bookings);
    sim_step(&s, 0, 70000000);
    CHECK(s.told == 0 && s.wake_at == 0, "a wake-up with nothing waiting tells and books nothing");
}

static void test_single(void)
{
    static sim s;

    sim_init(&s);
    sim_step(&s, UA101_MIDILAMP_IN, 5000000);
    CHECK(s.told == 1 && s.told_in == 1 && s.told_out == 0 && s.told_at[0] == 5000000,
          "the first MIDI is told at once, IN only");
    CHECK(s.wake_at == 0, "and books nothing");
    sim_step(&s, UA101_MIDILAMP_OUT, 5000000 + UA101_MIDILAMP_GAP_US);
    CHECK(s.told == 2 && s.told_out == 1 && s.wake_at == 0,
          "MIDI a whole gap later is told at once too, OUT only");
    sim_step(&s, UA101_MIDILAMP_IN | UA101_MIDILAMP_OUT, 9000000);
    CHECK(s.told == 3 && s.told_in == 2 && s.told_out == 2, "both at once: one event with both");
    sim_step(&s, 4, 20000000);
    CHECK(s.told == 3 && s.wake_at == 0, "a flag that isn't IN or OUT counts for nothing");
}

/* MIDI within the gap is told at its end, by the one wake-up: nothing is lost. */
static void test_within_gap(void)
{
    static sim s;

    sim_init(&s);
    sim_step(&s, UA101_MIDILAMP_IN, 1000000);
    sim_step(&s, UA101_MIDILAMP_OUT, 1050000);
    CHECK(s.told == 1 && s.wake_at == 1000000 + UA101_MIDILAMP_GAP_US,
          "OUT 50 ms after IN waits for the gap's end, with one wake-up booked for it");
    sim_step(&s, UA101_MIDILAMP_IN, 1060000);
    sim_step(&s, UA101_MIDILAMP_OUT, 1090000);
    CHECK(s.told == 1 && s.bookings == 1, "more MIDI within the gap books nothing more");
    sim_advance(&s, 1099999);
    CHECK(s.told == 1, "nothing before the gap's end");
    sim_advance(&s, 1100000);
    CHECK(s.told == 2 && s.told_in == 2 && s.told_out == 1 && s.told_at[1] == 1100000,
          "at the gap's end one event with both flags seen within it");
    CHECK(s.wake_at == 0, "and then nothing is booked");
    sim_advance(&s, 10000000);
    CHECK(s.told == 2 && s.wakeups == 1, "and nothing more comes");
}

/* A flood of MIDI, by either socket and both: never more than 10 events a second, events
   all along, and after it at most one wake-up within the gap, then nothing. */
static void test_flood(uint64_t every_us, unsigned flags, const char *what)
{
    static sim s;
    uint64_t t, end = 10000000 + 30000000, smallest;
    unsigned most;

    sim_init(&s);
    for (t = 10000000; t < end; t += every_us) {
        sim_advance(&s, t);
        sim_step(&s, flags, t);
        CHECK(s.wake_at == 0 || s.wake_at <= t + UA101_MIDILAMP_GAP_US,
              "%s: a wake-up is never booked further than the gap ahead", what);
    }
    sim_rate(&s, &most, &smallest);
    printf("   note  %s every %llu us for 30 s: %u events, at most %u in a second, the smallest "
           "gap %llu us, %u wake-ups\n",
           what, (unsigned long long)every_us, s.told, most, (unsigned long long)smallest,
           s.wakeups);
    CHECK(most <= 10, "%s: at most 10 events in any second (%u)", what, most);
    CHECK(smallest >= UA101_MIDILAMP_GAP_US, "%s: never two events closer than 0.1 s (%llu us)",
          what, (unsigned long long)smallest);
    CHECK(s.told >= (every_us >= UA101_MIDILAMP_GAP_US ? 30000000 / every_us : 300) - 1,
          "%s: told all along (%u)", what, s.told);
    sim_advance(&s, end + UA101_MIDILAMP_GAP_US);
    CHECK(s.wake_at == 0, "%s: within the gap after the last MIDI nothing is booked any more", what);
    {
        unsigned told = s.told, wakeups = s.wakeups;

        sim_advance(&s, end + 60000000);
        CHECK(s.told == told && s.wakeups == wakeups && s.wake_at == 0,
              "%s: a minute later nothing more has come or run", what);
    }
}

/* A random-looking pattern of IN and OUT a millisecond apart: whatever was seen is told, and
   nothing is left waiting or booked at the end. */
static void test_nothing_lost(void)
{
    static sim s;
    uint64_t t;
    unsigned seen_in = 0, seen_out = 0;

    sim_init(&s);
    for (t = 1000000; t < 21000000; t += 1000) {
        unsigned r = (unsigned)((t * 2654435761u) >> 7);
        unsigned flags = (r % 97 == 0 ? UA101_MIDILAMP_IN : 0) | (r % 89 == 0 ? UA101_MIDILAMP_OUT : 0);

        sim_advance(&s, t);
        seen_in += (flags & UA101_MIDILAMP_IN) != 0;
        seen_out += (flags & UA101_MIDILAMP_OUT) != 0;
        sim_step(&s, flags, t);
    }
    sim_advance(&s, 22000000);
    CHECK(seen_in > 50 && seen_out > 50, "the pattern had both (%u in, %u out)", seen_in, seen_out);
    CHECK(s.told_in > 0 && s.told_out > 0 && s.wake_at == 0 && s.lamp.pending == 0,
          "both were told, nothing seen is left untold, and nothing is left booked");
}

static void test_bytes(void)
{
    static const uint8_t note[] = {0x90, 0x3C, 0x40};
    static const uint8_t sensing[] = {0xFE, 0xFE, 0xFE};
    static const uint8_t clock[] = {0xF8};
    static const uint8_t mixed[] = {0xFE, 0xF8};
    static const uint8_t packet_note[] = {0x09, 0x90, 0x3C, 0x40};
    static const uint8_t packet_sensing[] = {0x0F, 0xFE, 0x00, 0x00, 0x0F, 0xFE, 0x00, 0x00};
    static const uint8_t packet_clock[] = {0x0F, 0xF8, 0x00, 0x00};
    static const uint8_t packet_sysex[] = {0x04, 0xF0, 0x7D, 0x01, 0x06, 0x02, 0xF7, 0x00};
    static const uint8_t packet_control[] = {0x14, 0xF0, 0x41, 0x10, 0x19, 0x90, 0x3C, 0x40};
    static const uint8_t packet_reserved[] = {0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00};
    static const uint8_t packet_sensing_then_note[] = {0x0F, 0xFE, 0x00, 0x00, 0x09, 0x90, 0x3C, 0x40};

    CHECK(ua101_midilamp_bytes_count(note, sizeof(note)), "MIDI IN: a note counts");
    CHECK(!ua101_midilamp_bytes_count(sensing, sizeof(sensing)), "active sensing alone doesn't");
    CHECK(ua101_midilamp_bytes_count(clock, sizeof(clock)), "MIDI clock does (a design choice)");
    CHECK(ua101_midilamp_bytes_count(mixed, sizeof(mixed)), "active sensing with clock does");
    CHECK(!ua101_midilamp_bytes_count(note, 0), "no bytes don't");
    CHECK(ua101_midilamp_packets_count(packet_note, sizeof(packet_note)), "MIDI OUT: a note counts");
    CHECK(!ua101_midilamp_packets_count(packet_sensing, sizeof(packet_sensing)),
          "active sensing alone doesn't");
    CHECK(ua101_midilamp_packets_count(packet_clock, sizeof(packet_clock)), "clock does");
    CHECK(ua101_midilamp_packets_count(packet_sysex, sizeof(packet_sysex)), "SysEx does");
    CHECK(!ua101_midilamp_packets_count(packet_control, sizeof(packet_control)),
          "packets of another cable (the control cable) don't");
    CHECK(!ua101_midilamp_packets_count(packet_reserved, sizeof(packet_reserved)),
          "packets with the reserved codes 0 and 1 don't");
    CHECK(ua101_midilamp_packets_count(packet_sensing_then_note, sizeof(packet_sensing_then_note)),
          "active sensing then a note does");
    CHECK(!ua101_midilamp_packets_count(packet_note, 3), "a part of a packet doesn't");
}

int main(void)
{
    test_quiet();
    test_single();
    test_within_gap();
    test_flood(1000, UA101_MIDILAMP_IN, "IN");
    test_flood(320, UA101_MIDILAMP_OUT, "OUT at the wire's speed");
    test_flood(50000, UA101_MIDILAMP_IN | UA101_MIDILAMP_OUT, "both");
    test_flood(150000, UA101_MIDILAMP_IN, "IN slower than the gap");
    test_nothing_lost();
    test_bytes();
    printf("test_midilamp: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
