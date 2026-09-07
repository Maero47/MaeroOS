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
  screen-paint.png  the screendump taken right after the paint line (PASS only)
  summary.txt       verdict, timings, attempt list, crash lines, last kernel
                    lines, moz.log tail
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
import subprocess
import sys
import tempfile
import time

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

    def close(self):
        if self.sock:
            try:
                self.sock.close()
            except OSError:
                pass
            self.sock = None


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
        L.append("screendumps")
        for k, v in self.shots.items():
            L.append("  %-6s %s" % (k, v))
        L.append("")
        L.append("qemu: " + " ".join(qemu_cmd))
        text = "\n".join(L) + "\n"
        with open(os.path.join(a.outdir, "summary.txt"), "w") as f:
            f.write(text)
        return text


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
    with open(os.path.join(args.outdir, "qemu-cmdline.txt"), "w") as f:
        f.write(" ".join(cmd) + "\n")

    print("smoke-firefox: accel=%s smp=%d mem=%s timeout=%ds" % (accel, args.smp, args.mem, args.timeout))
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
            # The paint line already survived ff's 5 s grace check; hold a
            # little longer so the screendump shows the window, not the flash.
            pump(3.0)
            shot("screen-paint")
        elif run.panic_lines:
            pump(2.0)         # collect the register dump / stack trace
        else:
            pump(0.5)
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
