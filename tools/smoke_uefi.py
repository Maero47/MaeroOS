#!/usr/bin/env python3
"""smoke-uefi — boot maeros-limine.iso (Limine, Multiboot 2) under each firmware.

The Limine ISO is hybrid: one image for legacy BIOS and UEFI.  This boots it,
with a copy of disk.img, under

  bios        qemu-system-i386, SeaBIOS
  uefi-x64    qemu-system-x86_64, OVMF x64 (most real UEFI PCs): Limine's
              64-bit BOOTX64.EFI hands the 32-bit kernel over in protected mode
  uefi-ia32   qemu-system-i386, OVMF IA32 (BOOTIA32.EFI)

and for each one checks, from the serial console: the kernel saw Multiboot 2
from Limine on the expected firmware (the "(UEFI)"/"(BIOS)" verdict comes
from the EFI tags in the boot info) with an ACPI RSDP tag; the framebuffer
came from the boot info (VBE or GOP) and its resolution is logged; getty's
login works; the desktop starts on that framebuffer; and a QMP screendump is
that size and not blank; ACPI came up on the RSDP from the boot info (the
BIOS-area scan is not used on UEFI), and `poweroff` makes QEMU exit through S5.

Firmware lookup: $OVMF_X64_CODE / $OVMF_IA32_CODE (with ..._VARS), else the
usual distro paths and tools/setup-linux.sh's ~/opt/hostpkgs.  A firmware
that is not installed is reported as SKIP (bios always runs); set
SMOKE_UEFI_REQUIRE=1 to make a missing one fail instead.  SMOKE_UEFI_ONLY=
"uefi-x64 bios" picks configurations.  Output: build/smoke-uefi/<name>/
(serial.log, desktop.png, qemu-cmdline.txt).

Accelerator: bios uses KVM when /dev/kvm is usable (SMOKE_GUI_ACCEL=tcg|kvm
overrides, as for smoke-gui).  The UEFI runs default to TCG
(SMOKE_UEFI_ACCEL=kvm overrides): OVMF reads the CD with one port exit per
16-bit word, so under KVM Limine needs ~3-5 minutes to load the 90 MB initrd,
against ~8 s under TCG.  The kernel itself runs normally under either.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

import smokelib
from smoke_gui import (AUTOSTART_MARKERS, Console, Image, Qmp,
                       distinct_colors, find_debugfs, pick_accel)

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
OUT = os.path.join(ROOT, "build", "smoke-uefi")
ISO = os.path.join(ROOT, "maeros-limine.iso")

HOSTPKGS = os.environ.get("HOSTPKGS_DIR",
                          os.path.join(os.path.expanduser("~"), "opt", "hostpkgs"))
FW_DIRS = [os.path.join(HOSTPKGS, "usr/share/OVMF"), "/usr/share/OVMF",
           "/usr/share/ovmf", "/usr/share/edk2/x64", "/usr/share/edk2/ia32",
           "/usr/share/edk2/ovmf", "/usr/share/edk2/ovmf-ia32",
           "/usr/share/edk2-ovmf/x64", "/usr/share/edk2-ovmf/ia32",
           "/usr/share/qemu"]
# (code, vars) file name pairs, most specific first.
X64_NAMES = [("OVMF_CODE_4M.fd", "OVMF_VARS_4M.fd"), ("OVMF_CODE.fd", "OVMF_VARS.fd"),
             ("OVMF_CODE.4m.fd", "OVMF_VARS.4m.fd"), ("edk2-x86_64-code.fd", None)]
IA32_NAMES = [("OVMF32_CODE_4M.fd", "OVMF32_VARS_4M.fd"),
              ("OVMF32_CODE.fd", "OVMF32_VARS.fd"),
              ("OVMF_CODE.fd", "OVMF_VARS.fd"), ("edk2-i386-code.fd", "edk2-i386-vars.fd")]


def find_firmware(env, names, dirs):
    code = os.environ.get(env + "_CODE")
    if code:
        return code, os.environ.get(env + "_VARS")
    for d in dirs:
        for c, v in names:
            if os.path.isfile(os.path.join(d, c)):
                vp = os.path.join(d, v) if v and os.path.isfile(os.path.join(d, v)) else None
                return os.path.join(d, c), vp
    return None, None


def configs():
    x64 = find_firmware("OVMF_X64", X64_NAMES,
                        [d for d in FW_DIRS if "ia32" not in d])
    ia32 = find_firmware("OVMF_IA32", IA32_NAMES,
                         [d for d in FW_DIRS if "x64" not in d and d != "/usr/share/ovmf"])
    # Distro dirs that hold both builds keep the IA32 one as OVMF32_*; an
    # "OVMF_CODE.fd" found for IA32 in a shared dir is the x64 one.
    if ia32[0] and x64[0] and os.path.samefile(ia32[0], x64[0]):
        ia32 = (None, None)
    return [
        ("bios", "qemu-system-i386", None, None, "BIOS"),
        ("uefi-x64", "qemu-system-x86_64", x64[0], x64[1], "UEFI"),
        ("uefi-ia32", "qemu-system-i386", ia32[0], ia32[1], "UEFI"),
    ]


def prepare_disk(dst):
    shutil.copyfile(os.path.join(ROOT, "disk.img"), dst)
    debugfs = find_debugfs()
    if not debugfs:
        raise RuntimeError("debugfs (e2fsprogs) is needed to prepare the disk")
    for marker in AUTOSTART_MARKERS:
        subprocess.run([debugfs, "-w", "-R", f"rm {marker}", dst],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def acpi_poweroff(con):
    """uACPI must have come up on the RSDP the loader passed (on UEFI there
    is no BIOS area to scan), and `poweroff` must end QEMU through S5."""
    text = con.text()
    if "[ACPI] RSDP at" not in text or "(from the boot loader)" not in text:
        raise AssertionError("ACPI did not take the RSDP from the boot loader")
    if "[ACPI] ready" not in text:
        raise AssertionError("ACPI did not come up")
    start = con.mark()
    smokelib.send(con.proc, "poweroff\n")
    # Console.pump raises once QEMU is gone; read until the line closes.
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
    after = con.text()[start:]
    if "[ACPI] powering off" not in after or "S5 failed" in after:
        raise AssertionError("poweroff did not go through ACPI S5")


def boot_one(name, qemu, code, vars_src, firmware, accel):
    out = os.path.join(OUT, name)
    os.makedirs(out)
    disk = os.path.join(out, "disk.img")
    prepare_disk(disk)
    sockdir = tempfile.mkdtemp(prefix="suefi")
    qmp_path = os.path.join(sockdir, "qmp")
    cmd = [qemu, "-M", "pc", "-cdrom", ISO,
           "-drive", f"file={disk},format=raw,if=ide",
           "-accel", accel, "-vga", "std", *smokelib.QEMU_DISPLAY,
           # No -no-shutdown: the closing poweroff must make QEMU exit.
           "-serial", "stdio", "-m", "512M", "-no-reboot",
           "-qmp", f"unix:{qmp_path},server=on,wait=off"]
    if code:
        cmd += ["-drive", f"if=pflash,format=raw,readonly=on,file={code}"]
        if vars_src:
            vars_copy = os.path.join(out, "vars.fd")
            shutil.copyfile(vars_src, vars_copy)
            cmd += ["-drive", f"if=pflash,format=raw,file={vars_copy}"]
    with open(os.path.join(out, "qemu-cmdline.txt"), "w") as f:
        f.write(" ".join(cmd) + "\n")
    t0 = time.time()
    proc = subprocess.Popen(cmd, cwd=ROOT, stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            bufsize=0)
    con = Console(proc)
    qmp = None
    try:
        qmp = Qmp(qmp_path)
        m = con.wait_re(r'\[BOOT\] multiboot2: loader "(Limine[^"]*)" \((\w+)\), '
                        r'(\d+) mmap entries, (\d+) module\(s\), fb (\w+), '
                        r'ACPI RSDP (\w+)', timeout=180)
        loader, fw_seen, nmods, fb_tag, rsdp = (m.group(1), m.group(2),
                                                 int(m.group(4)), m.group(5),
                                                 m.group(6))
        if fw_seen != firmware:
            raise AssertionError(f"kernel saw {fw_seen} firmware, expected {firmware}")
        if nmods < 1 or fb_tag != "yes" or rsdp == "none":
            raise AssertionError(f"boot info incomplete: modules={nmods} "
                                 f"fb={fb_tag} rsdp={rsdp}")
        m = con.wait_re(r"\[FB\]\s+(\d+)x(\d+)@(\d+) pitch=\d+ phys=(0x[0-9a-f]+)",
                        timeout=60)
        fb = (int(m.group(1)), int(m.group(2)))
        fb_phys = m.group(4)
        smokelib.login(proc, con.sel, con.log, timeout=180, start=0)
        m = con.wait_re(r"\[desktop\] ready fb=(\d+)x(\d+)", timeout=120, start=0)
        desk = (int(m.group(1)), int(m.group(2)))
        if desk != fb:
            raise AssertionError(f"desktop runs at {desk}, framebuffer is {fb}")
        time.sleep(1.5)
        ppm = os.path.join(out, "screen.ppm")
        qmp.cmd("screendump", filename=ppm)
        img = Image.read_ppm(ppm)
        img.write_png(os.path.join(out, "desktop.png"))
        os.remove(ppm)
        if (img.w, img.h) != fb:
            raise AssertionError(f"screendump is {img.w}x{img.h}, framebuffer {fb}")
        colors = distinct_colors(img, (0, 0, img.w, img.h))
        if colors < 50:
            raise AssertionError(f"desktop screen has only {colors} colours")
        acpi_poweroff(con)
        return (f"{loader}, firmware={fw_seen}, fb={fb[0]}x{fb[1]} at {fb_phys}, "
                f"ACPI RSDP {rsdp}, desktop up, {colors} colours, "
                f"ACPI up and poweroff exits, {time.time() - t0:.1f}s")
    except Exception:
        print(f"\n[SMOKE-UEFI] {name}: last serial output:\n{con.text()[-3000:]}",
              file=sys.stderr)
        raise
    finally:
        with open(os.path.join(out, "serial.log"), "w") as f:
            f.write(con.text())
        if qmp is not None:
            qmp.close()
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
        shutil.rmtree(sockdir, ignore_errors=True)


def main():
    shutil.rmtree(OUT, ignore_errors=True)
    os.makedirs(OUT)
    for f in (ISO, os.path.join(ROOT, "disk.img")):
        if not os.path.exists(f):
            raise RuntimeError(f"{os.path.basename(f)} is missing (make limine-iso disk)")
    only = os.environ.get("SMOKE_UEFI_ONLY", "").split()
    require = os.environ.get("SMOKE_UEFI_REQUIRE") == "1"
    accel = pick_accel()
    results, failed = [], []
    for name, qemu, code, vars_src, firmware in configs():
        if only and name not in only:
            continue
        if firmware == "UEFI" and not code:
            msg = "SKIP (no OVMF firmware found; tools/setup-linux.sh installs it)"
            results.append((name, msg))
            if require:
                failed.append(name)
            continue
        if not shutil.which(qemu):
            results.append((name, f"SKIP ({qemu} not found)"))
            if require:
                failed.append(name)
            continue
        acc = accel if firmware == "BIOS" else os.environ.get("SMOKE_UEFI_ACCEL", "tcg")
        print(f"\n[SMOKE-UEFI] {name}: {qemu} firmware={code or 'SeaBIOS'} accel={acc}")
        try:
            results.append((name, f"PASS ({acc}): " + boot_one(name, qemu, code, vars_src,
                                                               firmware, acc)))
        except Exception as exc:
            results.append((name, f"FAIL: {exc}"))
            failed.append(name)
    print()
    for name, msg in results:
        print(f"[SMOKE-UEFI] {name}: {msg}")
    if failed:
        print(f"[SMOKE-UEFI] failed: {' '.join(failed)}; see "
              f"{os.path.relpath(OUT, ROOT)}/<name>/serial.log")
        return 1
    print("[SMOKE-UEFI] passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"\n[SMOKE-UEFI] failed: {exc}", file=sys.stderr)
        raise SystemExit(1)
