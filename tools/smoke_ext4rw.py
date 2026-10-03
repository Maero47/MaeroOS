#!/usr/bin/env python3
"""smoke-ext4rw: ext4 read-write through fs/ext2.c (extents, metadata_csum,
64bit, flex_bg, uninitialised groups) with and without a jbd2 journal.

Builds disks in build/ext4rw/ with the host's mke2fs and boots q35 with the
boot disk on AHCI (sda, a snapshot):

  sdb      mkfs.ext4 defaults (has_journal, extent, 64bit, flex_bg,
           metadata_csum(+seed), huge_file, dir_index, extra_isize,
           orphan_file), 4 KiB blocks, with a 1200-entry htree directory
           made by e2fsck -D                                   -> /dev/sdb
  nvme0n1  mkfs.ext4 -O ^has_journal, 1 KiB blocks             -> /dev/nvme0n1
  sdc      mkfs.ext4 defaults, journal switched to checksum v3, and one
           transaction written into it with debugfs (jo/jw/jc) that rewrites
           hello.txt's data block: needs_recovery               -> /dev/sdc
  sdd      like sdb, small, mounted with -o x4crash: the driver stops after
           the first commit without writing anything in place, as if the
           power had gone                                       -> /dev/sdd

In the guest, with busybox, on both sdb and nvme0n1: create files and
directories, a file of more than four extents (depth-1 tree) and a sparse
one, a hole filled in the middle, a 1000-entry directory, renames (in and
across directories, over an existing name), deletes, truncates (shrink and
grow), a 50 MiB file (on sdb), changes in the htree directories (leaf
splits on sdb, a new index level on nvme0n1's 1 KiB blocks); umount and
mount again and everything reads back.  sdc: -o ro is refused, a read-write
mount replays the journal (hello.txt has the logged contents).  Then
`poweroff` with sdb and nvme0n1 still mounted read-write.

On the host: e2fsck -fn clean (no checksum errors) on all, debugfs and md5
see the data, the superblocks are clean without needs_recovery; sdd's
journal holds the guest's transaction and e2fsck replays it (e2fsprogs
accepts the descriptor, tag and commit checksums written here); a copy of it
is booted again and this driver replays its own log at mount.

Directory index and orphans:

  nvme0n2  mkfs.ext4 defaults, 40000 inodes: a new directory grows to 5000
           names and becomes indexed (debugfs htree, e2fsck -fn); a 10000-name
           one is created and looked up with `fsprobe mkfiles/lookups`
           (timings printed); then a truncate of a fragmented file loses
           power between two of its commits (-o x4crashtrunc)
  nvme0n3  the same without dir_index: the 10000-name directory stays linear,
           for comparison
  sdd      a file held open and unlinked before the power loss: an orphan in
           the orphan file
  sde      made without orphan_file, so the half-freed file of the
           x4crashunlink case is on the old s_last_orphan list

A second boot mounts copies of sdd, sde and nvme0n2 read-write: the driver
replays each journal and then finishes the orphans (deletes the unlinked
files, cuts the truncated one to its new size); e2fsck -fn is clean after.
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
OUT = os.path.join(ROOT, "build", "ext4rw")
PROMPT = smokelib.PROMPT
BIG = 50 * 1024 * 1024
BIG_LINE = b"ext4rw big file line 0123456789abcdef\n"

failures = []


def check(cond, what):
    if cond:
        print(f"\n[SMOKE-EXT4RW] ok: {what}")
    else:
        print(f"\n[SMOKE-EXT4RW] FAIL: {what}")
        failures.append(what)


def tool(name):
    for p in (shutil.which(name), f"/usr/sbin/{name}", f"/sbin/{name}"):
        if p and os.path.exists(p):
            return p
    raise SystemExit(f"smoke-ext4rw: {name} not found (install e2fsprogs)")


MKE2FS, DEBUGFS, E2FSCK, DUMPE2FS = (tool("mke2fs"), tool("debugfs"), tool("e2fsck"),
                                     tool("dumpe2fs"))


def run(*cmd):
    return subprocess.run(cmd, check=True, capture_output=True, text=True)


def debugfs(img, req):
    return subprocess.run([DEBUGFS, "-R", req, img], capture_output=True,
                          text=True).stdout


def live_names(img, path):
    """Names in a directory as debugfs lists them, minus the inode-0 entries
    `ls` also prints (a deleted entry that was first in its block keeps its
    name with inode 0, in ext4 as here)."""
    out = []
    for line in debugfs(img, f"ls -l {path}").splitlines():
        f = line.split()
        if len(f) >= 2 and f[0].isdigit() and f[0] != "0":
            out.append(f[-1])
    return out


def debugfs_cat(img, path):
    return subprocess.run([DEBUGFS, "-R", f"cat {path}", img], capture_output=True).stdout


def md5(b):
    return hashlib.md5(b).hexdigest()


def big_bytes():
    """50 copies of a 1 MiB pattern (the guest cats it from tmpfs, so the
    file is written in large chunks rather than yes(1)'s 38-byte lines)."""
    mib = (BIG_LINE * (1048576 // len(BIG_LINE) + 1))[:1048576]
    return mib * (BIG // 1048576)


def trunc_bytes():
    return "".join(f"{i}\n" for i in range(1, 50001)).encode()[:5000] + bytes(3000)


def frag_bytes():
    """What the guest's frag.bin holds: 4 KiB blocks 2*i (i = 0..9) written
    with byte 'A'+i, holes between, then block 5 filled with 'z'."""
    b = bytearray(19 * 4096)
    for i in range(10):
        b[2 * i * 4096:(2 * i + 1) * 4096] = bytes([65 + i]) * 4096
    b[5 * 4096:6 * 4096] = b"z" * 4096
    return bytes(b)


T32C = []
for _i in range(256):
    _c = _i
    for _k in range(8):
        _c = (_c >> 1) ^ (0x82F63B78 if _c & 1 else 0)
    T32C.append(_c)


def crc32c(crc, data):
    for b in data:
        crc = T32C[(crc ^ b) & 0xFF] ^ (crc >> 8)
    return crc


def journal_csum_v3(img):
    """What Linux does at the first mount of a metadata_csum ext4: turn on
    jbd2 checksum v3 in the journal superblock."""
    blk = int(debugfs(img, "bmap <8> 0").split()[0])
    with open(img, "r+b") as f:
        f.seek(blk * 4096)
        j = bytearray(f.read(1024))
        inc = struct.unpack(">I", j[0x28:0x2C])[0] | 0x10
        j[0x28:0x2C] = struct.pack(">I", inc)
        j[0x50] = 4
        j[0xFC:0x100] = b"\0" * 4
        j[0xFC:0x100] = struct.pack(">I", crc32c(0xFFFFFFFF, bytes(j)))
        f.seek(blk * 4096)
        f.write(j)


def htree_levels(img, path):
    """Indirect levels of an htree directory, None if it has no index."""
    m = re.search(r"Indirect levels:\s*(\d+)", debugfs(img, f"htree {path}"))
    return int(m.group(1)) if m else None


def torn_revoke(img, fsblk):
    """After debugfs's transaction 1 (descriptor, data, commit at journal
    blocks 1-3) append a revoke block of transaction 2 for the same block,
    with no commit block after it: a torn transaction whose revoke must not
    cancel the committed copy (jbd2 only honours committed revokes)."""
    jblk0 = int(debugfs(img, "bmap <8> 0").split()[0])
    jblk4 = int(debugfs(img, "bmap <8> 4").split()[0])
    with open(img, "r+b") as f:
        f.seek(jblk0 * 4096)
        jsb = f.read(1024)
        uuid = jsb[0x30:0x40]
        r = bytearray(4096)
        r[0:12] = struct.pack(">III", 0xC03B3998, 5, 2)
        r[12:16] = struct.pack(">I", 16 + 8)            # 64bit journal: 8-byte records
        r[16:24] = struct.pack(">Q", fsblk)
        seed = crc32c(0xFFFFFFFF, uuid)
        r[4092:4096] = struct.pack(">I", crc32c(seed, bytes(r)))
        f.seek(jblk4 * 4096)
        f.write(r)


def build_images():
    shutil.rmtree(OUT, ignore_errors=True)
    os.makedirs(OUT)
    info = {}

    src = os.path.join(OUT, "src-a")
    os.makedirs(os.path.join(src, "big"))
    os.makedirs(os.path.join(src, "emptydir"))
    a_files = {
        "hello.txt": b"hello from ext4 sdb\n",
        "delete-me.txt": b"short-lived\n",
        "blob.bin": bytes((i * 7 + 3) & 0xFF for i in range(300 * 1024)),
    }
    for i in range(1200):
        a_files[f"big/entry-{i:05d}-with-a-longer-name"] = f"{i}\n".encode()
    for n, d in a_files.items():
        with open(os.path.join(src, n), "wb") as f:
            f.write(d)
    a_img = os.path.join(OUT, "a.img")
    run(MKE2FS, "-q", "-F", "-t", "ext4", "-L", "ext4a", "-d", src, a_img, "320M")
    subprocess.run([E2FSCK, "-fyD", a_img], capture_output=True)
    info["a_htree"] = htree_levels(a_img, "/big") == 0
    info["a"] = {k: md5(v) for k, v in a_files.items() if "/" not in k}

    src = os.path.join(OUT, "src-b")
    os.makedirs(os.path.join(src, "dir"))
    os.makedirs(os.path.join(src, "big2"))
    b_files = {"hello.txt": b"hello from ext4 nvme, no journal\n",
               "dir/inner.txt": b"inner\n",
               "delete-me.txt": b"bye\n"}
    for n, d in b_files.items():
        with open(os.path.join(src, n), "wb") as f:
            f.write(d)
    for i in range(1500):
        open(os.path.join(src, "big2", f"entry-{i:05d}-with-a-much-longer-name-for-1k"), "wb").close()
    b_img = os.path.join(OUT, "b.img")
    run(MKE2FS, "-q", "-F", "-t", "ext4", "-O", "^has_journal", "-b", "1024",
        "-L", "ext4b", "-d", src, b_img, "96M")
    subprocess.run([E2FSCK, "-fyD", b_img], capture_output=True)
    info["b"] = {k: md5(v) for k, v in b_files.items()}
    info["b_levels"] = htree_levels(b_img, "/big2")

    src = os.path.join(OUT, "src-c")
    os.makedirs(src)
    with open(os.path.join(src, "hello.txt"), "wb") as f:
        f.write(b"original content\n")
    c_img = os.path.join(OUT, "c.img")
    run(MKE2FS, "-q", "-F", "-t", "ext4", "-L", "ext4c", "-d", src, c_img, "64M")
    journal_csum_v3(c_img)
    blk = int(debugfs(c_img, "bmap /hello.txt 0").split()[0])
    newblk = os.path.join(OUT, "c-new.bin")
    with open(newblk, "wb") as f:
        f.write(b"replayed content\n" + bytes(4096 - 17))
    cmds = os.path.join(OUT, "c-cmds")
    with open(cmds, "w") as f:
        f.write(f"jo\njw -b {blk} {newblk}\njc\n")
    run(DEBUGFS, "-w", "-f", cmds, c_img)
    torn_revoke(c_img, blk)
    info["c_dirty"] = "needs_recovery" in subprocess.run(
        [DUMPE2FS, "-h", c_img], capture_output=True, text=True).stdout

    d_img = os.path.join(OUT, "d.img")
    src = os.path.join(OUT, "src-d")
    os.makedirs(src)
    with open(os.path.join(src, "keep.txt"), "wb") as f:
        f.write(b"kept\n")
    with open(os.path.join(src, "held.bin"), "wb") as f:
        f.write(bytes((i * 13) & 0xFF for i in range(256 * 1024)))
    run(MKE2FS, "-q", "-F", "-t", "ext4", "-L", "ext4d", "-d", src, d_img, "64M")
    journal_csum_v3(d_img)
    info["d_held_ino"] = int(re.search(r"Inode:\s*(\d+)", debugfs(d_img, "stat /held.bin")).group(1))

    e_img = os.path.join(OUT, "e.img")
    run(MKE2FS, "-q", "-F", "-t", "ext4", "-O", "^orphan_file", "-L", "ext4e", e_img, "64M")
    journal_csum_v3(e_img)
    info["e_img"] = e_img

    g_img = os.path.join(OUT, "g.img")
    run(MKE2FS, "-q", "-F", "-t", "ext4", "-N", "40000", "-L", "ext4g", g_img, "256M")
    journal_csum_v3(g_img)
    info["g_img"] = g_img
    h_img = os.path.join(OUT, "h.img")
    run(MKE2FS, "-q", "-F", "-t", "ext4", "-O", "^dir_index", "-N", "40000", "-L", "ext4h", h_img, "256M")
    info["h_img"] = h_img

    # 128-byte group descriptors: readable, never written by fs/ext2.c.
    f_img = os.path.join(OUT, "f.img")
    run(MKE2FS, "-q", "-F", "-t", "ext4", "-E", "desc_size=128", "-L", "ext4f", f_img, "32M")
    info["f_img"] = f_img
    with open(f_img, "rb") as f:
        info["f_md5"] = md5(f.read())
    return a_img, b_img, c_img, d_img, info


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
    rc, out = g.sh(f"busybox md5sum {path}", timeout=180.0)
    m = re.search(r"\b([0-9a-f]{32})\b", out)
    return m.group(1) if rc == 0 and m else None


def common_ops(g, m, name):
    """Creates, extents, sparse, renames, deletes, truncates on mount `m`."""
    rc, out = g.sh(
        f"echo new file > {m}/new.txt && echo more >> {m}/new.txt && "
        f"busybox mkdir {m}/d1 && busybox mkdir {m}/d1/sub && "
        f"echo moved > {m}/d1/tomove.txt && busybox mv {m}/d1/tomove.txt {m}/d1/sub/moved.txt && "
        f"busybox mv {m}/hello.txt {m}/hello-renamed.txt && "
        f"echo replacement > {m}/repl.txt && echo victim > {m}/victim.txt", timeout=120.0)
    check(rc == 0, f"{name}: create/mkdir/rename ({out.strip()[-300:]!r})")
    rc, out = g.sh(
        f"busybox mv {m}/repl.txt {m}/victim.txt && "
        f"busybox rm {m}/delete-me.txt", timeout=120.0)
    check(rc == 0, f"{name}: create/mkdir/rename/delete ({out.strip()[-300:]!r})")
    rc, out = g.sh(f"busybox ln -s hello-renamed.txt {m}/link && busybox ln -s "
                   f"/a/rather/long/target/path/that/does/not/fit/into/the/sixty/bytes/of/i_block "
                   f"{m}/slowlink")
    check(rc == 0, f"{name}: fast and slow symlink ({out.strip()[-300:]!r})")
    # frag.bin: ten separate one-block extents with holes between (a depth-1
    # extent tree), then a hole filled in the middle.
    rc, out = g.sh(
        f"i=0; for c in A B C D E F G H I J; do "
        f"busybox printf \"%4096s\" \"\" | busybox tr \" \" $c | "
        f"busybox dd of={m}/frag.bin bs=4096 seek=$((2*i)) conv=notrunc 2>/dev/null; i=$((i+1)); done; "
        f"busybox printf \"%4096s\" \"\" | busybox tr \" \" z | "
        f"busybox dd of={m}/frag.bin bs=4096 seek=5 conv=notrunc 2>/dev/null; "
        f"busybox ls -l {m}/frag.bin", timeout=120.0)
    check(rc == 0 and "77824" in out, f"{name}: frag.bin written ({out.strip()[-200:]!r})")
    check(md5_of(g, f"{m}/frag.bin") == md5(frag_bytes()), f"{name}: frag.bin reads back")
    rc, out = g.sh(
        f"echo tail > {m}/sparse.bin && busybox dd if=/dev/zero of={m}/sparse.bin bs=4096 "
        f"seek=2560 count=1 conv=notrunc 2>/dev/null && busybox ls -l {m}/sparse.bin")
    check(rc == 0 and "10489856" in out, f"{name}: sparse 10 MiB file ({out.strip()[-200:]!r})")
    rc, out = g.sh(f"cd {m}/d1 && busybox mkdir many && cd many && "
                   f"for i in $(busybox seq 1 1000); do : > f$i; done && busybox ls | busybox wc -l",
                   timeout=300.0)
    check(rc == 0 and out.strip().endswith("1000"), f"{name}: 1000-entry directory ({out.strip()[-100:]!r})")
    rc, out = g.sh(f"busybox rm {m}/d1/many/f500 {m}/d1/many/f999 && "
                   f"busybox mv {m}/d1/many/f1 {m}/d1/many/first && busybox ls {m}/d1/many | busybox wc -l")
    check(rc == 0 and out.strip().endswith("998"), f"{name}: deletes/renames in it ({out.strip()!r})")
    # truncate: trunc.txt down to 5000 bytes and back up to 8000 (the cut
    # block's tail and the rest read as zeroes), new.txt up to 100000 (a hole)
    rc, out = g.sh(f"busybox seq 1 50000 > {m}/trunc.txt && busybox truncate -s 5000 {m}/trunc.txt && "
                   f"busybox truncate -s 8000 {m}/trunc.txt && "
                   f"busybox truncate -s 100000 {m}/new.txt && busybox ls -l {m}/trunc.txt {m}/new.txt")
    check(rc == 0 and "8000" in out and "100000" in out, f"{name}: truncate shrink/grow ({out.strip()!r})")
    check(md5_of(g, f"{m}/trunc.txt") == md5(trunc_bytes()), f"{name}: truncated file reads back")
    rc, out = g.sh(f"busybox mkdir {m}/gone && busybox rmdir {m}/gone && echo x > {m}/gone2 && "
                   f"busybox rm {m}/gone2")
    check(rc == 0, f"{name}: rmdir and unlink")


def guest_tests(g, info):
    g.sh("busybox mkdir -p /mnt/a /mnt/b /mnt/c /mnt/d /mnt/e")
    rc, out = g.sh("busybox mount -t ext4 /dev/sdb /mnt/a")
    check(rc == 0, f"mount -t ext4 /dev/sdb /mnt/a ({out.strip()!r})")
    rc, out = g.sh("busybox mount -t ext4 /dev/nvme0n1 /mnt/b")
    check(rc == 0, f"mount -t ext4 /dev/nvme0n1 /mnt/b ({out.strip()!r})")
    rc, out = g.sh("busybox cat /proc/mounts")
    check("/dev/sdb /mnt/a ext4 rw" in out, "/proc/mounts: /mnt/a ext4 rw (journaled)")
    check("/dev/nvme0n1 /mnt/b ext4 rw" in out, "/proc/mounts: /mnt/b ext4 rw (no journal)")
    # Two instances of the one ext2/ext4 driver: rename(2) and link(2)
    # between them are EXDEV (fs/vfs.c vfs_path_instance), never run on the
    # source's volume with the target's directory.
    rc, out = g.sh("echo xdev > /mnt/a/xdev.txt && fsprobe rename /mnt/a/xdev.txt /mnt/b/xdev.txt; "
                   "busybox ln /mnt/a/xdev.txt /mnt/b/xdev-link 2>&1; "
                   "busybox cat /mnt/a/xdev.txt; busybox ls /mnt/b/xdev.txt /mnt/b/xdev-link 2>&1; "
                   "busybox rm /mnt/a/xdev.txt")
    check("rename: -18" in out and re.search(r"^xdev\r?$", out, re.M) and
          out.count("No such file") >= 2,
          f"rename and link from /mnt/a to /mnt/b are EXDEV, the file stays ({out.strip()[-300:]!r})")
    boot = "".join(g.log)
    check(re.search(r"sdb: mounted read-write \(block 4096.*journal, extents, metadata_csum, 64bit", boot)
          is not None, "sdb mounted by the ext2 driver with journal, extents, metadata_csum, 64bit")

    for n, h in info["a"].items():
        check(md5_of(g, f"/mnt/a/{n}") == h, f"sdb {n} reads back")
    for n, h in info["b"].items():
        check(md5_of(g, f"/mnt/b/{n}") == h, f"nvme0n1 {n} reads back")

    common_ops(g, "/mnt/a", "sdb")
    common_ops(g, "/mnt/b", "nvme0n1")

    t0 = time.time()
    rc, out = g.sh(f"busybox yes \"{BIG_LINE.decode().strip()}\" | busybox head -c 1048576 > /tmp/pat1m && "
                   ": > /mnt/a/big50.bin && "
                   f"for i in $(busybox seq 1 {BIG // 1048576}); do busybox cat /tmp/pat1m >> /mnt/a/big50.bin; done "
                   "&& busybox sync && busybox ls -l /mnt/a/big50.bin", timeout=600.0)
    print(f"\n[SMOKE-EXT4RW] 50 MiB written and synced in {time.time() - t0:.1f}s")
    check(rc == 0 and str(BIG) in out, f"sdb: 50 MiB file written ({out.strip()[-200:]!r})")
    big_md5 = md5(big_bytes())
    check(md5_of(g, "/mnt/a/big50.bin") == big_md5, "sdb: 50 MiB file reads back")
    rc, out = g.sh("echo b-new > /mnt/a/big/zz-new && busybox rm /mnt/a/big/entry-00007-with-a-longer-name && "
                   "busybox mv /mnt/a/big/entry-00010-with-a-longer-name /mnt/a/big/renamed && "
                   "busybox ls /mnt/a/big | busybox wc -l && busybox cat /mnt/a/big/renamed")
    check(rc == 0 and "1200" in out.split() and "10" in out.split(), f"sdb htree dir changed ({out.strip()!r})")
    # 600 more names: leaves split, the index stays
    rc, out = g.sh("cd /mnt/a/big && for i in $(busybox seq 1 600); do "
                   ": > added-$i-with-a-name-long-enough-to-fill-leaves; done && busybox ls | busybox wc -l",
                   timeout=300.0)
    check(rc == 0 and out.strip().endswith("1800"), f"sdb: 600 names added to the htree dir ({out.strip()[-80:]!r})")
    # 2000 into a 1 KiB-block htree: the root fills and a second level appears
    rc, out = g.sh("cd /mnt/b/big2 && for i in $(busybox seq 1 2000); do "
                   ": > new-entry-$i-with-a-fairly-long-name-too; done && busybox rm entry-00003-with-a-much-longer-name-for-1k "
                   "new-entry-77-with-a-fairly-long-name-too && busybox ls | busybox wc -l", timeout=600.0)
    check(rc == 0 and out.strip().endswith("3498"), f"nvme0n1: 2000 names added to the htree dir ({out.strip()[-80:]!r})")
    rc, out = g.sh("busybox df -k /mnt/a /mnt/b")
    check(rc == 0, f"df ({out.strip()[-200:]!r})")

    # umount, mount again
    rc, out = g.sh("busybox umount /mnt/a && busybox umount /mnt/b")
    check(rc == 0, f"umount both ({out.strip()!r})")
    rc, out = g.sh("busybox mount -t ext4 /dev/sdb /mnt/a && busybox mount -t ext4 /dev/nvme0n1 /mnt/b")
    check(rc == 0, f"mount both again ({out.strip()!r})")
    check(md5_of(g, "/mnt/a/big50.bin") == big_md5, "sdb: 50 MiB file survives umount/mount")
    rc, out = g.sh("busybox ls /mnt/b/big2 | busybox wc -l; busybox ls /mnt/b/big2 | busybox grep -c entry-00003; "
                   "busybox ls /mnt/a/big | busybox wc -l")
    check(out.split()[:3] == ["3498", "0", "1800"],
          f"htree directories after umount/mount: 3498 (no entry-00003) and 1800 names ({out.split()!r})")
    for m, name in (("/mnt/a", "sdb"), ("/mnt/b", "nvme0n1")):
        check(md5_of(g, f"{m}/frag.bin") == md5(frag_bytes()), f"{name}: frag.bin survives umount/mount")
        rc, out = g.sh(f"busybox cat {m}/link {m}/d1/sub/moved.txt {m}/victim.txt && "
                       f"busybox ls {m}/d1/many | busybox wc -l && busybox readlink {m}/slowlink")
        check(rc == 0 and "moved" in out and "replacement" in out and "998" in out.split()
              and "sixty/bytes" in out, f"{name}: small files, symlinks, directory after remount")

    # dirty journal: read-only refused, read-write replays
    rc, out = g.sh("busybox mount -t ext4 -o ro /dev/sdc /mnt/c")
    check(rc != 0, f"sdc (needs_recovery): read-only mount refused ({out.strip()!r})")
    rc, out = g.sh("busybox mount -t ext4 /dev/sdc /mnt/c && busybox cat /mnt/c/hello.txt")
    check(rc == 0 and "replayed content" in out, f"sdc: read-write mount replays the journal ({out.strip()!r})")
    boot = "".join(g.log)
    check(re.search(r"sdc: journal replayed: transactions 1\.\.1, 1 blocks", boot) is not None,
          "sdc: one transaction, one block replayed")
    rc, out = g.sh("echo after-replay > /mnt/c/after.txt && busybox umount /mnt/c")
    check(rc == 0, "sdc: written after the replay, unmounted")
    # Commit in small steps everywhere (writes, frees), then a clean umount.
    g.sh("busybox printf \"%4096s\" \"\" > /tmp/4k")
    at = smokelib.mark(g.log)
    rc, out = g.sh("busybox mount -t ext4 -o x4smalltxn /dev/sdc /mnt/c && cd /mnt/c && "
                   "for i in $(busybox seq 1 100); do busybox cat /tmp/4k >> F; busybox cat /tmp/4k >> G; done && "
                   "for i in 1 2 3 4; do busybox cat /tmp/pat1m >> H; done && "
                   "busybox truncate -s 204800 F && busybox rm G && cd / && busybox umount /mnt/c", timeout=300.0)
    m = re.search(r"sdc: unmounted \((\d+) journal commits\)", "".join(g.log)[at:])
    check(rc == 0 and m is not None and int(m.group(1)) > 50,
          f"sdc: x4smalltxn workload committed in steps ({m.group(0) if m else out.strip()[-200:]!r})")

    # 128-byte descriptors: the read-write request falls back to read-only.
    rc, out = g.sh("busybox mkdir -p /mnt/f && busybox mount -t ext4 /dev/sdf /mnt/f; "
                   "busybox grep /mnt/f /proc/mounts; echo x > /mnt/f/x; busybox umount /mnt/f")
    check(" /mnt/f ext4 ro" in out and "Read-only" in out,
          f"sdf (128-byte descriptors): read-only, writes refused ({out.strip()[-200:]!r})")

    # Power loss in the middle of freeing a 300-extent file.
    rc, out = g.sh("busybox mount -t ext4 -o x4smalltxn /dev/sde /mnt/e && cd /mnt/e && "
                   "for i in $(busybox seq 1 300); do busybox cat /tmp/4k >> A; busybox cat /tmp/4k >> B; done && "
                   "for i in 1 2 3 4 5 6 7 8; do busybox cat /tmp/pat1m >> C; done && busybox sync && "
                   "cd / && busybox umount /mnt/e", timeout=300.0)
    check(rc == 0, f"sde: interleaved files written ({out.strip()[-200:]!r})")
    at = smokelib.mark(g.log)
    rc, out = g.sh("busybox mount -t ext4 -o x4smalltxn,x4crashunlink /dev/sde /mnt/e && "
                   "busybox rm /mnt/e/A; busybox md5sum /mnt/e/B")
    check(re.search(r"sde: x4crash: stopped after committing transaction", "".join(g.log)[at:]) is not None,
          f"sde: power lost at the first commit inside unlink ({out.strip()[-200:]!r})")

    # simulated power loss after one commit
    # held.bin is open and unlinked when the power goes: an orphan.
    rc, out = g.sh("busybox mount -t ext4 -o x4crash /dev/sdd /mnt/d && exec 3</mnt/d/held.bin && "
                   "busybox rm /mnt/d/held.bin && echo crash-test > /mnt/d/crash.txt; "
                   "busybox cat /mnt/d/crash.txt; busybox ls /mnt/d")
    check("crash-test" in out and "held.bin" not in out,
          f"sdd: mounted with x4crash, held.bin open and unlinked, file written ({out.strip()!r})")

    dir_index_tests(g, info)

    rc, out = g.sh("echo at-poweroff > /mnt/a/at-poweroff.txt && echo at-poweroff > /mnt/b/at-poweroff.txt && "
                   "busybox grep -E \" /mnt/(a|b) \" /proc/mounts")
    check(rc == 0 and "/mnt/a ext4 rw" in out and "/mnt/b ext4 rw" in out,
          "sdb and nvme0n1 left mounted read-write for poweroff")


def timing(out, op):
    m = re.search(op + r": (\d+) in (\d+) ms, (\d+) us each, (\d+) failed", out)
    return (int(m.group(2)), int(m.group(3)), int(m.group(4))) if m else None


def dir_index_tests(g, info):
    """nvme0n2: directories becoming indexed, lookup timings, a truncate cut
    short by a power loss; nvme0n3 (no dir_index): the same timings."""
    g.sh("busybox mkdir -p /mnt/g /mnt/h")
    rc, out = g.sh("busybox mount -t ext4 /dev/nvme0n2 /mnt/g && busybox mount -t ext4 /dev/nvme0n3 /mnt/h")
    check(rc == 0, f"mount nvme0n2 and nvme0n3 ({out.strip()!r})")
    rc, out = g.sh("busybox mkdir /mnt/g/grow && fsprobe mkfiles /mnt/g/grow 5000 && "
                   "busybox ls /mnt/g/grow | busybox wc -l", timeout=600.0)
    check(rc == 0 and out.strip().endswith("5000"), f"nvme0n2: a new directory grows to 5000 names ({out.strip()[-200:]!r})")
    rc, out = g.sh("cd /mnt/g/grow && busybox rm f17 f4000 && busybox mv f18 renamed-18 && "
                   "busybox mv f19 /mnt/g/moved-19 && echo hi > f17 && busybox cat f17 renamed-18 /mnt/g/moved-19 && "
                   "busybox ls | busybox wc -l && busybox ls -d f4000 2>&1")
    check(rc != 0 and "hi" in out and "4998" in out.split() and "No such file" in out,
          f"nvme0n2: unlink, rename, create in the indexed directory ({out.strip()[-200:]!r})")
    res = {}
    for m, name in (("/mnt/g", "indexed"), ("/mnt/h", "linear")):
        rc, out = g.sh(f"busybox mkdir {m}/ten && fsprobe mkfiles {m}/ten 10000 && "
                       f"fsprobe lookups {m}/ten 10000", timeout=1200.0)
        mk, lk = timing(out, "mkfiles"), timing(out, "lookups")
        check(rc == 0 and mk and lk and mk[2] == 0 and lk[2] == 0,
              f"10000-name directory, {name}: created and looked up ({out.strip()[-300:]!r})")
        res[name] = (mk, lk)
        print(f"\n[SMOKE-EXT4RW] timing {name}: create 10000: {mk and mk[0]} ms; "
              f"20000 lookups (10000 hits, 10000 misses): {lk and lk[0]} ms, {lk and lk[1]} us each")
    info["timings"] = res
    if res.get("indexed") and res.get("linear") and res["indexed"][1] and res["linear"][1]:
        check(res["indexed"][1][0] * 3 < res["linear"][1][0],
              f"lookups in the indexed directory are at least 3x faster ({res['indexed'][1][0]} vs "
              f"{res['linear'][1][0]} ms)")
    # Fragmented T (interleaved with U), then a truncate that loses power at
    # its first step commit: an orphan with the new size.
    rc, out = g.sh("cd /mnt/g && for i in $(busybox seq 1 200); do busybox cat /tmp/4k >> T; "
                   "busybox cat /tmp/4k >> U; done && busybox sync && cd / && busybox umount /mnt/g && "
                   "busybox umount /mnt/h", timeout=300.0)
    check(rc == 0, f"nvme0n2: fragmented T written, both unmounted ({out.strip()[-200:]!r})")
    at = smokelib.mark(g.log)
    rc, out = g.sh("busybox mount -t ext4 -o x4smalltxn,x4crashtrunc /dev/nvme0n2 /mnt/g && "
                   "busybox truncate -s 10000 /mnt/g/T; busybox ls -l /mnt/g/T")
    check(re.search(r"nvme0n2: x4crash: stopped after committing transaction", "".join(g.log)[at:]) is not None,
          f"nvme0n2: power lost at the first commit inside truncate ({out.strip()[-200:]!r})")


def host_checks(a_img, b_img, c_img, d_img, info):
    for name, img in (("sdb", a_img), ("nvme0n1", b_img), ("sdc", c_img)):
        r = subprocess.run([E2FSCK, "-fn", img], capture_output=True, text=True)
        check(r.returncode == 0 and "checksum" not in r.stdout.lower(),
              f"host e2fsck -fn {name} clean (rc={r.returncode})"
              + ("" if r.returncode == 0 else "\n" + r.stdout[-2500:]))
        st = subprocess.run([DUMPE2FS, "-h", img], capture_output=True, text=True).stdout
        check(re.search(r"Filesystem state:\s+clean", st) is not None and "needs_recovery" not in st,
              f"{name}: superblock clean, no needs_recovery")
    check(md5(debugfs_cat(a_img, "/big50.bin")) == md5(big_bytes()), "debugfs: sdb big50.bin content")
    for name, img in (("sdb", a_img), ("nvme0n1", b_img)):
        check(md5(debugfs_cat(img, "/frag.bin")) == md5(frag_bytes()), f"debugfs: {name} frag.bin content")
        ex = debugfs(img, "ex /frag.bin")
        check(re.search(r"^\s*0/\s*1\s", ex, re.M) is not None,
              f"debugfs: {name} frag.bin has a depth-1 extent tree")
        root = " ".join(live_names(img, "/"))
        check("hello-renamed.txt" in root and "delete-me.txt" not in root and "new.txt" in root
              and "gone" not in root, f"debugfs: {name} root has the guest's changes")
        many = live_names(img, "/d1/many")
        check(len([n for n in many if re.fullmatch(r"f\d+", n)]) == 997 and "first" in many,
              f"debugfs: {name} /d1/many has 998 entries")
        check(htree_levels(img, "/d1/many") == 0, f"debugfs: {name} /d1/many became an htree directory")
        check(debugfs_cat(img, "/victim.txt") == b"replacement\n", f"debugfs: {name} rename over existing")
        check(debugfs_cat(img, "/trunc.txt") == trunc_bytes(), f"debugfs: {name} truncated file")
        check(debugfs_cat(img, "/at-poweroff.txt") == b"at-poweroff\n",
              f"debugfs: {name} at-poweroff.txt, written just before poweroff")
        st = debugfs(img, "stat /sparse.bin")
        m = re.search(r"Blockcount:\s+(\d+)", st)
        check(m is not None and int(m.group(1)) <= 64, f"debugfs: {name} sparse.bin is sparse ({m and m.group(1)})")
    big = live_names(a_img, "/big")
    check("zz-new" in big and "renamed" in big and "added-600-with-a-name-long-enough-to-fill-leaves" in big
          and "entry-00007-with-a-longer-name" not in big and len(big) == 1800 + 2,
          f"debugfs: sdb htree dir changes ({len(big)} names)")
    check(htree_levels(a_img, "/big") == 0, "debugfs: sdb /big is still an htree directory")
    lv = htree_levels(b_img, "/big2")
    check(info["b_levels"] == 0 and lv == 1,
          f"debugfs: nvme0n1 /big2 is still an htree directory and grew a level ({info['b_levels']} -> {lv})")
    big2 = live_names(b_img, "/big2")
    check("new-entry-2000-with-a-fairly-long-name-too" in big2 and
          "new-entry-77-with-a-fairly-long-name-too" not in big2 and
          "entry-00003-with-a-much-longer-name-for-1k" not in big2 and len(big2) == 3498 + 2,
          f"debugfs: nvme0n1 /big2 changes ({len(big2)} names)")
    check(debugfs_cat(c_img, "/hello.txt") == b"replayed content\n", "debugfs: sdc hello.txt replayed")
    check(debugfs_cat(c_img, "/after.txt") == b"after-replay\n", "debugfs: sdc after.txt")
    sp = b" " * 4096
    check(debugfs_cat(c_img, "/H") == (big_bytes()[:1048576] * 4) and debugfs_cat(c_img, "/F") == sp * 50
          and "G" not in live_names(c_img, "/"), "debugfs: sdc x4smalltxn files")

    with open(info["f_img"], "rb") as f:
        check(md5(f.read()) == info["f_md5"], "sdf: byte-identical after the guest")

    # sde: power lost between two steps of an unlink.
    e_img = info["e_img"]
    st = subprocess.run([DUMPE2FS, "-h", e_img], capture_output=True, text=True).stdout
    check("needs_recovery" in st, "sde: needs_recovery after the power loss inside unlink")
    st = subprocess.run([DUMPE2FS, "-h", e_img], capture_output=True, text=True).stdout
    check("orphan_file" not in st, "sde: made without orphan_file (the old orphan list)")
    r = subprocess.run([E2FSCK, "-fy", e_img], capture_output=True, text=True)
    bad = "Multiply-claimed" in r.stdout or re.search(r"Block bitmap differences:[^\n]*\+", r.stdout)
    check("orphan" in r.stdout.lower(), f"sde: e2fsck finds the half-freed file on the orphan list\n{r.stdout[-800:]}")
    check("recovering journal" in r.stdout and not bad and r.returncode in (0, 1),
          f"sde: e2fsck replays; no block both free and in use, none claimed twice (rc={r.returncode})\n"
          f"{r.stdout[-1500:]}")
    r = subprocess.run([E2FSCK, "-fn", e_img], capture_output=True, text=True)
    check(r.returncode == 0, f"sde: e2fsck -fn clean afterwards (rc={r.returncode})")
    check(debugfs_cat(e_img, "/B") == sp * 300 and debugfs_cat(e_img, "/C") == big_bytes()[:1048576] * 8,
          "sde: the other files are intact")

    # sdd: the journal the guest left behind
    st = subprocess.run([DUMPE2FS, "-h", d_img], capture_output=True, text=True).stdout
    check("needs_recovery" in st, "sdd: needs_recovery after the simulated power loss")
    log = debugfs(d_img, "logdump")
    check("commit block" in log, f"sdd: debugfs logdump finds the guest's commit block\n{log[-800:]}")
    check(b"crash-test" not in debugfs_cat(d_img, "/crash.txt"), "sdd: nothing was written in place")
    r = subprocess.run([E2FSCK, "-fy", d_img], capture_output=True, text=True)
    check(r.returncode in (0, 1) and "recovering journal" in r.stdout and "checksum" not in r.stdout.lower(),
          f"sdd: e2fsck replays the guest's journal (rc={r.returncode})\n{r.stdout[-1500:]}")
    check(debugfs_cat(d_img, "/crash.txt") == b"crash-test\n", "sdd: crash.txt there after the replay")
    check("orphan" in r.stdout.lower(), f"sdd: e2fsck finds held.bin in the orphan file\n{r.stdout[-800:]}")
    r = subprocess.run([E2FSCK, "-fn", d_img], capture_output=True, text=True)
    check(r.returncode == 0, f"sdd: e2fsck -fn clean after the replay (rc={r.returncode})\n{r.stdout[-1500:]}")


def host_checks_index(info):
    g_img, h_img = info["g_img"], info["h_img"]
    r = subprocess.run([E2FSCK, "-fy", g_img], capture_output=True, text=True)
    check(r.returncode in (0, 1) and "recovering journal" in r.stdout,
          f"nvme0n2: e2fsck replays the truncate's first commit (rc={r.returncode})\n{r.stdout[-1200:]}")
    r = subprocess.run([E2FSCK, "-fn", g_img], capture_output=True, text=True)
    check(r.returncode == 0, f"nvme0n2: e2fsck -fn clean (htree checked) (rc={r.returncode})\n{r.stdout[-1500:]}")
    for d, n in (("/grow", 4998), ("/ten", 10000)):
        check(htree_levels(g_img, d) == 0, f"debugfs: nvme0n2 {d} is an htree directory")
        names = live_names(g_img, d)
        check(len(names) == n + 2, f"debugfs: nvme0n2 {d} has {n} names ({len(names) - 2})")
    grow = live_names(g_img, "/grow")
    check("renamed-18" in grow and "f17" in grow and "f4000" not in grow and "f19" not in grow,
          "debugfs: nvme0n2 /grow has the guest's unlink/rename/create")
    r = subprocess.run([E2FSCK, "-fn", h_img], capture_output=True, text=True)
    check(r.returncode == 0, f"nvme0n3: e2fsck -fn clean (rc={r.returncode})\n{r.stdout[-1500:]}")
    check(htree_levels(h_img, "/ten") is None and len(live_names(h_img, "/ten")) == 10002,
          "debugfs: nvme0n3 /ten (no dir_index) is linear with 10000 names")


def second_boot(d2_img, e2_img, g2_img, info):
    """Mount the power-lost sdd, sde and nvme0n2 read-write: this driver
    replays its own log and finishes the orphans."""
    accel = ["-accel", "kvm"] if os.access("/dev/kvm", os.R_OK | os.W_OK) else ["-accel", "tcg"]
    proc = subprocess.Popen(
        ["qemu-system-i386", *smokelib.QEMU_DISPLAY, *accel, "-M", "q35",
         "-kernel", "kernel.elf", "-initrd", "initrd.tar",
         "-drive", "file=disk.img,format=raw,index=0,media=disk,snapshot=on",
         "-drive", f"file={d2_img},format=raw,index=1,media=disk",
         "-drive", f"file={e2_img},format=raw,index=2,media=disk",
         "-drive", f"file={g2_img},format=raw,if=none,id=nv",
         "-device", "nvme,serial=ext4g2,drive=nv",
         "-serial", "stdio", "-m", "256M", "-no-reboot"],
        cwd=ROOT, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, bufsize=0)
    sel = selectors.DefaultSelector()
    sel.register(proc.stdout, selectors.EVENT_READ)
    log = []
    g = Guest(proc, sel, log)
    try:
        smokelib.login(proc, sel, log, timeout=120.0)
        g.sh("busybox mkdir -p /mnt/d")
        rc, out = g.sh("busybox mount -t ext4 /dev/sdb /mnt/d && busybox cat /mnt/d/crash.txt /mnt/d/keep.txt")
        check(rc == 0 and "crash-test" in out and "kept" in out,
              f"second boot: sdd mounts read-write, crash.txt is there ({out.strip()!r})")
        m = re.search(r"sdb: journal replayed: transactions (\d+)\.\.(\d+), (\d+) blocks", "".join(log))
        check(m is not None and int(m.group(3)) > 0,
              f"second boot: the driver replayed its own log ({m.group(0) if m else None})")
        text = "".join(log)
        check(re.search(r"sdb: orphans: 1 deleted, 0 truncated", text) is not None,
              "second boot: sdd's unlinked-open held.bin deleted from the orphan file at mount")
        rc, out = g.sh("echo after-crash >> /mnt/d/crash.txt && busybox umount /mnt/d")
        check(rc == 0, "second boot: written and unmounted")
        g.sh("busybox mkdir -p /mnt/e /mnt/g")
        rc, out = g.sh("busybox mount -t ext4 /dev/sdc /mnt/e && busybox md5sum /mnt/e/B && busybox ls /mnt/e && "
                       "busybox umount /mnt/e")
        text = "".join(log)
        check(rc == 0 and "A" not in out.split() and
              re.search(r"sdc: orphans: 1 deleted, 0 truncated", text) is not None,
              f"second boot: sde's half-freed A deleted from the old orphan list at mount ({out.strip()[-200:]!r})")
        rc, out = g.sh("busybox mount -t ext4 /dev/nvme0n1 /mnt/g && busybox ls -l /mnt/g/T && "
                       "busybox md5sum /mnt/g/T && busybox umount /mnt/g")
        text = "".join(log)
        check(rc == 0 and re.search(r"nvme0n1: orphans: 0 deleted, 1 truncated", text) is not None
              and " 10000 " in out and hashlib.md5(b" " * 10000).hexdigest() in out,
              f"second boot: nvme0n2's T cut to 10000 bytes at mount ({out.strip()[-300:]!r})")
        smokelib.send(proc, "poweroff\n")
        deadline = time.time() + 60
        while proc.poll() is None and time.time() < deadline:
            for k, _ in sel.select(0.2):
                os.read(k.fd, 4096)
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
    for name, img in (("sdd", d2_img), ("sde", e2_img), ("nvme0n2", g2_img)):
        r = subprocess.run([E2FSCK, "-fn", img], capture_output=True, text=True)
        check(r.returncode == 0, f"second boot: e2fsck -fn {name} clean (rc={r.returncode})\n{r.stdout[-1500:]}")
    st = debugfs(d2_img, f"stat <{info['d_held_ino']}>")
    check("held.bin" not in " ".join(live_names(d2_img, "/")) and re.search(r"Links: 0", st) and
          re.search(r"dtime:", st) is not None, "second boot: sdd held.bin's inode is freed")
    check(htree_levels(g2_img, "/grow") == 0, "second boot: nvme0n2 /grow still indexed")
    check(debugfs_cat(d2_img, "/crash.txt") == b"crash-test\nafter-crash\n",
          "second boot: debugfs sees crash.txt with the line added after the replay")


def main():
    a_img, b_img, c_img, d_img, info = build_images()
    check(info["a_htree"], "host: sdb /big is an htree directory (e2fsck -D)")
    check(info["b_levels"] == 0, "host: nvme0n1 /big2 is a one-level htree directory")
    check(info["c_dirty"], "host: sdc needs recovery (debugfs jw)")
    accel = ["-accel", "kvm"] if os.access("/dev/kvm", os.R_OK | os.W_OK) else ["-accel", "tcg"]
    proc = subprocess.Popen(
        ["qemu-system-i386", *smokelib.QEMU_DISPLAY, *accel, "-M", "q35",
         "-kernel", "kernel.elf", "-initrd", "initrd.tar",
         "-drive", "file=disk.img,format=raw,index=0,media=disk,snapshot=on",
         "-drive", f"file={a_img},format=raw,index=1,media=disk",
         "-drive", f"file={c_img},format=raw,index=2,media=disk",
         "-drive", f"file={d_img},format=raw,index=3,media=disk",
         "-drive", f"file={info['e_img']},format=raw,index=4,media=disk",
         "-drive", f"file={info['f_img']},format=raw,index=5,media=disk",
         "-drive", f"file={b_img},format=raw,if=none,id=nv",
         "-device", "nvme,serial=ext4rw,drive=nv",
         "-drive", f"file={info['g_img']},format=raw,if=none,id=nvg",
         "-device", "nvme,serial=ext4g,drive=nvg",
         "-drive", f"file={info['h_img']},format=raw,if=none,id=nvh",
         "-device", "nvme,serial=ext4h,drive=nvh",
         "-serial", "stdio", "-m", "512M", "-no-reboot"],
        cwd=ROOT, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, bufsize=0)
    sel = selectors.DefaultSelector()
    sel.register(proc.stdout, selectors.EVENT_READ)
    log = []
    g = Guest(proc, sel, log)
    try:
        smokelib.login(proc, sel, log, timeout=120.0)
        guest_tests(g, info)

        at_poweroff = len("".join(log))
        smokelib.send(proc, "poweroff\n")
        deadline = time.time() + 90
        while proc.poll() is None and time.time() < deadline:
            for k, _ in sel.select(0.2):
                chunk = os.read(k.fd, 4096).decode("latin1", "replace")
                if chunk:
                    log.append(chunk)
                    sys.stdout.write(chunk)
        check(proc.poll() is not None, "QEMU exited after poweroff")
        text = "".join(log)
        check(len(re.findall(r"\[EXT2\]\s+(sdb|nvme0n1): now read-only", text[at_poweroff:])) >= 2,
              "poweroff took both read-write ext4 mounts read-only")
        check("has no checksum tail" not in text, "no directory block without a checksum tail")
        check("index dropped" not in text, "no htree index dropped")
        check(re.search(r"journal commit \d+ failed|journal aborted", text) is None,
              "no failed journal commits, no journal abort")
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()

    # The journal the guest left on sdd, twice: e2fsck replays one copy
    # (host_checks), this driver the other (a second boot).
    d2_img = os.path.join(OUT, "d-guest.img")
    shutil.copyfile(d_img, d2_img)
    e2_img = os.path.join(OUT, "e-guest.img")
    shutil.copyfile(info["e_img"], e2_img)
    g2_img = os.path.join(OUT, "g-guest.img")
    shutil.copyfile(info["g_img"], g2_img)
    host_checks(a_img, b_img, c_img, d_img, info)
    host_checks_index(info)
    second_boot(d2_img, e2_img, g2_img, info)
    for name, (mk, lk) in info.get("timings", {}).items():
        print(f"[SMOKE-EXT4RW] {name}: create 10000 names {mk and mk[0]} ms, "
              f"20000 lookups {lk and lk[0]} ms ({lk and lk[1]} us each)")

    if failures:
        print(f"\n[SMOKE-EXT4RW] {len(failures)} check(s) failed:")
        for f in failures:
            print(f"  - {f}")
        return 1
    print("\n[SMOKE-EXT4RW] passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"\n[SMOKE-EXT4RW] failed: {exc}", file=sys.stderr)
        raise SystemExit(1)
