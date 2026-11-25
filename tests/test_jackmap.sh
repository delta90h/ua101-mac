#!/bin/bash
# Checks ua101jackmap on made-up recordings of loop-cable runs, with known wiring:
# the manual's map, a swap hidden by the straight loop, wrong cables, knobs that
# disagree, a Y cable, and 192 kHz.
set -u
BIN=${1:-build}
HERE=$(cd "$(dirname "$0")" && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fails=0
check() { if eval "$2"; then echo "  ok   $1"; else echo "  FAIL $1"; fails=$((fails + 1)); fi; }
make() { python3 -I "$HERE/jackmap_wav.py" "$@"; }
run() { "$BIN/ua101jackmap" "$@" > "$T/out.txt"; echo $?; }
has() { grep -q -- "$1" "$T/out.txt"; }

echo "ua101jackmap:"
# The manual's map: every channel where the manual says.
make "$T/a.wav"
make "$T/b.wav" --cables shifted
make "$T/s1.wav" --gain 1:10
make "$T/s2.wav" --gain 2:10
code=$(run levels "$T/a.wav")
check "straight loop: 8 of 10 arrive, one each" "[ $code -eq 0 ] && has '8 of 10 arrived, each on an input of its own'"
check "straight loop: the optical outputs reach nothing" "has 'Playback 10: not recorded'"
check "straight loop: the monitor mix isn't taken for an input" "has 'Playback  1: capture ch 1,'"
code=$(run solve "$T/a.wav" "$T/b.wav" "$T/s1.wav" "$T/s2.wav")
check "manual's map: solved and matches" "[ $code -eq 0 ] && has 'matches the manual'"
check "manual's map: SENS 2 confirms it" "has 'SENS 2 agrees'"
code=$(run solve "$T/a.wav" "$T/b.wav" "$T/s1.wav")
check "manual's map: solved without SENS 2" "[ $code -eq 0 ] && has 'matches the manual'"

# Playback 3 and 4 reach each other's OUTPUT jacks, and INPUT 3 and 4 are recorded on each
# other's channels. The straight loop can't see it; the shifted one can.
swap="--outmap '1 2 4 3 5 6 7 8' --inmap '1 2 4 3 5 6 7 8'"
eval make "$T/xa.wav" $swap
eval make "$T/xb.wav" $swap --cables shifted
eval make "$T/xs1.wav" $swap --gain 1:10
eval make "$T/xs2.wav" $swap --gain 2:10
code=$(run levels "$T/xa.wav")
check "hidden swap: the straight loop alone looks perfect" "[ $code -eq 0 ] && has 'Playback  3: capture ch 3,'"
code=$(run solve "$T/xa.wav" "$T/xb.wav" "$T/xs1.wav" "$T/xs2.wav")
check "hidden swap: found, in 4 places" "[ $code -eq 2 ] && has 'in 4 places'"
check "hidden swap: INPUT 3 is ch 4" "has 'INPUT 3: ch 4   DIFFERS'"
check "hidden swap: playback 3 is OUTPUT 4" "has 'Playback  3: OUTPUT 4   DIFFERS'"

# Mistakes in the session.
code=$(run solve "$T/a.wav" "$T/a.wav" "$T/s1.wav")
check "cables not moved for the shifted run: caught" "[ $code -eq 1 ] && has 'may not have'"
make "$T/bad.wav" --cables '2 3 5 4 6 7 8 1'
code=$(run solve "$T/a.wav" "$T/bad.wav" "$T/s1.wav")
check "two shifted cables in the wrong inputs: caught" "[ $code -eq 1 ] && has 'comes back after 7 steps'"
make "$T/s5.wav" --gain 5:10
code=$(run solve "$T/a.wav" "$T/b.wav" "$T/s1.wav" "$T/s5.wav")
check "the knobs disagree: caught" "[ $code -eq 1 ] && has 'The two knobs disagree'"
code=$(run solve "$T/a.wav" "$T/b.wav" "$T/a.wav")
check "SENS 1 not turned: caught" "[ $code -eq 1 ] && has \"doesn't show which channel SENS 1\""
code=$(run solve "$T/a.wav" "$T/b.wav" "$T/b.wav")
check "SENS 1 run with the shifted cables: caught" "[ $code -eq 1 ] && has \"cables aren't the same\""
make "$T/y.wav" --extra 1:5
code=$(run levels "$T/y.wav")
check "one output reaching two inputs: caught" "[ $code -eq 1 ] && has 'reached two inputs'"

# 192 kHz: 6 channels each way and no monitor mix.
make "$T/h.wav" --rate 192000 --channels 6 --outputs 6
code=$(run levels "$T/h.wav")
check "192 kHz: 6 of 6 arrive, one each" "[ $code -eq 0 ] && has '6 of 6 arrived, each on an input of its own'"
check "192 kHz: playback 6 on ch 6" "has 'Playback  6: capture ch 6,'"
echo "$fails failed"
[ "$fails" -eq 0 ]
