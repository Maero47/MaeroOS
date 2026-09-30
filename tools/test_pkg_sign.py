#!/usr/bin/env python3
"""Host test for the signed package index.

Checks the pure-Python Ed25519 signer (tools/ed25519.py, used by mkrepo.py)
and pkg's C verifier (userspace/pkg/ed25519.c over third_party/tweetnacl,
built here with the host compiler) against the RFC 8032 section 7.1 test
vectors, then feeds pkg's index check (userspace/pkg/indexsig.c) a signed
index and tampered variants: a valid index is accepted, one flipped byte in
the index or its signature, a missing or malformed signature, another key's
signature and trailing data are all rejected.  Also checks that mkrepo
creates its signing key on first use and that repo/index.txt (if built)
verifies under the key pkg is compiled with.
"""
import json
import os
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SRC = os.path.join(ROOT, "userspace", "pkg")
NACL = os.path.join(ROOT, "third_party", "tweetnacl")
sys.path.insert(0, os.path.join(ROOT, "tools"))
import ed25519  # noqa: E402
import mkrepo  # noqa: E402

L = 2**252 + 27742317777372353535851937790883648493
failures = 0


def check(ok, what):
    global failures
    print(("ok   " if ok else "FAIL ") + what)
    if not ok:
        failures += 1


def run(*args):
    p = subprocess.run(list(args), capture_output=True, text=True)
    return p.returncode, p.stdout.strip()


def flip(data, i):
    b = bytearray(data)
    b[i] ^= 0x01
    return bytes(b)


def main():
    vectors = json.load(open(os.path.join(ROOT, "tools", "rfc8032_ed25519.json")))
    with tempfile.TemporaryDirectory() as tmp:
        tool = os.path.join(tmp, "test_indexsig")
        nacl_o = os.path.join(tmp, "tweetnacl.o")
        subprocess.run(["cc", "-O2", "-w", "-c", "-o", nacl_o,
                        os.path.join(NACL, "tweetnacl.c")], check=True)
        subprocess.run(["cc", "-std=gnu99", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-I" + NACL, "-o", tool,
                        os.path.join(SRC, "test_indexsig.c"),
                        os.path.join(SRC, "indexsig.c"),
                        os.path.join(SRC, "ed25519.c"), nacl_o],
                       check=True)
        msgf = os.path.join(tmp, "msg")

        def c_verify(pk, sig, msg):
            with open(msgf, "wb") as f:
                f.write(msg)
            return run(tool, "verify", pk.hex(), sig.hex(), msgf)[0] == 0

        # RFC 8032 section 7.1, both directions
        for v in vectors["vectors"]:
            sk, pk = bytes.fromhex(v["secret"]), bytes.fromhex(v["public"])
            msg, sig = bytes.fromhex(v["message"]), bytes.fromhex(v["signature"])
            name = f"RFC 8032 TEST {v['name']}"
            check(ed25519.public_key(sk) == pk, f"{name}: python public key")
            check(ed25519.sign(sk, msg) == sig, f"{name}: python signature")
            check(c_verify(pk, sig, msg), f"{name}: C verifier accepts")
            check(not c_verify(pk, flip(sig, 0), msg), f"{name}: C rejects flipped R")
            check(not c_verify(pk, flip(sig, 40), msg), f"{name}: C rejects flipped S")
            if msg:
                check(not c_verify(pk, sig, flip(msg, len(msg) - 1)),
                      f"{name}: C rejects flipped message")
            check(not c_verify(pk, sig, msg + b"x"), f"{name}: C rejects extended message")
            check(not c_verify(flip(pk, 3), sig, msg), f"{name}: C rejects other key")
            # Same signature with S + L: TweetNaCl alone would accept it.
            s = int.from_bytes(sig[32:], "little") + L
            check(not c_verify(pk, sig[:32] + s.to_bytes(32, "little"), msg),
                  f"{name}: C rejects non-canonical S")

        # Independent cross-check, if the host has the `cryptography` package
        try:
            from cryptography.hazmat.primitives.asymmetric.ed25519 import (
                Ed25519PrivateKey)
        except ImportError:
            print("skip cross-check with `cryptography` (not installed)")
        else:
            for n in range(8):
                seed, msg = os.urandom(32), os.urandom(n * 37)
                ref = Ed25519PrivateKey.from_private_bytes(seed)
                check(ed25519.sign(seed, msg) == ref.sign(msg),
                      f"python signer == cryptography ({len(msg)}-byte message)")

        # The signed index format
        seed = os.urandom(32)
        pk = ed25519.public_key(seed)
        lines = ["hello|1.0|10240|hello-1.0.tar|Hello|/disk/apps/hello/hello|0||0|"
                 + "ab" * 32,
                 "doom|1.9|4096000|doom-1.9.tar|DOOM|/disk/apps/doom/doom|1|-iwad x|1|"
                 + "cd" * 32]
        good = mkrepo.sign_index(lines, seed, 1727700000).encode()
        sig_at = good.index(b"#sig ")
        idx = os.path.join(tmp, "index.txt")

        def c_index(data, key=pk):
            with open(idx, "wb") as f:
                f.write(data)
            return run(tool, "index", key.hex(), idx)

        rc, out = c_index(good)
        check(rc == 0 and out == f"serial=1727700000 signed={sig_at}",
              f"valid index accepted ({out})")
        rc, out = c_index(good.rstrip(b"\n"))
        check(rc == 0, f"valid index without final newline accepted ({out})")

        # One flipped byte anywhere in the signed part or the signature
        positions = sorted({0, 10, 25, good.index(b"hello"), good.index(b"cdcd"),
                            sig_at - 1, sig_at + 13, sig_at + 13 + 64,
                            len(good) - 2})
        for i in positions:
            rc, out = c_index(flip(good, i))
            check(rc == 1, f"flipped byte {i} ({good[i:i + 1]!r}) rejected ({out})")
        rc, out = c_index(good[:sig_at] + b"#sig ed25519 " +
                          good[sig_at + 13:].upper())
        check(rc == 0, "upper-case signature hex accepted")

        rejects = {
            "missing signature": good[:sig_at],
            "signature line emptied": good[:sig_at] + b"#sig ed25519 \n",
            "truncated signature": good[:-3] + b"\n",
            "non-hex signature": good[:sig_at + 13] + b"zz" + good[sig_at + 15:],
            "unsigned old-format index": "\n".join(lines).encode() + b"\n",
            "empty index": b"",
            "line appended after signature": good + lines[0].encode() + b"\n",
            "extra newline after signature": good + b"\n",
            "line inserted before signature":
                good[:sig_at] + lines[0].encode() + b"\n" + good[sig_at:],
            "signed by another key":
                mkrepo.sign_index(lines, os.urandom(32), 1727700000).encode(),
            "NUL in signed part": mkrepo.sign_index(
                [lines[0] + "\0x"], seed, 1).encode(),
            "serial not a number": mkrepo.sign_index(lines, seed, "x1").encode(),
            "serial overflows 32 bits":
                mkrepo.sign_index(lines, seed, 2**32).encode(),
        }
        for label, data in rejects.items():
            rc, out = c_index(data)
            check(rc == 1, f"{label}: rejected ({out})")
        rc, out = c_index(good, key=flip(pk, 0))
        check(rc == 1, f"verified against another key: rejected ({out})")

        # mkrepo creates the key on first use and emits a stable header
        env = dict(os.environ, MAEROS_REPO_KEY=os.path.join(tmp, "k", "repo.key"))
        hdr = os.path.join(tmp, "repo_pubkey.h")
        mk = os.path.join(ROOT, "tools", "mkrepo.py")
        rc = subprocess.run([sys.executable, mk, "--pubkey-header", hdr],
                            env=env, capture_output=True).returncode
        key = env["MAEROS_REPO_KEY"]
        check(rc == 0 and os.path.exists(key) and
              os.stat(key).st_mode & 0o777 == 0o600,
              "mkrepo creates a 0600 signing key on first use")
        tseed = bytes.fromhex(open(key).read().strip())
        check(ed25519.public_key(tseed).hex()[:2] in open(hdr).read(),
              "pubkey header written")
        mtime = os.stat(hdr).st_mtime_ns
        subprocess.run([sys.executable, mk, "--pubkey-header", hdr], env=env,
                       check=True)
        check(os.stat(hdr).st_mtime_ns == mtime and
              open(key).read().strip() == tseed.hex(),
              "second run keeps the key and leaves the header untouched")

        # The real repo, if built, verifies under the key pkg is built with
        index = os.path.join(ROOT, "repo", "index.txt")
        if os.path.exists(index):
            real_pk = ed25519.public_key(mkrepo.load_key())
            rc, out = c_index(open(index, "rb").read(), key=real_pk)
            check(rc == 0, f"repo/index.txt verifies under the repo key ({out})")

    print("[TEST-PKG-SIGN] " + ("passed" if not failures else f"FAILED ({failures})"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
