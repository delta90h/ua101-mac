#!/bin/bash
# The cable test: checks the rate correction against a third, independent clock.
# The Mac plays one quiet 5 s tone (-40 dBFS, left only) from its headphone jack into
# the unit's rear INPUT 3 while the unit records, and Core Audio times the Mac's output.
#
# Run it only with the user there, after they have:
#   1. turned the unit off, connected the cable (3.5 mm end in the Mac's headphone jack,
#      1/4-inch end in rear INPUT 3), and turned the unit on with the monitors low;
#   2. chosen External Headphones as the Mac's sound output, at about half volume.
#
# Usage: scripts/cabletest.sh [RESULTS_DIR]
# Results (small) go to RESULTS_DIR, by default under ~/ua101-runs; recordings to /tmp/ua101.
set -u
PROJECT=$(cd "$(dirname "$0")/.." && pwd)
BIN="$PROJECT/build"
NAME="cable-$(date +%Y%m%d-%H%M%S)"
DIR=${1:-$HOME/ua101-runs/$NAME}
WAVS=/tmp/ua101/$NAME
mkdir -p "$DIR" "$WAVS"

"$BIN/ua101macrate" | tee "$DIR/device.txt"
if ! grep -q -i 'output: .*headphone' "$DIR/device.txt"; then
    echo "Stopped: choose External Headphones as the sound output first."
    exit 2
fi
echo "Recording 100 s; the tone plays about 45 s in. Leave the Mac and the unit alone."
"$BIN/ua101stream" --capture-only --seconds 100 --wav "$WAVS/capture.wav" \
    --clock-log "$DIR/clock.csv" --stall-log "$DIR/stalls.csv" > "$DIR/stream.log" 2>&1 &
stream=$!
sleep 5
"$BIN/ua101macrate" --play --seconds 90 --tone-at 40 --log "$DIR/mac.csv" > "$DIR/mac.log" 2>&1
cat "$DIR/mac.log"
wait "$stream"
grep -a -E '=== (Stage 2|Clock)' "$DIR/stream.log"
python3 -I "$PROJECT/scripts/cablecheck.py" "$DIR" "$WAVS" | tee "$DIR/result.txt"
