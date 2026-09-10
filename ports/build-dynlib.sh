#!/bin/sh
# Phase 31 proofs of EXTERNAL shared-library loading.  Run inside the
# maeros-cross container with /build = ports/ and /out = testfiles/ bind-mounted.
set -e
CC=i686-linux-musl-gcc

cd /build/dynlib

echo "=== Proof A: libgreet.so.1 + dynprobe2 ==="
$CC -O2 -fPIC -shared -Wl,-soname,libgreet.so.1 greet.c -o /out/libgreet.so.1
# Link dynprobe2 against the freshly built lib (DT_NEEDED = libgreet.so.1).
cp /out/libgreet.so.1 /tmp/libgreet.so.1
ln -sf /tmp/libgreet.so.1 /tmp/libgreet.so
$CC -O2 -fpie -pie dynprobe2.c -L/tmp -lgreet -o /out/dynprobe2
i686-linux-musl-readelf -d /out/dynprobe2 | grep -i needed || true

echo "=== Proof B: shared libz.so.1 + zprobe ==="
ZS=/build/zlib-1.3.1
ZOBJ="adler32 crc32 deflate infback inffast inflate inftrees trees zutil \
      compress uncompr gzclose gzlib gzread gzwrite"
rm -rf /tmp/zbuild && mkdir -p /tmp/zbuild
for o in $ZOBJ; do
    $CC -O2 -fPIC -DHAVE_HIDDEN -I"$ZS" -c "$ZS/$o.c" -o "/tmp/zbuild/$o.o"
done
$CC -shared -Wl,-soname,libz.so.1 -o /out/libz.so.1 /tmp/zbuild/*.o
# Provide an unversioned -lz link name (soname keeps DT_NEEDED = libz.so.1).
cp /out/libz.so.1 /tmp/libz.so.1
ln -sf /tmp/libz.so.1 /tmp/libz.so
$CC -O2 -fpie -pie -I"$ZS" zprobe.c -L/tmp -lz -o /out/zprobe
i686-linux-musl-readelf -d /out/zprobe | grep -i needed || true

echo "=== Proof C: pthreadprobe (real multithreading) ==="
$CC -O2 -fpie -pie pthreadprobe.c -lpthread -o /out/pthreadprobe

echo "=== Proof D: usockprobe (AF_UNIX sockets) ==="
$CC -O2 -fpie -pie usockprobe.c -o /out/usockprobe

echo "=== Proof E: xprobe (X11 handshake client) ==="
$CC -O2 -fpie -pie xprobe.c -o /out/xprobe

echo "=== Proof F: xdraw (X11 drawing client) ==="
$CC -O2 -fpie -pie xdraw.c -o /out/xdraw

echo "=== Proof G: xevent (X11 events client) ==="
$CC -O2 -fpie -pie xevent.c -o /out/xevent

echo "=== Proof H: xkey (X11 keyboard client) ==="
$CC -O2 -fpie -pie xkey.c -o /out/xkey

echo "dynlib proofs built: libgreet.so.1 dynprobe2 libz.so.1 zprobe pthreadprobe usockprobe xprobe xdraw xevent xkey"
