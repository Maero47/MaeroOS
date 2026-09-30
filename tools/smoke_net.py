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

    # The `make run-net` command line, plus a dump of the guest's packets so
    # the shutdown check below can see a RST that slirp never passes on to
    # the host (it turns a guest RST into a plain close of the host socket).
    pcap_dir = tempfile.TemporaryDirectory()
    pcap_path = os.path.join(pcap_dir.name, "net.pcap")
    proc = subprocess.Popen(
        ["qemu-system-i386", "-kernel", "kernel.elf", "-initrd", "initrd.tar",
         "-serial", "stdio", "-m", "128M", "-no-reboot", "-no-shutdown",
         "-netdev", "user,id=n0", "-device", "rtl8139,netdev=n0",
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
        pcap_dir.cleanup()
        webdir.cleanup()


if __name__ == "__main__":
    sys.exit(main())
