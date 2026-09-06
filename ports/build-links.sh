#!/bin/sh
# Runs INSIDE the maeros-cross container: builds zlib, libpng, jpeg, then a
# static links2 with framebuffer graphics for i686 MaeroOS.
set -e

CC=i686-linux-musl-gcc
PREFIX=/build/sysroot
export CC
export AR=i686-linux-musl-ar
export RANLIB=i686-linux-musl-ranlib
export STRIP=i686-linux-musl-strip
export CFLAGS="-O2 -static -fno-pie"
export LDFLAGS="-static -no-pie -L$PREFIX/lib"
export CPPFLAGS="-I$PREFIX/include"
mkdir -p "$PREFIX"

cd /build

echo "=== zlib ==="
tar xzf zlib-1.3.1.tar.gz
cd zlib-1.3.1
./configure --prefix="$PREFIX" --static >/dev/null
make -j4 >/dev/null && make install >/dev/null
cd ..

echo "=== libpng ==="
tar xzf libpng-1.6.43.tar.gz
cd libpng-1.6.43
./configure --host=i686-linux-musl --prefix="$PREFIX" \
    --disable-shared --enable-static >/dev/null
make -j4 >/dev/null && make install >/dev/null
cd ..

echo "=== jpeg ==="
tar xzf jpegsrc.v9f.tar.gz
cd jpeg-9f
./configure --host=i686-linux-musl --prefix="$PREFIX" \
    --disable-shared --enable-static >/dev/null
make -j4 >/dev/null && make install >/dev/null
cd ..

echo "=== gpm stub ==="
$CC $CFLAGS -c gpm-stub/gpm-stub.c -o gpm-stub/gpm-stub.o
$AR rcs "$PREFIX/lib/libgpm.a" gpm-stub/gpm-stub.o
cp gpm-stub/gpm.h "$PREFIX/include/gpm.h"

echo "=== links2 ==="
rm -rf links-2.30          # stale tree keeps old .o files: tar restores
tar xzf links-2.30.tar.gz  # original mtimes and make would skip the relink
cd links-2.30
./configure --host=i686-linux-musl \
    --enable-graphics \
    --with-fb \
    --without-x \
    --without-directfb \
    --without-librsvg \
    --without-openmp \
    --without-ssl \
    --without-zstd \
    --without-brotli \
    --without-freetype \
    --without-libtiff \
    --without-libavif \
    >/tmp/links-configure.log 2>&1 || (tail -30 /tmp/links-configure.log; exit 1)
make -j4 2>/tmp/links-make-err.log || (tail -30 /tmp/links-make-err.log; exit 1)
cp links /build/out/links
i686-linux-musl-strip /build/out/links
echo "BUILT:"
file /build/out/links
