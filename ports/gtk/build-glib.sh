#!/bin/sh
# Cross-build GLib (+ zlib, libffi, pcre2) for i686-linux-musl, STATIC, into
# /sysroot.  Idempotent (skip-guards).  Run inside maeros-gtkbuild with internet.
set -e
export TARGET=i686-linux-musl
export PREFIX=/sysroot
export PATH="/opt/i686-linux-musl-cross/bin:$PATH"
export CC=$TARGET-gcc
export CXX=$TARGET-g++
export AR=$TARGET-ar
export RANLIB=$TARGET-ranlib
export PKG_CONFIG_PATH=$PREFIX/lib/pkgconfig:$PREFIX/share/pkgconfig
export PKG_CONFIG_LIBDIR=$PREFIX/lib/pkgconfig:$PREFIX/share/pkgconfig
export PKG_CONFIG_SYSROOT_DIR=
CROSS=/build/cross-musl.txt

cd /work
gh(){ [ -f "$3" ] || wget -q "$2" -O "$3"; [ -d "$1" ] || tar xf "$3"; }

echo "=== zlib ==="
gh zlib-1.3.1 "https://zlib.net/fossils/zlib-1.3.1.tar.gz" zlib-1.3.1.tar.gz
if [ ! -f $PREFIX/lib/libz.a ]; then
  ( cd zlib-1.3.1 && CHOST=$TARGET ./configure --prefix=$PREFIX --static >/dev/null 2>&1 && make -j2 >/tmp/z.log 2>&1 && make install >/dev/null 2>&1 ) || { echo "FAIL zlib"; tail -20 /tmp/z.log; exit 1; }
  echo "ok zlib"
else echo "skip zlib"; fi

echo "=== libffi ==="
gh libffi-3.4.6 "https://github.com/libffi/libffi/releases/download/v3.4.6/libffi-3.4.6.tar.gz" libffi-3.4.6.tar.gz
if [ ! -f $PREFIX/lib/libffi.a ]; then
  ( cd libffi-3.4.6 && ./configure --host=$TARGET --prefix=$PREFIX --disable-shared --enable-static >/tmp/ffi.log 2>&1 && make -j2 >>/tmp/ffi.log 2>&1 && make install >>/tmp/ffi.log 2>&1 ) || { echo "FAIL libffi"; tail -20 /tmp/ffi.log; exit 1; }
  echo "ok libffi"
else echo "skip libffi"; fi

echo "=== pcre2 ==="
gh pcre2-10.43 "https://github.com/PCRE2Project/pcre2/releases/download/pcre2-10.43/pcre2-10.43.tar.gz" pcre2-10.43.tar.gz
if [ ! -f $PREFIX/lib/libpcre2-8.a ]; then
  ( cd pcre2-10.43 && ./configure --host=$TARGET --prefix=$PREFIX --disable-shared --enable-static >/tmp/pcre.log 2>&1 && make -j2 >>/tmp/pcre.log 2>&1 && make install >>/tmp/pcre.log 2>&1 ) || { echo "FAIL pcre2"; tail -20 /tmp/pcre.log; exit 1; }
  echo "ok pcre2"
else echo "skip pcre2"; fi

echo "=== GLib (meson) ==="
gh glib-2.78.4 "https://download.gnome.org/sources/glib/2.78/glib-2.78.4.tar.xz" glib-2.78.4.tar.xz
if [ ! -f $PREFIX/lib/libglib-2.0.a ]; then
  ( cd glib-2.78.4 && rm -rf _b && meson setup _b --cross-file $CROSS --prefix=$PREFIX \
      --default-library=static -Dtests=false -Dnls=disabled -Dlibmount=disabled \
      -Dselinux=disabled -Dglib_debug=disabled \
      -Dlibelf=disabled -Dbsymbolic_functions=false >/tmp/glib.log 2>&1 \
    && ninja -C _b >>/tmp/glib.log 2>&1 && ninja -C _b install >>/tmp/glib.log 2>&1 ) \
    || { echo "FAIL glib"; tail -35 /tmp/glib.log; exit 1; }
  echo "ok glib"
else echo "skip glib"; fi

echo "GLIB_STACK_OK"
ls -la $PREFIX/lib/libglib-2.0.a $PREFIX/lib/libffi.a $PREFIX/lib/libpcre2-8.a 2>/dev/null
