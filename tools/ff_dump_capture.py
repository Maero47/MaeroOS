#!/usr/bin/env python3
"""Boot the -kernel (headless) path under -smp 2, run `ff` (watchdog + Firefox),
and capture maeroX's serial FRAME DUMPS (base64 half-res RGB between FFDUMP /
FFDUMPEND markers), decoding each to a PNG.  This makes the proven-but-headless
paint (putimg>0, no framebuffer) actually viewable without the flaky ISO path.

Usage: python3 tools/ff_dump_capture.py [run_seconds]
Writes /tmp/ff_dump_N.png for each frame dumped.
"""
import base64, os, re, subprocess, sys, time, struct, zlib

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
RUN_SECONDS = float(sys.argv[1]) if len(sys.argv) > 1 else 400.0
PROMPT = "MaeroOS$ "

QEMU = [
    "qemu-system-i386", "-kernel", "kernel.elf", "-initrd", "initrd.tar",
    "-drive", "file=disk-ff.img,format=raw,if=ide",
    "-smp", "2", "-serial", "stdio", "-display", "none",
    "-m", "2048M", "-no-reboot", "-no-shutdown",
]

def write_png(path, w, h, rgb):
    def chunk(typ, data):
        c = typ + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xffffffff)
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw.extend(rgb[y*w*3:(y+1)*w*3])
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    open(path, "wb").write(png)

def main():
    proc = subprocess.Popen(QEMU, cwd=ROOT, stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=0)
    buf = ""
    launched = False
    ndumps = 0
    deadline = time.time() + RUN_SECONDS + 90
    logf = open("/tmp/ff_dump.log", "w")
    try:
        while time.time() < deadline:
            chunk = proc.stdout.read1(65536) if hasattr(proc.stdout, "read1") else proc.stdout.read(4096)
            if not chunk:
                if proc.poll() is not None:
                    break
                continue
            s = chunk.decode("latin1", "replace")
            buf += s
            logf.write(s); logf.flush()
            if not launched and PROMPT in buf:
                launched = True
                proc.stdin.write(b"ff\n"); proc.stdin.flush()
                print("=== launched ff ===", flush=True)
                deadline = time.time() + RUN_SECONDS
            # extract complete dumps
            while "FFDUMP " in buf and "FFDUMPEND" in buf:
                m = re.search(r"FFDUMP (\d+) (\d+)\n(.*?)FFDUMPEND", buf, re.S)
                if not m:
                    break
                w, h = int(m.group(1)), int(m.group(2))
                # The base64 is interleaved with kernel printk lines on the shared
                # serial console.  maeroX writes each base64 row as one 76-char
                # line; kernel log lines contain spaces/brackets.  Keep only lines
                # that are ENTIRELY base64 chars (drops the interleaved kernel log).
                b64 = "".join(
                    ln.strip() for ln in m.group(3).splitlines()
                    if re.fullmatch(r"[A-Za-z0-9+/]{4,76}={0,2}", ln.strip())
                )
                b64 = b64[: len(b64) // 4 * 4]
                buf = buf[m.end():]
                try:
                    rgb = base64.b64decode(b64)
                    if len(rgb) >= w*h*3:
                        ndumps += 1
                        path = f"/tmp/ff_dump_{ndumps}.png"
                        write_png(path, w, h, rgb[:w*h*3])
                        print(f"[DUMP {ndumps}] {w}x{h} -> {path} ({len(rgb)} bytes)", flush=True)
                except Exception as e:
                    print("decode error:", e, flush=True)
            if len(buf) > 8_000_000:
                buf = buf[-2_000_000:]
    finally:
        try: proc.terminate()
        except Exception: pass
        logf.close()
    print(f"=== done, {ndumps} frame dump(s) ===", flush=True)

if __name__ == "__main__":
    main()
