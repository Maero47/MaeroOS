#!/usr/bin/env python3
"""AHCI smoke test: boot with the disk on a SATA (AHCI) controller only.

Two boots share one scratch copy of disk.img (build/smoke-ahci/disk.img), so
the suite never changes the image the other suites use:

1. pc machine with an explicit `-device ahci` and the disk as an ide-hd on its
   first port; no IDE disk.  Checks the kernel found the disk on AHCI and
   mounted /disk from it, runs diskprobe/fsprobe/symprobe, reads a random 3 MiB
   file the host planted (md5 must match the host's), copies it on the disk,
   then reboots (QEMU runs with -no-reboot, so it must exit).  The host then
   reads the copy back out of the image with debugfs and checks its md5:
   every byte went through AHCI DMA writes.
2. q35 machine (its built-in ICH9 AHCI), same image: the copy survived and
   still checksums, it is removed, and the guest powers off.
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
WORK = os.path.join(ROOT, "build", "smoke-ahci")
IMG = os.path.join(WORK, "disk.img")
BIG = os.path.join(WORK, "ahci-big.bin")
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
    if isinstance(expected, re.Pattern):
        if not expected.search(recent):
            raise AssertionError(f"{command.strip()!r} did not match {expected.pattern!r}:\n{recent}")
    elif expected not in recent:
        raise AssertionError(f"{command.strip()!r} did not produce {expected!r}:\n{recent}")
    if forbidden and forbidden in recent:
        raise AssertionError(f"{command.strip()!r} unexpectedly produced {forbidden!r}")
    return recent


def check_boot_log(log, machine):
    text = "".join(log)
    for needle in ("[ATA]  Drive not present.",
                   "[AHCI] disk 0: controller 0 port 0, \"QEMU HARDDISK\"",
                   "[BLK]  boot disk: ahci0",
                   "[EXT2]  Mounted:",
                   "[BOOT] Launching /disk/init"):
        if needle not in text:
            raise AssertionError(f"{machine}: boot log lacks {needle!r}")
    for bad in ("[AHCI] port 0: command", "recovery failed", "[BOOT] failed to launch /disk/init"):
        if bad in text:
            raise AssertionError(f"{machine}: boot log has {bad!r}")


def wait_exit(proc, sel, log, what, timeout=20.0):
    """The guest asked to reboot/power off; QEMU (-no-reboot) must exit."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        for key, _ in sel.select(timeout=0.2):
            data = key.fileobj.read(4096)
            if data:
                log.append(data.decode("utf-8", "replace"))
        if proc.poll() is not None:
            return
    raise AssertionError(f"QEMU did not exit after {what}")


# What the ext2 driver leaves behind on IDE just the same (not AHCI's doing):
# the group descriptor's directory count after a boot, and deleted inodes
# whose i_dtime is ext2_now() = seconds since boot (fs/ext2.c), which e2fsck
# reads as an orphan-list link when it is small and complains about when it
# is 0 (deleted within the first second of uptime).
FSCK_KNOWN = re.compile(r"^(Pass \d:.*|Inodes that were part of a corrupted orphan.*"
                        r"|Inode \d+ was part of the orphaned inode list.*"
                        r"|Deleted inode \d+ has zero dtime.*"
                        r"|Directories count wrong for group.*|Fix\? no|e2fsck .*|"
                        r".*WARNING: Filesystem still has errors.*|.*: \d+/\d+ files .*|)$")


def check_fsck():
    """e2fsck -fn must find nothing but the known ext2-driver leftovers: any
    other complaint means metadata reached the disk wrong."""
    fsck = subprocess.run([shutil.which("e2fsck") or "/sbin/e2fsck", "-fn", IMG],
                          capture_output=True, text=True, timeout=120)
    out = fsck.stdout + fsck.stderr
    bad = [l for l in out.splitlines() if not FSCK_KNOWN.match(l.strip())]
    if bad:
        raise AssertionError("e2fsck -fn reported:\n" + "\n".join(bad))


def main():
    os.makedirs(WORK, exist_ok=True)
    subprocess.run(["cp", "--reflink=auto", os.path.join(ROOT, "disk.img"), IMG], check=True)
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
    debugfs("-R", f"write {BIG} /ahci-big", write=True)
    if "Type: regular" not in debugfs("-R", "stat /ahci-big").stdout.decode():
        raise AssertionError("debugfs could not plant /ahci-big")

    # ── Boot 1: pc + explicit AHCI controller ───────────────────────────────
    t0 = time.time()
    log = []
    proc, sel = boot(["-device", "ahci,id=ahci",
                      "-drive", f"id=d0,file={IMG},format=raw,if=none",
                      "-device", "ide-hd,drive=d0,bus=ahci.0"])
    try:
        smokelib.login(proc, sel, log)
        check_boot_log(log, "pc+ahci")
        run(proc, sel, log, "cat /disk/hello.txt\n", "o from MaeroOS initrd!")  # smoke-disk overwrites the start
        run(proc, sel, log, "diskprobe\n", "diskprobe ok")
        run(proc, sel, log, "fsprobe\n", "fsprobe ok", "fsprobe FAIL")
        run(proc, sel, log, "symprobe\n", "symprobe ok", "symprobe FAIL")
        run(proc, sel, log, "toybox md5sum /disk/ahci-big\n", want)
        run(proc, sel, log, "cp /disk/ahci-big /disk/ahci-copy\n", PROMPT, "cp:", timeout=60.0)
        run(proc, sel, log, "toybox md5sum /disk/ahci-copy\n", want)
        run(proc, sel, log, "printf persisted > /disk/ahci-note\n", PROMPT)
        smokelib.send(proc, "reboot\n")
        wait_exit(proc, sel, log, "reboot")
    finally:
        stop(proc)
    t1 = time.time()

    got = debugfs("-R", "dump /ahci-copy /dev/stdout").stdout
    if len(got) != BIG_BYTES or hashlib.md5(got).hexdigest() != want:
        raise AssertionError(f"host: /ahci-copy is {len(got)} bytes, md5 "
                             f"{hashlib.md5(got).hexdigest()} (want {want})")
    if b"persisted" not in debugfs("-R", "cat /ahci-note").stdout:
        raise AssertionError("host: /ahci-note did not persist")
    check_fsck()

    # ── Boot 2: q35 (built-in ICH9 AHCI) ────────────────────────────────────
    log = []
    proc, sel = boot(["-M", "q35",
                      "-drive", f"file={IMG},format=raw,index=0,media=disk"])
    try:
        smokelib.login(proc, sel, log)
        check_boot_log(log, "q35")
        run(proc, sel, log, "cat /disk/ahci-note\n", "persisted")
        run(proc, sel, log, "toybox md5sum /disk/ahci-copy\n", want)
        run(proc, sel, log, "rm /disk/ahci-copy\n", PROMPT)
        run(proc, sel, log, "rm /disk/ahci-big\n", PROMPT)
        run(proc, sel, log, "cat /disk/ahci-copy\n", "cat: cannot open file")
        smokelib.send(proc, "poweroff\n")
        wait_exit(proc, sel, log, "poweroff")
    finally:
        stop(proc)
    t2 = time.time()

    if "Type: regular" in debugfs("-R", "stat /ahci-copy").stdout.decode():
        raise AssertionError("host: /ahci-copy still exists after rm on q35")
    check_fsck()

    os.unlink(BIG)
    os.unlink(IMG)
    print(f"\n[SMOKE-AHCI] passed (pc+ahci {t1 - t0:.1f}s, q35 {t2 - t1:.1f}s)")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"\n[SMOKE-AHCI] failed: {exc}", file=sys.stderr)
        raise SystemExit(1)
