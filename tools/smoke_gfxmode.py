#!/usr/bin/env python3
"""smoke-gfxmode — runtime display mode changes from the Settings Display page.

Boots the GRUB ISO with a copy of disk.img under

  std       -vga std: the Bochs DISPI driver (drivers/bochs_vga.c)
  virtio    -vga none -device virtio-vga: the virtio-gpu driver
            (drivers/virtio_gpu.c), whose screen only changes where the
            desktop flushes (FBIO_MAEROS_FLUSH)

and on each drives Settings -> Display with QMP mouse clicks, as smoke-gui
does (the apps trace the window-relative position of every control):

  * the mode list comes from the expected driver and holds 1024x768,
    1280x800 and 1920x1080;
  * Apply 1024x768: the desktop reports the mode, a screendump is 1024x768,
    shows the desktop (colour count) with the taskbar at the bottom (the orb
    at its new place opens the launcher), and Revert brings the old size back;
  * Apply 1920x1080 and Keep: still 1920x1080 after the 15 s confirmation
    window, and desktop.conf has mode=1920x1080;
  * Apply 1280x800 and wait: the desktop reverts by itself after 15 s;
  * virtio: the desktop's flushes cover its damage, not the screen (mouse
    moves flush a few KiB of pixels each), and the control queue never
    timed out;
  * std: a second boot of the same disk starts the desktop in the kept
    1920x1080 (desktop.conf mode= applied at login).

Then, if OVMF is installed, the Limine ISO on OVMF x64 with a ramfb display
(-vga none -device ramfb): a GOP framebuffer no driver here can change, so the
Display page must list the one fixed mode and the desktop must run on it.

Output: build/smoke-gfxmode/<run>/ (serial.log, NN-step.png).  KVM when
usable (SMOKE_GUI_ACCEL=tcg|kvm overrides); OVMF runs under TCG like
smoke-uefi.  SMOKE_GFXMODE_ONLY="std virtio uefi" picks runs.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

import smokelib
import smoke_uefi
from smoke_gui import (AUTOSTART_MARKERS, Console, GuiSmoke, Image, Qmp,
                       distinct_colors, find_debugfs, pick_accel)

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
OUT = os.path.join(ROOT, "build", "smoke-gfxmode")
WANT_MODES = {(1024, 768), (1280, 800), (1920, 1080)}


class GfxSmoke(GuiSmoke):
    def __init__(self, con, qmp, out):
        super().__init__(con, qmp)
        self.out = out

    def shot(self, name):
        self.shots += 1
        ppm = os.path.join(self.out, "screen.ppm")
        self.qmp.cmd("screendump", filename=ppm)
        img = Image.read_ppm(ppm)
        img.write_png(os.path.join(self.out, f"{self.shots:02d}-{name}.png"))
        os.remove(ppm)
        return img

    # -- the Display page --

    def open_display(self):
        target = self.open_launcher()
        self.launched_at = self.con.mark()
        self.click(*target)
        win = self.wait_window("Settings")
        self.con.wait_re(r"\[settings\] display button=(\d+),(\d+)",
                         start=self.launched_at)
        self.settle()
        # the last one: Settings traces it again when the desktop resizes it
        btn = re.findall(r"\[settings\] display button=(\d+),(\d+)",
                         self.con.text()[self.launched_at:])[-1]
        self.click(*self.rel(win, int(btn[0]), int(btn[1])))
        return win, self.read_display_page()

    def read_display_page(self):
        m = self.con.wait_re(
            r"\[settings\] display page driver=(\S+) modes=(\d+) "
            r"current=(\d+)x(\d+) settable=(\d) flush=(\d) apply=(\d+),(\d+) "
            r"back=(\d+),(\d+) keep=(\d+),(\d+) revert=(\d+),(\d+)")
        page = {"driver": m.group(1), "count": int(m.group(2)),
                "current": (int(m.group(3)), int(m.group(4))),
                "settable": m.group(5) == "1", "flush": m.group(6) == "1",
                "apply": (int(m.group(7)), int(m.group(8))),
                "keep": (int(m.group(11)), int(m.group(12))),
                "revert": (int(m.group(13)), int(m.group(14))),
                "modes": {}}
        while len(page["modes"]) < page["count"]:
            m = self.con.wait_re(r"\[settings\] modes((?: \d+x\d+@\d+,\d+)+)\n")
            for w, h, x, y in re.findall(r"(\d+)x(\d+)@(\d+),(\d+)", m.group(1)):
                page["modes"][(int(w), int(h))] = (int(x), int(y))
        print(f"\n[SMOKE-GFXMODE] display page: driver={page['driver']} "
              f"current={page['current']} modes="
              f"{sorted(page['modes'])}")
        return page

    def follow_relayout(self, win, start):
        """The desktop may have moved the Settings window into the new
        screen; take its new place from the relayout trace."""
        for m in re.finditer(r"\[desktop\] relayout Settings slot=(\d+) "
                             r"x=(-?\d+) y=(-?\d+) w=(\d+) h=(\d+)",
                             self.con.text()[start:]):
            if int(m.group(1)) == win["slot"]:
                win["x"], win["y"] = int(m.group(2)), int(m.group(3))
                win["w"], win["h"] = int(m.group(4)), int(m.group(5))

    def apply_mode(self, win, page, mode):
        start = self.con.mark()
        self.click(*self.rel(win, *page["modes"][mode]))
        self.con.wait_re(r"\[settings\] mode %dx%d selected" % mode)
        self.click(*self.rel(win, *page["apply"]))
        m = self.con.wait_re(r"\[desktop\] mode %dx%d was=(\d+)x(\d+) "
                             r"orb=(\d+),(\d+)" % mode, timeout=30)
        self.orb = (int(m.group(3)), int(m.group(4)))
        self.con.wait_re(r"\[desktop\] mode pending %dx%d" % mode)
        self.settle(1.5)
        self.follow_relayout(win, start)
        return (int(m.group(1)), int(m.group(2)))

    def check_screen(self, mode, name):
        """The screen is `mode` and shows the desktop: enough colours, and
        the taskbar at the bottom (its orb opens the launcher)."""
        img = self.shot(name)
        if (img.w, img.h) != mode:
            raise AssertionError(f"{name}: screendump is {img.w}x{img.h}, "
                                 f"expected {mode[0]}x{mode[1]}")
        colors = distinct_colors(img, (0, 0, img.w, img.h))
        bar = distinct_colors(img, (0, img.h - 40, img.w, 40))
        if colors < 50 or bar < 8:
            raise AssertionError(f"{name}: {colors} colours on screen, {bar} "
                                 f"in the taskbar strip")
        self.click(*self.orb)
        self.con.wait_re(r"\[desktop\] launcher open settings=\d+,\d+")
        self.settle(0.5)
        opened = self.shot(name + "-launcher")
        # the launcher rises from the taskbar: the lower-left area changes
        from smoke_gui import changed_fraction
        moved = changed_fraction(img, opened, (0, img.h // 3, img.w // 3,
                                               img.h * 2 // 3 - 40))
        if moved < 0.1:
            raise AssertionError(f"{name}: the launcher did not show "
                                 f"({moved:.0%} changed)")
        self.inp.press("esc")
        self.settle(0.6)
        print(f"\n[SMOKE-GFXMODE] {name}: {img.w}x{img.h}, {colors} colours, "
              f"taskbar {bar}, launcher {moved:.0%}")

    def desktop_stats(self):
        start = self.con.mark()
        self.con.run("echo stats > /tmp/.wm-1000/ctl")
        m = self.con.wait_re(r"\[desktop\] stats frames=(\d+) .* flushes=(\d+) "
                             r"flush_rects=(\d+) flush_kpx=(\d+)", start=start)
        return [int(g) for g in m.groups()]

    def check_damage_flushes(self, mode):
        s0 = self.desktop_stats()
        for i in range(12):                       # pointer wiggles: tiny damage
            self.qmp.events([{"type": "rel", "data": {"axis": "x", "value": 9 if i % 2 else -9}}])
            time.sleep(0.12)
        self.settle(0.5)
        s1 = self.desktop_stats()
        flushes, kpx = s1[1] - s0[1], s1[3] - s0[3]
        screen_kpx = mode[0] * mode[1] // 1024
        if flushes < 4:
            raise AssertionError(f"only {flushes} flushes for 12 pointer moves")
        per = kpx / flushes
        print(f"\n[SMOKE-GFXMODE] damage flushes: {flushes} flushes, "
              f"{s1[2] - s0[2]} rects, {kpx} Kpx ({per:.1f} Kpx each, screen "
              f"{screen_kpx} Kpx)")
        if per > screen_kpx / 8:
            raise AssertionError(f"pointer-move flushes average {per:.0f} Kpx, "
                                 f"the screen is {screen_kpx} Kpx")

    def run_modes(self, run):
        self.boot()
        boot_mode = self.fb
        win, page = self.open_display()
        want_driver = {"std": "bochs", "virtio": "virtio-gpu"}[run]
        if page["driver"] != want_driver or not page["settable"]:
            raise AssertionError(f"Display page driver {page['driver']} "
                                 f"settable={page['settable']}, expected "
                                 f"{want_driver}")
        if page["flush"] != (run == "virtio"):
            raise AssertionError(f"flush flag {page['flush']} on {run}")
        if page["current"] != boot_mode:
            raise AssertionError(f"current mode {page['current']}, desktop "
                                 f"runs at {boot_mode}")
        missing = WANT_MODES - set(page["modes"])
        if missing:
            raise AssertionError(f"mode list lacks {sorted(missing)}")
        self.shot("display-page")

        # 1. 1024x768, then Revert
        first = (1024, 768) if boot_mode != (1024, 768) else (1280, 800)
        was = self.apply_mode(win, page, first)
        if was != boot_mode:
            raise AssertionError(f"desktop switched from {was}, not {boot_mode}")
        self.check_screen(first, f"mode-{first[0]}x{first[1]}")
        self.click(*self.rel(win, *page["revert"]))
        self.con.wait_re(r"\[desktop\] mode reverted to %dx%d \(asked\)" % boot_mode)
        self.con.wait_re(r"\[settings\] mode revert \(button\)")
        self.settle(1.5)
        img = self.shot("reverted")
        if (img.w, img.h) != boot_mode:
            raise AssertionError(f"after Revert the screen is {img.w}x{img.h}")
        self.follow_relayout(win, 0)

        # 2. 1920x1080 and Keep
        self.apply_mode(win, page, (1920, 1080))
        self.check_screen((1920, 1080), "mode-1920x1080")
        self.click(*self.rel(win, *page["keep"]))
        self.con.wait_re(r"\[desktop\] mode kept 1920x1080")
        self.con.wait_re(r"\[settings\] mode keep 1920x1080 saved")
        if run == "virtio":
            self.check_damage_flushes((1920, 1080))
        conf = self.con.run("cat /disk/etc/desktop.conf")
        if "mode=1920x1080" not in conf:
            raise AssertionError(f"desktop.conf lacks mode=1920x1080: {conf!r}")
        mark = self.con.mark()
        deadline = time.time() + 17
        while time.time() < deadline:
            self.con.pump(0.2)
        if "mode reverted" in self.con.text()[mark:]:
            raise AssertionError("the kept mode was reverted")
        img = self.shot("kept")
        if (img.w, img.h) != (1920, 1080):
            raise AssertionError(f"kept mode: screen is {img.w}x{img.h}")

        # 3. Apply 1280x800 and let the confirmation run out
        self.apply_mode(win, page, (1280, 800))
        img = self.shot("mode-1280x800-unconfirmed")
        if (img.w, img.h) != (1280, 800):
            raise AssertionError(f"1280x800: screen is {img.w}x{img.h}")
        self.con.wait_re(r"\[desktop\] mode reverted to 1920x1080 \(timeout\)",
                         timeout=25)
        self.settle(1.5)
        img = self.shot("timeout-reverted")
        if (img.w, img.h) != (1920, 1080):
            raise AssertionError(f"after the timeout the screen is {img.w}x{img.h}")
        text = self.con.text()
        if "control queue timeout" in text or "PANIC" in text:
            raise AssertionError("kernel reported a display error")
        return f"{page['driver']}: boot {boot_mode[0]}x{boot_mode[1]}, " \
               f"{len(page['modes'])} modes, switch/revert/keep/timeout ok"

    def run_login(self):
        """Second boot of the disk the kept mode was saved on."""
        smokelib.login(self.con.proc, self.con.sel, self.con.log, timeout=120)
        m = self.con.wait_re(r"\[desktop\] ready fb=(\d+)x(\d+)", timeout=90,
                             start=0)
        fb = (int(m.group(1)), int(m.group(2)))
        if fb != (1920, 1080):
            raise AssertionError(f"desktop started at {fb}, desktop.conf says "
                                 f"1920x1080")
        self.settle(1.5)
        img = self.shot("login-1920x1080")
        if (img.w, img.h) != fb or distinct_colors(img, (0, 0, img.w, img.h)) < 50:
            raise AssertionError(f"login screen {img.w}x{img.h} wrong or blank")
        return "desktop.conf mode=1920x1080 applied at login"

    def run_fixed(self):
        smokelib.login(self.con.proc, self.con.sel, self.con.log, timeout=240)
        m = self.con.wait_re(r"\[desktop\] ready fb=(\d+)x(\d+) orb=(\d+),(\d+)",
                             timeout=180, start=0)
        self.fb = (int(m.group(1)), int(m.group(2)))
        self.orb = (int(m.group(3)), int(m.group(4)))
        self.settle(2)
        img = self.shot("desktop")
        if (img.w, img.h) != self.fb or distinct_colors(img, (0, 0, img.w, img.h)) < 50:
            raise AssertionError(f"GOP desktop {img.w}x{img.h} wrong or blank")
        win, page = self.open_display()
        if page["driver"] != "boot" or page["settable"] or page["count"] != 1 or \
                page["current"] != self.fb:
            raise AssertionError(f"GOP framebuffer: Display page {page}")
        self.settle(1)
        self.shot("display-page-fixed")
        return f"GOP {self.fb[0]}x{self.fb[1]} fixed, desktop up, 1 mode listed"


def prepare_disk(dst):
    shutil.copyfile(os.path.join(ROOT, "disk.img"), dst)
    debugfs = find_debugfs()
    if not debugfs:
        raise RuntimeError("debugfs (e2fsprogs) is needed to prepare the disk")
    for marker in AUTOSTART_MARKERS:
        subprocess.run([debugfs, "-w", "-R", f"rm {marker}", dst],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def boot(out, disk, display, fn, qemu="qemu-system-i386", iso="maeros.iso",
         accel=None, extra=()):
    os.makedirs(out, exist_ok=True)
    sockdir = tempfile.mkdtemp(prefix="sgfx")
    qmp_path = os.path.join(sockdir, "qmp")
    cmd = [qemu, "-M", "pc", "-cdrom", os.path.join(ROOT, iso),
           "-drive", f"file={disk},format=raw,if=ide",
           "-accel", accel or pick_accel(), *display, *smokelib.qemu_args(qemu),
           "-serial", "stdio", "-m", "512M", "-no-reboot", "-no-shutdown",
           "-qmp", f"unix:{qmp_path},server=on,wait=off", *extra]
    with open(os.path.join(out, "qemu-cmdline.txt"), "a") as f:
        f.write(" ".join(cmd) + "\n")
    proc = subprocess.Popen(cmd, cwd=ROOT, stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            bufsize=0)
    con = Console(proc)
    qmp = None
    smoke = None
    try:
        qmp = Qmp(qmp_path)
        smoke = GfxSmoke(con, qmp, out)
        smoke.shots = len([f for f in os.listdir(out) if f.endswith(".png")])
        return fn(smoke)
    except Exception:
        if smoke is not None:
            try:
                smoke.shot("fail")
            except Exception as exc:
                print(f"\n[SMOKE-GFXMODE] no failure screendump: {exc}",
                      file=sys.stderr)
        print("\n[SMOKE-GFXMODE] last serial output:\n" + con.text()[-3000:],
              file=sys.stderr)
        raise
    finally:
        with open(os.path.join(out, "serial.log"), "a") as f:
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


def main():
    shutil.rmtree(OUT, ignore_errors=True)
    os.makedirs(OUT)
    for f in ("maeros.iso", "disk.img"):
        if not os.path.exists(os.path.join(ROOT, f)):
            raise RuntimeError(f"{f} is missing (make iso disk)")
    only = os.environ.get("SMOKE_GFXMODE_ONLY", "std virtio uefi").split()
    results = []
    t0 = time.time()
    for run, display in (("std", ["-vga", "std"]),
                         ("virtio", ["-vga", "none", "-device", "virtio-vga"])):
        if run not in only:
            continue
        out = os.path.join(OUT, run)
        os.makedirs(out)
        disk = os.path.join(out, "disk.img")
        prepare_disk(disk)
        t = time.time()
        results.append((run, boot(out, disk, display,
                                  lambda s, r=run: s.run_modes(r)), time.time() - t))
        if run == "std":
            t = time.time()
            results.append(("std-login", boot(out, disk, display,
                                              lambda s: s.run_login()),
                            time.time() - t))
        os.remove(disk)
    if "uefi" in only:
        code, vars_src = smoke_uefi.configs()[1][2:4]
        iso = os.path.join(ROOT, "maeros-limine.iso")
        if not code or not os.path.exists(iso):
            results.append(("uefi-gop", "SKIP (no OVMF x64 or maeros-limine.iso)", 0))
        else:
            out = os.path.join(OUT, "uefi")
            os.makedirs(out)
            disk = os.path.join(out, "disk.img")
            prepare_disk(disk)
            extra = ["-drive", f"if=pflash,format=raw,readonly=on,file={code}"]
            if vars_src:
                shutil.copyfile(vars_src, os.path.join(out, "vars.fd"))
                extra += ["-drive", f"if=pflash,format=raw,file={out}/vars.fd"]
            t = time.time()
            accel = os.environ.get("SMOKE_UEFI_ACCEL", "tcg")
            results.append(("uefi-gop", boot(out, disk, ["-vga", "none", "-device", "ramfb"],
                                             lambda s: s.run_fixed(),
                                             qemu="qemu-system-x86_64",
                                             iso="maeros-limine.iso", accel=accel,
                                             extra=extra), time.time() - t))
            os.remove(disk)
    for name, verdict, secs in results:
        print(f"[SMOKE-GFXMODE] {name}: {verdict} ({secs:.1f}s)")
    print(f"[SMOKE-GFXMODE] passed in {time.time() - t0:.1f}s; screendumps in "
          f"{os.path.relpath(OUT, ROOT)}/")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"\n[SMOKE-GFXMODE] failed: {exc}; see "
              f"{os.path.relpath(OUT, ROOT)}/<run>/serial.log", file=sys.stderr)
        raise SystemExit(1)
