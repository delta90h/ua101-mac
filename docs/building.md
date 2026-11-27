# Building, testing and installing from source

You need a Mac with Apple silicon, macOS 14 or later, and Apple's command line tools:

```bash
xcode-select --install
```

There are no other dependencies. SIP stays on.

| Command | What it does |
|---|---|
| `make test` | Builds and runs the offline tests (a few minutes). No unit needed. |
| `make app-test` | The control app's self-test. No unit needed. |
| `make app` | The developer app, with a pretend unit. |
| `make user-app` | The end-user app. |
| `make pkg` | `build/UA-101 Driver.pkg`, its read-me, the manual as PDF, and `build/UA-101 Driver.pkg.sha256` (the package's checksum). |
| `make manual` | The help book and the PDF. |
| `make user-pictures` | Redraws the manual's pictures (with a pretend unit). |
| `make halcheck` | A silent check for the installed driver: `build/halcheck 30`. |
| `make tsan` | The shared-memory tests under the thread sanitizer. |

None of these install anything. `make test` uses the address and undefined-behaviour sanitizers.

A release uploads `UA-101 Driver.pkg`, `UA-101 Driver.pkg.sha256` and the manual PDF, and its notes end with the line `SHA-256 of UA-101 Driver.pkg: ` and the number `make pkg` printed. Users check their download against it (see the README's Install section).

`make pkg` is unsigned by default; to sign and notarize (never tried), see the top of `install/pkg/build-pkg.sh`. The build refuses to package a file that names your home folder.

## Install what you built

Quit apps that use sound, then open `build/UA-101 Driver.pkg`, or use the developer script, which prints every step:

```bash
install/install.sh --dry-run
```

```bash
install/install.sh
```

| Installed | Where |
|---|---|
| Helper | `/Library/Application Support/ua101-driver/ua101d` |
| Its launchd file | `/Library/LaunchDaemons/io.github.delta90h.ua101d.plist` |
| Audio plug-in | `/Library/Audio/Plug-Ins/HAL/UA101.driver` |
| MIDI plug-in | `/Library/Audio/MIDI Drivers/UA101MIDI.plugin` |
| Helper log | `/Library/Logs/ua101d.log` |

Core Audio and the MIDI service restart at the end.

## Remove

```bash
install/uninstall.sh
```

It asks whether to keep your settings. `--dry-run` shows the steps only.

## When something doesn't work

```bash
log show --last 5m --predicate 'subsystem BEGINSWITH "io.github.delta90h"'
```

```bash
tail -50 /Library/Logs/ua101d.log
```

In the app, Help → Save Report for Developer… collects these in one file. Read it before you share it.

## Testing with a real unit

- **Turn monitors down and take headphones off.**
- Don't load `build/UA101.driver` into a program by hand: macOS asks about the unsigned bundle and the program waits.
- A debugger or `sample` on the helper while it streams loses audio.
- Quit music apps before using the programs in [test-programs.md](test-programs.md).
- Before trying control messages, make a backup in the app.
