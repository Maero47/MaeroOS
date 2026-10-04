#!/usr/bin/env python3
"""smoke-net: the socket ABI over QEMU user networking.

    python3 tools/smoke_net.py               RTL8139, initrd only (smoke-net)
    python3 tools/smoke_net.py --nic e1000   Intel e1000 with the disk attached
                                             (smoke-net-e1000), plus DHCP's
                                             /etc/resolv.conf and name lookups
                                             against a DNS responder run here

The e1000 run boots with disk.img as a snapshot (nothing is written back): the
disk makes /etc writable, so the kernel can write the DHCP DNS server to
/etc/resolv.conf and the test can point the resolver at its own responder.
"""
import argparse
import os
import selectors
import socket
import subprocess
import sys
import time

import dns_responder
import smokelib
import tempfile
import threading
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROMPT = "MaeroOS$ "


def wait_for(proc, sel, needle, log, timeout=20.0, start=0):
    return smokelib.wait_for(proc, sel, needle, log, timeout, start)


def send(proc, line):
    proc.stdin.write((line + "\n").encode("latin1"))
    proc.stdin.flush()


def guest_tcp_flags(pcap_path, port):
    """Count FIN and RST segments the guest sent to the host's `port`, from a
    QEMU filter-dump pcap (Ethernet link type, frames as the guest sent)."""
    import struct
    counts = {"fin": 0, "rst": 0}
    with open(pcap_path, "rb") as f:
        data = f.read()
    off = 24                                    # pcap global header
    while off + 16 <= len(data):
        incl = struct.unpack_from("<I", data, off + 8)[0]
        frame = data[off + 16:off + 16 + incl]
        off += 16 + incl
        if len(frame) < 14 + 20 or frame[12:14] != b"\x08\x00":
            continue                            # not IPv4
        ip = frame[14:]
        ihl = (ip[0] & 0x0F) * 4
        if ip[9] != 6 or len(ip) < ihl + 14:
            continue                            # not TCP
        tcp = ip[ihl:]
        if ip[12:16] != bytes([10, 0, 2, 15]):
            continue                            # not from the guest
        if struct.unpack_from(">H", tcp, 2)[0] != port:
            continue
        if tcp[13] & 0x01:
            counts["fin"] += 1
        if tcp[13] & 0x04:
            counts["rst"] += 1
    return counts


def sink_server():
    """One-connection TCP sink: greet, then count bytes until EOF or reset.

    Returns (port, result dict, thread); result gets 'bytes' and 'end'
    ('eof' for an orderly FIN, otherwise the exception name)."""
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", 0))
    srv.listen(1)
    srv.settimeout(30)
    result = {}

    def run():
        try:
            conn, _ = srv.accept()
        except OSError as e:
            result["end"] = type(e).__name__
            return
        with conn:
            conn.settimeout(30)
            conn.sendall(b"HELLO FROM HOST\n")
            total = 0
            try:
                while True:
                    data = conn.recv(65536)
                    if not data:
                        result["end"] = "eof"
                        break
                    total += len(data)
            except OSError as e:
                result["end"] = type(e).__name__
            result["bytes"] = total
        srv.close()

    t = threading.Thread(target=run, daemon=True)
    t.start()
    return srv.getsockname()[1], result, t


def serve_forever(handler):
    """Accept connections on a fresh port until the process ends, handing
    each to handler(conn) on its own thread.  Returns the port."""
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", 0))
    srv.listen(8)

    def run():
        while True:
            try:
                conn, _ = srv.accept()
            except OSError:
                return
            threading.Thread(target=handler, args=(conn,), daemon=True).start()

    threading.Thread(target=run, daemon=True).start()
    return srv.getsockname()[1]


def idle_conn(conn):
    """Keep the connection open and silent (the guest's recv must EAGAIN)."""
    conn.settimeout(30)
    try:
        while conn.recv(4096):
            pass
    except OSError:
        pass
    conn.close()


def reset_conn(conn):
    """On the guest's first bytes, close with a reset (SO_LINGER 0): slirp
    passes it on to the guest as a RST, after which the guest's send must
    fail with EPIPE.  Waiting for data first keeps the reset from racing
    slirp's handshake with the guest (which would make it ECONNREFUSED)."""
    import struct
    conn.settimeout(30)
    try:
        conn.recv(16)
    except OSError:
        pass
    conn.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
    conn.close()


def udp_echo_server():
    """Echo every UDP datagram back to its sender.  Returns the port."""
    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    u.bind(("0.0.0.0", 0))

    def run():
        while True:
            try:
                data, addr = u.recvfrom(65536)
                u.sendto(data, addr)
            except OSError:
                return

    threading.Thread(target=run, daemon=True).start()
    return u.getsockname()[1]


def stalled_listener():
    """A listener whose accept queue is full, so a new connect() to it never
    completes its handshake (the host drops the SYNs).  Returns (port,
    sockets to keep alive)."""
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.bind(("0.0.0.0", 0))
    srv.listen(0)
    port = srv.getsockname()[1]
    keep = [srv]
    for _ in range(4):
        c = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        c.setblocking(False)
        try:
            c.connect(("127.0.0.1", port))
        except OSError:
            pass
        keep.append(c)
    time.sleep(0.2)
    return port, keep


def drain_later(srv, delay):
    """After `delay` seconds, accept (and hold) everything queued on srv."""
    time.sleep(delay)
    srv.settimeout(0.2)
    held = []
    end = time.time() + 8
    while time.time() < end:
        try:
            held.append(srv.accept()[0])
        except OSError:
            pass
    for c in held:
        c.close()


class Http11Handler(SimpleHTTPRequestHandler):
    """HTTP/1.1 replies: toybox wget only accepts an "HTTP/1.1" status line."""
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass


def line_echo(conn):
    """Read one line and answer "NC_OK <line>", then close (toybox nc)."""
    conn.settimeout(20)
    data = b""
    try:
        while b"\n" not in data:
            chunk = conn.recv(4096)
            if not chunk:
                break
            data += chunk
        conn.sendall(b"NC_OK " + data.split(b"\n")[0] + b"\n")
    except OSError:
        pass
    conn.close()


def run(proc, sel, log, cmd, timeout=20.0):
    """Send one shell command and return its output up to the next prompt."""
    before = len("".join(log))
    send(proc, cmd)
    wait_for(proc, sel, PROMPT, log, timeout=timeout, start=before)
    return "".join(log)[before:]


def dns_checks(proc, sel, log, webdir):
    """DHCP's /etc/resolv.conf, then the libc resolver (getaddrinfo,
    getnameinfo, gethostbyname via resolve_a) and the toybox network applets
    against a local DNS responder, with no internet dependency."""
    dns = dns_responder.DnsResponder({
        "web.maeros.test": {"A": ["10.0.2.2"], "AAAA": ["fd00::2"]},
        "alias.maeros.test": {"CNAME": "web.maeros.test"},
        "rev.maeros.test": {"A": ["192.0.2.7"]},
    })
    http11 = ThreadingHTTPServer(("0.0.0.0", 0),
                                 partial(Http11Handler, directory=webdir))
    threading.Thread(target=http11.serve_forever, daemon=True).start()
    nc_port = serve_forever(line_echo)
    try:
        out = run(proc, sel, log, "cat /etc/resolv.conf")
        if "from the DHCP lease" not in out or "nameserver 10.0.2.3" not in out:
            raise AssertionError("/etc/resolv.conf was not written from the DHCP lease")
        # OpenBSD's "[address]:port" form points the resolver at our port.
        run(proc, sel, log,
            f"echo 'nameserver [10.0.2.2]:{dns.port}' > /etc/resolv.conf")
        checks = [
            ("getent hosts web.maeros.test", "10.0.2.2        web.maeros.test"),
            # A and AAAA together, IPv4 first.
            ("getent ahosts web.maeros.test", "fd00::2"),
            ("getent ahostsv6 web.maeros.test", "fd00::2"),
            # A CNAME: the canonical name comes back with the alias.
            ("getent hosts alias.maeros.test",
             "10.0.2.2        web.maeros.test alias.maeros.test"),
            # getnameinfo: PTR from the responder, /etc/hosts before DNS.
            ("getent hosts 192.0.2.7", "192.0.2.7       rev.maeros.test"),
            ("getent hosts 10.0.2.2", "10.0.2.2        qemu-host"),
            # ::1 before 127.0.0.1 (RFC 6724 precedence, as musl orders it).
            ("getent hosts localhost", "::1             localhost"),
            ("getent hosts nosuch.maeros.test; echo rc=$?",
             "Name does not resolve"),
        ]
        for cmd, want in checks:
            out = run(proc, sel, log, cmd)
            if want not in out:
                raise AssertionError(f"{cmd!r}: expected {want!r}")
        out = run(proc, sel, log, f"httpget web.maeros.test {http11.server_address[1]} /index.html")
        if "MAEROS_HTTP_OK" not in out:
            raise AssertionError("httpget by name failed")
        out = run(proc, sel, log,
                  f"toybox wget -O /tmp/w.html http://web.maeros.test:{http11.server_address[1]}/index.html"
                  " && toybox cat /tmp/w.html")
        if "MAEROS_HTTP_OK" not in out:
            raise AssertionError("toybox wget by name failed")
        out = run(proc, sel, log,
                  f"toybox echo ping | toybox nc -W 10 alias.maeros.test {nc_port}")
        if "NC_OK ping" not in out:
            raise AssertionError("toybox nc by name failed")
        seen = set(dns.queries)
        for q in [("web.maeros.test", "A"), ("web.maeros.test", "AAAA"),
                  ("7.2.0.192.in-addr.arpa", "PTR")]:
            if q not in seen:
                raise AssertionError(f"DNS responder never saw {q}: {dns.queries}")
        # IDs come from getrandom(): no counter, and the AAAA query of a pair
        # does not reuse the A query's ID + 1 (off-path spoofing).
        steps = sum(1 for a, b in zip(dns.ids, dns.ids[1:]) if (b - a) % 65536 in (1, 65535))
        if len(dns.ids) >= 6 and steps > 1:
            raise AssertionError(f"DNS query IDs look sequential: {dns.ids}")
        print(f"dns ok: {len(dns.queries)} queries answered by the test responder")
    finally:
        dns.close()
        http11.shutdown()
        http11.server_close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--nic", choices=["rtl8139", "e1000"], default="rtl8139")
    args = ap.parse_args()
    nic = args.nic
    pci_id = {"rtl8139": "10ec:8139", "e1000": "8086:100e"}[nic]

    webdir = tempfile.TemporaryDirectory()
    index_path = os.path.join(webdir.name, "index.html")
    with open(index_path, "w", encoding="ascii") as f:
        f.write("MAEROS_HTTP_OK\n")

    handler = partial(SimpleHTTPRequestHandler, directory=webdir.name)
    httpd = ThreadingHTTPServer(("0.0.0.0", 0), handler)
    http_port = httpd.server_address[1]
    http_thread = threading.Thread(target=httpd.serve_forever, daemon=True)
    http_thread.start()

    # The `make run-net` command line, plus a dump of the guest's packets so
    # the shutdown check below can see a RST that slirp never passes on to
    # the host (it turns a guest RST into a plain close of the host socket).
    pcap_dir = tempfile.TemporaryDirectory()
    pcap_path = os.path.join(pcap_dir.name, "net.pcap")
    disk = []
    if nic == "e1000":
        disk = ["-drive", "file=disk.img,format=raw,index=0,media=disk,snapshot=on"]
    proc = subprocess.Popen(
        ["qemu-system-i386", *smokelib.QEMU_DISPLAY, "-kernel", "kernel.elf", "-initrd", "initrd.tar",
         *disk,
         "-serial", "stdio", "-m", "128M", "-no-reboot", "-no-shutdown",
         "-netdev", "user,id=n0", "-device", f"{nic},netdev=n0",
         "-object", f"filter-dump,id=d0,netdev=n0,file={pcap_path}"],
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
        smokelib.login(proc, sel, log, timeout=25.0)
        # The DHCP lease: with the e1000 QEMU drops the first OFFER while the
        # receiver settles, so the retransmit a second later binds.  The
        # kernel logs it to dmesg only (not the console, where it would land
        # in the middle of a command's output).
        deadline = time.time() + 20
        while "ip=10.0.2.15" not in run(proc, sel, log, "ifconfig"):
            if time.time() > deadline:
                raise AssertionError("no DHCP lease within 20 s")
            time.sleep(0.5)
        want = ["[LWIP] eth0 bound ip=10.0.2.15"]
        if nic == "e1000":
            # "written", or "unchanged" when an earlier boot of this disk
            # image (smoke-disk has QEMU's default e1000) already wrote it.
            want.append("[NET] /etc/resolv.conf from DHCP: ")
        deadline = time.time() + 10
        while True:
            out = run(proc, sel, log, "dmesg")
            if all(w in out for w in want):
                break
            if time.time() > deadline:
                raise AssertionError(f"dmesg lacks {want}")
            time.sleep(0.5)
        before = len("".join(log))
        send(proc, "lspci")
        wait_for(proc, sel, PROMPT, log, timeout=10.0, start=before)
        recent = "".join(log)[before:]
        if pci_id not in recent:
            raise AssertionError(f"{nic} PCI device {pci_id} was not listed")
        if "class=02:00" not in recent:
            raise AssertionError("network class code was not listed")
        before = len("".join(log))
        send(proc, "ifconfig")
        wait_for(proc, sel, PROMPT, log, timeout=10.0, start=before)
        recent = "".join(log)[before:]
        if f"eth0: {nic} up" not in recent:
            raise AssertionError(f"{nic} interface did not initialize")
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
        # send() after shutdown(SHUT_WR) must fail with EPIPE promptly,
        # not sleep forever waiting for send-buffer space.
        before = len("".join(log))
        send(proc, f"sockprobe tcpshut {http_port}")
        wait_for(proc, sel, PROMPT, log, timeout=10.0, start=before)
        recent = "".join(log)[before:]
        if "sockprobe tcpshut ok" not in recent:
            raise AssertionError("send after shutdown(SHUT_WR) did not fail with EPIPE")
        # shutdown(SHUT_RD) with a greeting left unread, send, then
        # shutdown(SHUT_WR): the peer must get every byte and a FIN, not a
        # reset that drops the queued data.
        sink_port, sink, sink_thread = sink_server()
        nbytes = 65536
        before = len("".join(log))
        send(proc, f"sockprobe shutrd {sink_port} {nbytes}")
        wait_for(proc, sel, PROMPT, log, timeout=30.0, start=before)
        recent = "".join(log)[before:]
        sink_thread.join(timeout=15)
        if f"sockprobe shutrd sent {nbytes}" not in recent:
            raise AssertionError("shutrd probe failed")
        if sink.get("end") != "eof" or sink.get("bytes") != nbytes:
            raise AssertionError(f"two-step shutdown was not orderly: {sink}")
        flags = guest_tcp_flags(pcap_path, sink_port)
        print(f"shutrd sink: {sink} guest flags: FIN={flags['fin']} RST={flags['rst']}")
        if flags["rst"] or not flags["fin"]:
            raise AssertionError(
                f"two-step shutdown sent FIN={flags['fin']} RST={flags['rst']} "
                "to the peer; expected a FIN and no RST")
        # 64 half-closed fetches fill the pcb pool with TIME_WAIT pcbs, the
        # oldest 24 with their fds still open; the next connection recycles
        # one of those.  Closing the old fds while it is live must not tear
        # it down.
        before = len("".join(log))
        send(proc, f"sockprobe tcptw {http_port} 64")
        wait_for(proc, sel, PROMPT, log, timeout=30.0, start=before)
        recent = "".join(log)[before:]
        if "sockprobe tcptw ok" not in recent:
            raise AssertionError("closing sockets whose pcbs were in TIME_WAIT broke TCP")
        # O_NONBLOCK/MSG_DONTWAIT, non-blocking connect, EPIPE + SIGPIPE
        # after a peer reset, MSG_NOSIGNAL (userspace/abi2probe).
        idle_port = serve_forever(idle_conn)
        reset_port = serve_forever(reset_conn)
        before = len("".join(log))
        send(proc, f"abi2probe net {idle_port} {reset_port}")
        wait_for(proc, sel, PROMPT, log, timeout=30.0, start=before)
        recent = "".join(log)[before:]
        if "abi2probe net ok" not in recent:
            raise AssertionError("abi2probe net: socket flag/EPIPE cases failed")
        # Non-blocking client calls: EINPROGRESS connect, SO_ERROR, the
        # socket names, EAGAIN from an idle recv, and a refused connect.
        closed = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        closed.bind(("0.0.0.0", 0))
        closed_port = closed.getsockname()[1]
        closed.close()
        before = len("".join(log))
        send(proc, f"sockprobe nb {http_port} {closed_port}")
        wait_for(proc, sel, PROMPT, log, timeout=30.0, start=before)
        recent = "".join(log)[before:]
        if "sockprobe nb ok" not in recent:
            raise AssertionError("non-blocking TCP client calls failed")
        # 48 TCP connections and 48 UDP sockets open at once (the socket
        # table was 32 slots with the RX rings in .bss), each then used.
        before = len("".join(log))
        send(proc, f"sockprobe many {http_port} 48 48")
        wait_for(proc, sel, PROMPT, log, timeout=60.0, start=before)
        recent = "".join(log)[before:]
        if "sockprobe many ok 96" not in recent:
            raise AssertionError("holding 96 sockets open at once failed")
        # sendmsg/recvmsg on TCP and UDP (iovec gather/scatter, msg_name).
        udp_port = udp_echo_server()
        before = len("".join(log))
        send(proc, f"sockprobe msg {http_port} {udp_port}")
        wait_for(proc, sel, PROMPT, log, timeout=20.0, start=before)
        recent = "".join(log)[before:]
        if "sockprobe msg ok" not in recent:
            raise AssertionError("sendmsg/recvmsg on AF_INET sockets failed")
        # A blocking connect() that a signal interrupts returns EINTR at
        # once instead of waiting out its 3 s handshake timeout.
        stall_port, stall_keep = stalled_listener()
        before = len("".join(log))
        send(proc, f"sockprobe conn {stall_port}")
        wait_for(proc, sel, PROMPT, log, timeout=20.0, start=before)
        recent = "".join(log)[before:]
        for s_ in stall_keep:
            s_.close()
        if "sockprobe conn ok" not in recent:
            raise AssertionError("a signal did not interrupt a blocking connect()")
        # With SA_RESTART the restarted connect() waits for the handshake
        # still under way (never EALREADY); the host frees its accept queue
        # after 2 s so that handshake completes.
        stall_port, stall_keep = stalled_listener()
        threading.Thread(target=drain_later, args=(stall_keep[0], 2.0),
                         daemon=True).start()
        before = len("".join(log))
        send(proc, f"sockprobe conn {stall_port} restart")
        wait_for(proc, sel, PROMPT, log, timeout=20.0, start=before)
        recent = "".join(log)[before:]
        time.sleep(0.5)
        for s_ in stall_keep:
            s_.close()
        if "sockprobe conn restart ok" not in recent:
            raise AssertionError("a connect() restarted after SA_RESTART failed")
        before = len("".join(log))
        send(proc, f"httpget 10.0.2.2 {http_port} /index.html")
        wait_for(proc, sel, PROMPT, log, timeout=15.0, start=before)
        recent = "".join(log)[before:]
        if "MAEROS_HTTP_OK" not in recent:
            raise AssertionError("HTTP fetch did not return host response")
        if nic == "e1000":
            dns_checks(proc, sel, log, webdir.name)
        print(f"smoke-net ok ({nic})")
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
        pcap_dir.cleanup()
        webdir.cleanup()


if __name__ == "__main__":
    sys.exit(main())
