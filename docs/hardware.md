# UA-101 hardware reference

What the unit physically is, taken from the user's manual (UA-101 Owner's Manual, 64 pages, not included here). Page numbers are in brackets. "Measured" marks anything found by this project, not stated in the manual.

## Front panel, left to right

Item numbers are the manual's (p. 9-12).

| # | Label | Type | What it does |
|---|---|---|---|
| 2 | SENS 1 | Knob | Input sensitivity for INPUT 1 |
| 1 | INPUT 1, INPUT 2 | XLR/TRS combo jacks | Mic preamp inputs. XLR: −50 to −10 dBu. TRS: −36 to +4 dBu |
| 2 | SENS 2 | Knob | Input sensitivity for INPUT 2 |
| 3 | Hi-Z | Push switch | In: high impedance for a guitar or bass. INPUT 2's TRS socket only. INPUT 1 is fixed at low impedance |
| 4 | LIMITER | Push switch | One hardware limiter switch for both combo inputs, before the A/D converter |
| 5 | LEVEL | 5 LEDs | Combo input level (red at −6 dB). After a time with no input, monitor output level before the monitor volume (red at 0 dB). All blink to confirm a Factory Reset (p. 45) |
| 8 | IN 9-10 | Optical connector | Digital input, IEC60958. Not usable at 192 kHz |
| 9 | OUT 9-10 | Optical connector | Digital output, IEC60958. Not usable at 192 kHz |
| 6 | SYNC | LED, beside DIGITAL IN | With DIGITAL IN on: lit green is locked, blinking is not locked (p. 10, 30) |
| 7 | DIGITAL IN | Push switch | In: the unit follows the clock at IN 9-10. Measured: with no source, the unit stops its audio |
| 10 | SAMPLE RATE | Rotary switch | As drawn, the positions run counter-clockwise from 44.1 kHz at the upper left, through 48, 88.2 and 96, to 192 kHz at the bottom. Change it only with software closed and the power off (p. 11), except during a Factory Reset |
| 11 | MONO | Push switch | In: inputs are monitored in mono. USB recording stays stereo |
| 12 | SOFT CTRL | Push switch | In: the Control Panel or ASIO software controls the mixer. Out: the mixer's settings are bypassed |
| 13 | MIX | Knob | Direct monitor balance. Center: inputs and computer playback both at 100%. Left lowers the inputs, right lowers the playback (p. 12) |
| 14 | OUTPUT/POWER | Knob | **One knob for both monitor volume and power.** Fully left is STANDBY, so the unit always powers on near minimum volume. Sets PHONES and MONITOR OUT only |
| 15 | (beside OUTPUT/POWER) | LED | Lit when the power is on |
| 16 | PHONES | 1/4-inch stereo jack | The monitor mix. MONITOR OUT stays active with headphones plugged in |
| 17 | USB | LED | Red: Hi-Speed. Green: Full-Speed. Blinking: not recognized (p. 12, 49) |

## Rear panel, left to right as you face it

Item numbers are the manual's (p. 13-15). **The jack numbers run backwards from this side: 8 is on the left.**

| # | Label | Type | What it does |
|---|---|---|---|
| 18 | Ground symbol | Screw terminal | Optional ground. Never use water pipes, gas pipes, or telephone or lightning grounds |
| 19 | DC IN | Power jack | DC 9 V, 1 A, from the supplied adaptor only. The label names the Roland PSB-1U |
| 20 | PHANTOM +48 | Slide switch | Left: on. Right: off. DC 48 V, 20 mA maximum, to both XLR sockets only |
| 21 | HI-SPEED | DIP switch, first of four | Up: Hi-Speed, 10 in and 10 out. Down: Full-Speed, 2 in and 2 out, 44.1 and 48 kHz only. Needs a power cycle |
| 22 | INPUT LEVEL 7-8, 5-6, 3-4 | DIP switches, in that order | Up: +4 dBu nominal, +16 dBu maximum. Down: −10 dBu nominal, +9 dBu maximum |
| 23 | MIDI OUT, MIDI IN | 5-pin DIN | OUT is on the left |
| 24 | USB symbol | USB type B | To the computer |
| 25 | OUTPUT 8 to 1 | Top row, 1/4-inch TRS | Balanced, +4 dBu nominal |
| 26 | INPUT 8 to 3 | Bottom row, 1/4-inch TRS | Balanced |
| 27 | MONITOR OUT R, L | Bottom row, far right, 1/4-inch TRS | Balanced. The monitor mix after the OUTPUT/POWER knob |

## Signal flow

From the block diagrams on the cover and on p. 37.

```text
INPUT 1-2  phantom (XLR) > SENS > Hi-Z (INPUT 2, TRS) > LIMITER > A/D > LEVEL LEDs --+
INPUT 3-8  ATT (+4/-10 switch) > A/D -------------------------------------------------+--> USB IN 1-10
OPTICAL IN --------------------------------------------------------------------------+

USB IN 1-10 sources   > INPUT monitor mixer  > MONO/STEREO > MIX knob --+
USB OUT 1-10          > OUTPUT monitor mixer > ........... > MIX knob --+--> sum (monitor mix)

monitor mix --> USB IN 11-12
            --> patch bay, as a source
            --> LEVEL LEDs > OUTPUT/POWER knob > D/A > PHONES and MONITOR OUT

USB OUT 1-10, inputs 1-10, monitor mix --> PATCH BAY --> D/A > OUTPUT 1-8
                                                     --> OPTICAL OUT 9-10
```

- **The recorded monitor mix is taken after the MIX knob and before the OUTPUT/POWER knob.** So the volume knob can't affect channels 11-12, and the MIX knob can. The manual's statement that MIX doesn't affect the recording level (p. 29) fits channels 1-10.
- **Soft Control switches the two mixers in or out.** Out, the mixer settings are bypassed and everything passes as it is (p. 11). Measured: with it out and MIX centered, channels 11-12 are the exact sum of the inputs and playback.
- **The patch bay chooses what each output pair sends:** any of the five computer pairs, inputs 1-10, or the monitor mix (p. 40).
- **There are five D/A pairs:** monitor, and outputs 1-2, 3-4, 5-6 and 7-8.

## USB channels

| Channels | Capture | Playback |
|---|---|---|
| 1-2 | INPUT 1-2 | OUTPUT 1-2, through the patch bay |
| 3-8 | INPUT 3-8 | OUTPUT 3-8, through the patch bay |
| 9-10 | OPTICAL IN | OPTICAL OUT |
| 11-12 | Monitor mix ("UA-101 Mon", p. 22, 42) | None |

The specifications (p. 60) count 10 record channels. The driver pages (p. 22-23) count 12, including the monitor mix, which matches the USB descriptors measured at 44.1 and 48 kHz.

## Sample rates

| Rate | Channels in the specifications (p. 60) | Digital in and out | Jacks 7/8 | Frequency response, ±1 dB |
|---|---|---|---|---|
| 44.1 kHz | 10 in, 10 out | Yes | Yes | 20 Hz to 20 kHz |
| 48 kHz | 10 in, 10 out | Yes | Yes | 20 Hz to 22 kHz |
| 88.2 kHz | 10 in, 10 out | Yes | Yes | 20 Hz to 40 kHz |
| 96 kHz | 10 in, 10 out | Yes | Yes | 20 Hz to 40 kHz |
| 192 kHz | 6 in, 6 out | No | No | Not listed |

Whether the monitor mix is still recorded at 192 kHz isn't stated, and has to be read from the USB descriptors at that rate.

## Control Panel and stored settings

- **Device panel:** Roland's app shows the rate, the clock, and the Mono and Soft states (p. 39-40). The manual's pictures show "48 kHz" and "CLOCK INTERNAL CLK", which fits the unit reading clock source 0 with DIGITAL IN out (measured).
- **Mixer strips:** each has signal, on/off, solo, pan, volume and link (p. 40).
- **Memories:** up to five sets of settings are stored in the unit (p. 40). A memory can only be read by loading it.
- **Factory Reset:** restores the mixer and patch bay and erases the memories (p. 45).

## Handling rules in the manual

- Turn the volume down and the power off on all equipment before connecting anything (p. 27).
- Turn connected monitors down before powering on. Some sound at power-on is normal (p. 19).
- Close all software that uses the unit before powering it off (p. 48).
- Leave phantom power off unless a condenser mic needs it on an XLR socket (p. 4, 13).
- Don't connect anything to inputs you aren't using (p. 28).
- Use the supplied AC adaptor and USB cable, connected directly to the computer, not through a hub (p. 13, 15, 54).
- Keep the unit away from power amplifiers, large transformers and phones in use, and on its own power outlet away from motors and inverters (p. 5).
- Never open the unit (p. 3).

## Specifications

| Item | Value |
|---|---|
| Conversion | 24-bit linear |
| S/N, typical | Output 107 dB, input 101 dB (p. 60) |
| Residual noise, typical | −97 dBu at OUTPUT 1-2, IHF-A, balanced (p. 60) |
| Power | DC 9 V, 1 A |
| Size | 218.0 × 159.1 × 45.1 mm |
| Weight | 1.15 kg |

## Slips and dated statements in the manual

- Item 8 on p. 11 refers to the "sample rate select switch (18)". That switch is item 10.
- TRS input range is −36 to +4 dBu on p. 9, and −35 to +4 dBu on p. 60.
- The imperial weight on p. 61 reads "1 oz", which can't be right for 1.15 kg.
- "Hi-Speed USB mode can be used only with Windows XP" (p. 14) is out of date. This project has streamed at Hi-Speed on macOS 27 (measured).
