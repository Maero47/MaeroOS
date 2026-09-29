#!/usr/bin/env python3
"""Host test for pkg's package-safety code (userspace/pkg/tarx.c, sha256.c).

Builds userspace/pkg/test_tarx.c with the host compiler, then feeds it
well-formed and malicious tarballs: absolute names, `..` components, nested
paths, symlinks/hardlinks, prefixed names, bad checksums and truncation must
all be rejected without writing anything; a normal package must extract.
Also checks package-name validation and that the SHA-256 matches hashlib and
the digests tools/mkrepo.py put in repo/index.txt (if a repo was built).
"""
import hashlib
import io
import os
import subprocess
import sys
import tarfile
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SRC = os.path.join(ROOT, "userspace", "pkg")
failures = 0


def check(ok, what):
    global failures
    print(("ok   " if ok else "FAIL ") + what)
    if not ok:
        failures += 1


def member(name, data=b"x", mode=0o644, type_=tarfile.REGTYPE, link=""):
    ti = tarfile.TarInfo(name)
    ti.size = len(data) if type_ == tarfile.REGTYPE else 0
    ti.mode = mode
    ti.type = type_
    ti.linkname = link
    return ti, (io.BytesIO(data) if type_ == tarfile.REGTYPE else None)


def make_tar(path, members):
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w", format=tarfile.USTAR_FORMAT) as tf:
        for ti, data in members:
            tf.addfile(ti, data)
    with open(path, "wb") as f:
        f.write(buf.getvalue())
    return buf.getvalue()


def run(tool, *args):
    p = subprocess.run([tool, *args], capture_output=True, text=True)
    return p.returncode, p.stdout.strip()


def main():
    with tempfile.TemporaryDirectory() as tmp:
        tool = os.path.join(tmp, "test_tarx")
        subprocess.run(["cc", "-std=gnu99", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-o", tool, os.path.join(SRC, "test_tarx.c"),
                        os.path.join(SRC, "tarx.c"), os.path.join(SRC, "sha256.c")],
                       check=True)

        # Package names
        for name in ["doom", "links", "busybox", "lib-2.0_x"]:
            check(run(tool, "name", name)[0] == 0, f"name {name!r} accepted")
        for name in ["", ".", "..", "../etc", "a/b", "/etc", "-rf", ".hidden",
                     "x" * 32, "a b", "a\nb"]:
            check(run(tool, "name", name)[0] == 1, f"name {name!r} refused")

        # A normal package extracts, keeps exec bits, handles a 100-char name
        long_name = "n" * 100
        good = os.path.join(tmp, "good.tar")
        raw = make_tar(good, [member("readme", b"hello\n"),
                              member("./tool", b"\x7fELF", mode=0o755),
                              member(long_name, b"long")])
        dest = os.path.join(tmp, "good")
        os.mkdir(dest)
        rc, out = run(tool, "extract", good, dest)
        check(rc == 0 and out == "files=3", f"good package extracts ({out})")
        check(open(os.path.join(dest, "readme"), "rb").read() == b"hello\n",
              "file content intact")
        check(os.stat(os.path.join(dest, "tool")).st_mode & 0o111 != 0,
              "executable bit kept")
        check(sorted(os.listdir(dest)) == sorted(["readme", "tool", long_name]),
              "100-char name not run into the mode field")
        check(run(tool, "sha", good)[1] == hashlib.sha256(raw).hexdigest(),
              "sha256 matches hashlib")

        # Malicious / malformed archives: rejected, nothing written anywhere
        evil = {
            "dotdot": [member("../escape")],
            "dotdot-nested": [member("sub/../../escape")],
            "dot-slash-dotdot": [member("./../escape")],
            "absolute": [member("/tmp/escape-abs")],
            "nested": [member("sub/file")],
            "symlink": [member("link", type_=tarfile.SYMTYPE, link="/etc")],
            "hardlink": [member("hard", type_=tarfile.LNKTYPE, link="/etc/passwd")],
            "directory": [member("dir", type_=tarfile.DIRTYPE)],
            "prefixed": [member("p" * 60 + "/" + "q" * 60)],
            "good-then-evil": [member("readme"), member("../escape")],
        }
        for label, members in evil.items():
            t = os.path.join(tmp, label + ".tar")
            make_tar(t, members)
            d = os.path.join(tmp, "x-" + label)
            os.mkdir(d)
            rc, out = run(tool, "extract", t, d)
            check(rc == 1 and not os.listdir(d) and
                  not os.path.exists(os.path.join(tmp, "escape")),
                  f"{label}: rejected, nothing written ({out})")

        # Corruption
        for label, mutate in [("bad-checksum", lambda b: b[:10] + b"Z" + b[11:]),
                              ("truncated", lambda b: b[:600]),
                              ("no-end-marker", lambda b: b[:1024]),
                              ("not-a-tar", lambda b: b"\x01" * 2048)]:
            t = os.path.join(tmp, label + ".tar")
            with open(t, "wb") as f:
                f.write(mutate(raw))
            d = os.path.join(tmp, "x-" + label)
            os.mkdir(d)
            rc, out = run(tool, "extract", t, d)
            check(rc == 1 and not os.listdir(d), f"{label}: rejected ({out})")

        # The real repo, if built: digests agree and every package extracts
        index = os.path.join(ROOT, "repo", "index.txt")
        if os.path.exists(index):
            for line in open(index).read().splitlines():
                f = line.split("|")
                tar = os.path.join(ROOT, "repo", f[3])
                check(len(f) == 10 and run(tool, "sha", tar)[1] == f[9],
                      f"repo {f[0]}: index sha256 matches")
                d = os.path.join(tmp, "repo-" + f[0])
                os.mkdir(d)
                check(run(tool, "extract", tar, d)[0] == 0, f"repo {f[0]}: extracts")

    print("[TEST-PKG] " + ("passed" if not failures else f"FAILED ({failures})"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
