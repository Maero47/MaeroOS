#!/usr/bin/env python3
import os
import selectors
import subprocess
import sys
import time


ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
PROMPT = "MaeroOS$ "
TIMEOUT = 20.0


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
            ("ls\n", "hello.txt"),
            ("cat hello.txt\n", "Hello from MaeroOS initrd!"),
            ("printf ok\n", "ok"),
            ("sysprobe\n", "sysprobe ok"),
            ("shmprobe\n", "shmprobe ok"),
            ("threadprobe\n", "threadprobe ok"),
            ("busybox sh -c 'busybox seq 3 | busybox tail -1; busybox awk \"BEGIN{printf \\\"fpu %.1f\\\\n\\\", 2.5*2}\"'\n", "fpu 5.0"),
            ("randprobe\n", "randprobe getrandom ok"),
            ("randprobe\n", "randprobe urandom ok"),
            ("ptytest\n", "ptytest ok"),
            ("cttytest\n", "cttytest ok"),
            ("cat /proc/self/status\n", "Pgid:"),
            ("cat /proc/processes\n", "PID PPID PGRP SID STATE TTY TIME NAME"),
            ("cat /proc/1/status\n", "Pid:\t1"),
            ("cat /proc/1/stat\n", "1 ("),
            ("dmesg\n", "[BOOT]"),
            ("cat /proc/kmsg\n", "MaeroOS Kernel"),
            ("ps\n", "PID PPID PGRP SID STATE TTY TIME NAME", "cttytest"),
        ]

        for check in checks:
            command, expected = check[0], check[1]
            forbidden = check[2] if len(check) > 2 else None
            before = len("".join(log))
            send(proc, command)
            wait_for(proc, sel, PROMPT, log, start=before)
            recent = "".join(log)[before:]
            if expected not in recent:
                raise AssertionError(
                    f"command {command.strip()!r} did not produce {expected!r}"
                )
            if forbidden and forbidden in recent:
                raise AssertionError(
                    f"command {command.strip()!r} unexpectedly produced {forbidden!r}"
                )

        print("\n[SMOKE] passed")
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
        print(f"\n[SMOKE] failed: {exc}", file=sys.stderr)
        raise SystemExit(1)
