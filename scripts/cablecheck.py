#!/usr/bin/env python3
"""Checks the cable test's result. Usage: cablecheck.py RESULTS_DIR WAV_DIR

Three clocks are measured separately:
  the unit against the host clock, from the stream tool's clock log (clock.csv);
  the Mac's audio output against the host clock, from Core Audio's timestamps (mac.log);
  the Mac's output against the unit, from the tone's frequency in the recording.
The third must follow from the first two. Then the recording is rate corrected
with the first figure, and the tone must come out at the Mac output's rate.
"""
import os, re, subprocess, sys

TOLERANCE_PPM = 1.0
SKIP_SECONDS = 10
CHANNEL = 3  # rear INPUT 3

res, wavs = sys.argv[1], sys.argv[2]
build = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build")


def fit(xs, ys):
    n = len(xs)
    mx, my = sum(xs) / n, sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    sxy = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    b = sxy / sxx
    return my - b * mx, b


def tone(path):
    out = subprocess.run([os.path.join(build, "ua101tonerate"), "measure", path, str(CHANNEL)],
                         capture_output=True, text=True).stdout
    m = re.search(r"Frequency [0-9.]+ Hz: ([+-][0-9.]+) ppm", out)
    level = re.search(r"Level (-?[0-9.]+) dBFS", out)
    return (float(m.group(1)) if m else None, "phase jumped" in out,
            float(level.group(1)) if level else None, out)


rows = [l.strip().split(",") for l in open(os.path.join(res, "clock.csv"))][1:]
rows = [[float(v) for v in r] for r in rows if len(r) == 4]
rows = [r for r in rows if r[3] >= SKIP_SECONDS]
a, b = fit([r[2] for r in rows], [r[3] for r in rows])   # host seconds from bus frames
c, unit_hz = fit([a + b * r[0] for r in rows], [r[1] for r in rows])
p_unit = (unit_hz / 48000.0 - 1.0) * 1e6

mac = open(os.path.join(res, "mac.log")).read()
m = re.search(r"Mac output against host: ([0-9.]+) Hz \(([+-][0-9.]+) ppm\)", mac)
if not m:
    print("No Mac output rate in mac.log:\n" + mac)
    sys.exit(1)
p_mac = float(m.group(2))

raw = os.path.join(wavs, "capture.wav")
fixed = os.path.join(wavs, "fixed.wav")
p_raw, jumped_raw, level, raw_out = tone(raw)
# ua101fixrate never overwrites, so an old result is removed first.
if os.path.exists(fixed):
    os.remove(fixed)
done = subprocess.run([os.path.join(build, "ua101fixrate"), raw, fixed, "%.6f" % unit_hz],
                      capture_output=True, text=True)
if done.returncode != 0:
    print("ua101fixrate failed:\n" + done.stdout + done.stderr)
    sys.exit(1)
p_fixed, jumped_fixed, _, fixed_out = tone(fixed)

print("Unit against host:        %+9.3f ppm (%.4f Hz, from %d clock readings)" % (p_unit, unit_hz, len(rows)))
print("Mac output against host:  %+9.3f ppm" % p_mac)
if p_raw is None or p_fixed is None:
    print("No tone found on input %d:\n%s" % (CHANNEL, raw_out))
    sys.exit(1)
predicted_raw = ((1 + p_mac * 1e-6) / (1 + p_unit * 1e-6) - 1) * 1e6
print("Tone level on input %d:    %.1f dBFS" % (CHANNEL, level))
print("Tone in the recording:    %+9.3f ppm; predicted from the two above %+9.3f; off by %.3f"
      % (p_raw, predicted_raw, p_raw - predicted_raw))
print("Tone after correction:    %+9.3f ppm; should equal the Mac output's %+9.3f; off by %.3f"
      % (p_fixed, p_mac, p_fixed - p_mac))
ok = (abs(p_raw - predicted_raw) <= TOLERANCE_PPM and abs(p_fixed - p_mac) <= TOLERANCE_PPM
      and not jumped_raw and not jumped_fixed)
if jumped_raw or jumped_fixed:
    print("Samples were lost or added during the tone, so this run can't be judged.")
print("\n=== Cable test: %s (tolerance %.1f ppm) ===" % ("PASS" if ok else "FAIL", TOLERANCE_PPM))
sys.exit(0 if ok else 1)
