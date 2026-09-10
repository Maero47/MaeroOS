#!/usr/bin/env python3
"""smoke-firefox: does Firefox 115 paint a window on the MaeroOS desktop?

Boots the GRUB ISO (framebuffer-capable) with the Firefox disk attached,
headless, and decides PASS/FAIL from the serial console:

  * With a framebuffer and the disk userland, /disk/init runs /etc/rc, the
    services, and then the graphical session (/disk/desktop) as the user; no
    shell prompt reaches the serial line while the desktop runs.  The desktop
    auto-launches /disk/ff a few seconds after its window manager is up when
    the marker file /disk/ffauto exists (testfiles/ffauto is committed, so
    disk-ff.img carries it).  The desktop inherits init's console, so
    everything ff prints lands on the serial line.
  * ff starts maeroX in a desktop slot, launches firefox-bin, and waits for
    maeroX to drop /tmp/ff_painted on the first PutImage.  It prints
    "ff: Firefox painted ..." only after a 5 s grace check that the browser
    process is still alive (a crash-reporter dialog painting does not count),
    otherwise it reports the attempt's stall/crash, dumps the moz.log tail and
    retries (20 attempts, 40 s each), ending in "ff: gave up ...".

Verdict:
  PASS  "ff: Firefox painted" seen                          -> exit 0
  FAIL  "ff: gave up", a kernel panic, QEMU dying, the launcher never
        starting, or the overall timeout                    -> exit 1
  ERROR the harness could not run (missing images/QEMU)     -> exit 2

Artifacts go to build/ff-smoke/<timestamp>-<tag>/ (gitignored):
  serial.log        the complete serial console, verbatim
  screen.png        the last VGA screendump (screen.ppm if PNG is impossible)
  screen-paint.png  the richest of several frames sampled over --hold seconds
                    after the paint line (PASS only) — the paint marker fires on
                    the first PutImage, so one immediate dump can catch an empty
                    window
  screen-typed.png  with --type TEXT only: the frame after TEXT was typed into
                    Firefox's address bar through QEMU sendkey
  summary.txt       verdict, timings, attempt list, crash lines, last kernel
                    lines, moz.log tail, and with --keycheck the key-trace result
  qemu-cmdline.txt  the exact QEMU command

Usage:
  python3 tools/smoke_firefox.py [--accel kvm|tcg|<qemu -accel value>]
                                 [--smp N] [--timeout SEC] [--mem SIZE]
                                 [--iso PATH] [--disk PATH] [--out DIR]
                                 [--tag NAME] [-v]
Defaults: -accel kvm when /dev/kvm is writable, else tcg; -smp 1; -m 2048M;
timeout 360 s under KVM, 900 s under TCG.
"""
import argparse
import datetime
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

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))

# ── serial-line patterns ─────────────────────────────────────────────────
GFX_START   = "[init] Starting graphical session"
GFX_NONE    = "[init] Graphics unavailable"
DESKTOP_END = "[init] Graphical session ended"
FF_EXEC     = "exec '/disk/ff'"
FF_BANNER   = "Starting maeroX X server in a desktop slot"
FF_LAUNCH   = "Launching Firefox 115"
FFBIN_EXEC  = "exec '/disk/firefox/firefox-bin'"
FF_PAINTED  = "ff: Firefox painted"
FF_GAVE_UP  = "ff: gave up"
FF_STALLED  = "ff: Firefox stalled"
FF_EXITED   = "ff: Firefox exited"
FF_CRASHREP = "ff: paint was the crash reporter"
PANIC       = "=== KERNEL PANIC ==="
PAINT_SHOTS = 10           # frames sampled across --hold to pick screen-paint
MOZ_TAIL_BEGIN = "=== tail("
MOZ_TAIL_END   = "=== end tail ==="
SIG_KILLED  = re.compile(r"\[SIG\] pid=(\d+) killed by signal (\d+)")
RESTART_N   = re.compile(r"restart (\d+)/(\d+)")
PAINT_ATT   = re.compile(r"\(attempt (\d+)\)")
STATUS_N    = re.compile(r"status=(-?\d+)")
KERNEL_LINE = re.compile(r"^\[[A-Za-z_]+\]")
ASSERT_PAT  = re.compile(r"(ASSERT|assert(ion)? failed|Unhandled exception|Double fault)", re.I)


def kvm_usable():
    return os.access("/dev/kvm", os.R_OK | os.W_OK)


def fmt_t(t):
    return "%7.1fs" % t if t is not None else "      - "


def read_ppm(path):
    """Parse a binary P6 PPM.  Returns (width, height, rgb bytes) or None."""
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError:
        return None
    if not data.startswith(b"P6"):
        return None
    fields, i = [], 2
    while len(fields) < 3 and i < len(data):
        while i < len(data) and data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b"#":                      # comment to end of line
            while i < len(data) and data[i:i + 1] != b"\n":
                i += 1
            continue
        start = i
        while i < len(data) and not data[i:i + 1].isspace():
            i += 1
        try:
            fields.append(int(data[start:i]))
        except ValueError:
            return None
    if len(fields) < 3:
        return None
    i += 1                                             # single whitespace byte
    w, h, maxval = fields
    if maxval != 255 or w <= 0 or h <= 0:
        return None
    rgb = data[i:i + w * h * 3]
    if len(rgb) < w * h * 3:
        return None
    return w, h, rgb


def frame_detail(frame):
    """How much of the browser is drawn in this frame: near-white pixel count.

    Firefox's chrome and its blank about:blank content area are a large light
    rectangle; everything else on this desktop is dark or saturated (blue
    wallpaper, black console, maeroX's own dark background when it has no
    client content to composite).  Measured on real screendumps: a frame
    showing the chrome scores 134k-136k, one where the maeroX window is still
    empty scores 27k-42k.

    Counting distinct colours was tried first and is useless here - the
    wallpaper gradient alone contributes thousands, so a painted frame (5180)
    and a blank one (5116) are indistinguishable.
    """
    if not frame:
        return -1
    w, h, rgb = frame
    stride = w * 3
    n = 0
    for y in range(0, h, 2):
        row = rgb[y * stride:(y + 1) * stride]
        for x in range(0, len(row) - 2, 6):
            r, g, b = row[x], row[x + 1], row[x + 2]
            if r >= 200 and g >= 200 and b >= 200 and max(r, g, b) - min(r, g, b) <= 24:
                n += 1
    return n


def write_png(path, frame):
    """Write an RGB frame as a PNG.  No third-party imaging library needed."""
    w, h, rgb = frame
    stride = w * 3
    raw = bytearray()
    for y in range(h):
        raw.append(0)                                  # filter type 0 (None)
        raw += rgb[y * stride:(y + 1) * stride]

    def chunk(tag, payload):
        return (struct.pack(">I", len(payload)) + tag + payload +
                struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(bytes(raw), 6)))
        f.write(chunk(b"IEND", b""))


class Qmp:
    """Minimal QMP client over a UNIX socket (screendump only)."""

    def __init__(self, path):
        self.path = path
        self.sock = None

    def connect(self, deadline):
        while time.time() < deadline:
            if os.path.exists(self.path):
                try:
                    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                    s.settimeout(5.0)
                    s.connect(self.path)
                    self.sock = s
                    self._recv()                       # greeting
                    self._cmd("qmp_capabilities")
                    return True
                except (OSError, ValueError):
                    self.sock = None
            time.sleep(0.2)
        return False

    def _recv(self):
        buf = b""
        while True:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise OSError("qmp closed")
            buf += chunk
            for line in buf.split(b"\n"):
                if not line.strip():
                    continue
                try:
                    msg = json.loads(line)
                except ValueError:
                    continue
                if "return" in msg or "error" in msg or "QMP" in msg:
                    return msg

    def _cmd(self, name, **args):
        req = {"execute": name}
        if args:
            req["arguments"] = args
        self.sock.sendall((json.dumps(req) + "\n").encode())
        return self._recv()

    def screendump_ppm(self, path_ppm):
        """Dump one frame as PPM (easy to parse and score).  Path or None."""
        if not self.sock:
            return None
        try:
            r = self._cmd("screendump", filename=path_ppm)
        except OSError:
            return None
        if "return" in r and os.path.exists(path_ppm) and os.path.getsize(path_ppm) > 0:
            return path_ppm
        return None

    def screendump(self, path_png, path_ppm):
        """Return the path written (png preferred, ppm fallback) or None."""
        if not self.sock:
            return None
        try:
            r = self._cmd("screendump", filename=path_png, format="png")
            if "return" in r and os.path.exists(path_png) and os.path.getsize(path_png) > 0:
                return path_png
            r = self._cmd("screendump", filename=path_ppm)
            if "return" in r and os.path.exists(path_ppm) and os.path.getsize(path_ppm) > 0:
                try:
                    from PIL import Image
                    Image.open(path_ppm).save(path_png)
                    os.unlink(path_ppm)
                    return path_png
                except Exception:
                    return path_ppm
        except (OSError, ValueError):
            self.sock = None
        return None

    def hmp(self, command):
        """Run one HMP monitor command on the live guest and return its text.

        This is the only view into a wedged guest that does not need the guest
        to cooperate: 'info registers' says whether the CPU is in ring 0 or
        ring 3 and whether EIP moves, 'info cpus' whether it is halted."""
        if not self.sock:
            return "(no qmp)"
        try:
            r = self._cmd("human-monitor-command", **{"command-line": command})
        except OSError:
            self.sock = None
            return "(qmp closed)"
        if "return" in r:
            return r["return"] or ""
        return "(error: %s)" % r.get("error")

    def close(self):
        if self.sock:
            try:
                self.sock.close()
            except OSError:
                pass
            self.sock = None


# ── typing into the guest ────────────────────────────────────────────────
# QEMU's monitor `sendkey` injects a PS/2 scancode pair, so a key typed this way
# travels the whole real path: i8042 -> drivers/keyboard.c -> /dev/input/event0
# -> the desktop -> the WM event channel -> maeroX -> an X11 KeyPress -> GTK.
# Nothing about it is synthetic on the guest side.
QEMU_KEYNAME = {
    " ": "spc", ".": "dot", ",": "comma", "/": "slash", "-": "minus",
    "=": "equal", ";": "semicolon", "'": "apostrophe", "[": "bracket_left",
    "]": "bracket_right", "\\": "backslash", "`": "grave_accent",
    "\n": "ret", "\t": "tab",
}
QEMU_SHIFTED = {
    "!": "1", "@": "2", "#": "3", "$": "4", "%": "5", "^": "6", "&": "7",
    "*": "8", "(": "9", ")": "0", "_": "minus", "+": "equal", ":": "semicolon",
    '"': "apostrophe", "<": "comma", ">": "dot", "?": "slash", "{": "bracket_left",
    "}": "bracket_right", "|": "backslash", "~": "grave_accent",
}


def qemu_keys_for(text):
    """Turn a string into a list of QEMU `sendkey` arguments, or raise."""
    keys = []
    for ch in text:
        if ch.islower() or ch.isdigit():
            keys.append(ch)
        elif ch.isupper():
            keys.append("shift-" + ch.lower())
        elif ch in QEMU_KEYNAME:
            keys.append(QEMU_KEYNAME[ch])
        elif ch in QEMU_SHIFTED:
            keys.append("shift-" + QEMU_SHIFTED[ch])
        else:
            raise ValueError("no QEMU key name for %r" % ch)
    return keys


def capture_wedge(qmp, outdir, samples=4, gap=0.75):
    """Photograph a wedged guest from outside it.

    Four register samples a fraction of a second apart answer the first
    question a silent hang poses: is the CPU spinning (EIP moves, or moves
    within a small range), halted with nothing to do (halted=1), or stuck at
    one instruction with interrupts off (EIP identical, halted=0)."""
    L = ["wedge capture (QEMU monitor, guest not consulted)", "=" * 72,
         "EFL's IF bit is the one to read first: this kernel enters syscalls",
         "through an interrupt gate, so IF=0 with CPL=0 means the guest is",
         "spinning inside a syscall and no timer tick can ever land.  'info pic'",
         "then shows IRQ0 sitting in irr, never delivered.",
         ""]
    for k in range(samples):
        L.append("--- sample %d ---" % k)
        L.append(qmp.hmp("info cpus"))
        L.append(qmp.hmp("info registers"))
        L.append(qmp.hmp("info pic"))
        L.append(qmp.hmp("x/8i $pc"))
        if k + 1 < samples:
            time.sleep(gap)
    L.append("--- info mem ---")
    L.append(qmp.hmp("info mem"))
    path = os.path.join(outdir, "wedge-qmp.txt")
    with open(path, "w") as f:
        f.write("\n".join(x if isinstance(x, str) else str(x) for x in L) + "\n")
    return path


class Run:
    def __init__(self, args):
        self.args = args
        self.t0 = None
        self.lines = []            # (t, line) for every complete serial line
        self.partial = ""
        self.raw = open(os.path.join(args.outdir, "serial.log"), "wb")
        # timings / evidence
        self.t_gfx = None
        self.t_ff = None
        self.t_ff_launch = None
        self.t_firefox_exec = []
        self.t_paint = None
        self.paint_attempt = None
        self.attempts = []         # (t, kind, status, n, total)
        self.crash_lines = []      # (t, line)
        self.panic_lines = []
        self.assert_lines = []
        self.moz_tail = []
        self.moz_tails_seen = 0
        self._in_tail = False
        self._tail_buf = []
        self.desktop_ended = None
        self.xt_last = None        # last "XT hist" line from maeroX (putimg= counter)
        self.t_xwindow = None      # first Firefox toplevel seen by maeroX
        self.result = None         # PASS / FAIL / ERROR
        self.reason = ""
        self.shots = {}
        self.paint_scores = []
        self.wedge_qmp = None
        self.type_note = None
        self.key_notes = None
        self.key_ok = None

    # ── serial intake ────────────────────────────────────────────────────
    def feed(self, chunk):
        self.raw.write(chunk)
        self.raw.flush()
        text = self.partial + chunk.decode("utf-8", "replace")
        parts = text.split("\n")
        self.partial = parts.pop()
        now = time.time() - self.t0
        for line in parts:
            line = line.rstrip("\r")
            self.lines.append((now, line))
            self.on_line(now, line)

    def echo(self, t, line):
        sys.stdout.write("[%6.1fs] %s\n" % (t, line[:200]))
        sys.stdout.flush()

    def on_line(self, t, line):
        interesting = False
        if self._in_tail:
            if MOZ_TAIL_END in line:
                self._in_tail = False
                self.moz_tail = self._tail_buf
                self.moz_tails_seen += 1
            else:
                self._tail_buf.append(line)
            return
        if MOZ_TAIL_BEGIN in line:
            self._in_tail = True
            self._tail_buf = []
            return
        if self.t_gfx is None and GFX_START in line:
            self.t_gfx = t; interesting = True
        elif GFX_NONE in line:
            self.result, self.reason = "ERROR", "kernel booted without a framebuffer (no graphical session): boot the ISO, not -kernel"
            interesting = True
        elif DESKTOP_END in line:
            self.desktop_ended = t; interesting = True
        elif self.t_ff is None and (FF_EXEC in line or FF_BANNER in line):
            self.t_ff = t; interesting = True
        elif FF_LAUNCH in line:
            if self.t_ff_launch is None:
                self.t_ff_launch = t
            interesting = True
        elif FFBIN_EXEC in line:
            self.t_firefox_exec.append(t); interesting = True
        elif FF_PAINTED in line:
            self.t_paint = t
            m = PAINT_ATT.search(line)
            self.paint_attempt = int(m.group(1)) if m else None
            self.result, self.reason = "PASS", line.strip()
            interesting = True
        elif FF_GAVE_UP in line:
            self.result, self.reason = "FAIL", line.strip()
            interesting = True
        elif FF_STALLED in line or FF_EXITED in line or FF_CRASHREP in line:
            kind = "stalled" if FF_STALLED in line else ("exited" if FF_EXITED in line else "crash-reporter-paint")
            st = STATUS_N.search(line)
            rn = RESTART_N.search(line)
            self.attempts.append((t, kind, int(st.group(1)) if st else None,
                                  int(rn.group(1)) if rn else None,
                                  int(rn.group(2)) if rn else None))
            interesting = True
        elif PANIC in line:
            self.panic_lines.append(line)
            self.result, self.reason = "FAIL", "kernel panic"
            interesting = True
        elif self.panic_lines and len(self.panic_lines) < 30:
            self.panic_lines.append(line)
        elif line.startswith("XT hist"):
            self.xt_last = (t, line.strip())
        elif line.startswith("ff:") or line.startswith("[init]") or line.startswith("maerox:"):
            if self.t_xwindow is None and "ChangeProperty" in line and "firefox" in line.lower():
                self.t_xwindow = t
            interesting = True
        m = SIG_KILLED.search(line)
        if m:
            self.crash_lines.append((t, line.strip()))
            interesting = True
        if ASSERT_PAT.search(line) and not line.startswith("[ftrace]"):
            self.assert_lines.append((t, line.strip()))
            interesting = True
        if interesting or self.args.verbose:
            self.echo(t, line)

    def kernel_tail(self, n=40):
        out = [l for _, l in self.lines if KERNEL_LINE.match(l)]
        return out[-n:]

    # ── summary ──────────────────────────────────────────────────────────
    def write_summary(self, qemu_cmd, accel, elapsed):
        a = self.args
        L = []
        L.append("smoke-firefox summary")
        L.append("=" * 72)
        L.append("result      : %s" % self.result)
        L.append("reason      : %s" % self.reason)
        L.append("date        : %s" % datetime.datetime.now().isoformat(timespec="seconds"))
        L.append("accel       : %s   smp=%d   mem=%s   timeout=%ds   elapsed=%.0fs"
                 % (accel, a.smp, a.mem, a.timeout, elapsed))
        L.append("iso/disk    : %s  %s" % (a.iso, a.disk))
        L.append("artifacts   : %s" % a.outdir)
        L.append("")
        L.append("timeline (seconds since QEMU start)")
        L.append("  graphical session started : %s" % fmt_t(self.t_gfx))
        L.append("  ff launcher started       : %s" % fmt_t(self.t_ff))
        L.append("  first firefox-bin exec    : %s" % fmt_t(self.t_firefox_exec[0] if self.t_firefox_exec else None))
        L.append("  first paint (ff verdict)  : %s%s" % (
            fmt_t(self.t_paint),
            ("   (%.1fs after the launcher started)" % (self.t_paint - self.t_ff))
            if self.t_paint is not None and self.t_ff is not None else ""))
        L.append("  first Firefox X window    : %s" % fmt_t(self.t_xwindow))
        if self.desktop_ended is not None:
            L.append("  desktop session ended     : %s" % fmt_t(self.desktop_ended))
        L.append("")
        L.append("maeroX last trace line (putimg= is the PutImage count; 0 = nothing drawn)")
        L.append("  %s  %s" % (fmt_t(self.xt_last[0]), self.xt_last[1]) if self.xt_last else "  (none)")
        L.append("")
        L.append("attempts")
        L.append("  firefox-bin launched      : %d time(s)" % len(self.t_firefox_exec))
        L.append("  attempts concluded        : %d" % len(self.attempts))
        for (t, kind, st, n, tot) in self.attempts:
            L.append("  %s  attempt %s/%s  %-20s status=%s" % (
                fmt_t(t), n if n is not None else "?", tot if tot is not None else "?",
                kind, st if st is not None else "-"))
        if self.paint_attempt is not None:
            L.append("  painted on attempt        : %d" % self.paint_attempt)
        L.append("")
        L.append("crash lines ([SIG] killed by signal): %d" % len(self.crash_lines))
        for t, l in self.crash_lines[-40:]:
            L.append("  %s  %s" % (fmt_t(t), l))
        if self.assert_lines:
            L.append("")
            L.append("assert/exception lines: %d" % len(self.assert_lines))
            for t, l in self.assert_lines[-20:]:
                L.append("  %s  %s" % (fmt_t(t), l))
        if self.panic_lines:
            L.append("")
            L.append("kernel panic")
            L.extend("  " + l for l in self.panic_lines)
        L.append("")
        L.append("last 40 kernel/init trace lines")
        L.extend("  " + l for l in self.kernel_tail(40))
        L.append("")
        L.append("moz.log tail (%d dump(s) seen; last one below)" % self.moz_tails_seen)
        if self.moz_tail:
            L.extend("  " + l for l in self.moz_tail[-60:])
        else:
            L.append("  (none captured)")
        L.append("")
        if self.paint_scores:
            L.append("paint-frame light-pixel scores (>~100000 = chrome visible, "
                     "<~50000 = the maeroX window was blank in that frame)")
            L.append("  " + " ".join(str(x) for x in self.paint_scores))
            L.append("")
        if self.key_notes:
            L.append("key checks")
            for n in self.key_notes:
                L.append("  " + n)
            L.append("")
        if self.type_note:
            L.append("typing      : %s" % self.type_note)
            L.append("")
        if self.wedge_qmp:
            L.append("wedge capture : %s" % self.wedge_qmp)
            L.append("")
        L.append("screendumps")
        for k, v in self.shots.items():
            L.append("  %-6s %s" % (k, v))
        L.append("")
        L.append("qemu: " + " ".join(qemu_cmd))
        text = "\n".join(L) + "\n"
        with open(os.path.join(a.outdir, "summary.txt"), "w") as f:
            f.write(text)
        return text


# maeroX's own trace of every key event it delivered, one line each:
#   XT key code=30(x38) press state=0x8 -> win=0x200012
XT_KEY = re.compile(r"XT key code=(\d+)\(x(\d+)\) (press|release) "
                    r"state=0x([0-9a-fA-F]+) -> win=0x([0-9a-fA-F]+)")
# maeroX says once, at startup, whether it has a key-injection channel and
# whether the node exists on disk: "XT keychannel: absent (no node)".
XT_KEYCHANNEL = re.compile(r"XT keychannel: (\S+) \(([^)]*)\)")


def check_key_trace(run):
    """Replay maeroX's key trace and check what a client would have seen.

    Two things are asserted.  First the pairing invariant: every KeyPress is
    matched by a KeyRelease for the same keycode on the same window, none
    arrives unmatched, and nothing is left held — an unpaired press leaves the
    client believing a key is still down.  Second, that right Alt reaches the
    client as Mod1Mask and that Alt-Tab, which the desktop keeps for itself,
    delivered a complete Alt pair and no Tab at all.

    Returns (ok, [notes])."""
    events = []
    for _, line in run.lines:
        m = XT_KEY.search(line)
        if m:
            events.append((int(m.group(1)), int(m.group(2)), m.group(3),
                           int(m.group(4), 16), m.group(5)))
    notes = ["maeroX delivered %d key events" % len(events)]
    if not events:
        return False, notes + ["FAIL no key events reached maeroX at all"]

    held, bad = {}, []
    for code, _kc, edge, _state, win in events:
        if edge == "press":
            if code in held:
                bad.append("keycode %d pressed twice with no release" % code)
            held[code] = win
        else:
            if code not in held:
                bad.append("keycode %d released with no press" % code)
            elif held.pop(code) != win:
                bad.append("keycode %d pressed and released on different windows" % code)
    for code in held:
        bad.append("keycode %d never released" % code)

    # KEY_A pressed while KEY_RIGHTALT (100) was held must carry Mod1Mask (0x8).
    altgr = [e for e in events if e[0] == 30 and e[2] == "press" and e[3] & 0x8]
    # KEY_LEFTALT (56) from the Alt-Tab: a complete pair, and no KEY_TAB (15).
    alt_edges = [e[2] for e in events if e[0] == 56]
    tab_events = [e for e in events if e[0] == 15]

    if bad:
        notes += ["FAIL pairing: " + b for b in bad]
    else:
        notes.append("pairing ok: every press matched a release on the same window")
    if altgr:
        notes.append("right Alt sets Mod1Mask: 'a' arrived with state=0x%x" % altgr[0][3])
    else:
        notes.append("FAIL right Alt did not set Mod1Mask on the following key")
    if alt_edges.count("press") == 1 and alt_edges.count("release") == 1:
        notes.append("Alt-Tab delivered a complete Alt press/release pair")
    else:
        notes.append("FAIL Alt-Tab left Alt edges %r" % (alt_edges,))
    if tab_events:
        notes.append("FAIL Tab reached the client although the desktop consumed it")
    else:
        notes.append("Tab was kept by the desktop, and no half of it leaked")

    # The key-injection channel is a test-only path.  A desktop session must not
    # have one — otherwise anything on the machine could type into the browser,
    # and the typed-text evidence below would not be proof of the real keyboard
    # path at all.  maeroX reports what it found on the filesystem, not just its
    # own flag, so a node left by anything else would show up here too.
    chan = [m.groups() for m in
            (XT_KEYCHANNEL.search(line) for _, line in run.lines) if m]
    if not chan:
        notes.append("FAIL maeroX never reported its key-injection channel state")
    elif any(state == "OPEN" or node != "no node" for state, node in chan):
        notes.append("FAIL a key-injection channel exists in the desktop session: %r" % (chan,))
    else:
        notes.append("no key-injection channel in the desktop session "
                     "(maeroX: %s, %s)" % chan[0])
    return not any(n.startswith("FAIL") for n in notes), notes


def count_key_events(run):
    return sum(1 for _, line in run.lines if XT_KEY.search(line))


def send_and_wait(qmp, run, pump, keys, expect, timeout):
    """Send one sendkey and wait for the events it should produce.

    A fixed sleep is not good enough here: maeroX streams its diagnostic frame
    over the same 115200-baud serial line, so how long it takes to get round to
    a keystroke varies by tens of seconds.  Waiting for the events themselves —
    and saying so when they never come — is the difference between a real check
    and one that reports whatever the timing happened to produce."""
    want = count_key_events(run) + expect
    r = qmp.hmp("sendkey " + keys)
    if r and r.strip():
        print("smoke-firefox:   sendkey %s -> %s" % (keys, r.strip()))
    end = time.time() + timeout
    while time.time() < end:
        pump(1.0)
        if count_key_events(run) >= want:
            return True
    print("smoke-firefox:   sendkey %s: only %d of %d expected events in %ds"
          % (keys, count_key_events(run) - (want - expect), expect, timeout))
    return False


def run_key_checks(qmp, args, run, pump):
    """Drive the two paths that used to split a key in two, through real
    scancodes, then check maeroX's trace.

    Order matters: Alt-Tab moves the focus off the maeroX window, so it goes
    last."""
    print("smoke-firefox: key checks — right Alt combination, then Alt-Tab")
    pump(2.0)
    # AltGr + a: four events (Alt_R down, a down, a up, Alt_R up), and the 'a'
    # must carry Mod1Mask.
    send_and_wait(qmp, run, pump, "alt_r-a", 4, args.keycheck_timeout)
    # Alt-Tab: the desktop keeps Tab for itself, so only the Alt pair reaches
    # the client — but it must be a PAIR, on the same window.
    send_and_wait(qmp, run, pump, "alt-tab", 2, args.keycheck_timeout)
    ok, notes = check_key_trace(run)
    for n in notes:
        print("smoke-firefox:   " + n)
    run.key_notes = notes
    run.key_ok = ok
    if not ok:
        run.result, run.reason = "FAIL", "key checks failed: " + \
            "; ".join(n for n in notes if n.startswith("FAIL"))
    return ok


def type_into_guest(qmp, args, run, pump, shot):
    """Focus Firefox's address bar and type args.type_text into it.

    Ctrl+L is the address-bar accelerator; it only works if the modifier state
    reaches the browser as a real X11 KeyPress with ControlMask set, so this is
    itself part of what the screenshot proves."""
    try:
        keys = qemu_keys_for(args.type_text)
    except ValueError as exc:
        print("smoke-firefox: --type: %s" % exc)
        run.type_note = "not typed: %s" % exc
        return
    print("smoke-firefox: typing %r into the address bar (%d keys)"
          % (args.type_text, len(keys)))
    pump(2.0)
    qmp.hmp("sendkey ctrl-l")
    pump(1.5)
    for k in keys:
        qmp.hmp("sendkey " + k)
        pump(args.type_delay)
    pump(args.type_settle)
    p = shot("screen-typed")
    run.type_note = "typed %r -> %s" % (args.type_text, p or "(no screendump)")
    print("smoke-firefox: %s" % run.type_note)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--accel", default=None, help="kvm, tcg, or any QEMU -accel value (default: kvm if usable else tcg)")
    ap.add_argument("--smp", type=int, default=1)
    ap.add_argument("--mem", default="2048M")
    ap.add_argument("--timeout", type=int, default=None, help="overall timeout in seconds (default 360 KVM / 900 TCG)")
    ap.add_argument("--iso", default="maeros.iso")
    ap.add_argument("--disk", default="disk-ff.img")
    ap.add_argument("--out", default=os.path.join("build", "ff-smoke"), help="artifact root")
    ap.add_argument("--tag", default=None, help="suffix for the artifact directory (default: accel-smpN)")
    ap.add_argument("--qemu", default="qemu-system-i386")
    ap.add_argument("--cpu", default=None,
                    help="QEMU -cpu model (default: QEMU's own). "
                         "Use to test what the guest does with a wider feature set, e.g. --cpu host")
    ap.add_argument("--hold", type=float, default=25.0,
                    help="seconds to keep sampling frames after the paint verdict "
                         "before choosing screen-paint.png (default 25)")
    ap.add_argument("--type", dest="type_text", default=None, metavar="TEXT",
                    help="after the paint verdict, focus Firefox's address bar (Ctrl+L) and "
                         "type TEXT through QEMU sendkey, then save screen-typed.png. "
                         "Off by default; the default path is unchanged.")
    ap.add_argument("--type-delay", type=float, default=0.35,
                    help="seconds between injected keystrokes (default 0.35)")
    ap.add_argument("--type-settle", type=float, default=20.0,
                    help="seconds to wait after typing before the screenshot (default 20). The guest repaints the address bar several seconds behind the keystrokes, so a short settle catches a half-drawn string.")
    ap.add_argument("--keycheck", action="store_true",
                    help="after the paint verdict, send a right-Alt combination and Alt-Tab "
                         "through QEMU sendkey and assert maeroX's key trace: every press "
                         "matched by a release on the same window, right Alt setting Mod1Mask, "
                         "and no half of the Alt-Tab leaking to a client. Off by default.")
    ap.add_argument("--keycheck-timeout", type=float, default=90.0,
                    help="seconds to wait for each --keycheck keystroke to show up in "
                         "maeroX's trace (default 90; the guest can be busy streaming a "
                         "diagnostic frame over the serial line)")
    ap.add_argument("-v", "--verbose", action="store_true", help="echo every serial line")
    args = ap.parse_args()

    os.chdir(ROOT)
    accel = args.accel or ("kvm" if kvm_usable() else "tcg")
    is_kvm = accel.split(",")[0] == "kvm"
    if args.timeout is None:
        args.timeout = 360 if is_kvm else 900
    boot_timeout = 120 if is_kvm else 420
    launch_timeout = 90 if is_kvm else 300

    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    tag = args.tag or ("%s-smp%d" % (accel.split(",")[0], args.smp))
    args.outdir = os.path.abspath(os.path.join(args.out, "%s-%s" % (stamp, tag)))
    os.makedirs(args.outdir, exist_ok=True)

    # ── preflight ────────────────────────────────────────────────────────
    qemu = shutil.which(args.qemu)
    problems = []
    if not qemu:
        problems.append("%s not on PATH (source ~/opt/cross/maeros-env.sh?)" % args.qemu)
    if not os.path.exists(args.iso):
        problems.append("%s missing (make iso)" % args.iso)
    if not os.path.exists(args.disk):
        problems.append("%s missing (make disk-ff)" % args.disk)
    if not os.path.exists(os.path.join("testfiles", "ffauto")):
        problems.append("testfiles/ffauto missing: the desktop auto-launches ff only when /disk/ffauto exists on the disk")
    if is_kvm and not kvm_usable():
        problems.append("--accel kvm requested but /dev/kvm is not writable")
    if problems:
        for p in problems:
            print("smoke-firefox: ERROR:", p)
        with open(os.path.join(args.outdir, "summary.txt"), "w") as f:
            f.write("result      : ERROR\n" + "".join("reason      : %s\n" % p for p in problems))
        return 2

    tmp = tempfile.mkdtemp(prefix="ffsmoke-")
    qmp_path = os.path.join(tmp, "qmp.sock")
    cmd = [qemu,
           "-cdrom", args.iso,
           "-drive", "file=%s,format=raw,if=ide" % args.disk,
           "-m", args.mem,
           "-smp", str(args.smp),
           "-accel", accel,
           "-vga", "std",
           "-display", "none",
           "-serial", "stdio",
           "-monitor", "none",
           "-qmp", "unix:%s,server,nowait" % qmp_path,
           "-no-reboot", "-no-shutdown"]
    if args.cpu:
        cmd[1:1] = ["-cpu", args.cpu]
    with open(os.path.join(args.outdir, "qemu-cmdline.txt"), "w") as f:
        f.write(" ".join(cmd) + "\n")

    print("smoke-firefox: accel=%s smp=%d mem=%s cpu=%s timeout=%ds"
          % (accel, args.smp, args.mem, args.cpu or "default", args.timeout))
    print("smoke-firefox: artifacts -> %s" % args.outdir)

    run = Run(args)
    run.t0 = time.time()
    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, bufsize=0)
    sel = selectors.DefaultSelector()
    sel.register(proc.stdout, selectors.EVENT_READ)
    qmp = Qmp(qmp_path)
    qmp_ok = qmp.connect(time.time() + 15)
    if not qmp_ok:
        print("smoke-firefox: warning: QMP socket did not come up; no screendumps")

    def pump(seconds):
        end = time.time() + seconds
        while time.time() < end:
            for key, _ in sel.select(0.2):
                try:
                    chunk = os.read(key.fd, 65536)
                except OSError:
                    chunk = b""
                if chunk:
                    run.feed(chunk)
            if proc.poll() is not None:
                return False
        return True

    def shot(name):
        p = qmp.screendump(os.path.join(args.outdir, name + ".png"),
                           os.path.join(args.outdir, name + ".ppm"))
        if p:
            run.shots[name] = p
        return p

    deadline = run.t0 + args.timeout
    next_shot = time.time() + 30
    try:
        while time.time() < deadline and run.result is None:
            alive = pump(1.0)
            now = time.time() - run.t0
            if not alive:
                run.result, run.reason = "FAIL", "QEMU exited with status %s before a verdict" % proc.returncode
                break
            if run.t_gfx is None and now > boot_timeout:
                run.result, run.reason = "FAIL", "no '%s' within %ds of boot" % (GFX_START, boot_timeout)
                break
            if run.t_gfx is not None and run.t_ff is None and now - run.t_gfx > launch_timeout:
                run.result, run.reason = ("FAIL", "desktop up but the ff launcher never started within %ds "
                                          "(is /disk/ffauto on %s?)" % (launch_timeout, args.disk))
                break
            if time.time() >= next_shot:
                shot("screen")
                next_shot = time.time() + 30
        if run.result is None:
            run.result, run.reason = "FAIL", "timeout after %ds without a verdict from ff" % args.timeout
        if run.result == "PASS":
            # The paint marker is dropped on the FIRST PutImage, which is well
            # before Firefox has drawn its whole chrome and before the desktop
            # has composited it, so a single screendump here races the frame and
            # can catch an empty window.  Hold for a while, sample several
            # frames and keep the one with the most drawn detail — this image is
            # the run's evidence, so it has to show what actually got painted.
            best, best_score = None, -1
            n = max(2, PAINT_SHOTS)
            for k in range(n):
                pump(max(0.2, float(args.hold) / n))
                cand = qmp.screendump_ppm(os.path.join(tmp, "paint%d.ppm" % k))
                frame = read_ppm(cand) if cand else None
                score = frame_detail(frame)
                run.paint_scores.append(score)
                if score > best_score:
                    best, best_score = frame, score
            paint_png = os.path.join(args.outdir, "screen-paint.png")
            if best:
                write_png(paint_png, best)
                run.shots["screen-paint"] = paint_png
                print("smoke-firefox: screen-paint = best of %d frames over %.0fs "
                      "(%d light pixels; scores %s)"
                      % (n, args.hold, best_score,
                         ",".join(str(x) for x in run.paint_scores)))
            else:
                shot("screen-paint")   # QMP unavailable: fall back to one dump
            if args.type_text:
                type_into_guest(qmp, args, run, pump, shot)
            if args.keycheck:
                run_key_checks(qmp, args, run, pump)
        elif run.panic_lines:
            pump(2.0)         # collect the register dump / stack trace
        else:
            # A FAIL with no panic and no ff verdict is a wedge: the guest
            # stopped saying anything.  Photograph the CPU from outside before
            # QEMU is killed — this is the only evidence a silent hang leaves.
            pump(0.5)
            if proc.poll() is None:
                run.wedge_qmp = capture_wedge(qmp, args.outdir)
                print("smoke-firefox: wedge capture -> %s" % run.wedge_qmp)
                # Knock on the guest with an NMI.  It is delivered even with
                # IF=0, so the kernel's NMI handler can print every process's
                # state onto the serial line from inside a hang that no timer
                # tick can reach.  The pump is what lands it in serial.log.
                qmp.hmp("nmi")
                pump(3.0)
        shot("screen")
    except KeyboardInterrupt:
        run.result, run.reason = "FAIL", "interrupted"
    finally:
        qmp.close()
        try:
            proc.terminate()
            proc.wait(timeout=5)
        except Exception:
            try:
                proc.kill()
            except Exception:
                pass
        # drain what is left
        try:
            rest = proc.stdout.read()
            if rest:
                run.feed(rest)
        except Exception:
            pass
        if run.partial:
            run.lines.append((time.time() - run.t0, run.partial))
        run.raw.close()
        shutil.rmtree(tmp, ignore_errors=True)

    text = run.write_summary(cmd, accel, time.time() - run.t0)
    print()
    print(text)
    print("smoke-firefox: %s -- %s" % (run.result, run.reason))
    return 0 if run.result == "PASS" else (2 if run.result == "ERROR" else 1)


if __name__ == "__main__":
    sys.exit(main())
