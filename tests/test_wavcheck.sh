#!/bin/bash
# Checks ua101wavcheck on 192 kHz recordings with known answers. At that rate the capture
# has no monitor mix, so tones are only recorded when a cable brings an output back.
set -u
BIN=${1:-build}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fails=0
check() { if eval "$2"; then echo "  ok   $1"; else echo "  FAIL $1"; fails=$((fails + 1)); fi; }

# make OUT.wav ROUTES [drop]: 6 channels of quiet noise, two tones played, and ROUTES saying
# which capture channel records which output ("" for neither, "1:3" or "1:3,2:4"), 2 dB
# down as through a cable. "drop" removes one frame between the two tones.
make() {
    python3 -I - "$@" <<'EOF'
import array, math, random, struct, sys
path, routes = sys.argv[1], sys.argv[2]
drop = len(sys.argv) > 3
rate, channels, db, seconds, freqs = 192000, 6, -60.0, 0.25, (997.0, 1499.0)
starts = (28800, 134400)  # output frames where each tone starts
latency = 37              # output frame to recording frame
frames = int(1.1 * rate)
route = dict((int(a) - 1, int(b) - 1) for a, b in (r.split(":") for r in routes.split(",") if r))
length = int(seconds * rate + 0.5)
fade = int(0.02 * rate + 0.5)
amp = 10 ** (db / 20) * 10 ** (-2 / 20)
rng = random.Random(1)
data = array.array("i", (int(rng.gauss(0, 2.2e-5) * 8388607) * 256 for _ in range(frames * channels)))
def env(n):
    edge = min(n, length - 1 - n)
    return 1.0 if edge >= fade else 0.5 - 0.5 * math.cos(math.pi * (edge + 0.5) / fade)
for out, ch in route.items():
    for s in starts:
        for n in range(length):
            v = amp * env(n) * math.sin(2 * math.pi * freqs[out] * n / rate)
            data[(s + latency + n) * channels + ch] += round(v * 8388607) * 256
if drop:  # one frame lost halfway between the tones
    m = (starts[0] + length + starts[1]) // 2 * channels
    del data[m:m + channels]
raw = data.tobytes()
with open(path, "wb") as f:
    f.write(b"RIFF" + struct.pack("<I", 36 + len(raw)) + b"WAVEfmt " +
            struct.pack("<IHHIIHH", 16, 1, channels, rate, rate * channels * 4, channels * 4, 32) +
            b"data" + struct.pack("<I", len(raw)) + raw)
with open(path + ".tones", "w") as f:
    for s in starts:  # the guess is a little off, as it is in real runs
        f.write(f"{db} {seconds} {freqs[0]} {freqs[1]} {s} {s + 30}\n")
EOF
}

echo "ua101wavcheck at 192 kHz:"
make "$T/none.wav" ""
"$BIN/ua101wavcheck" "$T/none.wav" > "$T/none.txt"; code=$?
check "nothing recorded: no false NOT CLEAN" "[ $code -eq 0 ]"
check "nothing recorded: says the tones weren't checked" "grep -q 'WAV check: CLEAN, tones not checked' '$T/none.txt'"
check "nothing recorded: says so for both tones" "[ \$(grep -c 'Not recorded' '$T/none.txt') -eq 2 ]"

make "$T/one.wav" "1:3"
"$BIN/ua101wavcheck" "$T/one.wav" > "$T/one.txt"; code=$?
check "output 1 looped to ch 3: clean" "[ $code -eq 0 ] && grep -q 'WAV check: CLEAN (0 problems)' '$T/one.txt'"
check "output 1 looped to ch 3: found there" "grep -q 'Output 1 on capture ch 3' '$T/one.txt'"
check "output 1 looped to ch 3: output 2 skipped" "grep -q 'Output 2: not recorded' '$T/one.txt'"
check "output 1 looped to ch 3: offsets compared" "grep -q 'Same offset as tone 1' '$T/one.txt'"

make "$T/two.wav" "1:3,2:4"
"$BIN/ua101wavcheck" "$T/two.wav" > "$T/two.txt"; code=$?
check "both outputs looped: clean" "[ $code -eq 0 ] && grep -q 'WAV check: CLEAN (0 problems)' '$T/two.txt'"
check "both outputs looped: output 2 on ch 4" "grep -q 'Output 2 on capture ch 4' '$T/two.txt'"

make "$T/drop.wav" "1:3" drop
"$BIN/ua101wavcheck" "$T/drop.wav" > "$T/drop.txt"; code=$?
check "one frame lost: NOT CLEAN" "[ $code -eq 1 ] && grep -q 'WAV check: NOT CLEAN' '$T/drop.txt'"
check "one frame lost: offset changed by -1" "grep -q 'offset changed by -1 frames' '$T/drop.txt'"
echo "$fails failed"
[ "$fails" -eq 0 ]
