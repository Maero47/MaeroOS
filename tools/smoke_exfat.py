#!/usr/bin/env python3
"""smoke-exfat: the read-write exFAT driver on AHCI and USB mass storage.

Builds test volumes with tools/mkexfatimg.py (mkfs.exfat + tools/exfatimg.py)
and boots q35 with the boot disk on AHCI (sda), an exFAT partition on AHCI
(sdb1, 4 KiB clusters), a 64 GiB sparse exFAT disk with 4096-byte sectors and
a 5 GiB file on AHCI (sdc), and an exFAT USB stick without a partition table
on a qemu-xhci usb-storage device (sdd).  In the guest, with busybox:

  * every file's md5 matches the host's (contiguous, FAT-chain and
    ValidDataLength < DataLength files), names come back in UTF-8 (Turkish,
    non-BMP), lookups are case-insensitive through the up-case table
  * mkdir, create, write, append (contiguous files that stay contiguous, and
    one whose next cluster is taken, so it becomes a FAT chain), write past
    the end, truncate down and up, rename (same directory, across, a
    directory with contents, over an existing file, case only), unlink,
    rm -r, rmdir; a directory that grows past its cluster
  * statfs, a file unlinked while open, umount + mount, ro / remount,rw, EBUSY
  * the 5 GiB file: its first 4 GiB - 1 bytes read and written at the right
    places (64-bit cluster arithmetic), its length kept
  * the stick and the big volume are left mounted: `poweroff` must flush them
    and clear VolumeDirty

then, on the host: `fsck.exfat -n` is clean for every volume, and
tools/exfatimg.py (an independent reader) finds exactly the expected files,
contents and a consistent bitmap.  A second boot mounts crafted volumes
(malformed entry sets, bad boot regions, a broken up-case table, fuzzed
metadata): refused or read safely, never a crash.
"""
import hashlib
import json
import os
import re
import selectors
import shutil
import subprocess
import sys

import smokelib
from exfatimg import Volume

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
OUT = os.path.join(ROOT, "build", "exfattest")
PROMPT = smokelib.PROMPT
TAG = "[SMOKE-EXFAT]"
GiB = 1 << 30
ENV = dict(os.environ, LC_ALL="C.UTF-8")
ENV["PATH"] = os.path.expanduser("~/opt/bin") + ":" + ENV.get("PATH", "") + ":/usr/sbin:/sbin"

failures = []


def check(cond, what):
    print(f"\n{TAG} {'ok' if cond else 'FAIL'}: {what}")
    if not cond:
        failures.append(what)


def u8(text):
    return text.encode("latin1").decode("utf-8", "replace")


def md5(b):
    return hashlib.md5(b).hexdigest()


class Guest:
    def __init__(self, proc, sel, log):
        self.proc, self.sel, self.log = proc, sel, log

    def sh(self, cmd, timeout=120.0):
        """Run `cmd` (UTF-8 allowed) under busybox sh; (status, output)."""
        assert "'" not in cmd
        at = smokelib.mark(self.log)
        line = f"busybox sh -c '{cmd}; echo @@\"RC\"=$?'\n"
        smokelib.send(self.proc, line.encode("utf-8").decode("latin1"))
        end = smokelib.wait_for(self.proc, self.sel, "@@RC=", self.log, timeout, at)
        nl = smokelib.wait_for(self.proc, self.sel, "\n", self.log, 10.0, end)
        smokelib.wait_for(self.proc, self.sel, PROMPT, self.log, 10.0, nl)
        text = "".join(self.log)
        rc = int(re.match(r"\d+", text[end:nl]).group(0))
        out = text[at:end - len("@@RC=")].replace("\r", "")
        out = out.split("\n", 1)[1] if "\n" in out else ""
        out = re.sub(r"\x1b\[[0-9;]*m", "", out)
        out = "\n".join(l for l in out.split("\n") if not re.match(r"\[[A-Z0-9_-]+\]", l))
        return rc, u8(out)

    def md5_tree(self, mnt):
        rc, out = self.sh(f"cd {mnt} && busybox find . -type f | busybox sort | "
                          "while read -r f; do busybox md5sum \"$f\"; done", timeout=300.0)
        res = {}
        for line in out.splitlines():
            m = re.match(r"([0-9a-f]{32})  \./(.*)$", line.strip("\n"))
            if m:
                res[m.group(2)] = m.group(1)
        return res


def compare_tree(got, want, what):
    missing = sorted(set(want) - set(got))
    extra = sorted(set(got) - set(want))
    wrong = sorted(p for p in want if p in got and got[p] != want[p])
    for p in missing[:5]:
        print(f"{TAG}   missing: {p!r}")
    for p in extra[:5]:
        print(f"{TAG}   unexpected: {p!r}")
    for p in wrong[:5]:
        print(f"{TAG}   md5 differs: {p!r}")
    check(not missing and not extra and not wrong,
          f"{what}: {len(want)} files, names and md5s match "
          f"({len(missing)} missing, {len(extra)} extra, {len(wrong)} differ)")


def boot(args, accel):
    proc = subprocess.Popen(
        ["qemu-system-i386", *smokelib.QEMU_DISPLAY, *accel, *args,
         "-kernel", "kernel.elf", "-initrd", "initrd.tar",
         "-serial", "stdio", "-m", "256M", "-no-reboot"],
        cwd=ROOT, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, bufsize=0)
    sel = selectors.DefaultSelector()
    sel.register(proc.stdout, selectors.EVENT_READ)
    log = []
    return proc, sel, log, Guest(proc, sel, log)


def stop(proc):
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()


# ── Host side ────────────────────────────────────────────────────────────

def extract(image, start, sectors, name):
    """The volume as its own file (fsck.exfat has no offset option)."""
    path = os.path.join(OUT, name)
    with open(image, "rb") as a, open(path, "wb") as b:
        a.seek(start * 512)
        left = sectors * 512
        while left:
            chunk = a.read(min(left, 1 << 20))
            if not chunk:
                break
            b.write(chunk)
            left -= len(chunk)
    return path


def fsck(path, what):
    r = subprocess.run(["fsck.exfat", "-n", path], capture_output=True, text=True, env=ENV)
    text = r.stdout + r.stderr
    print(text)
    check(r.returncode == 0 and "clean" in text and "corrupt" not in text.lower()
          and "error" not in text.lower(), f"fsck.exfat -n {what} is clean (rc={r.returncode})")


def host_check(path, offset, want, what):
    v = Volume(path, offset)
    problems = v.check()
    files, dirs, sizes = v.tree()
    v.close()
    for p in problems[:8]:
        print(f"{TAG}   checker: {p}")
    check(not problems, f"{what}: the independent checker finds no problems "
                        "(checksums, name hashes, bitmap, lengths, VolumeDirty clear)")
    compare_tree(files, want, f"{what}: read back on the host")
    return sizes


# ── The guest session ────────────────────────────────────────────────────

def free(g, mnt):
    rc, out = g.sh(f"busybox stat -f -c \"%t %S %b %f\" {mnt}")
    m = re.search(r"(\S+) (\d+) (\d+) (\d+)", out)
    return (m.group(1), int(m.group(2)), int(m.group(3)), int(m.group(4))) if m else None


def raw_write_refused(g, dev, what):
    rc, out = g.sh(f"busybox dd if={dev} of=/tmp/sect bs=512 skip=1 count=1 2>/dev/null; "
                   f"busybox dd if=/tmp/sect of={dev} bs=512 seek=1 count=1 conv=notrunc")
    check(rc != 0 and "busy" in out.lower(),
          f"raw write to {dev} refused with EBUSY: {what} ({out.strip()[-120:]!r})")


def ahci_session(g, man, big):
    m = man["ahci"]
    rc, out = g.sh("busybox mkdir -p /mnt && busybox mount -t exfat /dev/sdb1 /mnt")
    check(rc == 0, f"mount -t exfat /dev/sdb1 /mnt (rc={rc}, {out.strip()!r})")
    rc, out = g.sh("busybox grep sdb1 /proc/mounts")
    check("/dev/sdb1 /mnt exfat rw" in out, "/proc/mounts: /dev/sdb1 /mnt exfat rw")
    rc, out = g.sh("cd /mnt && busybox ls -a")
    names = set(out.split())
    check({".", "..", "hello.txt", "big.bin", "many"} <= names,
          "the root lists, with . and ..")
    rc, out = g.sh("cd /mnt && busybox find . -maxdepth 1 | busybox sed s,^./,,")
    names = set(out.split("\n"))
    check({"Türkçe ğüşıöç İI.txt", "Gülümseme 😀 dosyası.txt",
           "This is a long file name with many words in it.text"} <= names,
          "Turkish, non-BMP and multi-entry names list as on the host")
    compare_tree(g.md5_tree("/mnt"), m["files"], "exFAT on AHCI, read (contiguous, FAT chain, "
                                                  "ValidDataLength < DataLength)")
    rc, out = g.sh("busybox ls /mnt/many | busybox wc -l")
    check(out.strip().endswith("300"), "the 300-entry directory (12 clusters) lists completely")
    rc, out = g.sh("busybox cat /mnt/HELLO.TXT \"/mnt/KLASÖR/ŞARKı SÖZLERI.MP3\" "
                   "\"/mnt/türkçe ĞÜŞıÖÇ İi.TXT\" | busybox wc -c")
    want_n = 17 + 70000 + len("Merhaba dünya: çğıöşü ÇĞİÖŞÜ\n".encode())
    check(rc == 0 and out.strip().endswith(str(want_n)),
          f"lookups are case-insensitive through the up-case table, incl. Turkish ({out.strip()!r})")
    rc, out = g.sh("busybox cat \"/mnt/türkçe ğüşIöç İI.txt\"")
    check(rc != 0, "dotless ı and I stay different names (the up-case table maps ı to itself)")
    rc, out = g.sh("busybox stat -c \"%s %F\" /mnt/big.bin /mnt/Klasör /mnt/sparse.bin")
    check("5242880 regular file" in out and "directory" in out and "200000 regular file" in out,
          "stat: size and type")

    cmds = [
        ("mkdir", "busybox mkdir \"/mnt/Yeni Klasör\""),
        ("create + write 109 KiB", "busybox seq 1 20000 > \"/mnt/Yeni Klasör/çalışma notları.txt\""),
        ("5 MiB copy", "busybox cp /mnt/big.bin \"/mnt/Kopya büyük.bin\""),
        ("150 entries (the directory grows)", "busybox mkdir \"/mnt/Yeni Klasör/çok\" && "
         "for i in $(busybox seq 1 150); do echo $i > \"/mnt/Yeni Klasör/çok/dosya numarası $i.txt\"; done"),
        ("rename across directories", "busybox mv /mnt/hello.txt \"/mnt/Klasör/merhaba dünya.txt\""),
        ("move a directory with contents", "busybox mv \"/mnt/Klasör/alt dizin\" \"/mnt/Yeni Klasör/taşınan dizin\""),
        ("rename over an existing file", "busybox mv /mnt/readme.md \"/mnt/This is a long file name with many words in it.text\""),
        ("case-only rename", "echo short > /mnt/short.txt && busybox mv /mnt/short.txt /mnt/SHORT.txt"),
        ("unlink", "busybox rm /mnt/empty.txt"),
        ("rm -r of 300 entries", "busybox rm -r /mnt/many"),
        ("append", "echo appended >> \"/mnt/Türkçe ğüşıöç İI.txt\""),
        ("append to a file whose next cluster is taken (becomes a FAT chain)",
         "busybox dd if=/mnt/big.bin of=/mnt/blocked.bin bs=4096 count=3 seek=2 conv=notrunc 2>/dev/null"),
        ("write into the middle of the FAT-chain file",
         "echo -n MIDDLE | busybox dd of=/mnt/frag.bin bs=1 seek=150000 conv=notrunc 2>/dev/null"),
        ("append to the FAT-chain file", "busybox head -c 50000 /mnt/big.bin >> /mnt/frag.bin"),
        ("write past the end", "echo -n X | busybox dd of=/mnt/hole.bin bs=1 seek=100000 2>/dev/null"),
        ("write past ValidDataLength", "echo -n Y | busybox dd of=/mnt/sparse.bin bs=1 seek=150000 conv=notrunc 2>/dev/null"),
        ("truncate down", "busybox cp /mnt/big.bin /mnt/trunc.bin && busybox truncate -s 12345 /mnt/trunc.bin"),
        ("truncate up (past ValidDataLength, reads as zeros)",
         "echo -n abc > /mnt/grow.bin && busybox truncate -s 1000000 /mnt/grow.bin"),
        ("mkdir + rmdir", "busybox mkdir /mnt/gone && busybox rmdir /mnt/gone"),
        ("utimes", "busybox touch -d \"2024-02-03 04:05:06\" /mnt/SHORT.txt"),
    ]
    for what, cmd in cmds:
        rc, out = g.sh(cmd, timeout=300.0)
        check(rc == 0, f"{what} ({out.strip()[-200:]!r})")
    rc, out = g.sh("busybox stat -c %Y /mnt/SHORT.txt")
    check(out.strip().endswith("1706933106"), f"utimes: mtime kept to the second ({out.strip()!r})")
    rc, out = g.sh("busybox rmdir \"/mnt/Yeni Klasör\"")
    check(rc != 0, "rmdir of a non-empty directory refused")
    rc, out = g.sh("busybox mv \"/mnt/Yeni Klasör\" \"/mnt/Yeni Klasör/çok/içine\"")
    check(rc != 0, f"a directory cannot move below itself ({out.strip()!r})")
    f0 = free(g, "/mnt")
    g.sh("busybox cp /mnt/big.bin /mnt/statfs-probe.bin")
    f1 = free(g, "/mnt")
    g.sh("busybox rm /mnt/statfs-probe.bin")
    f2 = free(g, "/mnt")
    check(f0 and f1 and f2 and f0[0] == "2011bab0" and f0[1] == 4096 and f0[3] - f1[3] == 1280
          and f2[3] == f0[3], f"statfs: exFAT magic, 4 KiB clusters, 1280 used by 5 MiB and "
                              f"given back ({f0}, {f1}, {f2})")
    rc, out = g.sh("busybox cp /mnt/big.bin /mnt/open.bin && exec 3< /mnt/open.bin && "
                   "busybox rm /mnt/open.bin && busybox test ! -e /mnt/open.bin && "
                   "busybox md5sum <&3 && exec 3<&-")
    check(rc == 0 and m["files"]["big.bin"] in out, "a file unlinked while open still reads back whole")
    f3 = free(g, "/mnt")
    check(f3 and f3[3] == f0[3], "its clusters are free once it is closed")
    rc, out = g.sh("cd /mnt && busybox find . -maxdepth 1 | busybox sed s,^./,,")
    check("SHORT.txt" in out.split("\n") and "short.txt" not in out.split("\n"),
          "the case-only rename shows the new spelling")
    rc, out = g.sh("cd \"/mnt/Yeni Klasör/taşınan dizin\" && busybox cat \"derin dosya.txt\" && "
                   "cd .. && busybox find . -maxdepth 1 && cd .. && busybox pwd")
    check("deep" in out and "çok" in out and out.strip().endswith("/mnt"),
          "relative paths and .. through moved directories")
    rc, out = g.sh("busybox ln -s hello.txt /mnt/link")
    check(rc != 0 and "not permitted" in out, f"symlink refused with EPERM ({out.strip()!r})")
    rc, out = g.sh("echo x > \"/mnt/bad:name\"")
    check(rc != 0, "a name with ':' is refused")

    want = dict(m["files"])
    want["Klasör/merhaba dünya.txt"] = want.pop("hello.txt")
    want["Yeni Klasör/taşınan dizin/derin dosya.txt"] = want.pop("Klasör/alt dizin/derin dosya.txt")
    want["This is a long file name with many words in it.text"] = want.pop("readme.md")
    del want["empty.txt"]
    for p in [p for p in want if p.startswith("many/")]:
        del want[p]
    want["Türkçe ğüşıöç İI.txt"] = md5("Merhaba dünya: çğıöşü ÇĞİÖŞÜ\n".encode() + b"appended\n")
    want["Yeni Klasör/çalışma notları.txt"] = md5("".join(f"{i}\n" for i in range(1, 20001)).encode())
    want["Kopya büyük.bin"] = want["big.bin"]
    want["SHORT.txt"] = md5(b"short\n")
    for i in range(1, 151):
        want[f"Yeni Klasör/çok/dosya numarası {i}.txt"] = md5(f"{i}\n".encode())
    want["hole.bin"] = md5(b"\0" * 100000 + b"X")
    want["trunc.bin"] = md5(big[:12345])
    want["grow.bin"] = md5(b"abc" + bytes(1000000 - 3))
    blocked = bytearray(man["blocked"])
    blocked[8192:8192 + 12288] = big[:12288]
    want["blocked.bin"] = md5(bytes(blocked))
    frag = bytearray(man["frag"])
    frag[150000:150006] = b"MIDDLE"
    want["frag.bin"] = md5(bytes(frag) + big[:50000])
    sp = bytearray(b"valid head\n" * 100 + bytes(200000 - 1100))
    sp[150000] = ord("Y")
    want["sparse.bin"] = md5(bytes(sp))
    compare_tree(g.md5_tree("/mnt"), want, "exFAT after the writes, read in the guest")

    rc, out = g.sh("cd /mnt && busybox umount /mnt")
    check(rc != 0, "umount refused while a cwd is inside")
    rc, out = g.sh("busybox umount /mnt")
    check(rc == 0, f"umount /mnt ({out.strip()!r})")
    rc, out = g.sh("busybox mount -t exfat -o ro /dev/sdb1 /mnt")
    check(rc == 0, "mount again, read-only")
    compare_tree(g.md5_tree("/mnt"), want, "exFAT after umount + mount")
    rc, out = g.sh("echo nope > /mnt/nope.txt")
    check(rc != 0 and "Read-only" in out, f"ro mount: create refused with EROFS ({out.strip()!r})")
    rc, out = g.sh("busybox rm /mnt/SHORT.txt")
    check(rc != 0 and "Read-only" in out, "ro mount: unlink refused with EROFS")
    rc, out = g.sh("busybox mount -o remount,rw /mnt && echo yes > /mnt/remounted.txt && "
                   "busybox grep sdb1 /proc/mounts")
    check(rc == 0 and "exfat rw" in out, "remount,rw makes it writable")
    want["remounted.txt"] = md5(b"yes\n")
    rc, out = g.sh("busybox mount -t exfat /dev/sdb1 /tmp")
    check(rc != 0, "the same device cannot be mounted twice")
    raw_write_refused(g, "/dev/sdb", "whole disk while sdb1 is mounted")
    rc, out = g.sh("busybox umount /mnt")
    check(rc == 0, "umount /mnt")
    return want


def big_session(g, man):
    b = man["big"]
    rc, out = g.sh("busybox mkdir -p /big && busybox mount -t exfat /dev/sdc /big && "
                   "busybox cat \"/big/Arşiv/küçük.txt\"")
    check(rc == 0 and "small file on a big volume" in out,
          f"64 GiB volume with 4096-byte sectors mounts and reads ({out.strip()[-120:]!r})")
    f = free(g, "/big")
    check(f and f[1] == 128 * 1024 and f[2] > 500000,
          f"statfs: 128 KiB clusters, {f[2] if f else '?'} clusters on the 64 GiB volume")
    rc, out = g.sh("busybox stat -c %s /big/huge.bin")
    check(out.strip().endswith("4294967295"),
          f"the 5 GiB file shows its first 4 GiB - 1 bytes through the 32-bit VFS ({out.strip()!r})")
    # Markers at 0 and 4 GiB - 8 KiB (block 1048574 of 4096 bytes).
    rc, out = g.sh("busybox dd if=/big/huge.bin bs=4096 count=1 2>/dev/null | busybox md5sum; "
                   "busybox dd if=/big/huge.bin bs=4096 skip=1048574 count=1 2>/dev/null | busybox md5sum",
                   timeout=180.0)
    sums = re.findall(r"([0-9a-f]{32})", out)
    check(sums == [b["marks"]["0"], b["marks"][str(4 * GiB - 8192)]],
          f"markers at 0 and 4 GiB - 8 KiB read back ({sums})")
    rc, out = g.sh("busybox dd if=/dev/urandom of=/tmp/mark bs=4096 count=1 2>/dev/null && "
                   "busybox dd if=/tmp/mark of=/big/huge.bin bs=4096 seek=1048574 conv=notrunc "
                   "2>/dev/null && busybox md5sum /tmp/mark", timeout=180.0)
    newmark = re.findall(r"([0-9a-f]{32})", out)
    check(rc == 0 and len(newmark) == 1, "a new 4 KiB marker written at 4 GiB - 8 KiB")
    rc, out = g.sh("busybox head -c 20971520 /dev/urandom > /big/yeni.bin && busybox md5sum /big/yeni.bin")
    yeni = re.findall(r"([0-9a-f]{32})", out)
    check(rc == 0 and len(yeni) == 1, "a 20 MiB file written on the big volume")
    return (newmark[0] if newmark else None), (yeni[0] if yeni else None)


def usb_session(g, man):
    rc, out = g.sh("busybox cat /proc/partitions")
    check(re.search(r"\bsdd\b", out) is not None, "the USB stick is sdd in /proc/partitions")
    rc, out = g.sh("busybox mkdir -p /usb && busybox mount /dev/sdd /usb && busybox grep sdd /proc/mounts")
    check(rc == 0 and "/dev/sdd /usb exfat rw" in out,
          f"mount /dev/sdd /usb without -t: exfat found through /proc/filesystems ({out.strip()!r})")
    compare_tree(g.md5_tree("/usb"), man["usb"]["files"], "USB stick, read")
    rc, out = g.sh("busybox seq 1 50000 > \"/usb/USB-yazıldı.txt\" && "
                   "busybox mkdir /usb/Yedek && "
                   "busybox cp /usb/Belgeler/photo.jpg /usb/Yedek/fotoğraf.jpg && "
                   "busybox mv /usb/usb-hello.txt /usb/Yedek/ && busybox rm /usb/DOS.TXT")
    check(rc == 0, f"USB: create, copy, mkdir, rename, unlink ({out.strip()[-200:]!r})")
    raw_write_refused(g, "/dev/sdd", "USB stick mounted read-write")
    want = dict(man["usb"]["files"])
    want["USB-yazıldı.txt"] = md5("".join(f"{i}\n" for i in range(1, 50001)).encode())
    want["Yedek/fotoğraf.jpg"] = want["Belgeler/photo.jpg"]
    want["Yedek/usb-hello.txt"] = want.pop("usb-hello.txt")
    del want["DOS.TXT"]
    compare_tree(g.md5_tree("/usb"), want, "USB stick after the writes, read in the guest")
    return want


def crafted(man, accel):
    """Malformed entry sets, bad boot regions, a broken up-case table and
    fuzzed volumes: refused or read safely, never a crash."""
    work = []
    for name in ("crafted", "badboot", "upcase"):
        dst = os.path.join(OUT, f"run-{name}.img")
        shutil.copyfile(man[name]["image"], dst)
        work.append(dst)
    for i, src in enumerate(man["fuzz"]):
        dst = os.path.join(OUT, f"run-fuzz-{i}.img")
        shutil.copyfile(src, dst)
        work.append(dst)
    args = ["-M", "q35", "-drive", "file=disk.img,format=raw,index=0,media=disk,snapshot=on"]
    for i, w in enumerate(work):
        args += ["-drive", f"file={w},format=raw,index={i + 1},media=disk"]
    proc, sel, log, g = boot(args, accel)
    try:
        smokelib.login(proc, sel, log, timeout=120.0)
        at = smokelib.mark(log)
        rc, out = g.sh("busybox mkdir -p /c && busybox mount -t exfat /dev/sdb /c && cd /c && "
                       "busybox ls; busybox find . -type f | busybox sort | "
                       "while read -r f; do busybox md5sum \"$f\"; done")
        got = {}
        for line in out.splitlines():
            mm = re.match(r"([0-9a-f]{32})  \./(.*)$", line)
            if mm:
                got[mm.group(2)] = mm.group(1)
        good = man["crafted"]["good"]
        check(rc == 0 and all(got.get(p) == s for p, s in good.items()),
              f"crafted entry sets: the {len(good)} good files read correctly next to them")
        names = set(out.split("\n"))
        check(not {"badsum.txt", "fewsec.txt", "longname.txt"} & names,
              "sets with a bad checksum, too few secondaries or a too-long name are ignored")
        rc, out = g.sh("busybox cat /c/badclus.bin /c/hugelen.bin /c/short.bin > /dev/null; "
                       "busybox md5sum /c/loop.bin; busybox find /c/loopdir -maxdepth 4 | busybox wc -l; "
                       "busybox ls /c | busybox grep -c sl")
        check("panic" not in "".join(log)[at:].lower(),
              "clusters outside the heap, a 2^62-byte length, short and looping chains, "
              "a directory on the root's cluster: read without a crash")
        rc, out = g.sh("busybox rm -rf /c/* ; busybox ls -a /c; echo new > /c/new.txt && "
                       "busybox mkdir /c/d && busybox mv /c/new.txt /c/d/ && busybox cat /c/d/new.txt && "
                       "busybox umount /c")
        check(rc == 0 and "new" in out, f"the crafted volume: rm -rf, write, umount ({out.strip()[-160:]!r})")

        at_bad = smokelib.mark(log)
        # Read-write is refused with EROFS; busybox mount then retries
        # read-only by itself.
        rc, out = g.sh("busybox mount -t exfat /dev/sdc1 /c && busybox grep sdc1 /proc/mounts && "
                       "busybox cat /c/boot.txt && busybox umount /c")
        check(rc == 0 and "exfat ro" in out and "boot test" in out,
              f"main boot region checksum broken: mounted read-only from the backup region ({out.strip()[-120:]!r})")
        results = {}
        for n, what in ((2, "ClusterCount past the volume"), (3, "root cluster 1"),
                        (4, "both boot checksums broken")):
            rc, out = g.sh(f"busybox mount -t exfat -o ro /dev/sdc{n} /c")
            results[what] = rc != 0
            if rc == 0:
                g.sh("busybox umount /c")
        bad_log = "".join(log)[at_bad:]
        check(all(results.values()), f"invalid boot regions refused at mount ({results})")
        check("inconsistent boot sector" in bad_log and "checksum mismatch" in bad_log,
              "the refusals are named in the log")
        rc, out = g.sh("busybox mount -t exfat /dev/sdd /c && busybox grep sdd /proc/mounts && "
                       "busybox cat \"/c/UPCASE TEST.TXT\" && busybox umount /c")
        check(rc == 0 and "exfat ro" in out and "upcase" in out,
              f"an up-case table that fails its checksum: read-only, with the built-in mapping ({out.strip()[-120:]!r})")
        at_f = smokelib.mark(log)
        for dev in ("/dev/sde", "/dev/sdf"):
            g.sh(f"busybox mount -t exfat -o ro {dev} /c && "
                 "busybox find /c -maxdepth 6 | busybox wc -l; "
                 "busybox find /c -maxdepth 6 -type f -exec busybox cat {} + >/dev/null 2>&1; "
                 "busybox umount /c", timeout=180.0)
            g.sh(f"busybox mount -t exfat {dev} /c && "
                 "busybox rm -rf /c/* 2>/dev/null; echo new > /c/new.txt; "
                 "busybox mkdir /c/d; busybox mv /c/new.txt /c/d/; busybox umount /c", timeout=180.0)
        flog = "".join(log)[at_f:]
        check("sde: 4096-byte clusters" in flog and "sdf: 4096-byte clusters" in flog,
              "the fuzzed volumes mount (the damage is in the FAT, bitmap, directories and data)")
        rc, out = g.sh("echo still-alive")
        text = "".join(log)[at:].lower()
        check(rc == 0 and "still-alive" in out and "panic" not in text and "page fault" not in text,
              "2 fuzzed volumes read, written and unmounted without a crash")
    finally:
        stop(proc)


def main():
    subprocess.run([sys.executable, os.path.join(ROOT, "tools", "mkexfatimg.py")], check=True)
    man = json.load(open(os.path.join(OUT, "manifest.json")))
    accel = ["-accel", "kvm"] if os.access("/dev/kvm", os.R_OK | os.W_OK) else ["-accel", "tcg"]
    work = {}
    for k in ("ahci", "usb"):
        work[k] = os.path.join(OUT, f"run-{k}.img")
        shutil.copyfile(man[k]["image"], work[k])
    bigimg = man["big"]["image"]           # 64 GiB sparse: used in place, never copied
    v = Volume(man["ahci"]["image"], man["ahci"]["start"] * 512)
    big = v.read_at("big.bin", 0, 5 << 20)
    man["blocked"] = v.read_at("blocked.bin", 0, 8192)
    man["frag"] = v.read_at("frag.bin", 0, 300000)
    v.close()
    check(md5(big) == man["ahci"]["files"]["big.bin"], "host: big.bin reads back with exfatimg.py")

    proc, sel, log, g = boot([
        "-M", "q35",
        "-drive", "file=disk.img,format=raw,index=0,media=disk,snapshot=on",
        "-drive", f"file={work['ahci']},format=raw,index=1,media=disk",
        "-drive", f"file={bigimg},format=raw,index=2,media=disk",
        "-device", "qemu-xhci,id=xhci",
        "-drive", f"file={work['usb']},format=raw,if=none,id=stick",
        "-device", "usb-storage,drive=stick,bus=xhci.0"], accel)
    try:
        smokelib.login(proc, sel, log, timeout=120.0)
        rc, out = g.sh("i=0; while [ $i -lt 50 ] && ! busybox grep -q sdd /proc/partitions; "
                       "do busybox sleep 0.2; i=$((i+1)); done; busybox cat /proc/filesystems")
        check("exfat" in out, "/proc/filesystems lists exfat")
        boot_log = "".join(log)
        check("[PART] sdd: FAT filesystem on the whole disk" in boot_log,
              "USB stick: exFAT on the whole disk recognised (no bogus partitions)")

        want_ahci = ahci_session(g, man, big)
        newmark, yeni = big_session(g, man)
        want_usb = usb_session(g, man)
        rc, out = g.sh("busybox mount -t exfat /dev/sda /mnt")
        check(rc != 0, "the boot disk (ext2) is not mountable as exfat")
        rc, out = g.sh("busybox setuidgid user busybox mount -t exfat /dev/sdb1 /mnt")
        check(rc != 0, "unprivileged mount refused")

        # Power off with the USB stick and the big volume mounted read-write.
        smokelib.send(proc, "poweroff\n")
        try:
            proc.wait(timeout=90)
            check(True, "QEMU exited after poweroff")
        except subprocess.TimeoutExpired:
            check(False, "QEMU exited after poweroff")
        text = "".join(log)
        check(not any(x in text for x in ("there were errors", "corrupt chain", "malformed",
                                          "impossible storage", "error at sector")),
              "no I/O errors, corrupt chains or malformed sets logged")
    finally:
        stop(proc)

    # ── Host: fsck.exfat and the independent checker ──────────────────────
    part = extract(work["ahci"], man["ahci"]["start"], man["ahci"]["sectors"], "check-ahci.img")
    fsck(part, "AHCI partition")
    host_check(part, 0, want_ahci, "AHCI partition")
    fsck(work["usb"], "USB stick (left mounted at poweroff)")
    host_check(work["usb"], 0, want_usb, "USB stick")
    fsck(bigimg, "64 GiB volume (left mounted at poweroff)")
    v = Volume(bigimg)
    problems = v.check()
    files, dirs, sizes = v.tree()
    b = man["big"]
    check(not problems, f"64 GiB volume: the checker finds no problems ({problems[:3]})")
    check(sizes.get("huge.bin") == b["huge"], f"huge.bin keeps its 5 GiB length ({sizes.get('huge.bin')})")
    check(md5(v.read_at("huge.bin", 4 * GiB - 8192, 4096)) == newmark,
          "the guest's marker landed at 4 GiB - 8 KiB of huge.bin")
    check(md5(v.read_at("huge.bin", 4 * GiB + GiB // 2, 4096)) == b["marks"][str(4 * GiB + GiB // 2)]
          and md5(v.read_at("huge.bin", b["huge"] - 123, 123)) == b["marks"][str(b["huge"] - 123)]
          and md5(v.read_at("huge.bin", 4 * GiB - 4096, 4096)) == b["marks"][str(4 * GiB - 4096)],
          "the markers past 4 GiB and next to the written one are untouched")
    check(files.get("yeni.bin") == yeni, "the 20 MiB file written on the big volume reads back")
    v.close()

    crafted(man, accel)

    if failures:
        print(f"\n{TAG} {len(failures)} check(s) failed:")
        for f in failures:
            print(f"  - {f}")
        return 1
    print(f"\n{TAG} passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"\n{TAG} failed: {exc}", file=sys.stderr)
        raise SystemExit(1)
