#!/bin/sh
# Build the dynamically-linked dynprobe + export the musl dynamic linker.
# Run inside the maeros-cross container with /out bind-mounted.
set -e
i686-linux-musl-gcc -O2 -fpie -pie /build/dynprobe/dynprobe.c -o /out/dynprobe
# ld-musl-i386.so.1 is musl's libc.so; ship the real file (symlink target).
cp /opt/i686-linux-musl-cross/i686-linux-musl/lib/libc.so /out/ld-musl-i386.so.1
echo "dynprobe + ld-musl built"
