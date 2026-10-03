#!/usr/bin/env python3
"""Big Kernel Lock contention: where the lock is held and who waits for it.

Boots a `make BKLSTAT=1` kernel.elf + initrd.tar (KVM when /dev/kvm is
usable, else TCG) with -smp N, logs in on the serial console and runs each
workload between `schedlat bkl reset` and `schedlat bkl dump`.  The kernel
prints its counters ([bklstat] lines, arch/i686/cpu/bkl.c):

  busy        % of wall time some CPU held the lock
  spin        per CPU: % of wall time spent spinning for it, interrupts off
  hold        top lock holders by time (syscall number or interrupt vector)
  blocked_by  what the holder was doing while other CPUs spun
  waiter      what the spinning CPUs wanted the lock for

Workloads: schedlat's busybox scripts (forks, forks4, targz, targz4, par4)
and `schedlat scale KIND P 1000` (getpid, pipe, stat, mmap loops in P
processes at once), whose ops/s against P is the scaling the lock allows.
Results go to build/bench-bkl/results.txt (the summary) and
build/bench-bkl/serial-<tag>-smp<N>.log (every line).  See docs/smp-plan.md.

  python3 tools/bench_bkl.py [--smp N] [--tag NAME] [--tcg] [--quick]
"""
import argparse
import os
import re
import selectors
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(__file__))
import smokelib  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
PROMPT = smokelib.PROMPT
OUT = os.path.join(ROOT, "build", "bench-bkl")

# i386 syscall numbers that show up in the tables, for readability.
SYSNAMES = {
    1: "exit", 2: "fork", 3: "read", 4: "write", 5: "open", 6: "close", 7: "waitpid",
    10: "unlink", 11: "execve", 12: "chdir", 20: "getpid", 33: "access", 37: "kill",
    42: "pipe", 45: "brk", 54: "ioctl", 57: "setpgid", 63: "dup2", 64: "getppid",
    65: "getpgrp", 85: "readlink", 90: "mmap", 91: "munmap", 106: "stat", 114: "wait4",
    119: "sigreturn", 120: "clone", 122: "uname", 125: "mprotect", 140: "_llseek",
    145: "readv", 146: "writev", 158: "sched_yield", 162: "nanosleep", 168: "poll",
    173: "rt_sigreturn", 174: "rt_sigaction", 175: "rt_sigprocmask", 183: "getcwd",
    190: "vfork", 192: "mmap2", 195: "stat64", 196: "lstat64", 197: "fstat64",
    199: "getuid32", 220: "getdents64", 221: "fcntl64", 224: "gettid", 240: "futex",
    243: "set_thread_area", 252: "exit_group", 258: "set_tid_address", 265: "clock_gettime",
    295: "openat", 300: "fstatat64", 301: "unlinkat", 308: "pselect6", 309: "ppoll",
    331: "pipe2", 383: "statx", 403: "clock_gettime64", 507: "bklstat",
}
VECNAMES = {7: "#NM", 13: "#GP", 14: "#PF", 32: "PIT", 33: "kbd", 36: "com1",
            240: "lapic-timer", 252: "resched-ipi"}


def pretty(name):
    kind, _, num = name.partition(":")
    if not num:
        return name
    n = int(num)
    if kind == "sys":
        return "%s(%d)" % (SYSNAMES.get(n, "sys"), n)
    return "%s(vec %d)" % (VECNAMES.get(n, "irq" if 32 <= n < 48 else "vec"), n)


def workloads(quick):
    w = [("forks", "/schedlat time forks"), ("forks4", "/schedlat time forks4"),
         ("targz", "/schedlat time targz"), ("targz4", "/schedlat time targz4"),
         ("par4", "/schedlat time par4")]
    for kind in ("getpid", "pipe", "stat", "mmap"):
        for p in ((1, 4) if quick else (1, 2, 4)):
            w.append(("scale-%s-%d" % (kind, p), "/schedlat scale %s %d 1000" % (kind, p)))
    return w


def summarize(name, out):
    """The interesting lines of one [bklstat] dump, as text."""
    lines = [l.split("[bklstat] ", 1)[1].strip() for l in out.splitlines() if "[bklstat] " in l]
    res = [l.strip() for l in out.splitlines() if l.startswith("schedlat ") and "bkl" not in l]
    s = ["-- %s" % name] + ["   " + r for r in res]
    tops = {"hold": [], "blocked_by": [], "waiter": []}
    for l in lines:
        if l.startswith(("total", "begin")) or l.startswith("cpu"):
            s.append("   " + l)
        m = re.match(r"(hold|blocked_by|waiter) (\S+) us=(\d+) pm=([\d.]+) n=(\d+)", l)
        if m and len(tops[m.group(1)]) < 6:
            tops[m.group(1)].append("%s %s%%/%sus" % (pretty(m.group(2)),
                                                      m.group(4),
                                                      m.group(3)))
    for k, v in tops.items():
        if v:
            s.append("   %-10s %s" % (k, ", ".join(v)))
    return "\n".join(s)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--smp", type=int, default=4)
    ap.add_argument("--tag", default=None)
    ap.add_argument("--tcg", action="store_true")
    ap.add_argument("--quick", action="store_true", help="scale runs with 1 and 4 processes only")
    ap.add_argument("--kernel", default="kernel.elf")
    a = ap.parse_args()

    kvm = not a.tcg and os.access("/dev/kvm", os.R_OK | os.W_OK)
    tag = a.tag or subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=ROOT,
                                  capture_output=True, text=True).stdout.strip()
    os.makedirs(OUT, exist_ok=True)
    cmd = ["qemu-system-i386", *smokelib.QEMU_DISPLAY, "-kernel", a.kernel,
           "-initrd", "initrd.tar", "-serial", "stdio", "-m", "512M",
           "-no-reboot", "-no-shutdown", "-smp", str(a.smp)]
    if kvm:
        cmd += ["-accel", "kvm", "-cpu", "host"]
    proc = subprocess.Popen(cmd, cwd=ROOT, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, bufsize=0)
    sel = selectors.DefaultSelector()
    sel.register(proc.stdout, selectors.EVENT_READ)
    log = []
    summary = []
    try:
        smokelib.login(proc, sel, log, timeout=120 if kvm else 300)
        at = smokelib.mark(log)
        smokelib.send(proc, "/schedlat bkl dump\n")
        smokelib.wait_for(proc, sel, PROMPT, log, timeout=60, start=at)
        if "[bklstat] end" not in "".join(log)[at:]:
            raise SystemExit("bench-bkl: no [bklstat] dump - is kernel.elf built with BKLSTAT=1?")
        for name, c in workloads(a.quick):
            at = smokelib.mark(log)
            smokelib.send(proc, "/schedlat bkl reset; %s; /schedlat bkl dump\n" % c)
            smokelib.wait_for(proc, sel, "[bklstat] end", log, timeout=900, start=at)
            smokelib.wait_for(proc, sel, PROMPT, log, timeout=60, start=at)
            summary.append(summarize(name, "".join(log)[at:]))
            print(summary[-1], flush=True)
    finally:
        proc.kill()
        proc.wait()
        with open(os.path.join(OUT, "serial-%s-smp%d.log" % (tag, a.smp)), "w") as f:
            f.write("".join(log))
    head = "== %s smp=%d accel=%s %s" % (tag, a.smp, "kvm" if kvm else "tcg",
                                         time.strftime("%Y-%m-%d %H:%M"))
    with open(os.path.join(OUT, "results.txt"), "a") as f:
        f.write(head + "\n" + "\n".join(summary) + "\n")
    print("\n" + head)


if __name__ == "__main__":
    main()
