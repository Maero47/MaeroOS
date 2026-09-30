# TweetNaCl 20140427

Unmodified copies of `tweetnacl.c` and `tweetnacl.h` from
<https://tweetnacl.cr.yp.to/software.html> (version 20140427). Public domain,
see `LICENSE`.

    sha256  02e65bc3013ff2168983365e55906bc783c4c7e0a60d8100f17bb303a17175c4  tweetnacl.c
    sha256  43f29ad721d9927b747b0100ab4160c119e7bb180c7c98a66e4bf79d31244287  tweetnacl.h

MaeroOS uses only `crypto_sign_open` (Ed25519 verification) from it, in
`userspace/pkg/ed25519.c`, to check the signature on the package repository
index. `userspace/pkg` supplies the `randombytes` symbol the library expects
(pkg never generates keys). `tools/test_pkg_sign.py` runs the RFC 8032
test vectors against it on the host.
