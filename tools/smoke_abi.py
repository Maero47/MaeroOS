#!/usr/bin/env python3
"""smoke-abi -- run the Linux-ABI probes (docs/audit/firefox-first-paint.md,
section 8) on MaeroOS under QEMU and compare with the Linux reference.

Every probe in testfiles/abiprobes/ (built by ports/abiprobes/Makefile) prints
one final line: PASS <name>, FAIL <name>: <detail> or SKIP <name>: <why>.  The
same binaries print PASS on a Linux host; that is the behaviour asserted here.

Probes the audit expects to FAIL on the current kernel are listed in XFAIL
with the finding ids they cover.  A kernel fix that makes one of them pass
shows up as XPASS: remove the entry so the probe becomes required.  The run
is green when there is no unexpected FAIL (and, with --strict, no XPASS).

Usage: tools/smoke_abi.py [--mem 1024M] [--only p05,p13] [--strict]
                          [--timeout SECS] [--qemu qemu-system-i386]
"""
import argparse
import os
import selectors
import subprocess
import sys
import time


ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
PROBE_DIR = os.path.join(ROOT, "testfiles", "abiprobes")
GUEST_DIR = "/abiprobes"
PROMPT = "MaeroOS$ "

# name -> (per-probe timeout in seconds, extra arguments)
PROBES = {
    "p01_fatal_signal_scope":  (90, ""),
    "p02_spawn_exit_group":    (90, ""),
    "p03_sigchld_thread":      (90, ""),
    "p04_spurious_futex":      (90, ""),
    "p05_stale_wake_tick":     (90, ""),
    "p06_mmap_prot_madvise":   (90, ""),
    "p07_ftruncate64":         (90, ""),
    "p08_select_timeout":      (90, ""),
    "p09_poll_eintr_restart":  (90, ""),
    "p10_unix_socket":         (90, ""),
    "p11_addr_space_reuse":    (150, ""),
    "p12_clocks":              (90, ""),
    "p13_futex_timeout":       (90, ""),
    "p14_exec_arg_size":       (90, ""),
    "p15_socket_cloexec":      (90, ""),
    "p16_memfd_cloexec_size":  (150, ""),
    "p17_signal_busy_thread":  (90, ""),
    "p18_high_memory":         (300, "{p18_mib}"),
    "p19_siginfo":             (90, ""),
    "p20_shared_futex":        (90, ""),
    "p21_sigsuspend":          (90, ""),
    "p22_mprotect_cow":        (90, ""),
}

# Expected to FAIL today, with the audit findings that the fix must address.
# Delete an entry when the corresponding kernel change lands (the run then
# reports XPASS until you do).
XFAIL = {
    "p07_ftruncate64":         "syscalls 194/193/297/40 missing",
    "p10_unix_socket":         "U2-U5: SCM_RIGHTS position, recvmsg 0, flags, POLLHUP",
    "p14_exec_arg_size":       "C4: 4 KiB arg page, EXEC_MAXARGS 64",
    "p15_socket_cloexec":      "C5: SOCK_CLOEXEC ignored",
    "p16_memfd_cloexec_size":  "C5, M5: MFD_CLOEXEC ignored, write not reflected",
    "p19_siginfo":             "S6: si_addr/si_code, sa_mask, sigaltstack",
}


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
    ap.add_argument("--mem", default="1024M", help="QEMU memory (default 1024M; audit P18 also asks for 2048M)")
    ap.add_argument("--only", default="", help="comma-separated probe prefixes, e.g. p05,p13")
    ap.add_argument("--strict", action="store_true", help="treat XPASS as a failure")
    ap.add_argument("--timeout", type=int, default=0, help="override every per-probe timeout")
    ap.add_argument("--qemu", default="qemu-system-i386")
    args = ap.parse_args()

    mem_mib = parse_mem_mib(args.mem)
    # P18 touches this much in one calloc; leave room for kernel + initrd.
    p18_mib = max(64, min(700, mem_mib - 324))

    selected = list(PROBES)
    if args.only:
        prefixes = [p.strip() for p in args.only.split(",") if p.strip()]
        selected = [n for n in selected if any(n.startswith(p) for p in prefixes)]
        if not selected:
            raise SystemExit(f"--only {args.only!r} matched no probe")

    missing = [n for n in selected if not os.path.exists(os.path.join(PROBE_DIR, n))]
    if missing:
        raise SystemExit("probe binaries missing (run `make abiprobes`): " + ", ".join(missing))

    proc = subprocess.Popen(
        [args.qemu, "-kernel", "kernel.elf", "-initrd", "initrd.tar",
         "-serial", "stdio", "-display", "none", "-m", args.mem,
         "-no-reboot", "-no-shutdown"],
        cwd=ROOT, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, bufsize=0,
    )
    sel = selectors.DefaultSelector()
    sel.register(proc.stdout, selectors.EVENT_READ)
    log = []
    results = {}   # name -> (tag, line)
    try:
        if not wait_for(proc, sel, PROMPT, log, timeout=75):
            raise TimeoutError("no shell prompt after boot")

        for name in selected:
            tmo, extra = PROBES[name]
            if args.timeout:
                tmo = args.timeout
            cmd = f"{GUEST_DIR}/{name} {extra.format(p18_mib=p18_mib)}".rstrip()
            before = len("".join(log))
            print(f"\n[SMOKE-ABI] running {cmd}")
            send(proc, cmd + "\n")
            got_prompt = wait_for(proc, sel, PROMPT, log, timeout=tmo, start=before)
            body = "".join(log)[before:]
            v = verdict(body, name)
            if not got_prompt:
                results[name] = ("HANG", f"HANG {name}: no prompt after {tmo} s")
                print(f"\n[SMOKE-ABI] {name} hung the shell; stopping the run")
                for rest in selected[selected.index(name) + 1:]:
                    results[rest] = ("NOTRUN", f"NOTRUN {rest}: shell hung earlier")
                break
            if v is None:
                v = ("FAIL", f"FAIL {name}: no verdict line (crashed?)")
            results[name] = v
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                proc.kill()

    npass = nxfail = nxpass = nskip = nfail = 0
    print("\n[SMOKE-ABI] results:")
    for name in selected:
        tag, line = results.get(name, ("NOTRUN", f"NOTRUN {name}"))
        expected_fail = name in XFAIL
        if tag == "PASS" and not expected_fail:
            status = "pass"
            npass += 1
        elif tag == "PASS" and expected_fail:
            status = "XPASS (remove from XFAIL: " + XFAIL[name] + ")"
            nxpass += 1
        elif tag == "SKIP":
            status = "skip"
            nskip += 1
        elif tag in ("FAIL", "HANG", "NOTRUN") and expected_fail:
            status = "xfail (" + XFAIL[name] + ")"
            nxfail += 1
        else:
            status = "UNEXPECTED " + tag
            nfail += 1
        print(f"  {status:<12} {line}")

    print(f"\n[SMOKE-ABI] {npass} pass, {nxfail} xfail, {nfail} unexpected, "
          f"{nxpass} xpass, {nskip} skip (mem {args.mem}, p18 {p18_mib} MiB)")
    if nfail or (args.strict and nxpass):
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
