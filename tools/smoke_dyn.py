#!/usr/bin/env python3
"""smoke-dyn — verify the dynamic-linker (ld.so) path.

Runs `dynprobe`, a dynamically-linked PIE built against musl that requires
/lib/ld-musl-i386.so.1.  Success means the kernel loaded the PIE + the
interpreter, the interpreter relocated/resolved libc symbols, and main ran.
Also re-runs a static binary (toybox) to confirm static loading still works.
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

    try:
        wait_for(proc, sel, PROMPT, log)

        # 1) Dynamic binary: must print DYNPROBE_OK via ld.so.
        before = len("".join(log))
        send(proc, "/dynprobe\n")
        wait_for(proc, sel, PROMPT, log, start=before)
        body = "".join(log)[before:]
        if "DYNPROBE_OK" not in body:
            raise AssertionError("dynprobe did not print DYNPROBE_OK (ld.so path broken)")
        if "needs ld.so" not in body:
            raise AssertionError("dynprobe was not recognized as needing the dynamic linker")
        if "ld-musl-i386.so.1" not in body:
            raise AssertionError("the musl dynamic linker was not loaded")

        # 2) Static binary must still load and run unchanged.
        before = len("".join(log))
        send(proc, "toybox echo STATIC_OK\n")
        wait_for(proc, sel, PROMPT, log, start=before)
        if "STATIC_OK" not in "".join(log)[before:]:
            raise AssertionError("static binary (toybox) regressed")

        print("\n[SMOKE-DYN] passed")
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
        print(f"\n[SMOKE-DYN] failed: {exc}", file=sys.stderr)
        raise SystemExit(1)
