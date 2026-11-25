/*
 * The driver's settings from an ordinary program (src/ua101_drvset.h): the control app and
 * build/ua101drv read and set them here, through Core Audio's own calls on the UA-101's device.
 * Nothing goes to the helper or to the unit. While the unit is off (or the driver isn't
 * installed) Core Audio has no such device, and there is nothing to read or set.
 */
#ifndef UA101_DRVCLIENT_H
#define UA101_DRVCLIENT_H

typedef struct {
    int found;         /* the UA-101's device is in Core Audio */
    int have_settings; /* the two delays were read */
    double input_ms, output_ms;
    int correction;    /* rate correction: 1 on, 0 off, -1 not read */
    /* The run that is on, or the last one; any_run is 0 before the first since Core Audio
       started. Frames are frames at `rate`. */
    int have_stats, running, any_run;
    double rate, seconds;
    double input_offset, output_offset;
    int capture_spare_known, playback_spare_known;
    double capture_spare, playback_spare; /* negative: frames were missed */
    double capture_missing, playback_late;
} ua101_drv_state;

/* Reads everything. Returns 0, with found 0 if there is no device; or Core Audio's error. */
int ua101_drv_read(ua101_drv_state *out);

/* Each sets one setting and returns 0 or Core Audio's error ('!obj' when there is no device,
   'nope' for a value the driver doesn't offer). A changed delay makes Core Audio restart
   I/O, so sound that is playing breaks for a moment. */
int ua101_drv_set_input_ms(double ms);
int ua101_drv_set_output_ms(double ms);
int ua101_drv_set_correction(int on);

/* The values the driver offers, in milliseconds, shortest first, for a panel: how many, each
   one, which is the default, which is the shortest recommended one (those before it come with
   a plain note of what they need or do, never a warning: 2026-11-20, a design choice), and
   which a number names (-1 for none).
   No Core Audio in these. */
int ua101_drv_input_count(void);
double ua101_drv_input_value(int index);
int ua101_drv_input_default(void);
int ua101_drv_input_recommended(void);
int ua101_drv_input_index(double ms);
int ua101_drv_output_count(void);
double ua101_drv_output_value(int index);
int ua101_drv_output_default(void);
int ua101_drv_output_recommended(void);
int ua101_drv_output_index(double ms);
/* The shortest music-app buffer, in frames at `rate`, that keeps a playback delay below the
   recommended ones clean (2026-11-20, measured at the box); 0 when any buffer will do. */
unsigned ua101_drv_output_min_buffer(int index, double rate);
/* The short note under a recording margin ("gaps", "clean"...); "" for no such place. The
   playback delays' notes are their buffers: "buffer 512+", or "any buffer" for 0 above. */
const char *ua101_drv_input_note(int index);

#endif
