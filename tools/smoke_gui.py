#!/usr/bin/env python3
"""smoke-gui — drive the desktop and its apps headlessly and check they work.

Boots the GRUB ISO (the only path with a framebuffer, so the only one that
starts the desktop) with a copy of disk.img, -vga std and -display none, and
drives the PS/2 keyboard and mouse through QMP input-send-event.  The desktop
and the apps print one "[desktop] ..." / "[files] ..." line to the serial
console per state change (window opened/closed, directory entered, settings
applied ...), including the screen position of each control the test clicks,
so the test waits on those lines instead of guessing at pixels or copying
layout constants.  Side effects are cross-checked from a root shell on the
serial getty, and each step takes a screendump with a region check (the
window's area must change when it opens).

Covered: the terminal from the launcher (runs a command whose output is
checked from the serial shell, and whose $$ must be the pid of the shell the
Terminal app started, not the desktop's built-in Console), the image viewer
on /disk/wallpaper.ppm (launched from that terminal), the file manager (enters a directory by
double-click), settings (picks an accent, applies it, checks desktop.conf
and the desktop's reload), the store (verified list or the "run pkg update"
state), the task manager (lists the running processes, including the
desktop and the store), and closing windows with the close button, Esc and
Alt-Tab, after which the focus must pass to the topmost window left.

Output goes to build/smoke-gui/: serial.log, NN-step.png per step, and on a
failure fail.png plus the log tail.  Uses KVM when /dev/kvm is usable, else
TCG; SMOKE_GUI_ACCEL=tcg|kvm overrides.  SMOKE_DISPLAY=1 shows the window.
"""
import json
import os
import re
import selectors
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zlib

import smokelib

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
OUT = os.path.join(ROOT, "build", "smoke-gui")
PROMPT = smokelib.PROMPT

# The diagnostic auto-launch markers on the default disk (desktop.c starts
# Firefox / gtkprobe when one exists); the test wants a quiet desktop.
AUTOSTART_MARKERS = ("ffauto", "gtkauto")


# ── serial console ─────────────────────────────────────────────────────────

class Console:
    """The guest's serial line: everything it printed, and typing into it."""

    def __init__(self, proc):
        self.proc = proc
        self.sel = selectors.DefaultSelector()
        self.sel.register(proc.stdout, selectors.EVENT_READ)
        self.log = []
        self.at = 0          # where the next wait_re() starts looking

    def text(self):
        return "".join(self.log)

    def mark(self):
        return len(self.text())

    def pump(self, timeout):
        for key, _ in self.sel.select(timeout):
            chunk = os.read(key.fd, 4096).decode("latin1", "replace")
            if chunk:
                self.log.append(chunk)
                sys.stdout.write(chunk)
                sys.stdout.flush()
        if self.proc.poll() is not None:
            raise RuntimeError(f"QEMU exited with status {self.proc.returncode}")

    def wait_re(self, pattern, timeout=20.0, start=None):
        """Wait for a line matching `pattern` after offset `start` (default:
        where the previous wait ended).  Returns the match; later waits start
        after it, so a sequence of waits follows the order of events."""
        rx = re.compile(pattern)
        begin = self.at if start is None else start
        deadline = time.time() + timeout
        while True:
            # Complete lines only: the serial line delivers a trace line in
            # pieces, and "close=787,13" must not match before its "0".
            text = self.text()
            m = rx.search(text, begin, text.rfind("\n") + 1)
            if m:
                self.at = m.end()
                return m
            if time.time() >= deadline:
                raise TimeoutError(f"timed out waiting for /{pattern}/")
            self.pump(0.2)

    def run(self, command, timeout=20.0):
        """Run `command` in the root shell on the serial line; returns what
        it printed (the echo of the command line stripped)."""
        before = self.mark()
        smokelib.send(self.proc, command + "\n")
        smokelib.wait_for(self.proc, self.sel, PROMPT, self.log, timeout, before)
        out = self.text()[before:]
        out = out.split("\n", 1)[1] if "\n" in out else ""
        return out[: out.rfind(PROMPT)] if PROMPT in out else out


# ── QMP: input and screendumps ─────────────────────────────────────────────

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
        self.file.readline()                      # greeting
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
            # else an asynchronous event; skip it

    def events(self, evs):
        self.cmd("input-send-event", events=evs)

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def rel(axis, value):
    return {"type": "rel", "data": {"axis": axis, "value": value}}


def btn(down, button="left"):
    return {"type": "btn", "data": {"down": down, "button": button}}


def key(down, qcode):
    return {"type": "key",
            "data": {"down": down, "key": {"type": "qcode", "data": qcode}}}


# Characters the tests type, as (qcode, shifted).
KEYMAP = {" ": ("spc", False), "\n": ("ret", False), "-": ("minus", False),
          "_": ("minus", True), ".": ("dot", False), "/": ("slash", False),
          ">": ("dot", True), "=": ("equal", False), ",": ("comma", False),
          "$": ("4", True), ";": ("semicolon", False)}
for _c in "abcdefghijklmnopqrstuvwxyz":
    KEYMAP[_c] = (_c, False)
for _c in "0123456789":
    KEYMAP[_c] = (_c, False)


class Input:
    """Keyboard and mouse.  The PS/2 mouse is relative, so every move first
    pushes the pointer into the top-left corner (the desktop clamps it
    there) and then moves by the absolute target; the desktop traces each
    click's position, which the test checks."""

    STEP = 120   # per event: stays well inside one PS/2 packet

    def __init__(self, qmp):
        self.qmp = qmp

    def _move(self, dx, dy):
        while dx or dy:
            sx = max(-self.STEP, min(self.STEP, dx))
            sy = max(-self.STEP, min(self.STEP, dy))
            self.qmp.events([rel("x", sx), rel("y", sy)])
            dx -= sx
            dy -= sy
            time.sleep(0.004)

    def move_to(self, x, y):
        self._move(-4000, -4000)
        self._move(x, y)
        time.sleep(0.05)

    def click(self, x, y, double=False):
        self.move_to(x, y)
        for _ in range(2 if double else 1):
            self.qmp.events([btn(True)])
            time.sleep(0.05)
            self.qmp.events([btn(False)])
            time.sleep(0.08)

    def press(self, qcode):
        self.qmp.events([key(True, qcode)])
        time.sleep(0.02)
        self.qmp.events([key(False, qcode)])
        time.sleep(0.02)

    def type(self, text):
        for ch in text:
            qcode, shifted = KEYMAP[ch]
            if shifted:
                self.qmp.events([key(True, "shift")])
            self.press(qcode)
            if shifted:
                self.qmp.events([key(False, "shift")])


# ── screendumps ────────────────────────────────────────────────────────────

class Image:
    def __init__(self, w, h, rgb):
        self.w, self.h, self.rgb = w, h, rgb

    @staticmethod
    def read_ppm(path):
        with open(path, "rb") as f:
            data = f.read()
        fields, pos = [], 0
        while len(fields) < 4:                  # P6 W H MAX, # comments
            while data[pos:pos + 1].isspace():
                pos += 1
            if data[pos:pos + 1] == b"#":
                pos = data.index(b"\n", pos)
                continue
            end = pos
            while not data[end:end + 1].isspace():
                end += 1
            fields.append(data[pos:end])
            pos = end
        if fields[0] != b"P6":
            raise ValueError(f"{path}: not a P6 PPM")
        w, h = int(fields[1]), int(fields[2])
        return Image(w, h, data[pos + 1:pos + 1 + w * h * 3])

    def pixel(self, x, y):
        o = (y * self.w + x) * 3
        return self.rgb[o], self.rgb[o + 1], self.rgb[o + 2]

    def samples(self, x, y, w, h, step=4):
        x0, y0 = max(0, x), max(0, y)
        x1, y1 = min(self.w, x + w), min(self.h, y + h)
        for yy in range(y0, y1, step):
            for xx in range(x0, x1, step):
                yield xx, yy

    def write_png(self, path):
        raw = b"".join(b"\0" + self.rgb[y * self.w * 3:(y + 1) * self.w * 3]
                       for y in range(self.h))

        def chunk(kind, body):
            return (struct.pack(">I", len(body)) + kind + body +
                    struct.pack(">I", zlib.crc32(kind + body) & 0xffffffff))
        with open(path, "wb") as f:
            f.write(b"\x89PNG\r\n\x1a\n")
            f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", self.w, self.h,
                                               8, 2, 0, 0, 0)))
            f.write(chunk(b"IDAT", zlib.compress(raw, 6)))
            f.write(chunk(b"IEND", b""))


def changed_fraction(a, b, rect):
    """Share of sampled pixels in rect (x, y, w, h) that differ between two
    screendumps."""
    total = changed = 0
    for x, y in a.samples(*rect):
        pa, pb = a.pixel(x, y), b.pixel(x, y)
        total += 1
        if max(abs(pa[i] - pb[i]) for i in range(3)) > 24:
            changed += 1
    return changed / total if total else 0.0


def distinct_colors(img, rect):
    return len({img.pixel(x, y) for x, y in img.samples(*rect, step=8)})


# ── the test ───────────────────────────────────────────────────────────────

class GuiSmoke:
    def __init__(self, con, qmp):
        self.con, self.qmp, self.inp = con, qmp, Input(qmp)
        self.shots = 0
        self.fb = (0, 0)
        self.orb = (0, 0)
        self.launched_at = 0

    def shot(self, name):
        """Screendump to <OUT>/NN-name.png; returns the image."""
        self.shots += 1
        ppm = os.path.join(OUT, "screen.ppm")
        self.qmp.cmd("screendump", filename=ppm)
        img = Image.read_ppm(ppm)
        img.write_png(os.path.join(OUT, f"{self.shots:02d}-{name}.png"))
        return img

    def settle(self, seconds=0.6):
        """Let the desktop composite the last change before a screendump."""
        end = time.time() + seconds
        while time.time() < end:
            self.con.pump(0.05)

    def click(self, x, y, double=False):
        self.inp.click(x, y, double)
        m = self.con.wait_re(r"\[desktop\] click (\d+),(\d+)")
        if (int(m.group(1)), int(m.group(2))) != (x, y):
            raise AssertionError(f"pointer landed at {m.group(1)},{m.group(2)}, "
                                 f"not {x},{y}")
        if double:
            self.con.wait_re(r"\[desktop\] click %d,%d" % (x, y))

    def open_launcher(self):
        self.click(*self.orb)
        m = self.con.wait_re(r"\[desktop\] launcher open settings=(\d+),(\d+)")
        return int(m.group(1)), int(m.group(2))

    def wait_window(self, title, timeout=30.0, start=None):
        m = self.con.wait_re(
            r"\[desktop\] window opened: %s slot=(\d+) x=(-?\d+) y=(-?\d+) "
            r"w=(\d+) h=(\d+) close=(\d+),(\d+)" % re.escape(title), timeout,
            start)
        v = [int(g) for g in m.groups()]
        win = {"title": title, "slot": v[0], "x": v[1], "y": v[2], "w": v[3],
               "h": v[4], "close": (v[5], v[6])}
        print(f"\n[SMOKE-GUI] window {title}: {win}")
        return win

    def launch(self, search, title):
        """Open the launcher, type into its search box, press Enter.  The
        app's own trace lines may come before or after the desktop's
        "window opened", so wait for them from self.launched_at."""
        before = self.shot(f"before-{search}")
        self.open_launcher()
        self.launched_at = self.con.mark()
        self.inp.type(search + "\n")
        win = self.wait_window(title)
        self.settle()
        self.check_region_changed(before, win, f"open-{search}")
        return win

    def check_region_changed(self, before, win, name):
        """The window must show up on screen where the desktop says it is:
        its title bar strip changes almost entirely (a dark app over the
        dark Console changes little of its body), and its whole area a
        little."""
        after = self.shot(name)
        bar = changed_fraction(before, after, (win["x"] + 4, win["y"] + 2,
                                               win["w"] - 8, 24))
        area = changed_fraction(before, after,
                                (win["x"], win["y"], win["w"], win["h"]))
        print(f"\n[SMOKE-GUI] {name}: title bar {bar:.0%}, window area "
              f"{area:.0%} changed")
        if bar < 0.5 or area < 0.03:
            raise AssertionError(f"{name}: {win['title']} did not appear on "
                                 f"screen (title bar {bar:.0%}, area {area:.0%} "
                                 f"changed)")
        return after

    def focused_slot(self):
        """Slot of the window the desktop last reported focused (0 for the
        Console and System windows).  Focus lines name the slot because a
        client's title can still be the slot's previous one when it is
        focused."""
        text = self.con.text()
        found = re.findall(r"\[desktop\] focus .* slot=(\d+)\n",
                           text[:text.rfind("\n") + 1])
        return int(found[-1]) if found else 0

    def expect_focus(self, win, timeout=5.0):
        deadline = time.time() + timeout
        while self.focused_slot() != win["slot"]:
            if time.time() >= deadline:
                raise AssertionError(f"{win['title']} (slot {win['slot']}) "
                                     f"does not have the focus (slot "
                                     f"{self.focused_slot()} does)")
            self.con.pump(0.1)

    def bring_forward(self, win):
        """Alt-Tab until `win` has the focus (and so is on top)."""
        for _ in range(8):
            start = self.con.mark()
            self.qmp.events([key(True, "alt")])
            self.inp.press("tab")
            self.qmp.events([key(False, "alt")])
            m = self.con.wait_re(r"\[desktop\] focus .* slot=(\d+)\n",
                                 start=start)
            if int(m.group(1)) == win["slot"]:
                return
        raise AssertionError(f"Alt-Tab never focused {win['title']}")

    def close(self, win, how="button", then_focus=None):
        """Close `win`; with then_focus, the desktop must hand the focus to
        that window (the topmost one left), not raise the Console."""
        if how == "alttab-button":
            self.bring_forward(win)
            how = "button"
        if how == "button":
            self.click(*win["close"])
        else:
            self.inp.press("esc")
        self.con.wait_re(r"\[desktop\] window closed: %s slot=%d\b"
                         % (re.escape(win["title"]), win["slot"]))
        self.con.wait_re(r"\[desktop\] app exited: slot=%d\b" % win["slot"],
                         timeout=15)
        if then_focus:
            self.expect_focus(then_focus)

    def rel(self, win, x, y):
        return win["x"] + x, win["y"] + y

    # -- steps --

    def boot(self):
        smokelib.login(self.con.proc, self.con.sel, self.con.log, timeout=120)
        m = self.con.wait_re(r"\[desktop\] ready fb=(\d+)x(\d+) orb=(\d+),(\d+)",
                             timeout=90, start=0)
        self.fb = (int(m.group(1)), int(m.group(2)))
        self.orb = (int(m.group(3)), int(m.group(4)))
        self.settle(1.5)
        img = self.shot("desktop")
        if (img.w, img.h) != self.fb:
            raise AssertionError(f"screendump is {img.w}x{img.h}, desktop "
                                 f"reported {self.fb[0]}x{self.fb[1]}")
        colors = distinct_colors(img, (0, 0, img.w, img.h))
        if colors < 50:
            raise AssertionError(f"desktop screen has only {colors} colours")

    def type_in_terminal(self, win, command, proof):
        """Type `command` into the Terminal app and prove its own shell ran
        it: the command also writes $$ to `proof`, which must be the pid of
        the shell the Terminal started, not of the desktop's built-in
        Console (also a shell on a pty, which gets keys when it has the
        focus)."""
        self.expect_focus(win)
        self.con.run(f"rm -f {proof}")
        self.inp.type(f"echo $$ > {proof}; {command}\n")
        deadline = time.time() + 20
        while True:
            # The console also carries kernel "[SYSCALL] exec" lines.
            got = self.con.run(f"cat {proof}")
            if str(win["shell_pid"]) in re.findall(r"^(\d+)\r?$", got, re.M):
                return
            if time.time() >= deadline:
                raise AssertionError(
                    f"{command!r} did not run in the Terminal's shell (pid "
                    f"{win['shell_pid']}); {proof} holds {got!r}")
            self.settle(0.5)

    def terminal(self):
        win = self.launch("term", "Terminal")
        m = self.con.wait_re(r"\[term\] shell pid=(\d+) tty=(\S+)",
                             start=self.launched_at)
        win["shell_pid"] = int(m.group(1))
        self.type_in_terminal(win, "echo guismoke-term-ok > /tmp/guismoke.out",
                              "/tmp/guismoke.term")
        out = self.con.run("cat /tmp/guismoke.out")
        if "guismoke-term-ok" not in out:
            raise AssertionError(f"terminal command wrote {out!r}")
        self.shot("term-command")
        return win

    def viewer(self, term):
        """Launch the viewer from the terminal, which still has the focus."""
        before = self.shot("before-view")
        start = self.con.mark()
        self.type_in_terminal(term, "wmctl launch view /disk/wallpaper.ppm",
                              "/tmp/guismoke.view")
        # The viewer loads the image after its window is up; the two lines
        # come from different processes, so do not rely on their order.
        m = self.con.wait_re(r"\[view\] (.*)\n", timeout=30, start=start)
        if not re.match(r"/disk/wallpaper\.ppm +\(\d+x\d+\)", m.group(1)):
            raise AssertionError(f"viewer did not load the image: {m.group(1)}")
        win = self.wait_window("Viewer", start=start)
        self.settle()
        after = self.check_region_changed(before, win, "view")
        # The body must show the picture, not the viewer's plain backdrop.
        body = (win["x"] + 10, win["y"] + 40, win["w"] - 20, win["h"] - 80)
        colors = distinct_colors(after, body)
        if colors < 30:
            raise AssertionError(f"viewer body has only {colors} colours")
        return win

    def files(self):
        win = self.launch("files", "Files")
        self.con.wait_re(r"\[files\] cwd / entries=(\d+)",
                         start=self.launched_at)
        m = self.con.wait_re(r"\[files\] dir (\S+) at (\d+),(\d+)")
        name = m.group(1)
        before = self.shot("files-root")
        self.click(*self.rel(win, int(m.group(2)), int(m.group(3))), double=True)
        self.con.wait_re(r"\[files\] cwd /%s entries=\d+" % re.escape(name))
        self.settle()
        after = self.shot("files-entered")
        if changed_fraction(before, after, (win["x"], win["y"], win["w"],
                                            win["h"])) < 0.01:
            raise AssertionError("Files window did not repaint after entering "
                                 f"/{name}")
        return win

    def settings(self):
        before = self.shot("before-settings")
        target = self.open_launcher()
        self.launched_at = self.con.mark()
        self.click(*target)
        win = self.wait_window("Settings")
        self.settle()
        self.check_region_changed(before, win, "open-settings")
        m = self.con.wait_re(r"\[settings\] ready walls=\d+ accent5=(\d+),(\d+) "
                             r"apply=(\d+),(\d+)", start=self.launched_at)
        accent = self.rel(win, int(m.group(1)), int(m.group(2)))
        apply_btn = self.rel(win, int(m.group(3)), int(m.group(4)))
        self.click(*accent)
        self.con.wait_re(r"\[settings\] accent 5 selected")
        self.click(*apply_btn)
        self.con.wait_re(r"\[settings\] applied wallpaper=-1 accent=5")
        self.con.wait_re(r"\[desktop\] conf reloaded accent=#4c9a8f")
        conf = self.con.run("cat /disk/etc/desktop.conf")
        if "accent=#4c9a8f" not in conf:
            raise AssertionError(f"desktop.conf lacks the new accent: {conf!r}")
        self.settle()
        self.shot("settings-applied")
        return win

    def store(self):
        win = self.launch("store", "Store")
        m = self.con.wait_re(r"\[store\] (verified list: (\d+) packages|"
                             r"unverified list \(.*\): run pkg update|"
                             r"no package list: run pkg update)",
                             start=self.launched_at)
        if m.group(2) is None:
            # No verified list yet: the store runs `pkg update` by itself
            # (no network here, so it fails) and must still say what to do.
            self.con.wait_re(r"\[store\] pkg update exit=\d+", timeout=60)
            self.con.wait_re(r"\[store\] (verified list: \d+ packages|"
                             r"unverified list \(.*\): run pkg update|"
                             r"no package list: run pkg update)")
        self.settle()
        self.shot("store")
        return win

    def taskmgr(self):
        win = self.launch("task", "Task Manager")
        m = self.con.wait_re(r"\[taskmgr\] listed (\d+) processes: (\S*)",
                             start=self.launched_at)
        count, names = int(m.group(1)), m.group(2).split(",")
        for want in ("init", "desktop", "store"):
            if not any(n.endswith(want) for n in names):
                raise AssertionError(f"task manager does not list {want}: "
                                     f"{names}")
        if count < 5:
            raise AssertionError(f"task manager lists only {count} processes")
        return win

    def run(self):
        steps = []

        def step(name, fn, *args):
            t0 = time.time()
            result = fn(*args)
            steps.append((name, time.time() - t0))
            print(f"\n[SMOKE-GUI] step {name}: ok ({time.time() - t0:.1f}s)")
            return result

        step("boot", self.boot)
        term = step("terminal", self.terminal)
        view = step("viewer", self.viewer, term)
        step("close viewer (button)", self.close, view, "button", term)
        step("close terminal (button)", self.close, term)
        files = step("files", self.files)
        step("close files (button)", self.close, files)
        settings = step("settings", self.settings)
        step("close settings (Esc)", self.close, settings, "esc")
        store = step("store", self.store)
        tm = step("taskmgr", self.taskmgr)
        step("close task manager (button)", self.close, tm, "button", store)
        step("close store (Alt-Tab, button)", self.close, store,
             "alttab-button")
        self.settle()
        self.shot("final")
        return steps


def find_debugfs():
    for c in ("debugfs", "/sbin/debugfs", "/usr/sbin/debugfs"):
        p = shutil.which(c)
        if p:
            return p
    return None


def prepare_disk():
    src = os.path.join(ROOT, "disk.img")
    dst = os.path.join(OUT, "disk.img")
    shutil.copyfile(src, dst)
    debugfs = find_debugfs()
    if not debugfs:
        raise RuntimeError("debugfs (e2fsprogs) is needed to prepare the disk")
    for marker in AUTOSTART_MARKERS:
        subprocess.run([debugfs, "-w", "-R", f"rm {marker}", dst],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return dst


def pick_accel():
    want = os.environ.get("SMOKE_GUI_ACCEL")
    if want:
        return want
    return "kvm" if os.access("/dev/kvm", os.R_OK | os.W_OK) else "tcg"


def main():
    shutil.rmtree(OUT, ignore_errors=True)
    os.makedirs(OUT)
    for f in ("maeros.iso", "disk.img"):
        if not os.path.exists(os.path.join(ROOT, f)):
            raise RuntimeError(f"{f} is missing (make iso disk)")
    disk = prepare_disk()
    sockdir = tempfile.mkdtemp(prefix="sgui")   # AF_UNIX paths are short
    qmp_path = os.path.join(sockdir, "qmp")
    accel = pick_accel()
    cmd = ["qemu-system-i386", "-cdrom", os.path.join(ROOT, "maeros.iso"),
           "-drive", f"file={disk},format=raw,if=ide",
           "-accel", accel, "-vga", "std", *smokelib.QEMU_DISPLAY,
           "-serial", "stdio", "-m", "512M", "-no-reboot", "-no-shutdown",
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
        smoke = GuiSmoke(con, qmp)
        steps = smoke.run()
        total = time.time() - t0
        print("\n[SMOKE-GUI] timings: " +
              ", ".join(f"{n} {s:.1f}s" for n, s in steps))
        print(f"[SMOKE-GUI] passed in {total:.1f}s (accel={accel}); "
              f"screendumps in {os.path.relpath(OUT, ROOT)}/")
        return 0
    except Exception:
        if smoke is not None:
            try:
                ppm = os.path.join(OUT, "screen.ppm")
                qmp.cmd("screendump", filename=ppm)
                Image.read_ppm(ppm).write_png(os.path.join(OUT, "fail.png"))
            except Exception as exc:          # QEMU may be gone
                print(f"\n[SMOKE-GUI] no failure screendump: {exc}",
                      file=sys.stderr)
        tail = con.text()[-3000:]
        print("\n[SMOKE-GUI] last serial output:\n" + tail, file=sys.stderr)
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
        print(f"\n[SMOKE-GUI] failed: {exc}; see {os.path.relpath(OUT, ROOT)}/"
              f"serial.log and fail.png", file=sys.stderr)
        raise SystemExit(1)
