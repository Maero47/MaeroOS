#!/usr/bin/env python3
"""Host test for the kernel random generator (kernel/random.c): builds
tools/test_random.c with the host compiler and checks the ChaCha20 block
against RFC 8439, request lengths, byte frequencies, non-sequential port
draws and RFC 6528 TCP ISNs."""
import os
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))


def main():
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "test_random")
        cmd = ["cc", "-std=gnu99", "-O2", "-Wall", "-Wextra", "-Werror",
               "-I", os.path.join(ROOT, "drivers"),
               "-I", os.path.join(ROOT, "include"),
               os.path.join(ROOT, "tools", "test_random.c"), "-o", exe]
        # ASan/UBSan catch a write past a request; plain if the host
        # compiler lacks the runtimes.
        if subprocess.run(cmd + ["-fsanitize=address,undefined",
                                 "-fno-sanitize-recover"]).returncode:
            subprocess.run(cmd, check=True)
        return subprocess.run([exe]).returncode


if __name__ == "__main__":
    sys.exit(main())
