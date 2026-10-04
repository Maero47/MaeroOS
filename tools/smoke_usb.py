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
  - the kernel enumerated all three devices (keyboard, tablet, boot mouse),
    and xhci.c's boot self-tests passed (Link TRB chaining; aborting a TD
    pending on a Running endpoint, which is what a timeout does);
  - the tablet's absolute positions land exactly where they were sent: the
    desktop traces each click's position, which must match (launcher orb,
    window close button);
  - typing on the USB keyboard opens the Terminal from the launcher and runs
    a command in the Terminal's own shell (checked from the serial shell);
  - a login over the USB keyboard: in that Terminal, `doas login root`
    asks for the user's password and then root's, both typed on the USB
    keyboard, and the root shell it starts writes `id -u` to a file;
  - a key held down on the USB keyboard repeats (kernel typematic);
  - relative motion from the USB boot mouse reaches /dev/input/event1 and
    moves the desktop pointer by the amount sent;
  - a usb-hub on a root port, with the usb-mouse and stick A behind it;
  - the xHCI runs on interrupts (MSI-X under QEMU's default qemu-xhci);
  - two usb-storage sticks at once, both FAT volumes made here: stick A
    (8 MiB, full speed behind the hub) and stick B (16 MiB, on a root port,
    so at the fastest speed QEMU's usb-storage offers).  Each is a
    /dev/usbdiskN and an sdX disk; stick A reads back whole with its
    image's checksum through /dev/usbdiskN, and a write through the device
    lands in the image file;
  - both sticks mounted (vfat) at the same time, a file copied from each to
    the other, checked in the guest and, after umount, in the image files;
  - a usb-bot device with two SCSI LUNs behind the hub: one disk each;
  - Caps Lock pressed on the USB keyboard: the kernel sends the keyboard
    its LED report (SET_REPORT, traced), on and off again;
  - the keyboard's Volume Down, Up and Mute keys reach the desktop, which
    sets the HDA card's ALSA Master Playback Volume / Switch (desktop and
    kernel traces agree);
  - stick B unplugged and plugged back in five times (QMP device_del /
    device_add) while a reader loops over it, each time found again under
    the same names with the same data; stick A once on the hub's port; no
    transfer timeouts or errors in the log, and as many devices in use
    afterwards as before;
  - two exFAT sticks plugged in behind the hub (QMP), mounted together
    without -t (exfat found through /proc/filesystems, listed after vfat), a
    file copied each way and one moved across (rename(2) between the two
    instances is EXDEV, so mv copies); stick X unmounted and checked on the
    host (tools/exfatimg.py and fsck.exfat -n); stick Y pulled out while
    mounted with a reader on it: no panic, the mount can be taken down, and
    plugged back in it mounts again with its data;
  - a usb-uas device with a scsi-hd (USB Attached SCSI) on a SuperSpeed
    port: brought up as UAS with streams (stream 1 on its status and data
    pipes), mounted (vfat), a file checked by md5, a copy and a new file
    written, and after umount both found in the image on the host;
  - kusbd's CPU time over 10 idle seconds (/proc/cputime), reported.

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
import mkexfatimg
from exfatimg import Volume
from smoke_gui import Console, Qmp, Image, GuiSmoke, KEYMAP, key, btn

ROOT = smoke_gui.ROOT
OUT = os.path.join(ROOT, "build", "smoke-usb")
smoke_gui.OUT = OUT          # GuiSmoke.shot() writes there

KBD, TABLET, MOUSE = "ukbd", "utab", "umouse"
STICK, STICK_B = "ustick", "ustickb"
STICK_SIZE, STICK_B_SIZE = 8 << 20, 16 << 20
# A raw write through /dev/usbdiskN, near the end of stick A (a free
# cluster of the nearly empty volume).
WRITE_AT, WRITE_TEXT = STICK_SIZE - 3000, "written-over-usb"
REPLUGS = 5
LUN_SIZES = (2 << 20, 3 << 20)
# A usb-uas disk (UAS, streams) on a SuperSpeed-only root port.
UAS_SIZE = 12 << 20
# Two exFAT sticks plugged in late, behind the hub.
EXFAT_STICKS = (("X", "uexx", "3.4", 4 << 20), ("Y", "uexy", "3.5", 6 << 20))
ENV = dict(os.environ, MTOOLS_SKIP_CHECK="1")


def make_stick(path, size, label, files):
    """A FAT volume (FAT12 or FAT16 by size) on the whole device, as sticks
    come, holding `files`."""
    with open(path, "wb") as f:
        f.truncate(size)
    subprocess.run(["mkfs.fat", "-n", label, path], check=True,
                   stdout=subprocess.DEVNULL, env=ENV)
    for name, data in files.items():
        src = path + "." + name
        with open(src, "wb") as f:
            f.write(data)
        subprocess.run(["mcopy", "-i", path, src, "::" + name], check=True,
                       env=ENV)
        os.remove(src)


def make_exfat_stick(path, size, files):
    """An exFAT volume on the whole device (no partition table)."""
    mkexfatimg.mkfs(path, size)
    v = Volume(path)
    for name, data in files.items():
        v.add_file(name, data)
    v.close()


def exfat_tree(path):
    """{name: md5} of the files on an exFAT image, and checker problems."""
    v = Volume(path)
    files, _, _ = v.tree()
    problems = v.check()
    v.close()
    return files, problems


def stick_file(path, name):
    """A file's bytes, read from the image with mtools."""
    return subprocess.run(["mcopy", "-n", "-i", path, "::" + name, "-"],
                          check=True, stdout=subprocess.PIPE, env=ENV).stdout


def md5(data):
    return hashlib.md5(data).hexdigest()
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
            if not re.search(r"\[USB\] port [\d.]+: \S+ \S+ speed, slot \d+: "
                             r"HID %s " % kind, text):
                raise AssertionError(f"kernel did not bring up a USB {kind}")
        # xhci.c's boot-time checks: a chained TD across the ring's Link
        # TRB keeps the chain, and a pending TD on a Running endpoint (the
        # timeout case) is stopped and skipped, after which the keyboard
        # must still work (it types everything below).
        for check in (r"ring wrap with chained TD ok",
                      r"abort of a pending TD on a running endpoint ok"):
            if not re.search(r"\[XHCI\] self-test: " + check, text):
                raise AssertionError(f"xHCI self-test failed: {check}")
        if not re.search(r"\[USB\] port \d+: \S+ full speed, slot \d+: hub, "
                         r"\d+ ports", text):
            raise AssertionError("kernel did not bring up the USB hub")
        if not re.search(r"\[USB\] port \d+\.1: \S+ \S+ speed, slot \d+: "
                         r"HID mouse", text):
            raise AssertionError("the mouse behind the hub is missing")
        m = re.search(r"\[XHCI\] interrupts: (MSI-X|MSI|INTx)", text)
        if not m:
            raise AssertionError("the xHCI is not on interrupts")
        if not re.search(r"\[USB-HID\] self-test: consumer control "
                         r"\(array, variables\) ok", text):
            raise AssertionError("HID consumer-control self-test failed")
        print(f"\n[SMOKE-USB] xHCI interrupts: {m.group(1)}")
        # Both sticks: their usbdiskN by size, and their sdX.
        self.disk = {}
        at = self.con.at                 # these waits look back; keep the order
        for blocks, which in ((STICK_SIZE // 512, "A"),
                              (STICK_B_SIZE // 512, "B")):
            m = self.con.wait_re(r"\[USB-MSC\] /dev/(usbdisk\d): .* %d "
                                 r"blocks of 512 bytes" % blocks, start=0)
            node = m.group(1)
            m = self.con.wait_re(r"\[USB-MSC\] /dev/%s is /dev/(sd[a-z])"
                                 % node, start=0)
            self.disk[which] = (node, m.group(1))
        self.con.at = max(at, self.con.at)
        print(f"\n[SMOKE-USB] stick A {self.disk['A']}, stick B "
              f"{self.disk['B']}")
        # The two-LUN device: two disks on one slot.
        luns = {}
        for lun, blocks in ((0, LUN_SIZES[0] // 512), (1, LUN_SIZES[1] // 512)):
            m = self.con.wait_re(r"\[USB-MSC\] /dev/(usbdisk\d): .* %d blocks "
                                 r"of 512 bytes .*, slot (\d+) LUN %d"
                                 % (blocks, lun), start=0)
            luns[lun] = (m.group(1), m.group(2))
        self.con.at = max(at, self.con.at)
        if luns[0][1] != luns[1][1] or luns[0][0] == luns[1][0]:
            raise AssertionError(f"LUNs not two disks of one device: {luns}")
        for lun, (node, _) in luns.items():
            out = self.con.run(f"toybox dd if=/dev/{node} bs=512 count=1 "
                               f"2>/dev/null")
            if f"LUN{lun}-MARK" not in out:
                raise AssertionError(f"LUN {lun} ({node}) reads {out!r}")
        print(f"\n[SMOKE-USB] usb-bot LUN 0 is {luns[0][0]}, LUN 1 is "
              f"{luns[1][0]} (slot {luns[0][1]})")

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

    def key_repeat(self, term):
        """USB keyboards do not repeat keys themselves; the kernel does
        (usb_hid.c).  Hold x for 1.2 s in the Terminal: 500 ms delay, then
        ~33 a second, so well over 10 x's must arrive."""
        self.expect_focus(term)
        proof = "/tmp/usbsmoke.rep"
        self.con.run(f"rm -f {proof}")
        self.inp.type("echo ")
        self.inp.send(KBD, [key(True, "x")])
        self.settle(1.2)
        self.inp.send(KBD, [key(False, "x")])
        self.inp.type(f" > {proof}\n")
        deadline = time.time() + 10
        while True:
            got = self.con.run(f"cat {proof}")
            m = re.search(r"^(x+)\r?$", got, re.M)
            if m:
                n = len(m.group(1))
                if not 10 <= n <= 60:
                    raise AssertionError(f"holding x for 1.2 s gave {n} x's")
                print(f"\n[SMOKE-USB] held key repeated: {n} x's")
                return
            if time.time() >= deadline:
                raise AssertionError(f"no repeat output; {proof}: {got!r}")
            self.settle(0.5)

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

    def bsh(self, cmd, timeout=60):
        """`cmd` under busybox sh; (status, output)."""
        assert "'" not in cmd
        out = self.con.run(f"busybox sh -c '{cmd}; echo @@RC=$?'",
                           timeout=timeout)
        m = re.search(r"@@RC=(\d+)", out)
        if not m:
            raise AssertionError(f"no status from {cmd!r}: {out!r}")
        return int(m.group(1)), out[:m.start()]

    def storage(self):
        """Stick A through its raw node: whole-device checksum, a write."""
        con = self.con
        node = self.disk["A"][0]
        with open(self.stick, "rb") as f:
            image = f.read()
        out = con.run(f"toybox dd if=/dev/{node} of=/tmp/usb.img bs=32768 "
                      "2>/dev/null; md5sum /tmp/usb.img; rm /tmp/usb.img",
                      timeout=60)
        if md5(image) not in out:
            raise AssertionError(f"{node} checksum {out!r}, image {md5(image)}")
        print(f"\n[SMOKE-USB] {node} reads back with md5 {md5(image)}")
        con.run(f"echo {WRITE_TEXT} > /tmp/usbw.txt")
        con.run(f"toybox dd if=/tmp/usbw.txt of=/dev/{node} bs=1 "
                f"seek={WRITE_AT} conv=notrunc 2>/dev/null")
        got = con.run(f"toybox dd if=/dev/{node} bs=1 skip={WRITE_AT} "
                      f"count={len(WRITE_TEXT)} 2>/dev/null")
        if WRITE_TEXT not in got:
            raise AssertionError(f"write did not read back: {got!r}")
        with open(self.stick, "rb") as f:
            image = f.read()
        if image[WRITE_AT:WRITE_AT + len(WRITE_TEXT)] != WRITE_TEXT.encode():
            raise AssertionError("the write did not reach the stick's image")

    def two_sticks(self):
        """Both sticks mounted at once; a file copied each way."""
        a, b = self.disk["A"][1], self.disk["B"][1]
        rc, out = self.bsh(
            f"busybox mkdir -p /mnt/ua /mnt/ub && "
            f"busybox mount -t vfat /dev/{a} /mnt/ua && "
            f"busybox mount -t vfat /dev/{b} /mnt/ub && "
            f"busybox grep -E \"^/dev/({a}|{b}) \" /proc/mounts && "
            f"busybox cp /mnt/ua/abig.bin /mnt/ub/ && "
            f"busybox cp /mnt/ub/bbig.bin /mnt/ua/ && "
            f"busybox md5sum /mnt/ua/abig.bin /mnt/ub/abig.bin "
            f"/mnt/ua/bbig.bin /mnt/ub/bbig.bin", timeout=120)
        if rc != 0:
            raise AssertionError(f"mount/copy failed ({rc}): {out!r}")
        mounted = re.findall(r"^/dev/(sd[a-z]) /mnt/u[ab] vfat", out, re.M)
        if sorted(mounted) != sorted([a, b]):
            raise AssertionError(f"not both mounted: {out!r}")
        for name, want in (("abig.bin", self.files["abig.bin"]),
                           ("bbig.bin", self.files["bbig.bin"])):
            n = out.count(md5(want))
            if n != 2:
                raise AssertionError(f"{name}: md5 {md5(want)} seen {n} "
                                     f"times, not 2: {out!r}")
        rc, out = self.bsh("busybox sync && busybox umount /mnt/ua && "
                           "busybox umount /mnt/ub")
        if rc != 0:
            raise AssertionError(f"umount failed: {out!r}")
        # The copies are in the image files themselves.
        if md5(stick_file(self.stick_b, "abig.bin")) != \
                md5(self.files["abig.bin"]):
            raise AssertionError("abig.bin did not reach stick B's image")
        if md5(stick_file(self.stick, "bbig.bin")) != \
                md5(self.files["bbig.bin"]):
            raise AssertionError("bbig.bin did not reach stick A's image")
        print(f"\n[SMOKE-USB] {a} and {b} mounted together; abig.bin "
              f"{md5(self.files['abig.bin'])} and bbig.bin "
              f"{md5(self.files['bbig.bin'])} copied across, found in both "
              f"images")

    def leds(self):
        """Caps Lock on the USB keyboard: the kernel sends the LED report."""
        con = self.con
        for state in ("on", "off"):
            start = con.mark()
            self.inp.press("caps_lock")
            m = con.wait_re(r"\[USB\] slot \d+: keyboard LEDs num off caps "
                            r"%s scroll off \(SET_REPORT (\w+)\)" % state,
                            timeout=10, start=start)
            if m.group(1) != "ok":
                raise AssertionError(f"SET_REPORT {m.group(0)!r}")
            print(f"\n[SMOKE-USB] Caps Lock {state}: {m.group(0)}")

    def media_key(self):
        """The keyboard's Volume Down / Up / Mute keys reach the desktop,
        which sets the HDA card's ALSA Master volume and switch."""
        steps = (("volumedown", r"volume (\d+) \(mixer: card0 (\d+)%\)",
                  "Volume = "),
                 ("volumeup", r"volume (\d+) \(mixer: card0 (\d+)%\)",
                  "Volume = "),
                 ("audiomute", r"volume (\d+) muted \(mixer: card0 (\d+)% "
                               r"off\)", "Switch = 0"),
                 ("audiomute", r"volume (\d+) \(mixer: card0 (\d+)%\)",
                  "Switch = 1"))
        levels = []
        for qcode, pat, kernel in steps:
            start = self.con.mark()
            self.inp.press(qcode)
            m = self.con.wait_re(r"\[desktop\] " + pat, timeout=10,
                                 start=start)
            if m.group(1) != m.group(2):
                raise AssertionError(f"mixer not at the desktop's level: "
                                     f"{m.group(0)}")
            self.con.wait_re(r"\[ALSA\] card 0: Master Playback " +
                             re.escape(kernel), timeout=10, start=start)
            levels.append(int(m.group(1)))
            print(f"\n[SMOKE-USB] {qcode}: {m.group(0)}")
        if levels[1] != levels[0] + 5 and levels[1] != 100:
            raise AssertionError(f"volume levels {levels}")

    def in_use(self):
        found = re.findall(r"\[USB\] (\d+) device\(s\) in use",
                           self.con.text())
        return int(found[-1]) if found else -1

    def replug(self, which, port, node_name, size_blocks, load):
        con = self.con
        node, sd = self.disk[which]
        dev_id = STICK if which == "A" else STICK_B
        image = self.stick if which == "A" else self.stick_b
        start = con.mark()
        if load:
            # A reader looping over the stick while it is pulled.
            con.run(f"busybox sh -c \"while toybox dd if=/dev/{node} "
                    f"of=/dev/null bs=65536 2>/dev/null; do :; done &\"")
            time.sleep(0.3)
        self.qmp.cmd("device_del", id=dev_id)
        con.wait_re(r"\[USB-MSC\] /dev/%s removed" % node, start=start)
        con.wait_re(r"\[USB\] port [\d.]+: device removed", start=start)
        if node in con.run("ls /dev"):
            raise AssertionError(f"/dev/{node} still listed after unplug")
        # The guest has seen the unplug, but QEMU drops the old backend only
        # once the device is finalized, which an in-flight read under load
        # can delay: the command-line -drive of the first plug-in goes by
        # itself then, the node from a later plug-in is deleted here.  Wait
        # until no block node has the image open, or the new one cannot
        # take its write lock.
        old = getattr(self, "node_" + which, None)
        deadline = time.time() + 15
        while True:
            if old:
                try:
                    self.qmp.cmd("blockdev-del", **{"node-name": old})
                    old = None
                except RuntimeError:
                    pass
            nodes = self.qmp.cmd("query-named-block-nodes")
            if not old and not any(n.get("file") == image for n in nodes):
                break
            if time.time() > deadline:
                raise AssertionError(f"{image} still open in QEMU after unplug")
            time.sleep(0.1)
        start = con.mark()
        self.qmp.cmd("blockdev-add", driver="raw", **{"node-name": node_name},
                     file={"driver": "file", "filename": image})
        setattr(self, "node_" + which, node_name)
        self.qmp.cmd("device_add", driver="usb-storage", drive=node_name,
                     id=dev_id, bus="xhci.0", port=port)
        con.wait_re(r"\[USB-MSC\] /dev/%s: .* %d blocks" % (node, size_blocks),
                    timeout=20, start=start)
        con.wait_re(r"\[USB-MSC\] /dev/%s is /dev/%s" % (node, sd),
                    timeout=20, start=start)
        with open(image, "rb") as f:
            head = f.read(1 << 18)
        out = con.run(f"toybox dd if=/dev/{node} bs=65536 count=4 2>/dev/null "
                      f"| md5sum")
        if md5(head) not in out:
            raise AssertionError(f"replugged {node} reads {out!r}")

    def hotplug(self):
        con = self.con
        before = self.in_use()
        start = con.mark()
        t0 = time.time()
        for n in range(REPLUGS):
            self.replug("B", "4", f"sb{n}", STICK_B_SIZE // 512, load=True)
            print(f"\n[SMOKE-USB] stick B replug {n + 1}/{REPLUGS}: ok")
        self.replug("A", "3.2", "sa0", STICK_SIZE // 512, load=False)
        self.settle(1.0)
        log = con.text()[start:]
        bad = re.findall(r"^.*(?:timed out|failed, code|too many|PANIC|"
                         r"[Pp]age fault|panic).*$", log, re.M)
        if bad:
            raise AssertionError(f"errors during the replugs: {bad[:5]}")
        after = self.in_use()
        if after != before:
            raise AssertionError(f"{before} devices in use before the "
                                 f"replugs, {after} after")
        print(f"\n[SMOKE-USB] {REPLUGS} replugs of stick B under load and one "
              f"of stick A in {time.time() - t0:.1f}s, no errors; "
              f"{after} devices in use before and after")

    def plug_exfat(self, which, dev_id, port, size, node_name):
        """QMP-plug exFAT stick `which`; its (usbdiskN, sdX)."""
        con = self.con
        start = con.mark()
        self.qmp.cmd("blockdev-add", driver="raw", **{"node-name": node_name},
                     file={"driver": "file", "filename": self.exfat[which]})
        self.qmp.cmd("device_add", driver="usb-storage", drive=node_name,
                     id=dev_id, bus="xhci.0", port=port)
        m = con.wait_re(r"\[USB-MSC\] /dev/(usbdisk\d): .* %d blocks of 512 "
                        r"bytes" % (size // 512), timeout=30, start=start)
        node = m.group(1)
        m = con.wait_re(r"\[USB-MSC\] /dev/%s is /dev/(sd[a-z])" % node,
                        timeout=30, start=start)
        return node, m.group(1)

    def exfat_sticks(self):
        """Two exFAT sticks mounted together; one unmounted and checked on
        the host, the other pulled out while mounted and plugged back."""
        con = self.con
        start = con.mark()
        devs = {}
        for which, dev_id, port, size in EXFAT_STICKS:
            devs[which] = self.plug_exfat(which, dev_id, port, size,
                                          f"ex{which.lower()}0")
        x, y = devs["X"][1], devs["Y"][1]
        print(f"\n[SMOKE-USB] exFAT sticks X {devs['X']}, Y {devs['Y']}")
        out = con.run("cat /proc/filesystems")
        if not re.search(r"^\tvfat\r?\n\texfat\r?$", out, re.M):
            raise AssertionError(f"/proc/filesystems: not vfat, exfat: {out!r}")
        rc, out = self.bsh(
            f"busybox mkdir -p /mnt/xa /mnt/xb && "
            f"busybox mount /dev/{x} /mnt/xa && busybox mount /dev/{y} /mnt/xb && "
            f"busybox grep -E \"^/dev/({x}|{y}) \" /proc/mounts && "
            f"busybox cp /mnt/xa/xbig.bin /mnt/xb/ && "
            f"busybox cp /mnt/xb/ybig.bin /mnt/xa/ && "
            f"busybox mv /mnt/xa/x-hello.txt /mnt/xb/ && "
            f"busybox md5sum /mnt/xa/xbig.bin /mnt/xb/xbig.bin "
            f"/mnt/xa/ybig.bin /mnt/xb/ybig.bin && busybox ls /mnt/xa /mnt/xb",
            timeout=120)
        if rc != 0:
            raise AssertionError(f"exFAT mount/copy failed ({rc}): {out!r}")
        mounted = re.findall(r"^/dev/(sd[a-z]) /mnt/x[ab] exfat rw", out, re.M)
        if sorted(mounted) != sorted([x, y]):
            raise AssertionError(f"not both mounted rw as exfat: {out!r}")
        for name in ("xbig.bin", "ybig.bin"):
            n = out.count(md5(self.exfiles[name]))
            if n != 2:
                raise AssertionError(f"{name}: md5 seen {n} times, not 2: "
                                     f"{out!r}")
        # Stick X: unmounted cleanly, then read on the host.
        rc, out = self.bsh("busybox sync && busybox umount /mnt/xa")
        if rc != 0:
            raise AssertionError(f"umount /mnt/xa failed: {out!r}")
        files, problems = exfat_tree(self.exfat["X"])
        want = {"xbig.bin": md5(self.exfiles["xbig.bin"]),
                "ybig.bin": md5(self.exfiles["ybig.bin"])}
        if files != want or problems:
            raise AssertionError(f"stick X on the host: {files} {problems}, "
                                 f"want {want}")
        r = subprocess.run([mkexfatimg.tool("fsck.exfat"), "-n",
                            self.exfat["X"]], capture_output=True, text=True)
        if r.returncode != 0:
            raise AssertionError(f"fsck.exfat -n stick X: {r.stdout}{r.stderr}")
        print(f"\n[SMOKE-USB] {x} and {y} mounted together as exfat; files "
              f"copied and moved across; {x} unmounted, clean on the host "
              f"(exfatimg.py, fsck.exfat)")
        # Stick Y: pulled out while mounted, with a reader looping on it.
        node = devs["Y"][0]
        con.run("busybox rm -f /tmp/xstop; busybox sh -c \"while [ ! -e "
                "/tmp/xstop ] && busybox cat /mnt/xb/ybig.bin >/dev/null "
                "2>&1; do :; done &\"")
        time.sleep(0.3)
        mark = con.mark()
        self.qmp.cmd("device_del", id="uexy")
        con.wait_re(r"\[USB-MSC\] /dev/%s removed" % node, start=mark)
        con.wait_re(r"\[USB\] port [\d.]+: device removed", start=mark)
        self.settle(1.0)
        con.run("busybox touch /tmp/xstop; busybox sleep 1")
        con.run("busybox ls /mnt/xb >/dev/null 2>&1; echo z > /mnt/xb/z.txt; "
                "busybox sync", timeout=30)
        rc, out = self.bsh("busybox umount /mnt/xb || busybox umount -l /mnt/xb; "
                           f"! busybox grep \"^/dev/{y} \" /proc/mounts",
                           timeout=30)
        if rc != 0:
            raise AssertionError(f"/mnt/xb not taken down after the unplug: "
                                 f"{out!r}")
        for _ in range(20):
            try:
                self.qmp.cmd("blockdev-del", **{"node-name": "exy0"})
                break
            except RuntimeError:
                time.sleep(0.2)
        node2, y2 = self.plug_exfat("Y", "uexy", "3.5", EXFAT_STICKS[1][3],
                                    "exy1")
        rc, out = self.bsh(f"busybox mount -t exfat -o ro /dev/{y2} /mnt/xb && "
                           f"busybox md5sum /mnt/xb/ybig.bin /mnt/xb/xbig.bin "
                           f"/mnt/xb/x-hello.txt && busybox umount /mnt/xb",
                           timeout=60)
        if rc != 0 or out.count(md5(self.exfiles["ybig.bin"])) != 1 or \
                out.count(md5(self.exfiles["xbig.bin"])) != 1 or \
                md5(b"hello X\n") not in out:
            raise AssertionError(f"stick Y after the replug ({rc}): {out!r}")
        log = con.text()[start:]
        bad = re.findall(r"^.*(?:PANIC|[Pp]age fault|panic).*$", log, re.M)
        if bad:
            raise AssertionError(f"errors around the exFAT sticks: {bad[:5]}")
        print(f"\n[SMOKE-USB] {y} pulled out while mounted under a reader: "
              f"no panic, the mount taken down; back as {node2}/{y2} it "
              f"mounts with its data")

    def uas(self):
        """The UAS disk: streams, mount, read, write."""
        con = self.con
        m = con.wait_re(r"\[USB-MSC\] slot (\d+): UAS on interface \d+ alt "
                        r"\d+, streams", start=0)
        slot = m.group(1)
        m = con.wait_re(r"\[USB-MSC\] /dev/(usbdisk\d): .* %d blocks of 512 "
                        r"bytes .*, slot %s LUN 0, UAS with streams"
                        % (UAS_SIZE // 512, slot), start=0)
        node = m.group(1)
        sd = con.wait_re(r"\[USB-MSC\] /dev/%s is /dev/(sd[a-z])" % node,
                         start=0).group(1)
        rc, out = self.bsh(
            f"busybox mkdir -p /mnt/uas && "
            f"busybox mount -t vfat /dev/{sd} /mnt/uas && "
            f"busybox md5sum /mnt/uas/ubig.bin && "
            f"busybox cp /mnt/uas/ubig.bin /mnt/uas/ucopy.bin && "
            f"echo hello-uas > /mnt/uas/u-new.txt && "
            f"busybox md5sum /mnt/uas/ucopy.bin && busybox sync && "
            f"busybox umount /mnt/uas", timeout=120)
        want = md5(self.uas_files["ubig.bin"])
        if rc != 0 or out.count(want) != 2:
            raise AssertionError(f"UAS mount/read/write ({rc}): {out!r}")
        if md5(stick_file(self.uas_img, "ucopy.bin")) != want or \
                stick_file(self.uas_img, "u-new.txt") != b"hello-uas\n":
            raise AssertionError("the UAS writes are not in the image")
        bad = [l for l in con.text().splitlines()
               if re.search(r"slot %s: .*(timed out|failed)" % slot, l)]
        if bad:
            raise AssertionError(f"UAS transfer errors: {bad[:5]}")
        print(f"\n[SMOKE-USB] UAS disk {node} = {sd}: streams, vfat mounted, "
              f"md5 ok, a copy and a new file written and found in the image")

    def cputime(self):
        """kusbd's CPU time over 10 idle seconds."""
        def sample():
            out = self.con.run("cat /proc/cputime")
            idle = int(re.search(r"^idle (\d+)", out, re.M).group(1))
            m = re.search(r"^\d+ \d+ (\d+) kusbd\r?$", out, re.M)
            return time.time(), idle, int(m.group(1))
        t0, idle0, k0 = sample()
        time.sleep(10)
        t1, idle1, k1 = sample()
        wall = (t1 - t0) * 1e6
        irqs = re.findall(r"device\(s\) in use, (\d+) interrupts",
                          self.con.text())
        print(f"\n[SMOKE-USB] idle {t1 - t0:.1f}s: kusbd {k1 - k0} us "
              f"({100.0 * (k1 - k0) / wall:.3f}%), CPU idle "
              f"{100.0 * (idle1 - idle0) / wall:.1f}%")
        if (k1 - k0) > wall * 0.01:
            raise AssertionError(f"kusbd used {k1 - k0} us of {wall:.0f}")

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
        step("key repeat", self.key_repeat, term)
        step("close terminal (tablet click)", self.close, term)
        step("boot mouse", self.boot_mouse)
        step("keyboard LEDs", self.leds)
        step("media key", self.media_key)
        step("mass storage", self.storage)
        step("two sticks mounted together", self.two_sticks)
        step("replug the sticks", self.hotplug)
        step("two exFAT sticks, one pulled out mounted", self.exfat_sticks)
        step("UAS disk", self.uas)
        step("idle CPU", self.cputime)
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
    stick_b = os.path.join(OUT, "stickb.img")
    files = {"abig.bin": os.urandom(300 * 1024),
             "bbig.bin": os.urandom(200 * 1024)}
    make_stick(stick, STICK_SIZE, "STICKA",
               {"abig.bin": files["abig.bin"], "a-hello.txt": b"hello A\n"})
    luns = []
    for n, size in enumerate(LUN_SIZES):
        path = os.path.join(OUT, f"lun{n}.img")
        with open(path, "wb") as f:
            f.write(f"LUN{n}-MARK\n".encode())
            f.truncate(size)
        luns.append(path)
    make_stick(stick_b, STICK_B_SIZE, "STICKB",
               {"bbig.bin": files["bbig.bin"], "b-hello.txt": b"hello B\n"})
    exfat = {w: os.path.join(OUT, f"exfat{w.lower()}.img")
             for w, _, _, _ in EXFAT_STICKS}
    exfiles = {"xbig.bin": os.urandom(150 * 1024),
               "ybig.bin": os.urandom(120 * 1024)}
    make_exfat_stick(exfat["X"], EXFAT_STICKS[0][3],
                     {"xbig.bin": exfiles["xbig.bin"],
                      "x-hello.txt": b"hello X\n"})
    make_exfat_stick(exfat["Y"], EXFAT_STICKS[1][3],
                     {"ybig.bin": exfiles["ybig.bin"]})
    uas_img = os.path.join(OUT, "uas.img")
    uas_files = {"ubig.bin": os.urandom(400 * 1024)}
    make_stick(uas_img, UAS_SIZE, "UASDISK", uas_files)
    sockdir = tempfile.mkdtemp(prefix="susb")
    qmp_path = os.path.join(sockdir, "qmp")
    accel = smoke_gui.pick_accel()
    cmd = ["qemu-system-i386", "-cdrom", os.path.join(ROOT, "maeros.iso"),
           "-drive", f"file={disk},format=raw,if=ide",
           "-accel", accel, "-vga", "none", "-device", f"VGA,id={DISPLAY}", *smokelib.QEMU_DISPLAY,
           "-serial", "stdio", "-m", "512M", "-no-reboot", "-no-shutdown",
           # Keyboard and tablet on root ports; the mouse and stick A
           # behind a (full-speed) hub on root port 3; stick B on port 4.
           # an HDA card for the volume keys' mixer (output discarded)
           "-audiodev", "none,id=hdanull", "-device", "intel-hda",
           "-device", "hda-duplex,audiodev=hdanull",
           # p3=8: root ports 5-8 are SuperSpeed only (the UAS disk)
           "-device", "qemu-xhci,id=xhci,p2=4,p3=8",
           "-device", f"usb-kbd,id={KBD},display={DISPLAY},bus=xhci.0,port=1",
           "-device", f"usb-tablet,id={TABLET},display={DISPLAY},bus=xhci.0,"
                      "port=2",
           "-device", "usb-hub,id=hub,bus=xhci.0,port=3",
           "-device", f"usb-mouse,id={MOUSE},bus=xhci.0,port=3.1",
           "-drive", f"if=none,id=stick,format=raw,file={stick}",
           "-device", f"usb-storage,drive=stick,id={STICK},bus=xhci.0,"
                      "port=3.2",
           "-device", "usb-bot,id=bot,bus=xhci.0,port=3.3",
           "-drive", f"if=none,id=lun0,format=raw,file={luns[0]}",
           "-device", "scsi-hd,bus=bot.0,scsi-id=0,lun=0,drive=lun0",
           "-drive", f"if=none,id=lun1,format=raw,file={luns[1]}",
           "-device", "scsi-hd,bus=bot.0,scsi-id=0,lun=1,drive=lun1",
           "-drive", f"if=none,id=stickb,format=raw,file={stick_b}",
           "-device", f"usb-storage,drive=stickb,id={STICK_B},bus=xhci.0,"
                      "port=4",
           "-drive", f"if=none,id=uasd,format=raw,file={uas_img}",
           "-device", "usb-uas,id=uas,bus=xhci.0,port=5",
           "-device", "scsi-hd,bus=uas.0,scsi-id=0,lun=0,drive=uasd",
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
        smoke.stick_b = stick_b
        smoke.files = files
        smoke.exfat = exfat
        smoke.exfiles = exfiles
        smoke.uas_img = uas_img
        smoke.uas_files = uas_files
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
