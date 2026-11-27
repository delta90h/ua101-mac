# How it fits together

```
 music apps ── Core Audio (coreaudiod) ── UA101.driver ──┐ XPC + shared memory
                                                         │
 MIDI apps ─── MIDIServer ─── UA101MIDI.plugin ──────────┼── ua101d (helper) ── USB ── UA-101
                                                         │
 UA-101 Control (app) ───────────────────────────────────┘ XPC
```

Only the helper talks USB. Everything else asks it.

| Part | Where | What it does |
|---|---|---|
| Helper `ua101d` | `src/ua101d.c`, `ua101_engine.c`, `ua101_service.c`, `ua101_midiport.c` | Started by launchd when asked for. Streams with the unit, keeps the clock, decides which control messages may reach the unit. Runs as the installing user, not root. |
| Audio plug-in | `src/plugin/` | AudioServerPlugIn inside coreaudiod: the "UA-101" sound device, with rate correction. |
| MIDI plug-in | `src/midiplugin/` | CoreMIDI driver: the port "UA-101 MIDI". |
| Control app | `app/` (Swift) on `src/ua101_ctlengine.c` | Mixer, patch bay, memories, backups, Driver Settings. |
| Uninstaller, package | `install/` | `uninstall.sh`, and `pkg/` for the installer. |

## Things to know before changing code

- **Shared memory** (`ua101_shared.h`): one block with the clock and two rings between helper and audio plug-in. Change its layout, change its version.
- **Link version** (`UA101_LINK_VERSION` in `ua101_link.h`): in every request; the helper refuses any other number. Raise it when what is said between the parts changes. Helper, plug-ins and app must come from one build.
- **The helper is the gate.** It passes on only messages it lists, byte for byte (`ua101_service_passes`, `ua101_service_memory_passes`): never a volume above the top of the unit's range (+12 dB; up to link version 18, 0 dB); Load, Store and Reset never while sound streams and only after a fresh read. The app has checks of its own, but the helper doesn't trust them.
- **MIDI apps can't reach the control cable.** MIDI goes to cable 0 only, and SysEx addressed to the UA-101 is dropped.
- **Clock:** the unit sets the pace (implicit feedback). Its clock was measured about 120 ppm slow at 48 kHz on the unit tested; the plug-in corrects this so apps see the nominal rate, adding about 0.6 ms each way.
- **Channels:** 12 in and 10 out up to 96 kHz, 6 and 6 at 192 kHz. The rate is the knob on the unit. Full-Speed USB is not supported.
- **Two app builds:** developer (`make app`, with a pretend unit) and end-user (`make user-app`, what the package holds). The switch is `UA101_USER`, in `app/Build.swift`.

More: [hardware.md](hardware.md), [control-protocol.md](control-protocol.md), [building.md](building.md), [test-programs.md](test-programs.md).
