#!/usr/bin/env python3
"""Kernel-heap leak check: boot the initrd system, run `heapprobe N`, and
compare the kprof dumps it asks for before and after N rounds of fork/wait,
pipe, AF_UNIX connect/accept, UDP socket and tmpfs file churn.

The "heap live=" figure (payload in live kmalloc blocks) must come back to the
baseline; a leak of even one block per round would show as N blocks.
SMOKE_SMP=N boots with -smp N.  Usage: tools/heap_stress.py [rounds]
"""
import os
import re
import selectors
import subprocess
import sys

import smokelib

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
PROMPT = "MaeroOS$ "
LIVE = re.compile(r"heap live=(\d+)KiB/(\d+) blk free=(\d+)KiB/(\d+) blk")


def main():
    rounds = int(sys.argv[1]) if len(sys.argv) > 1 else 500
    cmd = ["qemu-system-i386", *smokelib.QEMU_DISPLAY, "-kernel", "kernel.elf",
           "-initrd", "initrd.tar", "-serial", "stdio", "-m", "512M",
           "-no-reboot", "-no-shutdown", "-smp", os.environ.get("SMOKE_SMP", "1")]
    proc = subprocess.Popen(cmd, cwd=ROOT, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, bufsize=0)
    sel = selectors.DefaultSelector()
    sel.register(proc.stdout, selectors.EVENT_READ)
    log = []
    try:
        smokelib.login(proc, sel, log)
        at = smokelib.mark(log)
        smokelib.send(proc, "heapprobe %d\n" % rounds)
        smokelib.wait_for(proc, sel, "heapprobe: done", log, 60.0 + rounds * 0.2, at)
        smokelib.wait_for(proc, sel, PROMPT, log, 20.0, at)
        out = "".join(log)[at:]
        snaps = LIVE.findall(out)
        if len(snaps) < 2:
            raise AssertionError("expected two kprof dumps, got %d" % len(snaps))
        (b_kib, b_blk, _, _), (a_kib, a_blk, _, _) = snaps[0], snaps[-1]
        print("\n[HEAP-STRESS] %d rounds: live %s KiB / %s blocks -> %s KiB / %s blocks"
              % (rounds, b_kib, b_blk, a_kib, a_blk))
        # Allow a handful of blocks for state that legitimately differs between
        # the two instants (a timer, the shell's own pending I/O).
        if int(a_blk) - int(b_blk) > 8:
            raise AssertionError("kernel heap grew by %d blocks over %d rounds"
                                 % (int(a_blk) - int(b_blk), rounds))
        print("[HEAP-STRESS] passed")
        return 0
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                proc.kill()


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print("\n[HEAP-STRESS] FAIL:", exc)
        raise SystemExit(1)
