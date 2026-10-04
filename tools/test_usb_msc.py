#!/usr/bin/env python3
"""Host test for the USB mass-storage bulk-only transport (drivers/usb/
usb_msc.c): builds tools/test_usb_msc.c with the host compiler against a
simulated stick that ends data phases early and reports residues (or lies
about them); short reads and writes must fail and never return the shared
bounce buffer's stale bytes (another disk's blocks) as data."""
import os
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))


def main():
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "test_usb_msc")
        cmd = ["cc", "-std=gnu99", "-O2", "-Wall", "-Wextra", "-Werror",
               "-I", os.path.join(ROOT, "drivers"),
               "-I", os.path.join(ROOT, "include"),
               os.path.join(ROOT, "tools", "test_usb_msc.c"), "-o", exe]
        # ASan/UBSan catch a parser walking off a buffer; plain if the host
        # compiler lacks the runtimes.
        if subprocess.run(cmd + ["-fsanitize=address,undefined",
                                 "-fno-sanitize-recover"]).returncode:
            subprocess.run(cmd, check=True)
        return subprocess.run([exe]).returncode


if __name__ == "__main__":
    sys.exit(main())
