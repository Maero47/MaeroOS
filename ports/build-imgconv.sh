#!/bin/sh
# Runs INSIDE the maeros-cross container: builds the static imgconv tool
# (PNG/JPEG/BMP/PPM -> PPM) against the libpng/libjpeg/zlib already built by
# build-links.sh into /build/sysroot.
set -e

CC=i686-linux-musl-gcc
PREFIX=/build/sysroot
CFLAGS="-O2 -static -fno-pie -I$PREFIX/include"
LDFLAGS="-static -no-pie -L$PREFIX/lib"

if [ ! -f "$PREFIX/lib/libpng.a" ]; then
    echo "imgconv: libpng not in sysroot — run build-links.sh first" >&2
    exit 1
fi

mkdir -p /build/out
echo "=== imgconv ==="
$CC $CFLAGS imgconv/imgconv.c $LDFLAGS -lpng16 -ljpeg -lz -o /build/out/imgconv
i686-linux-musl-strip /build/out/imgconv
echo "BUILT:"
file /build/out/imgconv
