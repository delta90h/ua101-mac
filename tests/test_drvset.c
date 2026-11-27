/*
 * The driver's settings (src/ua101_drvset.h): the two lists of delays and how a number of
 * milliseconds names one of their values, and the measure of spare room, on a real frame ring
 * with a pretend app writing and a pretend helper reading, time moved by hand. The plug-in's
 * and the helper's use of them is tested in tests/test_plugin.c and tests/test_service.c.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/ua101_drvset.h"
#include "../src/ua101_framering.h"

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

static void test_lists(void)
{
    static const double rates[] = {44100, 48000, 88200, 96000, 192000};
    static const uint32_t in48[] = {48, 96, 144, 240, 384, 480};
    static const uint32_t out48[] = {96, 192, 288, 384, 480, 576, 672, 768, 1024};
    int i;
    size_t r;

    CHECK(ua101_input_margins[UA101_INPUT_MARGIN_DEFAULT] == UA101_INPUT_OFFSET_SECONDS,
          "the default recording margin is the helper's own, 8 ms since 1.1");
    CHECK(ua101_output_delays[UA101_OUTPUT_DELAY_DEFAULT] == UA101_OUTPUT_OFFSET_SECONDS,
          "the default playback delay is the helper's own, 8 ms since 1.1");
    for (i = 0; i < UA101_INPUT_MARGIN_COUNT; i++) {
        CHECK(ua101_input_margin_index(ua101_input_margins[i] * 1000.0) == i, "margin %d names itself", i);
        CHECK(i == 0 || ua101_input_margins[i] > ua101_input_margins[i - 1], "margins rise");
        /* A transfer is 2 ms long and arrives at its end, so the newest frame is up to 2.2 ms
           old: the recommended margins are over that, and the ones offered with a warning
           (2026-11-20) are the only ones under it. Zero means "not set" and is never one. */
        CHECK(ua101_input_margins[i] >= 0.001 && ua101_input_margins[i] <= 0.010,
              "margin %d is between 1 and 10 ms", i);
        CHECK((ua101_input_margins[i] > 0.0022) == (i >= UA101_INPUT_MARGIN_RECOMMENDED),
              "margin %d: over 2.2 ms exactly when it is recommended", i);
        CHECK(ua101_offset_frames(ua101_input_margins[i], 44100) > 0, "margin %d is never 0 frames", i);
        CHECK(ua101_offset_frames(ua101_input_margins[i], 48000) == in48[i],
              "margin %d is %u frames at 48 kHz, %u", i, in48[i],
              ua101_offset_frames(ua101_input_margins[i], 48000));
    }
    for (i = 0; i < UA101_OUTPUT_DELAY_COUNT; i++) {
        CHECK(ua101_output_delay_index(ua101_output_delays[i] * 1000.0) == i, "delay %d names itself", i);
        CHECK(i == 0 || ua101_output_delays[i] > ua101_output_delays[i - 1], "delays rise");
        /* The helper reads up to 7.0 ms ahead and a transfer is 2 ms more: 9 ms are needed.
           The recommended delays are over that; the ones offered with a warning are under,
           and each names the music-app buffer it needs (checked below). */
        CHECK(ua101_output_delays[i] >= 0.002 && ua101_output_delays[i] <= 0.0214,
              "delay %d is between 2 and 21.4 ms", i);
        CHECK((ua101_output_delays[i] > 0.009) == (i >= UA101_OUTPUT_DELAY_RECOMMENDED),
              "delay %d: over 9 ms exactly when it is recommended", i);
        CHECK((ua101_output_min_buffers[i] > 0.0) == (i < UA101_OUTPUT_DELAY_RECOMMENDED),
              "delay %d names a buffer exactly when it is below the recommended", i);
        CHECK(i == 0 || ua101_output_min_buffers[i] <= ua101_output_min_buffers[i - 1],
              "a longer delay never needs a bigger buffer");
        CHECK(ua101_offset_frames(ua101_output_delays[i], 44100) > 0, "delay %d is never 0 frames", i);
        CHECK(ua101_offset_frames(ua101_output_delays[i], 48000) == out48[i],
              "delay %d is %u frames at 48 kHz, %u", i, out48[i],
              ua101_offset_frames(ua101_output_delays[i], 48000));
    }
    /* At every rate a value is the same time, rounded up to a frame, and never under it. */
    for (r = 0; r < sizeof(rates) / sizeof(rates[0]); r++) {
        for (i = 0; i < UA101_INPUT_MARGIN_COUNT; i++) {
            double frames = ua101_offset_frames(ua101_input_margins[i], rates[r]);

            CHECK(frames / rates[r] >= ua101_input_margins[i] - 1e-9 &&
                      frames / rates[r] < ua101_input_margins[i] + 1.0 / rates[r],
                  "margin %d at %.0f Hz: %.0f frames", i, rates[r], frames);
        }
        for (i = 0; i < UA101_OUTPUT_DELAY_COUNT; i++) {
            double frames = ua101_offset_frames(ua101_output_delays[i], rates[r]);

            CHECK(frames / rates[r] >= ua101_output_delays[i] - 1e-9 &&
                      frames / rates[r] < ua101_output_delays[i] + 1.0 / rates[r],
                  "delay %d at %.0f Hz: %.0f frames", i, rates[r], frames);
        }
    }
    /* What a panel shows, and what an offset comes to at 44.1 kHz (221 and 941 frames: 5.011
       and 21.338 ms; the helper's own 8 ms is 353 frames, 8.005 ms), name the same values. */
    CHECK(ua101_output_delay_index(21.3) == 8 && ua101_output_delay_index(21.33) == 8 &&
              ua101_output_delay_index(1024.0 / 48.0) == 8 &&
              ua101_output_delay_index(941.0 / 44.1) == 8,
          "21.3 ms, however written, is the longest delay");
    CHECK(ua101_input_margin_index(5.0) == 3 && ua101_input_margin_index(221.0 / 44.1) == 3,
          "5 ms, however written, is the margin used until 1.1");
    CHECK(ua101_input_margin_index(353.0 / 44.1) == UA101_INPUT_MARGIN_DEFAULT &&
              ua101_output_delay_index(353.0 / 44.1) == UA101_OUTPUT_DELAY_DEFAULT,
          "the helper's own 8 ms at 44.1 kHz names the two defaults");
    CHECK(ua101_input_margin_index(1.0) == 0 && ua101_input_margin_index(10.0) == 5 &&
              ua101_output_delay_index(2.0) == 0 && ua101_output_delay_index(14.0) == 6,
          "the ends and a middle one");
    /* The shortest recommended ones are the shortest of 2026-11-19: 3 ms and 10 ms. */
    CHECK(ua101_input_margin_index(3.0) == UA101_INPUT_MARGIN_RECOMMENDED &&
              ua101_output_delay_index(10.0) == UA101_OUTPUT_DELAY_RECOMMENDED,
          "3 ms and 10 ms are the shortest recommended values");
    /* The buffers the low playback delays need (2026-11-20, measured at the box): at 48 kHz
       512, 512, 256 and 128 frames; the same times at the other rates, as powers of two. */
    {
        static const unsigned at48[] = {512, 512, 256, 128, 0, 0, 0, 0, 0};
        static const unsigned at441[] = {512, 512, 256, 128, 0, 0, 0, 0, 0};
        static const unsigned at96[] = {1024, 1024, 512, 256, 0, 0, 0, 0, 0};
        static const unsigned at192[] = {2048, 2048, 1024, 512, 0, 0, 0, 0, 0};

        for (i = 0; i < UA101_OUTPUT_DELAY_COUNT; i++) {
            CHECK(ua101_output_min_buffer_frames(i, 48000) == at48[i] &&
                      ua101_output_min_buffer_frames(i, 44100) == at441[i] &&
                      ua101_output_min_buffer_frames(i, 96000) == at96[i] &&
                      ua101_output_min_buffer_frames(i, 192000) == at192[i],
                  "delay %d: buffer %u at 48 kHz, %u at 44.1, %u at 96, %u at 192", i,
                  ua101_output_min_buffer_frames(i, 48000), ua101_output_min_buffer_frames(i, 44100),
                  ua101_output_min_buffer_frames(i, 96000), ua101_output_min_buffer_frames(i, 192000));
        }
        CHECK(ua101_output_min_buffer_frames(-1, 48000) == 0 &&
                  ua101_output_min_buffer_frames(UA101_OUTPUT_DELAY_COUNT, 48000) == 0 &&
                  ua101_output_min_buffer_frames(0, 0.0) == 0 && ua101_output_min_buffer_frames(0, -1.0) == 0,
              "no buffer for a place or a rate that isn't one");
    }
    /* Each of those buffers is the shortest that leaves 2 ms by the line measured at the box:
       room = delay + buffer - need, the need 8.2 ms at 128 frames and 9.4 ms at 512 (48 kHz).
       2 ms is the 1 ms asked for plus the 1.1 ms a busy Mac took away; 10 ms and over leave it
       at 32 frames, the smallest buffer music apps offer. */
    {
        unsigned f;

        for (i = 0; i < UA101_OUTPUT_DELAY_COUNT; i++) {
            double d = ua101_output_delays[i] * 1000.0;
#define ROOM(frames) (d + (frames) / 48.0 - (8.2 + 1.2 * ((frames) / 48.0 - 128.0 / 48.0) / (384.0 / 48.0)))
            f = ua101_output_min_buffer_frames(i, 48000);
            if (f == 0) {
                CHECK(ROOM(32.0) >= 2.0, "delay %d: 2 ms of room at 32 frames (%.2f)", i, ROOM(32.0));
            } else {
                CHECK(ROOM((double)f) >= 2.0 && ROOM(f / 2.0) < 2.0,
                      "delay %d: %u frames leave %.2f ms, half of it %.2f ms", i, f, ROOM((double)f),
                      ROOM(f / 2.0));
            }
#undef ROOM
        }
    }
    /* The defaults since 1.1 (2026-11-27): recording 8 ms is recommended; playback 8 ms is
       not, by the owner's choice, and needs the music app's buffer at 128 frames at 48 kHz. */
    CHECK(UA101_INPUT_MARGIN_DEFAULT >= UA101_INPUT_MARGIN_RECOMMENDED &&
              ua101_input_margins[UA101_INPUT_MARGIN_DEFAULT] == 0.008,
          "the default recording margin is 8 ms, a recommended value");
    CHECK(ua101_output_delays[UA101_OUTPUT_DELAY_DEFAULT] == 0.008 &&
              UA101_OUTPUT_DELAY_DEFAULT < UA101_OUTPUT_DELAY_RECOMMENDED &&
              ua101_output_min_buffer_frames(UA101_OUTPUT_DELAY_DEFAULT, 48000.0) == 128,
          "the default playback delay is 8 ms, which needs a buffer of 128 frames at 48 kHz");
    CHECK(ua101_input_margin_index(2.0) == 1 && ua101_output_delay_index(4.0) == 1 &&
              ua101_output_delay_index(6.0) == 2 && ua101_output_delay_index(8.0) == 3,
          "the places of 2 ms recording, and of 4, 6 and 8 ms playback");
    /* Nothing else names a value. */
    CHECK(ua101_input_margin_index(0.0) < 0 && ua101_input_margin_index(2.9) < 0 &&
              ua101_input_margin_index(0.5) < 0 && ua101_input_margin_index(1.5) < 0 &&
              ua101_input_margin_index(3.06) < 0 && ua101_input_margin_index(4.0) < 0 &&
              ua101_input_margin_index(10.1) < 0 && ua101_input_margin_index(21.33) < 0 &&
              ua101_input_margin_index(-5.0) < 0 && ua101_input_margin_index(5000.0) < 0,
          "no other recording margin");
    CHECK(ua101_output_delay_index(0.0) < 0 && ua101_output_delay_index(5.0) < 0 &&
              ua101_output_delay_index(1.0) < 0 && ua101_output_delay_index(3.0) < 0 &&
              ua101_output_delay_index(7.0) < 0 && ua101_output_delay_index(9.0) < 0 &&
              ua101_output_delay_index(9.9) < 0 && ua101_output_delay_index(11.0) < 0 &&
              ua101_output_delay_index(21.2) < 0 && ua101_output_delay_index(21.4) < 0 &&
              ua101_output_delay_index(-10.0) < 0 && ua101_output_delay_index(1e9) < 0,
          "no other playback delay");
    CHECK(ua101_input_margin_index(NAN) < 0 && ua101_output_delay_index(NAN) < 0 &&
              ua101_input_margin_index(INFINITY) < 0 && ua101_output_delay_index(-INFINITY) < 0,
          "not a number names nothing");
}

static void test_spare_of(void)
{
    CHECK(ua101_spare_of(1000, 900, 0) == 100, "100 frames beyond the last one wanted");
    CHECK(ua101_spare_of(1000, 1000, 0) == 0, "just enough is 0");
    CHECK(ua101_spare_of(1000, 1096, 96) == -96, "96 short, 96 missed");
    /* The writer finished between reading the end and the copy: the copy got everything. */
    CHECK(ua101_spare_of(1000, 1010, 0) == 0, "a read that got everything is never negative");
    /* Or frames were missed although the end, read first, looked far enough (a reader a whole
       ring behind): never a spare left. */
    CHECK(ua101_spare_of(5000, 1000, 40) == -40, "a read that missed frames has no room left");
    CHECK(ua101_spare_of(5000, 1000, UINT64_MAX) == -INT64_MAX, "and doesn't overflow");
}

/* A pretend run at 48 kHz. The app writes `block` frames every `block` frames of time, each
   block `ahead` frames ahead of now; the helper reads 96 frames every 96, `lead` ahead of now.
   Returns what the tracker says; *late what the reads missed after the first full one. */
#define RING 32768
static uint8_t g_frames[RING * 4];

typedef struct {
    ua101_frame_ring ring;
    ua101_spare spare;
    uint64_t late, late_at_full;
    int filled;
} run;

static void run_init(run *r)
{
    memset(r, 0, sizeof(*r));
    ua101_frame_ring_init(&r->ring, g_frames, RING, 4);
    ua101_spare_reset(&r->spare);
}

static void run_write(run *r, uint64_t first, uint32_t frames)
{
    static int32_t zero[4096];

    ua101_frame_ring_write(&r->ring, first, zero, frames);
}

static void run_read(run *r, uint64_t first, uint32_t frames)
{
    static int32_t out[4096];
    ua101_frame_ring_miss miss = {0, 0};
    uint64_t end = ua101_frame_ring_end(&r->ring);

    ua101_frame_ring_read(&r->ring, first, out, frames, &miss);
    ua101_spare_note(&r->spare, end, first + frames, miss.not_yet + miss.overwritten);
    r->late += miss.not_yet + miss.overwritten;
    if (miss.not_yet + miss.overwritten == 0) {
        if (!r->filled) {
            r->filled = 1;
            r->late = 0;
        }
        r->late_at_full = r->late;
    }
}

/* Runs `cycles` app cycles from time `t0`. The cycle numbered `late_cycle` wakes `late_by`
   frames of time late, and the cycles due meanwhile follow right behind it, as on the one
   I/O thread Core Audio has. Then the helper reads on alone for `tail` reads. A write and a
   read due at the same moment: the write comes first. */
static void play(run *r, uint64_t t0, uint32_t block, uint32_t ahead, uint32_t lead, int cycles,
                 int late_cycle, uint32_t late_by, int tail)
{
    uint64_t t, end = t0 + (uint64_t)cycles * block;
    uint64_t next_write = t0, next_read = t0, blocked_until = 0;
    int cycle = 0, late_done = 0;

    for (t = t0; t < end + (uint64_t)tail * 96; t++) {
        while (cycle < cycles && next_write <= t && t >= blocked_until) {
            if (cycle == late_cycle && !late_done) {
                late_done = 1;
                blocked_until = t + late_by;
                break;
            }
            run_write(r, next_write + ahead, block);
            cycle++;
            next_write += block;
        }
        if (t == next_read) {
            run_read(r, t + lead, 96);
            next_read += 96;
        }
    }
}

/* What the ring holds beyond the last read made before a write that comes at time `w` and was
   due at time `due`: the ring then ends at due + ahead, and that read wanted up to its own
   time + lead + 96. Reads fall every 96 frames from t0. */
static int64_t room_before(uint64_t t0, uint64_t due, uint64_t w, uint32_t ahead, uint32_t lead)
{
    uint64_t last_read = t0 + ((w - 1 - t0) / 96) * 96;

    return (int64_t)(due + ahead) - (int64_t)(last_read + lead + 96);
}

/* The least of those over a run in which every cycle is on time. */
static int64_t least_on_time(uint64_t t0, uint32_t block, uint32_t ahead, uint32_t lead, int cycles)
{
    int64_t least = INT64_MAX;
    int k;

    for (k = 1; k < cycles; k++) {
        uint64_t w = t0 + (uint64_t)k * block;
        int64_t room = room_before(t0, w, w, ahead, lead);

        if (room < least)
            least = room;
    }
    return least;
}

static void test_tracker(void)
{
    run r;
    int64_t want;

    /* Nothing measured before anything ran. */
    run_init(&r);
    CHECK(!r.spare.have_least, "a new tracker has measured nothing");

    /* The user's Mac as installed: 512-frame cycles whose frames belong a buffer and the
       safety offset (1024) after the cycle's start, the helper reading up to 336 ahead and 96
       at a time. With an app that writes the moment it wakes, the room is the safety offset
       less the 432 the helper wants, plus most of a buffer. */
    run_init(&r);
    play(&r, 48000, 512, 512 + 1024, 336, 400, -1, 0, 0);
    want = least_on_time(48000, 512, 512 + 1024, 336, 400);
    CHECK(r.spare.have_least && r.spare.least == want,
          "512-frame cycles, 1024 ahead, read 336 ahead: least %lld, worked out %lld",
          (long long)r.spare.least, (long long)want);
    CHECK(want == 1024 - 432 + 512 + 32, "which is 1136 frames: %lld", (long long)want);
    CHECK(r.late_at_full == 0, "and nothing was late");

    /* The start doesn't count: the app's first block lies far ahead of where the helper
       reads, so the first reads get nothing (the same as "late" not counting them). */
    run_init(&r);
    play(&r, 0, 128, 128 + 480, 336, 3, -1, 0, 0);
    CHECK(r.spare.begun && (!r.spare.have_least || r.spare.least >= 0),
          "reads before the app's first block aren't a negative spare");

    /* The end doesn't count: the helper reads on after the app's last cycle and runs the
       ring dry. */
    run_init(&r);
    play(&r, 48000, 128, 128 + 480, 336, 400, -1, 0, 50);
    want = least_on_time(48000, 128, 128 + 480, 336, 400);
    CHECK(r.spare.have_least && r.spare.least == want && want == 480 - 432 + 128 + 32,
          "after the app stops, the tail isn't counted: least %lld, worked out %lld",
          (long long)r.spare.least, (long long)want);
    CHECK(r.spare.have_waiting && r.spare.waiting < 0,
          "though the reads after the last write did run dry (%lld), and wait uncounted",
          (long long)r.spare.waiting);

    /* A shorter delay leaves less, by exactly the difference. */
    {
        run a, b;

        run_init(&a);
        play(&a, 48000, 128, 128 + 768, 336, 500, -1, 0, 0);
        run_init(&b);
        play(&b, 48000, 128, 128 + 576, 336, 500, -1, 0, 0);
        CHECK(a.spare.have_least && b.spare.have_least && a.spare.least - b.spare.least == 768 - 576,
              "16 ms against 12 ms: %lld and %lld frames", (long long)a.spare.least,
              (long long)b.spare.least);
        CHECK(b.late_at_full == 0, "12 ms loses nothing when every cycle is on time");
    }

    /* One cycle wakes 8 ms late with a 10 ms delay and 128-frame cycles: the ring runs dry,
       and the spare says by how much. */
    {
        const uint64_t due = 48000 + 250 * 128;

        run_init(&r);
        play(&r, 48000, 128, 128 + 480, 336, 500, 250, 384, 0);
        want = room_before(48000, due, due + 384, 128 + 480, 336);
        CHECK(want < 0 && r.spare.have_least && r.spare.least == want,
              "a late cycle shows as a negative spare: %lld, worked out %lld",
              (long long)r.spare.least, (long long)want);
        CHECK(r.late_at_full >= (uint64_t)-want && r.late_at_full < 384,
              "and its frames are counted late: %llu, the worst read short by %lld",
              (unsigned long long)r.late_at_full, (long long)-want);
        /* The same late cycle with 21.3 ms: room to spare, nothing lost. */
        run_init(&r);
        play(&r, 48000, 128, 128 + 1024, 336, 500, 250, 384, 0);
        want = room_before(48000, due, due + 384, 128 + 1024, 336);
        CHECK(want > 0 && r.spare.have_least && r.spare.least == want && r.late_at_full == 0,
              "21.3 ms rides through the same late cycle: least %lld, worked out %lld",
              (long long)r.spare.least, (long long)want);
        CHECK(want < least_on_time(48000, 128, 128 + 1024, 336, 500),
              "with less room than a run where every cycle is on time");
    }

    /* A reset forgets everything, as at each start of the stream. */
    ua101_spare_reset(&r.spare);
    CHECK(!r.spare.have_least && !r.spare.have_waiting && !r.spare.begun && r.spare.last_end == 0,
          "reset");

    /* By hand: the least changes only when the end moves, and says so. */
    {
        ua101_spare s;

        ua101_spare_reset(&s);
        CHECK(ua101_spare_note(&s, 0, 96, 96) == 0 && !s.begun, "nothing before the first write");
        CHECK(ua101_spare_note(&s, 1000, 200, 96) == 0 && !s.begun,
              "nor while reads still miss the app's first block");
        CHECK(ua101_spare_note(&s, 1000, 600, 0) == 0 && s.begun && !s.have_least,
              "the first full read begins it, and waits");
        CHECK(ua101_spare_note(&s, 1000, 696, 0) == 0 && s.waiting == 304, "still waiting: 304");
        CHECK(ua101_spare_note(&s, 1512, 792, 0) == 1 && s.have_least && s.least == 304,
              "the end moved: 304 is the least");
        CHECK(ua101_spare_note(&s, 1512, 888, 0) == 0 && s.least == 304, "720, 624 wait");
        CHECK(ua101_spare_note(&s, 2024, 984, 0) == 0 && s.least == 304,
              "624 isn't less than 304: nothing changed");
        CHECK(ua101_spare_note(&s, 2024, 1900, 0) == 0 && s.waiting == 124, "124 waits");
        CHECK(ua101_spare_note(&s, 2024, 2096, 72) == 0 && s.waiting == -72, "then 72 short");
        CHECK(ua101_spare_note(&s, 2536, 2192, 0) == 1 && s.least == -72, "and that counts");
    }
}

int main(void)
{
    test_lists();
    test_spare_of();
    test_tracker();
    printf("test_drvset: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
