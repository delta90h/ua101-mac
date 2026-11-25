"""Makes a made-up jack map recording, as ua101stream --jack-map would with loop cables.

Usage: jackmap_wav.py OUT.wav [options]
  --rate HZ          48000 (default) or 192000
  --channels N       capture channels: 12 (default; the last two are the monitor mix) or 6
  --outputs N        playback channels playing a tone: 10 (default) or 6
  --outmap LIST      which OUTPUT jack each playback channel reaches (default 1 2 3 ...)
  --inmap LIST       which capture channel each INPUT jack is recorded on (default 1 2 3 ...)
  --cables LIST      which INPUT jack each OUTPUT jack is cabled to, or "straight" (the
                     default) or "shifted" (OUTPUT n to INPUT n+1, the last to INPUT 1)
  --gain JACK:DB     extra gain on one INPUT jack, as a SENS knob turned up; repeatable
  --extra P:CH       playback P also reaches capture channel CH, as through a Y cable

Only the analog jacks are cabled: as many as there are, up to 8. The rest of the playback
channels (the optical output) reach nothing.
"""
import argparse
import array
import math
import random
import struct

FREQS = (401.0, 523.0, 661.0, 829.0, 937.0, 1103.0, 1259.0, 1433.0, 1601.0, 1789.0)

ap = argparse.ArgumentParser()
ap.add_argument("path")
ap.add_argument("--rate", type=int, default=48000)
ap.add_argument("--channels", type=int, default=12)
ap.add_argument("--outputs", type=int, default=10)
ap.add_argument("--outmap")
ap.add_argument("--inmap")
ap.add_argument("--cables", default="straight")
ap.add_argument("--gain", action="append", default=[])
ap.add_argument("--extra", action="append", default=[])
a = ap.parse_args()

mix = a.channels == 12
jacks = min(8, a.channels - (2 if mix else 0), a.outputs)
ident = list(range(1, jacks + 1))
outmap = [int(x) for x in a.outmap.split()] if a.outmap else ident
inmap = [int(x) for x in a.inmap.split()] if a.inmap else ident
if a.cables == "straight":
    cables = ident
elif a.cables == "shifted":
    cables = [j % jacks + 1 for j in ident]
else:
    cables = [int(x) for x in a.cables.split()]
gain = {int(j): float(d) for j, d in (g.split(":") for g in a.gain)}

# playback channel (from 1) -> list of (capture channel from 1, gain dB)
routes = {}
for p in range(1, jacks + 1):
    jack_in = cables[outmap[p - 1] - 1]
    routes.setdefault(p, []).append((inmap[jack_in - 1], gain.get(jack_in, 0.0)))
for e in a.extra:
    p, ch = (int(x) for x in e.split(":"))
    routes.setdefault(p, []).append((ch, 0.0))

db, seconds, out_start, latency = -60.0, 0.6, int(0.2 * a.rate), 60
frames = int(1.0 * a.rate)
length = int(seconds * a.rate + 0.5)
fade = int(0.02 * a.rate + 0.5)
amp = 10 ** (db / 20)


def env(n):
    edge = min(n, length - 1 - n)
    return 1.0 if edge >= fade else 0.5 - 0.5 * math.cos(math.pi * (edge + 0.5) / fade)


rng = random.Random(7)
data = array.array("d", (rng.gauss(0, 1e-5) for _ in range(frames * a.channels)))
for p, dests in routes.items():
    wave = [amp * env(n) * math.sin(2 * math.pi * FREQS[p - 1] * n / a.rate) for n in range(length)]
    for ch, g in dests:
        k = 10 ** (g / 20)
        for n, v in enumerate(wave):
            data[(out_start + latency + n) * a.channels + ch - 1] += v * k
if mix:  # the monitor mix: odd inputs and playback 1 on the left, even and playback 2 right
    c = a.channels
    for f in range(frames):
        row = f * c
        for side in (0, 1):
            s = sum(data[row + ch] for ch in range(side, 10, 2))
            n = f - out_start
            if 0 <= n < length:
                s += amp * env(n) * math.sin(2 * math.pi * FREQS[side] * n / a.rate)
            data[row + 10 + side] = s

ints = array.array("i", (max(-8388608, min(8388607, round(v * 8388607))) * 256 for v in data))
raw = ints.tobytes()
with open(a.path, "wb") as f:
    f.write(b"RIFF" + struct.pack("<I", 36 + len(raw)) + b"WAVEfmt " +
            struct.pack("<IHHIIHH", 16, 1, a.channels, a.rate, a.rate * a.channels * 4,
                        a.channels * 4, 32) + b"data" + struct.pack("<I", len(raw)) + raw)
with open(a.path + ".jack", "w") as f:
    f.write(f"{db:.3f} {seconds:.3f} {out_start} {out_start} {a.outputs} " +
            " ".join(f"{x:.3f}" for x in FREQS[:a.outputs]) + "\n")
