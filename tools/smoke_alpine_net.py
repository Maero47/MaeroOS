#!/usr/bin/env python3
"""smoke-alpine-net: Alpine's networking tools and OpenSSH's server in the
Alpine chroot (ports/alpine), on an e1000 with QEMU user networking.

Opt-in like smoke-alpine (it needs disk-alpine.img and, on the host,
ssh/ssh-keygen).  Inside `chroot /disk/alpine`:

  - busybox `ip addr`/`ip route`/`ip link` (rtnetlink dumps), `ifconfig`
    and `route -n` (SIOC* ioctls, /proc/net/route) show eth0 with the
    address the kernel's DHCP client got and the default route;
  - `udhcpc -i eth0 -n -q` gets a lease over AF_PACKET (with its BPF
    filter) and its script re-configures eth0 through rtnetlink
    (addr flush/add, route add default ... metric) and writes resolv.conf;
    the kernel's own DHCP client steps aside;
  - `ping` over a raw ICMP socket;
  - IPv6: `ip -6 addr` shows the SLAAC address, lo has 127.0.0.1 and ::1,
    `ip -6 route` the RA's default route, ping to 127.0.0.1, ::1 and the
    host's fec0::2 over raw ICMP/ICMPv6, and python3 TCP over [::1];
  - advisory locks: busybox `flock` contention (EWOULDBLOCK with -n, a
    blocking lock waits for the holder) and apk refusing to run while its
    database lock is held;
  - openssh-server installed with apk from the offline repo on the disk,
    host keys generated, sshd started on port 22; the host logs in through
    QEMU hostfwd with a throwaway ed25519 key: `ssh ... true`, a command
    with output, and an interactive session on a pty (`ssh -tt`).

No [SYSCALL] unimplemented line may appear.  The image is copied first.
"""
import base64
import os
import selectors
import shutil
import socket
import subprocess
import sys
import threading

import smokelib

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
IMG = os.path.abspath(os.environ.get("ALPINE_IMG", os.path.join(ROOT, "disk-alpine.img")))
WORKDIR = os.path.join(ROOT, "build", "smoke-alpine-net")
WORK = os.path.join(WORKDIR, "disk.img")
KEY = os.path.join(WORKDIR, "id_ed25519")

ENV = "/usr/bin/env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin HOME=/root TERM=vt100"
CHROOT = "toybox chroot /disk/alpine " + ENV


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def main():
    for tool in ("ssh", "ssh-keygen"):
        if not shutil.which(tool):
            raise SystemExit(f"smoke_alpine_net: the host needs {tool} (OpenSSH client)")
    if not os.path.exists(IMG):
        raise SystemExit("smoke_alpine_net: disk-alpine.img is missing - run "
                         "`python3 ports/alpine/prepare.py` (or make smoke-alpine-net)")
    os.makedirs(WORKDIR, exist_ok=True)
    shutil.copyfile(IMG, WORK)
    for f in (KEY, KEY + ".pub"):
        if os.path.exists(f):
            os.unlink(f)
    subprocess.run(["ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-C", "smoke",
                    "-f", KEY], check=True)
    pubkey = " ".join(open(KEY + ".pub").read().split()[:2])
    port = free_port()

    accel = ["-accel", "kvm"] if os.access("/dev/kvm", os.R_OK | os.W_OK) else []
    smp = os.environ.get("SMOKE_SMP")
    proc = subprocess.Popen(
        ["qemu-system-i386", "-kernel", "kernel.elf", "-initrd", "initrd.tar",
         "-drive", f"file={WORK},format=raw,index=0,media=disk",
         "-serial", "stdio", "-m", "1024M", "-no-reboot", "-no-shutdown",
         "-netdev", f"user,id=n0,hostfwd=tcp:127.0.0.1:{port}-:22",
         "-device", "e1000,netdev=n0"]
        + (["-smp", smp] if smp else []) + accel + smokelib.QEMU_DISPLAY,
        cwd=ROOT, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, bufsize=0)
    sel = selectors.DefaultSelector()
    sel.register(proc.stdout, selectors.EVENT_READ)
    log = []

    def check(what, body, needles, absent=()):
        for n in needles:
            if n not in body:
                raise AssertionError(f"{what}: missing {n!r}")
        for n in tuple(absent) + ("[SYSCALL] unimplemented",):
            if n in body:
                raise AssertionError(f"{what}: unexpected {n!r}")

    def run(cmd, *needles, absent=(), timeout=120.0):
        at = smokelib.mark(log)
        smokelib.send(proc, cmd + "\n")
        smokelib.wait_for(proc, sel, smokelib.PROMPT, log, timeout, at)
        body = "".join(log)[at:]
        check(cmd, body, needles, absent)
        return body

    def alpine(cmd, *needles, **kw):
        # The marker proves the exit status; spelled AN_""OK so the echoed
        # command line cannot match it.
        return run(f"{CHROOT} /bin/sh -c '{cmd} && echo AN_\"\"OK'",
                   *needles, "AN_OK", **kw)

    def host(args, stdin=None, timeout=90):
        """Run a host command while the guest console keeps draining (a
        full serial pipe would stall the guest under it)."""
        res = {}

        def body():
            try:
                r = subprocess.run(args, input=stdin, capture_output=True,
                                   text=True, timeout=timeout)
                res["out"] = r.stdout + r.stderr
                res["rc"] = r.returncode
            except subprocess.TimeoutExpired:
                res["out"], res["rc"] = "timeout", -1
        t = threading.Thread(target=body)
        t.start()
        while t.is_alive():
            for key, _ in sel.select(0.2):
                chunk = os.read(key.fd, 4096).decode("latin1", "replace")
                if chunk:
                    log.append(chunk)
                    sys.stdout.write(chunk)
                    sys.stdout.flush()
            if proc.poll() is not None:
                raise RuntimeError("QEMU exited")
        print(f"[host] {' '.join(args[:1] + args[-2:])} -> rc={res['rc']}\n{res['out']}",
              flush=True)
        return res["rc"], res["out"]

    ssh = ["ssh", "-o", "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=/dev/null",
           "-o", "BatchMode=yes", "-o", "LogLevel=ERROR", "-o", "ConnectTimeout=30",
           "-i", KEY, "-p", str(port), "root@127.0.0.1"]

    try:
        smokelib.login(proc, sel, log, timeout=90)
        # The kernel's DHCP client configures eth0 at boot.
        alpine("i=0; until ip -4 route | grep -q ^default; do i=$((i+1)); "
               "[ $i -lt 60 ] || exit 1; sleep 1; done")
        alpine("ip addr show dev eth0", "inet 10.0.2.15/24 brd 10.0.2.255",
               "link/ether 52:54:00:12:34:56")
        alpine("ip link", "eth0: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500")
        alpine("ip route", "default via 10.0.2.2 dev eth0",
               "10.0.2.0/24 dev eth0 scope link  src 10.0.2.15")
        alpine("ip route get 1.1.1.1", "via 10.0.2.2 dev eth0")
        alpine("ifconfig eth0", "inet addr:10.0.2.15", "Mask:255.255.255.0",
               "HWaddr 52:54:00:12:34:56")
        alpine("route -n", "10.0.2.2", "UG")
        # IPv6 (QEMU user-net's default, fec0::/64 by SLAAC) and lo, before
        # udhcpc's script flushes eth0's addresses.
        alpine("i=0; until ip -6 addr show dev eth0 | grep -q 'inet6 fec0'; do "
               "i=$((i+1)); [ $i -lt 30 ] || exit 1; sleep 1; done")
        alpine("ip -6 addr show dev eth0", "inet6 fec0::5054:ff:fe12:3456/64",
               "inet6 fe80::5054:ff:fe12:3456/64 scope link")
        alpine("ip addr show dev lo", "inet 127.0.0.1/8 scope host lo",
               "inet6 ::1/128 scope host")
        alpine("ip -6 route", "default via fe80::2 dev eth0")
        alpine("ping -c 1 -W 5 127.0.0.1", "1 packets received")
        alpine("ping -c 1 -W 5 ::1", "1 packets received")
        alpine("ping -c 1 -W 5 fec0::2", "1 packets received")
        # python3 sockets over [::1] and localhost (the script goes in as
        # base64: alpine() quotes with '...').
        py = ("import socket\n"
              "s = socket.socket(socket.AF_INET6)\n"
              "s.bind(('::1', 0)); s.listen(1)\n"
              "c = socket.create_connection(('localhost', s.getsockname()[1]))\n"
              "a = s.accept()[0]; c.sendall(b'v6')\n"
              "print('PY6', a.recv(2), c.getpeername()[0])\n")
        b64 = base64.b64encode(py.encode()).decode()
        alpine(f"echo {b64} | base64 -d > /tmp/py6.py && python3 /tmp/py6.py",
               "PY6 b'v6' ::1")
        # udhcpc over AF_PACKET; its script reconfigures eth0 through
        # rtnetlink and the kernel's DHCP client lets go.
        alpine("udhcpc -i eth0 -n -q -f", "lease of 10.0.2.15 obtained")
        alpine("ip -4 addr show dev eth0 && ip route",
               "inet 10.0.2.15/24", "default via 10.0.2.2 dev eth0  metric 2")
        alpine("cat /etc/resolv.conf", "nameserver 10.0.2.3")
        run("grep 'DHCP client stopped' /proc/kmsg", "configured by hand")
        alpine("ping -c 1 -W 5 10.0.2.2", "1 packets received")
        # flock: -n fails while another process holds the lock, a blocking
        # lock waits for it.
        alpine("flock /tmp/lk sleep 3 & sleep 1; flock -n /tmp/lk true "
               "&& echo NB_\"\"FREE; s=$(date +%s); flock /tmp/lk true; "
               "echo waited=$(( $(date +%s) - s ))", "waited=", absent=("NB_FREE",))
        body = "".join(log)
        w = body[body.rfind("waited=") + 7:].split()[0]
        if not w.isdigit() or int(w) < 1:
            raise AssertionError(f"blocking flock did not wait (waited={w})")
        # apk keeps out while its database lock is held.
        alpine("flock /lib/apk/db/lock sleep 3 & sleep 1; ! apk add tree; wait",
               "Unable to lock database")
        alpine("! apk info -e tree")
        # sshd from the offline repo.
        alpine("apk add openssh-server", "Installing openssh-server",
               absent=("ERROR",), timeout=180)
        alpine("ssh-keygen -A", "generating new host keys")
        alpine("mkdir -p /root/.ssh && chmod 700 /root/.ssh")
        alpine(f"echo {pubkey} > /root/.ssh/authorized_keys && "
               "chmod 600 /root/.ssh/authorized_keys")
        alpine("/usr/sbin/sshd -E /var/log/sshd.log && sleep 1 && "
               "test -s /var/run/sshd.pid")
        rc, out = host(ssh + ["true"])
        if rc != 0:
            raise AssertionError(f"ssh true: rc={rc}: {out}")
        rc, out = host(ssh + ["echo SSH-$((6*7)); cat /etc/alpine-release; id -un"])
        check("ssh command", out, ("SSH-42", "3.2", "root"))
        if rc != 0:
            raise AssertionError(f"ssh command: rc={rc}")
        rc, out = host(ssh[:1] + ["-tt"] + ssh[1:],
                       stdin="echo TTY-$((6*7)); tty; exit\n")
        check("ssh -tt", out, ("TTY-42", "/dev/pts/"))
        alpine("cat /var/log/sshd.log", "Accepted publickey for root")
        print("\n[SMOKE-ALPINE-NET] passed")
        return 0
    except Exception as e:
        print(f"\n[SMOKE-ALPINE-NET] FAILED: {e}")
        return 1
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()


if __name__ == "__main__":
    sys.exit(main())
