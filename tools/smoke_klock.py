#!/usr/bin/env python3
"""Lock primitives torture: boots a `make KLOCK_TEST=1` kernel with -smp 4.

kernel/klock.c starts four kernel threads at boot.  They step out from under
the Big Kernel Lock and hammer a kspinlock (with a nested second lock) and a
kmutex from several CPUs at once, checking that no update is lost and no two
CPUs are ever inside together; the lock-order checker must stay silent for
the consistent order and catch one deliberate inversion.  Stage 2 adds: the
same threads allocate heap blocks and frames outside the BKL and send TLB
shootdowns at once (every test page must read at least its published
generation afterwards), and two more threads ping-pong a token 20000 times
through sleep_locked with a deadline that catches a lost wakeup.  The log must show
"[KLOCK-TEST] PASS" with the threads seen on at least two CPUs at the same
time, no "[lockdep]" line other than the deliberate one, and a working shell
afterwards.  KVM when /dev/kvm is usable (real parallel CPUs), else TCG.

  python3 tools/smoke_klock.py [--smp N] [--kernel ELF]
"""
import argparse
import os
import re
import selectors
import subprocess
import sys

sys.path.insert(0, os.path.dirname(__file__))
import smokelib  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--smp", type=int, default=4)
    ap.add_argument("--kernel", default="kernel.elf")
    a = ap.parse_args()
    kvm = os.access("/dev/kvm", os.R_OK | os.W_OK)
    cmd = ["qemu-system-i386", *smokelib.QEMU_DISPLAY, "-kernel", a.kernel,
           "-initrd", "initrd.tar", "-serial", "stdio", "-m", "512M",
           "-no-reboot", "-no-shutdown", "-smp", str(a.smp)]
    if kvm:
        cmd += ["-accel", "kvm"]
    proc = subprocess.Popen(cmd, cwd=ROOT, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, bufsize=0)
    sel = selectors.DefaultSelector()
    sel.register(proc.stdout, selectors.EVENT_READ)
    log = []
    try:
        at = smokelib.wait_for(proc, sel, "[KLOCK-TEST] threads=", log,
                               timeout=180 if kvm else 600)
        smokelib.wait_for(proc, sel, "\n", log, timeout=10, start=at)
        text = "".join(log)
        m = re.search(r"\[KLOCK-TEST\] threads=(\d+) cpus=(\d+) max_parallel=(\d+) "
                      r"spin_iters=(\d+) contended=(\d+) mutex_iters=(\d+) "
                      r"lockdep_inversion_caught=(\d+)", text)
        if not m:
            raise SystemExit("smoke-klock: malformed [KLOCK-TEST] summary line")
        threads, cpus, par, spins, cont, mutexes, caught = map(int, m.groups())
        # Stage 2 (docs/smp-plan.md): sleep_locked ping-pong, concurrent
        # shootdowns checked against stale translations, heap/frame traffic
        # from every CPU outside the BKL.
        m2 = re.search(r"\[KLOCK-TEST\] stage2 pingpong=(\d+)\+(\d+) sleeps=(\d+) "
                       r"lost_wakeups=(\d+) shootdowns=(\d+) tlb_checks=(\d+) "
                       r"mem_rounds=(\d+) kstacks=(\d+)", text)
        if not m2:
            raise SystemExit("smoke-klock: no [KLOCK-TEST] stage2 summary line")
        pp0, pp1, sleeps, lost, sds, checks, mems, kstacks = map(int, m2.groups())
        if lost or not sds or not checks or not mems or not sleeps or not kstacks:
            raise SystemExit("smoke-klock: stage2 counters wrong: " + m2.group(0))
        if "[KLOCK-TEST] FAIL" in text:
            raise SystemExit("smoke-klock: " + re.search(r"\[KLOCK-TEST\] FAIL[^\n]*", text).group(0))
        try:
            smokelib.wait_for(proc, sel, "[KLOCK-TEST] PASS", log, timeout=10)
        except TimeoutError:
            # printk takes no lock, so a FAIL line printed while other CPUs
            # print can come out interleaved with theirs: no PASS is a FAIL.
            raise SystemExit("smoke-klock: no [KLOCK-TEST] PASS (spin_iters=%d mutex_iters=%d); "
                             "see build/smoke-klock.log" % (spins, mutexes))
        text = "".join(log)
        reports = re.findall(r"\[lockdep\][^\n]*", text)
        unexpected = [r for r in reports if "klocktest.c" not in r and "klocktest.d" not in r]
        if unexpected:
            raise SystemExit("smoke-klock: unexpected lockdep report: " + unexpected[0])
        if a.smp > 1 and kvm and (cpus < 2 or par < 2):
            raise SystemExit("smoke-klock: the threads never ran on two CPUs at once "
                             "(cpus=%d max_parallel=%d)" % (cpus, par))
        if caught != 1:
            raise SystemExit("smoke-klock: lockdep caught %d inversions, expected 1" % caught)
        at = smokelib.login(proc, sel, log, timeout=120 if kvm else 300)
        smokelib.send(proc, "echo klock-shell-ok\n")
        smokelib.wait_for(proc, sel, "klock-shell-ok\n", log, timeout=30, start=at)
        print("\nsmoke-klock: PASS (threads=%d cpus=%d max_parallel=%d spin_iters=%d "
              "contended=%d mutex_iters=%d; pingpong=%d+%d sleeps=%d shootdowns=%d "
              "tlb_checks=%d mem_rounds=%d kstacks=%d)" % (threads, cpus, par, spins, cont,
                                                           mutexes, pp0, pp1, sleeps, sds,
                                                           checks, mems, kstacks))
    finally:
        proc.kill()
        proc.wait()
        os.makedirs(os.path.join(ROOT, "build"), exist_ok=True)
        with open(os.path.join(ROOT, "build", "smoke-klock.log"), "w") as f:
            f.write("".join(log))


if __name__ == "__main__":
    main()
