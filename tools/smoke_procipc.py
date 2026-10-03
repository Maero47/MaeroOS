#!/usr/bin/env python3
"""smoke-procipc: /proc/stat and /proc/<pid>, System V IPC, utmp and inotify.

Boots the initrd with disk.img attached (snapshot=on, nothing is written
back), logs in as root and checks:

  * the process tools read /proc the way they do on Linux: busybox ps, top,
    free, uptime; toybox ps, top, free, uptime, vmstat, pgrep, killall;
  * utmp/wtmp: toybox who, w and last show the console login;
  * System V shm/sem/msg between processes, with permissions, IPC_RMID and
    SEM_UNDO on exit (abiprobe p45), and ipcs lists a live segment;
  * inotify sees each directory operation on tmpfs and on ext2 (p46), and
    on fresh vfat, exFAT and ext4 volumes (build/smoke-procipc/, made with the
    host's mkfs tools when they are there);
  * /proc/<pid> files of another user's process are refused where Linux
    refuses them, and readable where it does not (p47, and by hand as
    "user").

--alpine (opt-in, like smoke-alpine) also boots disk-alpine.img and runs
Alpine's procps (ps, free, uptime, vmstat, pgrep, top) and htop in the
chroot; they are fetched with apk from the pinned mirror when missing.

SMOKE_SMP=N runs the guest with N CPUs (default 2).
"""
import argparse
import os
import re
import selectors
import subprocess
import sys
import threading
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import smokelib

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
PROMPT = smokelib.PROMPT


class Guest:
    def __init__(self, disk, mem="512M", extra=()):
        smp = os.environ.get("SMOKE_SMP", "2")
        cmd = ["qemu-system-i386", *smokelib.QEMU_DISPLAY, "-kernel", "kernel.elf",
               "-initrd", "initrd.tar", "-serial", "stdio", "-m", mem,
               "-no-reboot", "-no-shutdown", "-smp", smp,
               "-drive", f"file={disk},format=raw,index=0,media=disk,snapshot=on",
               *extra]
        self.proc = subprocess.Popen(cmd, cwd=ROOT, stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                     bufsize=0)
        self.sel = selectors.DefaultSelector()
        self.sel.register(self.proc.stdout, selectors.EVENT_READ)
        self.log = []

    def login(self, user="root", password="root"):
        smokelib.login(self.proc, self.sel, self.log, user, password, timeout=120)

    def run(self, command, timeout=60.0, prompt=PROMPT):
        """Run one shell line; return what it printed (echo stripped)."""
        at = smokelib.mark(self.log)
        smokelib.send(self.proc, command + "\n")
        end = smokelib.wait_for(self.proc, self.sel, prompt, self.log, timeout, at)
        out = "".join(self.log)[at:end - len(prompt)]
        out = out.split("\n", 1)[1] if "\n" in out else ""
        # Kernel exec traces interleave with the output.
        return "\n".join(l for l in out.splitlines() if not l.startswith("[SYSCALL] exec"))

    def stop(self):
        if self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait()


def expect(out, pattern, what):
    if not re.search(pattern, out, re.M):
        raise AssertionError(f"{what}: {pattern!r} not in output:\n{out}")


def reject(out, pattern, what):
    if re.search(pattern, out, re.M):
        raise AssertionError(f"{what}: unexpected {pattern!r} in output:\n{out}")


def check_tools(g):
    out = g.run("busybox ps")
    expect(out, r"^\s*PID\s+USER\s+TIME\s+COMMAND", "busybox ps header")
    expect(out, r"^\s*1 root .*init", "busybox ps lists init")
    out = g.run("busybox top -b -n 1")
    expect(out, r"^Mem: \d+K used, \d+K free", "busybox top memory line")
    expect(out, r"^CPU:\s+\d+% usr\s+\d+% sys", "busybox top CPU line")
    expect(out, r"^Load average: \d+\.\d\d \d+\.\d\d \d+\.\d\d \d+/\d+ \d+", "busybox top load")
    expect(out, r"^\s*1\s+0 root\s+S", "busybox top lists init")
    out = g.run("busybox free")
    expect(out, r"^Mem:\s+\d+\s+\d+\s+\d+", "busybox free")
    out = g.run("busybox uptime")
    expect(out, r"up .* load average: \d+\.\d\d, \d+\.\d\d, \d+\.\d\d", "busybox uptime")

    out = g.run("toybox ps -ef")
    expect(out, r"^UID\s+PID\s+PPID\s+C\s+STIME\s+TTY\s+TIME\s+CMD", "toybox ps -ef header")
    expect(out, r"^root\s+1\s+0\s", "toybox ps -ef lists init")
    out = g.run("toybox top -b -n 1")
    expect(out, r"^Tasks: \d+ total,\s+\d+ running", "toybox top tasks")
    expect(out, r"\d+%cpu\s+\d+%user\s+\d+%nice\s+\d+%sys\s+\d+%idle", "toybox top cpu")
    out = g.run("toybox free")
    expect(out, r"^Mem:\s+\d+\s+\d+\s+\d+", "toybox free")
    out = g.run("toybox uptime")
    expect(out, r"load average: \d+\.\d\d, \d+\.\d\d, \d+\.\d\d", "toybox uptime")
    # vmstat's first line divides by the idle seconds of /proc/uptime.
    out = g.run("toybox sleep 2; toybox vmstat 1 3", timeout=60)
    expect(out, r"^ r  b\s+swpd\s+free\s+buff\s+cache", "toybox vmstat header")
    rows = re.findall(r"^\s*\d+\s+\d+\s+\d+\s+\d+\s+\d+\s+\d+(\s+\d+){10}\s*$", out, re.M)
    if len(rows) != 3:
        raise AssertionError(f"toybox vmstat 1 3: {len(rows)} sample rows:\n{out}")
    reject(out, r"killed by signal", "toybox vmstat")
    out = g.run("cat /proc/loadavg; cat /proc/uptime; head -1 /proc/stat")
    expect(out, r"^\d+\.\d\d \d+\.\d\d \d+\.\d\d \d+/\d+ \d+$", "/proc/loadavg")
    expect(out, r"^\d+\.\d\d \d+\.\d\d$", "/proc/uptime")
    expect(out, r"^cpu  \d+ 0 \d+ \d+ 0 0 0 0 0 0$", "/proc/stat cpu line")

    # killall and pgrep on a background sleeper.
    g.run("toybox sleep 300 &")
    # Its comm is "toybox" (the multiplexer), as on Linux: match the command line.
    out = g.run("toybox pgrep -f 'sleep 300'")
    expect(out, r"^\d+$", "toybox pgrep -f finds the sleeper")
    out = g.run("toybox killall sleep; echo killall=$?")
    expect(out, r"killall=0", "toybox killall")
    out = g.run("toybox sleep 1; toybox pgrep -f 'sleep 300'; echo pgrep=$?")
    expect(out, r"pgrep=1", "the sleeper is gone after killall")
    out = g.run("toybox killall nosuchprocess; echo killall=$?")
    expect(out, r"killall=1", "toybox killall of nothing")


def check_utmp(g):
    out = g.run("toybox who")
    expect(out, r"^root\s+tty0\s+\d{4}-\d\d-\d\d \d\d:\d\d", "toybox who")
    out = g.run("toybox w")
    expect(out, r"^root\s+tty0\s", "toybox w")
    out = g.run("toybox last")
    expect(out, r"^root\s+tty0 .*still logged in", "toybox last")
    expect(out, r"^reboot\s+", "toybox last shows the boot")


def check_ipc(g):
    out = g.run("/abiprobes/p45_sysv_ipc", timeout=90)
    expect(out, r"^PASS p45_sysv_ipc$", "SysV IPC probe")
    # An unprivileged user cannot pin unbounded kernel memory (inotify
    # queues, message queues, shm) and the system stays usable.
    out = g.run("/abiprobes/p48_ipc_limits", timeout=300)
    expect(out, r"^PASS p48_ipc_limits$", "IPC/inotify limits probe")
    # /proc fd/fdinfo nodes go once the fds close; inotify accounting buckets
    # do not drift when a spilled user later gets a slot.
    out = g.run("/abiprobes/p49_accounting", timeout=150)
    expect(out, r"^PASS p49_accounting$", "accounting probe")
    out = g.run("cat /proc/sys/fs/inotify/queued_bytes")
    expect(out, r"^0$", "no inotify event memory left behind")
    out = g.run("cat /proc/sysvipc/shm | toybox wc -l")
    expect(out, r"^\s*1$", "no segment left behind by the probe")
    out = g.run("toybox ipcs -m")
    expect(out, r"Shared Memory Segments", "toybox ipcs")
    out = g.run("cat /proc/sys/kernel/shmmax /proc/sys/kernel/sem")
    expect(out, r"^\d+$", "shmmax")
    expect(out, r"^\d+\t\d+\t\d+\t\d+$", "sem limits")


# Scratch filesystems for the inotify probe beyond tmpfs and the ext2 disk:
# (image, mkfs command, fstype), attached as hdb, hdc, hdd.
EXTRA_FS = [("vfat.img", ["mkfs.vfat", "-F", "32"], "vfat"),
            ("exfat.img", ["mkfs.exfat"], "exfat"),
            ("ext4.img", ["mkfs.ext4", "-q", "-F"], "ext4")]
WORK = os.path.join(ROOT, "build", "smoke-procipc")


def find_tool(name):
    for d in os.environ.get("PATH", "").split(os.pathsep) + ["/usr/sbin", "/sbin"]:
        if d and os.access(os.path.join(d, name), os.X_OK):
            return os.path.join(d, name)
    return None


def make_extra_fs():
    """Fresh 64 MiB images; the filesystems whose mkfs is missing are left out."""
    os.makedirs(WORK, exist_ok=True)
    made = []
    for img, mkfs, fstype in EXTRA_FS:
        tool = find_tool(mkfs[0])
        if not tool:
            print(f"[SMOKE-PROCIPC] {mkfs[0]} not found: no {fstype} inotify run")
            continue
        path = os.path.join(WORK, img)
        with open(path, "wb") as f:
            f.truncate(64 << 20)
        subprocess.run([tool, *mkfs[1:], path], check=True, stdout=subprocess.DEVNULL)
        made.append((path, fstype))
    return made


def check_inotify(g, extra):
    for d in ("/tmp", "/disk"):
        out = g.run(f"/abiprobes/p46_inotify {d}", timeout=150)
        expect(out, r"^PASS p46_inotify$", f"inotify probe on {d}")
    for i, (_, fstype) in enumerate(extra):
        dev, mnt = "/dev/hd" + "bcd"[i], "/tmp/" + fstype
        out = g.run(f"busybox mkdir -p {mnt} && busybox mount -t {fstype} {dev} {mnt}; echo rc=$?")
        expect(out, r"rc=0", f"mount {fstype}")
        out = g.run(f"/abiprobes/p46_inotify {mnt}", timeout=150)
        expect(out, r"^PASS p46_inotify$", f"inotify probe on {fstype}")


def check_proc_perms(g):
    out = g.run("/abiprobes/p47_proc_pid", timeout=90)
    expect(out, r"^PASS p47_proc_pid$", "/proc/<pid> probe")
    # By hand, as the unprivileged account: init (root) is another user.
    g.run("exit", prompt=smokelib.LOGIN_PROMPT, timeout=60)
    g.login("user", "user")
    out = g.run("toybox cat /proc/1/environ; echo rc=$?")
    expect(out, r"Permission denied", "another user's environ")
    expect(out, r"rc=1", "another user's environ")
    out = g.run("toybox ls /proc/1/fd; echo rc=$?")
    expect(out, r"rc=1", "another user's fd/")
    out = g.run("toybox readlink /proc/1/cwd; echo rc=$?")
    expect(out, r"rc=1", "another user's cwd link")
    out = g.run("cat /proc/1/stat")
    expect(out, r"^1 \(init\) S 0 ", "another user's stat stays readable")
    out = g.run("toybox grep -c . /proc/self/environ")
    expect(out, r"^\d+$", "one's own environ")
    out = g.run("toybox who")
    expect(out, r"^user\s+tty0\s", "who after the second login")
    out = g.run("toybox last")
    expect(out, r"^root\s+tty0 .*\d\d:\d\d - \d\d:\d\d", "last shows root's ended session")


def run_initrd(args):
    extra = make_extra_fs()
    g = Guest("disk.img", extra=[a for i, (path, _) in enumerate(extra) for a in
                                 ("-drive", f"file={path},format=raw,index={i + 1},media=disk")])
    try:
        g.login()
        check_tools(g)
        check_utmp(g)
        check_ipc(g)
        check_inotify(g, extra)
        check_proc_perms(g)
    finally:
        g.stop()


class MirrorProxy(BaseHTTPRequestHandler):
    """GET /<path> from the Alpine mirror (ALPINE_MIRROR, as prepare.py), so
    the guest's apk reaches it over QEMU user networking without DNS or TLS.
    apk still checks the signed index and every package against the keys in
    the image."""
    mirror = os.environ.get("ALPINE_MIRROR", "https://dl-cdn.alpinelinux.org/alpine")

    def do_GET(self):
        try:
            with urllib.request.urlopen(self.mirror + self.path, timeout=60) as r:
                body = r.read()
            self.send_response(200)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        except Exception as e:
            self.send_error(404, str(e))

    def log_message(self, fmt, *a):
        sys.stderr.write("[mirror] " + (fmt % a) + "\n")


def run_alpine(args):
    img = os.environ.get("ALPINE_IMG", os.path.join(ROOT, "disk-alpine.img"))
    if not os.path.exists(img):
        raise AssertionError("disk-alpine.img missing: make disk-alpine")
    httpd = ThreadingHTTPServer(("127.0.0.1", 0), MirrorProxy)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    port = httpd.server_address[1]
    branch = os.environ.get("ALPINE_BRANCH", "v3.22")
    g = Guest(img, mem="1024M",
              extra=["-netdev", "user,id=n0", "-device", "rtl8139,netdev=n0"])
    try:
        g.login()
        a = ("toybox chroot /disk/alpine /usr/bin/env -i "
             "PATH=/usr/sbin:/usr/bin:/sbin:/bin HOME=/root TERM=vt100 /bin/sh -c ")
        # Not in the image (its package set is locked): fetched from the
        # mirror through the host; the disk is a snapshot, nothing persists.
        out = g.run(a + f"'apk add --repositories-file /dev/null -X http://10.0.2.2:{port}/{branch}/main procps-ng htop; "
                    "echo rc=$?'", timeout=900)
        expect(out, r"rc=0", "apk add procps-ng htop")
        out = g.run(a + "'ps -eo pid,user,stat,etime,rss,cmd; echo rc=$?'")
        expect(out, r"^\s*PID USER\s+STAT\s+ELAPSED\s+RSS CMD", "procps ps")
        expect(out, r"rc=0", "procps ps exit")
        out = g.run(a + "'free -m; uptime; pgrep -l sh; echo rc=$?'")
        expect(out, r"^Mem:\s+\d+", "procps free")
        expect(out, r"load average:", "procps uptime")
        out = g.run(a + "'vmstat 1 2; echo rc=$?'", timeout=60)
        expect(out, r"rc=0", "procps vmstat")
        out = g.run(a + "'top -b -n 1 | head -5; echo rc=$?'")
        expect(out, r"^top - .* load average", "procps top")
        out = g.run(a + "'TERM=vt100 timeout 5 htop; echo; echo rc=$?'", timeout=60)
        expect(out, r"rc=(0|124|143)", "htop starts and runs until killed")
        reject(out, r"killed by signal 11", "htop")
    finally:
        g.stop()
        httpd.shutdown()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--alpine", action="store_true",
                    help="also run Alpine's procps and htop (needs disk-alpine.img)")
    ap.add_argument("--only-alpine", action="store_true")
    args = ap.parse_args()
    if not args.only_alpine:
        run_initrd(args)
    if args.alpine or args.only_alpine:
        run_alpine(args)
    print("\n[SMOKE-PROCIPC] passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"\n[SMOKE-PROCIPC] failed: {exc}", file=sys.stderr)
        raise SystemExit(1)
