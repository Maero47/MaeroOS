#!/usr/bin/env python3
"""bench-gfx — what a small animating region costs the desktop and maeroX.

Boots the desktop the way smoke-gui does (ISO + a copy of disk.img, -vga std,
no display), then runs userspace/gfxbench from the root shell on the serial
line, one case at a time:

  gui        libgui client redrawing a 446x174 region as fast as it can
  x          raw X client PutImage-ing a 446x174 region into a windowed maeroX
             (GetInputFocus round trip per frame, like XSync)
  gui-full   the libgui client redrawing its whole ~1200x670 surface
  x-full     the X client redrawing a whole 800x520 window
  gui-60, x-60, ...  the same, paced to 60 frames a second: the CPU shares
             are then the absolute cost of a 60 Hz animation

Each prints one GFXBENCH line: frames, fps, and the CPU split between the
desktop, maeroX and the client over the run (per-thread ticks from
/proc/processes).  The lines are collected into build/bench-gfx/results.txt.

  python3 tools/bench_gfx.py [--secs N] [--cases gui,x,...] [--label TEXT]

Uses KVM when /dev/kvm is usable, else TCG; SMOKE_GUI_ACCEL overrides.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

import smokelib
from smoke_gui import Console, Qmp, Image, prepare_disk, pick_accel, ROOT

OUT = os.path.join(ROOT, "build", "bench-gfx")


STATS = (r"\[desktop\] stats frames=(\d+) rows=(\d+) commits=(\d+) "
         r"present_kb=(\d+) render_ms=(\d+) fast_rows=(\d+)")


def desktop_stats(con):
    """The desktop's compositor counters, or None when it has none."""
    start = con.mark()
    con.run("echo stats > /tmp/wmctl")
    try:
        m = con.wait_re(STATS, timeout=3, start=start)
    except TimeoutError:
        return None
    return [int(m.group(i)) for i in range(1, 7)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--secs", type=int, default=10)
    ap.add_argument("--cases",
                    default="gui,x,gui-full,x-full,gui-60,x-60,x-full-60")
    ap.add_argument("--label", default="")
    ap.add_argument("--iso", default=os.path.join(ROOT, "maeros.iso"),
                    help="the ISO to boot (default maeros.iso), to compare kernels")
    args = ap.parse_args()

    os.makedirs(OUT, exist_ok=True)
    import smoke_gui
    smoke_gui.OUT = OUT                      # prepare_disk() copies into OUT
    disk = prepare_disk()
    sockdir = tempfile.mkdtemp(prefix="bgfx")
    qmp_path = os.path.join(sockdir, "qmp")
    accel = pick_accel()
    cmd = ["qemu-system-i386", "-cdrom", os.path.abspath(args.iso),
           "-drive", f"file={disk},format=raw,if=ide",
           "-accel", accel, "-vga", "std", *smokelib.QEMU_DISPLAY,
           "-serial", "stdio", "-m", "512M", "-no-reboot", "-no-shutdown",
           "-qmp", f"unix:{qmp_path},server=on,wait=off"]
    proc = subprocess.Popen(cmd, cwd=ROOT, stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            bufsize=0)
    con = Console(proc)
    qmp = None
    results = []
    try:
        qmp = Qmp(qmp_path)
        smokelib.login(con.proc, con.sel, con.log, timeout=120)
        con.wait_re(r"\[desktop\] ready fb=(\d+)x(\d+)", timeout=90, start=0)
        con.pump(3.0)                         # let the desktop go idle
        for case in args.cases.split(","):
            parts = case.split("-")
            mode = parts[0]
            size = "full" if "full" in parts else "small"
            fps = next((p for p in parts if p.isdigit()), "")
            line = f"/disk/gfxbench {mode} {args.secs} {size} {fps}".rstrip()
            s0 = desktop_stats(con)
            out = con.run(line, timeout=args.secs + 60)
            s1 = desktop_stats(con)
            m = re.search(r"GFXBENCH .*", out)
            if not m:
                raise AssertionError(f"{case}: no GFXBENCH line in {out!r}")
            res = m.group(0).strip()
            if s0 and s1:                     # desktops before this change
                d = [b - a for a, b in zip(s0, s1)]  # have no counters
                f = max(d[0], 1)
                res += (f" | desktop: frames={d[0]} rows/frame={d[1] // f}"
                        f" commits={d[2]} present={d[3] // f}KB/frame"
                        f" render={d[4] * 1000 // f}us/frame"
                        f" fast-rows={100 * d[5] // max(d[1], 1)}%")
            results.append(res)
            ppm = os.path.join(OUT, "screen.ppm")
            qmp.cmd("screendump", filename=ppm)
            Image.read_ppm(ppm).write_png(os.path.join(OUT, f"{case}.png"))
            os.remove(ppm)
            con.pump(2.0)
        hdr = f"# {time.strftime('%Y-%m-%d %H:%M')} accel={accel} {args.label}"
        with open(os.path.join(OUT, "results.txt"), "a") as f:
            f.write(hdr + "\n" + "\n".join(results) + "\n")
        print("\n[BENCH-GFX] " + hdr)
        for r in results:
            print("[BENCH-GFX] " + r)
        return 0
    finally:
        with open(os.path.join(OUT, "serial.log"), "w") as f:
            f.write(con.text())
        if qmp is not None:
            qmp.close()
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
        shutil.rmtree(sockdir, ignore_errors=True)


if __name__ == "__main__":
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"\n[BENCH-GFX] failed: {exc}", file=sys.stderr)
        raise SystemExit(1)
