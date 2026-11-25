#!/bin/sh
# Builds the installer package "build/UA-101 Driver.pkg". Run by `make pkg`,
# after make has built the helper, both plug-ins, the MIDI agent and the end-user app. It only
# builds files: nothing is installed, and no password is asked.
#
#     install/pkg/build-pkg.sh BUILD MACOS_MIN VERSION
#
# Signing: the default is option A, no Apple signing. Every program and
# bundle is signed ad hoc (codesign -s -), the package itself is unsigned, and nothing goes to
# Apple. Option B later (Developer ID and notarization) is these three settings, nothing else:
#
#     make pkg CODESIGN_ID="Developer ID Application: NAME (TEAM)" \
#              INSTALLER_ID="Developer ID Installer: NAME (TEAM)" NOTARY_PROFILE=ua101
#
# (NOTARY_PROFILE is a name saved once with `xcrun notarytool store-credentials`.) With them the
# programs get the hardened runtime and a timestamp, the package is signed, sent to Apple's
# notary service and the ticket stapled. That path is written but has never run: untested.
#
# Layout: one package for each place on the Mac, each installed at its own folder or bundle, so
# no folder of macOS itself (/Applications, /Library, ...) is in any of them and none can change
# their owner or permissions. A first package with no files runs scripts/preinstall, a last one
# runs scripts/postinstall (both as root, by Installer).

set -eu
# No "._" files from tools that keep extended attributes that way.
export COPYFILE_DISABLE=1

BUILD=${1:?build folder}
MACOS_MIN=${2:?oldest macOS}
VERSION=${3:?version}
CODESIGN_ID=${CODESIGN_ID:--}
INSTALLER_ID=${INSTALLER_ID:-}
NOTARY_PROFILE=${NOTARY_PROFILE:-}
ID=io.github.delta90h.ua101
OUT="$BUILD/UA-101 Driver.pkg"
HERE=$(cd "$(dirname "$0")" && pwd)
INSTALL_DIR=$(cd "$HERE/.." && pwd)

fail() { printf 'build-pkg: %s\n' "$*" >&2; exit 1; }
say() { printf '%s\n' "$*"; }

[ -x "$BUILD/ua101d" ] && [ -x "$BUILD/ua101midiagent" ] && [ -d "$BUILD/UA101.driver" ] &&
    [ -d "$BUILD/UA101MIDI.plugin" ] && [ -d "$BUILD/user/UA-101 Control.app" ] ||
    fail "a part is missing: run make helper plugin midiplugin agent user-app"

# Outside iCloud: iCloud and the Finder put information on folders in the project, which
# codesign refuses and which would end up in the package.
work=$(mktemp -d "${TMPDIR:-/tmp}/ua101-pkg.XXXXXX")
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/root" "$work/pkgs" "$work/empty"

stage() { # source, name of the staged copy
    mkdir -p "$work/root/$2"
    ditto --noextattr --noqtn --norsrc "$1" "$work/root/$2/$(basename "$1")"
}
stage "$BUILD/user/UA-101 Control.app" app
stage "$BUILD/UA101.driver" hal
# The debug symbols the build keeps inside the bundle name the build folder (and so the
# developer's account): not for a user's Mac.
rm -rf "$work/root/hal/UA101.driver/Contents/MacOS/UA101.dSYM"
stage "$BUILD/UA101MIDI.plugin" midi
mkdir -p "$work/root/helper/ua101-driver"
for f in "$BUILD/ua101d" "$BUILD/ua101midiagent" "$INSTALL_DIR/uninstall.sh" \
        "$INSTALL_DIR/io.github.delta90h.ua101d.plist"; do
    ditto --noextattr --noqtn --norsrc "$f" "$work/root/helper/ua101-driver/$(basename "$f")"
done
H="$work/root/helper/ua101-driver"
chmod 755 "$H" "$H/ua101d" "$H/ua101midiagent" "$H/uninstall.sh"
chmod 644 "$H/io.github.delta90h.ua101d.plist"
plutil -lint -s "$H/io.github.delta90h.ua101d.plist" || fail "the helper's launchd file doesn't read"
chmod -R u+rwX,go+rX,go-w "$work/root"
xattr -cr "$work/root" 2>/dev/null || true

# Every program for Apple silicon and the chosen oldest macOS, or the build stops (an older
# build left in build/ would have the macOS it was built on: make clean, then make pkg).
machos=$(find "$work/root" -type f -perm -u+x -exec sh -c 'file -b "$1" | grep -q Mach-O && echo "$1"' _ {} \;)
[ -n "$machos" ] || fail "no programs found"
echo "$machos" | while IFS= read -r m; do
    archs=$(lipo -archs "$m")
    [ "$archs" = arm64 ] || fail "$m is for $archs, not arm64 only"
    minos=$(vtool -show-build "$m" | awk '$1 == "minos" {print $2}')
    [ "$minos" = "$MACOS_MIN" ] || fail "$m is built for macOS $minos, not $MACOS_MIN: run make clean, then make pkg"
done
for p in "$work/root/app/UA-101 Control.app/Contents/Info.plist" \
         "$work/root/hal/UA101.driver/Contents/Info.plist" \
         "$work/root/midi/UA101MIDI.plugin/Contents/Info.plist"; do
    v=$(/usr/libexec/PlistBuddy -c 'Print :LSMinimumSystemVersion' "$p")
    [ "$v" = "$MACOS_MIN" ] || fail "$p says LSMinimumSystemVersion $v, not $MACOS_MIN"
done

# Signatures, inside out. Ad hoc unless option B's names are given.
sign() { # path; a plain program keeps its file name as its signing name, as the linker gives it
    set -- "$1" --force
    [ -d "$1" ] || set -- "$@" --identifier "$(basename "$1")"
    if [ "$CODESIGN_ID" = - ]; then
        codesign "$@" -s - "$1" 2>/dev/null
    else
        codesign "$@" -s "$CODESIGN_ID" --options runtime --timestamp "$1"
    fi
}
# Debug entries (source file paths) out of the handed-out copies; they are signed again below.
for b in "$H/ua101d" "$H/ua101midiagent" "$work/root/hal/UA101.driver/Contents/MacOS/UA101" \
         "$work/root/midi/UA101MIDI.plugin/Contents/MacOS/UA101MIDI" \
         "$work/root/app/UA-101 Control.app/Contents/MacOS/ua101panel"; do
    strip -S "$b" 2>/dev/null || fail "strip -S $b failed"
done
# Nothing handed out may name the build's home folder (a path in debug info or a string).
if grep -r -a -l -F "$HOME" "$work/root" >/dev/null 2>&1; then
    fail "these staged files name $HOME: $(grep -r -a -l -F "$HOME" "$work/root" | tr '\n' ' ')"
fi
sign "$H/ua101d"
sign "$H/ua101midiagent"
sign "$work/root/hal/UA101.driver"
sign "$work/root/midi/UA101MIDI.plugin"
sign "$work/root/app/UA-101 Control.app"
for b in "$H/ua101d" "$H/ua101midiagent" "$work/root/hal/UA101.driver" \
         "$work/root/midi/UA101MIDI.plugin" "$work/root/app/UA-101 Control.app"; do
    codesign --verify --strict "$b" || fail "the signature of $b doesn't check out"
done
[ "$(nm -gU "$work/root/midi/UA101MIDI.plugin/Contents/MacOS/UA101MIDI" | awk '{print $3}')" = _UA101MIDI_Create ] ||
    fail "the MIDI plug-in exports more than its factory"
! otool -L "$work/root/midi/UA101MIDI.plugin/Contents/MacOS/UA101MIDI" | grep -q IOKit ||
    fail "the MIDI plug-in links IOKit"
/usr/libexec/PlistBuddy -c 'Print :AudioServerPlugIn_MachServices' \
    "$work/root/hal/UA101.driver/Contents/Info.plist" | grep -q io.github.delta90h.ua101d ||
    fail "the plug-in doesn't list the helper"

# pkgbuild stores extended attributes (com.apple.provenance, which macOS puts on every file a
# program like this one writes, and which can't be removed) as "._" files. Those would be put
# on the Mac with every file. So each package is rebuilt without them: the file list (Bom)
# without the "._" lines, the files (Payload) packed again with no attributes, owner root:wheel.
component() { # name, root, install location, [scripts folder]
    name=$1 root=$2 location=$3 scripts=${4-}
    plist="$work/$name.plist"
    if [ "$root" = "$work/empty" ]; then
        set -- --nopayload
    else
        pkgbuild --analyze --root "$root" "$plist" >/dev/null
        n=0
        while /usr/libexec/PlistBuddy -c "Print :$n" "$plist" >/dev/null 2>&1; do
            /usr/libexec/PlistBuddy -c "Delete :$n:BundleIsRelocatable" "$plist" >/dev/null 2>&1 || true
            /usr/libexec/PlistBuddy -c "Add :$n:BundleIsRelocatable bool false" "$plist"
            n=$((n + 1))
        done
        set -- --root "$root" --component-plist "$plist" --install-location "$location"
    fi
    [ -z "$scripts" ] || set -- "$@" --scripts "$scripts"
    pkgbuild "$@" --identifier "$ID.$name" --version "$VERSION" --ownership recommended \
        --min-os-version "$MACOS_MIN" "$work/raw-$name.pkg" >/dev/null
    if [ "$root" = "$work/empty" ]; then
        mv "$work/raw-$name.pkg" "$work/pkgs/$name.pkg"
        return
    fi
    x="$work/x-$name"
    pkgutil --expand "$work/raw-$name.pkg" "$x"
    lsbom "$x/Bom" | grep -v '/\._' >"$work/$name.bomlist"
    rm "$x/Bom" "$x/Payload"
    mkbom -i "$work/$name.bomlist" "$x/Bom"
    (cd "$root" && find . ! -name '._*' | LC_ALL=C sort |
        cpio -o --quiet --format odc -R 0:0 | gzip -9 -n) >"$x/Payload"
    files=$(lsbom -s "$x/Bom" | wc -l | tr -d ' ')
    sed -i '' -E "s/numberOfFiles=\"[0-9]+\"/numberOfFiles=\"$files\"/" "$x/PackageInfo"
    lsbom "$x/Bom" | grep -q '/\._' && fail "$name: \"._\" files are still listed"
    gunzip -c "$x/Payload" | cpio -t --quiet 2>/dev/null | grep -q '/\._' && fail "$name: \"._\" files are still packed"
    pkgutil --flatten "$x" "$work/pkgs/$name.pkg"
}

cp "$HERE/scripts/preinstall" "$HERE/scripts/postinstall" "$work/"
mkdir -p "$work/pre" "$work/post"
mv "$work/preinstall" "$work/pre/preinstall"
mv "$work/postinstall" "$work/post/postinstall"
chmod 755 "$work/pre/preinstall" "$work/post/postinstall"
sh -n "$work/pre/preinstall" && sh -n "$work/post/postinstall" || fail "a script doesn't read"

component prepare "$work/empty" / "$work/pre"
component helper "$work/root/helper/ua101-driver" "/Library/Application Support/ua101-driver"
component hal "$work/root/hal/UA101.driver" /Library/Audio/Plug-Ins/HAL/UA101.driver
component midi "$work/root/midi/UA101MIDI.plugin" "/Library/Audio/MIDI Drivers/UA101MIDI.plugin"
component app "$work/root/app/UA-101 Control.app" "/Applications/UA-101 Control.app"
component finish "$work/empty" / "$work/post"

sed -e "s/@VERSION@/$VERSION/g" -e "s/@MACOS_MIN@/$MACOS_MIN/g" "$HERE/distribution.xml" \
    >"$work/distribution.xml"
mkdir -p "$work/resources"
cp "$HERE/resources/welcome.html" "$HERE/resources/readme.html" "$work/resources/"

rm -f "$OUT"
if [ -n "$INSTALLER_ID" ]; then
    set -- --sign "$INSTALLER_ID" --timestamp
else
    set --
fi
productbuild --distribution "$work/distribution.xml" --resources "$work/resources" \
    --package-path "$work/pkgs" "$@" "$work/UA-101 Driver.pkg" >/dev/null
ditto --noextattr --noqtn "$work/UA-101 Driver.pkg" "$OUT"

if [ -n "$NOTARY_PROFILE" ]; then
    [ -n "$INSTALLER_ID" ] || fail "notarizing needs INSTALLER_ID too"
    xcrun notarytool submit "$OUT" --keychain-profile "$NOTARY_PROFILE" --wait
    xcrun stapler staple "$OUT"
fi

cp "$HERE/read-me-first.txt" "$BUILD/UA-101 Driver - Read Me First.txt"
# The manual beside the installer, to read before installing (2026-11-23): the
# same PDF the app carries (make user-app builds it).
MANUAL_PDF="$BUILD/manual/UA-101 Control Manual.pdf"
[ -f "$MANUAL_PDF" ] || fail "no manual PDF at $MANUAL_PDF (make manual)"
cp "$MANUAL_PDF" "$BUILD/UA-101 Control Manual.pdf"
say "beside it: UA-101 Driver - Read Me First.txt and UA-101 Control Manual.pdf"
say "built $OUT ($(du -k "$OUT" | awk '{print $1}') KB, version $VERSION, macOS $MACOS_MIN or later, Apple silicon)"
if [ -z "$INSTALLER_ID" ]; then
    say "not signed by Apple (option A): a downloaded copy needs Open Anyway once"
fi
