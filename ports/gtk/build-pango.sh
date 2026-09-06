#!/bin/sh
# Cross-build Pango (+fribidi) and GdkPixbuf for i686-linux-musl, into /sysroot.
set -e
if [ ! -f /usr/bin/pkg-config.real ]; then
  cp /usr/bin/pkg-config /usr/bin/pkg-config.real
  printf '#!/bin/sh\nexport PKG_CONFIG_LIBDIR=/sysroot/lib/pkgconfig:/sysroot/share/pkgconfig\nexport PKG_CONFIG_PATH=\nexport PKG_CONFIG_SYSROOT_DIR=\nexec /usr/bin/pkg-config.real "$@"\n' > /usr/bin/pkg-config
  chmod +x /usr/bin/pkg-config
fi
export TARGET=i686-linux-musl PREFIX=/sysroot
export PATH="/opt/i686-linux-musl-cross/bin:$PATH"
export CC=$TARGET-gcc CXX=$TARGET-g++ AR=$TARGET-ar RANLIB=$TARGET-ranlib
export PKG_CONFIG_LIBDIR=$PREFIX/lib/pkgconfig:$PREFIX/share/pkgconfig
export PKG_CONFIG_PATH=$PKG_CONFIG_LIBDIR PKG_CONFIG_SYSROOT_DIR=
export CPPFLAGS="-I$PREFIX/include" LDFLAGS="-L$PREFIX/lib"
CROSS=/build/cross-musl.txt
cd /work
gh(){ [ -f "$3" ] || wget -q "$2" -O "$3"; [ -d "$1" ] || tar xf "$3"; }
mesonbuild(){
  m="$1"; d="$2"; shift 2
  if [ -e "$m" ]; then echo "skip $d"; return; fi
  echo "=== $d (meson) ==="
  ( cd "$d" && rm -rf _b && meson setup _b --cross-file $CROSS --prefix=$PREFIX "$@" >/tmp/$d.log 2>&1 \
    && ninja -C _b >>/tmp/$d.log 2>&1 && ninja -C _b install >>/tmp/$d.log 2>&1 ) || { echo "FAIL $d"; tail -30 /tmp/$d.log; exit 1; }
  echo "ok $d"
}

gh fribidi-1.0.13 "https://github.com/fribidi/fribidi/releases/download/v1.0.13/fribidi-1.0.13.tar.xz" fribidi-1.0.13.tar.xz
mesonbuild $PREFIX/lib/libfribidi.a fribidi-1.0.13 -Dtests=false -Ddocs=false -Dbin=false

gh pango-1.51.0 "https://download.gnome.org/sources/pango/1.51/pango-1.51.0.tar.xz" pango-1.51.0.tar.xz
mesonbuild $PREFIX/lib/libpango-1.0.a pango-1.51.0 -Dintrospection=disabled

gh gdk-pixbuf-2.42.10 "https://download.gnome.org/sources/gdk-pixbuf/2.42/gdk-pixbuf-2.42.10.tar.xz" gdk-pixbuf-2.42.10.tar.xz
mesonbuild $PREFIX/lib/libgdk_pixbuf-2.0.a gdk-pixbuf-2.42.10 -Dintrospection=disabled -Dman=false -Dtests=false -Dinstalled_tests=false -Dgtk_doc=false -Dpng=enabled -Djpeg=disabled -Dtiff=disabled -Dgio_sniffing=false -Dbuiltin_loaders=all

echo "PANGO_STACK_OK"
ls -la $PREFIX/lib/libpango-1.0.a $PREFIX/lib/libfribidi.a $PREFIX/lib/libgdk_pixbuf-2.0.a 2>/dev/null
