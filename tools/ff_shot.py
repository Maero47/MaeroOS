#!/usr/bin/env python3
"""Boot the GRUB ISO (framebuffer-capable) with the Firefox disk under -smp 2,
launch Firefox, and capture a screenshot of the VGA surface via the QEMU
monitor `screendump`.  This is the honesty-gate proof that Firefox paints.

Usage: python3 tools/ff_shot.py [wait_seconds] [out.ppm]
"""
import os, selectors, socket, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
PROMPT = "MaeroOS$ "
WAIT = float(sys.argv[1]) if len(sys.argv) > 1 else 150.0
OUT = sys.argv[2] if len(sys.argv) > 2 else "/tmp/ff_shot.ppm"
MON = "/tmp/ff_qmon.sock"

if os.path.exists(MON):
    os.unlink(MON)

QEMU = [
    "qemu-system-i386",
    "-cdrom", "maeros.iso",
    "-drive", "file=disk-ff.img,format=raw,if=ide",
    "-smp", "2",
    "-m", "2048M",
    "-vga", "std",
    "-display", "none",
    "-serial", "stdio",
    "-monitor", f"unix:{MON},server,nowait",
    "-no-reboot", "-no-shutdown",
]

def mon_cmd(cmd):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(MON)
    time.sleep(0.3)
    try:
        s.recv(4096)
    except Exception:
        pass
    s.sendall((cmd + "\n").encode())
    time.sleep(1.0)
    try:
        data = s.recv(4096).decode("latin1", "replace")
    except Exception:
        data = ""
    s.close()
    return data

def main():
    proc = subprocess.Popen(QEMU, cwd=ROOT, stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=0)
    sel = selectors.DefaultSelector()
    sel.register(proc.stdout, selectors.EVENT_READ)
    log = []
    logf = open("/tmp/ff_shot.log", "w")

    def pump(deadline):
        while time.time() < deadline:
            for key, _ in sel.select(0.2):
                chunk = os.read(key.fd, 65536).decode("latin1", "replace")
                if chunk:
                    log.append(chunk); logf.write(chunk); logf.flush()
            if proc.poll() is not None:
                return False
        return True

    boot_deadline = time.time() + 90
    while time.time() < boot_deadline:
        pump(time.time() + 0.5)
        if PROMPT in "".join(log):
            break
    if PROMPT in "".join(log):
        print("\n=== prompt; launching ff ===", flush=True)
        proc.stdin.write(b"ff\n"); proc.stdin.flush()
    else:
        print("\n!!! no prompt", flush=True)

    # Let Firefox start + paint, taking periodic screenshots.
    shots = []
    t_end = time.time() + WAIT
    n = 0
    while time.time() < t_end:
        pump(time.time() + 20)
        n += 1
        out = OUT.replace(".ppm", f"_{n}.ppm")
        r = mon_cmd(f"screendump {out}")
        if os.path.exists(out) and os.path.getsize(out) > 0:
            shots.append(out)
            print(f"[shot {n}] {out} ({os.path.getsize(out)} bytes)", flush=True)

    # Final shot
    r = mon_cmd(f"screendump {OUT}")
    if os.path.exists(OUT):
        print(f"[final] {OUT} ({os.path.getsize(OUT)} bytes)", flush=True)
    try:
        proc.terminate()
    except Exception:
        pass
    logf.close()

    full = "".join(log)
    for m in ["putimg=", "render="]:
        ln = [l for l in full.splitlines() if "putimg=" in l]
        if ln:
            print("LAST XT:", ln[-1][-60:])
            break

if __name__ == "__main__":
    main()
