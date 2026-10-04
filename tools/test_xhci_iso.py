#!/usr/bin/env python3
"""Host test of the xHCI isochronous TD fields (drivers/usb/xhci_iso.h):
builds tools/test_xhci_iso.c with the host compiler (ASan/UBSan when
available) and runs it; hostile descriptors (a companion on a full-speed
device, bMaxBurst 255, Mult 3) must never give a TD whose burst counts
spill into the TRB's BEI or type bits."""
import os
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))


def main():
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "test_xhci_iso")
        cmd = ["cc", "-std=gnu99", "-O2", "-Wall", "-Wextra", "-Werror",
               "-I", os.path.join(ROOT, "drivers"),
               "-I", os.path.join(ROOT, "include"),
               os.path.join(ROOT, "tools", "test_xhci_iso.c"), "-o", exe]
        # ASan/UBSan catch a parser walking off a buffer; plain if the host
        # compiler lacks the runtimes.
        if subprocess.run(cmd + ["-fsanitize=address,undefined",
                                 "-fno-sanitize-recover"]).returncode:
            subprocess.run(cmd, check=True)
        return subprocess.run([exe]).returncode


if __name__ == "__main__":
    sys.exit(main())
