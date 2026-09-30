#!/usr/bin/env python3
"""Smoke test for the Phase 28 Linux-parity commands over serial.

Boots MaeroOS (initrd) and exercises the native coreutils-style commands:
uname, whoami, hostname, free, df, uptime, which, clear.  reboot is checked
last because it powers the machine off.

Runs kwprobe first (a user write to kernel memory must die of SIGSEGV, a user
int3 of SIGTRAP); the commands after it show the system kept running.

SMOKE_SMP=N boots the same machine as `make run` with -smp N (the kernel and
initrd must already be built, as `make smoke-cmds` does).
"""
import os
import selectors
import subprocess
import sys
import time

import smokelib


ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
PROMPT = "MaeroOS$ "
TIMEOUT = 25.0


def wait_for(proc, sel, needle, log, timeout=TIMEOUT, start=0):
    return smokelib.wait_for(proc, sel, needle, log, timeout, start)


def send(proc, text):
    smokelib.send(proc, text)


def main():
    smp = os.environ.get("SMOKE_SMP")
    if smp:
        # Same flags as the Makefile's `run` target, plus -smp.
        cmd = ["qemu-system-i386", *smokelib.QEMU_DISPLAY, "-kernel", "kernel.elf",
               "-initrd", "initrd.tar", "-serial", "stdio", "-m", "512M",
               "-no-reboot", "-no-shutdown",
               "-smp", str(int(smp))]
    else:
        cmd = ["make", "run"] + smokelib.MAKE_DISPLAY
    proc = subprocess.Popen(
        cmd,
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
        smokelib.login(proc, sel, log)
        checks = [
            ("kwprobe\n", "kwprobe ok"),
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
