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
  screen-web.png    with --web only: the frame in which the served page's
                    marker image was found on screen (or the last one tried),
                    retaken once the page's font report is in
  fonts.json        with --web only: the page's font report
  qemu-cmdline.txt  the exact QEMU command

--web (implies --net) is the page-load check: after the paint verdict it serves
a small page from the host (an HTML document with a stylesheet rule and a
WEB_MARK_W x WEB_MARK_H pure-red PNG), types http://10.0.2.2:PORT/ into the
address bar, and PASSes only if the image is requested from the server AND its
red block shows up in a screendump within --web-timeout seconds.  The request
alone proves the document was parsed; the pixels prove it was laid out and
composited.  summary.txt records the time from Enter to each request and to
the red block appearing.  The page also carries a font test (see FONT_JS):
its script reports within --font-timeout seconds whether any character of
Latin, Turkish/German accented, Greek and Cyrillic text came out as a
missing-glyph box in sans/serif/mono/system-ui and named families, and
whether serif, mono, bold and italic are real faces; any failure FAILs the
run and the report is saved as fonts.json.

--scroll SEC (implies --web) is the compositor benchmark: after the page-load
check it opens a long served page (/long: text and colour blocks) and holds
the Down arrow for SEC seconds, so Firefox scrolls and repaints the whole
time.  For the numbers, build the kernel with KTRACE=1: its periodic dump then
carries a per-thread line ("[kprof] cpu window=... idle=... desktop=...")
every 10 s, and the run puts /gfxstats on the disk so the desktop traces its
compositor counters ("[desktop] stats frames=... render_ms=...") alongside.
The dump windows that fall inside the scroll are averaged into scroll.txt
(CPU share per process, desktop frames/s and ms per frame) and the summary.

--audio (opt-in, implies --net) is the <audio> check (docs/audio.md): a copy
of the disk gets /audio.html and a 3 s 440 Hz /tone.wav plus autoplay prefs,
the guest gets an Intel HDA recorded by QEMU into audio.wav, and after the
paint verdict file:///disk/audio.html is typed into the address bar.  The
page reports its media events to the host; the capture must hold the tone at
440 Hz for about 3 s with few silent 10 ms blocks inside.  Firefox plays
through cubeb's PulseAudio backend on apulse (libpulse over alsa-lib, in the
Firefox tree) and the kernel's ALSA ABI.

Usage:
  python3 tools/smoke_firefox.py [--accel kvm|tcg|<qemu -accel value>]
                                 [--smp N] [--timeout SEC] [--mem SIZE]
                                 [--iso PATH] [--disk PATH] [--out DIR]
                                 [--tag NAME] [--net] [--web] [--audio] [-v]
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
import threading
import zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

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


WEB_MARK_W, WEB_MARK_H = 96, 96

# The --web page's font section: one row per family, every row carrying the
# same text so a missing-glyph box stands out next to a rendered line.
FONT_FAMILIES = [
    ("sans", "sans-serif"),
    ("serif", "serif"),
    ("mono", "monospace"),
    ("system-ui", "system-ui"),
    # example.com's body stack: named families the disk does not have, then
    # the generic, so it exercises fontconfig's aliases and fallback
    ("web-stack", "-apple-system, BlinkMacSystemFont, 'Segoe UI', 'Open Sans', "
                  "'Helvetica Neue', Helvetica, Arial, sans-serif"),
    ("times", "'Times New Roman', Times"),
    ("courier", "'Courier New', Courier"),
]
FONT_TEXT = "Latin AaBbGgQq äöüçşğ ÄÖÜÇŞĞİı " \
            "Ελληνικά Кириллица"

# Runs in the page after load.  For every family and every character of
# FONT_TEXT it measures the advance and ink box on a canvas and compares them
# with those of U+0378/U+0379 (unassigned, so no font has them: that is
# Firefox's missing-glyph hex box in that family).  A character whose advance
# and ink box both match was drawn as a box (the advance alone is not enough:
# in a monospaced font every glyph can have the box's width); U+A000, which
# no installed font covers, must come out as a box, or the detector is blind.
# It also checks that serif, sans and monospace are different faces, that
# monospace is monospaced, that bold and italic change the rendering and that
# serif italic is a real italic face, then POSTs the result to /fontreport and
# puts the verdict in the title (visible in maeroX's _NET_WM_NAME trace on the
# serial line).
FONT_JS = r"""
(function () {
  var FAM = %s, TEXT = %s, SIZE = 37;
  var c = document.createElement('canvas').getContext('2d');
  function w(font, s) { c.font = font; return c.measureText(s).width; }
  // advance + ink box; a missing-glyph box matches the reference in all five
  function shape(font, s) {
    c.font = font; var m = c.measureText(s);
    return [m.width, m.actualBoundingBoxLeft, m.actualBoundingBoxRight,
            m.actualBoundingBoxAscent, m.actualBoundingBoxDescent];
  }
  function same(a, b) { return a.every(function (x, i) { return Math.abs(x - b[i]) < 0.001; }); }
  function ink(font, s) { return shape(font, s).slice(1, 3).map(function (x) { return +x.toFixed(2); }); }
  var r = {families: {}, checks: {}, fails: []};
  var chars = Array.from(TEXT).filter(function (ch) { return ch !== ' '; });
  FAM.forEach(function (f) {
    var font = SIZE + 'px ' + f[1];
    var box = shape(font, '\u0378'), box2 = shape(font, '\u0379'), boxed = [];
    chars.forEach(function (ch) { if (same(shape(font, ch), box)) boxed.push(ch); });
    r.families[f[0]] = {box: box.map(function (x) { return +x.toFixed(2); }),
                        text: +w(font, TEXT).toFixed(3), boxed: boxed.join('')};
    if (!same(box, box2)) r.fails.push(f[0] + ': missing-glyph reference differs between U+0378 and U+0379');
    if (boxed.length) r.fails.push(f[0] + ': ' + boxed.length + ' boxed (' + boxed.join('') + ')');
  });
  var sans = SIZE + 'px sans-serif', serif = SIZE + 'px serif', mono = SIZE + 'px monospace';
  var ck = r.checks;
  // negative control: U+A000 (Yi) is in no installed font, so the detector
  // must call it a box; if it does not, "0 boxed" above proves nothing
  ck.detector_sees_boxes = same(shape(sans, '\ua000'), shape(sans, '\u0378'));
  ck.serif_ne_sans = Math.abs(w(serif, TEXT) - w(sans, TEXT)) > 1;
  ck.mono_ne_sans = Math.abs(w(mono, TEXT) - w(sans, TEXT)) > 1;
  ck.mono_fixed = Math.abs(w(mono, 'iiiiiiii') - w(mono, 'MMMMMMMM')) < 0.01;
  ck.bold_differs = Math.abs(w('bold ' + sans, TEXT) - w(sans, TEXT)) > 1;
  ck.italic_differs = JSON.stringify(ink('italic ' + sans, 'lIl')) !== JSON.stringify(ink(sans, 'lIl'));
  // a real serif italic 'f' has a descender; a synthesized oblique only
  // slants the upright one (a sans oblique is a slanted upright either way)
  ck.serif_italic_face = shape('italic ' + serif, 'f')[4] > 3 && shape(serif, 'f')[4] < 2;
  Object.keys(ck).forEach(function (k) { if (!ck[k]) r.fails.push('check ' + k + ' failed'); });
  document.title = r.fails.length ? 'FONTS FAIL ' + r.fails.length : 'FONTS OK';
  fetch('/fontreport', {method: 'POST', body: JSON.stringify(r)});
})();
"""


# --audio: the page also carries an <audio autoplay> of a 440 Hz WAV served
# from the host, and reports the element's events back.  The run's profile
# gets media.cubeb.backend=alsa and autoplay allowed (AUDIO_PREFS, written into
# a copy of the disk), the guest gets an Intel HDA whose output QEMU records,
# and the capture must hold the tone.
AUDIO_HZ = 440
AUDIO_SECS = 3.0
AUDIO_HTML = ("<audio id=snd src=\"tone.wav\" autoplay></audio><script>(function(){"
              "var a=document.getElementById('snd');function r(e){try{var x=new XMLHttpRequest();"
              "x.open('POST',REPORT_URL);x.send(JSON.stringify({ev:e,t:a.currentTime,"
              "err:a.error?a.error.code+' '+a.error.message:null,paused:a.paused}));}catch(_){}}"
              "['play','playing','ended','error','stalled','pause'].forEach(function(e){"
              "a.addEventListener(e,function(){r(e);});});"
              "var p=a.play();if(p&&p.catch)p.catch(function(e){r('play() rejected: '+e);});"
              "setTimeout(function(){r('t+2s');},2000);})();</script>")
# The --audio page itself, written onto the disk with tone.wav and opened as
# file:///disk/audio.html: no network between Firefox and the media.  Its
# event reports still go to the host's WebServer when the network is there.
AUDIO_PAGE = ("<!doctype html><html><head><meta charset=\"utf-8\"><title>MaeroOS audio</title>"
              "</head><body style=\"background:#e8eefc\"><h1>audio test</h1>"
              "<img src=\"mark.png\" width=\"%d\" height=\"%d\">%s</body></html>")
AUDIO_PREFS = ('user_pref("media.autoplay.default", 0);\n'
               'user_pref("media.autoplay.blocking_policy", 0);\n')
AUDIO_MOZLOG = "sync,timestamp,Widget:5,AudioStream:2"


def tone_wav(hz, secs, rate):
    """A stereo S16LE WAV of a sine at hz."""
    import math
    n = int(rate * secs)
    pcm = b"".join(struct.pack("<hh", v, v) for v in
                   (int(0.6 * 32767 * math.sin(2 * math.pi * hz * i / rate)) for i in range(n)))
    fmt = struct.pack("<HHIIHH", 1, 2, rate, rate * 4, 4, 16)
    body = b"WAVE" + b"fmt " + struct.pack("<I", 16) + fmt + b"data" + struct.pack("<I", len(pcm)) + pcm
    return b"RIFF" + struct.pack("<I", len(body)) + body


def audio_disk(args, port):
    """A copy of the disk with AUDIO_PREFS appended to /ffprofile/user.js,
    cubeb logging in /ffcfg/ffmozlog (moz.log on the disk at /mozaudio.log),
    and /audio.html + /tone.wav + /mark.png (reports to the host's `port`).
    In build/ (the image is ~1 GiB; not /tmp)."""
    work = os.path.join(ROOT, "build", "ff-audio-disk.img")
    subprocess.run(["cp", "--sparse=always", args.disk, work], check=True)
    tmpd = tempfile.mkdtemp(prefix="ffaudio-", dir=os.path.join(ROOT, "build"))
    try:
        uj = os.path.join(tmpd, "user.js")
        subprocess.run(["debugfs", "-R", "dump /ffprofile/user.js " + uj, work],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        with open(uj, "a") as f:
            f.write(AUDIO_PREFS)
        with open(os.path.join(tmpd, "ffmozlog"), "w") as f:
            f.write(AUDIO_MOZLOG + "\n")
        with open(os.path.join(tmpd, "ffmozlogfile"), "w") as f:
            f.write("/disk/mozaudio.log\n")
        html = AUDIO_PAGE % (WEB_MARK_W, WEB_MARK_H, AUDIO_HTML.replace(
            "REPORT_URL", "'http://10.0.2.2:%d/audioreport'" % port))
        for name, data in (("audio.html", html.encode()), ("tone.wav", tone_wav(AUDIO_HZ, AUDIO_SECS, 44100)),
                           ("mark.png", png_bytes(WEB_MARK_W, WEB_MARK_H, (255, 0, 0)))):
            with open(os.path.join(tmpd, name), "wb") as f:
                f.write(data)
        cmds = ["write %s /%s" % (os.path.join(tmpd, n), n) for n in ("audio.html", "tone.wav", "mark.png")] + ["rm /ffprofile/user.js", "write %s /ffprofile/user.js" % uj,
                "cd /ffcfg", "rm ffmozlog", "write %s ffmozlog" % os.path.join(tmpd, "ffmozlog"),
                "rm ffmozlogfile", "write %s ffmozlogfile" % os.path.join(tmpd, "ffmozlogfile")]
        cf = os.path.join(tmpd, "cmds")
        with open(cf, "w") as f:
            f.write("\n".join(cmds) + "\n")
        subprocess.run(["debugfs", "-w", "-f", cf, work], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    finally:
        shutil.rmtree(tmpd, ignore_errors=True)
    return work


def audio_load(qmp, args, run, pump, web):
    """Open file:///disk/audio.html, wait for its red block on screen and
    then for 'ended' (or AUDIO_SECS + 60 s); note what the page reported.
    The capture itself is judged once QEMU has exited."""
    send_text(qmp, args, pump, "file:///disk/audio.htm")
    t_enter = time.time()
    qmp.hmp("sendkey l")
    pump(0.5)
    qmp.hmp("sendkey ret")
    need = WEB_MARK_W * WEB_MARK_H * 9 // 10
    t_red = None
    tmpdir = tempfile.mkdtemp(prefix="ffaudio-")
    try:
        end = t_enter + args.web_timeout
        while time.time() < end and t_red is None:
            if not pump(2.0):
                break
            cand = qmp.screendump_ppm(os.path.join(tmpdir, "a.ppm"))
            if red_pixels(read_ppm(cand) if cand else None) >= need:
                t_red = time.time()
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)
    run.web_notes.append("audio page on screen %s after Enter" %
                         ("%.1fs" % (t_red - t_enter) if t_red else "never"))
    end = time.time() + AUDIO_SECS + 60
    while time.time() < end and not any(
            ev.get("ev") in ("ended", "error") for _, ev in web.audio_events):
        if not pump(1.0):
            break
    pump(3.0)
    for t, ev in web.audio_events:
        run.web_notes.append("audio event +%.1fs %s" % (t - t_enter, json.dumps(ev)))


def audio_capture_check(args, run):
    """Judge build/.../audio.wav: the 440 Hz stretch (blocks labelled against
    the login chime's notes too, so the chime is not counted) must last about
    AUDIO_SECS, sit at 440 Hz, and have few silent 10 ms blocks inside it."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from smoke_hda import classify, peak_freq, read_wav
    from smoke_audio import main_run
    path = os.path.join(args.outdir, "audio.wav")
    try:
        rate, _, s = read_wav(path)
    except Exception as e:  # noqa: BLE001
        return "no usable capture (%s)" % e, False
    labels = classify(s, rate, [AUDIO_HZ, 523, 659, 784, 1047])
    span = main_run(labels, 0, gap=25)
    if not span:
        return "capture of %.2f s holds no %d Hz tone" % (len(s) / rate, AUDIO_HZ), False
    first, last, n = span
    blk = rate // 100
    part = s[first * blk:(last + 1) * blk]
    f = peak_freq(part, rate)
    holes = sum(1 for x in labels[first:last + 1] if x is None)
    msg = ("capture: %d Hz from %.2f s to %.2f s (%.2f s, %d tone blocks, %d silent blocks "
           "inside), peak %d Hz" % (AUDIO_HZ, first / 100, (last + 1) / 100,
                                     (last + 1 - first) / 100, n, holes, f))
    ok = (abs(f - AUDIO_HZ) <= AUDIO_HZ * 0.02 and n >= AUDIO_SECS * 100 * 0.9
          and (last + 1 - first) <= AUDIO_SECS * 100 * 1.15 and holes <= 10)
    return msg, ok


def png_bytes(w, h, rgb):
    """A w x h PNG of one solid colour (no colour-space chunk, so Firefox
    shows the exact value; a tagged image would be colour-managed)."""
    raw = b"".join(b"\x00" + bytes(rgb) * w for _ in range(h))

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))
    return (b"\x89PNG\r\n\x1a\n" +
            chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


class WebServer:
    """The page --web loads, served from a thread; records every request and
    the font report the page POSTs back."""
    PAGE_HEAD = (b"<!doctype html><html><head><meta charset=\"utf-8\">"
                 b"<title>MaeroOS web smoke</title>"
                 b"<style>body{background:#e8eefc;font-family:sans-serif;margin:6px 8px}"
                 b"h1{color:#1d4ed8;font-size:22px;margin:2px 0}p{margin:2px 0}"
                 b"#mark{display:block;margin:4px 0}"
                 b"table{border-collapse:collapse;font-size:15px;line-height:1.15}"
                 b"td{padding:0 6px 0 0;white-space:nowrap}td.k{color:#555;font:11px monospace}"
                 b"</style></head><body>"
                 b"<h1>Served over HTTP to MaeroOS</h1>"
                 b"<p>Fetched by Firefox through the MaeroOS TCP stack.</p>")

    @staticmethod
    def page():
        rows = []
        for key, fam in FONT_FAMILIES:
            style = "font-family:%s" % fam
            rows.append("<tr><td class=k>%s</td><td style=\"%s\">%s</td>"
                        "<td style=\"%s\"><b>Bold</b> <i>Italic</i> <b><i>BoldItalic</i></b></td></tr>"
                        % (key, style, FONT_TEXT, style))
        body = ("<img id=mark src=\"mark.png\" width=\"%d\" height=\"%d\" alt=\"mark\">"
                "<table>%s</table><p style=\"font-size:15px\">missing-glyph control "
                "(U+A000, no font has it): \ua000</p><script>%s</script></body></html>"
                % (WEB_MARK_W, WEB_MARK_H, "".join(rows),
                   FONT_JS % (json.dumps([list(f) for f in FONT_FAMILIES]), json.dumps(FONT_TEXT))))
        return WebServer.PAGE_HEAD + body.encode("utf-8")

    @staticmethod
    def long_page():
        """--scroll's page: tall enough to scroll for minutes, with text and
        coloured blocks so every scrolled frame differs."""
        parts = [b"<!doctype html><html><head><meta charset=\"utf-8\"><title>long</title>"
                 b"<style>body{font-family:sans-serif;margin:8px}div.b{height:40px;margin:4px 0}"
                 b"</style></head><body><h1>Scrolling</h1>"]
        for i in range(600):
            parts.append(("<p>%d. The quick brown fox jumps over the lazy dog; "
                          "pack my box with five dozen liquor jugs.</p>"
                          "<div class=b style=\"background:hsl(%d,70%%,60%%)\"></div>"
                          % (i, (i * 37) % 360)).encode())
        parts.append(b"</body></html>")
        return b"".join(parts)

    def __init__(self, audio=False):
        self.requests = []          # (host time, path, status)
        self.font_report = None     # (host time, parsed JSON) from the page
        self.audio_events = []      # (host time, parsed JSON) from AUDIO_JS
        page = self.page()
        if audio:
            page = page.replace(b"</body>", AUDIO_HTML.replace(
                "REPORT_URL", "'/audioreport'").encode() + b"</body>")
        files = {"/": page, "/mark.png": png_bytes(WEB_MARK_W, WEB_MARK_H, (255, 0, 0)),
                 "/long": self.long_page()}
        types = {"/": "text/html; charset=utf-8", "/mark.png": "image/png",
                 "/long": "text/html; charset=utf-8"}
        if audio:
            files["/tone.wav"] = tone_wav(AUDIO_HZ, AUDIO_SECS, 44100)
            types["/tone.wav"] = "audio/wav"
        outer = self

        class Handler(BaseHTTPRequestHandler):
            def do_GET(self):
                body = files.get(self.path)
                outer.requests.append((time.time(), self.path, 200 if body else 404))
                if body is None:
                    self.send_error(404)
                    return
                self.send_response(200)
                self.send_header("Content-Type", types[self.path])
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def do_POST(self):
                n = int(self.headers.get("Content-Length") or 0)
                data = self.rfile.read(n) if n > 0 else b""
                ok = self.path in ("/fontreport", "/audioreport")
                outer.requests.append((time.time(), "POST " + self.path, 204 if ok else 404))
                if self.path == "/audioreport":
                    try:
                        outer.audio_events.append((time.time(), json.loads(data.decode("utf-8"))))
                    except ValueError:
                        pass
                elif ok:
                    try:
                        outer.font_report = (time.time(), json.loads(data.decode("utf-8")))
                    except ValueError:
                        outer.font_report = (time.time(), {"fails": ["unparsable report: %r" % data[:200]]})
                self.send_response(204 if ok else 404)
                self.send_header("Content-Length", "0")
                self.end_headers()

            def log_message(self, *a):
                pass

        self.httpd = ThreadingHTTPServer(("0.0.0.0", 0), Handler)
        self.port = self.httpd.server_address[1]
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def first(self, path):
        for t, p, _ in self.requests:
            if p == path:
                return t
        return None

    def close(self):
        self.httpd.shutdown()
        self.httpd.server_close()


def red_pixels(frame):
    """Pixels of exactly (255, 0, 0) - the --web marker; nothing else on this
    desktop uses that colour."""
    if not frame:
        return 0
    rgb = frame[2]
    return sum(1 for i in range(0, len(rgb) - 2, 3)
               if rgb[i] == 255 and rgb[i + 1] == 0 and rgb[i + 2] == 0)


SANDBOX_REPORT = "maeros-sandbox: "
SANDBOX_WAIT = 45          # s after the verdict to wait for the sandbox report


def check_sandbox(run):
    """FAIL a PASS whose Firefox reports the content sandbox off or a syscall
    the filter refused (docs/sandbox.md).  No report at all is only noted:
    a runtime tree without ports/firefox/autoconfig cannot print one."""
    sb = run.sandbox
    if sb is None or run.result != "PASS":
        return
    bad = []
    if not sb.get("hasSeccompBPF") or not sb.get("hasSeccompTSync"):
        bad.append("seccomp-bpf/TSYNC not detected")
    if not sb.get("canSandboxContent"):
        bad.append("canSandboxContent false")
    if not sb.get("effectiveContentSandboxLevel"):
        bad.append("effective content sandbox level %r" % sb.get("effectiveContentSandboxLevel"))
    if sb.get("syscallLog"):
        bad.append("%d rejected syscall(s) in the sandbox log" % len(sb["syscallLog"]))
    if bad:
        run.result, run.reason = "FAIL", "content sandbox: " + "; ".join(bad)


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
        self.web_notes = []
        self.site_rows = []
        self.key_notes = None
        self.key_ok = None
        self.sandbox = None        # about:support's sandbox section (maeros.cfg)

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
        elif SANDBOX_REPORT in line:
            try:
                self.sandbox = json.loads(line.split(SANDBOX_REPORT, 1)[1])
            except ValueError:
                self.sandbox = {"unparsed": line.split(SANDBOX_REPORT, 1)[1].strip()}
            interesting = True
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
        L.append("content sandbox (about:support, from maeros.cfg)")
        if self.sandbox is None:
            L.append("  (not reported: is ports/firefox/autoconfig installed in testfiles/firefox?)")
        else:
            for k in sorted(self.sandbox):
                L.append("  %-30s %s" % (k, json.dumps(self.sandbox[k])))
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
        for n in self.web_notes:
            L.append("web         : %s" % n)
            L.append("")
        for r in self.site_rows:
            L.append("site        : %s" % site_row_text(r))
            for l in r.get("sig", [])[:10]:
                L.append("                %s" % l)
        if self.site_rows:
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


def send_text(qmp, args, pump, text):
    """Ctrl+L, then `text` through QEMU sendkey.  Raises ValueError for a
    character with no key name."""
    keys = qemu_keys_for(text)
    pump(2.0)
    qmp.hmp("sendkey ctrl-l")
    pump(1.5)
    for k in keys:
        qmp.hmp("sendkey " + k)
        pump(args.type_delay)
    return len(keys)


def web_load(qmp, args, run, pump, web):
    """Type the served page's URL and wait for its image to be requested and
    its red block to reach the screen."""
    url = "http://10.0.2.2:%d/" % web.port
    print("smoke-firefox: --web: loading %s" % url)
    send_text(qmp, args, pump, url[:-1])       # all but the last key...
    t_enter = time.time()
    qmp.hmp("sendkey ret")                     # ...then Enter, timed
    need = WEB_MARK_W * WEB_MARK_H * 9 // 10
    end = t_enter + args.web_timeout
    best, best_red, t_red = None, -1, None
    tmpdir = tempfile.mkdtemp(prefix="ffweb-")
    try:
        while time.time() < end:
            if not pump(1.0):
                break
            if web.first("/mark.png") is None:
                continue
            cand = qmp.screendump_ppm(os.path.join(tmpdir, "web.ppm"))
            frame = read_ppm(cand) if cand else None
            red = red_pixels(frame)
            if red > best_red:
                best, best_red = frame, red
            if red >= need:
                t_red = time.time()
                break
        if best is None:
            cand = qmp.screendump_ppm(os.path.join(tmpdir, "web.ppm"))
            best = read_ppm(cand) if cand else None
            best_red = red_pixels(best)
        # The page's script reports its font measurements right after load;
        # give it a little longer than the image, then take a frame with the
        # text fully drawn for screen-web.png.
        if t_red is not None:
            fend = time.time() + args.font_timeout
            while web.font_report is None and time.time() < fend:
                if not pump(1.0):
                    break
            pump(2.0)
            cand = qmp.screendump_ppm(os.path.join(tmpdir, "web.ppm"))
            frame = read_ppm(cand) if cand else None
            if red_pixels(frame) >= need:
                best = frame
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)
    if best:
        pth = os.path.join(args.outdir, "screen-web.png")
        write_png(pth, best)
        run.shots["screen-web"] = pth

    def since(t):
        return "%.1fs" % (t - t_enter) if t else "never"
    reqs = ", ".join("%s %d at +%.1fs" % (p, st, t - t_enter) for t, p, st in web.requests)
    run.web_notes = ["url %s" % url,
                     "requests: %s" % (reqs or "none"),
                     "page requested %s after Enter, image %s, red block on screen %s "
                     "(%d of %d marker pixels)" % (since(web.first("/")), since(web.first("/mark.png")),
                                                   since(t_red), max(best_red, 0),
                                                   WEB_MARK_W * WEB_MARK_H)]
    rep = web.font_report[1] if web.font_report else None
    if rep is not None:
        with open(os.path.join(args.outdir, "fonts.json"), "w") as f:
            json.dump(rep, f, indent=1, ensure_ascii=False)
        fams = rep.get("families", {})
        run.web_notes.append("font report %s after Enter: %s" % (
            since(web.font_report[0]), "OK" if not rep.get("fails") else
            "%d problem(s): %s" % (len(rep["fails"]), "; ".join(rep["fails"]))))
        run.web_notes.append("fonts: boxed chars per family: %s" % ", ".join(
            "%s=%d" % (k, len(v.get("boxed", ""))) for k, v in fams.items()))
        run.web_notes.append("fonts: checks %s" % " ".join(
            "%s=%s" % (k, "ok" if v else "NO") for k, v in rep.get("checks", {}).items()))
    else:
        run.web_notes.append("font report: never received")
    for n in run.web_notes:
        print("smoke-firefox:   " + n)
    if web.first("/") is None:
        run.result, run.reason = "FAIL", "--web: the page was never requested from the host"
    elif web.first("/mark.png") is None:
        run.result, run.reason = "FAIL", "--web: the page was fetched but its image never was"
    elif t_red is None:
        run.result, run.reason = "FAIL", "--web: the image was fetched but never appeared on screen"
    elif rep is None:
        run.result, run.reason = "FAIL", "--web: the page's font report never arrived"
    elif rep.get("fails"):
        run.result, run.reason = "FAIL", "--web: text rendering: %s" % "; ".join(rep["fails"])
    else:
        run.reason += (" Page loaded: image on screen %.1fs after Enter; fonts OK "
                       "(no missing-glyph boxes in %d families)." % (t_red - t_enter, len(rep.get("families", {}))))


CPU_LINE = re.compile(r"\[kprof\] cpu window=(\d+)ms idle=(\d+)ms(.*)")
STATS_LINE = re.compile(r"\[desktop\] stats frames=(\d+) rows=(\d+) commits=(\d+) "
                        r"present_kb=(\d+) render_ms=(\d+)")


def scroll_run(qmp, args, run, pump, web):
    """Open /long and hold Down for --scroll seconds; average the KTRACE
    per-thread CPU windows and the desktop's counters over that time."""
    url = "http://10.0.2.2:%d/long" % web.port
    send_text(qmp, args, pump, url)
    qmp.hmp("sendkey ret")
    end = time.time() + 60
    while web.first("/long") is None and time.time() < end:
        if not pump(1.0):
            return
    pump(10.0)                                   # layout, first paint
    t_start = time.time() - run.t0
    end = time.time() + args.scroll
    presses = 0
    while time.time() < end:
        qmp.hmp("sendkey down")
        presses += 1
        if not pump(0.08):
            break
    t_end = time.time() - run.t0
    pump(12.0)                                   # the dump that closes the window
    shot_p = qmp.screendump(os.path.join(args.outdir, "screen-scroll.png"),
                            os.path.join(args.outdir, "screen-scroll.ppm"))
    if shot_p:
        run.shots["screen-scroll"] = shot_p

    # CPU windows wholly inside the scroll.
    tot, idle, procs, nwin = 0, 0, {}, 0
    for t, line in run.lines:
        m = CPU_LINE.search(line)
        if not m:
            continue
        win = int(m.group(1)) / 1000.0
        if t - win < t_start or t > t_end + 1.0:
            continue
        nwin += 1
        tot += int(m.group(1))
        idle += int(m.group(2))
        for name, ms in re.findall(r" (\S+)=(\d+)ms", m.group(3)):
            procs[name] = procs.get(name, 0) + int(ms)
    stats = [(t, [int(x) for x in m.groups()]) for t, line in run.lines
             for m in [STATS_LINE.search(line)] if m and t_start - 10 <= t <= t_end + 10]
    notes = ["scroll %.0fs: %d Down presses on %s" % (t_end - t_start, presses, url)]
    if nwin:
        top = sorted(procs.items(), key=lambda kv: -kv[1])[:8]
        notes.append("cpu over %d KTRACE windows (%.0fs): idle %.1f%%, %s" % (
            nwin, tot / 1000.0, 100.0 * idle / tot,
            ", ".join("%s %.1f%%" % (k, 100.0 * v / tot) for k, v in top)))
    else:
        notes.append("cpu: no '[kprof] cpu' lines inside the scroll (build with KTRACE=1)")
    if len(stats) >= 2:
        (ta, a), (tb, b) = stats[0], stats[-1]
        frames = b[0] - a[0]
        notes.append("desktop over %.0fs: %d frames (%.1f/s), %.2f ms compositing per frame, "
                     "%d rows/frame, %d KB presented/frame, %d client commits" % (
                         tb - ta, frames, frames / max(tb - ta, 1e-6),
                         (b[4] - a[4]) / max(frames, 1), (b[1] - a[1]) // max(frames, 1),
                         (b[3] - a[3]) // max(frames, 1), b[2] - a[2]))
    else:
        notes.append("desktop counters: not traced (no /disk/gfxstats, or an older desktop)")
    with open(os.path.join(args.outdir, "scroll.txt"), "w") as f:
        f.write("\n".join(notes) + "\n")
    for n in notes:
        print("smoke-firefox:   " + n)
    run.web_notes.extend(notes)


def type_into_guest(qmp, args, run, pump, shot):
    """Focus Firefox's address bar and type args.type_text into it.

    Ctrl+L is the address-bar accelerator; it only works if the modifier state
    reaches the browser as a real X11 KeyPress with ControlMask set, so this is
    itself part of what the screenshot proves."""
    print("smoke-firefox: typing %r into the address bar" % args.type_text)
    try:
        send_text(qmp, args, pump, args.type_text)
    except ValueError as exc:
        print("smoke-firefox: --type: %s" % exc)
        run.type_note = "not typed: %s" % exc
        return
    pump(args.type_settle)
    p = shot("screen-typed")
    run.type_note = "typed %r -> %s" % (args.type_text, p or "(no screendump)")
    print("smoke-firefox: %s" % run.type_note)


# ── --sites: real-world pages (manual, needs the live internet) ─────────
# Never part of a default smoke target: what a live site serves changes from
# day to day, so this mode reports what happened instead of judging it.
DEFAULT_SITES = [
    ("https://en.wikipedia.org/wiki/Operating_system", "Kernel"),
    ("https://www.gnu.org/", "Philosophy"),
    ("https://news.ycombinator.com/", "past"),
    ("https://duckduckgo.com/html/", "About"),
    ("https://github.com/", "Pricing"),
]


def frame_diff(a, b, step=7):
    """Fraction (0..1) of sampled pixels that differ between two frames."""
    if not a or not b or a[:2] != b[:2]:
        return 1.0
    ra, rb = a[2], b[2]
    n = diff = 0
    for i in range(0, len(ra) - 2, 3 * step):
        n += 1
        if ra[i] != rb[i] or ra[i + 1] != rb[i + 1] or ra[i + 2] != rb[i + 2]:
            diff += 1
    return diff / max(n, 1)


def parse_sites(spec):
    """'default' or a comma list of URL[|link text]."""
    if spec in ("default", "all", ""):
        return list(DEFAULT_SITES)
    out = []
    for item in spec.split(","):
        url, _, link = item.partition("|")
        out.append((url.strip(), link.strip() or None))
    return out


def settle(qmp, pump, tmpdir, base, timeout, gap=2.0, still=3):
    """Sample the screen until it differs from `base` and then stays pixel-for-
    pixel identical for `still` consecutive samples (a spinning tab throbber
    keeps a loading page "unsettled").  Returns (frame, t_first_change,
    t_settled, alive) with times relative to the call."""
    t0 = time.time()
    prev, t_change, t_settled, n_still, t_still = None, None, None, 0, None
    alive = True
    while time.time() - t0 < timeout:
        if not pump(gap):
            alive = False
            break
        p = qmp.screendump_ppm(os.path.join(tmpdir, "s.ppm"))
        fr = read_ppm(p) if p else None
        if fr is None:
            continue
        now = time.time() - t0
        if t_change is None and frame_diff(fr, base) > 0.002:
            t_change = now
        if t_change is not None and prev is not None and fr[2] == prev[2]:
            n_still += 1
            if n_still >= still:
                t_settled = t_still
                prev = fr
                break
        else:
            n_still, t_still = 0, now
        prev = fr
    return prev, t_change, t_settled, alive


def press_enter(qmp, pump, tmpdir, base, retries=2, wait=6.0):
    """Enter, and again if the screen has not moved at all within `wait` s
    (the dropdown closing is enough).  Returns how many Enters were needed."""
    for k in range(1 + retries):
        qmp.hmp("sendkey ret")
        end = time.time() + wait
        while time.time() < end:
            pump(1.0)
            p = qmp.screendump_ppm(os.path.join(tmpdir, "e.ppm"))
            fr = read_ppm(p) if p else None
            if fr and frame_diff(fr, base) > 0.002:
                return k + 1
    return 0


def sites_run(qmp, args, run, pump):
    """Load each site, time it to a settled frame, PageDown twice, then follow
    a link through Firefox's quick-find-links (') + Enter.  Evidence per site:
    site-N-load.png, site-N-scroll.png, site-N-click.png and a summary row."""
    tmpdir = tempfile.mkdtemp(prefix="ffsites-")
    try:
        for n, (url, link) in enumerate(parse_sites(args.sites), 1):
            print("smoke-firefox: --sites [%d] %s" % (n, url))
            row = {"url": url, "n": n}
            first_line = len(run.lines)
            try:
                send_text(qmp, args, pump, url)
            except ValueError as exc:
                row["note"] = str(exc)
                run.site_rows.append(row)
                continue
            pump(2.0)
            p = qmp.screendump_ppm(os.path.join(tmpdir, "b.ppm"))
            base = read_ppm(p) if p else None
            t_enter = time.time()
            row["enters"] = press_enter(qmp, pump, tmpdir, base)
            off = time.time() - t_enter
            fr, tc, ts, alive = settle(qmp, pump, tmpdir, base, args.site_timeout)
            row["settled"] = None if ts is None else ts + off
            if fr:
                pth = os.path.join(args.outdir, "site-%d-load.png" % n)
                write_png(pth, fr)
                run.shots["site-%d-load" % n] = pth
            if alive:
                before = fr
                for _ in range(2):
                    qmp.hmp("sendkey pgdn")
                    pump(3.0)
                p = qmp.screendump_ppm(os.path.join(tmpdir, "c.ppm"))
                fr2 = read_ppm(p) if p else None
                row["scroll_diff"] = frame_diff(fr2, before) if fr2 else None
                if fr2:
                    pth = os.path.join(args.outdir, "site-%d-scroll.png" % n)
                    write_png(pth, fr2)
                    run.shots["site-%d-scroll" % n] = pth
                if link:
                    qmp.hmp("sendkey home")
                    pump(2.0)
                    try:
                        keys = qemu_keys_for("'" + link)
                    except ValueError:
                        keys = []
                    for k in keys:
                        qmp.hmp("sendkey " + k)
                        pump(args.type_delay)
                    pump(2.0)
                    p = qmp.screendump_ppm(os.path.join(tmpdir, "d.ppm"))
                    base2 = read_ppm(p) if p else fr2
                    t_enter = time.time()
                    row["click_enters"] = press_enter(qmp, pump, tmpdir, base2)
                    off = time.time() - t_enter
                    fr3, tc3, ts3, alive = settle(qmp, pump, tmpdir, base2, args.site_timeout)
                    row["link"] = link
                    row["click_settled"] = None if ts3 is None else ts3 + off
                    if fr3:
                        pth = os.path.join(args.outdir, "site-%d-click.png" % n)
                        write_png(pth, fr3)
                        run.shots["site-%d-click" % n] = pth
            lines = [l for _, l in run.lines[first_line:]]
            row["sig"] = [l for l in lines if SIG_KILLED.search(l)]
            row["ff_events"] = [l for l in lines if FF_STALLED in l or FF_EXITED in l or FF_CRASHREP in l]
            row["enosys"] = [l for l in lines if "ENOSYS" in l or "[SYSCALL] unimplemented" in l]
            row["oom"] = sum(1 for l in lines if l.startswith("[OOM]"))
            row["panic"] = any(PANIC in l for l in lines)
            run.site_rows.append(row)
            print("smoke-firefox:   %s" % site_row_text(row))
            if not alive or row["panic"]:
                break
            # back to a blank page so the next site's first change is its own
            send_text(qmp, args, pump, "about:blank")
            pump(1.0)
            qmp.hmp("sendkey ret")
            pump(5.0)
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)


def site_row_text(r):
    def s(t):
        return "%.0fs" % t if t is not None else "never"
    parts = ["[%d] %s" % (r["n"], r["url"]),
             "Enter x%s, settled +%s" % (r.get("enters"), s(r.get("settled")))]
    if r.get("scroll_diff") is not None:
        parts.append("PageDown moved %.0f%% of the screen" % (100 * r["scroll_diff"]))
    if r.get("link"):
        parts.append("link %r: Enter x%s, settled +%s" % (r["link"], r.get("click_enters"),
                                                            s(r.get("click_settled"))))
    if r.get("sig"):
        parts.append("%d [SIG] kill(s)" % len(r["sig"]))
    if r.get("ff_events"):
        parts.append("ff: " + "; ".join(r["ff_events"]))
    if r.get("enosys"):
        parts.append("ENOSYS: " + "; ".join(r["enosys"]))
    if r.get("oom"):
        parts.append("%d [OOM] line(s)" % r["oom"])
    if r.get("panic"):
        parts.append("KERNEL PANIC")
    if r.get("note"):
        parts.append(r["note"])
    return " | ".join(parts)


# Firefox's enterprise policies, /etc/firefox/policies/policies.json on the
# disk.  user.js's app.update.enabled has been ignored since Firefox 63; only
# the DisableAppUpdate policy keeps the run off Mozilla's update server.
FF_POLICIES = os.path.join("testfiles", "etc", "firefox", "policies", "policies.json")


def app_update_disabled():
    try:
        with open(FF_POLICIES) as f:
            return json.load(f).get("policies", {}).get("DisableAppUpdate") is True
    except (OSError, ValueError, AttributeError):
        return False


def set_gfxstats_marker(disk, tmp, on):
    """Put /gfxstats on the disk image (the desktop then traces its
    compositor counters every 10 s) or take it off again."""
    debugfs = shutil.which("debugfs") or "/sbin/debugfs"
    cmd = "rm gfxstats"
    if on:
        empty = os.path.join(tmp, "gfxstats")
        open(empty, "w").close()
        cmd = "write %s gfxstats" % empty
    subprocess.run([debugfs, "-w", "-R", cmd, disk],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


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
    ap.add_argument("--cpu", default=os.environ.get("SMOKE_CPU", "qemu32,+nx") or None,
                    help="QEMU -cpu model (default: $SMOKE_CPU, else qemu32,+nx: QEMU's "
                         "own model plus NX, so the JIT runs under W^X; \"\" = QEMU's own). "
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
    ap.add_argument("--net", action="store_true",
                    help="attach an rtl8139 on QEMU user networking, so the guest can reach "
                         "the host as 10.0.2.2 (e.g. --type 'http://10.0.2.2:8000/'). "
                         "Off by default: the default run has no NIC.")
    ap.add_argument("--nic", default="rtl8139", choices=["rtl8139", "e1000"],
                    help="the NIC model --net attaches (default rtl8139)")
    ap.add_argument("--web", action="store_true",
                    help="after the paint verdict, load a page served from the host "
                         "(implies --net) and require its image on screen; see above")
    ap.add_argument("--audio", action="store_true",
                    help="after the paint verdict open file:///disk/audio.html, a page that "
                         "plays a 440 Hz WAV through <audio autoplay> (both written into a "
                         "copy of the disk); the guest gets an Intel HDA recorded by QEMU and "
                         "Firefox the ALSA cubeb backend; the capture must hold the tone")
    ap.add_argument("--web-timeout", type=float, default=240.0,
                    help="seconds from Enter to the page's image on screen (default 240)")
    ap.add_argument("--font-timeout", type=float, default=60.0,
                    help="seconds after the image appears to wait for the page's font report (default 60)")
    ap.add_argument("--sites", default=None, metavar="LIST",
                    help="(implies --net; needs the live internet; manual only) after the paint, "
                         "load each site, time it to a settled frame, PageDown, and follow a link "
                         "via quick find. LIST is 'default' or a comma list of URL[|link text]")
    ap.add_argument("--site-timeout", type=float, default=150.0,
                    help="seconds to wait for a site's frame to settle (default 150)")
    ap.add_argument("--pcap", action="store_true",
                    help="(with --net/--web) record the NIC's traffic to net.pcap in the "
                         "artifact directory (QEMU filter-dump; read it with tcpdump -r)")
    ap.add_argument("--scroll", type=float, default=0, metavar="SEC",
                    help="after --web, hold Down on a long page for SEC seconds and report "
                         "the CPU split (KTRACE=1 kernel) and compositor counters")
    ap.add_argument("-v", "--verbose", action="store_true", help="echo every serial line")
    args = ap.parse_args()

    os.chdir(ROOT)
    if args.scroll:
        args.web = True
    if args.audio:
        args.net = True
    if args.web or args.sites:
        args.net = True
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
    if not app_update_disabled():
        problems.append("%s does not set DisableAppUpdate: Firefox then asks aus5.mozilla.org for "
                        "an update at startup and, unable to apply it, opens an 'Update available' "
                        "panel that takes the keyboard and keeps the guest busy" % FF_POLICIES)
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
    if args.scroll:
        set_gfxstats_marker(args.disk, tmp, True)
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
    if args.net:
        cmd += ["-netdev", "user,id=n0", "-device", "%s,netdev=n0" % args.nic]
        if args.pcap:
            cmd += ["-object", "filter-dump,id=pcap0,netdev=n0,file=%s"
                    % os.path.join(args.outdir, "net.pcap")]
    else:
        # QEMU's pc machine adds an e1000 unless told not to, and the kernel
        # drives it (drivers/e1000.c): keep the no-network run NIC-less.
        cmd += ["-nic", "none"]
    if args.audio:
        cmd += ["-audiodev", "wav,id=snd0,path=%s,out.frequency=48000,out.channels=2,out.format=s16"
                % os.path.join(args.outdir, "audio.wav"),
                "-device", "intel-hda", "-device", "hda-duplex,audiodev=snd0"]
        audio_web = WebServer(audio=True)
        disk = audio_disk(args, audio_web.port)
        cmd = [("file=%s,format=raw,if=ide" % disk) if c == "file=%s,format=raw,if=ide" % args.disk
               else c for c in cmd]
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
            if args.sites:
                sites_run(qmp, args, run, pump)
            if args.web:
                web = WebServer()
                try:
                    web_load(qmp, args, run, pump, web)
                    if args.scroll and run.result == "PASS":
                        scroll_run(qmp, args, run, pump, web)
                finally:
                    web.close()
            if args.audio:
                try:
                    audio_load(qmp, args, run, pump, audio_web)
                finally:
                    audio_web.close()
            # about:support's sandbox section, printed by maeros.cfg a few
            # seconds after start (ports/firefox/autoconfig).
            waited = 0.0
            while run.sandbox is None and waited < SANDBOX_WAIT:
                pump(1.0)
                waited += 1.0
            check_sandbox(run)
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
        if args.scroll:
            set_gfxstats_marker(args.disk, tmp, False)
        shutil.rmtree(tmp, ignore_errors=True)

    if args.audio:
        msg, ok = audio_capture_check(args, run)
        run.web_notes.append("audio " + msg)
        print("smoke-firefox: audio " + msg)
        if run.result == "PASS" and not ok:
            run.result, run.reason = "FAIL", "--audio: " + msg
    text = run.write_summary(cmd, accel, time.time() - run.t0)
    print()
    print(text)
    print("smoke-firefox: %s -- %s" % (run.result, run.reason))
    return 0 if run.result == "PASS" else (2 if run.result == "ERROR" else 1)


if __name__ == "__main__":
    sys.exit(main())
