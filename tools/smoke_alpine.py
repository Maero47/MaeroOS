#!/usr/bin/env python3
"""smoke-alpine: Alpine Linux x86 userland in a chroot on the ext2 disk.

Boots with disk-alpine.img (ports/alpine/prepare.py: the Alpine root at
/alpine on the disk, so /disk/alpine in the guest) and runs, each through
`chroot /disk/alpine`:

  bash -c 'echo ok', GNU ls --version, python3 -c 'print(1+1)',
  vim --version, git (init/add/commit/log), ssh -V, less on a pipe,
  and an apk add / apk del cycle of `tree` from the offline repo on the
  disk (/repo, the signed Alpine APKINDEX plus the .apk), which apk
  verifies against /etc/apk/keys itself.

Offline: everything comes from the image.  No [SYSCALL] unimplemented line
may appear while the Alpine programs run.  The image is copied first, so
the apk cycle never changes disk-alpine.img itself.  ALPINE_IMG picks
another image (e.g. one built with ALPINE_BRANCH=v3.24, apk-tools 3).

--net (opt-in) adds an RTL8139 on QEMU user networking and serves the
repo prepare.py staged (build/alpine/stage/alpine/repo) over HTTP on the
host; the guest then also installs `tree` from http://10.0.2.2:<port>/main,
which is apk's own fetch code over the kernel's TCP/IP.
"""
import os
import selectors
import shutil
import subprocess
import sys
import threading
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer

import smokelib

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
IMG = os.path.abspath(os.environ.get("ALPINE_IMG", os.path.join(ROOT, "disk-alpine.img")))
WORK = os.path.join(ROOT, "build", "smoke-alpine.img")
REPO = os.path.join(ROOT, "build", "alpine", "stage", "alpine", "repo")


class QuietHandler(SimpleHTTPRequestHandler):
    def log_message(self, fmt, *args):
        sys.stderr.write("[http] " + (fmt % args) + "\n")


def serve_repo():
    """Serve REPO on an ephemeral host port; returns (server, port)."""
    httpd = ThreadingHTTPServer(("127.0.0.1", 0),
                                partial(QuietHandler, directory=REPO))
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    return httpd, httpd.server_address[1]

ENV = "/usr/bin/env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin HOME=/root TERM=vt100"
CHROOT = "toybox chroot /disk/alpine " + ENV


def main():
    net = "--net" in sys.argv[1:]
    httpd = port = None
    if net:
        if not os.path.isdir(REPO):
            raise SystemExit("smoke_alpine --net: no staged repo at " + REPO +
                             " - run ports/alpine/prepare.py")
        httpd, port = serve_repo()
    if not os.path.exists(IMG):
        raise SystemExit("smoke_alpine: disk-alpine.img is missing - run "
                         "`python3 ports/alpine/prepare.py` (or make smoke-alpine)")
    os.makedirs(os.path.dirname(WORK), exist_ok=True)
    shutil.copyfile(IMG, WORK)
    accel = ["-accel", "kvm"] if os.access("/dev/kvm", os.R_OK | os.W_OK) else []
    proc = subprocess.Popen(
        ["qemu-system-i386", "-kernel", "kernel.elf", "-initrd", "initrd.tar",
         "-drive", f"file={WORK},format=raw,index=0,media=disk",
         "-serial", "stdio", "-m", "1024M", "-no-reboot", "-no-shutdown"]
        + (["-netdev", "user,id=n0", "-device", "rtl8139,netdev=n0"] if net else [])
        + accel + smokelib.QEMU_DISPLAY,
        cwd=ROOT, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, bufsize=0)
    sel = selectors.DefaultSelector()
    sel.register(proc.stdout, selectors.EVENT_READ)
    log = []

    def run(cmd, *needles, absent=(), timeout=120.0):
        at = smokelib.mark(log)
        smokelib.send(proc, cmd + "\n")
        smokelib.wait_for(proc, sel, smokelib.PROMPT, log, timeout, at)
        body = "".join(log)[at:]
        for n in needles:
            if n not in body:
                raise AssertionError(f"{cmd!r}: missing {n!r}")
        for n in absent + ("[SYSCALL] unimplemented",):
            if n in body:
                raise AssertionError(f"{cmd!r}: unexpected {n!r}")
        return body

    def alpine(cmd, *needles, **kw):
        # One `sh -c` per command; the marker proves the command's exit
        # status.  It is spelled AL_""OK so the echoed command line, which
        # the console shows too, cannot match it.
        return run(f"{CHROOT} /bin/sh -c '{cmd} && echo AL_\"\"OK'",
                   *needles, "AL_OK", **kw)

    try:
        smokelib.login(proc, sel, log, timeout=90)
        run(f"{CHROOT} /bin/bash -c 'echo ok-$BASH_VERSION'", "ok-5.")
        alpine("ls --version | head -1", "ls (GNU coreutils)")
        alpine("python3 -c \"print(1+1, 40+2)\"", "2 42")
        alpine("vim --version | head -1", "VIM - Vi IMproved")
        alpine("cat /etc/alpine-release")
        # Pipes are not terminals: less copies piped input straight through.
        alpine("echo piped-text | less | cat", "piped-text",
               absent=("Missing filename",))
        alpine("ls -l /bin/bash", "root root")
        alpine("ssh -V 2>&1", "OpenSSH_")
        alpine("cd /tmp && rm -rf g && git init -q g && cd g && echo a > a && "
               "git add a && git -c user.name=t -c user.email=t@t commit -qm first && "
               "git --no-pager log --oneline", "first")
        # The apk cycle: tree is in /repo, not installed.
        alpine("! apk info -e tree")
        alpine("apk add tree", "Installing tree", absent=("ERROR", "WARNING"))
        alpine("test ! -L /usr/bin/tree && tree --version", "tree v2.")
        alpine("tree /etc/apk | tail -1", "directories,")
        alpine("apk del tree", "Purging tree", absent=("ERROR",))
        # The package's /usr/bin/tree is gone; busybox's trigger puts its own
        # `tree` applet back as a symlink, as on any Alpine system.
        alpine("! apk info -e tree && test -L /usr/bin/tree")
        alpine("apk verify", absent=("ERROR",))
        if net:
            # The same package, fetched by apk over HTTP from the host.
            url = f"http://10.0.2.2:{port}/main"
            alpine(f"apk add --repositories-file /dev/null -X {url} tree",
                   "Installing tree", absent=("ERROR", "WARNING"), timeout=180)
            alpine("test ! -L /usr/bin/tree && apk del tree", "Purging tree")
        # /dev and /proc are the global ones inside the chroot.
        alpine("head -c 8 /dev/urandom | wc -c && test -r /proc/self/status", "8")
        print("\n[SMOKE-ALPINE] passed")
        return 0
    except Exception as e:
        print(f"\n[SMOKE-ALPINE] FAILED: {e}")
        return 1
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
        if httpd:
            httpd.shutdown()


if __name__ == "__main__":
    sys.exit(main())
