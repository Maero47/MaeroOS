#!/usr/bin/env python3
"""smoke-ext4: mount(2)/umount2(2), partitions and the read-only ext4 driver.

Builds two extra disks with tools/mkext4img.py (GPT + 4 KiB-block ext4 with
64bit/metadata_csum/huge_file/htree; MBR logical partition + 1 KiB-block ext4
with a two-level htree), attaches them as hdb and hdc next to the usual boot
disk, and checks from inside the guest, with busybox mount/umount:

  * the partitions show up in /proc/partitions and /dev
  * mount -t ext4 works (read-only; a read-write request gets EROFS and
    busybox falls back to read-only), /proc/mounts lists it
  * every file's md5 matches the host, including the 300 MiB sparse file and
    an unwritten extent; the 5000- and 20000-entry htree directories list
    and resolve completely; fast/slow/relative/absolute/dir symlinks resolve
  * writes fail with EROFS; umount is refused while a cwd is inside, works
    otherwise, and the directory underneath comes back
  * tmpfs mount + remount,ro; unprivileged mount refused; unknown types refused

and afterwards, on the host, that neither filesystem was modified (byte
comparison with the image it was copied from) and that e2fsck -fn is clean.
"""
import hashlib
import json
import os
import re
import selectors
import shutil
import subprocess
import sys
import time

import smokelib

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
OUT = os.path.join(ROOT, "build", "ext4test")
PROMPT = smokelib.PROMPT

failures = []


def check(cond, what):
    if cond:
        print(f"\n[SMOKE-EXT4] ok: {what}")
    else:
        print(f"\n[SMOKE-EXT4] FAIL: {what}")
        failures.append(what)


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
        # Kernel log lines share the console; busybox ls colours even pipes.
        out = re.sub(r"\x1b\[[0-9;]*m", "", out)
        out = "\n".join(l for l in out.split("\n")
                        if not re.match(r"\[[A-Z0-9_-]+\]", l))
        return rc, out


def md5_lines(out):
    """{path: md5} from md5sum output."""
    res = {}
    for line in out.splitlines():
        m = re.match(r"([0-9a-f]{32})\s+(\S+)$", line.strip())
        if m:
            res[m.group(2)] = m.group(1)
    return res


def verify_files(g, files, prefix, skip_dirs):
    names = [n for n in sorted(files) if n.split("/")[0] not in skip_dirs]
    ok = True
    for i in range(0, len(names), 8):
        chunk = names[i:i + 8]
        rc, out = g.sh("busybox md5sum " + " ".join(f"{prefix}/{n}" for n in chunk),
                       timeout=180.0)
        got = md5_lines(out)
        for n in chunk:
            if got.get(f"{prefix}/{n}") != files[n]:
                print(f"\n[SMOKE-EXT4] md5 mismatch {n}: {got.get(prefix + '/' + n)} "
                      f"!= {files[n]}")
                ok = False
    return ok, len(names)


def region_unchanged(image, fs, start):
    with open(image, "rb") as a, open(fs, "rb") as b:
        a.seek(start * 512)
        while True:
            x = b.read(1 << 20)
            if not x:
                return True
            if a.read(len(x)) != x:
                return False


def main():
    if not os.environ.get("SMOKE_EXT4_KEEP_IMAGES") or \
            not os.path.exists(os.path.join(OUT, "manifest.json")):
        subprocess.run([sys.executable, os.path.join(ROOT, "tools", "mkext4img.py")],
                       check=True)
    man = json.load(open(os.path.join(OUT, "manifest.json")))
    gpt, mbr, e3 = man["gpt"], man["mbr"], man["ext3"]
    accel = ["-accel", "kvm"] if os.access("/dev/kvm", os.R_OK | os.W_OK) else ["-accel", "tcg"]

    proc = subprocess.Popen(
        ["qemu-system-i386", *smokelib.QEMU_DISPLAY, *accel,
         "-kernel", "kernel.elf", "-initrd", "initrd.tar",
         "-drive", "file=disk.img,format=raw,index=0,media=disk",
         "-drive", f"file={gpt['image']},format=raw,index=1,media=disk",
         "-drive", f"file={mbr['image']},format=raw,index=2,media=disk",
         "-serial", "stdio", "-m", "256M", "-no-reboot", "-no-shutdown"],
        cwd=ROOT, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, bufsize=0)
    sel = selectors.DefaultSelector()
    sel.register(proc.stdout, selectors.EVENT_READ)
    log = []
    g = Guest(proc, sel, log)
    try:
        smokelib.login(proc, sel, log, timeout=90.0)
        boot = "".join(log)
        check("[ATA]  hdb:" in boot and "[ATA]  hdc:" in boot, "second and third disk detected")
        check("[PART] hdb2:" in boot and "[PART] hdc5:" in boot,
              "GPT partition hdb2 and MBR logical partition hdc5 found")

        rc, out = g.sh("busybox cat /proc/partitions")
        check(all(re.search(rf"\b{p}\b", out) for p in ("hda", "hdb", "hdb1", "hdb2", "hdc1", "hdc5")),
              "/proc/partitions lists hda hdb hdb1 hdb2 hdc1 hdc5")
        rc, out = g.sh("busybox ls -l /dev/hdb2 /dev/hdc5")
        check(rc == 0 and out.count("\nb") + out.startswith("b") == 2, "/dev/hdb2 and /dev/hdc5 are block devices")

        g.sh("busybox mkdir -p /mnt; busybox rm -f /mnt/underneath; echo UNDER-MARK > /mnt/underneath")

        # ── GPT / 4 KiB ext4 ────────────────────────────────────────────
        rc, out = g.sh("busybox mount -t ext4 /dev/hdb2 /mnt")
        check(rc == 0, f"mount -t ext4 /dev/hdb2 /mnt (rc={rc}, {out.strip()!r})")
        rc, out = g.sh("busybox cat /proc/mounts")
        check("/dev/hdb2 /mnt ext4 ro" in out, "/proc/mounts shows /dev/hdb2 on /mnt ext4 ro")
        check("/dev/hda /disk ext2 rw" in out, "/proc/mounts still shows the boot ext2 /disk")
        rc, out = g.sh("busybox ls /mnt")
        check("underneath" not in out and "hello.txt" in out and "big" in out,
              "/mnt shows the ext4 root, hiding the directory underneath")

        t0 = time.time()
        ok, n = verify_files(g, gpt["files"], "/mnt", {"big"})
        check(ok, f"md5 of {n} files incl. the 300 MiB sparse file matches the host "
                  f"({time.time() - t0:.1f}s)")
        rc, out = g.sh("busybox stat -c %s /mnt/sparse.bin")
        check(out.strip().endswith(str(gpt["sparse_size"])), "sparse.bin size is 300 MiB")
        if gpt["has_unwritten"]:
            rc, out = g.sh("busybox md5sum /mnt/prealloc.bin")
            check(gpt["prealloc_md5"] in out, "unwritten extent reads as zeros")

        rc, out = g.sh("busybox ls /mnt/big | busybox wc -l")
        check(out.strip().endswith(str(gpt["big"]["count"])),
              f"htree dir lists {gpt['big']['count']} entries ({out.strip()!r})")
        rc, out = g.sh("busybox find /mnt/big -mindepth 1 | busybox sed s,.*/,, | busybox sort | busybox md5sum")
        check(gpt["big"]["md5"] in out, "htree dir listing matches the host")
        t0 = time.time()
        # Globs in chunks: busybox xargs needs vfork, and one exec takes at
        # most 4096 arguments here.
        rc, out = g.sh("cd /mnt/big && for p in 0 1 2 3 4; do busybox cat f0$p*; done "
                       "| busybox cat - long-* | busybox md5sum",
                       timeout=300.0)
        check(gpt["big_concat"] in out,
              f"every file of the htree dir resolves by name and reads back ({time.time() - t0:.1f}s)")
        rc, out = g.sh("busybox find /mnt/big -type f | busybox wc -l")
        check(out.strip().endswith(str(gpt["big"]["count"])), "find walks the htree dir")
        rc, out = g.sh("busybox stat -c %s /mnt/big/f04999 /mnt/big/nonexistent")
        check("10" in out.splitlines() and "nonexistent" in out and rc != 0,
              "lookup of a present and an absent name in the htree")

        for name, target in gpt["links"].items():
            rc, out = g.sh(f"busybox readlink /mnt/{name}")
            check(rc == 0 and out.strip() == target, f"readlink {name}")
        rc, out = g.sh("busybox cat /mnt/link-rel /mnt/link-abs /mnt/link-slow")
        check("hello from ext4" in out and out.count("deep leaf") == 2,
              "relative, absolute and slow symlinks resolve")
        rc, out = g.sh("busybox ls /mnt/link-dir/")
        check(rc == 0 and out.strip() == "c", "directory symlink resolves")
        rc, out = g.sh("busybox cat /mnt/link-dangling")
        check(rc != 0, "dangling symlink fails")
        rc, out = g.sh("cd /mnt/deep/a && busybox cat ../../hello.txt b/c/d/leaf.txt && cd .. && busybox pwd")
        check("hello from ext4" in out and "deep leaf" in out and "/mnt/deep" in out,
              "relative paths and .. inside the mount")
        rc, out = g.sh("cd /mnt && busybox ls .. | busybox grep -c disk")
        check(out.strip().endswith("1"), ".. from the mount root leaves the mount")

        rc, out = g.sh("echo new > /mnt/newfile")
        check(rc != 0 and "Read-only" in out, f"create refused with EROFS ({out.strip()!r})")
        rc, out = g.sh("busybox rmdir /mnt")
        check(rc != 0, f"rmdir of the mountpoint refused ({out.strip()!r})")
        rc, out = g.sh("echo x >> /mnt/hello.txt")
        check(rc != 0, "append to an existing file refused")
        rc, out = g.sh("busybox rm /mnt/hello.txt")
        check(rc != 0 and "Read-only" in out, "unlink refused with EROFS")
        rc, out = g.sh("busybox mkdir /mnt/newdir")
        check(rc != 0 and "Read-only" in out, "mkdir refused with EROFS")
        rc, out = g.sh("busybox chmod 777 /mnt/hello.txt; busybox stat -c %a /mnt/hello.txt")
        check("Read-only" in out and "777" not in out, "chmod refused and mode unchanged")
        rc, out = g.sh("busybox mount -o remount,rw /mnt")
        check(rc != 0, "remount,rw of ext4 refused")
        rc, out = g.sh("busybox mount -t ext4 -o ro /dev/hdb2 /tmp")
        check(rc != 0, "the same device cannot be mounted twice")

        rc, out = g.sh("cd /mnt/deep && busybox umount /mnt")
        check(rc != 0, f"umount refused while a cwd is inside ({out.strip()!r})")
        rc, out = g.sh("busybox umount /mnt")
        check(rc == 0, f"umount /mnt ({out.strip()!r})")
        rc, out = g.sh("busybox cat /proc/mounts")
        check("/dev/hdb2" not in out, "/proc/mounts no longer lists hdb2")
        rc, out = g.sh("busybox cat /mnt/underneath")
        check(rc == 0 and "UNDER-MARK" in out.splitlines(), "the directory underneath is back")
        rc, out = g.sh("busybox mount -o ro -t ext4 /dev/hdb2 /mnt && busybox cat /mnt/hello.txt && busybox umount /mnt")
        check(rc == 0 and "hello from ext4" in out, "mount again after umount")

        # ── MBR logical partition / 1 KiB ext4, two-level htree ─────────
        rc, out = g.sh("busybox mount -o ro -t ext4 /dev/hdc5 /mnt")
        check(rc == 0, f"mount /dev/hdc5 (MBR logical, 1 KiB blocks) ({out.strip()!r})")
        ok, n = verify_files(g, mbr["files"], "/mnt", {"huge"})
        check(ok, f"md5 of {n} files on hdc5 matches")
        rc, out = g.sh("busybox find /mnt/huge -mindepth 1 | busybox sed s,.*/,, | busybox sort | busybox md5sum")
        check(mbr["huge"]["md5"] in out, f"{mbr['huge']['count']}-entry two-level htree dir listing matches")
        t0 = time.time()
        rc, out = g.sh("cd /mnt/huge && for p in 0 1; do for q in 0 1 2 3 4 5 6 7 8 9; do "
                       "busybox stat -c %s n$p$q*; done; done | busybox sort | busybox uniq -c",
                       timeout=600.0)
        counts = {int(b): int(a) for a, b in re.findall(r"(\d+)\s+(\d+)", out)}
        check(sum(counts.values()) == mbr["huge"]["count"],
              f"all {mbr['huge']['count']} names resolve through the two-level htree "
              f"({time.time() - t0:.1f}s)")
        rc, out = g.sh("busybox cat /mnt/readme-link /mnt/huge/n12345")
        check("logical partition" in out and "12345" in out, "symlink and lookup on hdc5")
        rc, out = g.sh("busybox umount /mnt")
        check(rc == 0, "umount hdc5")

        # ── ext3 on GPT p1: block-mapped files, TEA htree ───────────────
        rc, out = g.sh("busybox mount -t ext3 -o ro /dev/hdb1 /mnt")
        check(rc == 0, f"mount -t ext3 /dev/hdb1 (journal, no extents) ({out.strip()!r})")
        ok, n = verify_files(g, e3["files"], "/mnt", {"dir"})
        check(ok, "md5 of a 3 MiB block-mapped file (indirect + double indirect) matches")
        rc, out = g.sh("busybox find /mnt/dir -mindepth 1 | busybox sed s,.*/,, | busybox sort | busybox md5sum")
        check(e3["dir"]["md5"] in out, "TEA-hashed htree dir listing matches")
        rc, out = g.sh("cd /mnt/dir && busybox cat e0* | busybox wc -l")
        check(out.strip().endswith(str(e3["dir"]["count"])),
              f"all {e3['dir']['count']} names resolve through the TEA htree")
        rc, out = g.sh("busybox umount /mnt")
        check(rc == 0, "umount hdb1")

        # ── Bind pinning, umount by name, EROFS through descriptors ─────
        rc, out = g.sh("/disk/mntprobe /dev/hdb1", timeout=120.0)
        check(rc == 0 and "mntprobe ok" in out, f"mntprobe ({out.strip()[-300:]!r})")

        # ── Pseudo filesystems, flags, permissions ──────────────────────
        rc, out = g.sh("busybox mount -t tmpfs none /mnt && echo hi > /mnt/t && busybox cat /mnt/t")
        check(rc == 0 and "hi" in out, "tmpfs mounted on /mnt and writable")
        rc, out = g.sh("busybox mount -o remount,ro /mnt && echo x > /mnt/t2")
        check(rc != 0, "remount,ro makes the tmpfs read-only")
        rc, out = g.sh("busybox grep /mnt /proc/mounts")
        check("none /mnt tmpfs ro" in out, "/proc/mounts shows the tmpfs ro")
        rc, out = g.sh("busybox mount -o remount,rw /mnt && echo x > /mnt/t2 && busybox umount /mnt")
        check(rc == 0, "remount,rw and umount the tmpfs")
        rc, out = g.sh("busybox mkdir -p /tmp/p && busybox mount -t proc proc /tmp/p && busybox head -1 /tmp/p/version && busybox umount /tmp/p")
        check(rc == 0 and "Linux version" in out, "procfs mounted a second time on /tmp/p")
        rc, out = g.sh("busybox mount -t sysfs none /mnt")
        check(rc != 0, "unknown filesystem type refused")
        rc, out = g.sh("busybox setuidgid user busybox mount -t tmpfs none /mnt")
        check(rc != 0 and ("not permitted" in out or "denied" in out or "Operation" in out),
              f"unprivileged mount refused ({out.strip()!r})")
        rc, out = g.sh("busybox umount /disk")
        check(rc != 0, "boot mount /disk cannot be unmounted")

        log_text = "".join(log)
        check("fails its checksum" not in log_text, "no checksum errors logged")
        check("htree index" not in log_text, "no htree fallback scans logged")

        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()

        # ── Host side: nothing was written ──────────────────────────────
        check(region_unchanged(gpt["image"], gpt["fs"], gpt["start"]),
              "hdb2 is byte-identical to the image it was made from")
        check(region_unchanged(mbr["image"], mbr["fs"], mbr["start"]),
              "hdc5 is byte-identical to the image it was made from")
        check(region_unchanged(e3["image"], e3["fs"], e3["start"]),
              "hdb1 is byte-identical to the image it was made from")
        e2fsck = shutil.which("e2fsck") or "/sbin/e2fsck"
        for fs in (gpt["fs"], mbr["fs"], e3["fs"]):
            r = subprocess.run([e2fsck, "-fn", fs], capture_output=True, text=True)
            check(r.returncode == 0, f"e2fsck -fn {os.path.basename(fs)} clean")

        if failures:
            print(f"\n[SMOKE-EXT4] {len(failures)} check(s) failed:")
            for f in failures:
                print(f"  - {f}")
            return 1
        print("\n[SMOKE-EXT4] passed")
        return 0
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                proc.kill()


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"\n[SMOKE-EXT4] failed: {exc}", file=sys.stderr)
        raise SystemExit(1)
