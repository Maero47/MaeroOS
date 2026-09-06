#!/usr/bin/env python3
"""smoke-dynlib — verify loading of EXTERNAL shared libraries (Firefox rung 2).

dynprobe2 calls into a custom external .so (libgreet.so.1) → GREET_OK, proving
cross-module relocation.  zprobe links a real third-party shared library
(libz.so.1 / zlib, a Firefox dependency) → ZLIB_OK, proving the loader on a
genuine library.  Phase-30 dynprobe and a static binary must still run.
"""
import os
import selectors
import subprocess
import sys
import time


ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
PROMPT = "MaeroOS$ "
TIMEOUT = 30.0


def wait_for(proc, sel, needle, log, timeout=TIMEOUT, start=0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        for key, _ in sel.select(0.2):
            chunk = os.read(key.fd, 4096).decode("latin1", "replace")
            if not chunk:
                continue
            log.append(chunk)
            sys.stdout.write(chunk)
            sys.stdout.flush()
            if needle in "".join(log)[start:]:
                return
        if proc.poll() is not None:
            raise RuntimeError(f"QEMU exited with status {proc.returncode}")
    raise TimeoutError(f"timed out waiting for {needle!r}")


def send(proc, text):
    proc.stdin.write(text.encode("latin1"))
    proc.stdin.flush()


def main():
    proc = subprocess.Popen(
        ["make", "run"],
        cwd=ROOT,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        bufsize=0,
    )
    sel = selectors.DefaultSelector()
    sel.register(proc.stdout, selectors.EVENT_READ)
    log = []

    def run(cmd, *needles):
        before = len("".join(log))
        send(proc, cmd + "\n")
        wait_for(proc, sel, PROMPT, log, start=before)
        body = "".join(log)[before:]
        for n in needles:
            if n not in body:
                raise AssertionError(f"{cmd!r} missing {n!r}")
        return body

    try:
        wait_for(proc, sel, PROMPT, log, timeout=75)

        # External custom shared library + cross-module call.  GREET_OK comes
        # from libgreet.so.1 and sum=42 from its greet_add() — both prove the
        # external .so loaded and relocated (it is mmap'd by ld.so in user
        # space, so its name does not appear in the kernel serial log).
        run("/dynprobe2", "GREET_OK", "sum=42")
        # Real third-party shared library (zlib): version + compress round-trip.
        run("/zprobe", "ZLIB_OK", "ver=1.3")
        # Real multithreading: 4 threads, mutex-guarded counter, per-thread TLS.
        run("/pthreadprobe", "THREADS_OK", "count=200000")
        # AF_UNIX local sockets: socketpair + named bind/listen/connect/accept.
        run("/usockprobe", "UNIX_SOCK_OK")
        # Phase-30 single-dependency dynamic binary still works.
        run("/dynprobe", "DYNPROBE_OK")
        # Static binary still works.
        run("toybox echo STATIC_OK", "STATIC_OK")

        print("\n[SMOKE-DYNLIB] passed")
        return 0
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                proc.kill()


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"\n[SMOKE-DYNLIB] failed: {exc}", file=sys.stderr)
        raise SystemExit(1)
