#!/usr/bin/env python3
"""smoke-install — install MaeroOS to a disk from the live ISO, then boot it.

1. live      qemu-system-i386 -M q35: maeros-limine.iso on the AHCI CD, a
             copy of disk.img as sda (the live /disk) and an empty 1 GiB sdb.
             On the desktop (session user "user"): launcher, "install", Enter
             opens a Terminal running `sudo maeros-install`; it takes the
             password, the disk name sdb and "no", and cancels (screenshot
             gui-cancelled.png; no maeros-install left running).  As root on
             the serial console (with a sparse 3 TiB sdc attached too):
             `maeros-install -y /dev/sdc` is refused (the block layer's
             32-bit sector count saturates, so its end is unknown),
             `maeros-install -n /dev/sdb` prints the layout (ext4) and writes
             nothing, as does `-n --ext2` (ext2), `-n` on a 1 GiB + 1 sector sdd puts the backup GPT in
             its exact last sector (BLKGETSIZE64, not /proc/partitions' KiB), `maeros-install -l` lists the disks and marks sda in use,
             `maeros-install -y /dev/sda` is refused, `maeros-install -y
             /dev/sdb` installs; then poweroff.
   ide       qemu -M pc, -kernel, a sparse 130 GiB IDE hdb: the kernel logs
             that LBA28 reaches only 128 GiB of it, and maeros-install
             refuses it (BLKGETSIZE64 > the addressable size).
   ext2-size qemu -M pc, -kernel, an hda holding an 8 MiB ext2 cut to 4 MiB:
             the kernel refuses to mount it at /disk (superblock larger than
             the device) and boots from the initrd.
   host      the target's GPT verifies (sgdisk -v), its root partition is
             an ext4 (extent, flex_bg, metadata_csum, has_journal, dir_index,
             orphan_file) clean under `e2fsck -fn`, its ESP under
             `fsck.fat -n` (each when the tool is installed).
2. bios      qemu-system-i386 -M q35, SeaBIOS, ONLY the installed disk: Limine
             (BIOS) boots it, the kernel takes root=PARTUUID=... as /dev/sda3,
             /proc/mounts has it at /disk, login works, the desktop comes up;
             a file is written to /disk; poweroff.
3. uefi-x64  qemu-system-x86_64 -M q35, OVMF x64, ONLY the installed disk: the
             same checks, and the file from boot 2 is there.
4. host      after both boots (the kernel mounted the root read-write with its
             journal and marked it clean at poweroff): e2fsck -fn clean, no
             needs_recovery, persist.txt there (debugfs).

Output: build/smoke-install/ (serial logs, desktop screenshots, target.img).
OVMF missing: the uefi-x64 boot is SKIP (SMOKE_UEFI_REQUIRE=1 makes it fail).
The UEFI boot runs under TCG unless SMOKE_UEFI_ACCEL=kvm (see smoke_uefi.py).
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

import smokelib
from smoke_gui import Console, Image, Input, Qmp, distinct_colors, pick_accel
from smoke_uefi import X64_NAMES, FW_DIRS, find_firmware, prepare_disk

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
OUT = os.path.join(ROOT, "build", "smoke-install")
ISO = os.path.join(ROOT, "maeros-limine.iso")
TARGET = os.path.join(OUT, "target.img")
TARGET_SIZE = 1024 * 1024 * 1024
ODD_SECTORS = 2 * 1024 * 1024 + 1          # /proc/partitions counts 1 KiB


def start_qemu(name, cmd):
    out = os.path.join(OUT, name)
    os.makedirs(out, exist_ok=True)
    sockdir = tempfile.mkdtemp(prefix="sinst")
    qmp_path = os.path.join(sockdir, "qmp")
    cmd = cmd + ["-vga", "std", *smokelib.qemu_args(cmd[0]), "-serial", "stdio",
                 "-no-reboot", "-qmp", f"unix:{qmp_path},server=on,wait=off"]
    with open(os.path.join(out, "qemu-cmdline.txt"), "w") as f:
        f.write(" ".join(cmd) + "\n")
    proc = subprocess.Popen(cmd, cwd=ROOT, stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            bufsize=0)
    con = Console(proc)
    return out, sockdir, con, Qmp(qmp_path)


def finish(out, sockdir, con, qmp):
    with open(os.path.join(out, "serial.log"), "w") as f:
        f.write(con.text())
    if qmp is not None:
        qmp.close()
    if con.proc.poll() is None:
        con.proc.terminate()
        try:
            con.proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            con.proc.kill()
    shutil.rmtree(sockdir, ignore_errors=True)


def poweroff(con):
    smokelib.send(con.proc, "poweroff\n")
    deadline = time.time() + 60
    while con.sel.get_map() and time.time() < deadline:
        for k, _ in con.sel.select(0.2):
            chunk = os.read(k.fd, 4096).decode("latin1", "replace")
            if chunk:
                con.log.append(chunk)
            else:
                con.sel.unregister(k.fileobj)
    try:
        con.proc.wait(timeout=max(1, deadline - time.time()))
    except subprocess.TimeoutExpired:
        raise AssertionError("QEMU did not exit after poweroff")


def gui_front_end(con, qmp, out):
    """The desktop's "Install MaeroOS" launcher entry, up to the last
    question, answered "no"."""
    inp = Input(qmp)
    m = con.wait_re(r"\[desktop\] ready fb=\d+x\d+ orb=(\d+),(\d+)", timeout=120, start=0)
    time.sleep(1.5)
    at = con.mark()
    inp.click(int(m.group(1)), int(m.group(2)))
    con.wait_re(r"\[desktop\] launcher open", timeout=20, start=at)
    inp.type("install\n")
    con.wait_re(r"\[desktop\] window opened: Terminal", timeout=30, start=at)
    con.wait_re(r"exec '/disk/doas'", timeout=30, start=at)
    time.sleep(1)
    inp.type("user\n")                                      # doas password
    con.wait_re(r"exec '/disk/maeros-install'", timeout=30, start=at)
    time.sleep(1.5)
    inp.type("sdb\n")
    time.sleep(1.5)
    inp.type("no\n")
    time.sleep(1.5)
    ppm = os.path.join(out, "screen.ppm")
    qmp.cmd("screendump", filename=ppm)
    Image.read_ppm(ppm).write_png(os.path.join(out, "gui-cancelled.png"))
    os.remove(ppm)
    ps = con.run("ps", timeout=20)
    if "maeros-install" in ps or "doas" in ps:
        raise AssertionError(f"the GUI installer did not cancel on \"no\":\n{ps}")


def live_install(accel):
    live = os.path.join(OUT, "live.img")
    prepare_disk(live)
    with open(TARGET, "wb") as f:
        f.truncate(TARGET_SIZE)
    huge = os.path.join(OUT, "huge.img")         # sparse: nothing is written
    with open(huge, "wb") as f:
        f.truncate(3 << 40)
    odd = os.path.join(OUT, "odd.img")           # 1 GiB + one sector
    with open(odd, "wb") as f:
        f.truncate(ODD_SECTORS * 512)
    cmd = ["qemu-system-i386", "-M", "q35", "-accel", accel, "-m", "1024M",
           "-drive", f"file={live},format=raw,if=none,id=live",
           "-device", "ide-hd,drive=live,bus=ide.0",
           "-drive", f"file={TARGET},format=raw,if=none,id=target",
           "-device", "ide-hd,drive=target,bus=ide.1",
           "-drive", f"file={huge},format=raw,if=none,id=huge",
           "-device", "ide-hd,drive=huge,bus=ide.3",
           "-drive", f"file={odd},format=raw,if=none,id=odd",
           "-device", "ide-hd,drive=odd,bus=ide.4",
           "-drive", f"file={ISO},format=raw,if=none,id=cd,media=cdrom,readonly=on",
           "-device", "ide-cd,drive=cd,bus=ide.2", "-boot", "d"]
    out, sockdir, con, qmp = start_qemu("live", cmd)
    try:
        con.wait_re(r'\[BOOT\] multiboot2: loader "Limine', timeout=180)
        smokelib.login(con.proc, con.sel, con.log, timeout=180, start=0)
        gui_front_end(con, qmp, out)
        listing = con.run("maeros-install -l", timeout=30)
        if not re.search(r"/dev/sda .*\(in use\)", listing) or not re.search(r"/dev/sdb\s+1024 MiB", listing):
            raise AssertionError(f"maeros-install -l: unexpected listing:\n{listing}")
        refused = con.run("maeros-install -y /dev/sda; echo rc=$?", timeout=30)
        if "mounted filesystem" not in refused or "rc=1" not in refused:
            raise AssertionError(f"installing over the live disk was not refused:\n{refused}")
        big = con.run("maeros-install -y /dev/sdc; echo rc=$?", timeout=30)
        if "2 TiB or larger" not in big or "rc=1" not in big:
            raise AssertionError(f"a 3 TiB disk (saturated size) was not refused:\n{big}")
        dry = con.run("maeros-install -n /dev/sdb; echo rc=$?", timeout=60)
        if "Dry run: nothing written." not in dry or "rc=0" not in dry or "\next4: " not in dry.replace("\r", ""):
            raise AssertionError(f"maeros-install -n failed:\n{dry}")
        dry = con.run("maeros-install -n --ext2 /dev/sdb; echo rc=$?", timeout=60)
        if "Dry run: nothing written." not in dry or "rc=0" not in dry or "\next2: " not in dry.replace("\r", ""):
            raise AssertionError(f"maeros-install -n --ext2 failed:\n{dry}")
        dry = con.run("maeros-install -n /dev/sdd; echo rc=$?", timeout=60)
        if f"backup GPT at LBA {ODD_SECTORS - 1}\n" not in dry.replace("\r", ""):
            raise AssertionError(f"odd-sized disk: backup GPT not at its last LBA "
                                 f"{ODD_SECTORS - 1}:\n{dry}")
        with open(TARGET, "rb") as f:
            if f.read(1 << 20).strip(b"\0"):
                raise AssertionError("maeros-install -n wrote to the target")
        # Scan-to-copy race: after the scan, while the installer waits for
        # "yes", a file is swapped for a symlink to a root-only file.  The
        # copy keeps the scanned owner and mode, so it must not follow the
        # link: the installed file is empty (checked on the host).
        con.run("mkdir -p /tmp/rs/u; echo public > /tmp/rs/u/f; "
                "echo RACE_SECRET > /tmp/rs_secret; chmod 600 /tmp/rs_secret", timeout=30)
        race = con.run("shell -c 'sleep 4; rm /tmp/rs/u/f; ln -s /tmp/rs_secret /tmp/rs/u/f; echo yes' "
                       "| maeros-install --source /tmp/rs /dev/sdd; echo rc=$?", timeout=300)
        if "rc=0" not in race or "/tmp/rs/u/f changed since the scan" not in race:
            raise AssertionError(f"scan/copy race install:\n{race[-3000:]}")
        t0 = time.time()
        log = con.run("maeros-install -y /dev/sdb; echo rc=$?", timeout=900)
        took = time.time() - t0
        m = re.search(r"root=PARTUUID=([0-9a-f-]{36})", log)
        if "rc=0" not in log or not m:
            raise AssertionError(f"maeros-install failed:\n{log[-3000:]}")
        poweroff(con)
        return m.group(1), took
    except Exception:
        print(f"\n[SMOKE-INSTALL] live: last serial output:\n{con.text()[-3000:]}",
              file=sys.stderr)
        raise
    finally:
        finish(out, sockdir, con, qmp)


def ide_lba28(accel):
    """pc + IDE: a 130 GiB disk is reached by LBA28 only up to 128 GiB; the
    kernel says so, BLKGETSIZE64 reports the real size and maeros-install
    refuses the disk (its backup GPT would not be at the end)."""
    big = os.path.join(OUT, "ide130g.img")       # sparse
    with open(big, "wb") as f:
        f.truncate(130 << 30)
    cmd = ["qemu-system-i386", "-M", "pc", "-accel", accel, "-m", "512M",
           "-kernel", "kernel.elf", "-initrd", "initrd.tar",
           "-drive", "file=disk.img,format=raw,index=0,media=disk,snapshot=on",
           "-drive", f"file={big},format=raw,index=1,media=disk,snapshot=on"]
    out, sockdir, con, qmp = start_qemu("ide", cmd)
    try:
        smokelib.login(con.proc, con.sel, con.log, timeout=120, start=0)
        if not re.search(r"\[ATA\]  hdb: 133120 MiB, but LBA28 reaches only the first 131071 MiB",
                         con.text()):
            raise AssertionError("the kernel did not report hdb's LBA28 limit")
        got = con.run("maeros-install -y /dev/hdb; echo rc=$?", timeout=30)
        if "can address only its first" not in got or "rc=1" not in got:
            raise AssertionError(f"a 130 GiB IDE disk was not refused:\n{got}")
        poweroff(con)
        return "130 GiB IDE disk: LBA28 limit reported, install refused"
    except Exception:
        print(f"\n[SMOKE-INSTALL] ide: last serial output:\n{con.text()[-3000:]}",
              file=sys.stderr)
        raise
    finally:
        finish(out, sockdir, con, qmp)
        os.remove(big)


def ext2_oversized(accel):
    """An ext2 superblock that claims more blocks than its disk has (an 8 MiB
    filesystem on a 4 MiB hda) is not mounted at /disk: its block numbers
    would run past the device, or wrap in an LBA28 command."""
    mke2fs = shutil.which("mke2fs") or (os.path.exists("/usr/sbin/mke2fs") and "/usr/sbin/mke2fs")
    if not mke2fs:
        return "SKIP (no mke2fs)"
    img = os.path.join(OUT, "oversized.img")
    with open(img, "wb") as f:
        f.truncate(8 << 20)
    subprocess.run([mke2fs, "-q", "-F", "-t", "ext2", "-b", "1024", img], check=True)
    with open(img, "r+b") as f:
        f.truncate(4 << 20)
    cmd = ["qemu-system-i386", "-M", "pc", "-accel", accel, "-m", "256M",
           "-kernel", "kernel.elf", "-initrd", "initrd.tar",
           "-drive", f"file={img},format=raw,index=0,media=disk"]
    out, sockdir, con, qmp = start_qemu("ext2-size", cmd)
    try:
        con.wait_re(r"\[EXT2\]  Superblock claims 8192 blocks \(8 MiB\) but the device "
                    r"has 4 MiB; not mounting", timeout=60)
        con.wait_re(r"\[BOOT\] Launching /init", timeout=60)
        return "8 MiB ext2 on a 4 MiB disk: not mounted, booted from the initrd"
    except Exception:
        print(f"\n[SMOKE-INSTALL] ext2-size: last serial output:\n{con.text()[-3000:]}",
              file=sys.stderr)
        raise
    finally:
        finish(out, sockdir, con, qmp)
        os.remove(img)


def race_check():
    """The file swapped for a symlink after the scan went in empty."""
    sgdisk = shutil.which("sgdisk")
    debugfs = shutil.which("debugfs") or "/usr/sbin/debugfs"
    if not sgdisk or not os.path.exists(debugfs):
        return "race SKIP (no sgdisk or debugfs)"
    odd = os.path.join(OUT, "odd.img")
    r = subprocess.run([sgdisk, "-i", "3", odd], capture_output=True, text=True)
    first = int(re.search(r"First sector: (\d+)", r.stdout).group(1))
    img = f"{odd}?offset={first * 512}"
    got = subprocess.run([debugfs, "-R", "cat /u/f", img], capture_output=True).stdout
    st = subprocess.run([debugfs, "-R", "stat /u/f", img], capture_output=True, text=True).stdout
    if b"RACE_SECRET" in got or not re.search(r"Size: 0\b", st):
        raise AssertionError(f"race: installed /u/f is {got!r}\n{st[:800]}")
    return "swapped file installed empty"


def host_checks(uuid):
    notes = [race_check()]
    sgdisk = shutil.which("sgdisk")
    parts = {}
    if sgdisk:
        r = subprocess.run([sgdisk, "-v", TARGET], capture_output=True, text=True)
        if "No problems found" not in r.stdout:
            raise AssertionError(f"sgdisk -v: {r.stdout}{r.stderr}")
        for n in (1, 2, 3):
            r = subprocess.run([sgdisk, "-i", str(n), TARGET], capture_output=True, text=True)
            first = int(re.search(r"First sector: (\d+)", r.stdout).group(1))
            last = int(re.search(r"Last sector: (\d+)", r.stdout).group(1))
            puuid = re.search(r"unique GUID: ([0-9A-F-]+)", r.stdout).group(1).lower()
            parts[n] = (first, last, puuid)
        if parts[3][2] != uuid:
            raise AssertionError(f"partition 3 is {parts[3][2]}, limine.conf says {uuid}")
        notes.append("GPT ok")
    else:
        notes.append("sgdisk missing, GPT unchecked")
        return notes

    def extract(n, name):
        first, last, _ = parts[n]
        path = os.path.join(OUT, name)
        with open(TARGET, "rb") as src, open(path, "wb") as dst:
            src.seek(first * 512)
            left = (last - first + 1) * 512
            while left:
                buf = src.read(min(left, 1 << 22))
                dst.write(buf)
                left -= len(buf)
        return path

    dumpe2fs = shutil.which("dumpe2fs") or (os.path.exists("/usr/sbin/dumpe2fs") and "/usr/sbin/dumpe2fs")
    if dumpe2fs:
        img = extract(3, "part3.img")
        st = subprocess.run([dumpe2fs, "-h", img], capture_output=True, text=True).stdout
        os.remove(img)
        feats = re.search(r"Filesystem features:\s*(.*)", st)
        feats = feats.group(1).split() if feats else []
        want = ("has_journal", "dir_index", "orphan_file", "extent", "flex_bg", "metadata_csum")
        if any(f not in feats for f in want):
            raise AssertionError(f"root is not the expected ext4: {feats}")
        notes.append("root is ext4 (" + " ".join(want) + ")")
    for tool, n, args, label in (("e2fsck", 3, ["-fn"], "ext4"), ("fsck.fat", 2, ["-n"], "FAT32")):
        exe = shutil.which(tool) or (os.path.exists("/usr/sbin/" + tool) and "/usr/sbin/" + tool)
        if not exe:
            notes.append(f"{tool} missing, {label} unchecked")
            continue
        img = extract(n, f"part{n}.img")
        r = subprocess.run([exe, *args, img], capture_output=True, text=True)
        os.remove(img)
        if r.returncode != 0:
            raise AssertionError(f"{tool} on partition {n}: rc={r.returncode}\n{r.stdout}{r.stderr}")
        notes.append(f"{label} clean")
    return notes


def root_after_boots(token):
    """The installed root after the kernel has run on it: clean, journal
    empty, the file written in the BIOS boot there."""
    e2fsck = shutil.which("e2fsck") or (os.path.exists("/usr/sbin/e2fsck") and "/usr/sbin/e2fsck")
    sgdisk = shutil.which("sgdisk")
    if not e2fsck or not sgdisk:
        return "SKIP (no e2fsck or sgdisk)"
    r = subprocess.run([sgdisk, "-i", "3", TARGET], capture_output=True, text=True)
    first = int(re.search(r"First sector: (\d+)", r.stdout).group(1))
    last = int(re.search(r"Last sector: (\d+)", r.stdout).group(1))
    img = os.path.join(OUT, "root-after.img")
    with open(TARGET, "rb") as src, open(img, "wb") as dst:
        src.seek(first * 512)
        left = (last - first + 1) * 512
        while left:
            buf = src.read(min(left, 1 << 22))
            dst.write(buf)
            left -= len(buf)
    try:
        r = subprocess.run([e2fsck, "-fn", img], capture_output=True, text=True)
        if r.returncode != 0:
            raise AssertionError(f"e2fsck -fn after the boots: rc={r.returncode}\n{r.stdout[-2000:]}")
        dumpe2fs = shutil.which("dumpe2fs") or "/usr/sbin/dumpe2fs"
        st = subprocess.run([dumpe2fs, "-h", img], capture_output=True, text=True).stdout
        if "needs_recovery" in st or not re.search(r"Filesystem state:\s+clean", st):
            raise AssertionError(f"root not clean after poweroff:\n{st[:1500]}")
        notes = "e2fsck -fn clean, state clean, no needs_recovery"
        if token:
            debugfs = shutil.which("debugfs") or "/usr/sbin/debugfs"
            got = subprocess.run([debugfs, "-R", "cat /persist.txt", img], capture_output=True).stdout
            if token.encode() not in got:
                raise AssertionError(f"debugfs: /persist.txt is {got!r}")
            notes += ", debugfs sees persist.txt"
        return notes
    finally:
        os.remove(img)


def boot_installed(name, qemu, firmware, accel, code=None, vars_src=None,
                   write=None, expect=None):
    cmd = [qemu, "-M", "q35", "-accel", accel, "-m", "1024M",
           "-drive", f"file={TARGET},format=raw,if=none,id=disk",
           "-device", "ide-hd,drive=disk,bus=ide.0"]
    out = os.path.join(OUT, name)
    os.makedirs(out, exist_ok=True)
    if code:
        cmd += ["-drive", f"if=pflash,format=raw,readonly=on,file={code}"]
        if vars_src:
            vars_copy = os.path.join(out, "vars.fd")
            shutil.copyfile(vars_src, vars_copy)
            cmd += ["-drive", f"if=pflash,format=raw,file={vars_copy}"]
    t0 = time.time()
    out, sockdir, con, qmp = start_qemu(name, cmd)
    try:
        m = con.wait_re(r'\[BOOT\] multiboot2: loader "(Limine[^"]*)" \((\w+)\)', timeout=240)
        if m.group(2) != firmware:
            raise AssertionError(f"kernel saw {m.group(2)} firmware, expected {firmware}")
        m = con.wait_re(r"\[BOOT\] root=PARTUUID=([0-9a-f-]+) is (/dev/\w+)", timeout=60)
        root_uuid, root_dev = m.group(1), m.group(2)
        if root_dev != "/dev/sda3":
            raise AssertionError(f"root is {root_dev}, expected /dev/sda3")
        con.wait_re(r"\[BOOT\] Launching /disk/init", timeout=60)
        m = con.wait_re(r"\[FB\]\s+(\d+)x(\d+)@", timeout=60, start=0)
        fb = (int(m.group(1)), int(m.group(2)))
        smokelib.login(con.proc, con.sel, con.log, timeout=180, start=0)
        con.wait_re(r"\[desktop\] ready fb=(\d+)x(\d+)", timeout=120, start=0)
        if not re.search(r"\[EXT2\]  Mounted: .*journal, extents, metadata_csum", con.text()):
            raise AssertionError("the kernel did not mount the root as a journaled ext4")
        mounts = con.run("cat /proc/mounts", timeout=20)
        if not re.search(r"^/dev/sda3 /disk ext4 ", mounts, re.M):
            raise AssertionError(f"/disk is not /dev/sda3:\n{mounts}")
        # The ESP (FAT32 from maeros-install's own FAT writer) through the
        # kernel's vfat driver: limine.conf must name this root.
        esp = con.run("busybox sh -c 'busybox mkdir -p /tmp/esp && "
                      "busybox mount -t vfat -o ro /dev/sda2 /tmp/esp && "
                      "busybox cat /tmp/esp/boot/limine/limine.conf && "
                      "busybox ls /tmp/esp/EFI/BOOT && busybox umount /tmp/esp'",
                      timeout=30)
        if f"root=PARTUUID={root_uuid}" not in esp or "BOOTX64.EFI" not in esp:
            raise AssertionError(f"ESP /dev/sda2 through vfat: limine.conf or "
                                 f"EFI/BOOT not as installed:\n{esp}")
        if expect is not None:
            got = con.run("cat /disk/persist.txt", timeout=20)
            if expect not in got:
                raise AssertionError(f"/disk/persist.txt lost across the reboot: {got!r}")
        if write is not None:
            con.run(f"echo {write} > /disk/persist.txt; sync", timeout=20)
            got = con.run("cat /disk/persist.txt", timeout=20)
            if write not in got:
                raise AssertionError(f"could not write /disk/persist.txt: {got!r}")
        time.sleep(1.5)
        ppm = os.path.join(out, "screen.ppm")
        qmp.cmd("screendump", filename=ppm)
        img = Image.read_ppm(ppm)
        img.write_png(os.path.join(out, "desktop.png"))
        os.remove(ppm)
        colors = distinct_colors(img, (0, 0, img.w, img.h))
        if colors < 50:
            raise AssertionError(f"desktop screen has only {colors} colours")
        at = con.mark()
        poweroff(con)
        if not re.search(r"\[EXT2\]  disk: clean at shutdown", con.text()[at:]):
            raise AssertionError("poweroff did not mark the ext4 root clean")
        return (f"Limine ({firmware}) -> root {root_dev} (ext4, journal), ESP mounted as vfat, "
                f"login, desktop {fb[0]}x{fb[1]} "
                f"({colors} colours), {time.time() - t0:.1f}s")
    except Exception:
        print(f"\n[SMOKE-INSTALL] {name}: last serial output:\n{con.text()[-3000:]}",
              file=sys.stderr)
        raise
    finally:
        finish(out, sockdir, con, qmp)


def main():
    shutil.rmtree(OUT, ignore_errors=True)
    os.makedirs(OUT)
    for f in (ISO, os.path.join(ROOT, "disk.img")):
        if not os.path.exists(f):
            raise RuntimeError(f"{os.path.basename(f)} is missing (make limine-iso disk)")
    accel = pick_accel()
    results, failed = [], []

    print(f"\n[SMOKE-INSTALL] live: installing to an empty AHCI disk (accel={accel})")
    uuid, took = live_install(accel)
    results.append(("live", f"PASS: installed in {took:.0f}s, root=PARTUUID={uuid}"))
    results.append(("host", "PASS: " + ", ".join(host_checks(uuid))))
    print(f"\n[SMOKE-INSTALL] ide: a disk past LBA28 (accel={accel})")
    results.append(("ide", "PASS: " + ide_lba28(accel)))
    print(f"\n[SMOKE-INSTALL] ext2-size: an oversized superblock (accel={accel})")
    r = ext2_oversized(accel)
    results.append(("ext2-size", r if r.startswith("SKIP") else "PASS: " + r))

    token = "persist-%08x" % int.from_bytes(os.urandom(4), "little")
    print(f"\n[SMOKE-INSTALL] bios: booting the installed disk alone (accel={accel})")
    try:
        results.append(("bios", "PASS: " + boot_installed(
            "bios", "qemu-system-i386", "BIOS", accel, write=token)))
    except Exception as exc:
        results.append(("bios", f"FAIL: {exc}"))
        failed.append("bios")

    code, vars_src = find_firmware("OVMF_X64", X64_NAMES,
                                   [d for d in FW_DIRS if "ia32" not in d])
    if not code or not shutil.which("qemu-system-x86_64"):
        results.append(("uefi-x64", "SKIP (no OVMF x64 or qemu-system-x86_64)"))
        if os.environ.get("SMOKE_UEFI_REQUIRE") == "1":
            failed.append("uefi-x64")
    else:
        acc = os.environ.get("SMOKE_UEFI_ACCEL", "tcg")
        print(f"\n[SMOKE-INSTALL] uefi-x64: booting the installed disk alone (accel={acc})")
        try:
            results.append(("uefi-x64", "PASS: " + boot_installed(
                "uefi-x64", "qemu-system-x86_64", "UEFI", acc, code, vars_src,
                expect=None if "bios" in failed else token) +
                ("" if "bios" in failed else f", {token} survived the reboot")))
        except Exception as exc:
            results.append(("uefi-x64", f"FAIL: {exc}"))
            failed.append("uefi-x64")

    try:
        results.append(("host-after", "PASS: " + root_after_boots(None if "bios" in failed else token)))
    except Exception as exc:
        results.append(("host-after", f"FAIL: {exc}"))
        failed.append("host-after")

    print()
    for name, msg in results:
        print(f"[SMOKE-INSTALL] {name}: {msg}")
    if failed:
        print(f"[SMOKE-INSTALL] failed: {' '.join(failed)}; see "
              f"{os.path.relpath(OUT, ROOT)}/<name>/serial.log")
        return 1
    print("[SMOKE-INSTALL] passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"\n[SMOKE-INSTALL] failed: {exc}", file=sys.stderr)
        raise SystemExit(1)
