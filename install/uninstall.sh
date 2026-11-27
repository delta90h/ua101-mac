#!/bin/sh
# Takes out what the installer package (UA-101 Driver.pkg) or install/install.sh put in. Two ways
# (a design choice of 2026-11-24):
#
#   Remove the driver, keep my settings: the driver, its helper, the MIDI port and the app go.
#     The Driver Settings, the app's own choices and record, and the logs stay, for a later install.
#   Remove everything: the same, then every UA-101 setting, record, log and crash report on this
#     Mac, in every account. Your own files (Documents/UA-101 Backups, Documents/UA-101 Reports)
#     stay unless you ask for them too; then they go to that account's Trash, never deleted for
#     good (2026-11-27), so a wrong choice can be undone.
#
# macOS's privacy protection can refuse even an administrator some places in an account: the
# Documents folder, and other apps' data (Help Viewer's copy of the help, in Group Containers).
# What it refuses stays; the script goes on, and its summary lists each one as "kept (macOS
# refused)" (2026-11-27). The app moves its own account's Documents folders to the Trash itself,
# before this script runs, as it may write there.
#
# The app's menu (UA-101 Control > Uninstall UA-101...) runs this same script, so there is one
# list of what goes. From Terminal, in the ua101-driver folder:
#
#     install/uninstall.sh                          asks which way, then yes, then your password
#     install/uninstall.sh --keep-settings          the first way
#     install/uninstall.sh --everything             the second way (your own files stay)
#     install/uninstall.sh --everything --with-my-files      ... and they go too
#     add --dry-run to any of them: every step is shown, nothing is changed, no password
#
# On a Mac without this project, the package's copy is
#
#     "/Library/Application Support/ua101-driver/uninstall.sh"
#
# What it never touches: the unit's own memories and settings; macOS's MIDI settings files (they
# hold every MIDI device of the account, so the grey "UA-101" in Audio MIDI Setup goes through
# CoreMIDI, from the app, or by hand); Core Audio's lists of default devices (they name other
# devices too). In Core Audio's two settings files only the entries that belong to the UA-101
# alone go, whole and by name: into a copy first, checked, written back over the file's contents
# (Core Audio keeps the file's owner and permissions), and Core Audio is restarted at once so it
# can't write its old copy back. The files as they were are kept in /private/tmp only if something
# went wrong; when all went well they are deleted, with any an earlier run kept (2026-11-27).
#
# Internal: --asked-by=UID means the questions were asked already, by the account UID (the app,
# or this script before it ran itself again with sudo); it runs as root. --from-app: the app asked,
# and it removes the grey MIDI entry of its own account itself.

set -eu
PATH=/usr/bin:/bin:/usr/sbin:/sbin
export PATH

LABEL=io.github.delta90h.ua101d
HELPER_DIR="/Library/Application Support/ua101-driver"
DAEMON_PLIST="/Library/LaunchDaemons/$LABEL.plist"
PLUGIN="/Library/Audio/Plug-Ins/HAL/UA101.driver"
AGENT_LABEL=io.github.delta90h.ua101midi
AGENT_PLIST="/Library/LaunchAgents/$AGENT_LABEL.plist"
MIDI_PLUGIN="/Library/Audio/MIDI Drivers/UA101MIDI.plugin"
APP="/Applications/UA-101 Control.app"
APP_ID=local.ua101.control
PKG_ID=io.github.delta90h.ua101
CA_SYSTEM="/Library/Preferences/Audio/com.apple.audio.SystemSettings.plist"
CA_DEVICES="/Library/Preferences/Audio/com.apple.audio.DeviceSettings.plist"
# Core Audio's entries that belong to the UA-101 alone: the Driver Settings (the plug-in's own
# storage, "Plug-In." and its bundle id) and what Core Audio remembers of the device itself.
CA_SYSTEM_KEYS="Plug-In.io.github.delta90h.ua101.driver
device.io.github.delta90h.ua101:UA-101"
CA_DEVICES_KEYS="io.github.delta90h.ua101:UA-101"

usage() {
    echo "Usage: uninstall.sh [--keep-settings | --everything [--with-my-files]] [--dry-run]" >&2
    exit 2
}

dry=0 mode= myfiles=0 asked_by= from_app=0
for a in "$@"; do
    case "$a" in
        --dry-run) dry=1 ;;
        --keep-settings) [ -z "$mode" ] || usage; mode=keep ;;
        --everything) [ -z "$mode" ] || usage; mode=all ;;
        --with-my-files) myfiles=1 ;;
        --asked-by=*) asked_by=${a#--asked-by=} ;;
        --from-app) from_app=1 ;;
        *) usage ;;
    esac
done
[ "$myfiles" = 0 ] || [ "$mode" = all ] || usage

say() { printf '%s\n' "$*"; }
fail() { printf '\nStopped: %s\n' "$*" >&2; exit 1; }
problems=
problem() { say "  ! $*"; problems="$problems
- $*"; }

# A step: shown as it would be typed, then run (as root by then), unless this is a dry run.
step() {
    line=
    for a in "$@"; do
        case "$a" in
            *" "*) line="$line \"$a\"" ;;
            *) line="$line $a" ;;
        esac
    done
    printf '  %s\n' "${line# }"
    [ "$dry" = 1 ] || "$@"
}

if [ -n "$asked_by" ]; then
    case "$asked_by" in "" | *[!0-9]*) usage ;; esac
    [ "$(id -u)" = 0 ] || fail "--asked-by is for the app and for this script itself."
    [ -n "$mode" ] && [ "$dry" = 0 ] || usage
else
    [ "$(id -u)" != 0 ] || fail "run it without sudo. It asks for your password itself."
fi
me=${asked_by:-$(id -u)}

# The package's records, if it installed the driver (one for each of its parts).
receipts=$(pkgutil --pkgs 2>/dev/null | grep "^$PKG_ID\." || true)
installed=0
if [ -e "$PLUGIN" ] || [ -e "$DAEMON_PLIST" ] || [ -e "$HELPER_DIR" ] || [ -e "$AGENT_PLIST" ] ||
    [ -e "$MIDI_PLUGIN" ] || [ -n "$receipts" ]; then
    installed=1
fi

# From Terminal: which way, then yes, then the password; then this script again, as root.
if [ -z "$asked_by" ]; then
    if [ -z "$mode" ]; then
        say "Two ways to remove the UA-101 driver:"
        say "  1  Remove the driver, keep my settings (for a later install)"
        say "  2  Remove everything: also every UA-101 setting, record and log, in every account"
        printf 'Type 1 or 2: '
        read -r answer || answer=
        case "$answer" in
            1) mode=keep ;;
            2) mode=all ;;
            *) fail "you didn't type 1 or 2. Nothing was changed." ;;
        esac
        if [ "$mode" = all ]; then
            say "Your own files stay: Documents/UA-101 Backups and Documents/UA-101 Reports, in every account."
            printf "Type remove to move them to the Trash too, or just press Return to keep them: "
            read -r answer || answer=
            [ "$answer" != remove ] || myfiles=1
        fi
    fi
    if [ "$mode" = keep ] && [ "$installed" = 0 ] && [ "$dry" = 0 ]; then
        say "The UA-101 driver isn't installed. Nothing to do."
        exit 0
    fi
    if [ "$dry" = 0 ]; then
        if pgrep -x -u "$me" ua101panel >/dev/null 2>&1; then
            fail "UA-101 Control is open. Quit it first, or use its own menu: UA-101 Control > Uninstall UA-101..."
        fi
        if [ "$mode" = all ]; then
            say "This removes everything of the UA-101 from this Mac, in every account."
            [ "$myfiles" = 1 ] && say "Your backup files and reports go to the Trash too." ||
                say "Your backup files and reports stay."
        else
            say "This removes the UA-101 driver. Your settings stay, for a later install."
        fi
        say "Quit any app using the UA-101 first. All sound and MIDI on this Mac stop for a moment."
        printf 'Type yes to go on: '
        read -r answer || answer=
        [ "$answer" = yes ] || fail "you didn't type yes. Nothing was changed."
        sudo -v || fail "sudo didn't take the password. Nothing was changed."
        set -- --keep-settings
        [ "$mode" = keep ] || set -- --everything
        [ "$myfiles" = 0 ] || set -- "$@" --with-my-files
        exec sudo /bin/sh "$0" "$@" "--asked-by=$me"
    fi
fi

if [ "$mode" = all ]; then
    what="remove everything"
    [ "$myfiles" = 1 ] && what="$what, your own files to the Trash" || what="$what, your own files stay"
else
    what="remove the driver, keep the settings"
fi
if [ "$dry" = 1 ]; then
    say "Dry run ($what): these are the steps. Nothing is removed or restarted."
else
    say "Removing the UA-101 ($what), $(date '+%Y-%m-%d %H:%M:%S')."
fi

# Core Audio's entries for the UA-101, step 1: in a copy of one file, the entries named go, and
# the copy is checked. Returns 1 when there is nothing to take out or the copy didn't check out.
esc_key() { printf '%s' "$1" | sed 's/\./\\./g'; }
prepare_ca() { # file, keys (one a line), work folder
    f=$1 keys=$2 copy="$3/$(basename "$1")"
    [ -f "$f" ] || return 1
    cp -p "$f" "$copy.as-it-was"
    cp "$f" "$copy"
    binary=0
    if [ "$(head -c 6 "$copy")" = bplist ]; then
        binary=1
        plutil -convert xml1 "$copy" || return 1
    fi
    removed=0
    old_ifs=$IFS
    IFS='
'
    for k in $keys; do
        grep -q -F "<key>$k</key>" "$copy" || continue
        if ! plutil -remove "$(esc_key "$k")" "$copy" 2>/dev/null; then
            case "$k" in
                *:*) ;;
                *) /usr/libexec/PlistBuddy -c "Delete :$k" "$copy" >/dev/null 2>&1 || true ;;
            esac
        fi
        plutil -convert xml1 "$copy" 2>/dev/null || true
        if grep -q -F "<key>$k</key>" "$copy"; then
            IFS=$old_ifs
            problem "couldn't take \"$k\" out of $f: that file is left as it is"
            return 1
        fi
        removed=$((removed + 1))
        say "    \"$k\""
    done
    IFS=$old_ifs
    [ "$removed" -gt 0 ] || return 1
    before=$(plutil -p "$copy.as-it-was" | grep -c '^  "' || true)
    after=$(plutil -p "$copy" | grep -c '^  "' || true)
    if ! plutil -lint -s "$copy" || [ $((before - removed)) != "$after" ]; then
        problem "the changed copy of $f didn't check out: that file is left as it is"
        return 1
    fi
    [ "$binary" = 0 ] || plutil -convert binary1 "$copy" || return 1
    return 0
}

# Step 2: each file prepared is written over (if Core Audio hasn't changed it since the copy),
# and Core Audio is stopped at once, without the chance to save what it had in memory; launchd
# starts it again straight away, without the plug-in, from the files as they are now.
forget_core_audio() {
    # Copies an earlier run kept (it keeps them only when something went wrong).
    for old in /private/tmp/ua101-uninstall.*; do
        if [ -d "$old" ]; then step rm -rf "$old"; fi
    done
    work=$(mktemp -d /private/tmp/ua101-uninstall.XXXXXX)
    say "  Core Audio's entries for the UA-101 (the Driver Settings, and what it remembers of the device):"
    ready=
    if prepare_ca "$CA_SYSTEM" "$CA_SYSTEM_KEYS" "$work"; then ready="$ready system"; fi
    if prepare_ca "$CA_DEVICES" "$CA_DEVICES_KEYS" "$work"; then ready="$ready devices"; fi
    [ -n "$ready" ] || say "    (none there)"
    if [ "$dry" = 1 ]; then
        say "  (each file: written over from its checked copy, then at once:)"
        step killall -KILL coreaudiod
        rm -rf "$work"
        return 0
    fi
    for which in $ready; do
        f=$CA_SYSTEM
        [ "$which" = system ] || f=$CA_DEVICES
        if cmp -s "$f" "$work/$(basename "$f").as-it-was"; then
            cat "$work/$(basename "$f")" >"$f"
            say "  written: $f"
        else
            problem "Core Audio changed $f just then: it is left as it is"
        fi
    done
    step killall -KILL coreaudiod || say "  (it wasn't running)"
    sleep 3
    old_ifs=$IFS
    IFS='
'
    for k in $CA_SYSTEM_KEYS; do
        if grep -q -F "<key>$k</key>" "$CA_SYSTEM"; then problem "Core Audio's file still has \"$k\""; fi
    done
    for k in $CA_DEVICES_KEYS; do
        if grep -q -F "<key>$k</key>" "$CA_DEVICES"; then problem "Core Audio's file still has \"$k\""; fi
    done
    IFS=$old_ifs
    ca_copy=$work
}

# The accounts, and where each one's home is.
accounts() { dscl . -list /Users UniqueID | awk '$2 >= 501 {print $1}'; }
home_of() { dscl . -read "/Users/$1" NFSHomeDirectory 2>/dev/null | sed -n 's/^NFSHomeDirectory: *//p' | head -n 1; }
logged_in=$(ps -axo uid=,comm= | awk '$2 ~ /loginwindow$/ {print $1}' | sort -u)

# One file or folder in an account. If macOS refuses (its privacy protection), it stays, the
# script goes on, and the summary lists it.
refused=
refuse() { say "    kept (macOS refused): $1"; refused="$refused
  $1"; }
remove_in_home() {
    if [ "$dry" = 1 ]; then step rm -rf "$1"; return 0; fi
    step rm -rf "$1" || true
    if [ -e "$1" ] || [ -L "$1" ]; then refuse "$1"; fi
}
# Help Viewer's copy of the app's help, in its own container. macOS protects that container, even
# from root and without asking (seen 2026-11-27, also for the app itself): then the copy stays
# (it does no harm), and the summary says how to drag it to the Trash in the Finder. Only looked
# at for real: in a dry run macOS could ask whether this script may see another app's data.
HV_CACHES="Library/Group Containers/group.com.apple.helpviewer.content/Library/Caches"
HV_NAME="$APP_ID.$APP_ID.help"
hv_left=
help_viewer_copy() { # home
    if [ "$dry" = 1 ]; then
        say "    (if there: $1/$HV_CACHES/$HV_NAME*)"
        return 0
    fi
    [ -d "$1/Library/Group Containers/group.com.apple.helpviewer.content" ] || return 0
    if listing=$(ls -1 "$1/$HV_CACHES" 2>&1); then
        old_ifs=$IFS
        IFS='
'
        set -f # its names hold a "*"
        left=0
        for n in $listing; do
            case "$n" in "$HV_NAME"*)
                step rm -rf "$1/$HV_CACHES/$n" || true
                if [ -e "$1/$HV_CACHES/$n" ]; then left=1; fi ;;
            esac
        done
        set +f
        IFS=$old_ifs
    else
        case "$listing" in *"not permitted"*) left=1 ;; *) left=0 ;; esac
    fi
    if [ "$left" = 1 ]; then
        say "    kept (macOS protects Help Viewer's folder): $1/$HV_CACHES/$HV_NAME*"
        hv_left="$hv_left
  $1/$HV_CACHES"
    fi
}

# The accounts a person logs in to, besides the account UID: numbered from 501, not hidden, with a
# login shell (apps make hidden accounts of their own, such as a VPN's).
people_besides() { # uid
    for n in $(accounts); do
        u=$(id -u "$n" 2>/dev/null) || continue
        [ "$u" != "$1" ] || continue
        login=$(dscl . -read "/Users/$n" UserShell 2>/dev/null | awk '{print $2}')
        case "$login" in "" | */false | */nologin) continue ;; esac
        if dscl . -read "/Users/$n" IsHidden 2>/dev/null | grep -q ': *1$'; then continue; fi
        echo "$n"
    done
}

# ---- The driver: MIDI first, then audio, then the helper, the app and the package's records.
[ "$dry" = 0 ] || say "  (the MIDI agent: only where it is loaded)"
for uid in $logged_in; do
    if [ "$dry" = 1 ] || launchctl print "gui/$uid/$AGENT_LABEL" >/dev/null 2>&1; then
        step launchctl bootout "gui/$uid/$AGENT_LABEL" || say "  (it was already stopping)"
    fi
done
step rm -f "$AGENT_PLIST"
step rm -rf "$MIDI_PLUGIN"
step killall MIDIServer || say "  (it wasn't running)"
step rm -rf "$PLUGIN"
if [ "$mode" = all ]; then
    forget_core_audio
else
    step killall coreaudiod || say "  (it wasn't running)"
fi
if [ "$dry" = 1 ] || launchctl print "system/$LABEL" >/dev/null 2>&1; then
    [ "$dry" = 0 ] || say "  (this one only if the helper is loaded)"
    step launchctl bootout "system/$LABEL" || say "  (it was already stopping)"
fi
step rm -f "$DAEMON_PLIST"
step rm -rf "$HELPER_DIR"
# The app if the package put it there; with "everything", also a copy put there by hand.
app_ours=0
if [ -d "$APP" ] && [ "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$APP/Contents/Info.plist" 2>/dev/null)" = "$APP_ID" ]; then
    app_ours=1
fi
app_gone=0
if [ "$app_ours" = 1 ] && { [ -n "$receipts" ] || [ "$mode" = all ]; }; then
    step rm -rf "$APP"
    app_gone=1
elif [ "$app_ours" = 1 ]; then
    say "  ($APP stays: the installer package didn't put it there)"
fi
for r in $receipts; do
    step pkgutil --forget "$r"
done

# ---- Everything: the logs and crash reports of the driver's programs, then each account.
kept=
trashed=
# The user's own files: into that account's Trash, under a name with the time (never deleted
# for good). If macOS refuses, they stay and are listed.
trash_in_home() { # home, uid, path
    to="$1/.Trash/$(basename "$3") $(date '+%Y-%m-%d %H.%M.%S')"
    if [ "$dry" = 1 ]; then step mv "$3" "$to"; return 0; fi
    if [ ! -d "$1/.Trash" ]; then
        mkdir -m 700 "$1/.Trash" 2>/dev/null && chown "$2" "$1/.Trash" || true
    fi
    step mv "$3" "$to" || true
    if [ -e "$3" ]; then
        refuse "$3"
    else
        trashed="$trashed
  $3"
    fi
}
if [ "$mode" = all ]; then
    say "  The driver's logs and crash reports:"
    for f in /Library/Logs/ua101d.log* /Library/Logs/ua101midiagent.log*; do
        if [ -e "$f" ]; then step rm -f "$f"; fi
    done
    find /Library/Logs/DiagnosticReports -maxdepth 2 \( -name 'ua101d[-_.]*' -o -name 'ua101midiagent[-_.]*' \
        -o -name 'ua101panel[-_.]*' \) 2>/dev/null | while IFS= read -r f; do step rm -f "$f"; done

    for name in $(accounts); do
        home=$(home_of "$name")
        [ -n "$home" ] && [ -d "$home/Library" ] || continue
        uid=$(id -u "$name" 2>/dev/null) || continue
        say "  The account $name:"
        if [ "$dry" = 1 ] && [ ! -r "$home/Library/Preferences" ]; then
            say "    (its folders can't be looked into without the password: whatever of this list is there goes)"
            say "    ~/Library/Preferences/$APP_ID.plist, ~/Library/Logs/UA-101 Control.log,"
            say "    ~/Library/Saved Application State/$APP_ID.savedState, ~/Library/Caches/$APP_ID,"
            say "    ~/Library/HTTPStorages/$APP_ID, crash reports of ua101panel, Help Viewer's copy of the help"
            continue
        fi
        # UA-101 Control open in that account (not the one that asked): it would write its
        # settings back as it quits. Stopped first.
        if [ "$uid" != "$me" ] && pgrep -x -u "$uid" ua101panel >/dev/null 2>&1; then
            step pkill -x -u "$uid" ua101panel || true
            [ "$dry" = 1 ] || sleep 1
        fi
        # The app's settings: through that account's settings service when it runs, so it
        # doesn't write its copy back; then the file, if it is still there.
        prefs="$home/Library/Preferences/$APP_ID.plist"
        if [ -e "$prefs" ]; then
            if pgrep -x -u "$uid" cfprefsd >/dev/null 2>&1; then
                step launchctl asuser "$uid" sudo -u "$name" defaults delete "$APP_ID" 2>/dev/null || true
            fi
            if [ "$dry" = 1 ] || [ -e "$prefs" ]; then remove_in_home "$prefs"; fi
        fi
        for p in "Library/Logs/UA-101 Control.log" "Library/Saved Application State/$APP_ID.savedState" \
            "Library/Caches/$APP_ID" "Library/HTTPStorages/$APP_ID" "Library/HTTPStorages/$APP_ID.binarycookies" \
            "Library/WebKit/$APP_ID"; do
            if [ -e "$home/$p" ]; then remove_in_home "$home/$p"; fi
        done
        found=$({
            find "$home/Library/Application Support/CrashReporter" -maxdepth 1 -name 'ua101panel_*.plist'
            find "$home/Library/Logs/DiagnosticReports" -maxdepth 2 -name 'ua101panel[-_.]*'
            find "$home/Library/Caches/com.apple.helpd/Generated" -maxdepth 1 -name "$APP_ID.help*"
        } 2>/dev/null || true)
        old_ifs=$IFS
        IFS='
'
        set -f
        for f in $found; do remove_in_home "$f"; done
        set +f
        IFS=$old_ifs
        help_viewer_copy "$home"
        for d in "Documents/UA-101 Backups" "Documents/UA-101 Reports"; do
            [ -e "$home/$d" ] || continue
            if [ "$myfiles" = 1 ]; then
                trash_in_home "$home" "$uid" "$home/$d"
            else
                say "    kept: $home/$d"
                kept="$kept
  $home/$d"
            fi
        done
    done
fi

# ---- Core Audio's two files as they were: deleted when all went well, kept for a way back if not.
ca_copy=${ca_copy:-}
if [ -n "$ca_copy" ] && [ "$dry" = 0 ]; then
    if [ -z "$problems" ]; then
        step rm -rf "$ca_copy"
    else
        problems="$problems
- Core Audio's two files as they were are kept in $ca_copy (until the Mac restarts)"
    fi
fi

# ---- What happened, in short (the app shows from here on).
say ""
if [ "$dry" = 1 ]; then
    say "== Dry run finished =="
    say "Nothing was changed."
    exit 0
fi
say "== What happened =="
if [ "$mode" = keep ]; then
    if [ "$app_gone" = 1 ]; then
        say "The UA-101 driver is removed: the driver, its helper, the MIDI port and the app."
    else
        say "The UA-101 driver is removed: the driver, its helper and the MIDI port."
    fi
    say "Kept for a later install: your Driver Settings, the app's own choices and its record, and the logs."
    say "Audio MIDI Setup may still show a grey \"UA-101\": it does nothing now, and it comes back"
    say "to life when you install the driver again."
else
    say "Everything of the UA-101 is removed from this Mac, in every account: the driver, its helper,"
    say "the MIDI port, the app, the Driver Settings, the app's choices and record, logs and crash reports."
    if [ -n "$kept" ]; then
        say "Your own files stay (you can delete them yourself in the Finder):$kept"
    fi
    if [ -n "$trashed" ]; then
        say "Moved to the Trash (empty the Trash to delete them for good):$trashed"
    fi
    if [ -n "$refused" ]; then
        say "These stay: macOS's privacy protection didn't let the uninstaller remove them."
        say "You can move them to the Trash yourself in the Finder:$refused"
    fi
    if [ -n "$hv_left" ]; then
        say "Help Viewer's copy of the UA-101 manual stays: macOS protects Help Viewer's folder, even from"
        say "the uninstaller. It does no harm. To remove it: in the Finder choose Go > Go to Folder...,"
        say "paste the folder below, press Return, and drag the items whose names start with"
        say "$HV_NAME to the Trash:$hv_left"
    fi
    # The other accounts a person uses (the app clears its own account's grey UA-101 itself).
    others=$(people_besides "$me" | wc -l | tr -d ' ')
    if [ "$from_app" = 0 ] || [ "$others" -gt 0 ]; then
        if [ "$from_app" = 1 ]; then
            say "In other accounts, Audio MIDI Setup may still show a grey \"UA-101\". To remove it there:"
        elif [ "$others" -gt 0 ]; then
            say "Audio MIDI Setup may still show a grey \"UA-101\" (in each account). To remove it:"
        else
            say "Audio MIDI Setup may still show a grey \"UA-101\". To remove it:"
        fi
        say "open Audio MIDI Setup, choose Window > Show MIDI Studio, click the UA-101, press Delete."
    fi
fi
if [ -n "$problems" ]; then
    say "Not everything went as planned:$problems"
    exit 1
fi
exit 0
