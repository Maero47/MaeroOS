#!/usr/bin/env python3
"""smoke-tcpsrv: AF_INET server sockets, reached from the host through QEMU
user-net port forwarding (hostfwd: the host connects to 127.0.0.1:<fwd>,
slirp connects on to the guest's 10.0.2.15:<port>).

    python3 tools/smoke_tcpsrv.py               Intel e1000 (default)
    python3 tools/smoke_tcpsrv.py --nic rtl8139

Checks, in the guest:
  - `toybox nc -l -p 8080` serves a host client, data both ways;
  - srvprobe tcp: non-blocking accept (EAGAIN), SO_RCVTIMEO on accept, poll
    and epoll on a listener, three host clients queued in the backlog at once
    and taken with accept4(SOCK_NONBLOCK|SOCK_CLOEXEC), getpeername/
    getsockname on accepted sockets, SO_REUSEADDR rebind after close (and
    EADDRINUSE without it), a blocking accept, shutdown of a listener;
  - srvprobe udp: blocking recvfrom with SO_RCVTIMEO, MSG_DONTWAIT, and a
    datagram from the host answered to its sender.
"""
import argparse
import os
import selectors
import socket
import subprocess
import sys
import threading
import time

import smokelib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROMPT = "MaeroOS$ "
NC_PORT, TCP_PORT, UDP_PORT = 8080, 8081, 9000


def free_port(kind=socket.SOCK_STREAM):
    s = socket.socket(socket.AF_INET, kind)
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def send(proc, line):
    proc.stdin.write((line + "\n").encode("latin1"))
    proc.stdin.flush()


def run(proc, sel, log, cmd, timeout=20.0):
    before = len("".join(log))
    send(proc, cmd)
    smokelib.wait_for(proc, sel, PROMPT, log, timeout=timeout, start=before)
    return "".join(log)[before:]


def recv_until(conn, needle, timeout=15.0):
    conn.settimeout(timeout)
    data = b""
    while needle not in data:
        chunk = conn.recv(4096)
        if not chunk:
            break
        data += chunk
    return data


def client(port, line, result, key, wait_eof=False):
    """Connect through hostfwd, send `line`, read the guest's ECHO reply.
    With wait_eof the guest closes first (its side then sits in TIME_WAIT,
    which the SO_REUSEADDR rebind check relies on)."""
    try:
        c = socket.create_connection(("127.0.0.1", port), timeout=15)
        c.sendall(line)
        result[key] = recv_until(c, b"\n")
        if wait_eof:
            while c.recv(4096):
                pass
        c.close()
    except OSError as e:
        result[key] = repr(e)


def nc_check(proc, sel, log, fwd):
    """`echo GUEST_SAYS_HI | toybox nc -l -p 8080`: the host's line shows up
    on the guest console, the guest's line reaches the host."""
    result = {}

    def host_side():
        # slirp accepts our connection before the guest listens and closes
        # it if the guest refuses, so retry until the greeting arrives.
        end = time.time() + 20
        while time.time() < end:
            try:
                c = socket.create_connection(("127.0.0.1", fwd), timeout=5)
                c.sendall(b"HOST_SAYS_HELLO\n")
                data = recv_until(c, b"GUEST_SAYS_HI", timeout=5)
                if b"GUEST_SAYS_HI" in data:
                    result["got"] = data
                    c.shutdown(socket.SHUT_WR)
                    c.close()
                    return
                c.close()
            except OSError:
                pass
            time.sleep(0.5)

    t = threading.Thread(target=host_side, daemon=True)
    t.start()
    out = run(proc, sel, log,
              f"toybox echo GUEST_SAYS_HI | toybox nc -l -p {NC_PORT}", timeout=40)
    t.join(timeout=5)
    if b"GUEST_SAYS_HI" not in result.get("got", b""):
        raise AssertionError(f"host never got the guest's line from nc -l: {result}")
    if "HOST_SAYS_HELLO" not in out:
        raise AssertionError("nc -l did not print the host's line")
    print("nc -l ok: data both ways")


def tcp_check(proc, sel, log, fwd):
    before = len("".join(log))
    send(proc, f"srvprobe tcp {TCP_PORT}")
    smokelib.wait_for(proc, sel, "srvprobe: listening", log, timeout=20, start=before)
    res = {}
    threads = [threading.Thread(target=client, args=(fwd, f"client{i}\n".encode(), res, i, True))
               for i in range(3)]
    for t in threads:
        t.start()
    smokelib.wait_for(proc, sel, "srvprobe: rebound", log, timeout=40, start=before)
    for t in threads:
        t.join(timeout=10)
    for i in range(3):
        if res.get(i) != f"ECHO client{i}\n".encode():
            raise AssertionError(f"backlog client {i} got {res.get(i)!r}")
    client(fwd, b"late\n", res, "late")
    smokelib.wait_for(proc, sel, PROMPT, log, timeout=20, start=before)
    out = "".join(log)[before:]
    if res.get("late") != b"ECHO late\n":
        raise AssertionError(f"client of the rebound listener got {res.get('late')!r}")
    if "srvprobe tcp ok" not in out:
        raise AssertionError("srvprobe tcp failed")
    print("srvprobe tcp ok: 3 queued clients + 1 after rebind")


def udp_check(proc, sel, log, fwd):
    before = len("".join(log))
    send(proc, f"srvprobe udp {UDP_PORT}")
    smokelib.wait_for(proc, sel, "srvprobe: udp ready", log, timeout=20, start=before)
    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    u.settimeout(1.0)
    got = None
    # The guest is (or soon will be) asleep in recvfrom; resend in case the
    # first datagram raced slirp's setup.
    for _ in range(10):
        u.sendto(b"PING_FROM_HOST", ("127.0.0.1", fwd))
        try:
            got = u.recvfrom(1024)[0]
            break
        except socket.timeout:
            pass
    u.close()
    smokelib.wait_for(proc, sel, PROMPT, log, timeout=20, start=before)
    out = "".join(log)[before:]
    if got != b"UDPECHO PING_FROM_HOST":
        raise AssertionError(f"udp reply was {got!r}")
    if "srvprobe udp ok" not in out:
        raise AssertionError("srvprobe udp failed")
    print("srvprobe udp ok")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--nic", choices=["rtl8139", "e1000"], default="e1000")
    args = ap.parse_args()

    nc_fwd, tcp_fwd, udp_fwd = free_port(), free_port(), free_port(socket.SOCK_DGRAM)
    netdev = (f"user,id=n0,hostfwd=tcp:127.0.0.1:{nc_fwd}-:{NC_PORT}"
              f",hostfwd=tcp:127.0.0.1:{tcp_fwd}-:{TCP_PORT}"
              f",hostfwd=udp:127.0.0.1:{udp_fwd}-:{UDP_PORT}")
    proc = subprocess.Popen(
        ["qemu-system-i386", *smokelib.QEMU_DISPLAY, "-kernel", "kernel.elf",
         "-initrd", "initrd.tar", "-serial", "stdio", "-m", "128M",
         "-no-reboot", "-no-shutdown",
         "-netdev", netdev, "-device", f"{args.nic},netdev=n0"],
        cwd=ROOT, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, bufsize=0)
    sel = selectors.DefaultSelector()
    sel.register(proc.stdout, selectors.EVENT_READ)
    log = []
    try:
        smokelib.login(proc, sel, log, timeout=25.0)
        deadline = time.time() + 20
        while "ip=10.0.2.15" not in run(proc, sel, log, "ifconfig"):
            if time.time() > deadline:
                raise AssertionError("no DHCP lease within 20 s")
            time.sleep(0.5)
        nc_check(proc, sel, log, nc_fwd)
        tcp_check(proc, sel, log, tcp_fwd)
        udp_check(proc, sel, log, udp_fwd)
        # The client path is unchanged: an outbound fetch still works.
        out = run(proc, sel, log, "sockprobe")
        if "sockprobe udp ok" not in out:
            raise AssertionError("sockprobe (client UDP) failed")
        print(f"smoke-tcpsrv ok ({args.nic})")
        return 0
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()


if __name__ == "__main__":
    sys.exit(main())
