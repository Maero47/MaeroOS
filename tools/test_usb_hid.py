#!/usr/bin/env python3
"""Host test for the USB HID report-descriptor parser and pointer scaling
(drivers/usb/usb_hid.c): builds tools/test_usb_hid.c with the host compiler
and feeds it hostile descriptors (the full 32-bit logical range, degenerate
and reversed ranges, zero-size fields, truncated items) and random
descriptors and reports from the item grammar."""
import os
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))


def main():
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "test_usb_hid")
        cmd = ["cc", "-std=gnu99", "-O2", "-Wall", "-Wextra", "-Werror",
               "-I", os.path.join(ROOT, "drivers"),
               "-I", os.path.join(ROOT, "include"),
               os.path.join(ROOT, "tools", "test_usb_hid.c"), "-o", exe]
        # ASan/UBSan catch a parser walking off a buffer; plain if the host
        # compiler lacks the runtimes.
        if subprocess.run(cmd + ["-fsanitize=address,undefined",
                                 "-fno-sanitize-recover"]).returncode:
            subprocess.run(cmd, check=True)
        return subprocess.run([exe]).returncode


if __name__ == "__main__":
    sys.exit(main())
