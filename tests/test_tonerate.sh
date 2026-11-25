#!/bin/bash
# Checks ua101tonerate on files with known answers: exact offsets, the caps on level and
# length, a silent channel, a lost sample, and a round trip through ua101fixrate.
set -u
BIN=${1:-build}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fails=0
check() { if eval "$2"; then echo "  ok   $1"; else echo "  FAIL $1"; fails=$((fails + 1)); fi; }
ppm() { "$BIN/ua101tonerate" measure "$1" "${2:-1}" | sed -n -E 's/.*Hz: ([+-][0-9.]+) ppm.*/\1/p'; }

echo "ua101tonerate:"
for p in 0 115 -37.5 1000; do
    "$BIN/ua101tonerate" make "$T/t.wav" --ppm "$p" > /dev/null
    check "measures $p ppm" "python3 -c 'import sys; sys.exit(abs(float(\"$(ppm "$T/t.wav")\") - $p) > 0.001)'"
done
"$BIN/ua101tonerate" make "$T/t44.wav" --rate 44100 --ppm 50 > /dev/null
check "measures 50 ppm at 44.1 kHz" "python3 -c 'import sys; sys.exit(abs(float(\"$(ppm "$T/t44.wav")\") - 50) > 0.001)'"
"$BIN/ua101tonerate" make "$T/t192.wav" --rate 192000 --ppm -20 > /dev/null
check "measures -20 ppm at 192 kHz" "python3 -c 'import sys; sys.exit(abs(float(\"$(ppm "$T/t192.wav")\") + 20) > 0.001)'"
check "refuses a tone above -40 dBFS" "! '$BIN/ua101tonerate' make '$T/x.wav' --db -39 > /dev/null"
check "refuses a tone over 5 s" "! '$BIN/ua101tonerate' make '$T/x.wav' --seconds 5.1 > /dev/null"
"$BIN/ua101tonerate" make "$T/t.wav" > /dev/null
check "finds no tone on the right" "! '$BIN/ua101tonerate' measure '$T/t.wav' 2 > /dev/null"
python3 -I -c '
import sys
d = open(sys.argv[1], "rb").read(); mid = 68 + (len(d) - 68) // 16 * 8
open(sys.argv[2], "wb").write(d[:mid] + d[mid + 8:])' "$T/t.wav" "$T/lost.wav"
check "notices one lost sample" "'$BIN/ua101tonerate' measure '$T/lost.wav' 1 | grep -q 'phase jumped'"
check "no false alarm on a clean tone" "! '$BIN/ua101tonerate' measure '$T/t.wav' 1 | grep -q 'phase jumped'"
"$BIN/ua101tonerate" make "$T/slow.wav" --ppm 115 > /dev/null
"$BIN/ua101fixrate" "$T/slow.wav" "$T/fixed.wav" 47994.480632 > /dev/null
check "ua101fixrate brings +115 ppm back to 0" "python3 -c 'import sys; sys.exit(abs(float(\"$(ppm "$T/fixed.wav")\")) > 0.01)'"
echo "$fails failed"
[ "$fails" -eq 0 ]
