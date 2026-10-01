#!/usr/bin/env python3
"""Scheduler benchmark: wake latency and throughput under CPU hogs.

Boots kernel.elf + initrd.tar (KVM when /dev/kvm is usable, else TCG), logs
in on the serial console and runs testfiles/schedlat:

  pipe / futex   thread A wakes thread B (pipe write / FUTEX_WAKE) while
                 HOGS busy-looping processes run; rdtsc wake-to-run latency
  audio          a producer sends a buffer every 10 ms through a pipe to a
                 consumer while hogs run; late deliveries are counted

and the throughput workloads timed by `schedlat time`: a busybox sh
arithmetic loop, 200 fork+exec, tar|gzip of /lib /bin /usr, and four sh loops
at once (each three times).  Every result line is appended to build/bench-sched/results.txt
under a header naming the run (--tag, default the git HEAD).

  python3 tools/bench_sched.py [--smp N] [--tag NAME] [--tcg] [--iters N]
"""
import argparse
import os
import selectors
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(__file__))
import smokelib  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
PROMPT = smokelib.PROMPT
OUT = os.path.join(ROOT, "build", "bench-sched")

def commands(smp, iters):
    hogs = smp  # one hog per CPU: every CPU is busy when the wake happens
    return [
        "/schedlat pipe 0 %d" % iters,
        "/schedlat pipe %d %d" % (hogs, iters),
        "/schedlat futex %d %d" % (hogs, iters),
        "/schedlat audio %d %d" % (hogs, iters),
        "/schedlat audio %d %d" % (hogs * 2, iters),
        "/schedlat nice",
    ] + ["/schedlat time " + w for w in ("shloop", "forks", "targz", "par4")
         for _ in range(3)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--smp", type=int, default=1)
    ap.add_argument("--tag", default=None)
    ap.add_argument("--tcg", action="store_true")
    ap.add_argument("--iters", type=int, default=500)
    a = ap.parse_args()

    kvm = not a.tcg and os.access("/dev/kvm", os.R_OK | os.W_OK)
    tag = a.tag or subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=ROOT,
                                  capture_output=True, text=True).stdout.strip()
    os.makedirs(OUT, exist_ok=True)
    cmd = ["qemu-system-i386", *smokelib.QEMU_DISPLAY, "-kernel", "kernel.elf",
           "-initrd", "initrd.tar", "-serial", "stdio", "-m", "512M",
           "-no-reboot", "-no-shutdown", "-smp", str(a.smp)]
    if kvm:
        cmd += ["-accel", "kvm"]
    proc = subprocess.Popen(cmd, cwd=ROOT, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, bufsize=0)
    sel = selectors.DefaultSelector()
    sel.register(proc.stdout, selectors.EVENT_READ)
    log = []
    results = []
    try:
        smokelib.login(proc, sel, log, timeout=120 if kvm else 300)
        for c in commands(a.smp, a.iters):
            at = smokelib.mark(log)
            smokelib.send(proc, c + "\n")
            smokelib.wait_for(proc, sel, PROMPT, log, timeout=600, start=at)
            out = "".join(log)[at:]
            for line in out.splitlines():
                if line.startswith("schedlat ") and "done" not in line:
                    results.append(line.strip())
    finally:
        proc.kill()
        proc.wait()
        with open(os.path.join(OUT, "serial-%s-smp%d.log" % (tag, a.smp)), "w") as f:
            f.write("".join(log))
    head = "== %s smp=%d accel=%s %s" % (tag, a.smp, "kvm" if kvm else "tcg",
                                         time.strftime("%Y-%m-%d %H:%M"))
    with open(os.path.join(OUT, "results.txt"), "a") as f:
        f.write(head + "\n" + "\n".join(results) + "\n")
    print("\n" + head)
    print("\n".join(results))


if __name__ == "__main__":
    main()
