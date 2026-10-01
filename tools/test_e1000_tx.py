#!/usr/bin/env python3
"""Host test for the e1000 TX ring bookkeeping (drivers/e1000_txq.h):
builds tools/test_e1000_tx.c with the host compiler and runs it.  Covers the
one-free-slot rule, the 2 s stall watchdog, and a steady upload whose
completions are reclaimed by the send path (no false stall)."""
import os
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))


def main():
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "test_e1000_tx")
        subprocess.run(["cc", "-std=gnu99", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-I", os.path.join(ROOT, "drivers"),
                        os.path.join(ROOT, "tools", "test_e1000_tx.c"), "-o", exe],
                       check=True)
        return subprocess.run([exe]).returncode


if __name__ == "__main__":
    sys.exit(main())
