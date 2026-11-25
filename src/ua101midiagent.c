/*
 * ua101midiagent: the UA-101's MIDI IN and OUT sockets as the MIDI port "UA-101 MIDI", for
 * every MIDI app of the user who is logged in.
 *
 * macOS's MIDI server belongs to each logged-in user, so the helper, a system daemon, can't
 * publish MIDI ports itself. This small program runs in the user's session instead (launchd
 * starts it at login from /Library/LaunchAgents: install/install.sh) and does only that: it
 * is a MIDI client of the helper (src/ua101_midilink.h) on one side and two virtual CoreMIDI
 * ports on the other (src/ua101_midiports.h). It never touches USB, and it decides nothing
 * about what may reach the unit: the helper does (src/ua101_midigate.h).
 *
 * The ports are there while the unit is, and go when it goes. While nothing happens the
 * program sleeps: no timer runs while it is attached, or while it waits for a unit to appear.
 *
 * It takes no arguments. What it has to say goes to its log (stderr, which launchd points at
 * /Library/Logs/ua101midiagent.log): a line when the ports appear or go, and why.
 */
#include <dispatch/dispatch.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ua101_midiports.h"

/* The log gets a few lines a day at most. Should it ever pass this size, it starts again. */
#define LOG_LIMIT_BYTES (256 * 1024)

static ua101_midiports *g_ports;

/* CoreMIDI no longer knows the ports: leave with an error, so launchd starts a fresh one. */
static void broken(void *context, const char *why)
{
    (void)context;
    fprintf(stderr, "ua101midiagent: %s: stopping, to be started again\n", why);
    exit(1);
}

static void start_log_again_if_big(void)
{
    struct stat st;
    int flags;

    if (fstat(STDERR_FILENO, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= LOG_LIMIT_BYTES)
        return;
    if (ftruncate(STDERR_FILENO, 0) != 0)
        return;
    lseek(STDERR_FILENO, 0, SEEK_SET);
    flags = fcntl(STDERR_FILENO, F_GETFL);
    if (flags != -1)
        fcntl(STDERR_FILENO, F_SETFL, flags | O_APPEND);
    fprintf(stderr, "ua101midiagent: the log was over %d KB and starts again here\n",
            LOG_LIMIT_BYTES / 1024);
}

int main(int argc, char **argv)
{
    ua101_midiports_options options = {NULL, 0, stderr, broken, NULL};
    dispatch_source_t term;

    (void)argv;
    if (argc > 1) {
        printf("Usage: ua101midiagent\n"
               "  Publishes the UA-101's MIDI IN and OUT as the MIDI port \"" UA101_MIDIPORTS_NAME
               "\" while the unit is\n"
               "  connected, through the installed helper. launchd runs it; it takes no "
               "arguments.\n");
        return 2;
    }
    setvbuf(stderr, NULL, _IOLBF, 0);
    start_log_again_if_big();

    signal(SIGTERM, SIG_IGN);
    term = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGTERM, 0,
                                  dispatch_get_main_queue());
    dispatch_source_set_event_handler(term, ^{
      if (g_ports != NULL)
          ua101_midiports_stop(g_ports);
      exit(0);
    });
    dispatch_resume(term);

    g_ports = ua101_midiports_start(dispatch_get_main_queue(), &options);
    if (g_ports == NULL) {
        fprintf(stderr, "ua101midiagent: out of memory\n");
        return 1;
    }
    dispatch_main();
}
