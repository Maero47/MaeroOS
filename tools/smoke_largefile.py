#!/usr/bin/env python3
"""smoke-largefile: 64-bit file offsets end to end (files past 4 GiB).

Builds sparse disks in build/largefile/ on the host and boots q35 with the
boot disk on AHCI (sda, a snapshot):

  sdb  mkfs.ext4 defaults (extents, huge_file, 64bit, metadata_csum, jbd2), 16 GiB
  sdc  mkfs.exfat, 1 MiB clusters, 16 GiB
  sdd  mkfs.fat -F 32, 64 MiB
  sde  mkfs.ext2 -b 4096 (block-mapped: 8 GiB is in the triply indirect tree), 16 GiB

The disks are attached with detect-zeroes=unmap: exFAT has no holes, so the
guest writes zeroes up to each marker, and QEMU turns them into holes in the
image instead of real data.

In the guest, testfiles/lfprobe (ports/largefile/lfprobe.c, a static musl
binary) on ext4, exFAT and ext2: a file written across 4 GiB and past
8 GiB, read back (lseek+read, pread64, mmap2 past 8 GiB), stat64/statx
sizes, the 32-bit off_t ABI (lseek(2), open without O_LARGEFILE,
stat/fstat, sendfile(187), a 32-bit F_GETLK) answering EOVERFLOW,
F_SETLK64 record locks past 4 GiB against a child, copy_file_range and
sendfile64 past 4 GiB, ftruncate64 down below 4 GiB and up to 9 GiB.  On
vfat: writes and truncates at and past 4 GiB - 1 are EFBIG.  busybox dd and
cmp write and compare markers at 4 GiB + 4 KiB and 8 GiB + 4 KiB on ext4,
and busybox truncate cuts the file to 6 GiB.  Then `poweroff` with ext4,
ext2 and exFAT still mounted read-write.

On the host: e2fsck -fn, fsck.exfat -n and fsck.fat -n are clean; debugfs
reports the exact 64-bit sizes and the markers sit in the blocks bmap names;
tools/exfatimg.py (an independent reader) finds lf.bin with its exact size
and the md5 of its whole 9 GiB contents; ext4 and ext2 have large_file set.
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
from exfatimg import Volume

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
OUT = os.path.join(ROOT, "build", "largefile")
PROMPT = smokelib.PROMPT
GiB = 1 << 30
ENV = dict(os.environ, LC_ALL="C")

# lfprobe's final file (see ports/largefile/lfprobe.c).
M1_OFF, M1_KEEP = 4 * GiB - 100, 50
M3_OFF, M3_LEN = 8 * GiB + 12345, 1000
FINAL = 9 * GiB
# busybox markers: 8 KiB at these block numbers of 4 KiB.
BB_BLOCKS = (1048577, 2097153)            # 4 GiB + 4 KiB, 8 GiB + 4 KiB
BB_FINAL = 6 * GiB

failures = []


def check(cond, what):
    if cond:
        print(f"\n[SMOKE-LARGEFILE] ok: {what}")
    else:
        print(f"\n[SMOKE-LARGEFILE] FAIL: {what}")
        failures.append(what)


def tool(name):
    for p in (shutil.which(name), f"/usr/sbin/{name}", f"/sbin/{name}"):
        if p and os.path.exists(p):
            return p
    raise SystemExit(f"smoke-largefile: {name} not found")


def run(*cmd):
    return subprocess.run(cmd, check=True, capture_output=True, text=True, env=ENV)


def marker(k, n):
    return bytes((i * 31 + k * 17 + (i >> 8)) & 0xFF for i in range(n))


def sparse(path, size):
    if os.path.exists(path):
        os.remove(path)
    with open(path, "wb") as f:
        f.truncate(size)


def build_images():
    shutil.rmtree(OUT, ignore_errors=True)
    os.makedirs(OUT)
    imgs = {k: os.path.join(OUT, f"{k}.img") for k in ("ext4", "exfat", "vfat", "ext2")}
    sparse(imgs["ext4"], 16 * GiB)
    run(tool("mke2fs"), "-q", "-F", "-t", "ext4", "-L", "lf-ext4", imgs["ext4"])
    sparse(imgs["exfat"], 16 * GiB)
    run(tool("mkfs.exfat"), "-q", "-c", "1M", "-L", "LFEXFAT", imgs["exfat"])
    sparse(imgs["vfat"], 64 << 20)
    run(tool("mkfs.fat"), "-F", "32", "-n", "LFVFAT", imgs["vfat"])
    sparse(imgs["ext2"], 16 * GiB)
    run(tool("mke2fs"), "-q", "-F", "-t", "ext2", "-b", "4096", "-O", "^large_file",
        "-L", "lf-ext2", imgs["ext2"])
    return imgs


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


def probe(g, mode, d, what, timeout):
    t0 = time.time()
    rc, out = g.sh(f"/lfprobe {mode} {d}", timeout=timeout)
    took = time.time() - t0
    ok = rc == 0 and re.search(r"^PASS lfprobe", out, re.M) is not None
    fail = re.search(r"^FAIL lfprobe.*$", out, re.M)
    check(ok, f"{what}: lfprobe {mode} passes in {took:.0f}s"
              + (f" ({fail.group(0)})" if fail else ""))


def guest_tests(g, info):
    rc, out = g.sh("busybox mkdir -p /mnt/e4 /mnt/xf /mnt/vf /mnt/e2 && "
                   "busybox mount -t ext4 /dev/sdb /mnt/e4 && "
                   "busybox mount -t exfat /dev/sdc /mnt/xf && "
                   "busybox mount -t vfat /dev/sdd /mnt/vf && "
                   "busybox mount -t ext2 /dev/sde /mnt/e2 && busybox mount")
    check(rc == 0 and out.count(" /mnt/") >= 4 and " ro," not in out,
          "ext4, exFAT, vfat and ext2 mounted read-write")

    probe(g, "big", "/mnt/e4", "ext4", 300.0)
    probe(g, "big", "/mnt/e2", "ext2 (block-mapped, triply indirect)", 300.0)
    probe(g, "big", "/mnt/xf", "exFAT", 900.0)
    probe(g, "efbig", "/mnt/vf", "vfat", 60.0)

    # busybox dd/cmp on ext4: 8 KiB markers at 4 GiB + 4 KiB and 8 GiB + 4 KiB.
    rc, out = g.sh("busybox dd if=/dev/urandom of=/tmp/m bs=4096 count=2 2>/dev/null && "
                   "busybox md5sum /tmp/m")
    m = re.search(r"\b([0-9a-f]{32})\b", out)
    info["bb_md5"] = m.group(1) if m else None
    cmds = []
    for b in BB_BLOCKS:
        cmds.append(f"busybox dd if=/tmp/m of=/mnt/e4/bb.bin bs=4096 seek={b} conv=notrunc 2>/dev/null")
    for b in BB_BLOCKS:
        cmds.append(f"busybox dd if=/mnt/e4/bb.bin bs=4096 skip={b} count=2 2>/dev/null | "
                    f"busybox cmp - /tmp/m && echo CMP-{b}-OK")
    cmds.append("busybox stat -c SIZE=%s /mnt/e4/bb.bin")
    rc, out = g.sh(" && ".join(cmds), timeout=120.0)
    want = (BB_BLOCKS[1] + 2) * 4096
    check(rc == 0 and all(f"CMP-{b}-OK" in out for b in BB_BLOCKS) and f"SIZE={want}" in out,
          f"busybox dd/cmp markers at 4 GiB + 4 KiB and 8 GiB + 4 KiB; stat size {want}")
    rc, out = g.sh(f"busybox truncate -s {BB_FINAL} /mnt/e4/bb.bin && "
                   "busybox stat -c SIZE=%s /mnt/e4/bb.bin && "
                   f"busybox dd if=/mnt/e4/bb.bin bs=4096 skip={BB_BLOCKS[0]} count=2 2>/dev/null | "
                   "busybox cmp - /tmp/m && echo KEPT-OK && "
                   f"busybox tail -c 4096 /mnt/e4/bb.bin | busybox md5sum")
    check(rc == 0 and f"SIZE={BB_FINAL}" in out and "KEPT-OK" in out
          and hashlib.md5(bytes(4096)).hexdigest() in out,
          "busybox truncate -s 6G: size, the 4 GiB marker kept, zeroes at the end")
    # The busybox and probe files on exFAT read through busybox too.
    rc, out = g.sh(f"busybox stat -c SIZE=%s /mnt/xf/lf.bin && "
                   f"busybox dd if=/mnt/xf/lf.bin bs=1 skip={M3_OFF} count={M3_LEN} 2>/dev/null | "
                   "busybox md5sum", timeout=120.0)
    check(f"SIZE={FINAL}" in out and hashlib.md5(marker(3, M3_LEN)).hexdigest() in out,
          "busybox stat and dd see exFAT lf.bin's size and marker 3")
    # A 32-bit off_t view of a 9 GiB file through busybox (musl: 64-bit).
    rc, out = g.sh("busybox ls -l /mnt/e4/lf.bin /mnt/e2/lf.bin")
    check(rc == 0 and out.count(str(FINAL)) == 2, "busybox ls -l shows 9663676416 on ext4 and ext2")
    # vfat: busybox dd past 4 GiB - 1 fails with EFBIG.
    rc, out = g.sh("busybox dd if=/dev/zero of=/mnt/vf/dd.bin bs=1 count=1 seek=4294967296 2>&1; "
                   "busybox stat -c SIZE=%s /mnt/vf/dd.bin")
    check("File too large" in out and "SIZE=0" in out,
          f"vfat: busybox dd at 4 GiB is EFBIG ({out.strip()[-120:]!r})")
    rc, out = g.sh("busybox umount /mnt/vf")
    check(rc == 0, "vfat unmounted")


def debugfs(img, req):
    return subprocess.run([tool("debugfs"), "-R", req, img], capture_output=True,
                          text=True, env=ENV).stdout


def ext_size(img, path):
    m = re.search(r"Size: (\d+)", debugfs(img, f"stat {path}"))
    return int(m.group(1)) if m else None


def ext_read(img, path, off, n):
    """`n` bytes at `off` of `path`, block by block through debugfs bmap."""
    bs = int(re.search(r"Block size:\s+(\d+)", run(tool("dumpe2fs"), "-h", img).stdout).group(1))
    out = b""
    with open(img, "rb") as f:
        while n > 0:
            lblk, inb = divmod(off, bs)
            k = min(n, bs - inb)
            pblk = int(debugfs(img, f"bmap {path} {lblk}").split()[0])
            if pblk == 0:
                out += bytes(k)
            else:
                f.seek(pblk * bs + inb)
                out += f.read(k)
            off, n = off + k, n - k
    return out


def host_ext(img, what, info, busybox):
    r = subprocess.run([tool("e2fsck"), "-fn", img], capture_output=True, text=True, env=ENV)
    check(r.returncode == 0, f"{what}: e2fsck -fn clean (rc={r.returncode})\n{r.stdout[-1500:]}")
    feats = run(tool("dumpe2fs"), "-h", img).stdout
    check("large_file" in feats and "needs_recovery" not in feats,
          f"{what}: large_file set, no needs_recovery")
    check(ext_size(img, "/lf.bin") == FINAL, f"{what}: debugfs: lf.bin is {ext_size(img, '/lf.bin')} bytes")
    check(ext_read(img, "/lf.bin", M1_OFF, 100) == marker(1, M1_KEEP) + bytes(50),
          f"{what}: marker 1 head at 4 GiB - 100, zeroes after it")
    check(ext_read(img, "/lf.bin", M3_OFF, M3_LEN) == marker(3, M3_LEN),
          f"{what}: marker 3 at 8 GiB + 12345")
    check(ext_read(img, "/lf.bin", FINAL - 4096, 4096) == bytes(4096), f"{what}: zeroes at the end")
    check("lfcopy.bin" not in debugfs(img, "ls -l /"), f"{what}: lfprobe's copy was unlinked")
    if busybox:
        check(ext_size(img, "/bb.bin") == BB_FINAL, f"{what}: bb.bin is 6 GiB")
        got = ext_read(img, "/bb.bin", BB_BLOCKS[0] * 4096, 8192)
        check(info.get("bb_md5") and hashlib.md5(got).hexdigest() == info["bb_md5"],
              f"{what}: busybox's marker at 4 GiB + 4 KiB")


def expected_md5():
    """md5 of lfprobe's final 9 GiB file, streamed."""
    h = hashlib.md5()
    pos = 0
    zero = bytes(1 << 20)
    for off, data in ((M1_OFF, marker(1, M1_KEEP)), (M3_OFF, marker(3, M3_LEN)), (FINAL, b"")):
        while pos < off:
            k = min(off - pos, len(zero))
            h.update(zero[:k])
            pos += k
        h.update(data)
        pos += len(data)
    return h.hexdigest()


def host_checks(imgs, info):
    host_ext(imgs["ext4"], "ext4", info, True)
    host_ext(imgs["ext2"], "ext2", info, False)

    r = subprocess.run([tool("fsck.exfat"), "-n", imgs["exfat"]], capture_output=True,
                       text=True, env=ENV)
    text = r.stdout + r.stderr
    check(r.returncode == 0 and "clean" in text and "corrupt" not in text.lower(),
          f"exFAT: fsck.exfat -n clean (rc={r.returncode})\n{text[-800:]}")
    v = Volume(imgs["exfat"])
    problems = v.check()
    check(not problems, f"exFAT: exfatimg.py finds no problems {problems[:5]}")
    files, dirs, sizes = v.tree()
    check(sizes.get("lf.bin") == FINAL, f"exFAT: lf.bin is {sizes.get('lf.bin')} bytes")
    check(v.read_at("lf.bin", M1_OFF, 100) == marker(1, M1_KEEP) + bytes(50)
          and v.read_at("lf.bin", M3_OFF, M3_LEN) == marker(3, M3_LEN),
          "exFAT: markers 1 and 3 where lfprobe left them")
    check(files.get("lf.bin") == expected_md5(), "exFAT: md5 of all 9 GiB of lf.bin as expected")

    r = subprocess.run([tool("fsck.fat"), "-n", imgs["vfat"]], capture_output=True, text=True, env=ENV)
    check(r.returncode == 0, f"vfat: fsck.fat -n clean (rc={r.returncode})\n{r.stdout[-800:]}")


def main():
    if not os.path.exists(os.path.join(ROOT, "testfiles", "lfprobe")):
        raise SystemExit("testfiles/lfprobe missing (ports/largefile/build.sh)")
    imgs = build_images()
    info = {}
    accel = ["-accel", "kvm"] if os.access("/dev/kvm", os.R_OK | os.W_OK) else ["-accel", "tcg"]
    # cache=unsafe: the guest flushes after every AHCI write, and exFAT's
    # gigabytes of zero-fill would each cost a host fsync.
    drive = "format=raw,media=disk,cache=unsafe,discard=unmap,detect-zeroes=unmap"
    proc = subprocess.Popen(
        ["qemu-system-i386", *smokelib.QEMU_DISPLAY, *accel, "-M", "q35",
         "-kernel", "kernel.elf", "-initrd", "initrd.tar",
         "-drive", "file=disk.img,format=raw,index=0,media=disk,snapshot=on",
         "-drive", f"file={imgs['ext4']},index=1,{drive}",
         "-drive", f"file={imgs['exfat']},index=2,{drive}",
         "-drive", f"file={imgs['vfat']},index=3,{drive}",
         "-drive", f"file={imgs['ext2']},index=4,{drive}",
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
        deadline = time.time() + 120
        while proc.poll() is None and time.time() < deadline:
            for k, _ in sel.select(0.2):
                chunk = os.read(k.fd, 4096).decode("latin1", "replace")
                if chunk:
                    log.append(chunk)
                    sys.stdout.write(chunk)
        check(proc.poll() is not None, "QEMU exited after poweroff")
        text = "".join(log)
        # exFAT goes read-only silently; the host sees VolumeDirty clear.
        check(len(re.findall(r"\[EXT2\]\s+(sdb|sde): now read-only", text[at_poweroff:])) == 2,
              "poweroff took the ext4 and ext2 mounts read-only")
        check(re.search(r"journal commit \d+ failed|journal aborted|panic", text, re.I) is None,
              "no failed journal commit, no panic")
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()

    host_checks(imgs, info)
    used = sum(os.stat(p).st_blocks * 512 for p in imgs.values())
    check(used < 2 * GiB, f"the images stay sparse ({used >> 20} MiB on the host disk)")
    if failures:
        print(f"\n[SMOKE-LARGEFILE] {len(failures)} check(s) failed:")
        for f in failures:
            print(f"  - {f}")
        return 1
    print("\n[SMOKE-LARGEFILE] passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"\n[SMOKE-LARGEFILE] failed: {exc}", file=sys.stderr)
        raise SystemExit(1)
