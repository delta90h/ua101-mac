UA-101 Driver 1.1.

For Macs with Apple silicon (M1 and later), macOS 14 Sonoma or later. Tested on macOS 27.2 on an M1 MacBook Pro.

- A new look: each part of the mixer has its own metal plate, and the faders, knobs and keys look like real ones. The ON key is now MUTE: lit red when the channel is muted.
- Faders and the ↑ key go up to +12 dB, with a stop at 0 dB. A fader: let go and push again. The ↑ key: a held key stays at 0 dB, a fresh press goes on.
- Double-click a fader for 0 dB, a pan knob for the middle.
- A notes pad for what is wired where: View → Show Notes (⌥⌘N).
- Recall Memory shows what the memory would change, before anything changes.
- New driver defaults: playback 8 ms and recording 8 ms (were 21.3 ms and 5 ms). Playback at 8 ms needs the music app's buffer at 128 frames or more at 48 kHz; with a smaller buffer, choose 10 ms or more in Driver Settings. Values you chose yourself stay. Rate correction stays on.
- The manual has a new page, "What's new in 1.1", in the app's Help menu and as `UA-101 Control Manual.pdf` here.

Install: double-click `UA-101 Driver.pkg`, over 1.0 or on a clean Mac. It isn't signed through Apple's paid developer program, so the first time macOS won't open it: click Done, then System Settings → Privacy & Security → Open Anyway (see the README).
