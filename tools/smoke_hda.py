#!/usr/bin/env python3
"""smoke-hda — Intel HDA playback through /dev/dsp, verified from the audio.

Boots (2 CPUs) with an ICH6 HDA controller + hda-duplex codec whose output goes to
QEMU's wav audiodev (build/smoke-hda/out.wav), then plays two tones:
  1. `tone 1000 1500`     — 1 kHz for 1.5 s at full volume
  2. `tone 2500 1000 25`  — 2.5 kHz for 1 s at 25 % mixer volume
and checks the captured WAV: each tone is present (not silence), its
spectral peak sits at the played frequency, it lasts about as long as was
played without dropouts (1.5 s is more than two trips round the driver's
128 KiB cyclic DMA buffer), and the 25 % tone is clearly quieter.

SMOKE_HDA_DEVICES overrides the device flags (the last one gets the
audiodev), e.g. "-M q35 -device ich9-intel-hda -device hda-micro".
"""
import math
import os
import selectors
import signal
import struct
import subprocess
import sys

import smokelib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT, "build", "smoke-hda")
WAV = os.path.join(OUT_DIR, "out.wav")
PROMPT = smokelib.PROMPT


def run(proc, sel, log, line, timeout=20.0):
    at = smokelib.mark(log)
    smokelib.send(proc, line + "\n")
    smokelib.wait_for(proc, sel, PROMPT, log, timeout, at)
    return "".join(log)[at:]


def read_wav(path):
    """(rate, channels, mono int samples).  QEMU fills the RIFF sizes in on
    exit; if it did not get that far, trust the file length instead."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise AssertionError("captured file is not a WAV")
    pos, rate, chans, bits, pcm = 12, 0, 0, 0, b""
    while pos + 8 <= len(data):
        cid, size = data[pos:pos + 4], struct.unpack("<I", data[pos + 4:pos + 8])[0]
        body = pos + 8
        if cid == b"fmt ":
            _, chans, rate, _, _, bits = struct.unpack("<HHIIHH", data[body:body + 16])
        elif cid == b"data":
            end = len(data) if size == 0 or body + size > len(data) else body + size
            pcm = data[body:end]
            break
        pos = body + size + (size & 1)
    if bits != 16 or not rate:
        raise AssertionError(f"unexpected WAV format: {bits} bit, {rate} Hz")
    n = len(pcm) // (2 * chans)
    raw = struct.unpack(f"<{n * chans}h", pcm[:n * chans * 2])
    return rate, chans, raw[0::chans]


def goertzel(x, rate, f):
    k = 2 * math.cos(2 * math.pi * f / rate)
    s1 = s2 = 0.0
    for v in x:
        s1, s2 = v + k * s1 - s2, s1
    return s1 * s1 + s2 * s2 - k * s1 * s2


def classify(samples, rate, freqs, thresh=300):
    """Label each 10 ms block with the index of the strongest of `freqs`,
    or None when the block is silent (rms below `thresh`).  QEMU's wav
    backend records only while the codec stream runs, so the two tones end
    up back to back and are told apart by frequency, not by a gap."""
    blk = rate // 100
    out = []
    for i in range(0, len(samples) - blk + 1, blk):
        x = samples[i:i + blk]
        if rms(x) < thresh:
            out.append(None)
        else:
            out.append(max(range(len(freqs)),
                           key=lambda j: goertzel(x, rate, freqs[j])))
    return out


def peak_freq(samples, rate, lo=100, hi=8000, step=5):
    """Strongest frequency in [lo, hi] over a Hann-windowed 8192-sample slice
    (plain DFT at each probe frequency; no numpy here)."""
    n = min(8192, len(samples))
    mid = len(samples) // 2
    x = samples[mid - n // 2: mid - n // 2 + n]
    win = [0.5 - 0.5 * math.cos(2 * math.pi * i / (n - 1)) for i in range(n)]
    xw = [a * b for a, b in zip(x, win)]

    def power(f):
        return goertzel(xw, rate, f)

    best = max(range(lo, hi + 1, step * 10), key=power)       # coarse
    return max(range(best - step * 10, best + step * 10 + 1, 1), key=power)


def rms(samples):
    return math.sqrt(sum(s * s for s in samples) / max(1, len(samples)))


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    if os.path.exists(WAV):
        os.unlink(WAV)
    cmd = ["qemu-system-i386"] + smokelib.QEMU_DISPLAY + [
        "-kernel", "kernel.elf", "-initrd", "initrd.tar",
        "-serial", "stdio", "-m", "256M", "-no-reboot", "-no-shutdown",
        "-audiodev", f"wav,id=snd0,path={WAV},out.frequency=48000,"
                     "out.channels=2,out.format=s16",
        ] + os.environ.get("SMOKE_HDA_DEVICES",
                           "-smp 2 -device intel-hda -device hda-duplex").split()
    cmd[-1] += ",audiodev=snd0"
    proc = subprocess.Popen(cmd, cwd=ROOT, stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            bufsize=0)
    sel = selectors.DefaultSelector()
    sel.register(proc.stdout, selectors.EVENT_READ)
    log = []
    try:
        smokelib.login(proc, sel, log, timeout=60.0)
        boot = "".join(log)
        if "[HDA] up:" not in boot:
            raise AssertionError("HDA driver did not come up")
        at = smokelib.mark(log)
        out = run(proc, sel, log, "tone 1000 1500", timeout=30.0)
        if "tone: done" not in out:
            raise AssertionError("tone 1000 failed")
        smokelib.wait_for(proc, sel, "[HDA] playback done", log, 10.0, at)
        at = smokelib.mark(log)
        out = run(proc, sel, log, "tone 2500 1000 25", timeout=30.0)
        if "tone: done" not in out or "cannot set volume" in out:
            raise AssertionError("tone 2500 at 25% failed")
        smokelib.wait_for(proc, sel, "[HDA] playback done", log, 10.0, at)
        # let the wav backend catch up with the last silence
        run(proc, sel, log, "sleep 1")
    finally:
        proc.send_signal(signal.SIGTERM)       # QEMU finalises the WAV on exit
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()

    rate, _, s = read_wav(WAV)
    checks = [(1000, 1.5), (2500, 1.0)]
    labels = classify(s, rate, [hz for hz, _ in checks])
    blk = rate // 100
    print(f"\n[smoke-hda] wav: {len(s)} frames @ {rate} Hz, "
          f"{labels.count(None)} silent 10 ms blocks of {len(labels)}")
    levels = []
    prev_end = -1
    for j, (want_hz, want_s) in enumerate(checks):
        idx = [i for i, l in enumerate(labels) if l == j]
        if not idx:
            raise AssertionError(f"no {want_hz} Hz tone in the capture")
        first, last = idx[0], idx[-1]
        holes = (last - first + 1) - len(idx)
        dur = len(idx) / 100
        part = s[first * blk:(last + 1) * blk]
        f = peak_freq(part, rate)
        lvl = rms(part)
        levels.append(lvl)
        print(f"[smoke-hda] tone {want_hz} Hz: blocks {first}-{last}, "
              f"peak {f} Hz, {dur:.2f} s, {holes} dropout blocks, "
              f"rms {lvl:.0f}")
        if first <= prev_end:
            raise AssertionError("tones overlap or play out of order")
        prev_end = last
        if abs(f - want_hz) > want_hz * 0.02:
            raise AssertionError(f"peak at {f} Hz, expected {want_hz}")
        if not (want_s - 0.1 <= dur <= want_s + 0.1):
            raise AssertionError(f"tone lasted {dur:.2f} s, expected {want_s}")
        if holes > 2:
            raise AssertionError(f"{holes} dropout blocks inside the tone")
    if levels[0] < 3000:
        raise AssertionError(f"full-volume tone too quiet (rms {levels[0]:.0f})")
    if levels[1] > levels[0] * 0.7:
        raise AssertionError("25% volume tone is not quieter")
    print("[smoke-hda] PASS")


if __name__ == "__main__":
    try:
        main()
    except Exception as e:  # noqa: BLE001 — report and fail
        print(f"\n[smoke-hda] FAIL: {e}")
        sys.exit(1)
