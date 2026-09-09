#!/usr/bin/env python3
"""smoke-abi -- run the Linux-ABI probes (docs/audit/firefox-first-paint.md,
section 8) on MaeroOS under QEMU and compare with the Linux reference.

Every probe in testfiles/abiprobes/ (built by ports/abiprobes/Makefile) prints
one final line: PASS <name>, FAIL <name>: <detail> or SKIP <name>: <why>.  The
same binaries print PASS on a Linux host; that is the behaviour asserted here.

Probes the audit expects to FAIL on the current kernel are listed in XFAIL
with the finding ids they cover.  A kernel fix that makes one of them pass
shows up as XPASS: remove the entry so the probe becomes required.

Only a probe that actually ran and printed `FAIL <name>: ...` can satisfy an
XFAIL entry.  A probe that produced no verdict line, that wedged the guest
shell, or that never ran is a hard failure whether or not it is in XFAIL:
those are harness faults (missing binaries, exec failure, a crash before any
output, a kernel wedge), and swallowing them would hide a run in which
nothing was actually tested.  Each is counted separately in the summary.

When a probe wedges the shell the driver kills QEMU, reboots and continues
with the remaining probes, so one wedge does not turn the rest of the run
into NOTRUN.

Usage: tools/smoke_abi.py [--mem 1024M] [--only p05,p13] [--strict]
                          [--timeout SECS] [--qemu qemu-system-i386]
"""
import argparse
import os
import re
import selectors
import subprocess
import sys
import time


ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
PROBE_DIR = os.path.join(ROOT, "testfiles", "abiprobes")
SRC_DIR = os.path.join(ROOT, "ports", "abiprobes")
GUEST_DIR = "/abiprobes"
PROMPT = "MaeroOS$ "
BOOT_TIMEOUT = 75

# Seconds allowed on top of a probe's own watchdog before the driver gives up
# on the shell prompt.  The probe's watchdog always fires first and prints a
# FAIL line, so a stuck syscall is reported as a verdict rather than a wedge.
GRACE = 30

# name -> (watchdog seconds, extra guest arguments).  "watchdog" must equal the
# probe_watchdog(...) argument in ports/abiprobes/<name>.c; check_watchdogs()
# enforces that before the run.  None means the probe computes its watchdog
# from its arguments (p18); see p18_watchdog() below.
PROBES = {
    "p01_fatal_signal_scope":  (60, ""),
    "p02_spawn_exit_group":    (60, ""),
    "p03_sigchld_thread":      (60, ""),
    "p04_spurious_futex":      (60, ""),
    "p05_stale_wake_tick":     (60, ""),
    "p06_mmap_prot_madvise":   (60, ""),
    "p07_ftruncate64":         (60, ""),
    "p08_select_timeout":      (60, ""),
    "p09_poll_eintr_restart":  (60, ""),
    "p10_unix_socket":         (60, ""),
    "p11_addr_space_reuse":    (120, ""),
    "p12_clocks":              (60, ""),
    "p13_futex_timeout":       (60, ""),
    "p14_exec_arg_size":       (60, ""),
    "p15_socket_cloexec":      (60, ""),
    "p16_memfd_cloexec_size":  (120, ""),
    "p17_signal_busy_thread":  (60, ""),
    "p18_high_memory":         (None, "{p18_mib}"),
    "p19_siginfo":             (60, ""),
    "p20_shared_futex":        (60, ""),
    "p21_sigsuspend":          (60, ""),
    "p22_mprotect_cow":        (60, ""),
    "p23_exec_dethread":       (90, ""),
    "p24_unlink_open":         (60, ""),
    "p25_ptmx_lookup":         (60, ""),
    # Writes and deletes 2 MiB twelve times over the ATA PIO disk, so it needs
    # far longer than a probe that only exercises the ABI in memory.
    "p26_unlink_frees_space":  (240, ""),
}

# Expected to FAIL today, with the audit findings that the fix must address.
# Delete an entry when the corresponding kernel change lands (the run then
# reports XPASS until you do).
XFAIL = {
    # p07_ftruncate64 was here ("syscalls 194/193/297/40 missing") and now
    # passes: truncate64/ftruncate64/rmdir/mknodat are implemented.
    "p10_unix_socket":         "U2-U5: SCM_RIGHTS position, recvmsg 0, flags, POLLHUP",
    "p19_siginfo":             "S6: si_addr/si_code, sa_mask, sigaltstack",
    # p01-p06, p08, p09, p11-p13, p17, p18, p20 were listed here before the
    # process-model and memory/clock work landed on this branch; they are
    # required to pass now.  p20_shared_futex was never listed (F5/F6 are
    # conditional in the audit and the shared-futex cases already worked).
    # p21_sigsuspend and p22_mprotect_cow cover the review findings fixed on
    # this branch and are required from the start.
}


def p18_watchdog(mib):
    """Mirror of the probe_watchdog() expression in p18_high_memory.c."""
    return 120 + mib // 2


def watchdog_secs(name, p18_mib):
    wd = PROBES[name][0]
    return p18_watchdog(p18_mib) if wd is None else wd


def check_watchdogs(selected, p18_mib):
    """Every probe must fire its own watchdog before the driver's timeout.

    The driver's timeout is watchdog + GRACE by construction, so all that is
    left to verify is that the table matches the compiled-in value.  Reading
    the sources keeps the two from drifting apart silently.
    """
    problems = []
    for name in selected:
        src = os.path.join(SRC_DIR, name + ".c")
        if not os.path.exists(src):
            continue
        with open(src) as fh:
            m = re.search(r"probe_watchdog\(([^;]*)\)\s*;", fh.read())
        if not m:
            problems.append(f"{name}: no probe_watchdog() call in {name}.c")
            continue
        expr = m.group(1).strip()
        table = PROBES[name][0]
        if expr.isdigit():
            if table is None:
                problems.append(f"{name}: source has a constant watchdog {expr}, "
                                f"table says dynamic")
            elif int(expr) != table:
                problems.append(f"{name}: source watchdog {expr} s, table {table} s")
        else:
            if table is not None:
                problems.append(f"{name}: source watchdog is '{expr}' (dynamic), "
                                f"table says {table} s")
            elif "120 + (long)mib / 2" not in expr:
                problems.append(f"{name}: dynamic watchdog '{expr}' does not match "
                                f"p18_watchdog() in this driver")
    if problems:
        raise SystemExit("watchdog/timeout mismatch:\n  " + "\n  ".join(problems))


def wait_for(proc, sel, needle, log, timeout, start=0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        for key, _ in sel.select(0.2):
            chunk = os.read(key.fd, 4096).decode("latin1", "replace")
            if not chunk:
                continue
            log.append(chunk)
            sys.stdout.write(chunk)
            sys.stdout.flush()
            if needle in "".join(log)[start:]:
                return True
        if proc.poll() is not None:
            raise RuntimeError(f"QEMU exited with status {proc.returncode}")
    return False


def send(proc, text):
    proc.stdin.write(text.encode("latin1"))
    proc.stdin.flush()


def kill_qemu(proc):
    if proc is None or proc.poll() is not None:
        return
    proc.terminate()
    try:
        proc.wait(timeout=2)
    except subprocess.TimeoutExpired:
        proc.kill()


def boot(args):
    """Start QEMU and wait for the shell prompt.  Returns (proc, sel, log)."""
    proc = subprocess.Popen(
        # The ext2 volume is attached because p26 measures real filesystem
        # free space; the probes that predate it ignore it.
        [args.qemu, "-kernel", "kernel.elf", "-initrd", "initrd.tar",
         "-drive", "file=disk.img,format=raw,index=0,media=disk",
         "-serial", "stdio", "-display", "none", "-m", args.mem,
         "-no-reboot", "-no-shutdown"],
        cwd=ROOT, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, bufsize=0,
    )
    sel = selectors.DefaultSelector()
    sel.register(proc.stdout, selectors.EVENT_READ)
    log = []
    try:
        if not wait_for(proc, sel, PROMPT, log, timeout=BOOT_TIMEOUT):
            raise TimeoutError(f"no shell prompt within {BOOT_TIMEOUT} s")
    except Exception:
        kill_qemu(proc)
        raise
    return proc, sel, log


def parse_mem_mib(mem):
    mem = mem.strip().upper()
    if mem.endswith("G"):
        return int(mem[:-1]) * 1024
    if mem.endswith("M"):
        return int(mem[:-1])
    return int(mem)


def verdict(body, name):
    """Last PASS/FAIL/SKIP line for this probe in the captured output."""
    result = None
    for line in body.splitlines():
        line = line.strip()
        for tag in ("PASS", "FAIL", "SKIP"):
            if line.startswith(f"{tag} {name}"):
                result = (tag, line)
    return result


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mem", default="1024M",
                    help="QEMU memory (default 1024M; the audit also asks for 2048M, "
                         "which scales P18's allocation up with it)")
    ap.add_argument("--only", default="", help="comma-separated probe prefixes, e.g. p05,p13")
    ap.add_argument("--strict", action="store_true", help="treat XPASS as a failure")
    ap.add_argument("--timeout", type=int, default=0, help="override every per-probe timeout")
    ap.add_argument("--qemu", default="qemu-system-i386")
    args = ap.parse_args()

    mem_mib = parse_mem_mib(args.mem)
    # P18 calloc()s and touches this much in one go.  Leave the kernel, the
    # initrd and the guest page tables room, and stay inside what a 32-bit
    # user address space can hold: -m 1024M gives the audit's 700 MiB and
    # -m 2048M a genuinely larger 1400 MiB run.
    p18_mib = max(64, min(1400, mem_mib - 324))
    args.p18_mib = p18_mib

    selected = list(PROBES)
    if args.only:
        prefixes = [p.strip() for p in args.only.split(",") if p.strip()]
        selected = [n for n in selected if any(n.startswith(p) for p in prefixes)]
        if not selected:
            raise SystemExit(f"--only {args.only!r} matched no probe")

    missing = [n for n in selected if not os.path.exists(os.path.join(PROBE_DIR, n))]
    if missing:
        raise SystemExit("probe binaries missing (run `make abiprobes`): " + ", ".join(missing))

    check_watchdogs(selected, p18_mib)

    results = {}   # name -> (tag, line, wedged)
    proc = sel = log = None
    try:
        proc, sel, log = boot(args)
        for i, name in enumerate(selected):
            if proc is None:                      # previous probe wedged the guest
                print("\n[SMOKE-ABI] rebooting after the wedge")
                try:
                    proc, sel, log = boot(args)
                except Exception as exc:
                    for rest in selected[i:]:
                        results[rest] = ("NOTRUN", f"NOTRUN {rest}: reboot failed ({exc})", False)
                    break
            wd = watchdog_secs(name, p18_mib)
            tmo = args.timeout or wd + GRACE
            extra = PROBES[name][1].format(p18_mib=p18_mib)
            cmd = f"{GUEST_DIR}/{name} {extra}".rstrip()
            before = len("".join(log))
            print(f"\n[SMOKE-ABI] running {cmd} (watchdog {wd} s, timeout {tmo} s)")
            send(proc, cmd + "\n")
            got_prompt = wait_for(proc, sel, PROMPT, log, timeout=tmo, start=before)
            body = "".join(log)[before:]
            v = verdict(body, name)
            if got_prompt:
                if v is None:
                    results[name] = ("NOVERDICT", f"NOVERDICT {name}: ran but printed no "
                                                  f"PASS/FAIL/SKIP line", False)
                else:
                    results[name] = (v[0], v[1], False)
            else:
                detail = f" (last line: {v[1]})" if v else " and printed no verdict line"
                results[name] = ("HANG", f"HANG {name}: no shell prompt after {tmo} s"
                                         f"{detail}", True)
                print(f"\n[SMOKE-ABI] {name} wedged the shell; killing QEMU")
                kill_qemu(proc)
                proc = None
    finally:
        kill_qemu(proc)

    return summarise(selected, results, args)


def summarise(selected, results, args):
    """Print the per-probe verdicts and the summary; return the exit code.

    Only a FAIL from a probe that ran satisfies an XFAIL entry.  HANG, NOTRUN
    and NOVERDICT are harness faults and always fail the run.
    """
    counts = dict(npass=0, nxfail=0, nxpass=0, nskip=0, nfail=0,
                  nhang=0, nnotrun=0, nnoverdict=0)
    print("\n[SMOKE-ABI] results:")
    for name in selected:
        tag, line, _ = results.get(name, ("NOTRUN", f"NOTRUN {name}: never started", False))
        expected_fail = name in XFAIL
        if tag == "HANG":
            # A wedge is a harness fault even for an expected failure: the guest
            # had to be rebooted and the probe never returned a clean verdict.
            status, key = "HANG", "nhang"
        elif tag == "NOTRUN":
            status, key = "NOTRUN", "nnotrun"
        elif tag == "NOVERDICT":
            status, key = "NO VERDICT", "nnoverdict"
        elif tag == "SKIP":
            status, key = "skip", "nskip"
        elif tag == "PASS":
            if expected_fail:
                status, key = "XPASS (remove from XFAIL: " + XFAIL[name] + ")", "nxpass"
            else:
                status, key = "pass", "npass"
        elif expected_fail:
            status, key = "xfail (" + XFAIL[name] + ")", "nxfail"
        else:
            status, key = "UNEXPECTED FAIL", "nfail"
        counts[key] += 1
        print(f"  {status:<12} {line}")

    print(f"\n[SMOKE-ABI] {counts['npass']} pass, {counts['nxfail']} xfail, "
          f"{counts['nfail']} unexpected fail, {counts['nxpass']} xpass, "
          f"{counts['nskip']} skip, {counts['nhang']} hang, "
          f"{counts['nnotrun']} not run, {counts['nnoverdict']} no verdict "
          f"(mem {args.mem}, p18 {args.p18_mib} MiB)")
    hard = counts["nfail"] + counts["nhang"] + counts["nnotrun"] + counts["nnoverdict"]
    if hard or (args.strict and counts["nxpass"]):
        print("[SMOKE-ABI] failed")
        return 1
    print("[SMOKE-ABI] passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"\n[SMOKE-ABI] failed: {exc}", file=sys.stderr)
        raise SystemExit(1)
