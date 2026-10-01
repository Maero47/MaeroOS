#!/usr/bin/env python3
"""smoke-vfat: the read-write FAT driver on AHCI, USB mass storage and IDE.

Builds test volumes with tools/mkvfatimg.py (mkfs.fat + mtools: long and
Turkish names, nested directories, a 300-entry directory, a 5 MiB file) and
boots q35 with the boot disk on AHCI (sda), a FAT32 disk on AHCI (sdb, MBR
partition sdb1), a FAT16 + FAT12 disk on AHCI (sdc1, sdc2) and a FAT32 USB
stick without a partition table on a qemu-xhci usb-storage device (sdd, the
next SCSI disk name).  In the guest, with busybox:

  * every file's md5 matches the host's, names come back in UTF-8, lookups
    are case-insensitive (long and 8.3 names), a large directory lists
  * mkdir, create, write (5 MiB copy, a 300-cluster file, many entries so a
    directory grows), append, write past the end (zero fill), truncate,
    rename (same directory, across directories, a directory with contents,
    over an existing file, a case-only change), unlink, rm -r, rmdir
  * the guest reads its own writes back after umount + mount
  * ro mounts refuse writes (EROFS), remount,rw makes them writable
  * the USB stick is mounted, written, and left mounted: `poweroff` must
    flush it and mark it clean

then, on the host, after QEMU has powered off: `fsck.fat -n` is clean for
every volume the guest wrote, and mtools sees exactly the files the guest
should have left, with the right contents.  Also boots the FAT32 disk as the
IDE slave (hdb1) on the PC machine for a read check.
"""
import hashlib
import json
import os
import re
import selectors
import shutil
import subprocess
import sys
import tempfile

import smoke_gui

import smokelib

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
OUT = os.path.join(ROOT, "build", "vfattest")
PROMPT = smokelib.PROMPT
TAG = "[SMOKE-VFAT]"
ENV = dict(os.environ, LC_ALL="C.UTF-8", MTOOLS_SKIP_CHECK="1")
ENV["PATH"] = os.path.expanduser("~/opt/bin") + ":" + ENV.get("PATH", "") + ":/usr/sbin:/sbin"

failures = []


def check(cond, what):
    print(f"\n{TAG} {'ok' if cond else 'FAIL'}: {what}")
    if not cond:
        failures.append(what)


def u8(text):
    """Console text (read as latin1) back to UTF-8."""
    return text.encode("latin1").decode("utf-8", "replace")


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
        """{relative path: md5} of every regular file below `mnt`."""
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


def md5(b):
    return hashlib.md5(b).hexdigest()


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
    """The volume as its own file (fsck.fat has no offset option)."""
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
    r = subprocess.run(["fsck.fat", "-n", path], capture_output=True, text=True, env=ENV)
    text = r.stdout + r.stderr
    print(text)
    check(r.returncode == 0 and "Dirty bit" not in text and "differ" not in text.lower()
          and "free cluster summary" not in text.lower(),
          f"fsck.fat -n {what} is clean (rc={r.returncode})")


def mtools_tree(path):
    """{path: md5} of every file, read with mtools."""
    r = subprocess.run(["mdir", "-i", path, "-/", "-b", "::/"], capture_output=True, env=ENV)
    res = {}
    for line in r.stdout.decode("utf-8", "replace").splitlines():
        line = line.strip()
        if not line.startswith("::/") or line.endswith("/"):
            continue
        rel = line[3:]
        t = subprocess.run(["mtype", "-i", path, "::/" + rel], capture_output=True, env=ENV)
        if t.returncode == 0:
            res[rel] = md5(t.stdout)
    # mdir -b lists directories too; drop the ones that are not files
    dirs = subprocess.run(["mdir", "-i", path, "-/", "-a", "::/"], capture_output=True, env=ENV)
    return res, dirs.stdout.decode("utf-8", "replace")


# ── The guest session ────────────────────────────────────────────────────

def crafted(man, accel):
    """Malformed long-name sequences and randomly corrupted volumes: the
    driver must read what it can, and never crash."""
    work = []
    for i, src in enumerate([man["crafted"]["image"]] + man["fuzz"]):
        dst = os.path.join(OUT, f"run-crafted-{i}.img")
        shutil.copyfile(src, dst)
        work.append(dst)
    args = ["-M", "q35", "-drive", "file=disk.img,format=raw,index=0,media=disk,snapshot=on"]
    for i, w in enumerate(work):
        args += ["-drive", f"file={w},format=raw,index={i + 1},media=disk"]
    proc, sel, log, g = boot(args, accel)
    try:
        smokelib.login(proc, sel, log, timeout=120.0)
        at = smokelib.mark(log)
        rc, out = g.sh("busybox mkdir -p /c && busybox mount -t vfat -o ro /dev/sdb /c && "
                       "cd /c && busybox find . -type f | busybox sort | "
                       "while read -r f; do busybox md5sum \"$f\"; done")
        got = sorted(re.findall(r"^([0-9a-f]{32})  ", out, re.M))
        check(rc == 0 and got == man["crafted"]["md5s"],
              f"crafted LFN sequences (ordinal 0 after a complete set, ordinals past 20, "
              f"bad checksum, orphan set): all {len(got)} files still read")
        rc, out = g.sh("busybox ls /c | busybox wc -l; busybox umount /c")
        check(rc == 0, "crafted volume lists and unmounts")
        names = "abcdefghij"
        for i in range(1, len(work)):
            dev = f"/dev/sd{names[i + 1]}"
            g.sh(f"busybox mkdir -p /f; busybox mount -t vfat -o ro {dev} /f && "
                 "busybox find /f -maxdepth 6 | busybox wc -l; "
                 "busybox find /f -maxdepth 6 -type f -exec busybox cat {} + >/dev/null 2>&1; "
                 "busybox umount /f", timeout=180.0)
            g.sh(f"busybox mount -t vfat {dev} /f && "
                 "busybox rm -rf /f/* 2>/dev/null; echo new > /f/new.txt; "
                 "busybox mkdir /f/d; busybox mv /f/new.txt /f/d/; busybox umount /f", timeout=180.0)
        rc, out = g.sh("echo still-alive")
        text = "".join(log)[at:].lower()
        check(rc == 0 and "still-alive" in out and "panic" not in text and "page fault" not in text,
              f"{len(work) - 1} fuzzed volumes read, written and unmounted without a crash")
    finally:
        stop(proc)


def hotplug(man, usb_img, accel):
    """Unplug the stick while it is mounted, then plug it back in."""
    sockdir = tempfile.mkdtemp(prefix="svfat")
    qmp_path = os.path.join(sockdir, "qmp")
    proc, sel, log, g = boot([
        "-M", "q35",
        "-drive", "file=disk.img,format=raw,index=0,media=disk,snapshot=on",
        "-device", "qemu-xhci,id=xhci",
        "-drive", f"file={usb_img},format=raw,if=none,id=stick,snapshot=on",
        "-device", "usb-storage,drive=stick,bus=xhci.0,id=ustick",
        "-qmp", f"unix:{qmp_path},server=on,wait=off"], accel)
    qmp = None
    try:
        smokelib.login(proc, sel, log, timeout=120.0)
        qmp = smoke_gui.Qmp(qmp_path)
        rc, out = g.sh("i=0; while [ $i -lt 50 ] && ! busybox grep -q sdb /proc/partitions; "
                       "do busybox sleep 0.2; i=$((i+1)); done; busybox mkdir -p /usb && "
                       "busybox mount -t vfat /dev/sdb /usb && busybox cat /usb/Yedek/usb-hello.txt")
        check(rc == 0 and "hello from the USB stick" in out,
              "hot-plug boot: the stick is sdb next to one AHCI disk, mounted")
        at = smokelib.mark(log)
        qmp.cmd("device_del", id="ustick")
        smokelib.wait_for(proc, sel, "/dev/usbdisk0 removed", log, 20.0, at)
        rc, out = g.sh("busybox grep -c sdb /proc/partitions; busybox md5sum /usb/Belgeler/photo.jpg; "
                       "echo x > /usb/new.txt")
        want = man["usb"]["files"]["Belgeler/photo.jpg"]
        check(rc != 0 and want not in out and re.search(r"^0$", out, re.M) is not None,
              f"unplugged: sdb leaves /proc/partitions, reads and writes fail ({out.strip()[-160:]!r})")
        rc, out = g.sh("busybox umount /usb")
        check(rc == 0, "the dead mount unmounts")
        at = smokelib.mark(log)
        qmp.cmd("blockdev-add", driver="raw", **{"node-name": "stick2"},
                file={"driver": "file", "filename": usb_img}, **{"read-only": True})
        qmp.cmd("device_add", driver="usb-storage", drive="stick2", id="ustick", bus="xhci.0")
        smokelib.wait_for(proc, sel, "/dev/usbdisk0 is /dev/sdb", log, 30.0, at)
        rc, out = g.sh("busybox mount -t vfat -o ro /dev/sdb /usb && busybox md5sum /usb/Belgeler/photo.jpg "
                       "&& busybox umount /usb")
        check(rc == 0 and want in out, "plugged back in: sdb again, mounts and reads")
    finally:
        if qmp:
            qmp.close()
        stop(proc)
        shutil.rmtree(sockdir, ignore_errors=True)


def fat32_session(g, man, big):
    f32 = man["fat32"]
    rc, out = g.sh("busybox mkdir -p /mnt && busybox mount -t vfat /dev/sdb1 /mnt")
    check(rc == 0, f"mount -t vfat /dev/sdb1 /mnt (rc={rc}, {out.strip()!r})")
    rc, out = g.sh("busybox grep sdb1 /proc/mounts")
    check("/dev/sdb1 /mnt vfat rw" in out, "/proc/mounts: /dev/sdb1 /mnt vfat rw")
    rc, out = g.sh("cd /mnt && busybox find . -maxdepth 1 | busybox sed s,^./,,")
    names = set(out.split("\n"))
    check({"Türkçe ğüşıöç İI.txt", "MixedCase.Txt", "This is a long file name.text",
           "readme.md", "hello.txt", "Klasör", "big.bin"} <= names,
          f"long, Turkish and lower-case 8.3 names list as on the host ({sorted(names)[:9]})")
    compare_tree(g.md5_tree("/mnt"), f32["files"], "FAT32 on AHCI, read")
    rc, out = g.sh("busybox ls /mnt/many | busybox wc -l")
    check(out.strip().endswith("300"), "the 300-entry directory lists completely")
    rc, out = g.sh("busybox cat /mnt/HELLO.TXT \"/mnt/klasör/ŞARKı SÖZLERI.MP3\" /mnt/THISIS~1.TEX "
                   "| busybox wc -c")
    check(rc == 0 and out.strip().endswith(str(17 + 70000 + 1000)),
          "lookups are case-insensitive (incl. Turkish letters) and find 8.3 aliases")
    rc, out = g.sh("busybox stat -c \"%s %F\" /mnt/big.bin /mnt/Klasör")
    check("5242880 regular file" in out and "directory" in out, "stat: size and type")

    # ── writes ───────────────────────────────────────────────────────
    cmds = [
        ("mkdir", "busybox mkdir \"/mnt/Yeni Klasör\""),
        ("create + write 109 KiB", "busybox seq 1 20000 > \"/mnt/Yeni Klasör/çalışma notları.txt\""),
        ("5 MiB copy", "busybox cp /mnt/big.bin \"/mnt/Kopya büyük.bin\""),
        ("8.3 names", "echo short > /mnt/short.txt && echo UP > /mnt/UPPER.TXT"),
        ("150 entries (the directory grows)", "busybox mkdir \"/mnt/Yeni Klasör/çok\" && "
         "for i in $(busybox seq 1 150); do echo $i > \"/mnt/Yeni Klasör/çok/dosya numarası $i.txt\"; done"),
        ("rename across directories", "busybox mv /mnt/hello.txt \"/mnt/Klasör/merhaba dünya.txt\""),
        ("move a directory with contents", "busybox mv \"/mnt/Klasör/alt dizin\" \"/mnt/Yeni Klasör/taşınan dizin\""),
        ("rename over an existing file", "busybox mv /mnt/readme.md /mnt/MixedCase.Txt"),
        ("case-only rename", "busybox mv /mnt/short.txt /mnt/SHORT.txt"),
        ("unlink", "busybox rm \"/mnt/This is a long file name.text\""),
        ("rm -r of 300 entries", "busybox rm -r /mnt/many"),
        ("append", "echo appended >> \"/mnt/Türkçe ğüşıöç İI.txt\""),
        ("write past the end", "echo -n X | busybox dd of=/mnt/hole.bin bs=1 seek=100000 2>/dev/null"),
        ("truncate", "busybox cp /mnt/big.bin /mnt/trunc.bin && busybox truncate -s 12345 /mnt/trunc.bin"),
        ("mkdir + rmdir", "busybox mkdir /mnt/gone && busybox rmdir /mnt/gone"),
        ("rename onto \"name.\" (trailing dot) replaces \"name\"",
         "echo first > /mnt/dotA && echo second > /mnt/dotB && busybox mv /mnt/dotA /mnt/dotB. && "
         "busybox test ! -e /mnt/dotA && busybox test \"$(busybox cat /mnt/dotB)\" = first && "
         "busybox test $(cd /mnt && busybox find . -maxdepth 1 -iname dotb | busybox wc -l) = 1"),
    ]
    for what, cmd in cmds:
        rc, out = g.sh(cmd, timeout=300.0)
        check(rc == 0, f"{what} ({out.strip()[-200:]!r})")
    rc, out = g.sh("busybox rmdir \"/mnt/Yeni Klasör\"")
    check(rc != 0, "rmdir of a non-empty directory refused")
    rc, out = g.sh("busybox mv \"/mnt/Yeni Klasör\" \"/mnt/Yeni Klasör/çok/içine\"")
    check(rc != 0, f"a directory cannot move below itself ({out.strip()!r})")
    # statfs: free clusters drop by exactly the clusters a 5 MiB file takes.
    def free():
        rc, out = g.sh("busybox stat -f -c \"%T %S %b %f\" /mnt")
        m = re.search(r"(\S+) (\d+) (\d+) (\d+)", out)
        return (m.group(1), int(m.group(2)), int(m.group(3)), int(m.group(4))) if m else None
    f0 = free()
    g.sh("busybox cp /mnt/big.bin /mnt/statfs-probe.bin")
    f1 = free()
    g.sh("busybox rm /mnt/statfs-probe.bin")
    f2 = free()
    check(f0 and f1 and f2 and f0[0] == "msdos" and f0[1] == 512 and f0[3] - f1[3] == 10240
          and f2[3] == f0[3], f"statfs: type msdos, 512-byte blocks, 10240 used by 5 MiB and "
                              f"given back ({f0}, {f1}, {f2})")
    # A file unlinked while open stays readable; its clusters go at close.
    rc, out = g.sh("busybox cp /mnt/big.bin /mnt/open.bin && exec 3< /mnt/open.bin && "
                   "busybox rm /mnt/open.bin && busybox test ! -e /mnt/open.bin && "
                   "busybox md5sum <&3 && exec 3<&-")
    check(rc == 0 and f32["files"]["big.bin"] in out,
          "a file unlinked while open still reads back whole")
    f3 = free()
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

    want = dict(f32["files"])
    hello = want.pop("hello.txt")
    want["Klasör/merhaba dünya.txt"] = hello
    want["Yeni Klasör/taşınan dizin/derin dosya.txt"] = want.pop("Klasör/alt dizin/derin dosya.txt")
    want["MixedCase.Txt"] = want.pop("readme.md")
    del want["This is a long file name.text"]
    for p in [p for p in want if p.startswith("many/")]:
        del want[p]
    want["Türkçe ğüşıöç İI.txt"] = md5("Merhaba dünya: çğıöşü ÇĞİÖŞÜ\n".encode() + b"appended\n")
    want["Yeni Klasör/çalışma notları.txt"] = md5("".join(f"{i}\n" for i in range(1, 20001)).encode())
    want["Kopya büyük.bin"] = want["big.bin"]
    want["SHORT.txt"] = md5(b"short\n")
    want["UPPER.TXT"] = md5(b"UP\n")
    for i in range(1, 151):
        want[f"Yeni Klasör/çok/dosya numarası {i}.txt"] = md5(f"{i}\n".encode())
    want["hole.bin"] = md5(b"\0" * 100000 + b"X")
    want["trunc.bin"] = md5(big[:12345])
    want["dotB"] = md5(b"first\n")
    compare_tree(g.md5_tree("/mnt"), want, "FAT32 after the writes, read in the guest")

    rc, out = g.sh("cd /mnt && busybox umount /mnt")
    check(rc != 0, "umount refused while a cwd is inside")
    rc, out = g.sh("busybox umount /mnt")
    check(rc == 0, f"umount /mnt ({out.strip()!r})")
    rc, out = g.sh("busybox mount -t vfat -o ro /dev/sdb1 /mnt")
    check(rc == 0, "mount again, read-only")
    compare_tree(g.md5_tree("/mnt"), want, "FAT32 after umount + mount")
    rc, out = g.sh("echo nope > /mnt/nope.txt")
    check(rc != 0 and "Read-only" in out, f"ro mount: create refused with EROFS ({out.strip()!r})")
    rc, out = g.sh("busybox rm /mnt/SHORT.txt")
    check(rc != 0 and "Read-only" in out, "ro mount: unlink refused with EROFS")
    rc, out = g.sh("busybox mount -o remount,rw /mnt && echo yes > /mnt/remounted.txt && "
                   "busybox grep sdb1 /proc/mounts")
    check(rc == 0 and "vfat rw" in out, "remount,rw makes it writable")
    want["remounted.txt"] = md5(b"yes\n")
    rc, out = g.sh("busybox mount -t vfat /dev/sdb1 /tmp")
    check(rc != 0, "the same device cannot be mounted twice")
    rc, out = g.sh("busybox umount /mnt")
    check(rc == 0, "umount /mnt")
    return want


def fat16_session(g, man):
    rc, out = g.sh("busybox mkdir -p /mnt16 /mnt12 && busybox mount -t vfat -o ro /dev/sdc1 /mnt16 && "
                   "busybox mount -t msdos -o ro /dev/sdc2 /mnt12")
    check(rc == 0, f"mount FAT16 sdc1 and FAT12 sdc2 read-only ({out.strip()!r})")
    compare_tree(g.md5_tree("/mnt16"), man["fat16"]["files"], "FAT16, read")
    compare_tree(g.md5_tree("/mnt12"), man["fat12"]["files"], "FAT12, read")
    # Fill the FAT12 volume: ENOSPC, then everything given back.
    rc, out = g.sh("busybox mount -o remount,rw /mnt12 && busybox stat -f -c %f /mnt12 && "
                   "busybox dd if=/dev/zero of=/mnt12/fill bs=4096; "
                   "busybox stat -f -c %f /mnt12; busybox rm /mnt12/fill && busybox stat -f -c %f /mnt12",
                   timeout=300.0)
    nums = [int(x) for x in re.findall(r"^(\d+)$", out, re.M)]
    check("No space" in out and len(nums) == 3 and nums[1] == 0 and nums[2] == nums[0],
          f"FAT12 filled up: ENOSPC, 0 free, all free again after rm ({nums})")
    # Writes on both: FAT16 entries, and FAT12's 12-bit entries that share bytes.
    rc, out = g.sh("busybox mount -o remount,rw /mnt16 && "
                   "busybox cp \"/mnt16/sub/data.bin\" /mnt16/sub/copy.bin && "
                   "busybox cp \"/mnt12/odd cluster chain.bin\" \"/mnt12/kopya ı.bin\" && "
                   "busybox rm /mnt12/fat12.txt && busybox mkdir /mnt12/dir && "
                   "busybox seq 1 3000 > /mnt12/dir/seq.txt && "
                   "busybox umount /mnt16 && busybox umount /mnt12")
    check(rc == 0, f"FAT16 and FAT12 written and unmounted ({out.strip()[-200:]!r})")
    w16 = dict(man["fat16"]["files"])
    w16["sub/copy.bin"] = w16["sub/data.bin"]
    w12 = dict(man["fat12"]["files"])
    w12["kopya ı.bin"] = w12["odd cluster chain.bin"]
    del w12["fat12.txt"]
    w12["dir/seq.txt"] = md5("".join(f"{i}\n" for i in range(1, 3001)).encode())
    return w16, w12


def usb_session(g, man):
    rc, out = g.sh("busybox cat /proc/partitions")
    check(re.search(r"\bsdd\b", out) is not None, "the USB stick is sdd in /proc/partitions")
    rc, out = g.sh("busybox mkdir -p /usb && busybox mount /dev/sdd /usb && busybox grep sdd /proc/mounts")
    check(rc == 0 and "/dev/sdd /usb vfat rw" in out,
          f"mount /dev/sdd /usb without -t: vfat found through /proc/filesystems ({out.strip()!r})")
    compare_tree(g.md5_tree("/usb"), man["usb"]["files"], "USB stick, read")
    rc, out = g.sh("busybox seq 1 50000 > \"/usb/USB-yazıldı.txt\" && "
                   "busybox mkdir /usb/Yedek && "
                   "busybox cp /usb/Belgeler/photo.jpg /usb/Yedek/fotoğraf.jpg && "
                   "busybox mv /usb/usb-hello.txt /usb/Yedek/ && busybox rm /usb/DOS.TXT")
    check(rc == 0, f"USB: create, copy, mkdir, rename, unlink ({out.strip()[-200:]!r})")
    want = dict(man["usb"]["files"])
    want["USB-yazıldı.txt"] = md5("".join(f"{i}\n" for i in range(1, 50001)).encode())
    want["Yedek/fotoğraf.jpg"] = want["Belgeler/photo.jpg"]
    want["Yedek/usb-hello.txt"] = want.pop("usb-hello.txt")
    del want["DOS.TXT"]
    compare_tree(g.md5_tree("/usb"), want, "USB stick after the writes, read in the guest")
    return want


def main():
    subprocess.run([sys.executable, os.path.join(ROOT, "tools", "mkvfatimg.py")], check=True)
    man = json.load(open(os.path.join(OUT, "manifest.json")))
    accel = ["-accel", "kvm"] if os.access("/dev/kvm", os.R_OK | os.W_OK) else ["-accel", "tcg"]
    # The guest writes to working copies; the originals stay for reference.
    work = {}
    for k in ("fat32", "usb", "fat16"):
        work[k] = os.path.join(OUT, f"run-{k}.img")
        shutil.copyfile(man[k]["image"], work[k])
    big = subprocess.run(["mtype", "-i", f"{man['fat32']['image']}@@{man['fat32']['start'] * 512}",
                          "::/big.bin"], capture_output=True, env=ENV, check=True).stdout
    check(md5(big) == man["fat32"]["files"]["big.bin"], "host: big.bin reads back with mtools")

    proc, sel, log, g = boot([
        "-M", "q35",
        "-drive", "file=disk.img,format=raw,index=0,media=disk,snapshot=on",
        "-drive", f"file={work['fat32']},format=raw,index=1,media=disk",
        "-drive", f"file={work['fat16']},format=raw,index=2,media=disk",
        "-device", "qemu-xhci,id=xhci",
        "-drive", f"file={work['usb']},format=raw,if=none,id=stick",
        "-device", "usb-storage,drive=stick,bus=xhci.0"], accel)
    try:
        smokelib.login(proc, sel, log, timeout=120.0)
        boot_log = "".join(log)
        check("[PART] sdb1:" in boot_log and "[PART] sdc2:" in boot_log,
              "MBR partitions sdb1 (FAT32) and sdc1/sdc2 (FAT16/FAT12) found")
        # The stick enumerates in kusbd, after the login prompt at the latest.
        rc, out = g.sh("i=0; while [ $i -lt 50 ] && ! busybox grep -q sdd /proc/partitions; "
                       "do busybox sleep 0.2; i=$((i+1)); done; busybox cat /proc/partitions")
        boot_log = "".join(log)
        check("/dev/usbdisk0 is /dev/sdd" in boot_log, "USB stick registered as /dev/sdd")
        check("[PART] sdd: FAT filesystem on the whole disk" in boot_log,
              "USB stick: FAT on the whole disk recognised (no bogus partitions)")

        want32 = fat32_session(g, man, big)
        want16, want12 = fat16_session(g, man)
        wantusb = usb_session(g, man)

        rc, out = g.sh("busybox mount -t vfat /dev/sda /mnt")
        check(rc != 0, "the boot disk (ext2) is not mountable as vfat")
        rc, out = g.sh("busybox setuidgid user busybox mount -t vfat /dev/sdb1 /mnt")
        check(rc != 0, "unprivileged mount refused")

        # Power off with the USB stick still mounted read-write.
        at = smokelib.mark(log)
        smokelib.send(proc, "poweroff\n")
        try:
            proc.wait(timeout=60)
            check(True, "QEMU exited after poweroff")
        except subprocess.TimeoutExpired:
            check(False, "QEMU exited after poweroff")
        text = "".join(log)
        check("I/O errors" not in text and "corrupt chain" not in text,
              "no I/O errors or corrupt chains logged")
    finally:
        stop(proc)

    # ── Host: fsck and mtools on what the guest left ────────────────────
    vols = [("fat32", work["fat32"], man["fat32"], want32),
            ("fat16", work["fat16"], man["fat16"], want16),
            ("fat12", work["fat16"], man["fat12"], want12),
            ("usb", work["usb"], man["usb"], wantusb)]
    for name, img, m, want in vols:
        part = extract(img, m["start"], m["sectors"], f"check-{name}.img")
        fsck(part, name)
        got, listing = mtools_tree(part)
        compare_tree(got, want, f"{name}: mtools on the host")

    # ── PC machine: the same FAT32 disk as the IDE slave ────────────────
    proc, sel, log, g = boot([
        "-drive", "file=disk.img,format=raw,index=0,media=disk,snapshot=on",
        "-drive", f"file={work['fat32']},format=raw,index=1,media=disk,snapshot=on"], accel)
    try:
        smokelib.login(proc, sel, log, timeout=120.0)
        rc, out = g.sh("busybox mkdir -p /mnt && busybox mount -t vfat -o ro /dev/hdb1 /mnt")
        check(rc == 0, "IDE: mount -t vfat /dev/hdb1")
        compare_tree(g.md5_tree("/mnt"), want32, "IDE: FAT32 as written by the q35 guest")
    finally:
        stop(proc)

    hotplug(man, work["usb"], accel)
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
