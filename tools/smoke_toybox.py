#!/usr/bin/env python3
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
    # pkg's tar/name and index-signature checks are plain C: exercise them on
    # the host first.
    for test in ("test_pkg_tarx.py", "test_pkg_sign.py"):
        if subprocess.run([sys.executable,
                           os.path.join(ROOT, "tools", test)]).returncode:
            raise AssertionError(f"tools/{test} failed")

    proc = subprocess.Popen(
        ["make", "run"] + smokelib.MAKE_DISPLAY,
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
            ("toybox echo TOYBOX_OK\n", "TOYBOX_OK"),
            ("toybox cat hello.txt\n", "Hello from MaeroOS initrd!"),
            ("toybox pwd\n", "/"),
            ("toybox basename /tmp/example.txt\n", "example.txt"),
            ("toybox dirname /tmp/example.txt\n", "/tmp"),
            ("toybox printf %d 90\n", "90"),
            ("toybox head -n 1 hello.txt\n", "Hello from MaeroOS initrd!"),
            ("toybox wc hello.txt\n", "hello.txt"),
            ("toybox ls\n", "hello.txt"),
            ("toybox cut -c 1-5 hello.txt\n", "Hello"),
            ("toybox sort hello.txt\n", "Hello from MaeroOS initrd!"),
            ("toybox uniq hello.txt\n", "Hello from MaeroOS initrd!"),
            ("toybox date\n", "1970"),
            ("toybox sleep 0\n", PROMPT),
            ("toybox sleep 1\n", PROMPT),
            ("toybox uname\n", "Linux"),
            ("toybox whoami\n", "root"),
            ("toybox id\n", "uid=0"),
            # Syscall-layer regressions: long relative paths, symlink loops,
            # offsets, getdents layouts, waitpid(WUNTRACED).
            ("sysmiscprobe\n", "sysmiscprobe ok"),
            # Creation modes, O_APPEND, supplementary groups, access(),
            # TIOCSPGRP, unlink(dir) (userspace/abi2probe).
            ("abi2probe\n", "abi2probe ok"),
            # Signal state across fork/clone/execve: SIG_IGN, the blocked mask
            # and pending signals survive exec, handlers reset
            # (userspace/sigexecprobe).
            ("sigexecprobe\n", "sigexecprobe ok"),
            # alarm/setitimer/POSIX timers deliver signals that interrupt
            # blocking calls (EINTR / SA_RESTART); fork clears, exec keeps
            # (userspace/timerprobe).
            ("timerprobe\n", "timerprobe ok"),
            ("whoami\n", "root"),
            # libc regression checks (userspace/libctest)
            ("libctest\n", "LIBCTEST PASS"),
            # a path instead of a package name must be refused outright
            ("pkg remove ../etc\n", "invalid package name '../etc'"),
        ]
        for command, expected in checks:
            before = len("".join(log))
            send(proc, command)
            wait_for(proc, sel, PROMPT, log, start=before)
            recent = "".join(log)[before:]
            body = recent.split("\n", 1)[1] if "\n" in recent else recent
            if "error" in body.lower():
                raise AssertionError(f"command {command.strip()!r} reported an error")
            if "FAIL:" in body:
                raise AssertionError(f"command {command.strip()!r} reported a failure")
            if expected not in body:
                raise AssertionError(
                    f"command {command.strip()!r} did not produce {expected!r}"
                )

        # Log out of the console shell while a background job of its session
        # lives on; init starts a new getty, and the next login's shell runs
        # in a new session.  The old leader's exit must free the console so
        # the new shell gets it (job control, the foreground group) instead
        # of the orphaned job.
        before = len("".join(log))
        send(proc, "sleep 60 &\n")
        wait_for(proc, sel, PROMPT, log, start=before)
        before = len("".join(log))
        send(proc, "exit\n")
        wait_for(proc, sel, "exited; restarting", log, start=before)
        smokelib.login(proc, sel, log, start=before)
        before = len("".join(log))
        send(proc, "abi2probe tty\n")
        wait_for(proc, sel, PROMPT, log, start=before)
        if "abi2probe tty ok" not in "".join(log)[before:]:
            raise AssertionError("console was not freed when its session leader exited")

        print("\n[SMOKE-TOYBOX] passed")
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
        print(f"\n[SMOKE-TOYBOX] failed: {exc}", file=sys.stderr)
        raise SystemExit(1)
