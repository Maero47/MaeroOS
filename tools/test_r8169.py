#!/usr/bin/env python3
"""Host test for the Realtek r8169 driver core (drivers/r8169_hw.h): builds
tools/test_r8169.c with the host compiler and runs it.  No emulator models
these chips, so the test drives the driver's start/stop/reset sequences,
MDIO and descriptor rings against a simulated register file and DMA engine:
TxConfig XID chip table, per-generation init ordering (8168G+ RXDV gate),
PHY autonegotiation setup, TX/RX ring wraparound, full rings, RDU recovery,
OWN handling, buffer recycling and error/fragment drops."""
import os
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))


def main():
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "test_r8169")
        cmd = ["cc", "-std=gnu99", "-O2", "-Wall", "-Wextra", "-Werror",
               "-I", os.path.join(ROOT, "drivers"),
               os.path.join(ROOT, "tools", "test_r8169.c"), "-o", exe]
        # ASan/UBSan catch a DMA model walking off a ring; plain if the
        # host compiler lacks the runtimes.
        if subprocess.run(cmd + ["-fsanitize=address,undefined",
                                 "-fno-sanitize-recover"]).returncode:
            subprocess.run(cmd, check=True)
        return subprocess.run([exe]).returncode


if __name__ == "__main__":
    sys.exit(main())
