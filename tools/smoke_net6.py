#!/usr/bin/env python3
"""smoke-net6: loopback and IPv6 over QEMU user networking with IPv6 on
(-netdev user,ipv4=on,ipv6=on, QEMU's default: slirp advertises fec0::/64 and is the host at fec0::2).

Checks, in the guest (e1000, disk.img as a snapshot for a writable /etc):
  - SLAAC: /proc/net/if_inet6 shows eth0's fe80:: and fec0:: addresses and
    lo's ::1, and /proc/net/dev lists lo;
  - net6probe lo: TCP over 127.0.0.1 and [::1] (a guest server reached by a
    guest client), an AF_INET6 dual-stack listener reached from AF_INET
    (peer ::ffff:127.0.0.1), IPV6_V6ONLY, UDP over ::1 and dual-stack, ICMP
    echo to 127.0.0.1 and ::1 over ping sockets, SO_LINGER;
  - toybox wget from a host HTTP server over IPv6 (http://[fec0::2]:port/,
    which slirp hands to the host's ::1), and over IPv4 as before;
  - getaddrinfo(AF_UNSPEC) of a dual-stack name from a DNS responder here:
    both AAAA and A, the IPv4 one first (fec0::/10 is below IPv4 in RFC
    6724's policy table), and localhost as ::1 then 127.0.0.1;
  - net6probe tcp to the dual-stack name connects (first address wins).
"""
import os
import selectors
import socket
import subprocess
import sys
import tempfile
import threading
import time
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer

import dns_responder
import smokelib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROMPT = "MaeroOS$ "


def send(proc, line):
    proc.stdin.write((line + "\n").encode("latin1"))
    proc.stdin.flush()


def run(proc, sel, log, cmd, timeout=20.0):
    before = len("".join(log))
    send(proc, cmd)
    smokelib.wait_for(proc, sel, PROMPT, log, timeout=timeout, start=before)
    return "".join(log)[before:]


class Http11Handler(SimpleHTTPRequestHandler):
    """toybox wget only accepts an "HTTP/1.1" status line."""
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass


class V6Server(ThreadingHTTPServer):
    address_family = socket.AF_INET6


def main():
    webdir = tempfile.TemporaryDirectory()
    with open(os.path.join(webdir.name, "index.html"), "w", encoding="ascii") as f:
        f.write("MAEROS_HTTP6_OK\n")
    handler = partial(Http11Handler, directory=webdir.name)
    # slirp connects a guest's [fec0::2]:port to the host's [::1]:port and
    # 10.0.2.2:port to 127.0.0.1:port: one port, both families.
    httpd6 = V6Server(("::1", 0), handler)
    port = httpd6.server_address[1]
    httpd4 = ThreadingHTTPServer(("127.0.0.1", port), handler)
    for h in (httpd6, httpd4):
        threading.Thread(target=h.serve_forever, daemon=True).start()

    dns = dns_responder.DnsResponder({
        "dual.maeros.test": {"A": ["10.0.2.2"], "AAAA": ["fec0::2"]},
    })

    proc = subprocess.Popen(
        ["qemu-system-i386", *smokelib.QEMU_DISPLAY, "-kernel", "kernel.elf",
         "-initrd", "initrd.tar",
         "-drive", "file=disk.img,format=raw,index=0,media=disk,snapshot=on",
         "-serial", "stdio", "-m", "128M", "-no-reboot", "-no-shutdown",
         "-netdev", "user,id=n0,ipv4=on,ipv6=on", "-device", "e1000,netdev=n0"],
        cwd=ROOT, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, bufsize=0)
    sel = selectors.DefaultSelector()
    sel.register(proc.stdout, selectors.EVENT_READ)
    log = []
    try:
        smokelib.login(proc, sel, log, timeout=40.0)
        deadline = time.time() + 20
        while "ip=10.0.2.15" not in run(proc, sel, log, "ifconfig"):
            if time.time() > deadline:
                raise AssertionError("no DHCP lease within 20 s")
            time.sleep(0.5)

        # SLAAC: DAD on the link-local address, a router solicitation, the
        # RA's prefix, DAD again.
        deadline = time.time() + 30
        while True:
            out = run(proc, sel, log, "cat /proc/net/if_inet6")
            have = [l.split() for l in out.splitlines() if len(l.split()) == 6]
            glob = [a for a in have if a[0].startswith("fec0") and a[5] == "eth0"
                    and int(a[4], 16) & 0x40 == 0]
            ll = [a for a in have if a[0].startswith("fe80") and a[5] == "eth0"]
            lo = [a for a in have if a[0] == "0" * 31 + "1" and a[5] == "lo"]
            if glob and ll and lo:
                break
            if time.time() > deadline:
                raise AssertionError(f"no SLAAC address within 30 s: {have}")
            time.sleep(1)
        print(f"SLAAC ok: {glob[0][0]}")
        if "lo:" not in run(proc, sel, log, "cat /proc/net/dev"):
            raise AssertionError("/proc/net/dev lacks lo")

        out = run(proc, sel, log, "net6probe lo", timeout=60)
        if "net6probe lo ok" not in out:
            raise AssertionError("net6probe lo failed")
        print("net6probe lo ok")

        out = run(proc, sel, log,
                  f"toybox wget -O /tmp/w6.html http://[fec0::2]:{port}/index.html"
                  " && cat /tmp/w6.html", timeout=40)
        if "MAEROS_HTTP6_OK" not in out:
            raise AssertionError("wget over IPv6 failed")
        print("wget over IPv6 ok")
        out = run(proc, sel, log,
                  f"toybox wget -O /tmp/w4.html http://10.0.2.2:{port}/index.html"
                  " && cat /tmp/w4.html", timeout=40)
        if "MAEROS_HTTP6_OK" not in out:
            raise AssertionError("wget over IPv4 failed")

        run(proc, sel, log,
            f"echo 'nameserver [10.0.2.2]:{dns.port}' > /etc/resolv.conf")
        out = run(proc, sel, log, "net6probe gai dual.maeros.test", timeout=30)
        lines = [l.strip() for l in out.splitlines() if l.startswith("gai: ")]
        if lines != ["gai: inet 10.0.2.2", "gai: inet6 fec0::2"]:
            raise AssertionError(f"getaddrinfo of the dual-stack name: {lines}")
        out = run(proc, sel, log, "net6probe gai localhost", timeout=30)
        lines = [l.strip() for l in out.splitlines() if l.startswith("gai: ")]
        if lines != ["gai: inet6 ::1", "gai: inet 127.0.0.1"]:
            raise AssertionError(f"getaddrinfo of localhost: {lines}")
        print("getaddrinfo ok: AAAA + A")
        out = run(proc, sel, log,
                  f"toybox wget -O /tmp/wd.html http://dual.maeros.test:{port}/index.html"
                  " && cat /tmp/wd.html", timeout=40)
        if "MAEROS_HTTP6_OK" not in out:
            raise AssertionError("wget of the dual-stack name failed")
        print("smoke-net6 ok")
        return 0
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
        dns.close()
        httpd6.shutdown()
        httpd4.shutdown()


if __name__ == "__main__":
    sys.exit(main())
