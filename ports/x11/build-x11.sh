#!/bin/sh
# Cross-build the X11 client stack for i686-linux-musl, STATIC, into /sysroot.
# Idempotent: each stage is skipped if its output already exists, so the build
# resumes after a timeout.  Run inside maeros-cross with internet.
set -e

export TARGET=i686-linux-musl
export PREFIX=/sysroot
export PATH="/opt/i686-linux-musl-cross/bin:$PATH"
export CC=$TARGET-gcc
export PKG_CONFIG_PATH=$PREFIX/lib/pkgconfig:$PREFIX/share/pkgconfig
export PKG_CONFIG_SYSROOT_DIR=
export PKG_CONFIG_LIBDIR=$PREFIX/lib/pkgconfig:$PREFIX/share/pkgconfig
export ACLOCAL_PATH=$PREFIX/share/aclocal
mkdir -p $PREFIX/share/aclocal

if ! command -v python3 >/dev/null; then
    echo "=== install host build deps ==="
    apt-get update >/dev/null 2>&1
    apt-get install -y --no-install-recommends \
        wget xz-utils python3 pkg-config autoconf automake libtool m4 \
        bison gperf gcc g++ >/dev/null 2>&1
    echo "deps installed"
fi

cd /work
DL="https://www.x.org/releases/individual"
fetch(){ [ -f "$2.tar.xz" ] || wget -q "$DL/$1/$2.tar.xz"; [ -d "$2" ] || tar xf "$2.tar.xz"; }

stage(){   # stage <marker> <dir> <configure-args...>
    marker="$1"; dir="$2"; shift 2
    if [ -e "$marker" ]; then echo "skip $dir (done)"; return; fi
    echo "=== build $dir ==="
    ( cd "$dir" && ./configure "$@" >/dev/null 2>/tmp/cfg.log && make -j2 >/tmp/make.log 2>&1 && make install >/dev/null 2>&1 ) \
        || { echo "FAILED $dir"; tail -25 /tmp/cfg.log /tmp/make.log 2>/dev/null; exit 1; }
    echo "ok $dir"
}

fetch util util-macros-1.20.0
stage $PREFIX/share/aclocal/xorg-macros.m4 util-macros-1.20.0 --prefix=$PREFIX

fetch proto xorgproto-2023.2
stage $PREFIX/include/X11/Xproto.h xorgproto-2023.2 --host=$TARGET --prefix=$PREFIX

fetch lib libXau-1.0.11
stage $PREFIX/lib/libXau.a libXau-1.0.11 --host=$TARGET --prefix=$PREFIX --disable-shared --enable-static

fetch lib libXdmcp-1.1.4
stage $PREFIX/lib/libXdmcp.a libXdmcp-1.1.4 --host=$TARGET --prefix=$PREFIX --disable-shared --enable-static

fetch xcb libpthread-stubs-0.5
stage $PREFIX/lib/pkgconfig/pthread-stubs.pc libpthread-stubs-0.5 --host=$TARGET --prefix=$PREFIX

fetch xcb xcb-proto-1.17.0
stage $PREFIX/share/pkgconfig/xcb-proto.pc xcb-proto-1.17.0 --prefix=$PREFIX PYTHON=python3

export PYTHONPATH=$(find $PREFIX -name xcbgen -type d 2>/dev/null | head -1 | xargs dirname 2>/dev/null)
echo "PYTHONPATH=$PYTHONPATH"

fetch xcb libxcb-1.17.0
stage $PREFIX/lib/libxcb.a libxcb-1.17.0 --host=$TARGET --prefix=$PREFIX --disable-shared --enable-static --without-doc PYTHON=python3

fetch lib xtrans-1.5.0
stage $PREFIX/include/X11/Xtrans/Xtrans.h xtrans-1.5.0 --host=$TARGET --prefix=$PREFIX

fetch lib libX11-1.8.7
stage $PREFIX/lib/libX11.a libX11-1.8.7 --host=$TARGET --prefix=$PREFIX --disable-shared --enable-static --disable-xcms --disable-xkb --without-xmlto --without-fop --disable-specs --disable-malloc0returnsnull PYTHON=python3

echo "STAGE_ALL_OK"
ls -la $PREFIX/lib/libxcb.a $PREFIX/lib/libX11.a 2>/dev/null
