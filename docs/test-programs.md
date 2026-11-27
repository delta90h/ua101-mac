# Developer test programs

The sections below are the test programs used to build the driver. None of them install anything, need `sudo` or change SIP or any other system setting. Most need the box to themselves: quit music apps first, and with the driver installed, stop its helper too (see [building.md](building.md)).

## Stage 0: hardware check

This confirms that your Mac can see and open the UA-101. It sets the UA-101's USB configuration, opens each interface and briefly selects the streaming settings, without sending any audio or MIDI. Turn your monitors down in case the unit clicks.

1. Install Apple's command line tools, if you haven't already:

   ```bash
   xcode-select --install
   ```

2. Turn the rear Hi-Speed USB switch on, power-cycle the UA-101, and connect it directly to the Mac. Quit any audio or MIDI apps.

3. In Terminal, from the top folder of the source, run:

   ```bash
   make test probe && ./build/ua101probe | tee stage0-output.txt
   ```

The last line reads `Stage 0 result: PASS` or `FAIL`. The full output is saved in `stage0-output.txt`. It includes your macOS version and the device's USB details, but not the unit's serial number.

## Stage 1: MIDI

`ua101midi` makes the unit's MIDI IN and OUT appear to other apps as "UA-101 MIDI" while it runs. (The installed driver does this all the time through its MIDI plug-in; this program is the early test of it.) It installs nothing, and the ports disappear when it stops. Quit MIDI apps and `ua101settings` first, because it needs the unit's MIDI interface to itself.

First, check the CoreMIDI side without the unit. This uses private ports, which other apps can't see, and doesn't open the unit:

```bash
make test midi && ./build/ua101midi --self-test
```

Then check both directions through the unit. Connect one MIDI cable from the unit's MIDI OUT to its own MIDI IN, with nothing else connected to either, and run:

```bash
./build/ua101midi --cable-test
```

It takes a few seconds. It sends 8 messages that would do nothing on an instrument: note offs, key pressure 0, a centred pitch bend, clock, song position and a SysEx message marked for non-commercial use. Then it checks that each one comes back unchanged. The last line reads `MIDI cable test: PASS` or `FAIL`.

To hear from a keyboard on MIDI IN, with nothing sent to the unit at all:

```bash
./build/ua101midi --receive-only --show --seconds 60
```

The ports get the same CoreMIDI ID on every run, so music apps remember them.

Without `--receive-only`, anything apps send to "UA-101 MIDI" goes out of MIDI OUT. Some apps send clock or other messages to every new port as soon as it appears, so turn down whatever is connected to MIDI OUT before you start it.

It only ever writes to the MIDI port's cable. Roland's control messages, which change the mixer and can erase stored settings, travel on a separate cable that it never writes. Each transfer is checked for that just before it's sent. The control messages the unit sends when a front-panel switch moves are kept from apps.

## Stage 2: streaming test

This runs capture and playback from a normal program, records all 12 capture channels and reports every USB error it sees. Recordings run at about 127 MB per minute at 44.1 kHz, so keep them out of iCloud Drive. The commands below use `/tmp`, which the Mac clears when it restarts.

Build the tools and run the tests:

```bash
make test stream
```

Run capture only, which never opens the playback interface, so nothing can reach the outputs:

```bash
./build/ua101stream --capture-only --seconds 10 --wav /tmp/ua101/capture.wav
```

Capture keeps 8 transfers of 2 ms queued, which rode through every stall in three overnight tests, where 4 sometimes lost audio. Playback keeps 4, because its queue adds delay. `--capture-queue N` and `--playback-queue N` change them, and `--queue N` sets both.

Run capture and playback, with digital silence on every output:

```bash
./build/ua101stream --seconds 30 --wav /tmp/ua101/duplex.wav
```

Add a test tone on outputs 1 and 2. **Turn your monitors down first.** The tone defaults to −60 dBFS for 1 second, with short fades. The limits are −40 dBFS and 5 seconds, fixed when the program is compiled, and no option goes past them. It isn't played at all if any earlier check in the run has failed.

```bash
./build/ua101stream --seconds 5 --tone-at 2 --wav /tmp/ua101/tone.wav
```

Check a recording:

```bash
./build/ua101wavcheck /tmp/ua101/tone.wav
```

According to the manual's block diagram, the recorded monitor mix is taken before the monitor volume knob, so the knob shouldn't affect these checks. It also doesn't protect OUTPUT 1 and 2, which bypass it.

The bit-exact checks rely on the monitor mix being at unity gain. They should need the MIX knob centered, which the manual says puts inputs and playback both at 100%, and the Soft Control switch off, though only one setting has been tried. The panels and signal flow are described in [docs/hardware.md](hardware.md). If the mix isn't exact, `ua101wavcheck` says so and falls back on level and phase measurements, which are limited by any noise on the inputs.

Keep the front-panel digital input switch out unless an optical source is connected. With it on and no source, the unit stops sending audio, and `ua101stream` says so.

`ua101stream` ends with two results. `Stage 2 streaming result` is `PASS` or `FAIL`, and covers USB errors, lost audio and the recording. `Clock result` says whether the unit's clock is inside the 100 ppm limit, outside it, or `too short to judge`. Under about 4 minutes can be too short, depending on how far off the clock is. The exit status is 0 when both are good, 2 when streaming passes but the clock is outside or not judged, and 1 when streaming fails. `ua101wavcheck` ends with `CLEAN` or `NOT CLEAN`, followed by `tones not checked` when no capture channel recorded the test tones, as at 192 kHz without a cable from an output to an input.

Each direction's report also has a `Timeline` line: where the driver will put each transfer's audio in time, with any gaps and the frames worked out for them. To check that after a stall the audio keeps its place, hold capture up once on purpose. It works with `--capture-only` only, so nothing is sent out:

```bash
./build/ua101stream --capture-only --no-wav --seconds 20 --stall-test 100
```

It holds capture up for 100 ms (up to 500), 10 s in, then reports where the frames after the gap sit against a straight line through the 2 s before it, and ends with `Stall test result`. The result is exact, one frame off, or not in place, and the exit status is 0 if it's within one frame with no other errors. The resync it causes is expected. `--transfer-log FILE` saves every capture transfer's bus frame and frame number as CSV, which is how `tests/data/ua101-48k-capture-transfers.txt` was made.

## Measuring and correcting the clock

The clock of the unit this was developed on isn't exactly on its rate. At 48 kHz it runs about 120 ppm slow, or about 0.43 s per hour, which is outside the limit. At 44.1 and 88.2 kHz it runs about 75 ppm fast, which is inside. To log the clock once per second during a run, add `--clock-log`:

```bash
./build/ua101stream --capture-only --seconds 600 --wav /tmp/ua101/take.wav --clock-log /tmp/ua101/clock.csv
```

To also save the time of each long gap, slow submit and resync, add `--stall-log FILE`.

To make a corrected copy of a recording, give the real rate from a long run's `Device against host` line. Take it with the Mac cool and quiet, because that figure moves by up to 45 ppm when the Mac heats up:

```bash
./build/ua101fixrate /tmp/ua101/take.wav /tmp/ua101/take-fixed.wav 47994.19
```

If the rate you give is right, the copy lasts as long as the recording really took. The tool can't check the rate itself. The original isn't changed, and the tool won't overwrite an existing file. The copy is resampled, so it's no longer bit-exact, and at 48 kHz nothing above about 22.1 kHz is kept.

This only corrects recordings. It doesn't change the unit's clock, so `ua101stream` still reports this unit's 48 kHz clock as outside the limit.

## The cable test

This checks the rate correction against a clock that has nothing to do with the unit: the Mac's own audio output. The Mac plays one quiet tone (−40 dBFS for 5 seconds, left channel only) from its headphone jack into rear INPUT 3 while the unit records. Core Audio's timestamps show how fast the Mac's output really runs. The tone's frequency in the recording must then match that and the unit's measured rate, and after `ua101fixrate` it must match the Mac's output alone.

You need a cable from a 3.5 mm stereo plug to a 1/4-inch stereo (TRS) plug. The tone goes on the left only, because INPUT 3 is balanced: it hears the tip minus the ring, so the same signal on both would cancel out.

1. Turn the UA-101 off. Plug the 3.5 mm end into the Mac's headphone jack and the 1/4-inch end into rear INPUT 3. Turn the unit on, with your monitors low.
2. In System Settings, Sound, choose External Headphones as the output, at about half volume.
3. Run:

   ```bash
   make test stream && bash scripts/cabletest.sh
   ```

It takes about 2 minutes. `ua101macrate` refuses to play unless the output is headphones, so nothing can come out of the speakers, and it never changes the volume. Small results go to a new folder under `~/ua101-runs`, and the recording to `/tmp/ua101`. The last line reads `Cable test: PASS` or `FAIL`. The two tools also work on their own: `./build/ua101macrate` with no options only shows the output device, and `./build/ua101tonerate measure FILE.wav CHANNEL` measures a tone in any recording to a small fraction of a ppm, and says if samples were lost during it.

## The jack map

This measures which jack each channel really is, so the driver and the control app can name and route them. Every output plays a quiet tone of its own at once (−60 dBFS for 3 seconds, a different frequency on each), through ¼-inch TRS loop cables from OUTPUT 1-8 to INPUT 1-8. Before any loop, read the unit's settings (below) and check that the patch bay sends only the Mac's playback to the outputs: on "Hard Thru" a loop would feed back.

Each run is the same command, with its own file name:

```bash
./build/ua101stream --seconds 6 --tone-at 1 --tone-seconds 3 --jack-map --wav /tmp/ua101/jack-straight.wav
```

```bash
./build/ua101jackmap levels /tmp/ua101/jack-straight.wav
```

`levels` shows every output's level on every capture channel, and where each one arrived. Make four runs: `jack-straight` (OUTPUT n to INPUT n, SENS 1 and 2 all the way down), `jack-shifted` (OUTPUT n to INPUT n+1, and OUTPUT 8 to INPUT 1), `jack-sens1` (the straight cables, with SENS 1 turned up to the middle) and `jack-sens2` (SENS 1 back down, SENS 2 up to the middle). Then:

```bash
./build/ua101jackmap solve /tmp/ua101/jack-straight.wav /tmp/ua101/jack-shifted.wav /tmp/ua101/jack-sens1.wav /tmp/ua101/jack-sens2.wav
```

It prints which capture channel each INPUT jack is and which OUTPUT jack each playback channel reaches, marks anything that differs from the manual's map, and ends with `matches the manual's channel map` or `DIFFERS`. It also says when a run wasn't done as described: cables not moved, two cables in the wrong inputs, or a knob that didn't act on one channel. The straight run alone can't show a swap made the same way on both sides; the shifted run and the knobs can. At 192 kHz, `levels` on a straight run shows which 6 jacks work there, and an ordinary `--tone-at` run gives `ua101wavcheck` the outputs back to check.

## Reading the unit's settings

This shows the settings stored in the UA-101: the direct monitor mixer, the patch bay, and the sample rate, Mono and Soft Control switches. It only reads. It sends one fixed request, the same one Roland's driver sends when it starts, and it has no way to change a setting. It can run while audio is streaming, though it needs a moment to open the MIDI interface, so quit any MIDI apps first.

```bash
make settings && ./build/ua101settings
```

To also watch for changes, run this and move a front-panel switch while it listens:

```bash
./build/ua101settings --watch 60
```

The last line reads `OK`, `NO REPLY`, or a warning that some values raise questions, for example if the sample rate doesn't match the USB descriptors. The protocol notes are in [docs/control-protocol.md](control-protocol.md).

A developer's test: `--all-blocks` also asks for the blocks at `02 01 00 00` to `02 0C 00 00`, one at a time, then for the settings block again. `02 03` to `02 07` are the five memories (2026-11-27, see the protocol notes). These are asks as well, and pass the same check; nothing sets anything. It prints each block as it comes and how it differs from the settings in use, stops at the first block that isn't answered, and says whether the settings in use were still the same at the end. It doesn't ask for `02 0D` to `02 0F`: on the UA-101 those got no answer and then stopped its control port until it was switched off and on. With the driver installed, the helper holds the unit, so stop it first and start it again afterwards (both need an administrator's password):

```bash
./build/ua101settings --all-blocks --dry-run
```

```bash
sudo launchctl bootout system/io.github.delta90h.ua101d
```

```bash
./build/ua101settings --all-blocks
```

```bash
sudo launchctl bootstrap system /Library/LaunchDaemons/io.github.delta90h.ua101d.plist
```

`--dry-run` opens nothing and sends nothing: it prints the 17 messages it would send and whether each passes the check.

## Stage 3: the helper's self-test

`ua101d` is the helper the driver uses: it streams with the unit and shares the audio and the clock through one block of memory. Run by hand, it only runs a self-test. A thread in the same program stands in for the plug-in's I/O cycle: it wakes at each period, as Core Audio would, reads capture by sample time and writes **silence** to playback, then checks the timestamps, what was missing and how late each step was. Nothing but digital silence goes to the outputs. Close any program using the unit first (`pgrep -fl 'ua101stream|ua101d'`).

```bash
make helper && ./build/ua101d --self-test 60 --wav /tmp/ua101/helper-duplex-60s.wav
```

`--capture-only` leaves the playback interface closed. `--buffer` sets the period, as an app's buffer size (default 512), `--input-offset` and `--output-offset` how far behind and ahead of sample time the cycle reads and writes (in frames; by default 5 ms and 21.3 ms at the unit's rate, which is 240 and 1024 frames at 48 kHz), and `--stamp-log FILE` saves the USB stack's raw bus timestamps. The report ends with `Self-test result: PASS` or `FAIL` and the reasons. The recording can be checked with `./build/ua101wavcheck`, as in Stage 2.

`ua101d --serve` is how launchd runs the installed helper; don't run it by hand, since it needs launchd to hold its name.

## Stage 3: the plug-in, without installing

`make plugin` builds `build/UA101.driver`, the AudioServerPlugIn, signed ad hoc. The installer puts it in place with the helper. `make test` tests it offline against a fake helper and a fake Core Audio. With the unit, `ua101linktest` runs the helper's service, the plug-in's XPC link and the plug-in in one program, with a thread doing Core Audio's I/O cycle. Playback is digital silence. It streams for the seconds given (default 60), stops, starts again for 10 s, and reports `Streaming: PASS` or `FAIL`, with the clock on its own line against the 100 ppm limit. The plug-in corrects the rate by default, so apps see exactly the nominal rate; the clock line then gives the unit's own rate from the correction's step, and how much the step varies. Add `--no-correction` to run the bit-exact path instead.

```bash
make linktest && ./build/ua101linktest 60
```

Don't load `build/UA101.driver` into a program by hand: macOS checks the unsigned bundle and puts up a prompt, and the program waits for it.
