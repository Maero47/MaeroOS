#!/usr/bin/env python3
"""Host test for the split virtqueue ring logic (drivers/virtio/virtqueue.c):
builds tools/test_virtqueue.c with the host compiler and runs it against a
simulated device: chain layout and flags, publish/notify suppression, a full
ring, out-of-order completion, mixed chain lengths across the 16-bit index
wrap, and a bogus used entry."""
import os
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))


def main():
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "test_virtqueue")
        cmd = ["cc", "-std=gnu99", "-O2", "-Wall", "-Wextra", "-Werror",
               "-I", os.path.join(ROOT, "drivers"),
               os.path.join(ROOT, "tools", "test_virtqueue.c"), "-o", exe]
        # ASan/UBSan catch a ring walk off the end; plain if the host
        # compiler lacks the runtimes.
        if subprocess.run(cmd + ["-fsanitize=address,undefined",
                                 "-fno-sanitize-recover"]).returncode:
            subprocess.run(cmd, check=True)
        return subprocess.run([exe]).returncode


if __name__ == "__main__":
    sys.exit(main())
