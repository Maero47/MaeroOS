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


def wait_for(proc, sel, needle, log, timeout=20.0, start=0):
    """Wait until `needle` appears in the console output after offset
    `start`, reading more into `log` as needed.  Returns the offset just past
    the match, which is where the next wait should start.

    Output already in `log` is searched first: one read often carries both
    the reply being waited for and the next prompt, and a wait that looked
    only at newly read data would miss a prompt that is already there."""
    deadline = time.time() + timeout
    while True:
        text = "".join(log)
        at = text.find(needle, start)
        if at >= 0:
            return at + len(needle)
        if time.time() >= deadline:
            raise TimeoutError(f"timed out waiting for {needle!r}")
        for key, _ in sel.select(0.2):
            chunk = os.read(key.fd, 4096).decode("latin1", "replace")
            if not chunk:
                continue
            log.append(chunk)
            sys.stdout.write(chunk)
            sys.stdout.flush()
        if proc.poll() is not None:
            raise RuntimeError(f"QEMU exited with status {proc.returncode}")


def send(proc, text):
    proc.stdin.write(text.encode("latin1"))
    proc.stdin.flush()


def mark(log):
    """The current end of the console output: take it before send()ing, and
    pass it as `start` to wait for the reply."""
    return len("".join(log))


def login(proc, sel, log, user="root", password="root", timeout=60.0, start=0):
    """Wait for getty's prompt (after log offset `start`), log in, and wait
    for the shell prompt.  `timeout` covers the boot up to the prompt.
    Returns the offset just past the shell prompt."""
    wait_for(proc, sel, LOGIN_PROMPT, log, timeout, start)
    # Nothing is typed until the prompt is out, so the reply to each line
    # starts after the output seen so far.
    at = mark(log)
    send(proc, user + "\n")
    wait_for(proc, sel, PASSWORD_PROMPT, log, 20.0, at)
    at = mark(log)
    send(proc, password + "\n")
    return wait_for(proc, sel, PROMPT, log, 30.0, at)
