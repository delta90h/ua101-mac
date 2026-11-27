# UA-101 control protocol

How a computer reads and changes the UA-101's direct monitor mixer, patch bay and memories. It comes from studying how Roland's Mac software (v1.0.1) talks to the unit, and from tests on one real unit. It is written in our own words; no Roland code or files are in this project.

**Confirmed** means tried on that unit, mostly at 48 kHz. **Inferred** means worked out, not shown.

## How it travels

- USB-MIDI **cable 1** on interface 2, alternate setting 0: bulk OUT `0x04`, bulk IN `0x83` (confirmed). Ordinary MIDI uses cable 0. **Never pass cable 1 to MIDI apps, or app data to it.**
- Standard USB-MIDI SysEx framing: CIN 4 for each 3 bytes, then CIN 5, 6 or 7 for the last 1, 2 or 3.
- Settings can be read while audio streams.

## Message format

```
F0 41 10 00 69 <cmd> <a0 a1 a2 a3> <payload> <sum> F7
```

- `41` is Roland, `10` the device ID, `00 69` the model.
- `<cmd>`: `11` requests data, `12` sets it.
- `<sum>`: `-(sum of address and payload bytes) & 0x7F`.

## Reading the settings (confirmed)

```
F0 41 10 00 69 11 02 00 00 00 00 00 02 00 7C F7
```

As USB-MIDI packets on cable 1:

```
14 F0 41 10   14 00 69 11   14 02 00 00   14 00 00 00   14 02 00 7C   15 F7 00 00
```

The reply is a 268-byte `12` message at address `02 00 00 00`, holding the 256-byte block below (360 bytes of USB-MIDI packets). `ua101settings` sends this request and prints the result.

## Change messages

When a front-panel switch moves, the unit sends one byte at `00 01 00 <n>`:

| `n` | Value |
|---|---|
| `02` | Clock source and status, as `d[5]`: `09` with DIGITAL IN in and no source, `00` with it out |
| `03` | Unknown (`d[6]` bit 0), never seen |
| `04` | MONO: `01` on, `00` off (confirmed) |
| `05` | SOFT CTRL: `01` on, `00` off (confirmed) |

They arrive in bursts (up to 7 messages in 377 ms for one release), and old ones can arrive when a program opens the interface. **Wait for about 500 ms of quiet, then read the block.**

## Settings block

`d[n]` is byte `n` of the 256. Multi-byte values hold one nibble per byte, most significant first.

| Bytes | Meaning |
|---|---|
| `d[0]` to `d[3]` | Section flags: switches, patch row 0, patch rows 1-2, mixer |
| `d[4]` | Bits 0-2, sample rate: 0 = 44.1, 1 = 48 (confirmed), 4 = 192 kHz; 2 and 3 are probably 88.2 and 96 (inferred). Bit 3 unknown |
| `d[5]` | Bits 0-2, clock source: 0 internal, 1 digital input. Bit 3, clock status: 1 with no source |
| `d[6]` | Bit 0 unknown, bit 1 MONO, bit 2 SOFT CTRL (confirmed) |
| `d[7]` to `d[21]` | Patch bay, 3 rows of 5. The UA-101 uses row 1, `d[12]` to `d[16]` |
| `d[22]`, `d[23]` | Solo of INPUT and OUTPUT: 0, or the one soloed channel 1 to 10 |
| `d[24]` to `d[28]` | Stereo link: MONITOR OUT `0x18`, INPUT `0x19`, OUTPUT `0x1B`. Pair `p`: byte `base + p/4`, bit `p & 3` |
| `d[29]` to `d[35]` | On/off: MONITOR OUT `0x1D`, INPUT `0x1E`, OUTPUT `0x21`. Channel `c`: byte `base + c/4`, bit `c & 3`. **1 mutes** (confirmed) |
| `d[36]` to `d[75]` | INPUT pan, 4 nibbles a channel |
| `d[76]` to `d[115]` | OUTPUT pan, 4 nibbles a channel |
| `d[116]` to `d[127]` | MONITOR OUT volume, 6 nibbles a channel |
| `d[128]` to `d[187]` | INPUT volume, 6 nibbles a channel |
| `d[188]` to `d[247]` | OUTPUT volume, 6 nibbles a channel |
| `d[248]` to `d[255]` | Not used |

**Groups:** 0 MONITOR OUT (2 channels), 1 INPUT (10; 9-10 are OPTICAL IN), 2 OUTPUT from the computer (10).

**Volume:** gain is `value / 0x200000`: `0x200000` is 0 dB, `0` silence, `0x7FFFFF` about +12 dB. The helper passes on the whole range up to `0x7FFFFF` (since link version 19, 2026-11-27; before, never more than 0 dB).
**Pan:** `0x0000` left, `0x4000` centre, `0x8000` right (confirmed). MONITOR OUT has none.

## Changing settings (confirmed)

All use `12`. The unit doesn't answer; read the block back to check.

| Control | Address | Payload |
|---|---|---|
| Volume | `00 <g+3> <ch> 07` | 6 nibbles, most significant first |
| Pan | `00 <g+3> <ch> 03` | 4 nibbles, most significant first |
| On/off | `00 <g+3> <ch> 02` | `01` mutes, `00` sounds |
| Stereo link | `00 <g+3> <pair*2> 01` | `01` linked, `00` not |
| Solo (INPUT, OUTPUT) | `00 <g+3> 00 00` | `00` none, or `01` to `0A` |
| Patch bay source | `00 02 01 <output pair 0-4>` | see below |
| Pause saving | `00 00 00 00` | `00` |
| Resume saving | `00 00 00 00` | `01` |

| Patch bay source | Value |
|---|---|
| MONITOR OUT | 1 |
| INPUT 1-2 to 9-10 | 2 to 6 |
| WAVE OUT 1-2 to 9-10 | 7 to 11 |

- **Saving:** pause it when a drag starts, resume it when it ends, as Roland's app does. The unit then probably saves once instead of on every step (inferred). 84 messages in one pause were all taken.
- **Link:** setting it changes neither channel. The program must send the same volume and on/off to both channels of a pair; the pan is not shared.
- **Solo:** silences the rest of its own group only. The other group stays audible.
- **SOFT CTRL out:** the unit bypasses its mixer. Roland's app and this one send no mixer change then.
- **Presets:** Hard Thru is sources 2 to 6 and Initial Setting 7 to 11, sent as ordinary patch bay changes. **Hard Thru can cause feedback.**
- **192 kHz:** channels 7 and up don't exist; don't send to them.

## Commands that change or erase stored settings

**These overwrite memories and can't be undone without a backup.**

| Action | Address | Payload |
|---|---|---|
| Reset everything, memories too | `00 00 00 02` | `00` |
| Reset the settings in use | `00 00 00 03` | `00` |
| Reset Memory 1 to 5 | `00 00 <3 to 7> 03` | `00` |
| Store into Memory 1 to 5 | `00 00 <3 to 7> 04` | `00 00 <patch bay 0/2> <mixer 0/2>` |
| Load Memory 1 to 5 | `00 00 <3 to 7> 08` | `00 00 <patch bay 0/1> <mixer 0/1>` |

Confirmed:
- After a Load or Reset the unit sends its whole block by itself, about 20 ms later. After a Store it sends nothing.
- Store and Load keep strictly to the part named: patch bay, mixer or both.
- A Load is taken with SOFT CTRL out too. Store and Reset were not tried that way.
- Settings and memories survive a power-off.
- A memory can't be read directly: only by loading it.

## Not safe to try

Messages nothing shows to be safe: requests at `02 xx 00 00` (`xx` not 0) and `01 00 00 00`; sets at `00 00 00 01`, `00 01 00 00`, and the one-message patch bay presets at `00 00 00 08`.

## Examples

| Action | Message |
|---|---|
| INPUT 1 volume to 0 dB | `F0 41 10 00 69 12 00 04 00 07 02 00 00 00 00 00 73 F7` |
| OUTPUT 1 pan to centre | `F0 41 10 00 69 12 00 05 00 03 04 00 00 00 74 F7` |
| INPUT 3 solo | `F0 41 10 00 69 12 00 04 00 00 03 79 F7` |
| INPUT 1-2 linked | `F0 41 10 00 69 12 00 04 00 01 01 7A F7` |
| OUTPUT 1-2 plays WAVE OUT 1-2 | `F0 41 10 00 69 12 00 02 01 00 07 76 F7` |

`src/ua101_ctlmsg.c` and `src/ua101_ctlbuild.c` read and build these; `tests/test_ctlengine.c` and `tests/test_ctlhelper.c` check them.

## Not part of this protocol

Level meters (worked out from the audio) and the driver's delays (Core Audio settings, `src/ua101_drvset.h`). Nothing of them goes to the unit.
