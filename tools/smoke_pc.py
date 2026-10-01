#!/usr/bin/env python3
"""smoke-pc — a PC with no legacy devices: q35, AHCI disk, USB input, ACPI.

Boots the GRUB ISO on `-M q35,i8042=off -smp 2`: the disk is on q35's
built-in ICH9 AHCI controller (no IDE), there is no 8042 PS/2 controller at
all, and the only keyboard and pointer are a usb-kbd and a usb-tablet on a
qemu-xhci controller.  The drivers from the acpi, ahci and xhci work meet
here on one boot:

  - the kernel finds no PS/2 controller and carries on (the 8042 drain loops
    used to spin for good when the status port floats to 0xFF);
  - /disk is mounted from ahci0, no ATA drive is present;
  - uACPI comes up, the second CPU is started from the MADT;
  - the USB keyboard and tablet are enumerated, the desktop starts, and the
    Terminal is opened and used with them (tablet clicks land where sent);
  - in that Terminal, `doas poweroff` (the password typed on the USB
    keyboard) enters ACPI S5 and QEMU, started without -no-shutdown, exits.

The serial console is only used to log in a root shell for the side-effect
checks (smokelib.login), as in smoke-gui and smoke-usb.

Output goes to build/smoke-pc/ (serial.log, screendumps).  Uses KVM when
/dev/kvm is usable, else TCG; SMOKE_GUI_ACCEL=tcg|kvm overrides.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

import smokelib
import smoke_gui
from smoke_gui import Console, Qmp, Image, GuiSmoke
from smoke_usb import UsbInput, KBD, TABLET, DISPLAY

ROOT = smoke_gui.ROOT
OUT = os.path.join(ROOT, "build", "smoke-pc")
smoke_gui.OUT = OUT          # prepare_disk() and GuiSmoke.shot() write there
CPUS = 2


class PcSmoke(GuiSmoke):
    def boot(self):
        super().boot()
        self.inp = UsbInput(self.qmp, self.fb)
        text = self.con.text()
        for needle in ("[KBD]  no PS/2 controller",
                       "[MOUSE] no PS/2 controller",
                       "[ATA]  Drive not present.",
                       "[BLK]  boot disk: ahci0",
                       "[EXT2]  Mounted:",
                       "[BOOT] Launching /disk/init",
                       "[ACPI] ready",
                       f"[SMP]  {CPUS} CPU(s) listed in the MADT",
                       f"[SMP]  {CPUS} CPU(s) online"):
            if needle not in text:
                raise AssertionError(f"boot log lacks {needle!r}")
        for kind in ("keyboard", "pointer"):
            if not re.search(r"\[USB\] port [\d.]+: \S+ \S+ speed, slot \d+: "
                             r"HID %s " % kind, text):
                raise AssertionError(f"kernel did not bring up a USB {kind}")

    def poweroff(self, term):
        """`doas poweroff` in the Terminal, typed on the USB keyboard."""
        self.expect_focus(term)
        start = self.con.mark()
        self.inp.type("doas poweroff\n")
        self.settle(2.0)                      # doas: password for user:
        self.inp.type("user\n")
        # Console.pump raises once QEMU is gone, which is the point here:
        # read the serial line directly until it closes.
        con = self.con
        deadline = time.time() + 30
        while True:
            for k, _ in con.sel.select(0.2):
                chunk = os.read(k.fd, 4096).decode("latin1", "replace")
                if not chunk:
                    con.sel.unregister(k.fileobj)
                    break
                con.log.append(chunk)
                sys.stdout.write(chunk)
            if con.proc.poll() is not None and not con.sel.get_map():
                break                           # exited, output all read
            if time.time() >= deadline:
                raise AssertionError("QEMU did not exit after poweroff")
        after = con.text()[start:]
        if "[ACPI] powering off" not in after:
            raise AssertionError("QEMU exited without the kernel's "
                                 "'[ACPI] powering off'")
        if "S5 failed" in after:
            raise AssertionError("ACPI S5 failed; the fallback ports were used")
        print(f"\n[SMOKE-PC] QEMU exited with status {con.proc.returncode} "
              f"after poweroff")

    def run(self):
        steps = []

        def step(name, fn, *args):
            t0 = time.time()
            result = fn(*args)
            steps.append((name, time.time() - t0))
            print(f"\n[SMOKE-PC] step {name}: ok ({time.time() - t0:.1f}s)")
            return result

        step("boot", self.boot)
        term = step("terminal (USB keyboard + tablet)", self.terminal)
        step("poweroff from the Terminal", self.poweroff, term)
        return steps


def main():
    shutil.rmtree(OUT, ignore_errors=True)
    os.makedirs(OUT)
    for f in ("maeros.iso", "disk.img"):
        if not os.path.exists(os.path.join(ROOT, f)):
            raise RuntimeError(f"{f} is missing (make iso disk)")
    disk = smoke_gui.prepare_disk()
    sockdir = tempfile.mkdtemp(prefix="spc")
    qmp_path = os.path.join(sockdir, "qmp")
    accel = smoke_gui.pick_accel()
    cmd = ["qemu-system-i386", "-M", "q35,i8042=off", "-smp", str(CPUS),
           "-cdrom", os.path.join(ROOT, "maeros.iso"),
           # index=0 on q35 is the first port of the ICH9 AHCI controller.
           "-drive", f"file={disk},format=raw,index=0,media=disk",
           "-accel", accel, "-vga", "none", "-device", f"VGA,id={DISPLAY}",
           *smokelib.QEMU_DISPLAY,
           "-serial", "stdio", "-m", "512M", "-no-reboot",
           "-device", "qemu-xhci,id=xhci",
           "-device", f"usb-kbd,id={KBD},display={DISPLAY},bus=xhci.0,port=1",
           "-device", f"usb-tablet,id={TABLET},display={DISPLAY},bus=xhci.0,"
                      "port=2",
           "-qmp", f"unix:{qmp_path},server=on,wait=off"]
    with open(os.path.join(OUT, "qemu-cmdline.txt"), "w") as f:
        f.write(" ".join(cmd) + "\n")
    t0 = time.time()
    proc = subprocess.Popen(cmd, cwd=ROOT, stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            bufsize=0)
    con = Console(proc)
    qmp = None
    smoke = None
    try:
        qmp = Qmp(qmp_path)
        smoke = PcSmoke(con, qmp)
        steps = smoke.run()
        print("\n[SMOKE-PC] timings: " +
              ", ".join(f"{n} {s:.1f}s" for n, s in steps))
        print(f"[SMOKE-PC] passed in {time.time() - t0:.1f}s "
              f"(accel={accel}); output in {os.path.relpath(OUT, ROOT)}/")
        return 0
    except Exception:
        if smoke is not None and proc.poll() is None:
            try:
                ppm = os.path.join(OUT, "screen.ppm")
                qmp.cmd("screendump", filename=ppm)
                Image.read_ppm(ppm).write_png(os.path.join(OUT, "fail.png"))
            except Exception as exc:
                print(f"\n[SMOKE-PC] no failure screendump: {exc}",
                      file=sys.stderr)
        print("\n[SMOKE-PC] last serial output:\n" + con.text()[-3000:],
              file=sys.stderr)
        raise
    finally:
        with open(os.path.join(OUT, "serial.log"), "w") as f:
            f.write(con.text())
        if qmp is not None:
            try:
                qmp.close()
            except OSError:
                pass
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
        shutil.rmtree(sockdir, ignore_errors=True)
        try:
            os.remove(os.path.join(OUT, "screen.ppm"))
        except OSError:
            pass


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"\n[SMOKE-PC] failed: {exc}; see {os.path.relpath(OUT, ROOT)}/"
              f"serial.log and fail.png", file=sys.stderr)
        raise SystemExit(1)
