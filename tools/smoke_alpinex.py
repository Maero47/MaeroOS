#!/usr/bin/env python3
"""smoke-alpinex — Alpine Linux X11 applications on maeroX (opt-in).

Boots the ISO with a sparse copy of disk-alpinex.img (ports/alpine/prepare.py
with ALPINE_X=1: the Alpine root, an offline repo of X11/GTK apps and the
set-uid helper /disk/xapp) and drives the desktop through QMP like
smoke_gui.py:

  1. Linux Apps (the desktop's front end) installs xterm with a click;
     /disk/xapp installs xeyes and mousepad (GTK 3) from the serial shell.
     apk verifies the offline repo's signatures inside the chroot.
  2. Each app is launched through the desktop (`wmctl launch <name>`, the
     launcher entry Linux Apps wrote), which runs `/disk/xapp <slot> <name>`:
     maeroX starts in that slot with the first app, the app runs in the
     chroot as the desktop user.  maeroX's log (/tmp/maerox.log) must report
     the app's toplevel, and its area on a screendump must hold real content
     (many colours), not a blank rectangle.
  3. xterm: a command typed into it writes a file (keyboard path).
     mousepad: text typed into it, Ctrl+S, a file name typed into the GTK
     save dialog, Enter: the file must hold the text.
  4. Each app is closed with the maeroX title bar's close button
     (WM_DELETE_WINDOW) and must exit; maeroX must still be running.

Output in build/smoke-alpinex/: serial.log, NN-step.png, maerox.log and the
apps' own logs.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import smokelib  # noqa: E402
import smoke_gui  # noqa: E402
from smoke_gui import Console, Qmp, Input, Image, distinct_colors, key  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
OUT = os.path.join(ROOT, "build", "smoke-alpinex")
IMG = os.path.join(ROOT, "disk-alpinex.img")
smoke_gui.OUT = OUT

BODY_X, BODY_Y = 1, 28           # libgui window body offset


class AlpineX:
    def __init__(self, con, qmp):
        self.con, self.qmp, self.inp = con, qmp, Input(qmp)
        self.g = smoke_gui.GuiSmoke(con, qmp)
        self.maerox = None        # the maeroX desktop window
        self.notes = []

    def sh(self, cmd, timeout=30.0):
        return self.con.run(cmd, timeout)

    def maerox_log(self):
        return self.sh("cat /tmp/maerox.log")

    def wait_log(self, pattern, timeout=90.0):
        deadline = time.time() + timeout
        while True:
            m = re.search(pattern, self.maerox_log())
            if m:
                return m
            if time.time() >= deadline:
                raise AssertionError(f"maeroX log never showed {pattern!r}")
            self.g.settle(1.0)

    def install_cli(self, name):
        out = self.sh(f"/disk/xapp install {name}", timeout=900)
        if f"{name} installed" not in out:
            raise AssertionError(f"xapp install {name} failed:\n{out[-800:]}")

    def install_frontend(self, name):
        start = self.con.mark()
        self.sh("wmctl launch linuxapps")
        win = self.g.wait_window("Linux Apps", start=start)
        self.con.wait_re(r"\[linuxapps\] ready root=1", start=start)
        m = self.con.wait_re(r"\[linuxapps\] row %s available button=(\d+),(\d+)" % name,
                             start=start)
        self.g.settle(1.0)
        x, y = int(m.group(1)), int(m.group(2))
        self.g.click(win["x"] + BODY_X + x, win["y"] + BODY_Y + y)
        self.con.wait_re(r"\[linuxapps\] installed %s exit=0" % name, timeout=900)
        self.g.shot(f"linuxapps-{name}-installed")
        self.g.close(win)

    def launch(self, name, title_re):
        """Launch through the desktop; returns the toplevel's screen rect."""
        start = self.con.mark()
        self.sh(f"wmctl launch {name}")
        if self.maerox is None:
            self.maerox = self.g.wait_window("maeroX :0", timeout=60, start=start)
        m = self.wait_log(r"map toplevel 0x([0-9a-f]+) client=(\d+) (\d+)x(\d+) "
                          r"@(-?\d+),(-?\d+) title='%s" % title_re)
        w, h, x, y = (int(m.group(i)) for i in (3, 4, 5, 6))
        self.g.settle(3.0)
        mx = self.maerox["x"] + BODY_X
        my = self.maerox["y"] + BODY_Y
        rect = (mx + x, my + y, w, h)
        img = self.g.shot(f"{name}-running")
        vis = (rect[0], rect[1], min(w, self.maerox["w"] - x - 2),
               min(h, self.maerox["h"] - y - 30))
        colors = distinct_colors(img, vis)
        print(f"\n[SMOKE-ALPINEX] {name}: toplevel {w}x{h} at {x},{y}, "
              f"{colors} colours on screen")
        if colors < 3:
            raise AssertionError(f"{name}: its window shows {colors} colour(s)")
        return {"rect": rect, "x": x, "y": y, "w": w, "h": h, "img": img,
                "id": m.group(1)}

    def click_in(self, app, dx, dy):
        x, y = app["rect"][0] + dx, app["rect"][1] + dy
        self.inp.click(x, y)
        self.g.settle(0.5)

    def close(self, name, app, proc):
        """The maeroX frame's close box: right end of its title bar."""
        x = app["rect"][0] + app["w"] - 11
        y = app["rect"][1] - 12
        self.inp.click(x, y)
        deadline = time.time() + 30
        while time.time() < deadline:
            if proc not in self.sh("ps"):
                return
            self.g.settle(1.0)
        raise AssertionError(f"{name} did not exit after the close button")

    def run(self):
        steps = []

        def step(n, fn, *a):
            t = time.time()
            r = fn(*a)
            steps.append((n, time.time() - t))
            print(f"\n[SMOKE-ALPINEX] step {n}: ok ({time.time() - t:.1f}s)")
            return r

        step("boot", self.g.boot)
        out = self.sh("/disk/xapp list")
        if "xterm" not in out or "mousepad" not in out:
            raise AssertionError("xapp list:\n" + out)
        step("install xterm (Linux Apps)", self.install_frontend, "xterm")
        step("install xeyes", self.install_cli, "xeyes")
        step("install mousepad", self.install_cli, "mousepad")
        if "launch" not in self.sh("ls /disk/apps/xterm/manifest && cat /disk/apps/xterm/manifest && echo launch"):
            raise AssertionError("no launcher entry for xterm")

        xt = step("xterm", self.launch, "xterm", "")
        self.click_in(xt, xt["w"] // 2, xt["h"] // 2)
        self.inp.type("echo typed-in-xterm > /tmp/xt.txt\n")
        self.g.settle(2.0)
        if "typed-in-xterm" not in self.sh("cat /disk/alpine/tmp/xt.txt"):
            raise AssertionError("text typed into xterm did not reach its shell")
        self.g.shot("xterm-typed")

        xe = step("xeyes", self.launch, "xeyes", "")
        self.inp.move_to(xe["rect"][0] + 5, xe["rect"][1] + 5)
        self.g.settle(1.0)
        a = self.g.shot("xeyes-pointer-a")
        self.inp.move_to(xe["rect"][0] + xe["w"] - 5, xe["rect"][1] + xe["h"] - 5)
        self.g.settle(1.5)
        b = self.g.shot("xeyes-pointer-b")
        moved = smoke_gui.changed_fraction(a, b, xe["rect"])
        print(f"\n[SMOKE-ALPINEX] xeyes: {moved:.1%} of the window changed when the pointer moved")
        self.notes.append(f"xeyes pupils follow the pointer: {moved:.1%} changed")

        mp = step("mousepad", self.launch, "mousepad", "")
        self.click_in(mp, mp["w"] // 2, mp["h"] // 2)
        self.inp.type("hello from maeroX\n")
        self.g.settle(1.0)
        self.g.shot("mousepad-typed")
        self.inp.combo(["ctrl"], "s")
        self.g.settle(4.0)
        self.g.shot("mousepad-save-dialog")
        self.inp.combo(["ctrl"], "a")
        self.inp.type("maerox-note.txt\n")
        self.g.settle(3.0)
        self.g.shot("mousepad-saved")
        saved = self.sh("cat /disk/alpine/home/*/maerox-note.txt")
        if "hello from maeroX" not in saved:
            raise AssertionError("mousepad did not save the typed text:\n" + saved)

        step("close mousepad", self.close, "mousepad", mp, "mousepad")
        step("close xeyes", self.close, "xeyes", xe, "xeyes")
        step("close xterm", self.close, "xterm", xt, "xterm")
        if "maerox" not in self.sh("ps"):
            raise AssertionError("maeroX is no longer running")
        return steps


def main():
    shutil.rmtree(OUT, ignore_errors=True)
    os.makedirs(OUT)
    if not os.path.exists(IMG):
        raise RuntimeError("disk-alpinex.img is missing (make disk-alpinex)")
    disk = os.path.join(OUT, "disk.img")
    subprocess.run(["cp", "--sparse=always", IMG, disk], check=True)
    sockdir = tempfile.mkdtemp(prefix="sax")
    qmp_path = os.path.join(sockdir, "qmp")
    accel = smoke_gui.pick_accel()
    cmd = ["qemu-system-i386", "-cdrom", os.path.join(ROOT, "maeros.iso"),
           "-drive", f"file={disk},format=raw,if=ide",
           "-accel", accel, "-vga", "std", *smokelib.QEMU_DISPLAY,
           "-serial", "stdio", "-m", "1536M", "-no-reboot", "-no-shutdown",
           "-qmp", f"unix:{qmp_path},server=on,wait=off"]
    t0 = time.time()
    proc = subprocess.Popen(cmd, cwd=ROOT, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, bufsize=0)
    con = Console(proc)
    qmp = None
    ax = None
    try:
        qmp = Qmp(qmp_path)
        ax = AlpineX(con, qmp)
        steps = ax.run()
        print("\n[SMOKE-ALPINEX] " + "; ".join(ax.notes))
        print("[SMOKE-ALPINEX] timings: " + ", ".join(f"{n} {s:.1f}s" for n, s in steps))
        print(f"[SMOKE-ALPINEX] passed in {time.time() - t0:.1f}s (accel={accel})")
        return 0
    except Exception:
        if ax is not None:
            try:
                ax.g.shot("fail")
            except Exception:
                pass
        print("\n[SMOKE-ALPINEX] last serial output:\n" + con.text()[-3000:], file=sys.stderr)
        raise
    finally:
        if ax is not None and proc.poll() is None:
            try:
                for f in ("/tmp/maerox.log",) + tuple(
                        f"/disk/alpine/tmp/xapp-{n}.log" for n in ("xterm", "xeyes", "mousepad")):
                    with open(os.path.join(OUT, os.path.basename(f)), "w") as fh:
                        fh.write(con.run(f"cat {f}", 10))
            except Exception:
                pass
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
        try:
            os.remove(disk)
        except OSError:
            pass


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"\n[SMOKE-ALPINEX] failed: {exc}; see build/smoke-alpinex/", file=sys.stderr)
        raise SystemExit(1)
