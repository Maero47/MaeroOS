#!/usr/bin/env python3
"""smoke-x — verify the maeroX X11 server connection handshake (Firefox rung 5).

Launches maeroX headless in the background (it binds the AF_UNIX socket
/tmp/.X11-unix/X0 and listens), then runs xprobe — a raw X11 client that
performs the connection setup and validates the reply.  XHANDSHAKE_OK means the
byte-exact handshake over our AF_UNIX sockets works end to end.
"""
import os
import selectors
import subprocess
import sys
import time


ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
PROMPT = "MaeroOS$ "


def wait_for(proc, sel, needle, log, timeout=30, start=0):
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
                return True
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
        wait_for(proc, sel, PROMPT, log, timeout=75)

        # xprobe spawns the real maeroX server (headless), connects over the
        # AF_UNIX socket, and runs the X11 connection handshake.
        before = len("".join(log))
        send(proc, "xprobe\n")
        wait_for(proc, sel, PROMPT, log, timeout=25, start=before)
        body = "".join(log)[before:]
        if "XHANDSHAKE_OK" not in body:
            raise AssertionError("xprobe did not complete the X11 handshake")
        if "screens=1" not in body:
            raise AssertionError("setup reply did not advertise a screen")

        # xdraw creates a window, draws into it, and round-trips GetGeometry —
        # exercising the full request loop (CreateWindow/GC/Map/Fill/PutImage).
        before = len("".join(log))
        send(proc, "xdraw --spawn\n")
        wait_for(proc, sel, PROMPT, log, timeout=25, start=before)
        body = "".join(log)[before:]
        if "XDRAW_OK" not in body:
            raise AssertionError("xdraw did not complete the X11 drawing round-trip")
        if "w=320 h=200" not in body:
            raise AssertionError("GetGeometry did not return the created window size")

        # xevent maps a window and waits for the Expose event the server sends.
        before = len("".join(log))
        send(proc, "xevent --spawn\n")
        wait_for(proc, sel, PROMPT, log, timeout=25, start=before)
        body = "".join(log)[before:]
        if "XEVENT_OK" not in body:
            raise AssertionError("xevent did not receive an Expose event")

        # xkey exercises the keyboard: it takes the input focus, reads the
        # keymap and the modifier map off the wire, injects keys through
        # maeroX's test channel and asserts each arrives as a KeyPress with the
        # right keycode and state and translates to the right character.
        before = len("".join(log))
        send(proc, "xkey --spawn\n")
        wait_for(proc, sel, PROMPT, log, timeout=40, start=before)
        body = "".join(log)[before:]
        if "XKEY_OK" not in body:
            raise AssertionError("xkey did not verify the keyboard path")

        # xreal is a REAL Xlib client (linked against cross-built libX11/libxcb):
        # XOpenDisplay -> XCreateSimpleWindow -> XMapWindow -> XFillRectangle.
        before = len("".join(log))
        send(proc, "xreal\n")
        wait_for(proc, sel, PROMPT, log, timeout=40, start=before)
        body = "".join(log)[before:]
        if "XREAL_PAINTED" not in body:
            raise AssertionError("real Xlib client did not connect+paint via libX11")

        print("\n[SMOKE-X] passed")
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
        print(f"\n[SMOKE-X] failed: {exc}", file=sys.stderr)
        raise SystemExit(1)
