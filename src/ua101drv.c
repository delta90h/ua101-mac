/*
 * ua101drv: the driver's settings and what its last run measured, through Core Audio
 * (src/ua101_drvclient.h). With no argument it only reads. Nothing goes to the unit either way.
 *
 *   ua101drv                    print the settings and the statistics
 *   ua101drv --input MS         set the recording margin (3, 5, 8 or 10; 1 and 2 are offered
 *                               too, below the recommended: expect gaps)
 *   ua101drv --output MS        set the playback delay (10, 12, 14, 16 or 21.3; 2, 4, 6 and 8
 *                               are offered too, each clean only with a large enough music-app
 *                               buffer: 8 ms 128 frames, 6 ms 256, 4 and 2 ms 512, at 48 kHz)
 *   ua101drv --correction on|off
 * A changed delay makes Core Audio restart I/O: sound that is playing breaks for a moment.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ua101_drvclient.h"
#include "ua101_drvset.h"

static void four(int status, char text[8])
{
    unsigned u = (unsigned)status;
    int i;

    for (i = 0; i < 4; i++) {
        unsigned char c = (unsigned char)(u >> (24 - 8 * i));

        text[i] = c >= 32 && c < 127 ? (char)c : '?';
    }
    text[4] = '\0';
}

static int report(const char *what, int status)
{
    char code[8];

    if (status == 0) {
        printf("%s: taken\n", what);
        return 0;
    }
    four(status, code);
    printf("%s: refused (%d, '%s')%s\n", what, status, code,
           strcmp(code, "!obj") == 0 ? ": no UA-101 device in Core Audio (is the unit on?)"
           : strcmp(code, "nope") == 0 ? ": not a value the driver offers" : "");
    return 1;
}

static void print_room(const char *name, int known, double frames, double rate, double lost,
                       const char *lost_word)
{
    if (known)
        printf("  %s: least room %+.2f ms (%+.0f frames)", name, frames * 1000.0 / rate, frames);
    else
        printf("  %s: not measured", name);
    printf("; %.0f frames %s\n", lost, lost_word);
}

static int print_state(void)
{
    ua101_drv_state s;
    int status = ua101_drv_read(&s);
    int i;

    if (status != 0) {
        char code[8];

        four(status, code);
        printf("Core Audio couldn't be asked (%d, '%s')\n", status, code);
        return 1;
    }
    if (!s.found) {
        printf("No UA-101 device in Core Audio: the unit is off, or the driver isn't installed.\n");
        return 1;
    }
    if (!s.have_settings) {
        printf("The UA-101's device is there, but it has no driver settings: an older driver?\n");
    } else {
        /* The buffer frames each playback rate needs are at the rate of the last run, or
           48 kHz before any. */
        double rate = s.have_stats && s.rate > 0 ? s.rate : 48000.0;

        /* Each recording margin with its note in brackets; for playback, the brackets hold
           the shortest music-app buffer that keeps a short delay clean. */
        printf("Recording margin: %.2f ms   (offered:", s.input_ms);
        for (i = 0; i < UA101_INPUT_MARGIN_COUNT; i++)
            printf(" %.4g [%s]", ua101_input_margins[i] * 1000.0, ua101_drv_input_note(i));
        printf(")\nPlayback delay:   %.2f ms   (offered:", s.output_ms);
        for (i = 0; i < UA101_OUTPUT_DELAY_COUNT; i++) {
            if (i < UA101_OUTPUT_DELAY_RECOMMENDED)
                printf(" %.4g [buffer %u+]", ua101_output_delays[i] * 1000.0,
                       ua101_output_min_buffer_frames(i, rate));
            else
                printf(" %.4g", ua101_output_delays[i] * 1000.0);
        }
        printf(")\n");
        /* Plain notes for the values in use, as the control app shows them under the sliders
           (2026-11-20, a design choice: no warnings, the recommendations instead). */
        i = ua101_input_margin_index(s.input_ms);
        if (i >= 0)
            printf("Recording margin %.4g ms: %s (the buffer doesn't change this).\n",
                   ua101_input_margins[i] * 1000.0, ua101_drv_input_note(i));
        i = ua101_output_delay_index(s.output_ms);
        if (i >= 0) {
            unsigned frames = ua101_output_min_buffer_frames(i, rate);

            if (frames)
                printf("Playback delay %.4g ms: buffer %u+ (the music app's buffer at %u frames or "
                       "more, at %.0f Hz).\n", ua101_output_delays[i] * 1000.0, frames, frames, rate);
            else
                printf("Playback delay %.4g ms: any buffer.\n", ua101_output_delays[i] * 1000.0);
        }
    }
    printf("Rate correction:  %s\n", s.correction == 1 ? "on" : s.correction == 0 ? "off" : "not read");
    if (!s.have_stats) {
        printf("Statistics: not there\n");
    } else if (!s.any_run) {
        printf("Statistics: no sound has run since Core Audio started\n");
    } else {
        printf("%s: %.1f s at %.0f Hz, recording read %.0f frames behind (%.2f ms), playback "
               "written %.0f ahead (%.2f ms)\n",
               s.running ? "Running now" : "The last run", s.seconds, s.rate, s.input_offset,
               s.input_offset * 1000.0 / s.rate, s.output_offset, s.output_offset * 1000.0 / s.rate);
        print_room("recording", s.capture_spare_known, s.capture_spare, s.rate, s.capture_missing,
                   "missing");
        print_room("playback", s.playback_spare_known, s.playback_spare, s.rate, s.playback_late,
                   "late");
    }
    return 0;
}

int main(int argc, char **argv)
{
    int i, failed = 0;

    for (i = 1; i < argc; i++) {
        if (i + 1 < argc && strcmp(argv[i], "--input") == 0) {
            failed |= report("recording margin", ua101_drv_set_input_ms(atof(argv[++i])));
        } else if (i + 1 < argc && strcmp(argv[i], "--output") == 0) {
            failed |= report("playback delay", ua101_drv_set_output_ms(atof(argv[++i])));
        } else if (i + 1 < argc && strcmp(argv[i], "--correction") == 0 &&
                   (strcmp(argv[i + 1], "on") == 0 || strcmp(argv[i + 1], "off") == 0)) {
            failed |= report("rate correction", ua101_drv_set_correction(strcmp(argv[++i], "on") == 0));
        } else {
            printf("usage: ua101drv [--input MS] [--output MS] [--correction on|off]\n"
                   "With no argument it only reads. Nothing goes to the unit.\n");
            return 2;
        }
    }
    return print_state() | failed;
}
