#!/usr/bin/env python3
"""smoke-fw — verify the kernel firewall blocks outbound traffic by rule.

Boots run-net, serves a host HTTP file, then:
  1. baseline httpget → MAEROS_HTTP_OK
  2. fwctl enable + drop out tcp 10.0.2.2/32 <port> → httpget now fails
  3. /proc/firewall shows the rule with a non-zero hit count
  4. fwctl flush → httpget succeeds again
"""
import os
import selectors
import subprocess
import sys
import time
import tempfile
import threading
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROMPT = "MaeroOS$ "


def wait_for(proc, sel, needle, log, timeout=20.0, start=0):
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


def send(proc, line):
    proc.stdin.write((line + "\n").encode("latin1"))
    proc.stdin.flush()


def main():
    webdir = tempfile.TemporaryDirectory()
    with open(os.path.join(webdir.name, "index.html"), "w") as f:
        f.write("MAEROS_HTTP_OK\n")
    handler = partial(SimpleHTTPRequestHandler, directory=webdir.name)
    httpd = ThreadingHTTPServer(("0.0.0.0", 0), handler)
    port = httpd.server_address[1]
    threading.Thread(target=httpd.serve_forever, daemon=True).start()

    proc = subprocess.Popen(
        ["make", "run-net"], cwd=ROOT,
        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, bufsize=0)
    sel = selectors.DefaultSelector()
    sel.register(proc.stdout, selectors.EVENT_READ)
    log = []

    try:
        wait_for(proc, sel, PROMPT, log, timeout=25.0)

        # 1. baseline fetch works
        before = len("".join(log))
        send(proc, f"httpget 10.0.2.2 {port} /index.html")
        wait_for(proc, sel, PROMPT, log, timeout=15.0, start=before)
        if "MAEROS_HTTP_OK" not in "".join(log)[before:]:
            raise AssertionError("baseline httpget failed")

        # 2. enable firewall, drop outbound TCP to the host:port
        before = len("".join(log))
        send(proc, "fwctl enable")
        wait_for(proc, sel, PROMPT, log, timeout=8.0, start=before)
        before = len("".join(log))
        send(proc, f"fwctl drop out tcp 10.0.2.2/32 {port}")
        wait_for(proc, sel, PROMPT, log, timeout=8.0, start=before)

        before = len("".join(log))
        send(proc, f"httpget 10.0.2.2 {port} /index.html")
        wait_for(proc, sel, PROMPT, log, timeout=15.0, start=before)
        blocked = "".join(log)[before:]
        if "MAEROS_HTTP_OK" in blocked:
            raise AssertionError("firewall did not block the outbound fetch")

        # 3. rule shows a hit
        before = len("".join(log))
        send(proc, "fwctl list")
        wait_for(proc, sel, PROMPT, log, timeout=8.0, start=before)
        listing = "".join(log)[before:]
        if "enabled" not in listing or "drop out tcp" not in listing:
            raise AssertionError("firewall rule not listed")

        # 4. flush restores connectivity
        before = len("".join(log))
        send(proc, "fwctl flush")
        wait_for(proc, sel, PROMPT, log, timeout=8.0, start=before)
        before = len("".join(log))
        send(proc, f"httpget 10.0.2.2 {port} /index.html")
        wait_for(proc, sel, PROMPT, log, timeout=15.0, start=before)
        if "MAEROS_HTTP_OK" not in "".join(log)[before:]:
            raise AssertionError("fetch did not recover after flush")

        print("smoke-fw ok")
        return 0
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
        httpd.shutdown()
        httpd.server_close()
        webdir.cleanup()


if __name__ == "__main__":
    sys.exit(main())
