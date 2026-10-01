#!/usr/bin/env python3
"""smoke-ext2rw: more than one ext2 filesystem mounted read-write at once.

fs/ext2.c serves /disk and any number of mount(2) instances.  This builds
three extra disks in build/ext2rw/ and boots q35 with the boot disk on AHCI
(sda, a snapshot: /disk is only checked from inside):

  sdb      MBR, partition 1: ext2, 1 KiB blocks          -> /dev/sdb1
  nvme0n1  whole disk: ext3 (empty journal, dir_index, an htree directory),
           4 KiB blocks, mounted with -t ext4            -> /dev/nvme0n1
  sdc      the same ext3 with needs_recovery set         -> /dev/sdc

and checks, with busybox:

  * both mount read-write at the same time (ext2 driver; /proc/mounts rw),
    their files read back, statfs reports each filesystem's own size
  * create, write (into double-indirect blocks), append, mkdir, symlink,
    rename within and across directories, unlink, rmdir, a copy and a move
    between the two mounts (EXDEV -> copy), changes in the htree directory
  * the same device a second time is EBUSY; raw /dev writes to it (or its
    disk) are EBUSY; umount with a file open is EBUSY; remount,ro refuses writes and remount,rw allows them again
  * umount, mount again: everything written is there
  * needs_recovery is refused read-write and read-only alike
  * /disk reads and writes as before
  * `poweroff` (ACPI S5), then on the host: e2fsck -fn is clean on both
    filesystems, debugfs sees the guest's changes, sdc is byte-identical
"""
import hashlib
import os
import re
import selectors
import shutil
import struct
import subprocess
import sys
import time

import smokelib

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
OUT = os.path.join(ROOT, "build", "ext2rw")
PROMPT = smokelib.PROMPT
SEQ_N = 300000                     # `seq 1 300000`: ~2 MiB, double indirect

failures = []


def check(cond, what):
    if cond:
        print(f"\n[SMOKE-EXT2RW] ok: {what}")
    else:
        print(f"\n[SMOKE-EXT2RW] FAIL: {what}")
        failures.append(what)


def tool(name):
    for p in (shutil.which(name), f"/usr/sbin/{name}", f"/sbin/{name}"):
        if p and os.path.exists(p):
            return p
    raise SystemExit(f"smoke-ext2rw: {name} not found (install e2fsprogs)")


MKE2FS, DEBUGFS, E2FSCK = tool("mke2fs"), tool("debugfs"), tool("e2fsck")


def run(*cmd):
    return subprocess.run(cmd, check=True, capture_output=True, text=True)


def debugfs(img, req):
    return subprocess.run([DEBUGFS, "-R", req, img], capture_output=True,
                          text=True).stdout


def seq_text(n):
    return "".join(f"{i}\n" for i in range(1, n + 1)).encode()


def md5(b):
    return hashlib.md5(b).hexdigest()


def build_images():
    shutil.rmtree(OUT, ignore_errors=True)
    os.makedirs(OUT)
    files = {}

    # ext2, 1 KiB blocks, in MBR partition 1 (sector 2048, 16 MiB).
    src = os.path.join(OUT, "src-a")
    os.makedirs(os.path.join(src, "dir"))
    os.makedirs(os.path.join(src, "emptydir"))
    a_files = {
        "hello.txt": b"hello from sdb1\n",
        "delete-me.txt": b"short-lived\n",
        "dir/inner.txt": b"inner file\n",
        "blob.bin": bytes((i * 7 + 3) & 0xFF for i in range(700 * 1024)),
    }
    for n, d in a_files.items():
        with open(os.path.join(src, n), "wb") as f:
            f.write(d)
    a_fs = os.path.join(OUT, "a-fs.img")
    run(MKE2FS, "-q", "-F", "-t", "ext2", "-b", "1024", "-L", "ext2a", "-d", src,
        a_fs, "16M")
    a_img = os.path.join(OUT, "a.img")
    with open(a_img, "wb") as out:
        mbr = bytearray(512)
        # status, CHS (unused), type 0x83, CHS, start LBA, sectors
        mbr[446:462] = struct.pack("<B3sB3sII", 0, b"\0\0\0", 0x83, b"\0\0\0",
                                   2048, 16 * 2048)
        mbr[510:512] = b"\x55\xaa"
        out.write(mbr)
        out.write(bytes(2047 * 512))
        with open(a_fs, "rb") as f:
            out.write(f.read())
    files["a"] = {k: md5(v) for k, v in a_files.items()}

    # ext3 (journal, dir_index) with an htree directory, 4 KiB blocks.
    src = os.path.join(OUT, "src-b")
    os.makedirs(os.path.join(src, "big"))
    b_files = {"readme.txt": b"ext3 on nvme\n"}
    for i in range(400):
        b_files[f"big/entry-{i:05d}-with-a-longer-name"] = f"{i}\n".encode()
    for n, d in b_files.items():
        with open(os.path.join(src, n), "wb") as f:
            f.write(d)
    b_img = os.path.join(OUT, "b.img")
    run(MKE2FS, "-q", "-F", "-t", "ext3", "-b", "4096", "-O", "dir_index",
        "-L", "ext3b", "-d", src, b_img, "32M")
    # mke2fs -d writes directories linear; e2fsck -D indexes the big one.
    subprocess.run([E2FSCK, "-fyD", b_img], capture_output=True)
    files["b"] = {"readme.txt": md5(b_files["readme.txt"])}
    files["b_htree"] = "0x1000" in debugfs(b_img, "stat /big").split("Flags:")[1][:12] \
        if "Flags:" in debugfs(b_img, "stat /big") else False

    # The same ext3, marked as needing journal recovery.
    c_img = os.path.join(OUT, "c.img")
    shutil.copyfile(b_img, c_img)
    run(DEBUGFS, "-w", "-R", "feature needs_recovery", c_img)
    with open(c_img, "rb") as f:
        files["c_md5"] = md5(f.read())
    return a_img, a_fs, b_img, c_img, files


class Guest:
    def __init__(self, proc, sel, log):
        self.proc, self.sel, self.log = proc, sel, log

    def sh(self, cmd, timeout=90.0):
        """Run `cmd` under busybox sh; returns (exit status, output)."""
        assert "'" not in cmd
        at = smokelib.mark(self.log)
        smokelib.send(self.proc, f"busybox sh -c '{cmd}; echo @@\"RC\"=$?'\n")
        end = smokelib.wait_for(self.proc, self.sel, "@@RC=", self.log, timeout, at)
        nl = smokelib.wait_for(self.proc, self.sel, "\n", self.log, 10.0, end)
        smokelib.wait_for(self.proc, self.sel, PROMPT, self.log, 10.0, nl)
        text = "".join(self.log)
        rc = int(re.match(r"\d+", text[end:nl]).group(0))
        out = text[at:end - len("@@RC=")].replace("\r", "")
        out = out.split("\n", 1)[1] if "\n" in out else ""   # drop the echo
        out = re.sub(r"\x1b\[[0-9;]*m", "", out)
        out = "\n".join(l for l in out.split("\n")
                        if not re.match(r"\[[A-Z0-9_-]+\]", l))
        return rc, out


def md5_of(g, path):
    rc, out = g.sh(f"busybox md5sum {path}", timeout=120.0)
    m = re.search(r"\b([0-9a-f]{32})\b", out)
    return m.group(1) if rc == 0 and m else None


def guest_tests(g, files):
    seq_md5 = md5(seq_text(SEQ_N))
    boot = "".join(g.log)
    check("[PART] sdb1:" in boot, "sdb1 (MBR partition on AHCI) found")

    # /disk before: a file written there survives everything below.
    rc, out = g.sh("echo disk-mark > /disk/ext2rw-mark && busybox cat /disk/ext2rw-mark")
    check(rc == 0 and "disk-mark" in out, "/disk writable before")
    disk_md5 = md5_of(g, "/disk/busybox")

    g.sh("busybox mkdir -p /mnt/a /mnt/b /mnt/c")
    rc, out = g.sh("busybox mount -t ext2 /dev/sdb1 /mnt/a")
    check(rc == 0, f"mount -t ext2 /dev/sdb1 /mnt/a ({out.strip()!r})")
    rc, out = g.sh("busybox mount -t ext4 /dev/nvme0n1 /mnt/b")
    check(rc == 0, f"mount -t ext4 /dev/nvme0n1 /mnt/b: ext3 rw via ext2 ({out.strip()!r})")
    rc, out = g.sh("busybox cat /proc/mounts")
    check("/dev/sdb1 /mnt/a ext2 rw" in out, "/proc/mounts: /dev/sdb1 /mnt/a ext2 rw")
    check("/dev/nvme0n1 /mnt/b ext4 rw" in out, "/proc/mounts: /dev/nvme0n1 /mnt/b ext4 rw")
    check("/dev/sda /disk ext2 rw" in out, "/proc/mounts: /disk still listed")

    for n, h in files["a"].items():
        check(md5_of(g, f"/mnt/a/{n}") == h, f"sdb1 {n} reads back")
    check(md5_of(g, "/mnt/b/readme.txt") == files["b"]["readme.txt"], "nvme0n1 readme.txt reads back")
    rc, out = g.sh("busybox ls /mnt/b/big | busybox wc -l")
    check(out.strip().endswith("400"), f"htree dir lists 400 entries ({out.strip()!r})")

    rc, out = g.sh("busybox df -k /mnt/a /mnt/b /disk")
    sizes = [int(m) for m in re.findall(r"^\S+\s+(\d+)\s+\d+\s+\d+", out, re.M)]
    check(len(sizes) == 3 and 15000 < sizes[0] < 16500 and 30000 < sizes[1] < 33000
          and sizes[2] > 100000, f"statfs: each mount reports its own size {sizes}")

    # ── Writes on both at once ───────────────────────────────────────────
    rc, out = g.sh(f"busybox seq 1 {SEQ_N} > /mnt/a/seq.txt && "
                   "echo new file > /mnt/a/new.txt && echo more >> /mnt/a/new.txt && "
                   "busybox mkdir /mnt/a/newdir && busybox mkdir /mnt/a/newdir/sub && "
                   "busybox mv /mnt/a/hello.txt /mnt/a/dir/hello2.txt && "
                   "busybox ln -s ../dir/hello2.txt /mnt/a/newdir/link && "
                   "busybox mv /mnt/a/dir/inner.txt /mnt/a/dir/inner-renamed.txt && "
                   "busybox rm /mnt/a/delete-me.txt && busybox rmdir /mnt/a/emptydir",
                   timeout=180.0)
    check(rc == 0, f"writes on /mnt/a ({out.strip()[-200:]!r})")
    rc, out = g.sh("echo b-new > /mnt/b/big/zz-new && busybox rm /mnt/b/big/entry-00007-with-a-longer-name && "
                   "busybox mv /mnt/b/big/entry-00010-with-a-longer-name /mnt/b/big/renamed && "
                   "busybox cp /mnt/a/seq.txt /mnt/b/seq-copy.txt && "
                   "echo moving > /mnt/a/to-move.txt && busybox mv /mnt/a/to-move.txt /mnt/b/moved.txt",
                   timeout=180.0)
    check(rc == 0, f"writes on /mnt/b, copy and move across mounts ({out.strip()[-200:]!r})")
    check(md5_of(g, "/mnt/a/seq.txt") == seq_md5, "seq.txt (double indirect) reads back")
    check(md5_of(g, "/mnt/b/seq-copy.txt") == seq_md5, "the copy on /mnt/b reads back")
    rc, out = g.sh("busybox ls /mnt/a; busybox cat /mnt/a/newdir/link /mnt/b/moved.txt")
    check("to-move.txt" not in out and "hello from sdb1" in out and "moving" in out,
          "symlink resolves; moved file gone from /mnt/a, present on /mnt/b")
    rc, out = g.sh("busybox ls /mnt/b/big | busybox wc -l; busybox cat /mnt/b/big/renamed")
    check("400" in out.split() and "10" in out.split(), "htree dir after create/unlink/rename")
    rc, out = g.sh("busybox ln /mnt/a/new.txt /mnt/b/hardlink")
    check(rc != 0, "hard link across the two mounts refused")

    # ── EBUSY cases, remount ─────────────────────────────────────────────
    rc, out = g.sh("busybox mount -t ext2 /dev/sdb1 /mnt/c")
    check(rc != 0 and "busy" in out.lower(), f"same device twice: EBUSY ({out.strip()!r})")
    rc, out = g.sh("busybox dd if=/dev/zero of=/dev/sdb1 bs=512 count=1 seek=32700 conv=notrunc")
    check(rc != 0 and "busy" in out.lower(), f"raw write to the mounted /dev/sdb1: EBUSY ({out.strip()!r})")
    rc, out = g.sh("busybox dd if=/dev/zero of=/dev/sdb bs=512 count=1 seek=34748 conv=notrunc")
    check(rc != 0 and "busy" in out.lower(), f"raw write to its disk /dev/sdb: EBUSY ({out.strip()!r})")
    rc, out = g.sh("exec 3</mnt/a/seq.txt; busybox umount /mnt/a")
    check(rc != 0 and "busy" in out.lower(), f"umount with an open file: EBUSY ({out.strip()!r})")
    # Lazy umount with a file open for writing: the instance lives on behind
    # the descriptor, so the device stays taken until it closes.
    rc, out = g.sh("exec 3>>/mnt/a/lazy.txt; echo before >&3; "
                   "busybox umount -l /mnt/a && echo DETACHED; "
                   "busybox mount -t ext2 /dev/sdb1 /mnt/a; echo second=$?; "
                   "busybox dd if=/dev/zero of=/dev/sdb1 bs=512 count=1 seek=32700 conv=notrunc; echo raw=$?; "
                   "echo after-detach >&3; exec 3>&-; "
                   "busybox mount -t ext2 /dev/sdb1 /mnt/a && echo REMOUNTED; "
                   "busybox cat /mnt/a/lazy.txt")
    check("DETACHED" in out, f"umount -l with a file open for writing ({out.strip()[-300:]!r})")
    check("second=0" not in out and "busy" in out.lower(),
          "after umount -l, a second mount of the still-open device is EBUSY")
    check("raw=0" not in out, "after umount -l, raw /dev writes are still EBUSY")
    check("REMOUNTED" in out and "before\nafter-detach" in out,
          "once the last descriptor closes the device mounts again, with the data written through it")
    rc, out = g.sh("busybox mount -o remount,ro /mnt/b && echo x > /mnt/b/ro-test")
    check(rc != 0 and "Read-only" in out, f"remount,ro refuses writes ({out.strip()!r})")
    rc, out = g.sh("busybox grep nvme0n1 /proc/mounts")
    check("/mnt/b ext4 ro" in out, "/proc/mounts shows /mnt/b ro")
    rc, out = g.sh("busybox mount -o remount,rw /mnt/b && echo rw-again > /mnt/b/rw-again")
    check(rc == 0, f"remount,rw writes again ({out.strip()!r})")

    # ── umount, mount again: the data is on the disk ─────────────────────
    rc, out = g.sh("busybox umount /mnt/a && busybox umount /mnt/b")
    check(rc == 0, f"umount both ({out.strip()!r})")
    rc, out = g.sh("busybox cat /proc/mounts")
    check("/mnt/a" not in out and "/mnt/b" not in out, "/proc/mounts no longer lists them")
    rc, out = g.sh("busybox mount -t ext2 /dev/sdb1 /mnt/a && busybox mount -t ext3 /dev/nvme0n1 /mnt/b")
    check(rc == 0, "mount both again")
    check(md5_of(g, "/mnt/a/seq.txt") == seq_md5 and md5_of(g, "/mnt/b/seq-copy.txt") == seq_md5,
          "seq.txt and its copy survive umount/mount")
    rc, out = g.sh("busybox cat /mnt/a/new.txt /mnt/a/dir/hello2.txt /mnt/b/rw-again")
    check("new file\nmore" in out and "hello from sdb1" in out and "rw-again" in out,
          "small files survive umount/mount")
    rc, out = g.sh("busybox umount /mnt/a && busybox umount /mnt/b")
    check(rc == 0, "umount both again")

    # ── read-only mount through the ext2 driver ─────────────────────────
    rc, out = g.sh("busybox mount -t ext2 -o ro /dev/sdb1 /mnt/a && busybox cat /mnt/a/new.txt && "
                   "echo y > /mnt/a/y")
    check(rc != 0 and "new file" in out and "Read-only" in out, "mount -o ro: reads, refuses writes")
    rc, out = g.sh("busybox umount /mnt/a")
    check(rc == 0, "umount the read-only mount")

    # ── needs_recovery ───────────────────────────────────────────────────
    rc, out = g.sh("busybox mount -t ext3 /dev/sdc /mnt/c")
    check(rc != 0, f"needs_recovery: read-write mount refused ({out.strip()!r})")
    rc, out = g.sh("busybox mount -t ext2 -o ro /dev/sdc /mnt/c")
    check(rc != 0, f"needs_recovery: read-only mount refused too ({out.strip()!r})")

    # ── /disk after ──────────────────────────────────────────────────────
    rc, out = g.sh("busybox cat /disk/ext2rw-mark && echo again >> /disk/ext2rw-mark && "
                   "busybox cat /disk/ext2rw-mark && busybox rm /disk/ext2rw-mark")
    check(rc == 0 and "disk-mark\nagain" in out, "/disk reads and writes after")
    check(disk_md5 is not None and md5_of(g, "/disk/busybox") == disk_md5, "/disk/busybox unchanged")

    # ── left mounted read-write for poweroff ─────────────────────────────
    # vfs_mounts_shutdown must take both read-only (superblocks clean) when
    # init powers off with them still mounted; the host checks below.
    rc, out = g.sh("busybox mount -t ext2 /dev/sdb1 /mnt/a && busybox mount -t ext3 /dev/nvme0n1 /mnt/b && "
                   "echo at-poweroff > /mnt/a/at-poweroff.txt && echo at-poweroff > /mnt/b/at-poweroff.txt && "
                   "busybox grep -E \" /mnt/(a|b) \" /proc/mounts")
    check(rc == 0 and "/mnt/a ext2 rw" in out and "/mnt/b ext3 rw" in out,
          f"sdb1 and nvme0n1 mounted read-write and written, left mounted for poweroff ({out.strip()!r})")


def host_checks(a_img, a_fs, b_img, c_img, files):
    seq_md5 = md5(seq_text(SEQ_N))
    # The partition out of disk a, for the e2fsprogs tools.
    part = os.path.join(OUT, "a-part.img")
    with open(a_img, "rb") as f, open(part, "wb") as o:
        f.seek(2048 * 512)
        o.write(f.read(16 * 1024 * 1024))
    for name, img in (("sdb1", part), ("nvme0n1", b_img)):
        r = subprocess.run([E2FSCK, "-fn", img], capture_output=True, text=True)
        check(r.returncode == 0, f"host e2fsck -fn {name} clean (rc={r.returncode})"
              + ("" if r.returncode == 0 else "\n" + r.stdout[-1500:]))
    root = debugfs(part, "ls -l /")
    check("seq.txt" in root and "new.txt" in root and "newdir" in root and
          "hello.txt" not in root and "delete-me.txt" not in root and
          "emptydir" not in root and "to-move.txt" not in root,
          "debugfs: sdb1 root has the guest's creates and deletes")
    d = debugfs(part, "ls /dir")
    check("hello2.txt" in d and "inner-renamed.txt" in d and not re.search(r"\binner\.txt\b", d),
          "debugfs: renames in sdb1 /dir")
    check(md5(subprocess.run([DEBUGFS, "-R", "cat /seq.txt", part], capture_output=True).stdout)
          == seq_md5, "debugfs: sdb1 seq.txt content")
    check("../dir/hello2.txt" in debugfs(part, "stat /newdir/link"), "debugfs: symlink target")
    big = debugfs(b_img, "ls /big")
    check("zz-new" in big and "renamed" in big and "entry-00007-" not in big,
          "debugfs: nvme0n1 htree dir changes")
    check(subprocess.run([DEBUGFS, "-R", "cat /lazy.txt", part], capture_output=True,
                         text=True).stdout == "before\nafter-detach\n",
          "debugfs: sdb1 lazy.txt, written after umount -l")
    check(md5(subprocess.run([DEBUGFS, "-R", "cat /seq-copy.txt", b_img],
                             capture_output=True).stdout) == seq_md5,
          "debugfs: nvme0n1 seq-copy.txt content")
    check("moving" in subprocess.run([DEBUGFS, "-R", "cat /moved.txt", b_img],
                                     capture_output=True, text=True).stdout,
          "debugfs: nvme0n1 moved.txt")
    for name, img in (("sdb1", part), ("nvme0n1", b_img)):
        st = subprocess.run([tool("dumpe2fs"), "-h", img], capture_output=True, text=True).stdout
        check(re.search(r"Filesystem state:\s+clean", st) is not None,
              f"{name} superblock state clean after poweroff with it mounted read-write")
    with open(c_img, "rb") as f:
        check(md5(f.read()) == files["c_md5"], "sdc (needs_recovery) byte-identical")
    for name, img in (("sdb1", part), ("nvme0n1", b_img)):
        check(subprocess.run([DEBUGFS, "-R", "cat /at-poweroff.txt", img], capture_output=True,
                             text=True).stdout == "at-poweroff\n",
              f"debugfs: {name} at-poweroff.txt, written just before poweroff")


def main():
    a_img, a_fs, b_img, c_img, files = build_images()
    check(files["b_htree"], "host: nvme0n1 /big is an htree directory (e2fsck -D)")
    accel = ["-accel", "kvm"] if os.access("/dev/kvm", os.R_OK | os.W_OK) else ["-accel", "tcg"]
    proc = subprocess.Popen(
        ["qemu-system-i386", *smokelib.QEMU_DISPLAY, *accel, "-M", "q35",
         "-kernel", "kernel.elf", "-initrd", "initrd.tar",
         "-drive", "file=disk.img,format=raw,index=0,media=disk,snapshot=on",
         "-drive", f"file={a_img},format=raw,index=1,media=disk",
         "-drive", f"file={c_img},format=raw,index=2,media=disk",
         "-drive", f"file={b_img},format=raw,if=none,id=nv",
         "-device", "nvme,serial=ext2rw,drive=nv",
         "-serial", "stdio", "-m", "256M", "-no-reboot"],
        cwd=ROOT, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, bufsize=0)
    sel = selectors.DefaultSelector()
    sel.register(proc.stdout, selectors.EVENT_READ)
    log = []
    g = Guest(proc, sel, log)
    try:
        smokelib.login(proc, sel, log, timeout=120.0)
        guest_tests(g, files)

        # poweroff: ACPI S5, and QEMU (no -no-shutdown) exits.
        at_poweroff = len("".join(log))
        smokelib.send(proc, "poweroff\n")
        deadline = time.time() + 60
        while proc.poll() is None and time.time() < deadline:
            for k, _ in sel.select(0.2):
                chunk = os.read(k.fd, 4096).decode("latin1", "replace")
                if chunk:
                    log.append(chunk)
                    sys.stdout.write(chunk)
        check(proc.poll() is not None, "QEMU exited after poweroff")
        text = "".join(log)
        check("not cleanly unmounted" not in text, "no unclean-mount warnings")
        check(len(re.findall(r"\[EXT2\]\s+\S+: now read-only", text[at_poweroff:])) >= 2,
              "poweroff took both read-write ext2 mounts read-only")
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()

    host_checks(a_img, a_fs, b_img, c_img, files)

    if failures:
        print(f"\n[SMOKE-EXT2RW] {len(failures)} check(s) failed:")
        for f in failures:
            print(f"  - {f}")
        return 1
    print("\n[SMOKE-EXT2RW] passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"\n[SMOKE-EXT2RW] failed: {exc}", file=sys.stderr)
        raise SystemExit(1)
