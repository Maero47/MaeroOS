#!/usr/bin/env python3
"""Host test for the libc regex engine (userspace/libc/regex.c).

Copies regex.c next to its public headers with the includes pointed at the
host libc, builds userspace/libc/test_regex.c against it with the host
compiler and runs it: the matching/submatch/fnmatch cases must agree, and
patterns that backtrack exponentially without memoisation ((a|aa)*b,
(a*)*b, ...) must finish in under 50 ms each.
"""
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
LIBC = os.path.join(ROOT, "userspace", "libc")
INC = os.path.join(ROOT, "userspace", "include")
OWN = ("regex.h", "fnmatch.h")


def main():
    with tempfile.TemporaryDirectory() as tmp:
        src = open(os.path.join(LIBC, "regex.c")).read()

        def inc(m):
            name = m.group(1)
            return f'#include "{name}"' if name in OWN else f"#include <{name}>"
        src = re.sub(r'#include "\.\./include/([^"]+)"', inc, src)
        with open(os.path.join(tmp, "regex.c"), "w") as f:
            f.write(src)
        for name in OWN:
            with open(os.path.join(INC, name)) as a, open(os.path.join(tmp, name), "w") as b:
                b.write(a.read())
        tool = os.path.join(tmp, "test_regex")
        subprocess.run(["cc", "-std=gnu99", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-parameter", "-I", tmp, "-o", tool,
                        os.path.join(LIBC, "test_regex.c"), os.path.join(tmp, "regex.c")],
                       check=True)
        rc = subprocess.run([tool]).returncode
    print("[TEST-REGEX] " + ("passed" if rc == 0 else "FAILED"))
    return 1 if rc else 0


if __name__ == "__main__":
    sys.exit(main())
