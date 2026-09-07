#!/usr/bin/env python3
"""Catch the instruction that writes 0x80000000 into gtkprobe's heap (0x40073000)
via QEMU's software gdbstub watchpoint (works under TCG, unlike guest DR regs).

Boots QEMU with -s (gdbstub :1234), drives the shell to run gtkprobe, waits for
GTK_OK init, then attaches gdb with a CONDITIONAL write-watchpoint that only
breaks when the slot becomes 0x80000000 (so it naturally fires in gtkprobe's
context, ignoring other processes' identical virtual address).
"""
import os, sys, time, subprocess, selectors, shutil

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
PROMPT = "MaeroOS$ "
A = 0x40073000
# First cross gdb on PATH (Linux: gdb-multiarch, macOS/Homebrew: i386-elf-gdb).
GDB = (shutil.which("i686-elf-gdb") or shutil.which("i386-elf-gdb")
       or shutil.which("gdb-multiarch") or "/opt/homebrew/bin/i386-elf-gdb")

def main():
    subprocess.run(["make", "initrd", "disk"], cwd=ROOT, check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    qemu = subprocess.Popen(
        ["qemu-system-i386", "-kernel", "kernel.elf", "-initrd", "initrd.tar",
         "-drive", "file=disk.img,format=raw,if=ide", "-serial", "stdio",
         "-m", "512M", "-no-reboot", "-no-shutdown", "-s"],
        cwd=ROOT, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, bufsize=0)
    sel = selectors.DefaultSelector()
    sel.register(qemu.stdout, selectors.EVENT_READ)
    log = []

    def wait(needle, timeout):
        dl = time.time() + timeout
        while time.time() < dl:
            for k, _ in sel.select(0.2):
                c = os.read(k.fd, 4096).decode("latin1", "replace")
                if not c: continue
                log.append(c); sys.stdout.write(c); sys.stdout.flush()
                if needle in "".join(log): return True
            if qemu.poll() is not None: return False
        return False

    try:
        if not wait(PROMPT, 60):
            print("\n[hunt] no prompt"); return
        qemu.stdin.write(b"/disk/gtkprobe\n"); qemu.stdin.flush()
        if not wait("GTK_OK init", 200):
            print("\n[hunt] no GTK_OK"); return
        print("\n[hunt] GTK_OK — attaching gdb with conditional watchpoint")
        cmds = [
            "set pagination off", "set confirm off",
            "target remote :1234",
            f"watch *(unsigned int*)0x{A:08x} if *(unsigned int*)0x{A:08x} == 0x80000000",
            "continue",                       # blocks until the corrupting write
            'printf "\\n[hunt] CULPRIT eip=0x%08x\\n", $pc',
            "info registers eip eax ebx ecx edx esi edi ebp esp",
            "x/6i $pc-12",
            "detach", "quit",
        ]
        argv = [GDB, "-q", "-nx", "kernel.elf"]
        for c in cmds: argv += ["-ex", c]
        gdb = subprocess.Popen(argv, cwd=ROOT, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT)
        dl = time.time() + 600
        while time.time() < dl and gdb.poll() is None:
            for k, _ in sel.select(0.1):
                c = os.read(k.fd, 4096).decode("latin1", "replace")
                if c: log.append(c); sys.stdout.write(c); sys.stdout.flush()
        try: out = gdb.stdout.read().decode("latin1", "replace")
        except Exception: out = ""
        if gdb.poll() is None: gdb.kill()
        print("\n===== GDB OUTPUT =====\n" + out[-4000:])
    finally:
        qemu.kill()

if __name__ == "__main__":
    main()
