#!/usr/bin/env python3
"""smoke-usbaudio — USB Audio Class playback through xHCI isochronous
transfers, verified from the audio, plus the ALSA mixer.

Boots (2 CPUs) with an HDA controller (ALSA card 0, its output to a null
audiodev) and a qemu-xhci with QEMU's usb-audio (UAC1, full speed,
isochronous OUT, 48 kHz S16 stereo), whose output goes to QEMU's wav
audiodev (build/smoke-usbaudio/out.wav).  Then, from the serial shell:
  1. the kernel attached the device as ALSA card 1 (/dev/snd/controlC1,
     pcmC1D0p) with a Feature Unit volume;
  2. `mixer -c 1` / `mixer -c 0` read the Master volume of both cards, and
     `mixer -c 0 40` / `mute` set card 0's (HDA) and read it back;
  3. `tone -c 1 440 2000` plays 2 s of 440 Hz on card 1 through the raw
     ALSA PCM ioctls (the kernel's ALSA layer -> usb_audio.c -> isochronous
     TDs, one per 1 ms frame);
  4. `tone -c 1 1000 1000 30`: 1 kHz at 30 % of the USB card's Master
     volume (a SET_CUR to its Feature Unit);
  5. `tone -d /dev/dsp1 1500 700`: the raw OSS-style node of card 1;
  6. hot-unplug while playing: `tone -c 1 660 6000` in the background,
     QMP device_del of the usb-audio after ~1.5 s: the card leaves /dev/snd,
     tone fails with an error instead of hanging, no panic, the shell works;
  7. plugged back in (QMP device_add): card 1 again, `tone -c 1 2000 800`
     plays.
The captured WAV must hold the 440 Hz tone for about 2 s with at most a
few dropout blocks, the quieter 1 kHz tone, the 1.5 kHz one from /dev/dsp1,
part of the 660 Hz one, and the
2 kHz one from the replugged device.

Usage: python3 tools/smoke_usbaudio.py  (make smoke-usbaudio)
"""
import json
import os
import selectors
import signal
import socket
import subprocess
import sys
import time

import smokelib
from smoke_hda import read_wav, classify, peak_freq, rms

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT, "build", "smoke-usbaudio")
WAV = os.path.join(OUT_DIR, "out.wav")
# the replugged device gets its own capture: QEMU's wav backend starts its
# file afresh for every new voice
WAV2 = os.path.join(OUT_DIR, "replug.wav")
QMP = os.path.join(OUT_DIR, "qmp.sock")
PROMPT = smokelib.PROMPT


class Qmp:
    def __init__(self, path, timeout=20.0):
        deadline = time.time() + timeout
        while True:
            try:
                self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self.sock.connect(path)
                break
            except OSError:
                self.sock.close()
                if time.time() >= deadline:
                    raise
                time.sleep(0.1)
        self.file = self.sock.makefile("rw")
        self.file.readline()
        self.cmd("qmp_capabilities")

    def cmd(self, name, **args):
        self.file.write(json.dumps({"execute": name, "arguments": args}) + "\n")
        self.file.flush()
        while True:
            reply = json.loads(self.file.readline())
            if "return" in reply:
                return reply["return"]
            if "error" in reply:
                raise RuntimeError(f"QMP {name}: {reply['error']}")


def run(proc, sel, log, line, timeout=30.0):
    at = smokelib.mark(log)
    smokelib.send(proc, line + "\n")
    smokelib.wait_for(proc, sel, PROMPT, log, timeout, at)
    return "".join(log)[at:]


def expect(out, text, what):
    if text not in out:
        raise AssertionError(f"{what}: {text!r} not in:\n{out[-1500:]}")


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    for f in (WAV, WAV2, QMP):
        if os.path.exists(f):
            os.unlink(f)
    cmd = ["qemu-system-i386"] + smokelib.QEMU_DISPLAY + [
        "-kernel", "kernel.elf", "-initrd", "initrd.tar",
        "-serial", "stdio", "-m", "256M", "-no-reboot", "-no-shutdown",
        "-smp", "2", "-qmp", f"unix:{QMP},server=on,wait=off",
        "-audiodev", "none,id=hdanull",
        "-audiodev", f"wav,id=snd0,path={WAV},out.frequency=48000,"
                     "out.channels=2,out.format=s16",
        "-audiodev", f"wav,id=snd1,path={WAV2},out.frequency=48000,"
                     "out.channels=2,out.format=s16",
        "-device", "intel-hda", "-device", "hda-duplex,audiodev=hdanull",
        "-device", "qemu-xhci,id=xhci",
        "-device", "usb-audio,id=uaudio,audiodev=snd0,bus=xhci.0,port=1",
    ]
    proc = subprocess.Popen(cmd, cwd=ROOT, stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            bufsize=0)
    sel = selectors.DefaultSelector()
    sel.register(proc.stdout, selectors.EVENT_READ)
    log = []
    try:
        smokelib.login(proc, sel, log, timeout=90.0)
        qmp = Qmp(QMP)
        boot = "".join(log)
        expect(boot, "[ALSA] card 0: HDA", "HDA card")
        expect(boot, "[USB-AUDIO] slot", "USB audio attach")
        expect(boot, "[ALSA] card 1: USB Audio", "USB card registration")
        for line in boot.splitlines():
            if "[USB-AUDIO]" in line or "[ALSA] card" in line:
                print("  " + line.strip())

        out = run(proc, sel, log, "ls /dev/snd")
        for n in ("controlC0", "pcmC0D0p", "controlC1", "pcmC1D0p"):
            expect(out, n, "/dev/snd listing")

        # mixer: both cards readable; card 0 (HDA) set and read back
        out = run(proc, sel, log, "mixer -c 1")
        expect(out, "card 1 Master:", "USB card mixer")
        out = run(proc, sel, log, "mixer -c 0 40")
        expect(out, "card 0 Master: 40% [on]", "HDA volume set")
        out = run(proc, sel, log, "mixer -c 0 mute")
        expect(out, "card 0 Master: 40% [off]", "HDA mute")
        out = run(proc, sel, log, "mixer -c 0 unmute")
        out = run(proc, sel, log, "mixer -c 0 100")
        expect(out, "card 0 Master: 100% [on]", "HDA volume restore")

        at = smokelib.mark(log)
        out = run(proc, sel, log, "tone -c 1 440 2000", timeout=40.0)
        expect(out, "tone: done", "440 Hz tone")
        out = run(proc, sel, log, "tone -c 1 1000 1000 30", timeout=40.0)
        expect(out, "tone: done", "1 kHz tone at 30 %")
        out = run(proc, sel, log, "mixer -c 1")
        expect(out, "card 1 Master: 30%", "USB card volume read back")
        out = run(proc, sel, log, "mixer -c 1 100")
        out = run(proc, sel, log, "ls /dev")
        expect(out, "dsp1", "/dev/dsp1 listing")
        out = run(proc, sel, log, "tone -d /dev/dsp1 1500 700", timeout=40.0)
        expect(out, "tone: done", "1.5 kHz tone on /dev/dsp1")
        stats = "".join(log)[at:]
        for line in stats.splitlines():
            if "[USB-AUDIO] stream" in line:
                print("  " + line.strip())

        # hot-unplug in the middle of a tone
        at = smokelib.mark(log)
        smokelib.send(proc, "tone -c 1 660 6000; echo tone-exit-$?\n")
        time.sleep(2.5)
        qmp.cmd("device_del", id="uaudio")
        smokelib.wait_for(proc, sel, "[ALSA] card 1 removed", log, 20.0, at)
        smokelib.wait_for(proc, sel, "tone-exit-", log, 20.0, at)
        smokelib.wait_for(proc, sel, PROMPT, log, 20.0, at)
        out = "".join(log)[at:]
        expect(out, "tone-exit-1", "tone after unplug")
        if "panic" in out.lower() or "PANIC" in out:
            raise AssertionError("panic after unplug:\n" + out[-2000:])
        for line in out.splitlines():
            if "[USB-AUDIO]" in line or "tone:" in line:
                print("  " + line.strip())
        out = run(proc, sel, log, "ls /dev/snd")
        if "pcmC1D0p" in out or "pcmC0D0p" not in out:
            raise AssertionError("/dev/snd after unplug:\n" + out)
        out = run(proc, sel, log, "ls /dev")
        if "dsp1" in out:
            raise AssertionError("/dev/dsp1 still listed after unplug")

        # plugged back in
        at = smokelib.mark(log)
        qmp.cmd("device_add", driver="usb-audio", id="uaudio2",
                audiodev="snd1", bus="xhci.0", port="1")
        smokelib.wait_for(proc, sel, "[ALSA] card 1: USB Audio", log, 30.0,
                          at)
        time.sleep(0.5)
        out = run(proc, sel, log, "tone -c 1 2000 800", timeout=40.0)
        expect(out, "tone: done", "2 kHz tone after replug")
        run(proc, sel, log, "sleep 1")
    finally:
        proc.send_signal(signal.SIGTERM)       # QEMU finalises the WAV on exit
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        with open(os.path.join(OUT_DIR, "serial.log"), "w") as f:
            f.write("".join(log))

    levels = {}
    check_wav(WAV, [(440, 2.0, 4), (1000, 1.0, 3), (1500, 0.7, 3),
                    (660, None, None)], levels)
    check_wav(WAV2, [(2000, 0.8, 3)], levels)
    if levels[440] < 3000:
        raise AssertionError(f"440 Hz tone too quiet (rms {levels[440]:.0f})")
    if levels[1000] > levels[440] * 0.9:
        raise AssertionError("the 30 % tone is not quieter")
    print("[smoke-usbaudio] PASS")


def check_wav(path, tones, levels):
    rate, _, s = read_wav(path)
    freqs = [hz for hz, _, _ in tones]
    labels = classify(s, rate, freqs)
    blk = rate // 100
    print(f"\n[smoke-usbaudio] {os.path.basename(path)}: {len(s)} frames @ "
          f"{rate} Hz, {labels.count(None)} silent 10 ms blocks of "
          f"{len(labels)}")
    prev_end = -1
    for j, (hz, want_s, holes_max) in enumerate(tones):
        idx = [i for i, lab in enumerate(labels) if lab == j]
        if not idx:
            raise AssertionError(f"no {hz} Hz tone in the capture")
        first, last = idx[0], idx[-1]
        holes = (last - first + 1) - len(idx)
        dur = len(idx) / 100
        part = s[first * blk:(last + 1) * blk]
        f = peak_freq(part, rate)
        levels[hz] = rms(part)
        print(f"[smoke-usbaudio] tone {hz} Hz: blocks {first}-{last}, peak "
              f"{f} Hz, {dur:.2f} s, {holes} dropout blocks, rms "
              f"{levels[hz]:.0f}")
        if first <= prev_end:
            raise AssertionError("tones overlap or play out of order")
        prev_end = last
        if abs(f - hz) > hz * 0.02:
            raise AssertionError(f"peak at {f} Hz, expected {hz}")
        if want_s is None:
            # cut off by the unplug: some of it, not all six seconds
            if not 0.5 <= dur <= 5.0:
                raise AssertionError(f"660 Hz (unplugged) lasted {dur:.2f} s")
            continue
        if not (want_s - 0.1 <= dur <= want_s + 0.15):
            raise AssertionError(f"tone lasted {dur:.2f} s, expected {want_s}")
        if holes > holes_max:
            raise AssertionError(f"{holes} dropout blocks inside the tone")


if __name__ == "__main__":
    try:
        main()
    except Exception as e:  # noqa: BLE001 — report and fail
        print(f"\n[smoke-usbaudio] FAIL: {e}")
        sys.exit(1)
