#!/bin/bash
# Build SpiderMonkey (Firefox's JS engine) for i686-linux-musl, STATIC, to run
# on MaeroOS.  This is real Firefox code — the JIT, GC, parser.  Idempotent-ish.
set -e
export PATH=/root/.cargo/bin:/opt/i686-linux-musl-cross/bin:$PATH
cd /work
FF=firefox-115.15.0esr
SRC=$FF/source
TARBALL=$FF.source.tar.xz

if [ ! -d firefox-* ] 2>/dev/null && [ ! -d mozilla-* ] 2>/dev/null; then
  echo "=== downloading Firefox ESR 115 source (~480MB) ==="
  [ -f "$TARBALL" ] || wget -q "https://ftp.mozilla.org/pub/firefox/releases/115.15.0esr/source/$TARBALL"
  echo "=== extracting (~5GB) ==="
  tar xf "$TARBALL"
fi
SRCDIR=$(ls -d firefox-* mozilla-* 2>/dev/null | head -1)
echo "source: $SRCDIR"
cd "$SRCDIR"

echo "=== mozconfig for i686-musl static JS shell ==="
cat > mozconfig <<EOF
ac_add_options --enable-application=js
ac_add_options --target=i686-unknown-linux-musl
ac_add_options --host=x86_64-unknown-linux-gnu
ac_add_options --disable-jemalloc
ac_add_options --disable-tests
ac_add_options --enable-optimize
ac_add_options --disable-debug
mk_add_options MOZ_OBJDIR=/work/obj-js
export HOST_CC=clang
export HOST_CXX=clang++
EOF

export CC=i686-linux-musl-gcc
export CXX=i686-linux-musl-g++
export AR=i686-linux-musl-ar
export RANLIB=i686-linux-musl-ranlib
export RUST_TARGET=i686-unknown-linux-musl
export CARGO_BUILD_TARGET=i686-unknown-linux-musl
export HOST_CC=clang
export HOST_CXX=clang++
export HOST_CFLAGS="-O2"
export HOST_CXXFLAGS="-O2"
export PKG_CONFIG_LIBDIR=/sysroot/lib/pkgconfig:/sysroot/share/pkgconfig
export PKG_CONFIG_PATH=$PKG_CONFIG_LIBDIR
export PKG_CONFIG_SYSROOT_DIR=
export CFLAGS="-I/sysroot/include"
export CXXFLAGS="-I/sysroot/include"
export LDFLAGS="-L/sysroot/lib"

echo "=== mach configure (JS) ==="
python3 ./mach --no-interactive configure 2>&1 | tail -40
echo "CONFIGURE_EXIT=$?"

echo "=== mach build (SpiderMonkey + JS shell) — the long part ==="
python3 ./mach build -j4 2>&1 | tail -60
echo "BUILD_EXIT=$?"
echo "=== js shell binary ==="
ls -la /work/obj-js/dist/bin/js 2>/dev/null && mkdir -p /out && cp /work/obj-js/dist/bin/js /out/js-shell 2>/dev/null && echo JS_COPIED
i686-linux-musl-readelf -h /work/obj-js/dist/bin/js 2>/dev/null | grep -iE "Class|Machine|Type" || true
