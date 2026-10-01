#!/usr/bin/env python3
"""ACPI smoke test: power off and reboot through reboot(2) on both PC flavours.

For each machine type (QEMU's default i440FX/PIIX4 `pc` and the ICH9 `q35`):
  - boot, check the kernel found the tables and loaded the AML namespace
    ("[ACPI] ready"), log in as root and run `poweroff`: QEMU (started without
    -no-shutdown) must exit within a few seconds, through ACPI S5 — the
    fallback-port path logs "S5 failed" and fails the test;
  - boot again with -no-reboot, run `reboot`: QEMU must exit (a reset with
    -no-reboot ends QEMU) after the kernel's "[ACPI] restarting";
  - boot again and press the ACPI power button (monitor `system_powerdown`):
    the SCI's fixed event reaches init as SIGUSR2 and init powers off.
Machines can be narrowed with SMOKE_ACPI_MACHINES="pc" etc.; SMOKE_SMP=N adds
-smp N.
"""
import os
import selectors
import socket
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import smokelib  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
EXIT_TIMEOUT = 10.0


def boot(machine, extra):
    cmd = ["qemu-system-i386", *smokelib.QEMU_DISPLAY, "-M", machine,
           "-kernel", "kernel.elf", "-initrd", "initrd.tar",
           "-serial", "stdio", "-m", "512M", *extra]
    smp = os.environ.get("SMOKE_SMP")
    if smp:
        cmd += ["-smp", str(int(smp))]
    proc = subprocess.Popen(cmd, cwd=ROOT, stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            bufsize=0)
    sel = selectors.DefaultSelector()
    sel.register(proc.stdout, selectors.EVENT_READ)
    return proc, sel


def drain_until_exit(proc, sel, log, timeout):
    """Read console output until QEMU exits; True if it did within timeout."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        for key, _ in sel.select(0.2):
            chunk = os.read(key.fd, 4096).decode("latin1", "replace")
            if chunk:
                log.append(chunk)
                sys.stdout.write(chunk)
                sys.stdout.flush()
        if proc.poll() is not None:
            return True
    return False


def press_power_button(mon_path):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.connect(mon_path)
        s.sendall(b"system_powerdown\n")
        time.sleep(0.5)


def run_case(machine, command, extra, expect_msg):
    mon_path = None
    if command == "power-button":
        mon_path = os.path.join(ROOT, f".smoke-acpi-{os.getpid()}.mon")
        extra = extra + ["-monitor", f"unix:{mon_path},server=on,wait=off"]
    proc, sel = boot(machine, extra)
    log = []
    try:
        smokelib.login(proc, sel, log, timeout=90.0)
        text = "".join(log)
        if "[ACPI] ready" not in text:
            return f"{machine}: ACPI did not initialise"
        start = time.time()
        if mon_path:
            press_power_button(mon_path)
        else:
            smokelib.send(proc, command + "\n")
        if not drain_until_exit(proc, sel, log, EXIT_TIMEOUT):
            return f"{machine}: QEMU still running {EXIT_TIMEOUT:.0f}s after `{command}`"
        took = time.time() - start
        text = "".join(log)
        if expect_msg not in text:
            return f"{machine}: `{command}` exited QEMU without {expect_msg!r}"
        bads = ["S5 failed", "power off failed"]
        # i440FX's FADT is revision 1, without a reset register: falling back
        # to 0xCF9 there is the expected path.  Q35's FADT has one.
        if "reset reg supported" in text:
            bads.append("reset register:")
        for bad in bads:
            if bad in text:
                return f"{machine}: `{command}` used a fallback path ({bad!r})"
        print(f"\n[smoke-acpi] {machine}: `{command}` -> QEMU exited in {took:.1f}s")
        return None
    except (TimeoutError, RuntimeError) as e:
        return f"{machine}: {command}: {e}"
    finally:
        if proc.poll() is None:
            proc.kill()
        proc.wait()
        if mon_path and os.path.exists(mon_path):
            os.unlink(mon_path)


def main():
    machines = os.environ.get("SMOKE_ACPI_MACHINES", "pc q35").split()
    failures = []
    for m in machines:
        for command, extra, msg in (
                ("poweroff", [], "[ACPI] powering off"),
                ("reboot", ["-no-reboot"], "[ACPI] restarting"),
                ("power-button", [], "[init] Power button: shutting down")):
            err = run_case(m, command, extra, msg)
            if err:
                failures.append(err)
    print()
    if failures:
        for f in failures:
            print("[smoke-acpi] FAIL:", f)
        return 1
    print(f"[smoke-acpi] PASS: poweroff, reboot and power button on {', '.join(machines)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
