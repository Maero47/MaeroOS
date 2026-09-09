#!/usr/bin/env python3
"""race_probe: run /disk/raceprobe in the guest and report its verdict.

The probe races a clustered-read reader against a block writer on one ext2 file
and then checks every block against the last generation written; see
userspace/raceprobe/raceprobe.c.  Memory is deliberately small (128 MiB) so the
ext2 block cache is a few MiB and the probe's file is several times larger than
it — a cache hit never reaches the clustered path, so the reader has to keep
missing for the window to be exercised at all.

usage: race_probe.py [--kib N] [--passes N] [--mem SIZE] [--timeout SEC]
exit 0 = the probe reported OK, 1 = it reported FAIL, 2 = the harness failed.
"""
import argparse
import os
import selectors
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
PROMPT = "MaeroOS$ "


def wait_for(proc, sel, needles, log, timeout, start=0):
    """Wait until one of `needles` appears; return the one that did."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        for key, _ in sel.select(0.2):
            chunk = os.read(key.fd, 4096).decode("latin1", "replace")
            if not chunk:
                continue
            log.append(chunk)
            sys.stdout.write(chunk)
            sys.stdout.flush()
        blob = "".join(log)[start:]
        for n in needles:
            if n in blob:
                return n
        if proc.poll() is not None:
            raise RuntimeError("QEMU exited with status %s" % proc.returncode)
    raise TimeoutError("timed out waiting for %r" % (needles,))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--kib", type=int, default=12288, help="file size in KiB")
    ap.add_argument("--passes", type=int, default=12, help="reader passes")
    ap.add_argument("--mem", default="128M")
    ap.add_argument("--timeout", type=int, default=900)
    ap.add_argument("--fresh-disk", action="store_true",
                    help="rebuild disk.img before running.  Not needed: the run "
                         "creates and deletes a large file and gives the space "
                         "back, so repeated runs are independent on one image")
    args = ap.parse_args()

    os.chdir(ROOT)
    if args.fresh_disk:
        print("race_probe: rebuilding disk.img")
        if subprocess.call(["make", "disk"], stdout=subprocess.DEVNULL,
                           stderr=subprocess.STDOUT) != 0:
            print("race_probe: ERROR make disk failed")
            return 2
    for f in ("kernel.elf", "initrd.tar", "disk.img"):
        if not os.path.exists(f):
            print("race_probe: ERROR missing %s (make initrd disk)" % f)
            return 2

    proc = subprocess.Popen(
        ["qemu-system-i386", "-kernel", "kernel.elf", "-initrd", "initrd.tar",
         "-drive", "file=disk.img,format=raw,index=0,media=disk",
         "-serial", "stdio", "-display", "none",
         "-m", args.mem, "-no-reboot", "-no-shutdown"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, bufsize=0)
    sel = selectors.DefaultSelector()
    sel.register(proc.stdout, selectors.EVENT_READ)
    log = []
    rc = 2
    try:
        wait_for(proc, sel, [PROMPT], log, 60)
        cmd = "/raceprobe %d %d\n" % (args.kib, args.passes)
        print("\nrace_probe: running %s" % cmd.strip())
        start = len("".join(log))
        proc.stdin.write(cmd.encode("latin1"))
        proc.stdin.flush()
        hit = wait_for(proc, sel, ["raceprobe: OK", "raceprobe: FAIL"],
                       log, args.timeout, start)
        # The needle can match mid-line; drain briefly so the whole verdict
        # (which carries the block number and the reason) reaches the log.
        drain = time.time() + 2.0
        while time.time() < drain:
            for key, _ in sel.select(0.2):
                chunk = os.read(key.fd, 4096).decode("latin1", "replace")
                if chunk:
                    log.append(chunk)
                    sys.stdout.write(chunk)
                    sys.stdout.flush()
        rc = 0 if hit == "raceprobe: OK" else 1
    except Exception as exc:                      # harness problem, not a verdict
        print("\nrace_probe: ERROR %s" % exc)
        rc = 2
    finally:
        try:
            proc.kill()
            proc.wait(timeout=10)
        except Exception:
            pass

    print("\nrace_probe: %s" % {0: "PASS", 1: "FAIL (probe found stale blocks)"}
          .get(rc, "ERROR"))
    return rc


if __name__ == "__main__":
    sys.exit(main())
