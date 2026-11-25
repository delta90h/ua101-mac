#!/bin/sh
# Installs the UA-101 driver. Run it yourself from Terminal, in the ua101-driver folder:
#
#     install/install.sh            install (asks you to type yes, then your Mac password)
#     install/install.sh --dry-run  show every step and change nothing
#
# It builds the helper, the plug-in, the MIDI plug-in and the MIDI agent fresh, as you, then
# copies them into place with sudo:
#
#     /Library/Application Support/ua101-driver/ua101d          the helper that talks to the unit
#     /Library/LaunchDaemons/io.github.delta90h.ua101d.plist    starts the helper when needed
#     /Library/Audio/Plug-Ins/HAL/UA101.driver                  the plug-in Core Audio loads
#     /Library/Logs/ua101d.log                                  the helper's messages
#     /Library/Audio/MIDI Drivers/UA101MIDI.plugin              the MIDI plug-in your MIDI server
#                                                               loads: the device "UA-101" with its
#                                                               port "UA-101 MIDI" (option B)
#     /Library/Application Support/ua101-driver/ua101midiagent  the MIDI agent of option (A), kept,
#                                                               not started
#
# An earlier MIDI agent is stopped and its login file
# (/Library/LaunchAgents/io.github.delta90h.ua101midi.plist) removed: the plug-in replaced it
# (2026-11-17). Your MIDI server restarts at the end, which stops MIDI in every app for a moment.
# macOS may show a notice that a background item was added: that is the helper, as expected.
#
# It also removes the two go-ahead files of earlier versions, if one is still there
# (/Library/Application Support/ua101-driver/allow-changes and .../allow-memory). Since link
# version 18 (2026-11-21) the helper looks for neither: changes need no file any more.
#
# and restarts Core Audio, which stops all sound on the Mac for a moment. SIP and the security
# settings are left alone. install/uninstall.sh takes it all out again. See INSTALL.md.

set -eu

LABEL=io.github.delta90h.ua101d
HELPER_DIR="/Library/Application Support/ua101-driver"
HELPER="$HELPER_DIR/ua101d"
DAEMON_PLIST="/Library/LaunchDaemons/$LABEL.plist"
PLUGIN="/Library/Audio/Plug-Ins/HAL/UA101.driver"
LOG="/Library/Logs/ua101d.log"
AGENT_LABEL=io.github.delta90h.ua101midi
AGENT="$HELPER_DIR/ua101midiagent"
AGENT_PLIST="/Library/LaunchAgents/$AGENT_LABEL.plist"
MIDI_DRIVERS="/Library/Audio/MIDI Drivers"
MIDI_PLUGIN="$MIDI_DRIVERS/UA101MIDI.plugin"

dry=0
case "${1-}" in
    --dry-run) dry=1 ;;
    "") ;;
    *) echo "Usage: install/install.sh [--dry-run]" >&2; exit 2 ;;
esac

cd "$(dirname "$0")/.."

say() { printf '%s\n' "$*"; }
fail() { printf '\nStopped: %s\n' "$*" >&2; exit 1; }

# Every change goes through here: shown first, then run with sudo. A dry run only shows it.
step() {
    line="sudo"
    for a in "$@"; do
        case "$a" in
            *" "*) line="$line \"$a\"" ;;
            *) line="$line $a" ;;
        esac
    done
    printf '   %s\n' "$line"
    if [ "$dry" = 0 ]; then
        sudo "$@"
    fi
}

# The MIDI agent belongs to your own session, so its start and stop run as you: shown first,
# then run without sudo. A dry run only shows it.
mystep() {
    line="  "
    for a in "$@"; do
        case "$a" in
            *" "*) line="$line \"$a\"" ;;
            *) line="$line $a" ;;
        esac
    done
    printf '   %s\n' "$line (as you, without sudo)"
    if [ "$dry" = 0 ]; then
        "$@"
    fi
}

me=$(id -un)
uid=$(id -u)
[ "$(id -u)" != 0 ] || fail "run it without sudo. It asks for your password itself."
[ "$(uname -m)" = arm64 ] || fail "this is for Macs with Apple silicon."
busy=$(pgrep -fl 'ua101stream|ua101linktest|trials\.sh|ua101d --self-test' || true)
[ -z "$busy" ] || fail "a test program is using the unit. Let it finish first: $busy"

if [ "$dry" = 1 ]; then
    say "Dry run: these are the steps. Nothing is built, copied or restarted."
    say ""
    say "1. It would build the helper, the plug-in, the MIDI plug-in and the MIDI agent fresh, as $me:"
    say "   make helper plugin midiplugin agent"
    [ -x build/ua101d ] && [ -d build/UA101.driver ] && [ -x build/ua101midiagent ] &&
        [ -d build/UA101MIDI.plugin ] ||
        fail "there's no build to check yet. Run make helper plugin midiplugin agent first."
else
    say "1. Building the helper, the plug-in, the MIDI plug-in and the MIDI agent fresh, as $me."
    # A fresh bundle folder: the old one keeps whatever macOS marked on it, such as the
    # quarantine flag it set on the test copy on 2026-11-09.
    rm -rf build/UA101.driver build/ua101d build/ua101midiagent build/UA101MIDI.plugin build/UA101MIDI
    make helper plugin midiplugin agent >/dev/null ||
        fail "the build failed. Run make helper plugin midiplugin agent to see why."
fi

say "2. Checking what will be installed."
codesign --verify --strict build/UA101.driver 2>/dev/null ||
    fail "the plug-in's signature doesn't check out."
codesign --verify build/ua101d 2>/dev/null || fail "the helper's signature doesn't check out."
codesign --verify build/ua101midiagent 2>/dev/null ||
    fail "the MIDI agent's signature doesn't check out."
# Not strict: iCloud puts Finder information on the bundle folder in the project, which the copy
# below leaves out.
codesign --verify build/UA101MIDI.plugin 2>/dev/null ||
    fail "the MIDI plug-in's signature doesn't check out."
[ "$(nm -gU build/UA101MIDI.plugin/Contents/MacOS/UA101MIDI | awk '{print $3}')" = _UA101MIDI_Create ] ||
    fail "the MIDI plug-in exports more than its factory."
! otool -L build/UA101MIDI.plugin/Contents/MacOS/UA101MIDI | grep -q IOKit ||
    fail "the MIDI plug-in links IOKit: it must have no way to reach the unit by itself."
/usr/libexec/PlistBuddy -c 'Print :AudioServerPlugIn_MachServices' \
    build/UA101.driver/Contents/Info.plist 2>/dev/null | grep -q "$LABEL" ||
    fail "the plug-in doesn't list the helper, so Core Audio wouldn't let it reach it."
tmp=$(mktemp -d "${TMPDIR:-/tmp}/ua101-install.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
cp "install/$LABEL.plist" "$tmp/$LABEL.plist"
# The helper runs as you: that is how every test with the unit ran it, and it needs no more.
/usr/libexec/PlistBuddy -c "Add :UserName string $me" "$tmp/$LABEL.plist" >/dev/null
plutil -lint -s "$tmp/$LABEL.plist" || fail "the helper's launchd file doesn't read."
program=$(/usr/libexec/PlistBuddy -c 'Print :ProgramArguments:0' "$tmp/$LABEL.plist")
[ "$program" = "$HELPER" ] || fail "the launchd file starts $program, not $HELPER."
if [ -e "$PLUGIN" ] || [ -e "$DAEMON_PLIST" ] || [ -e "$HELPER" ]; then
    say "   An earlier copy is installed. It will be replaced."
fi
say "   All good: the four signatures check, the helper will run as $me, and the MIDI plug-in"
say "   reaches the unit only through the helper."

say "3. Installing. These run with sudo, so macOS asks for your Mac password once."
if [ "$dry" = 0 ]; then
    printf '   All sound and MIDI on this Mac stop for a moment at the end. Type yes to go on: '
    read -r answer
    [ "$answer" = yes ] || fail "you didn't type yes. Nothing was changed."
    sudo -v || fail "sudo didn't take the password. Nothing was changed."
fi
# An earlier MIDI agent stops first: its ports go, and it lets go of the helper. The plug-in
# replaces it, so it isn't started again.
if [ "$dry" = 1 ] || launchctl print "gui/$uid/$AGENT_LABEL" >/dev/null 2>&1; then
    [ "$dry" = 0 ] || say "   (this one only if an earlier MIDI agent is loaded)"
    mystep launchctl bootout "gui/$uid/$AGENT_LABEL" || say "   (it was already stopping)"
    # launchd may take a moment to let it go; the new one can't be started before it has.
    if [ "$dry" = 0 ]; then
        tries=0
        while launchctl print "gui/$uid/$AGENT_LABEL" >/dev/null 2>&1 && [ "$tries" -lt 10 ]; do
            sleep 0.5
            tries=$((tries + 1))
        done
    fi
fi
# An earlier helper stops first, so its file isn't replaced under it.
if [ "$dry" = 1 ] || sudo launchctl print "system/$LABEL" >/dev/null 2>&1; then
    [ "$dry" = 0 ] || say "   (this one only if an earlier helper is loaded)"
    step launchctl bootout "system/$LABEL"
fi
step mkdir -p "$HELPER_DIR"
# The two go-ahead files of link versions 5 to 17 (made with sudo touch). Since version 18 the
# helper looks for neither, so a leftover one is only clutter: removed.
step rm -f "$HELPER_DIR/allow-changes"
step rm -f "$HELPER_DIR/allow-memory"
step ditto --noextattr --noqtn build/ua101d "$HELPER"
step ditto --noextattr --noqtn build/ua101midiagent "$AGENT"
step chown -R root:wheel "$HELPER_DIR"
step chmod 755 "$HELPER_DIR" "$HELPER" "$AGENT"
step rm -f "$AGENT_PLIST"
step ditto --noextattr --noqtn "$tmp/$LABEL.plist" "$DAEMON_PLIST"
step chown root:wheel "$DAEMON_PLIST"
step chmod 644 "$DAEMON_PLIST"
step touch "$LOG"
step chown "$me" "$LOG"
step rm -rf "$PLUGIN"
step ditto --noextattr --noqtn build/UA101.driver "$PLUGIN"
step chown -R root:wheel "$PLUGIN"
step mkdir -p "$MIDI_DRIVERS"
step rm -rf "$MIDI_PLUGIN"
step ditto --noextattr --noqtn build/UA101MIDI.plugin "$MIDI_PLUGIN"
step chown -R root:wheel "$MIDI_PLUGIN"
step chmod -R go-w "$MIDI_PLUGIN"
step launchctl bootstrap system "$DAEMON_PLIST"
step killall coreaudiod
# Last, with the helper in place: your MIDI server, as you, so it loads the MIDI plug-in. It
# starts again by itself when an app asks for MIDI; Audio MIDI Setup does, in the background.
mystep killall MIDIServer || say "   (it wasn't running)"
[ "$dry" = 1 ] || sleep 1
mystep open -g -a "Audio MIDI Setup"

if [ "$dry" = 1 ]; then
    say ""
    say "Dry run finished. Nothing was changed."
    exit 0
fi

say "4. Checking the installed copies."
for f in "$HELPER" "$PLUGIN" "$PLUGIN/Contents/MacOS/UA101"; do
    tags=$(xattr "$f" 2>/dev/null | grep -E 'quarantine|provenance' || true)
    [ -z "$tags" ] || say "   Note: $f carries $(echo $tags)"
done
codesign --verify --strict "$PLUGIN" 2>/dev/null ||
    say "   Warning: the installed plug-in's signature doesn't check out."
say "   Waiting 5 seconds for Core Audio to start again."
sleep 5
server=$(pgrep -x MIDIServer | head -n 1 || true)
if [ -n "$server" ] && vmmap "$server" 2>/dev/null | grep -q "$MIDI_PLUGIN/Contents/MacOS/UA101MIDI"; then
    say "   The MIDI plug-in is loaded. With the unit on, MIDI apps list the port \"UA-101 MIDI\"."
else
    say "   Warning: your MIDI server hasn't loaded the MIDI plug-in (yet). Open Audio MIDI Setup,"
    say "   Window menu, Show MIDI Studio: a device \"UA-101\" should be there. Its messages:"
    say "   /usr/bin/log show --last 5m --predicate 'subsystem == \"io.github.delta90h.ua101midi\"'"
fi
if system_profiler SPAudioDataType 2>/dev/null | grep -q 'UA-101'; then
    say ""
    say "Done. The UA-101 is a sound device now: see Audio MIDI Setup, or the Sound settings."
else
    say ""
    say "Installed, but Core Audio doesn't list the UA-101 yet."
    say "If the unit is on and plugged in, see \"If the UA-101 doesn't appear\" in INSTALL.md."
fi
