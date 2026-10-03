#!/usr/bin/env python3
"""SMP stress: fork/exec, pipes, mmap and tar|gzip loops on every CPU at once.

Boots kernel.elf + initrd.tar (KVM when /dev/kvm is usable) with -smp N, logs
in on the serial console and runs, for --secs seconds, in parallel:

  forks   busybox sh fork+exec of /true in a loop (2 of them)
  pipes   echo | cat | wc pipelines, plus `schedlat scale pipe 2`
  mmap    `schedlat scale mmap 2` and `schedlat scale stat 2`
  targz   busybox tar cf - /bin | gzip | wc -c

Each worker counts its rounds; at the end the shell must answer, every worker
must report STRESS-<name> rounds=N with N > 0, and the console must show no
panic, kwatch STALL, lockdep report or page fault in the kernel.  Used for
the stage-1 BKL work (docs/smp-plan.md).

  python3 tools/stress_smp.py [--smp 4] [--secs 180]
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
BAD = ("PANIC", "STALL", "[lockdep]", "slow to ack")


def worker(name, body, secs):
    # Time-boxed loop in its own busybox sh; prints its round count at the end.
    return ("(e=$(($(busybox date +%%s)+%d)); n=0; "
            "while [ $(busybox date +%%s) -lt $e ]; do %s; n=$((n+1)); done; "
            "echo STRESS-%s rounds=$n) &" % (secs, body, name))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--smp", type=int, default=4)
    ap.add_argument("--secs", type=int, default=180)
    ap.add_argument("--kernel", default="kernel.elf")
    a = ap.parse_args()

    kvm = os.access("/dev/kvm", os.R_OK | os.W_OK)
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
    workers = [
        ("forks1", "i=0; while [ $i -lt 20 ]; do /true; i=$((i+1)); done"),
        ("forks2", "i=0; while [ $i -lt 20 ]; do /true; i=$((i+1)); done"),
        ("pipes", "echo stress | busybox cat | busybox cat | busybox wc -c >/dev/null; "
                  "/schedlat scale pipe 2 200 >/dev/null"),
        ("mmap", "/schedlat scale mmap 2 200 >/dev/null; /schedlat scale stat 2 200 >/dev/null"),
        ("targz", "busybox tar cf - /bin | busybox gzip | busybox wc -c >/dev/null"),
    ]
    try:
        smokelib.login(proc, sel, log, timeout=120 if kvm else 300)
        # Into busybox sh (job control syntax, $((...)), &, wait).
        at = smokelib.mark(log)
        smokelib.send(proc, "busybox sh\n")
        smokelib.send(proc, "echo BBSH-$((1+1))\n")
        smokelib.wait_for(proc, sel, "BBSH-2", log, timeout=30, start=at)
        at = smokelib.mark(log)
        script = " ".join(worker(n, b, a.secs) for n, b in workers) + " wait; echo STRESS-ALL-DONE-$((40+2))\n"
        smokelib.send(proc, script)
        # (The echoed command line carries the $((...)), not the value.)
        smokelib.wait_for(proc, sel, "STRESS-ALL-DONE-42", log, timeout=a.secs + 600, start=at)
        # The machine still answers afterwards.
        at2 = smokelib.mark(log)
        smokelib.send(proc, "echo ALIVE-$((6*7))\n")
        smokelib.wait_for(proc, sel, "ALIVE-42", log, timeout=30, start=at2)
        out = "".join(log)[at:]
        rounds = dict(re.findall(r"STRESS-(\w+) rounds=(\d+)", out))
        for n, _ in workers:
            if int(rounds.get(n, 0)) <= 0:
                raise AssertionError("worker %s made no progress (%r)" % (n, rounds))
        whole = "".join(log)
        bad = [b for b in BAD if b in whole]
        if bad:
            raise AssertionError("console shows %s" % bad)
        print("\n[STRESS-SMP] passed smp=%d secs=%d rounds=%s" % (a.smp, a.secs, rounds))
    finally:
        proc.kill()
        proc.wait()
        os.makedirs(os.path.join(ROOT, "build"), exist_ok=True)
        with open(os.path.join(ROOT, "build", "stress-smp%d.log" % a.smp), "w") as f:
            f.write("".join(log))


if __name__ == "__main__":
    main()
