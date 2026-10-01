#!/usr/bin/env python3
"""smoke-usb — drive the desktop with USB input only (xHCI + HID).

Boots the GRUB ISO like smoke-gui, with a qemu-xhci controller carrying a
usb-kbd, a usb-tablet and a usb-mouse.  The three are bound to the VGA
display (display=vga0) and every event goes through QMP input-send-event
addressed to that display: QEMU then routes keys to the usb-kbd, absolute
motion to the usb-tablet and relative motion to the usb-mouse, the handlers
bound to it, and the PS/2 keyboard and mouse QEMU also has never see any
input.  Whatever the desktop does here came through drivers/usb/.  (The
`device` argument of input-send-event names a display, not an input
device; naming a USB device there aborts QEMU.)

Checks:
  - the kernel enumerated all three devices (keyboard, tablet, boot mouse);
  - the tablet's absolute positions land exactly where they were sent: the
    desktop traces each click's position, which must match (launcher orb,
    window close button);
  - typing on the USB keyboard opens the Terminal from the launcher and runs
    a command in the Terminal's own shell (checked from the serial shell);
  - a login over the USB keyboard: in that Terminal, `doas login root`
    asks for the user's password and then root's, both typed on the USB
    keyboard, and the root shell it starts writes `id -u` to a file;
  - relative motion from the USB boot mouse reaches /dev/input/event1 and
    moves the desktop pointer by the amount sent;
  - a usb-storage stick (an 8 MiB image with markers written here) shows up
    as /dev/usbdisk0: markers read back at their offsets, a write through
    the device lands in the image file, the whole device reads back with
    the image's checksum; then the stick is unplugged and plugged back in
    (QMP device_del / device_add) and is found again.

The serial console is only used to log in a root shell for those checks (the
console reads the serial line, never a keyboard; see userspace/init/init.c).

Output goes to build/smoke-usb/ (serial.log, screendumps).  Uses KVM when
/dev/kvm is usable, else TCG; SMOKE_GUI_ACCEL=tcg|kvm overrides.
"""
import os
import re
import shutil
import subprocess
import sys
import hashlib
import tempfile
import time

import smokelib
import smoke_gui
from smoke_gui import Console, Qmp, Image, GuiSmoke, KEYMAP, key, btn

ROOT = smoke_gui.ROOT
OUT = os.path.join(ROOT, "build", "smoke-usb")
smoke_gui.OUT = OUT          # GuiSmoke.shot() writes there

KBD, TABLET, MOUSE = "ukbd", "utab", "umouse"
STICK = "ustick"
STICK_SIZE = 8 << 20
MARKS = {0: b"USBSTICK-START!\n", (1 << 20) + 100: b"MIDDLE-MARKER\n",
         STICK_SIZE - 16: b"USBSTICK-END!!!\n"}
WRITE_AT, WRITE_TEXT = 2000000, "written-over-usb"


def make_stick(path):
    data = bytearray(STICK_SIZE)
    for off, text in MARKS.items():
        data[off:off + len(text)] = text
    with open(path, "wb") as f:
        f.write(data)
DISPLAY = "vga0"
ABS_MAX = 0x8000             # QEMU's tablet axis: 0 .. 0x7fff


class UsbInput:
    """The smoke_gui Input interface, on the USB devices: keys to the
    usb-kbd, absolute pointer positions and buttons to the usb-tablet."""

    def __init__(self, qmp, fb):
        self.qmp = qmp
        self.w, self.h = fb

    def send(self, device, evs):
        # `device` documents the intended USB device; QEMU picks it by event
        # type among the handlers bound to the display (keys: usb-kbd, abs:
        # usb-tablet, rel: usb-mouse; buttons: either pointer).
        del device
        self.qmp.cmd("input-send-event", device=DISPLAY, head=0, events=evs)

    def _abs(self, pos, extent):
        # The kernel maps v to floor(v * extent / 0x8000); the smallest v
        # that maps to pos is the ceiling of the inverse.
        return min(ABS_MAX - 1, -(-pos * ABS_MAX // extent))

    def move_to(self, x, y):
        self.send(TABLET, [
            {"type": "abs", "data": {"axis": "x", "value": self._abs(x, self.w)}},
            {"type": "abs", "data": {"axis": "y", "value": self._abs(y, self.h)}},
        ])
        time.sleep(0.08)

    def click(self, x, y, double=False):
        self.move_to(x, y)
        for _ in range(2 if double else 1):
            self.send(TABLET, [btn(True)])
            time.sleep(0.06)
            self.send(TABLET, [btn(False)])
            time.sleep(0.1)

    def press(self, qcode):
        self.send(KBD, [key(True, qcode)])
        time.sleep(0.03)
        self.send(KBD, [key(False, qcode)])
        time.sleep(0.03)

    def type(self, text):
        for ch in text:
            qcode, shifted = KEYMAP[ch]
            if shifted:
                self.send(KBD, [key(True, "shift")])
            self.press(qcode)
            if shifted:
                self.send(KBD, [key(False, "shift")])


class UsbSmoke(GuiSmoke):
    def boot(self):
        super().boot()
        self.inp = UsbInput(self.qmp, self.fb)
        text = self.con.text()
        for kind in ("keyboard", "pointer", "mouse"):
            if not re.search(r"\[USB\] port \d+: \S+ \S+ speed, slot \d+: "
                             r"HID %s " % kind, text):
                raise AssertionError(f"kernel did not bring up a USB {kind}")

    def login(self, term):
        """`doas login root` in the Terminal, every key on the USB keyboard."""
        self.expect_focus(term)
        proof = "/tmp/usbsmoke.uid"
        self.con.run(f"rm -f {proof}")
        self.inp.type("doas login root\n")
        self.settle(2.0)                      # doas: password for user:
        self.inp.type("user\n")
        self.settle(3.0)                      # login's Password:
        self.inp.type("root\n")
        self.settle(3.0)                      # root shell
        self.inp.type(f"id -u > {proof}; echo $$ >> {proof}\n")
        deadline = time.time() + 20
        while True:
            got = self.con.run(f"cat {proof}")
            # MaeroOS's id prints "uid=0 gid=0" for -u too.
            uid = re.search(r"^(?:uid=)?(\d+)\b.*\r?$", got, re.M)
            pids = re.findall(r"^(\d+)\r?$", got, re.M)
            if uid and uid.group(1) == "0" and pids:
                if int(pids[-1]) == term["shell_pid"]:
                    raise AssertionError("id ran in the Terminal's own shell, "
                                         "not in the login shell")
                print(f"\n[SMOKE-USB] root login shell pid {pids[-1]} "
                      f"wrote uid 0")
                break
            if time.time() >= deadline:
                raise AssertionError(f"no root login over the USB keyboard; "
                                     f"{proof} holds {got!r}")
            self.settle(0.5)
        self.shot("login")
        # Leave the root shell so the Terminal's own shell is back.
        self.inp.type("exit\n")
        self.settle(1.0)

    def boot_mouse(self):
        """Relative motion from the usb-mouse: put the pointer somewhere
        known with the tablet, move it by (+40, +30) with the mouse, click,
        and the desktop must report the click there."""
        x0, y0 = self.fb[0] // 2 - 120, self.fb[1] // 2 - 90
        self.inp.move_to(x0, y0)
        self.inp.send(MOUSE, [{"type": "rel", "data": {"axis": "x", "value": 40}},
                              {"type": "rel", "data": {"axis": "y", "value": 30}}])
        time.sleep(0.15)
        self.inp.send(MOUSE, [btn(True)])
        time.sleep(0.06)
        self.inp.send(MOUSE, [btn(False)])
        m = self.con.wait_re(r"\[desktop\] click (\d+),(\d+)")
        got = (int(m.group(1)), int(m.group(2)))
        if not re.search(r"\[USB\] slot \d+: first mouse report",
                         self.con.text()):
            raise AssertionError("the motion did not come from the usb-mouse")
        if got != (x0 + 40, y0 + 30):
            raise AssertionError(f"usb-mouse click landed at {got}, not "
                                 f"{(x0 + 40, y0 + 30)}")

    def storage(self):
        con = self.con
        if not re.search(r"\[USB-MSC\] /dev/usbdisk0: .* 16384 blocks of 512 "
                         r"bytes", con.text()):
            raise AssertionError("the USB stick did not become /dev/usbdisk0")
        for off, text in MARKS.items():
            want = text.decode().strip()
            got = con.run(f"toybox dd if=/dev/usbdisk0 bs=1 skip={off} "
                          f"count={len(text)} 2>/dev/null")
            if want not in got:
                raise AssertionError(f"usbdisk0 at {off}: {got!r}, not {want!r}")
        con.run(f"echo {WRITE_TEXT} > /tmp/usbw.txt")
        con.run(f"toybox dd if=/tmp/usbw.txt of=/dev/usbdisk0 bs=1 "
                f"seek={WRITE_AT} conv=notrunc 2>/dev/null")
        got = con.run(f"toybox dd if=/dev/usbdisk0 bs=1 skip={WRITE_AT} "
                      f"count={len(WRITE_TEXT)} 2>/dev/null")
        if WRITE_TEXT not in got:
            raise AssertionError(f"write did not read back: {got!r}")
        with open(self.stick, "rb") as f:
            image = f.read()
        if image[WRITE_AT:WRITE_AT + len(WRITE_TEXT)] != WRITE_TEXT.encode():
            raise AssertionError("the write did not reach the stick's image")
        out = con.run("toybox dd if=/dev/usbdisk0 of=/tmp/usb.img bs=32768 "
                      "2>/dev/null; md5sum /tmp/usb.img; rm /tmp/usb.img",
                      timeout=60)
        want = hashlib.md5(image).hexdigest()
        if want not in out:
            raise AssertionError(f"usbdisk0 checksum {out!r}, image {want}")
        print(f"\n[SMOKE-USB] usbdisk0 reads back with md5 {want}")

    def hotplug(self):
        con = self.con
        start = con.mark()
        self.qmp.cmd("device_del", id=STICK)
        con.wait_re(r"\[USB-MSC\] /dev/usbdisk0 removed", start=start)
        con.wait_re(r"\[USB\] port \d+: device removed", start=start)
        if "usbdisk0" in con.run("ls /dev"):
            raise AssertionError("/dev/usbdisk0 still listed after unplug")
        start = con.mark()
        # device_del took the drive with it; plug a new one onto the image.
        self.qmp.cmd("blockdev-add", driver="raw", **{"node-name": "stick2"},
                     file={"driver": "file", "filename": self.stick})
        self.qmp.cmd("device_add", driver="usb-storage", drive="stick2",
                     id=STICK)
        con.wait_re(r"\[USB-MSC\] /dev/usbdisk0: .* 16384 blocks", timeout=20,
                    start=start)
        got = con.run("toybox dd if=/dev/usbdisk0 bs=1 count=16 2>/dev/null")
        if "USBSTICK-START!" not in got:
            raise AssertionError(f"replugged stick reads {got!r}")

    def run(self):
        steps = []

        def step(name, fn, *args):
            t0 = time.time()
            result = fn(*args)
            steps.append((name, time.time() - t0))
            print(f"\n[SMOKE-USB] step {name}: ok ({time.time() - t0:.1f}s)")
            return result

        step("boot", self.boot)
        term = step("terminal (USB keyboard + tablet)", self.terminal)
        step("login over the USB keyboard", self.login, term)
        step("close terminal (tablet click)", self.close, term)
        step("boot mouse", self.boot_mouse)
        step("mass storage", self.storage)
        step("unplug and replug the stick", self.hotplug)
        self.settle()
        self.shot("final")
        return steps


def main():
    shutil.rmtree(OUT, ignore_errors=True)
    os.makedirs(OUT)
    for f in ("maeros.iso", "disk.img"):
        if not os.path.exists(os.path.join(ROOT, f)):
            raise RuntimeError(f"{f} is missing (make iso disk)")
    disk = smoke_gui.prepare_disk()
    stick = os.path.join(OUT, "stick.img")
    make_stick(stick)
    sockdir = tempfile.mkdtemp(prefix="susb")
    qmp_path = os.path.join(sockdir, "qmp")
    accel = smoke_gui.pick_accel()
    cmd = ["qemu-system-i386", "-cdrom", os.path.join(ROOT, "maeros.iso"),
           "-drive", f"file={disk},format=raw,if=ide",
           "-accel", accel, "-vga", "none", "-device", f"VGA,id={DISPLAY}", *smokelib.QEMU_DISPLAY,
           "-serial", "stdio", "-m", "512M", "-no-reboot", "-no-shutdown",
           # 8+8 root ports: with the 4+4 default QEMU puts a hub in front of
           # the fourth device, and hubs are not supported yet.
           "-device", "qemu-xhci,id=xhci,p2=8,p3=8",
           "-device", f"usb-kbd,id={KBD},display={DISPLAY}",
           "-device", f"usb-tablet,id={TABLET},display={DISPLAY}",
           "-device", f"usb-mouse,id={MOUSE}",
           "-drive", f"if=none,id=stick,format=raw,file={stick}",
           "-device", f"usb-storage,drive=stick,id={STICK}",
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
        smoke = UsbSmoke(con, qmp)
        smoke.stick = stick
        steps = smoke.run()
        print("\n[SMOKE-USB] timings: " +
              ", ".join(f"{n} {s:.1f}s" for n, s in steps))
        print(f"[SMOKE-USB] passed in {time.time() - t0:.1f}s "
              f"(accel={accel}); output in {os.path.relpath(OUT, ROOT)}/")
        return 0
    except Exception:
        if smoke is not None:
            try:
                ppm = os.path.join(OUT, "screen.ppm")
                qmp.cmd("screendump", filename=ppm)
                Image.read_ppm(ppm).write_png(os.path.join(OUT, "fail.png"))
            except Exception as exc:
                print(f"\n[SMOKE-USB] no failure screendump: {exc}",
                      file=sys.stderr)
        print("\n[SMOKE-USB] last serial output:\n" + con.text()[-3000:],
              file=sys.stderr)
        raise
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
        try:
            os.remove(os.path.join(OUT, "screen.ppm"))
        except OSError:
            pass


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"\n[SMOKE-USB] failed: {exc}; see {os.path.relpath(OUT, ROOT)}/"
              f"serial.log and fail.png", file=sys.stderr)
        raise SystemExit(1)
