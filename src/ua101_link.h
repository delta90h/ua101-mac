/*
 * The link between the audio plug-in (UA101.driver, inside Core Audio) and the helper (ua101d,
 * a LaunchDaemon that owns the unit's USB streaming), through launchd's Mach service and XPC.
 *
 * The plug-in sends three messages, each a dictionary with UA101_LINK_OP, and waits for a reply:
 *   hello  the helper opens the unit if it can and replies with its format, the offsets to use
 *          and the shared block (src/ua101_shared.h), or that the unit isn't there.
 *   start  the helper starts streaming into the block, if it isn't already. The plug-in then
 *          waits for the block's state to be RUNNING, which means the clock is published.
 *   stop   this plug-in no longer needs streaming; the helper stops once no one does. It
 *          carries the plug-in's own counts for the run, for the helper's log.
 * The helper may send a dictionary with UA101_LINK_EVENT at any time, when the unit appears,
 * goes or fails, or when the Mac woke from a sleep that cut a stream; the plug-in then says
 * hello again. Every request has a time limit (src/plugin/ua101_link_xpc.c), so a helper that
 * hangs can't hang Core Audio.
 *
 * Only Core Audio's own user (_coreaudiod) and root may send those three, so no other program
 * can read what the inputs record.
 *
 * Since version 4 the control app (app/) uses the same service, with messages of its own.
 * They may come from root, from the user the helper runs as (the account that installed it),
 * and since 2026-11-23 from every other account a person logs in to (ua101_service_allows in
 * src/ua101_service.h), never from Core Audio's user. None of them gives out audio.
 *   ctl_open   the helper opens the unit's MIDI interface (src/ua101_midiport.h), which it then
 *              holds for as long as a control app is attached, and passes on what the unit
 *              says on the control cable as UA101_LINK_EVENT "ctl" with UA101_LINK_DATA.
 *   ctl_send   one SysEx message for the unit, in UA101_LINK_DATA. The helper classes it
 *              (ua101_ctlmsg_risk) and decides by itself (ua101_service_passes):
 *                the request for the settings block      always passed on
 *                "pause saving", "resume saving"         passed on (since version 18 with no
 *                the patch bay source of an output pair  go-ahead file: see THE OWNER'S
 *                a mixer channel's volume, up to 0 dB    GO-AHEADS below), each only as the
 *                its pan, hard left to right             exact messages and within the values
 *                its on/off, 0 or 1                      tried on the unit
 *                (the mixer's channels: MONITOR OUT L and R, which have no pan, INPUT 1 to 10,
 *                OUTPUT 1 to 10)
 *                its pair's stereo link, a group's solo
 *                Load, Store and Reset (37 messages)     only by the rules of version 17
 *                                                        below, less its go-ahead
 *                                                        (ua101_service_memory_passes)
 *                every other message that sets something never, in this version
 *                Roland's patch bay preset, anything unknown  never
 *              A refusal is answered UA101_LINK_ERR_REFUSED with the reason, and nothing
 *              reaches the unit. The helper isn't built with the code that makes a message
 *              (src/ua101_ctlbuild.c). ctl_open's answer names the changes this helper
 *              passes on (UA101_LINK_CTL_PASSES, _PASSES_ON, _PASSES_MEMORY).
 *   ctl_close this app no longer needs the unit; the interface is given back once none does.
 *   levels     the highest sample of each channel since this app last asked, from the audio
 *              the helper is streaming anyway; "not streaming" when no app plays or records.
 *
 * Since version 12 a MIDI client uses the same service too: the per-user agent
 * (src/ua101midiagent.c), which publishes the unit's MIDI IN and OUT sockets to apps as
 * "UA-101 MIDI". Its messages may come from the same users as the control app's.
 *   midi_open   the helper opens the unit's MIDI interface if it isn't open, holds it while
 *               this client is attached, and passes on what arrives at the MIDI IN socket
 *               (cable 0 only) as UA101_LINK_EVENT "midi" with UA101_LINK_DATA, plain MIDI
 *               bytes, and UA101_LINK_TIME, the Mac's clock when the USB read completed.
 *   midi_send   MIDI bytes for the MIDI OUT socket, in UA101_LINK_DATA: never USB packets,
 *               so a client can't name a cable. The helper packs them for cable 0, drops
 *               SysEx addressed to the UA-101 itself (F0 41 <device> 00 69 ...), and lets
 *               the rest out at the wire's speed from a queue of fixed size
 *               (src/ua101_midigate.h). It needs no answer: send it without waiting for one.
 *   midi_close  this client no longer needs the unit; the interface is given back once no
 *               MIDI client and no control app does.
 * A MIDI client is sent "changed" as a control app is (the unit came, went or was lost, or the
 * Mac woke: ask midi_open again). Nothing a MIDI client sends can reach the control cable:
 * that way stays ctl_send and the helper's rules alone.
 *
 * THE GO-AHEAD FILES (versions 5 to 17; gone since version 18). While the controls were
 * being tried on the unit one by one, the helper passed a change on only while a file that
 * root alone could make existed and was less than a day old
 * (/Library/Application Support/ua101-driver/allow-changes), and a Load, Store or Reset only
 * with a second one less than an hour old (.../allow-memory). Every kind of change has since
 * passed at the unit, and it was chosen on 2026-11-21 to remove both locks. Since version 18
 * the helper looks for neither file: one left there changes nothing, and install/install.sh
 * removes any it finds. What still stands between a control app and the unit is the list of
 * tried messages and values (ua101_service_passes, ua101_service_memory_passes) and the other
 * Load, Store and Reset rules.
 * A control app is also sent "changed" (the unit came, went or was lost, or the Mac woke: ask
 * ctl_open again) and "audio" with UA101_LINK_STREAMING (streaming started or stopped), and
 * while attached, since version 15, "midi_lamps" when MIDI passes the MIDI IN or OUT socket.
 *
 * VERSIONS. The number below is in every request, and a helper answers any other number with
 * UA101_LINK_ERR_VERSION. So the helper, the plug-in and the control app must come from the
 * same build: install/install.sh always installs helper and plug-in together.
 *   3  hello, start, stop (2026-11-10).
 *   4  ctl_open, ctl_send (read request only), ctl_close, levels (2026-11-11). Nothing changed
 *      for the plug-in but the number.
 *   5  the helper classes every message (ua101_ctlmsg_risk); with the user's go-ahead file it
 *      passes on "pause saving" and "resume saving", the first of the first writes
 *     ; ctl_goahead (2026-11-11). Nothing changed for the plug-in
 *      but the number.
 *   6  with the go-ahead the helper also passes on one patch bay change: the source (1 to 11)
 *      of OUTPUT 1-2, the first write that can be seen in the settings block (
 *      6e). The other four outputs wait until that one is proven on the unit: then
 *      UA101_PATCH_OUTPUTS_PASSED widens. Nothing changed for the plug-in but the number
 *      (2026-11-11).
 *   7  with the go-ahead the helper also passes on the patch bay source of all five output
 *      pairs (OUTPUT 1-2's was proven on the unit, 2026-11-11), and the first mixer control:
 *      the volume of INPUT 1 (UA101_VOLUMES_PASSED), and only a value that is at most 0 dB
 *      whichever way the unit reads the six digits of the message, because that order is not
 *      proven yet. Nothing changed for the plug-in but the number
 *      (2026-11-11).
 *   8  the order of a volume's six digits is proven on the unit (2026-11-12: the first digit
 *      sent is the highest, as in the settings block), so with the go-ahead the helper passes
 *      on every volume of INPUT 1 from 0x000000 to 0x200000 (0 dB) read in that order, and
 *      still none above. Still INPUT 1 only; the patch bay and saving rules are unchanged
 *     . The go-ahead file counts for a day only, and ctl_open and
 *      ctl_goahead say when it is there but too old (UA101_LINK_CTL_STALE). Nothing changed
 *      for the plug-in but the number (2026-11-12).
 *   9  with the go-ahead the helper also passes on the pan of INPUT 1 (UA101_PANS_PASSED):
 *      a value from 0x0000 to 0x8000 (the ends of the range, 0x4000 the middle) whichever
 *      way the unit reads the four digits of the message. The order is proven for a volume's
 *      six digits, not for a pan's four, so the rule is version 7's again: 4,100 of the 32,769
 *      values pass (every digit 0 to 7, or one digit 8 and the others 0), until a pan has been
 *      read back from the unit. Every other channel's pan: never. The
 *      volume, patch bay, saving and go-ahead rules are unchanged. The helper's log stamps
 *      carry milliseconds. Nothing changed for the plug-in but the number (2026-11-12).
 *  10  the order of a pan's four digits is proven on the unit (2026-11-12: 0x4000 sent
 *      as 04 00 00 00 read back as 04 00 00 00, the first digit sent the highest, as for a
 *      volume), so with the go-ahead the helper passes on every pan of INPUT 1 from 0x0000 to
 *      0x8000 read in that order (32,769 values), and none above. Still INPUT 1 only; the
 *      volume, patch bay, saving and go-ahead rules are unchanged.
 *      Nothing changed for the plug-in but the number (2026-11-12).
 *  11  with the go-ahead the helper also passes on the third mixer control: the on/off of
 *      INPUT 1 (UA101_ONS_PASSED), and only the two values the block can hold, 0 and 1: the
 *      two 13-byte messages F0 41 10 00 69 12 00 04 00 02 00 7A F7 and ... 01 79 F7. Which
 *      of the two means "muted" is not known (the engine assumes 1); nothing of it has been
 *      sent to the unit yet. The 64 bits of ctl_passes are full, so the
 *      channels go in a second number, ctl_passes_on. Every other channel's on/off: never.
 *      The volume, pan, patch bay, saving and go-ahead rules are unchanged. Nothing changed
 *      for the plug-in but the number (2026-11-12).
 *  12  MIDI clients: midi_open, midi_send, midi_close and the event "midi", for the unit's
 *      MIDI IN and OUT sockets (cable 0) through src/ua101_midigate.h. The MIDI interface is
 *      held while a control app or a MIDI client is attached. Nothing changed for the plug-in
 *      or the control app but the number (2026-11-16).
 *  13  the three mixer controls proven on INPUT 1, for every channel of the mixer (the user's
 *      choice of 2026-11-16, all four groups in one build): with the go-ahead the helper
 *      passes on the volume, up to 0 dB and never above, and the on/off, 0 or 1, of MONITOR
 *      OUT L and R, INPUT 1 to 10 (9 and 10 are OPTICAL IN L and R) and OUTPUT 1 to 10 (the
 *      computer's channels in the monitor mix), and the pan, 0x0000 to 0x8000, of INPUT 1 to
 *      10 and OUTPUT 1 to 10 (MONITOR OUT has none). The messages are INPUT 1's with another
 *      group and channel in the address (docs/control-protocol.md).
 *      Link, solo, Load, Store and Reset: still never. The patch bay, saving and go-ahead
 *      rules are unchanged. Nothing changed for the plug-in or the MIDI agent but the number
 *      (2026-11-16).
 *  14  the last two mixer controls (a design choice of 2026-11-16): with the
 *      go-ahead the helper also passes on the stereo link, 0 or 1, of every pair of the mixer
 *      (UA101_LINKS_PASSED: MONITOR OUT L-R, INPUT 1-2 to 9-10, OUTPUT 1-2 to 9-10), and the
 *      solo of the INPUT group and of the OUTPUT group (UA101_SOLOS_PASSED): one number a
 *      group, 0 for "no solo" or 1 to 10 for the one soloed channel, as read from Roland's own
 *      program on 2026-11-16 (docs/control-protocol.md, "Solo"). Each is exactly one of the
 *      messages written out in src/ua101_service.c (22 for the link, 22 for the solo).
 *      The pairs and groups travel in the upper bits of ctl_passes_on, which a version 13 app
 *      doesn't read. Load, Store and Reset, and Roland's one-message patch bay preset: still
 *      never. The volume, pan, on/off, patch bay, saving and go-ahead rules are unchanged.
 *      Nothing changed for the plug-in or the MIDI agent but the number (2026-11-16).
 *  15  the control app's MIDI IN and MIDI OUT lamps (the user's option B of 2026-11-17): the
 *      helper sends each attached control app the event "midi_lamps" with two bools,
 *      UA101_LINK_MIDI_IN (MIDI arrived at the MIDI IN socket, cable 0) and UA101_LINK_MIDI_OUT
 *      (MIDI really left through the MIDI OUT socket: packets the gate let out and the unit
 *      took). At most 10 a second, only while MIDI passes, and active sensing (0xFE) alone
 *      counts for neither (src/ua101_midilamp.h). No timer runs while MIDI is quiet. The app
 *      asks for nothing and sends nothing for it, and nothing goes to the unit; what may reach
 *      the unit is unchanged. Nothing changed for the plug-in or the MIDI plug-in but the
 *      number (2026-11-17).
 *  16  the driver's own settings (2026-11-19; src/ua101_drvset.h): the plug-in
 *      now chooses the two safety offsets itself, from settings it keeps in Core Audio's
 *      storage, and the ones in a hello's answer are only the values it starts from. Its stop
 *      carries the offsets the run used and the least room recording had; the shared block's
 *      counts grew by the least room playback had and its late frames (the block's version 2).
 *      The helper writes them in its log, in a line of their own after the verdict. Nothing
 *      of it goes to the unit, and what may reach the unit is unchanged. Nothing changed for
 *      the control app's link or the MIDI plug-in but the number (2026-11-19).
 *  17  Load, Store and Reset of the unit's memories (the user's
 *      choices of 2026-11-20). With BOTH of the user's go-aheads the helper passes on exactly
 *      37 messages, each written out byte for byte in src/ua101_service.c: Load Memory 1 to 5
 *      and Store into Memory 1 to 5, each with the patch bay, the mixer or both (30); Reset
 *      of the settings in use (Roland's "Current Setting", address 00 00 00 03, read from
 *      Roland's app and driver 2026-11-20 and 21); Reset of Memory 1 to 5; and Reset of
 *      everything ("All Settings"). Each is judged twice by code that shares nothing (the
 *      parser, and a whole-message comparison). On top of that, every one of these rules
 *      (ua101_service_memory_passes):
 *        - the second go-ahead: a file of its own beside the first (allow-memory), made the
 *          same way, that counts for one hour;
 *        - no sound: never while the helper is streaming;
 *        - a fresh read: the unit must have sent its whole settings block (a control app
 *          asked for it) in the UA101_MEMORY_FRESH_SECONDS before, after the last change and after the last
 *          Load, Store or Reset that was passed on; the helper writes that block into its log
 *          before the command goes out, so a record of the settings from before always exists;
 *        - one at a time: none within UA101_MEMORY_GAP_SECONDS of the one before.
 *      Roland's one-message patch bay preset: still never. ctl_open and ctl_goahead also say
 *      whether the second go-ahead is there (UA101_LINK_CTL_MEMORY, UA101_LINK_CTL_MEMORY_STALE)
 *      and which of the 37 this helper passes on (UA101_LINK_CTL_PASSES_MEMORY, with the flag
 *      UA101_CTL_PASS_MEMORY in ctl_passes). What the ordinary changes may do is unchanged.
 *      Nothing changed for the plug-in or the MIDI plug-in but the number (2026-11-21).
 *  18  both go-ahead locks removed (a design choice of 2026-11-21, after every kind of
 *      change had passed at the unit). The
 *      helper no longer looks for the files allow-changes and allow-memory: the ordinary
 *      changes of ua101_service_passes are passed on as they come, and a Load, Store or Reset
 *      by the remaining rules of version 17 (one of the 37 messages, judged twice; no sound
 *      streaming; a fresh read, written into the log first; one at a time). The lists of
 *      messages and values are unchanged, and nothing else is passed on. The request
 *      ctl_goahead and the answers ctl_writes, ctl_stale, ctl_memory and ctl_memory_stale are
 *      gone; ctl_open still names what this helper passes on. Nothing changed for the plug-in
 *      or the MIDI plug-in but the number (2026-11-21).
 * A later version that adds a control adds it to ua101_service_passes and UA101_CTL_PASS_.
 *
 * The plug-in calls the functions below, implemented with XPC in src/plugin/ua101_link_xpc.c,
 * or by a test without any helper (tests/test_plugin.c).
 */
#ifndef UA101_LINK_H
#define UA101_LINK_H

#include <stddef.h>
#include <stdint.h>

#include "ua101_shared.h"

#define UA101_SERVICE_NAME "io.github.delta90h.ua101d"
#define UA101_LINK_VERSION 18

/* Where the user's two go-ahead files were up to version 17 (see above). No program looks for
   them any more; the control app's report names one that is still there, and
   install/install.sh removes them. */
#define UA101_OLD_GO_AHEAD_PATH "/Library/Application Support/ua101-driver/allow-changes"
#define UA101_OLD_MEMORY_GO_AHEAD_PATH "/Library/Application Support/ua101-driver/allow-memory"
/* A Load, Store or Reset is passed on only if the unit sent its whole settings block at most
   this long before it (and after the last change and the last such command), and never within
   the second time of the one before. */
#define UA101_MEMORY_FRESH_SECONDS 30
#define UA101_MEMORY_GAP_SECONDS 5

/* Message keys. */
#define UA101_LINK_OP "op"           /* string: "hello", "start", "stop"; "ctl_open", "ctl_send",
                                        "ctl_close", "levels"; "midi_open",
                                        "midi_send", "midi_close" */
#define UA101_LINK_EVENT "event"     /* string, from the helper: "changed"; "ctl", "audio",
                                        "midi_lamps"; "midi" */
#define UA101_LINK_KEY_VERSION "version"
#define UA101_LINK_STATUS "status"   /* int64: 0, or what went wrong (UA101_LINK_ERR_...) */
#define UA101_LINK_WHY "why"         /* string, when status isn't 0 */
#define UA101_LINK_PRESENT "present" /* bool */
#define UA101_LINK_RATE "rate"       /* double, Hz */
#define UA101_LINK_IN "in"           /* uint64, capture channels */
#define UA101_LINK_OUT "out"         /* uint64, playback channels */
#define UA101_LINK_INPUT_OFFSET "input_offset"   /* uint64, frames: with hello, the helper's own values; */
#define UA101_LINK_OUTPUT_OFFSET "output_offset" /* with stop (since version 16), what the run used */
#define UA101_LINK_CAPTURE_SPARE "capture_spare" /* int64, with stop, when measured: the least room
                                                    recording had, in frames (src/ua101_drvset.h) */
#define UA101_LINK_SHMEM "shmem"     /* the block */
#define UA101_LINK_MISSING "missing" /* uint64, with stop: capture frames the plug-in found missing */
#define UA101_LINK_CLIPPED "clipped" /* uint64, with stop: playback samples the apps sent at or over full scale */
/* With stop, from a plug-in that measures them; a helper that finds one missing says nothing
   about it, so no link version changes for these. */
#define UA101_LINK_PEAK "peak"         /* double: the largest of the clipped samples, full scale 1.0; 0 if none */
#define UA101_LINK_CUT "cut"           /* uint64, only with rate correction on: samples of the correction's own result that went over full scale and were cut */
#define UA101_LINK_CUT_PEAK "cut_peak" /* double, with "cut": the largest of them, full scale 1.0; 0 if none */
#define UA101_LINK_WATCHING "watching" /* bool, with "not present": an event will come when a unit appears */
#define UA101_LINK_BLOCK "block"     /* uint64: which block it is; a new one each time the unit is opened */

/* The control app's. */
#define UA101_LINK_DATA "data"           /* data: one whole SysEx message, F0 to F7 */
#define UA101_LINK_STREAMING "streaming" /* bool: audio is streaming */
#define UA101_LINK_MIDI_IN "midi_in"   /* bool, with "midi_lamps": MIDI arrived at MIDI IN */
#define UA101_LINK_MIDI_OUT "midi_out" /* bool, with "midi_lamps": MIDI left through MIDI OUT */
#define UA101_LINK_CTL_PASSES "ctl_passes" /* uint64, with ctl_open: UA101_CTL_PASS_ bits, the
                                              changes this helper passes on */
#define UA101_CTL_PASS_SAVING 1u           /* pause and resume the unit's saving */
#define UA101_CTL_PASS_PATCH 2u            /* the patch bay source of some output pairs: */
#define UA101_CTL_PASS_PATCH_OUTPUT(n) (0x100u << (n)) /* of output pair n, 0 to 4 */
#define UA101_CTL_PASS_VOLUME 4u           /* the volume, up to 0 dB, of some channels: */
#define UA101_CTL_PASS_VOLUME_CHANNEL(n) (0x10000ull << (n)) /* of channel n, 0 to 21: 0 and 1
                                              MONITOR OUT L and R, 2 to 11 INPUT 1 to 10, 12 to
                                              21 OUTPUT 1 to 10 (the computer's channels) */
#define UA101_CTL_PASS_PAN 8u              /* the pan of some channels: */
#define UA101_CTL_PASS_PAN_CHANNEL(n) (0x10000000000ull << (n)) /* of channel n, numbered as
                                              for the volume: 2 to 11 INPUT 1 to 10, 12 to 21
                                              OUTPUT 1 to 10 (MONITOR OUT has no pan) */
#define UA101_CTL_PASS_ON 0x10u            /* the on/off of some channels, named in: */
#define UA101_LINK_CTL_PASSES_ON "ctl_passes_on" /* uint64, with ctl_passes: a bit per channel,
                                              numbered as for the volume (bit 2 INPUT 1); since
                                              version 14 also the linked pairs (bits 32 to 42)
                                              and the solo groups (bits 48 and 49) */
#define UA101_CTL_PASS_ON_CHANNEL(n) (1ull << (n))
#define UA101_CTL_PASS_LINK 0x20u          /* the stereo link of some pairs, named in the upper
                                              bits of ctl_passes_on: */
#define UA101_CTL_PASS_LINK_PAIR(n) (0x100000000ull << (n)) /* of pair n, 0 to 10: 0 MONITOR
                                              OUT L-R, 1 to 5 INPUT 1-2 to 9-10, 6 to 10 OUTPUT
                                              1-2 to 9-10 */
#define UA101_CTL_PASS_SOLO 0x40u          /* the solo of some groups, named in ctl_passes_on: */
#define UA101_CTL_PASS_SOLO_GROUP(g) (0x1000000000000ull << ((g) - 1)) /* of group g: 1 INPUT,
                                              2 OUTPUT (MONITOR OUT has no solo) */
#define UA101_CTL_PASS_MEMORY 0x80u        /* some Load, Store and Reset commands, named in: */
#define UA101_LINK_CTL_PASSES_MEMORY "ctl_passes_memory" /* uint64, with ctl_passes: a bit for
                                              each of the commands this helper passes on */
#define UA101_CTL_PASS_MEM_LOAD(m) (1ull << (m))        /* Load Memory m+1, m 0 to 4 (with the
                                                           patch bay, the mixer or both) */
#define UA101_CTL_PASS_MEM_STORE(m) (0x100ull << (m))   /* Store into Memory m+1 (the same) */
#define UA101_CTL_PASS_MEM_RESET(m) (0x10000ull << (m)) /* Reset Memory m+1 */
#define UA101_CTL_PASS_MEM_RESET_CURRENT 0x1000000ull   /* Reset the settings in use */
#define UA101_CTL_PASS_MEM_RESET_ALL 0x2000000ull       /* Reset everything */
/* The Load, Store and Reset commands this helper passes on, a bit each as above: all 17
   (Load and Store of each memory stand for three messages each, so 37 messages). */
#define UA101_MEMORY_PASSED 0x31F1F1Full
/* The output pairs whose patch bay source this helper passes on, a bit each: all five. */
#define UA101_PATCH_OUTPUTS_PASSED 0x1Fu
/* The channels whose volume this helper passes on, a bit each, numbered as above:
   every channel of the mixer (bits 0 to 21). */
#define UA101_VOLUMES_PASSED 0x3FFFFFu
/* The highest volume ever passed on: 0 dB. */
#define UA101_VOLUME_PASSED_MAX 0x200000u
/* The channels whose pan this helper passes on, a bit each, numbered as above:
   INPUT 1 to 10 and OUTPUT 1 to 10 (bits 2 to 21); MONITOR OUT has no pan. */
#define UA101_PANS_PASSED 0x3FFFFCu
/* The ends of a pan: 0x0000 and this (docs/control-protocol.md; 0x4000 is the middle). */
#define UA101_PAN_PASSED_MAX 0x8000u
/* The channels whose on/off this helper passes on, a bit each, numbered as above:
   every channel of the mixer (bits 0 to 21). Only the values 0 and 1. */
#define UA101_ONS_PASSED 0x3FFFFFu
/* The pairs whose stereo link this helper passes on, a bit each: bit 0 MONITOR OUT L-R, 1 to 5
   INPUT 1-2 to 9-10, 6 to 10 OUTPUT 1-2 to 9-10: every pair of the mixer. Only 0 and 1. */
#define UA101_LINKS_PASSED 0x7FFu
/* The groups whose solo this helper passes on: bit 0 INPUT, bit 1 OUTPUT (MONITOR OUT has no
   solo). And the highest solo number ever passed on: channel 10 of the group; 0 is "none". */
#define UA101_SOLOS_PASSED 0x3u
#define UA101_SOLO_PASSED_MAX 10u
#define UA101_CTL_PASSES_ON \
    ((uint64_t)UA101_ONS_PASSED | ((uint64_t)UA101_LINKS_PASSED * 0x100000000ull) | \
     ((uint64_t)UA101_SOLOS_PASSED * 0x1000000000000ull))
#define UA101_CTL_PASSES \
    ((uint64_t)UA101_CTL_PASS_SAVING | UA101_CTL_PASS_PATCH | (UA101_PATCH_OUTPUTS_PASSED * 0x100u) | \
     UA101_CTL_PASS_VOLUME | ((uint64_t)UA101_VOLUMES_PASSED * 0x10000ull) | \
     UA101_CTL_PASS_PAN | ((uint64_t)UA101_PANS_PASSED * 0x10000000000ull) | UA101_CTL_PASS_ON | \
     UA101_CTL_PASS_LINK | UA101_CTL_PASS_SOLO | UA101_CTL_PASS_MEMORY)
#define UA101_CTL_PASSES_MEMORY ((uint64_t)UA101_MEMORY_PASSED)
/* The MIDI client's. UA101_LINK_DATA is then plain MIDI bytes, any number of messages or
   parts of one. */
#define UA101_LINK_TIME "time" /* uint64, with the event "midi": the Mac's clock
                                  (mach_absolute_time) when the USB read completed */
#define UA101_LINK_MIDI_SEND_MAX 65536 /* the most bytes one midi_send may carry */
#define UA101_LINK_CAPTURE_PEAKS "capture_peaks"   /* data, with levels: a uint32 per channel, */
#define UA101_LINK_PLAYBACK_PEAKS "playback_peaks" /* the largest |sample|; 0x80000000 is full scale */
#define UA101_LINK_MAX_CHANNELS 16

enum {
    UA101_LINK_OK = 0,
    UA101_LINK_ERR_VERSION = 1,  /* the two sides were built from different versions */
    UA101_LINK_ERR_ABSENT = 2,   /* the unit isn't connected, or can't be opened */
    UA101_LINK_ERR_START = 3,    /* streaming couldn't start */
    UA101_LINK_ERR_REQUEST = 4,  /* a message it didn't understand */
    UA101_LINK_ERR_NO_HELPER = 5, /* the helper couldn't be reached, or didn't answer in time */
    /* The Mac is going to sleep or waking: the helper has given the unit back and opens nothing
       until it has woken. Nothing is known about the unit, so the plug-in changes nothing. */
    UA101_LINK_ERR_ASLEEP = 6,
    /* ctl_send: the helper doesn't pass this message on (not a message this version passes on
       at all, or a Load, Store or Reset against one of its rules). Nothing was sent to the
       unit. */
    UA101_LINK_ERR_REFUSED = 7,
    /* This user may not ask for this. */
    UA101_LINK_ERR_DENIED = 8
};

/* What the plug-in knows from the last hello. */
typedef struct {
    int present;
    int watching; /* not present, and the helper will say when a unit appears: no need to ask */
    double rate_hz;
    uint32_t capture_channels, playback_channels;
    uint32_t input_offset, output_offset; /* frames: the safety offsets Core Audio is given */
    ua101_shared *shared;                 /* mapped in this process; NULL if not present */
    size_t shared_bytes;
    /* Which block the helper streams into now. When a hello keeps an earlier block mapped,
       this still names the helper's current one, so a plug-in can tell that its block was
       left behind: the helper opened the unit again, or crashed and was started again. */
    uint64_t block_id;
} ua101_link_info;

typedef struct ua101_link ua101_link;

/* Connects, without sending anything yet. on_change is called, on a queue of the link's own,
   when the helper says something changed or the connection is lost. */
ua101_link *ua101_link_open(void (*on_change)(void *context), void *context);

/* Says hello and fills `info`. The block from an earlier hello stays mapped while `keep_mapped`
   is set, for a plug-in that is still using it; otherwise it's replaced. Returns a
   UA101_LINK_ status, with a reason in `why`. */
int ua101_link_hello(ua101_link *l, ua101_link_info *info, int keep_mapped, char *why,
                     size_t why_bytes);

/* Asks for streaming. Returns a UA101_LINK_ status. */
int ua101_link_start(ua101_link *l, char *why, size_t why_bytes);

/* What the plug-in counted in a run. Peaks are sizes where full scale is 1.0, and 0 if nothing
   was over. */
typedef struct {
    uint64_t capture_missing; /* capture frames it wanted that weren't in the ring */
    uint64_t clipped;         /* playback samples the apps sent at or over full scale */
    double clipped_peak;      /* the largest of them */
    int corrected;            /* whether rate correction was on; the next two are its own */
    uint64_t cut;             /* samples of the correction's result that went over and were cut */
    double cut_peak;          /* the largest of them */
    /* Since version 16: the safety offsets the run used, in frames (0 and 0 from a caller
       that doesn't say), and the least room recording had (src/ua101_drvset.h), if measured. */
    uint32_t input_offset, output_offset;
    int capture_spare_known;
    int64_t capture_spare;
} ua101_link_counts;

/* Says streaming is no longer needed, with what the plug-in counted in this run. */
void ua101_link_stop(ua101_link *l, const ua101_link_counts *counts);

/* Disconnects and unmaps the block. */
void ua101_link_close(ua101_link *l);

#endif
