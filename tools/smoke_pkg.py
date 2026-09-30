#!/usr/bin/env python3
"""smoke-pkg: pkg against a host HTTP repo, end to end in the guest.

Boots the disk image (a scratch copy, so disk.img is left as built) with
networking, points /disk/etc/pkg.conf at a host HTTP server holding the
busybox package from `make repo`, and serves a sequence of index.txt
variants signed with the repo key (tools/mkrepo.py):

  unsigned / tampered / signature-less index   -> pkg update rejects it
  valid index                                  -> update, install, run, remove
  validly signed index with a lower serial     -> rejected as a rollback
  validly signed index with a wrong sha256     -> install rejects the download
  cached index modified in the guest           -> pkg list refuses it
"""
import os
import selectors
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import mkrepo  # noqa: E402

PROMPT = "MaeroOS$ "
PKG = "busybox"


def wait_for(proc, sel, needle, log, timeout=25.0, start=0):
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


class Quiet(SimpleHTTPRequestHandler):
    def log_message(self, *args):
        pass


def main():
    index = os.path.join(ROOT, "repo", "index.txt")
    line = next((ln for ln in open(index).read().splitlines()
                 if ln.startswith(PKG + "|")), None)
    if not line:
        raise AssertionError(f"{PKG} is not in repo/index.txt (run make repo)")
    seed = mkrepo.load_key()

    web = tempfile.TemporaryDirectory()
    scratch = tempfile.TemporaryDirectory()
    shutil.copy(os.path.join(ROOT, "repo", line.split("|")[3]), web.name)
    disk = os.path.join(scratch.name, "disk.img")
    subprocess.run(["cp", "--reflink=auto", os.path.join(ROOT, "disk.img"), disk],
                   check=True)

    def serve(text):
        with open(os.path.join(web.name, "index.txt"), "w") as f:
            f.write(text)

    httpd = ThreadingHTTPServer(("0.0.0.0", 0),
                                partial(Quiet, directory=web.name))
    port = httpd.server_address[1]
    threading.Thread(target=httpd.serve_forever, daemon=True).start()

    proc = subprocess.Popen(
        ["qemu-system-i386", "-kernel", "kernel.elf", "-initrd", "initrd.tar",
         "-drive", f"file={disk},format=raw,index=0,media=disk",
         "-serial", "stdio", "-m", "128M", "-no-reboot", "-no-shutdown",
         "-netdev", "user,id=n0", "-device", "rtl8139,netdev=n0"],
        cwd=ROOT, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, bufsize=0)
    sel = selectors.DefaultSelector()
    sel.register(proc.stdout, selectors.EVENT_READ)
    log = []

    def run(cmd, expected, forbidden=None, timeout=30.0):
        before = len("".join(log))
        proc.stdin.write((cmd + "\n").encode("latin1"))
        proc.stdin.flush()
        wait_for(proc, sel, PROMPT, log, timeout=timeout, start=before)
        recent = "".join(log)[before:]
        if expected not in recent:
            raise AssertionError(f"{cmd!r} did not produce {expected!r}")
        if forbidden and forbidden in recent:
            raise AssertionError(f"{cmd!r} unexpectedly produced {forbidden!r}")

    good = mkrepo.sign_index([line], seed, 2000)
    tampered = good.replace("|300+ ", "|301+ ", 1)
    assert tampered != good
    wrong_sha = line[:-64] + ("0" if line[-64] != "0" else "1") + line[-63:]

    try:
        wait_for(proc, sel, PROMPT, log, timeout=40.0)
        run(f"printf 'repo=10.0.2.2:{port}\\n' > /disk/etc/pkg.conf", PROMPT)

        serve(line + "\n")
        run("pkg update", "index is not signed", "available")
        serve(tampered)
        run("pkg update", "index signature is invalid", "available")
        serve(good[:good.index("#sig ")])
        run("pkg update", "index signature is missing", "available")

        serve(good)
        run("pkg update", "pkg: 1 package(s) available")
        run("pkg list", PKG)
        run(f"pkg install {PKG}", f"Installed {PKG}", timeout=90.0)
        run(f"/disk/apps/{PKG}/busybox echo PKG_E2E_OK", "PKG_E2E_OK")
        run(f"pkg remove {PKG}", f"Removed {PKG}")

        serve(mkrepo.sign_index([line], seed, 1500))
        run("pkg update", "refusing rollback", "available")
        run("pkg list", PKG)                     # cached index kept

        serve(mkrepo.sign_index([wrong_sha], seed, 3000))
        run("pkg update", "pkg: 1 package(s) available")
        run(f"pkg install {PKG}", "checksum mismatch", "Installed", timeout=90.0)

        run("printf 'x|1|1|x.tar|x|x\\n' >> /disk/etc/pkg-index.txt", PROMPT)
        run("pkg list", "cached index not trusted", PKG + " ")
        print("\n[SMOKE-PKG] passed")
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
        web.cleanup()
        scratch.cleanup()


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"\n[SMOKE-PKG] failed: {exc}", file=sys.stderr)
        raise SystemExit(1)
