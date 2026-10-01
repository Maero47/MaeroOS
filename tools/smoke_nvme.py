#!/usr/bin/env python3
"""NVMe smoke test: boot with the disk on an NVMe controller only.

Two boots share one scratch copy of disk.img (build/smoke-nvme/disk.img), so
the suite never changes the image the other suites use:

1. pc machine, the disk as the single namespace of `-device nvme` (no IDE or
   AHCI disk).  Checks the kernel found the namespace and mounted /disk from
   it, runs diskprobe/fsprobe, reads a random 3 MiB file the host planted
   (md5 must match the host's), copies it on the disk, then reboots (QEMU runs
   with -no-reboot, so it must exit).  The driver's shutdown line must show
   I/O commands that needed a PRP list.  The host then reads the copy back out
   of the image with debugfs and checks its md5: every byte went through NVMe
   writes.
2. q35 machine, an NVMe controller with MDTS = 2 (16 KiB per command) and two
   namespaces: NSID 1 is the image, NSID 2 a blank 8 MiB scratch disk.  Both
   must be found; the copy survived and still checksums (through split
   transfers this time), it is removed, and the guest powers off.
"""
import hashlib
import os
import re
import selectors
import shutil
import subprocess
import sys
import time

import smokelib

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
WORK = os.path.join(ROOT, "build", "smoke-nvme")
IMG = os.path.join(WORK, "disk.img")
IMG2 = os.path.join(WORK, "ns2.img")
BIG = os.path.join(WORK, "nvme-big.bin")
PROMPT = smokelib.PROMPT
BIG_BYTES = 3 * 1024 * 1024


def debugfs(*args, write=False):
    tool = shutil.which("debugfs") or "/sbin/debugfs"
    cmd = [tool] + (["-w"] if write else []) + list(args) + [IMG]
    return subprocess.run(cmd, capture_output=True, timeout=60)


def boot(machine_args):
    proc = subprocess.Popen(
        ["qemu-system-i386", *smokelib.QEMU_DISPLAY,
         "-kernel", "kernel.elf", "-initrd", "initrd.tar",
         *machine_args,
         "-serial", "stdio", "-m", "128M", "-no-reboot"],
        cwd=ROOT, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, bufsize=0)
    sel = selectors.DefaultSelector()
    sel.register(proc.stdout, selectors.EVENT_READ)
    return proc, sel


def stop(proc):
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()


def run(proc, sel, log, command, expected, forbidden=None, timeout=30.0):
    before = smokelib.mark(log)
    smokelib.send(proc, command)
    smokelib.wait_for(proc, sel, PROMPT, log, timeout, before)
    recent = "".join(log)[before:]
    if expected not in recent:
        raise AssertionError(f"{command.strip()!r} did not produce {expected!r}:\n{recent}")
    if forbidden and forbidden in recent:
        raise AssertionError(f"{command.strip()!r} unexpectedly produced {forbidden!r}")
    return recent


def check_boot_log(log, machine, needles):
    text = "".join(log)
    for needle in ("[ATA]  Drive not present.",
                   "[BLK]  boot disk: nvme0",
                   "[EXT2]  Mounted:",
                   "[BOOT] Launching /disk/init", *needles):
        if needle not in text:
            raise AssertionError(f"{machine}: boot log lacks {needle!r}")
    for bad in ("[NVMe] command", "timed out", "[NVMe] completion for CID",
                "[BOOT] failed to launch /disk/init"):
        if bad in text:
            raise AssertionError(f"{machine}: boot log has {bad!r}")


SHUTDOWN = re.compile(r"\[NVMe\] controller 0 shut down: (\d+) I/O commands, (\d+) with a "
                      r"PRP list, (\d+) MDTS chunks, largest (\d+) pages")


def wait_exit(proc, sel, log, what, timeout=20.0):
    """The guest asked to reboot/power off; QEMU (-no-reboot) must exit, and
    the driver must have shut the controller down first.  Returns the
    driver's counters (commands, PRP-list commands, MDTS chunks, max pages)."""
    deadline = time.time() + timeout
    while proc.poll() is None:
        if time.time() > deadline:
            raise AssertionError(f"QEMU did not exit after {what}")
        for key, _ in sel.select(timeout=0.2):
            data = key.fileobj.read(4096)
            if data:
                log.append(data.decode("utf-8", "replace"))
    rest = proc.stdout.read()
    if rest:
        log.append(rest.decode("utf-8", "replace"))
    m = SHUTDOWN.search("".join(log))
    if not m:
        raise AssertionError(f"no NVMe shutdown line after {what}")
    print(m.group(0).strip())
    return tuple(int(g) for g in m.groups())


# What the ext2 driver leaves behind on IDE just the same (not NVMe's doing):
# see tools/smoke_ahci.py.
FSCK_KNOWN = re.compile(r"^(Pass \d:.*|Inodes that were part of a corrupted orphan.*"
                        r"|Inode \d+ was part of the orphaned inode list.*"
                        r"|Deleted inode \d+ has zero dtime.*"
                        r"|Directories count wrong for group.*|Fix\? no|e2fsck .*|"
                        r".*WARNING: Filesystem still has errors.*|.*: \d+/\d+ files .*|)$")


def check_fsck():
    fsck = subprocess.run([shutil.which("e2fsck") or "/sbin/e2fsck", "-fn", IMG],
                          capture_output=True, text=True, timeout=120)
    out = fsck.stdout + fsck.stderr
    bad = [l for l in out.splitlines() if not FSCK_KNOWN.match(l.strip())]
    if bad:
        raise AssertionError("e2fsck -fn reported:\n" + "\n".join(bad))


def main():
    os.makedirs(WORK, exist_ok=True)
    subprocess.run(["cp", "--reflink=auto", os.path.join(ROOT, "disk.img"), IMG], check=True)
    with open(IMG2, "wb") as f:
        f.truncate(8 * 1024 * 1024)
    # disk.img may already have been through smoke-disk (make check runs it
    # first), and diskprobe cannot remove the /dpdir that run left behind.
    listing = debugfs("-R", "ls /dpdir").stdout.decode("utf-8", "replace")
    if "File not found" not in listing:
        for ino, name in re.findall(r"(\d+)\s+\(\d+\)\s+(\S+)", listing):
            if ino != "0" and name not in (".", ".."):
                debugfs("-R", f"rm /dpdir/{name}", write=True)
        debugfs("-R", "rmdir /dpdir", write=True)
    data = os.urandom(BIG_BYTES)
    with open(BIG, "wb") as f:
        f.write(data)
    want = hashlib.md5(data).hexdigest()
    debugfs("-R", f"write {BIG} /nvme-big", write=True)
    if "Type: regular" not in debugfs("-R", "stat /nvme-big").stdout.decode():
        raise AssertionError("debugfs could not plant /nvme-big")

    # ── Boot 1: pc + one NVMe namespace ─────────────────────────────────────
    t0 = time.time()
    log = []
    proc, sel = boot(["-drive", f"file={IMG},if=none,id=nv0,format=raw",
                      "-device", "nvme,serial=deadbeef,drive=nv0"])
    try:
        smokelib.login(proc, sel, log)
        check_boot_log(log, "pc+nvme", (
            "\"QEMU NVMe Ctrl\" fw ",
            "sn deadbeef, ",
            "[NVMe] disk 0: controller 0 namespace 1, 384 MiB",
            "[NVMe] 1 disk(s) on 1 controller(s)"))
        run(proc, sel, log, "cat /disk/hello.txt\n", "o from MaeroOS initrd!")
        run(proc, sel, log, "diskprobe\n", "diskprobe ok")
        run(proc, sel, log, "fsprobe\n", "fsprobe ok", "fsprobe FAIL")
        run(proc, sel, log, "toybox md5sum /disk/nvme-big\n", want)
        run(proc, sel, log, "cp /disk/nvme-big /disk/nvme-copy\n", PROMPT, "cp:", timeout=60.0)
        run(proc, sel, log, "toybox md5sum /disk/nvme-copy\n", want)
        run(proc, sel, log, "printf persisted > /disk/nvme-note\n", PROMPT)
        smokelib.send(proc, "reboot\n")
        n_io, n_prp, _, max_pages = wait_exit(proc, sel, log, "reboot")
        if n_prp == 0 or max_pages <= 2:
            raise AssertionError("pc+nvme: no transfer needed a PRP list")
    finally:
        stop(proc)
    t1 = time.time()

    got = debugfs("-R", "dump /nvme-copy /dev/stdout").stdout
    if len(got) != BIG_BYTES or hashlib.md5(got).hexdigest() != want:
        raise AssertionError(f"host: /nvme-copy is {len(got)} bytes, md5 "
                             f"{hashlib.md5(got).hexdigest()} (want {want})")
    if b"persisted" not in debugfs("-R", "cat /nvme-note").stdout:
        raise AssertionError("host: /nvme-note did not persist")
    check_fsck()

    # ── Boot 2: q35, MDTS = 2, two namespaces ───────────────────────────────
    log = []
    proc, sel = boot(["-M", "q35",
                      "-device", "nvme,id=nvme0,serial=cafe0002,mdts=2",
                      "-drive", f"file={IMG},if=none,id=nv0,format=raw",
                      "-device", "nvme-ns,drive=nv0,bus=nvme0,nsid=1",
                      "-drive", f"file={IMG2},if=none,id=nv1,format=raw",
                      "-device", "nvme-ns,drive=nv1,bus=nvme0,nsid=2"])
    try:
        smokelib.login(proc, sel, log)
        check_boot_log(log, "q35", (
            "[NVMe] disk 0: controller 0 namespace 1, 384 MiB",
            "[NVMe] disk 1: controller 0 namespace 2, 8 MiB",
            "[NVMe] 2 disk(s) on 1 controller(s)"))
        run(proc, sel, log, "cat /disk/nvme-note\n", "persisted")
        run(proc, sel, log, "toybox md5sum /disk/nvme-copy\n", want)
        run(proc, sel, log, "rm /disk/nvme-copy\n", PROMPT)
        run(proc, sel, log, "rm /disk/nvme-big\n", PROMPT)
        run(proc, sel, log, "cat /disk/nvme-copy\n", "cat: cannot open file")
        smokelib.send(proc, "poweroff\n")
        _, _, n_split, max_pages = wait_exit(proc, sel, log, "poweroff")
        if n_split == 0 or max_pages != 4:
            raise AssertionError("q35: transfers were not split at MDTS (4 pages)")
    finally:
        stop(proc)
    t2 = time.time()

    if "Type: regular" in debugfs("-R", "stat /nvme-copy").stdout.decode():
        raise AssertionError("host: /nvme-copy still exists after rm on q35")
    check_fsck()

    for path in (BIG, IMG, IMG2):
        os.unlink(path)
    print(f"\n[SMOKE-NVME] passed (pc+nvme {t1 - t0:.1f}s, q35 {t2 - t1:.1f}s)")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"\n[SMOKE-NVME] failed: {exc}", file=sys.stderr)
        raise SystemExit(1)
