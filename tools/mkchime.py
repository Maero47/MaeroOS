#!/usr/bin/env python3
"""Generate the MaeroOS startup chime: a soft rising three-note arpeggio,
48 kHz S16LE stereo WAV (matches the AC97 driver's fixed format)."""
import math
import os
import struct
import wave

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "testfiles", "chime.wav")

RATE = 48000
NOTES = [(523.25, 0.0, 0.55), (659.25, 0.18, 0.55), (783.99, 0.36, 0.9)]
TOTAL = 1.5

frames = bytearray()
n_samples = int(RATE * TOTAL)
for i in range(n_samples):
    t = i / RATE
    v = 0.0
    for freq, start, dur in NOTES:
        if start <= t < start + dur:
            lt = t - start
            env = min(lt / 0.02, 1.0) * math.exp(-3.0 * lt / dur)
            v += 0.28 * env * math.sin(2 * math.pi * freq * lt)
            v += 0.10 * env * math.sin(4 * math.pi * freq * lt)
    s = max(-1.0, min(1.0, v))
    pcm = int(s * 32767)
    frames += struct.pack("<hh", pcm, pcm)

with wave.open(OUT, "wb") as w:
    w.setnchannels(2)
    w.setsampwidth(2)
    w.setframerate(RATE)
    w.writeframes(bytes(frames))
print(f"{OUT}: {len(frames)} PCM bytes, {TOTAL}s")
