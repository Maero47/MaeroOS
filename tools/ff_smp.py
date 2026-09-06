#!/usr/bin/env python3
"""Drive Firefox under -smp 2 to test whether multi-core breaks the
parent<->socket IPC handshake deadlock.  Boots kernel+initrd+disk-ff with two
CPUs, waits for the serial shell, types `ff`, and captures everything.

Usage: python3 tools/ff_smp.py [run_seconds]
"""
import os, selectors, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
PROMPT = "MaeroOS$ "
RUN_SECONDS = float(sys.argv[1]) if len(sys.argv) > 1 else 150.0

QEMU = [
    "qemu-system-i386",
    "-kernel", "kernel.elf",
    "-initrd", "initrd.tar",
    "-drive", "file=disk-ff.img,format=raw,if=ide",
    "-smp", "2",
    "-serial", "stdio",
    "-display", "none",
    "-m", "2048M",
    "-no-reboot", "-no-shutdown",
]

def main():
    proc = subprocess.Popen(QEMU, cwd=ROOT, stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=0)
    sel = selectors.DefaultSelector()
    sel.register(proc.stdout, selectors.EVENT_READ)
    log = []
    logf = open("/tmp/ff_smp.log", "w")

    def pump(deadline):
        while time.time() < deadline:
            for key, _ in sel.select(0.2):
                chunk = os.read(key.fd, 65536).decode("latin1", "replace")
                if chunk:
                    log.append(chunk); logf.write(chunk); logf.flush()
            if proc.poll() is not None:
                return False
        return True

    # Wait for the serial shell prompt (graphical session still starts too).
    boot_deadline = time.time() + 90
    while time.time() < boot_deadline:
        pump(time.time() + 0.5)
        if PROMPT in "".join(log):
            break
    if PROMPT not in "".join(log):
        print("\n!!! never reached shell prompt", flush=True)
    else:
        print("\n=== got prompt, launching ff ===", flush=True)
        proc.stdin.write(b"ff\n"); proc.stdin.flush()

    pump(time.time() + RUN_SECONDS)
    try:
        proc.terminate()
    except Exception:
        pass
    logf.close()

    full = "".join(log)
    print("\n\n========== SUMMARY ==========")
    for marker in ["putimg", "PutImage", "Compositor", "WebRender", "first paint",
                   "MapWindow", "CreateWindow", "socket process", "Socket Process",
                   "panic", "page fault", "GPF", "CPU 1 dispatched"]:
        c = full.lower().count(marker.lower())
        if c:
            print(f"  {marker:20s} x{c}")

if __name__ == "__main__":
    main()
