#!/usr/bin/env python3
"""smoke-net-virtio: the virtio-net driver (drivers/virtio/) on QEMU's
virtio-net-pci with user networking, IPv4 and IPv6 (-netdev
user,ipv4=on,ipv6=on), disk.img as a snapshot for a writable /etc.

Two boots, one per interrupt mode:

  MSI-X (the device default):
    - the driver attached with MSI-X, all-multicast set over the control
      queue: /proc/netif says so, and QEMU's query-rx-filter (QMP) shows
      promiscuous off and multicast "all";
    - eth0 is ifindex 2, after lo (busybox ip link, i.e. netlink);
    - DHCP lease, /etc/resolv.conf from it, DNS lookups and wget/httpget/nc
      by name against a responder here (smoke_net.dns_checks);
    - TCP and UDP servers in the guest reached through hostfwd
      (smoke_tcpsrv's nc -l, srvprobe tcp, srvprobe udp checks);
    - IPv6: SLAAC (fec0::/64 from slirp's RA, which needs multicast), wget
      over IPv6 from a host server;
    - ping 127.0.0.1 and ::1;
    - link state: QMP set_link off/on reaches the guest as a config-change
      interrupt ("[VNET] link down/up", /proc/netif link=);
    - throughput: 50 MB each way over TCP (sockprobe bulk rx/tx), checked
      for the byte count and reported in MB/s.
  INTx (-device virtio-net-pci,vectors=0: no MSI-X capability):
    - /proc/netif irq=intx, DHCP lease, a wget, ping, 50 MB each way.

    python3 tools/smoke_net_virtio.py                 both boots
    python3 tools/smoke_net_virtio.py --bench-only e1000
        only the throughput runs, on another NIC model (for comparison)
"""
import argparse
import json
import os
import selectors
import socket
import subprocess
import sys
import tempfile
import threading
import time
from functools import partial
from http.server import ThreadingHTTPServer

import smokelib
import smoke_net
import smoke_tcpsrv

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROMPT = "MaeroOS$ "
BULK = 50 * 1024 * 1024


def run(proc, sel, log, cmd, timeout=20.0):
    before = len("".join(log))
    smokelib.send(proc, cmd + "\n")
    smokelib.wait_for(proc, sel, PROMPT, log, timeout=timeout, start=before)
    return "".join(log)[before:]


class Qmp:
    """Just enough QMP: connect, negotiate, run a command, return 'return'."""

    def __init__(self, path):
        deadline = time.time() + 10
        while True:
            try:
                self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self.s.connect(path)
                break
            except OSError:
                if time.time() > deadline:
                    raise
                time.sleep(0.1)
        self.f = self.s.makefile("rwb", buffering=0)
        self.f.readline()                       # greeting
        self.cmd("qmp_capabilities")

    def cmd(self, command, **args):
        msg = {"execute": command}
        if args:
            msg["arguments"] = args
        self.f.write(json.dumps(msg).encode() + b"\n")
        while True:
            reply = json.loads(self.f.readline())
            if "return" in reply:
                return reply["return"]
            if "error" in reply:
                raise AssertionError(f"QMP {command}: {reply['error']}")
            # an asynchronous event: keep reading

    def close(self):
        self.s.close()


def bulk_servers():
    """A TCP source (sends BULK bytes, then closes) and a sink (counts bytes
    to EOF), on fresh ports.  Returns (src_port, sink_port, sink_results)."""
    results = []

    def listener():
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind(("0.0.0.0", 0))
        s.listen(4)
        return s

    src, sink = listener(), listener()
    payload = bytes(range(256)) * (BULK // 256)

    def run_src():
        while True:
            try:
                c, _ = src.accept()
            except OSError:
                return
            try:
                c.sendall(payload)
            except OSError:
                pass
            c.close()

    def run_sink():
        while True:
            try:
                c, _ = sink.accept()
            except OSError:
                return
            t0, n = time.time(), 0
            try:
                while True:
                    d = c.recv(1 << 16)
                    if not d:
                        break
                    n += len(d)
            except OSError:
                pass
            results.append((n, time.time() - t0))
            c.close()

    threading.Thread(target=run_src, daemon=True).start()
    threading.Thread(target=run_sink, daemon=True).start()
    return src.getsockname()[1], sink.getsockname()[1], results


def throughput(proc, sel, log, label):
    """50 MB from the host and 50 MB to it; returns (rx MB/s, tx MB/s)."""
    src, sink, sink_results = bulk_servers()
    out = run(proc, sel, log, f"sockprobe bulk rx {src} {BULK}", timeout=300)
    rx = [l for l in out.splitlines() if l.startswith("sockprobe bulk rx ") and " bytes " in l]
    if not rx or f"rx {BULK} bytes" not in rx[0]:
        raise AssertionError(f"{label}: bulk receive failed: {rx}")
    rx_ms = int(rx[0].split()[5])
    out = run(proc, sel, log, f"sockprobe bulk tx {sink} {BULK}", timeout=300)
    tx = [l for l in out.splitlines() if l.startswith("sockprobe bulk tx ") and " bytes " in l]
    if not tx or f"tx {BULK} bytes" not in tx[0]:
        raise AssertionError(f"{label}: bulk send failed: {tx}")
    deadline = time.time() + 10
    while not sink_results and time.time() < deadline:
        time.sleep(0.1)
    if not sink_results or sink_results[0][0] != BULK:
        raise AssertionError(f"{label}: host sink got {sink_results}, expected {BULK} bytes")
    tx_ms = int(tx[0].split()[5])
    mb = BULK / 1e6
    rx_rate, tx_rate = mb / (rx_ms / 1000), mb / (tx_ms / 1000)
    print(f"THROUGHPUT {label}: rx {mb:.0f} MB in {rx_ms} ms = {rx_rate:.1f} MB/s, "
          f"tx {mb:.0f} MB in {tx_ms} ms = {tx_rate:.1f} MB/s "
          f"(host sink: {sink_results[0][1]:.2f} s)")
    return rx_rate, tx_rate


def boot(device, netdev_extra, qmp_path=None):
    args = ["qemu-system-i386", *smokelib.QEMU_DISPLAY, "-kernel", "kernel.elf",
            "-initrd", "initrd.tar",
            "-drive", "file=disk.img,format=raw,index=0,media=disk,snapshot=on",
            "-serial", "stdio", "-m", "128M", "-no-reboot", "-no-shutdown",
            "-netdev", "user,id=n0,ipv4=on,ipv6=on" + netdev_extra,
            "-device", device]
    if qmp_path:
        args += ["-qmp", f"unix:{qmp_path},server=on,wait=off"]
    proc = subprocess.Popen(args, cwd=ROOT, stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=0)
    sel = selectors.DefaultSelector()
    sel.register(proc.stdout, selectors.EVENT_READ)
    log = []
    smokelib.login(proc, sel, log, timeout=60.0)
    deadline = time.time() + 30
    while "ip=10.0.2.15" not in run(proc, sel, log, "ifconfig"):
        if time.time() > deadline:
            raise AssertionError("no DHCP lease within 30 s")
        time.sleep(0.5)
    return proc, sel, log


def stop(proc):
    if proc and proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()


def netif_line(proc, sel, log):
    out = run(proc, sel, log, "cat /proc/netif")
    lines = [l for l in out.splitlines() if l.startswith("eth0: ")]
    if not lines:
        raise AssertionError("/proc/netif has no eth0")
    return lines[0]


def wait_dmesg(proc, sel, log, needle, timeout=10):
    deadline = time.time() + timeout
    while needle not in run(proc, sel, log, "dmesg | grep VNET"):
        if time.time() > deadline:
            raise AssertionError(f"dmesg lacks {needle!r}")
        time.sleep(0.5)


def slaac(proc, sel, log):
    deadline = time.time() + 30
    while True:
        out = run(proc, sel, log, "cat /proc/net/if_inet6")
        have = [l.split() for l in out.splitlines() if len(l.split()) == 6]
        glob = [a for a in have if a[0].startswith("fec0") and a[5] == "eth0"
                and int(a[4], 16) & 0x40 == 0]
        if glob:
            return glob[0][0]
        if time.time() > deadline:
            raise AssertionError(f"no SLAAC address within 30 s: {have}")
        time.sleep(1)


def pings(proc, sel, log):
    for addr in ("127.0.0.1", "::1"):
        out = run(proc, sel, log, f"ping -c 2 {addr}", timeout=30)
        if "2 packets received" not in out:
            raise AssertionError(f"ping {addr} failed")
    print("ping 127.0.0.1 and ::1 ok")


def msix_boot(webdir, v6port):
    qmp_dir = tempfile.TemporaryDirectory(dir=os.path.join(ROOT, "build"))
    qmp_path = os.path.join(qmp_dir.name, "qmp.sock")
    nc_fwd = smoke_tcpsrv.free_port()
    tcp_fwd = smoke_tcpsrv.free_port()
    udp_fwd = smoke_tcpsrv.free_port(socket.SOCK_DGRAM)
    fwd = (f",hostfwd=tcp:127.0.0.1:{nc_fwd}-:{smoke_tcpsrv.NC_PORT}"
           f",hostfwd=tcp:127.0.0.1:{tcp_fwd}-:{smoke_tcpsrv.TCP_PORT}"
           f",hostfwd=udp:127.0.0.1:{udp_fwd}-:{smoke_tcpsrv.UDP_PORT}")
    proc = qmp = None
    try:
        proc, sel, log = boot("virtio-net-pci,netdev=n0,id=vnet0", fwd, qmp_path)
        qmp = Qmp(qmp_path)
        wait_dmesg(proc, sel, log, "irq=msix")
        line = netif_line(proc, sel, log)
        print(line)
        for want in ("eth0: virtio-net up", "irq=msix/", "rxmode=allmulti", "link=up",
                     "ip=10.0.2.15", "gw=10.0.2.2"):
            if want not in line:
                raise AssertionError(f"/proc/netif lacks {want!r}")
        rxf = [f for f in qmp.cmd("query-rx-filter") if f["name"] == "vnet0"]
        if not rxf:
            raise AssertionError("query-rx-filter has no vnet0")
        rxf = rxf[0]
        print(f"rx-filter: promiscuous={rxf['promiscuous']} multicast={rxf['multicast']} "
              f"unicast={rxf['unicast']} broadcast-allowed={rxf['broadcast-allowed']}")
        # (broadcast-allowed is not checked: QEMU's virtio-net fills it from
        # its "no broadcast" flag as is, so it reads False when broadcasts
        # are allowed.  slirp's broadcast ARP requests for the hostfwd
        # connections below show broadcasts do arrive.)
        if rxf["promiscuous"] or rxf["multicast"] != "all":
            raise AssertionError("device RX filter is not allmulti/no-promisc: "
                                 f"promiscuous={rxf['promiscuous']} multicast={rxf['multicast']}")
        if "1af4:1000" not in run(proc, sel, log, "lspci"):
            raise AssertionError("lspci does not list the virtio-net device")
        out = run(proc, sel, log, "busybox ip link")
        if "1: lo:" not in out or "2: eth0:" not in out:
            raise AssertionError("ifindex order is not lo=1, eth0=2")
        print("ifindex ok: lo=1 eth0=2")

        smoke_net.dns_checks(proc, sel, log, webdir)
        out = run(proc, sel, log,
                  f"toybox wget -O /tmp/big.bin http://10.0.2.2:{v6port}/big.bin"
                  " && toybox wc -c /tmp/big.bin && toybox md5sum /tmp/big.bin", timeout=60)
        if f"{BIG_LEN} /tmp/big.bin" not in out or BIG_MD5 not in out:
            raise AssertionError("wget of a 1 MiB file: wrong size or checksum")
        print("wget 1 MiB ok (size and md5)")

        smoke_tcpsrv.nc_check(proc, sel, log, nc_fwd)
        smoke_tcpsrv.tcp_check(proc, sel, log, tcp_fwd)
        smoke_tcpsrv.udp_check(proc, sel, log, udp_fwd)

        print(f"SLAAC ok: {slaac(proc, sel, log)}")
        out = run(proc, sel, log,
                  f"toybox wget -O /tmp/w6.html http://[fec0::2]:{v6port}/index.html"
                  " && cat /tmp/w6.html", timeout=40)
        if "MAEROS_HTTP_OK" not in out:
            raise AssertionError("wget over IPv6 failed")
        print("wget over IPv6 ok")
        pings(proc, sel, log)

        # Link state: a config-change interrupt each way.
        qmp.cmd("set_link", name="vnet0", up=False)
        wait_dmesg(proc, sel, log, "[VNET] link down")
        if "link=down" not in netif_line(proc, sel, log):
            raise AssertionError("/proc/netif still says link=up")
        qmp.cmd("set_link", name="vnet0", up=True)
        wait_dmesg(proc, sel, log, "[VNET] link up")
        if "link=up" not in netif_line(proc, sel, log):
            raise AssertionError("/proc/netif did not see the link come back")
        print("link down/up ok (config-change interrupts)")

        rates = throughput(proc, sel, log, "virtio-net msix")
        line = netif_line(proc, sel, log)
        print(line)
        if "rxerr=0" not in line or "drop=0" not in line:
            raise AssertionError("receive errors or drops after the bulk runs")
        return rates
    finally:
        if qmp:
            qmp.close()
        stop(proc)
        qmp_dir.cleanup()


def intx_boot(v6port):
    proc = None
    try:
        proc, sel, log = boot("virtio-net-pci,netdev=n0,vectors=0", "")
        line = netif_line(proc, sel, log)
        print(line)
        if "irq=intx/" not in line or "rxmode=allmulti" not in line:
            raise AssertionError("not on INTx (vectors=0), or no allmulti")
        out = run(proc, sel, log,
                  f"toybox wget -O /tmp/big.bin http://10.0.2.2:{v6port}/big.bin"
                  " && toybox md5sum /tmp/big.bin", timeout=60)
        if BIG_MD5 not in out:
            raise AssertionError("INTx: wget of a 1 MiB file failed")
        print(f"INTx SLAAC ok: {slaac(proc, sel, log)}")
        pings(proc, sel, log)
        rates = throughput(proc, sel, log, "virtio-net intx")
        line = netif_line(proc, sel, log)
        if "irqs=0 " in line:
            raise AssertionError("no INTx interrupts were taken")
        print(line)
        return rates
    finally:
        stop(proc)


def bench_only(model):
    proc = None
    try:
        proc, sel, log = boot(f"{model},netdev=n0", "")
        return throughput(proc, sel, log, model)
    finally:
        stop(proc)


BIG_LEN = 1 << 20
BIG_MD5 = ""


def main():
    global BIG_MD5
    ap = argparse.ArgumentParser()
    ap.add_argument("--bench-only", metavar="MODEL",
                    help="only the 50 MB runs, on -device MODEL (e.g. e1000)")
    args = ap.parse_args()
    if args.bench_only:
        bench_only(args.bench_only)
        return 0

    import hashlib
    webdir = tempfile.TemporaryDirectory()
    with open(os.path.join(webdir.name, "index.html"), "w", encoding="ascii") as f:
        f.write("MAEROS_HTTP_OK\n")
    big = bytes((i * 7 + (i >> 8)) & 0xFF for i in range(BIG_LEN))
    with open(os.path.join(webdir.name, "big.bin"), "wb") as f:
        f.write(big)
    BIG_MD5 = hashlib.md5(big).hexdigest()
    # slirp hands a guest's [fec0::2]:port to the host's [::1]:port and
    # 10.0.2.2:port to 127.0.0.1:port: one port, both families.
    handler = partial(smoke_net.Http11Handler, directory=webdir.name)
    httpd6 = smoke_net_v6server(handler)
    port = httpd6.server_address[1]
    httpd4 = ThreadingHTTPServer(("127.0.0.1", port), handler)
    for h in (httpd6, httpd4):
        threading.Thread(target=h.serve_forever, daemon=True).start()
    try:
        msix = msix_boot(webdir.name, port)
        intx = intx_boot(port)
    finally:
        httpd6.shutdown()
        httpd4.shutdown()
        webdir.cleanup()
    print(f"smoke-net-virtio ok: MB/s rx/tx msix {msix[0]:.1f}/{msix[1]:.1f}, "
          f"intx {intx[0]:.1f}/{intx[1]:.1f}")
    return 0


def smoke_net_v6server(handler):
    class V6Server(ThreadingHTTPServer):
        address_family = socket.AF_INET6
    return V6Server(("::1", 0), handler)


if __name__ == "__main__":
    sys.exit(main())
