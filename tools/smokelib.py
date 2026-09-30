"""Shared bits of the smoke harnesses (tools/smoke*.py).

The console runs getty, so every suite logs in before its first command:
login() answers the "login:" and "Password:" prompts and waits for the shell.
The default accounts are root/root and user/user (testfiles/etc/shadow).

QEMU runs headless (-display none) so the suites work on hosts without a
display (ssh, CI).  Set SMOKE_DISPLAY=1 to get QEMU's window back.
"""
import os
import sys
import time

PROMPT = "MaeroOS$ "
LOGIN_PROMPT = " login: "
PASSWORD_PROMPT = "Password: "

# Extra QEMU arguments for every suite; empty with SMOKE_DISPLAY set.
QEMU_DISPLAY = [] if os.environ.get("SMOKE_DISPLAY") else ["-display", "none"]
# The same for suites that boot through a Makefile run target.
MAKE_DISPLAY = [] if not QEMU_DISPLAY else ["QEMU_DISPLAY=" + " ".join(QEMU_DISPLAY)]


def _wait(proc, sel, needle, log, timeout, start):
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


def _send(proc, text):
    proc.stdin.write(text.encode("latin1"))
    proc.stdin.flush()


def login(proc, sel, log, user="root", password="root", timeout=60.0, start=0):
    """Wait for getty's prompt (after log offset `start`), log in, and wait
    for the shell prompt.  `timeout` covers the boot up to the prompt."""
    _wait(proc, sel, LOGIN_PROMPT, log, timeout, start)
    before = len("".join(log))
    _send(proc, user + "\n")
    _wait(proc, sel, PASSWORD_PROMPT, log, 20.0, before)
    before = len("".join(log))
    _send(proc, password + "\n")
    _wait(proc, sel, PROMPT, log, 30.0, before)
