#!/usr/bin/env python3
"""Smoke test for the Phase 28 Linux-parity commands over serial.

Boots MaeroOS (initrd) and exercises the native coreutils-style commands:
uname, whoami, hostname, free, df, uptime, which, clear.  reboot is checked
last because it powers the machine off.
"""
import os
import selectors
import subprocess
import sys
import time


ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
PROMPT = "MaeroOS$ "
TIMEOUT = 25.0


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
        checks = [
            ("uname\n", "MaeroOS"),
            ("uname -a\n", "i686"),
            ("uname -m\n", "i686"),
            ("whoami\n", "root"),
            ("hostname\n", "maeros"),
            ("free\n", "Mem:"),
            ("df\n", "Filesystem"),
            ("uptime\n", "up"),
            ("which uname\n", "uname"),
        ]
        for command, expected in checks:
            before = len("".join(log))
            send(proc, command)
            wait_for(proc, sel, PROMPT, log, start=before)
            recent = "".join(log)[before:]
            body = recent.split("\n", 1)[1] if "\n" in recent else recent
            if expected not in body:
                raise AssertionError(
                    f"command {command.strip()!r} did not produce {expected!r}"
                )
            # which must not emit a doubled slash like //uname
            if command.startswith("which") and "//" in body:
                raise AssertionError("which produced a doubled-slash path")

        print("\n[SMOKE-CMDS] passed")
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
        print(f"\n[SMOKE-CMDS] failed: {exc}", file=sys.stderr)
        raise SystemExit(1)
