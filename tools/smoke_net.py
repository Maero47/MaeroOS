#!/usr/bin/env python3
import os
import selectors
import socket
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
    index_path = os.path.join(webdir.name, "index.html")
    with open(index_path, "w", encoding="ascii") as f:
        f.write("MAEROS_HTTP_OK\n")

    handler = partial(SimpleHTTPRequestHandler, directory=webdir.name)
    httpd = ThreadingHTTPServer(("0.0.0.0", 0), handler)
    http_port = httpd.server_address[1]
    http_thread = threading.Thread(target=httpd.serve_forever, daemon=True)
    http_thread.start()

    proc = subprocess.Popen(
        ["make", "run-net"],
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
        wait_for(proc, sel, PROMPT, log, timeout=25.0)
        before = len("".join(log))
        send(proc, "lspci")
        wait_for(proc, sel, PROMPT, log, timeout=10.0, start=before)
        recent = "".join(log)[before:]
        if "10ec:8139" not in recent:
            raise AssertionError("RTL8139 PCI device was not listed")
        if "class=02:00" not in recent:
            raise AssertionError("network class code was not listed")
        before = len("".join(log))
        send(proc, "ifconfig")
        wait_for(proc, sel, PROMPT, log, timeout=10.0, start=before)
        recent = "".join(log)[before:]
        if "eth0: rtl8139 up" not in recent:
            raise AssertionError("RTL8139 interface did not initialize")
        if "ip=10.0.2.15" not in recent or "gw=10.0.2.2" not in recent:
            raise AssertionError("DHCP did not assign the expected QEMU user-net address")
        before = len("".join(log))
        send(proc, "netprobe")
        wait_for(proc, sel, PROMPT, log, timeout=10.0, start=before)
        recent = "".join(log)[before:]
        if "netprobe ok" not in recent:
            raise AssertionError("network packet probe failed")
        before = len("".join(log))
        send(proc, "sockprobe")
        wait_for(proc, sel, PROMPT, log, timeout=10.0, start=before)
        recent = "".join(log)[before:]
        if "sockprobe udp ok" not in recent:
            raise AssertionError("socket ABI probe failed")
        before = len("".join(log))
        send(proc, f"httpget 10.0.2.2 {http_port} /index.html")
        wait_for(proc, sel, PROMPT, log, timeout=15.0, start=before)
        recent = "".join(log)[before:]
        if "MAEROS_HTTP_OK" not in recent:
            raise AssertionError("HTTP fetch did not return host response")
        print("smoke-net ok")
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
