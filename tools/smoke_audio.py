#!/usr/bin/env python3
"""smoke-audio — Alpine's alsa-utils `aplay` through the kernel's ALSA ABI.

Opt-in (needs disk-alpine.img with alsa-utils, ports/alpine/prepare.py).
WAV files of known tones are generated on the host and written into a copy of
the image's Alpine root (/tmp) with debugfs; the guest then plays each one
with an unmodified Alpine `aplay` inside `chroot /disk/alpine`, and QEMU's
wav audiodev captures what the sound card emitted (build/smoke-audio/out.wav):

  1. 440 Hz, 44.1 kHz mono S16_LE,  `aplay a440.wav`   (the "default" device:
     alsa-lib's plug -> hw; the kernel takes the format and resamples)
  2. 880 Hz, 22.05 kHz stereo U8,   `aplay -D hw:0 b880.wav`
  3. 1320 Hz, 48 kHz stereo FLOAT_LE, `aplay -D hw:0 c1320.wav`
  4. 2000 Hz from MaeroOS's own `tone` through /dev/dsp (same drivers)

The capture must contain each tone, in order, at its frequency (DFT peak and
zero crossings), for about as long as it was played, without dropouts.  Also
checked: `aplay -l` lists card 0, and `aplay` exits 0 each time.

--ac97 runs the same on an AC'97 card instead of HDA.
"""
import math
import os
import selectors
import shutil
import signal
import struct
import subprocess
import sys

import smokelib
from smoke_hda import classify, peak_freq, read_wav, rms

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMG = os.path.abspath(os.environ.get("ALPINE_IMG", os.path.join(ROOT, "disk-alpine.img")))
OUT_DIR = os.path.join(ROOT, "build", "smoke-audio")
WORK = os.path.join(OUT_DIR, "disk.img")
WAV = os.path.join(OUT_DIR, "out.wav")
PROMPT = smokelib.PROMPT
ENV = "/usr/bin/env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin HOME=/root TERM=vt100"
CHROOT = "toybox chroot /disk/alpine " + ENV

# (file, Hz, seconds, rate, channels, WAV format tag, bits, aplay args)
TONES = [
    ("a440.wav", 440, 1.5, 44100, 1, 1, 16, ""),
    ("b880.wav", 880, 1.0, 22050, 2, 1, 8, "-D hw:0 "),
    ("c1320.wav", 1320, 1.0, 48000, 2, 3, 32, "-D hw:0 "),
]
# Then the native /dev/dsp path (MaeroOS `tone`), which shares the drivers.
DSP_TONE = ("tone 2000 500", 2000, 0.5)


def make_wav(path, hz, secs, rate, chans, tag, bits):
    n = int(rate * secs)
    frames = bytearray()
    for i in range(n):
        v = 0.6 * math.sin(2 * math.pi * hz * i / rate)
        if tag == 3:
            smp = struct.pack("<f", v)
        elif bits == 8:
            smp = bytes([int(128 + v * 127)])
        else:
            smp = struct.pack("<h", int(v * 32767))
        frames += smp * chans
    blockalign = chans * bits // 8
    fmt = struct.pack("<HHIIHH", tag, chans, rate, rate * blockalign, blockalign, bits)
    body = b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt
    if tag == 3:   # non-PCM formats carry a fact chunk
        body += b"fact" + struct.pack("<II", 4, n)
    body += b"data" + struct.pack("<I", len(frames)) + bytes(frames)
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", len(body)) + body)


def zero_cross_hz(x, rate):
    c = sum(1 for a, b in zip(x, x[1:]) if (a < 0) != (b < 0))
    return c / 2 / (len(x) / rate)


def main_run(labels, j, gap=3):
    """(first, last, count) of the longest stretch of blocks labelled j,
    bridging holes of up to `gap` blocks: a stray block at a tone's edge
    (a partial 10 ms block of the next tone or of its fade) is not part of
    another tone's span."""
    best, cur = None, None
    for i, lab in enumerate(labels + [None] * (gap + 1)):
        if lab == j:
            if cur and i - cur[1] <= gap + 1:
                cur = (cur[0], i, cur[2] + 1)
            else:
                cur = (i, i, 1)
            if not best or cur[2] > best[2]:
                best = cur
    return best


def main():
    ac97 = "--ac97" in sys.argv[1:]
    if not os.path.exists(IMG):
        raise SystemExit("smoke_audio: no disk-alpine.img - run ports/alpine/prepare.py")
    os.makedirs(OUT_DIR, exist_ok=True)
    shutil.copyfile(IMG, WORK)
    cmds = ["cd /alpine/tmp"]
    for name, hz, secs, rate, chans, tag, bits, _ in TONES:
        src = os.path.join(OUT_DIR, name)
        make_wav(src, hz, secs, rate, chans, tag, bits)
        cmds.append(f"write {src} {name}")
    cmdfile = os.path.join(OUT_DIR, "debugfs.cmd")
    with open(cmdfile, "w") as f:
        f.write("\n".join(cmds) + "\n")
    subprocess.run(["debugfs", "-w", "-f", cmdfile, WORK], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if os.path.exists(WAV):
        os.unlink(WAV)

    dev = (["-device", "AC97,audiodev=snd0"] if ac97 else
           ["-device", "intel-hda", "-device", "hda-duplex,audiodev=snd0"])
    accel = ["-accel", "kvm"] if os.access("/dev/kvm", os.R_OK | os.W_OK) else []
    cmd = ["qemu-system-i386", "-kernel", "kernel.elf", "-initrd", "initrd.tar",
           "-drive", f"file={WORK},format=raw,index=0,media=disk",
           "-serial", "stdio", "-m", "1024M", "-no-reboot", "-no-shutdown",
           "-audiodev", f"wav,id=snd0,path={WAV},out.frequency=48000,"
                        "out.channels=2,out.format=s16"] + dev + accel + smokelib.QEMU_DISPLAY
    proc = subprocess.Popen(cmd, cwd=ROOT, stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=0)
    sel = selectors.DefaultSelector()
    sel.register(proc.stdout, selectors.EVENT_READ)
    log = []

    def run(line, *needles, timeout=60.0):
        at = smokelib.mark(log)
        smokelib.send(proc, line + "\n")
        smokelib.wait_for(proc, sel, PROMPT, log, timeout, at)
        body = "".join(log)[at:]
        for n in needles:
            if n not in body:
                raise AssertionError(f"{line!r}: missing {n!r}")
        if "[SYSCALL] unimplemented" in body:
            raise AssertionError(f"{line!r}: unimplemented syscall")
        return body

    done = "[AC97] playback done" if ac97 else "[HDA] playback done"
    try:
        smokelib.login(proc, sel, log, timeout=90)
        if "[ALSA] card 0" not in "".join(log):
            raise AssertionError("no ALSA card at boot")
        run("ls /dev/snd", "controlC0", "pcmC0D0p")
        run(f"{CHROOT} aplay -l", "card 0: MaeroOS", "device 0:")
        for name, hz, secs, rate, chans, tag, bits, args in TONES:
            at = smokelib.mark(log)
            run(f"{CHROOT} /bin/sh -c 'aplay {args}/tmp/{name} && echo AP_\"\"OK'",
                "AP_OK", timeout=60)
            smokelib.wait_for(proc, sel, done, log, 10.0, at)
        at = smokelib.mark(log)
        run(DSP_TONE[0], "tone: done", timeout=30)
        smokelib.wait_for(proc, sel, done, log, 10.0, at)
        run("sleep 1")
    finally:
        proc.send_signal(signal.SIGTERM)       # QEMU finalises the WAV on exit
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()

    rate, _, s = read_wav(WAV)
    checks = [(t[0], t[1], t[2]) for t in TONES] + [("/dev/dsp " + DSP_TONE[0],) + DSP_TONE[1:]]
    labels = classify(s, rate, [c[1] for c in checks])
    blk = rate // 100
    print(f"\n[smoke-audio] wav: {len(s)} frames @ {rate} Hz, "
          f"{labels.count(None)} silent 10 ms blocks of {len(labels)}")
    prev_end = -1
    for j, (name, hz, secs) in enumerate(checks):
        run = main_run(labels, j)
        if not run:
            raise AssertionError(f"no {hz} Hz tone in the capture")
        first, last, n = run
        holes = (last - first + 1) - n
        dur = n / 100
        part = s[first * blk:(last + 1) * blk]
        f = peak_freq(part, rate)
        zc = zero_cross_hz(part, rate)
        print(f"[smoke-audio] {name}: {hz} Hz tone in blocks {first}-{last}, "
              f"peak {f} Hz, zero crossings {zc:.0f} Hz, {dur:.2f} s, "
              f"{holes} dropout blocks, rms {rms(part):.0f}")
        if first <= prev_end:
            raise AssertionError("tones overlap or play out of order")
        prev_end = last
        if abs(f - hz) > hz * 0.02 or abs(zc - hz) > hz * 0.03:
            raise AssertionError(f"{name}: peak {f} Hz / crossings {zc:.0f} Hz, expected {hz}")
        if not (secs - 0.1 <= dur <= secs + 0.1):
            raise AssertionError(f"{name}: lasted {dur:.2f} s, expected {secs}")
        if holes > 2:
            raise AssertionError(f"{name}: {holes} dropout blocks")
    print("[smoke-audio] PASS")


if __name__ == "__main__":
    try:
        main()
    except Exception as e:  # noqa: BLE001 — report and fail
        print(f"\n[smoke-audio] FAIL: {e}")
        sys.exit(1)
