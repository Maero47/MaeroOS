#!/bin/sh
# Cross-build the font + Cairo stack for i686-linux-musl, STATIC, into /sysroot.
# Idempotent. Run inside maeros-gtkbuild. Depends on GLib stack already built.
set -e
# Replace system pkg-config with an env-forcing wrapper so meson cross builds
# (which ignore PKG_CONFIG_* env and the cross-file pkg-config) look in /sysroot.
if [ ! -f /usr/bin/pkg-config.real ]; then
  cp /usr/bin/pkg-config /usr/bin/pkg-config.real
  printf '#!/bin/sh\nexport PKG_CONFIG_LIBDIR=/sysroot/lib/pkgconfig:/sysroot/share/pkgconfig\nexport PKG_CONFIG_PATH=\nexport PKG_CONFIG_SYSROOT_DIR=\nexec /usr/bin/pkg-config.real "$@"\n' > /usr/bin/pkg-config
  chmod +x /usr/bin/pkg-config
fi
export TARGET=i686-linux-musl PREFIX=/sysroot
export PATH="/opt/i686-linux-musl-cross/bin:$PATH"
export CC=$TARGET-gcc CXX=$TARGET-g++ AR=$TARGET-ar RANLIB=$TARGET-ranlib
export PKG_CONFIG_PATH=$PREFIX/lib/pkgconfig:$PREFIX/share/pkgconfig
export PKG_CONFIG_LIBDIR=$PREFIX/lib/pkgconfig:$PREFIX/share/pkgconfig
export PKG_CONFIG_SYSROOT_DIR=
export CPPFLAGS="-I$PREFIX/include" LDFLAGS="-L$PREFIX/lib"
export xorg_cv_malloc0_returns_null=yes
CROSS=/build/cross-musl.txt
cd /work
gh(){ [ -f "$3" ] || wget -q "$2" -O "$3"; [ -d "$1" ] || tar xf "$3"; }
acbuild(){  # acbuild <marker> <dir> <extra-configure-args...>
  m="$1"; d="$2"; shift 2
  if [ -e "$m" ]; then echo "skip $d"; return; fi
  echo "=== $d ==="
  ( cd "$d" && ./configure --host=$TARGET --prefix=$PREFIX --disable-shared --enable-static "$@" >/tmp/$d.log 2>&1 \
    && make -j2 >>/tmp/$d.log 2>&1 && make install >>/tmp/$d.log 2>&1 ) || { echo "FAIL $d"; tail -30 /tmp/$d.log; exit 1; }
  echo "ok $d"
}
mesonbuild(){  # mesonbuild <marker> <dir> <-Dopts...>
  m="$1"; d="$2"; shift 2
  if [ -e "$m" ]; then echo "skip $d"; return; fi
  echo "=== $d (meson) ==="
  ( cd "$d" && rm -rf _b && meson setup _b --cross-file $CROSS --prefix=$PREFIX --default-library=static "$@" >/tmp/$d.log 2>&1 \
    && ninja -C _b >>/tmp/$d.log 2>&1 && ninja -C _b install >>/tmp/$d.log 2>&1 ) || { echo "FAIL $d"; tail -35 /tmp/$d.log; exit 1; }
  echo "ok $d"
}

gh expat-2.6.2 "https://github.com/libexpat/libexpat/releases/download/R_2_6_2/expat-2.6.2.tar.xz" expat-2.6.2.tar.xz
acbuild $PREFIX/lib/libexpat.a expat-2.6.2 --without-docbook

gh libpng-1.6.43 "https://download.sourceforge.net/libpng/libpng-1.6.43.tar.xz" libpng-1.6.43.tar.xz
acbuild $PREFIX/lib/libpng16.a libpng-1.6.43

gh freetype-2.13.2 "https://download.savannah.gnu.org/releases/freetype/freetype-2.13.2.tar.xz" freetype-2.13.2.tar.xz
acbuild $PREFIX/lib/libfreetype.a freetype-2.13.2 --without-harfbuzz --with-brotli=no --with-bzip2=no --with-png=yes

gh fontconfig-2.15.0 "https://www.freedesktop.org/software/fontconfig/release/fontconfig-2.15.0.tar.xz" fontconfig-2.15.0.tar.xz
acbuild $PREFIX/lib/libfontconfig.a fontconfig-2.15.0 --disable-docs --enable-libxml2=no

gh pixman-0.43.4 "https://www.cairographics.org/releases/pixman-0.43.4.tar.gz" pixman-0.43.4.tar.gz
mesonbuild $PREFIX/lib/libpixman-1.a pixman-0.43.4 -Dtests=disabled -Ddemos=disabled -Dgtk=disabled

gh harfbuzz-8.5.0 "https://github.com/harfbuzz/harfbuzz/releases/download/8.5.0/harfbuzz-8.5.0.tar.xz" harfbuzz-8.5.0.tar.xz
mesonbuild $PREFIX/lib/libharfbuzz.a harfbuzz-8.5.0 -Dtests=disabled -Ddocs=disabled -Dutilities=disabled -Dglib=enabled -Dfreetype=enabled -Dcairo=disabled -Dicu=disabled

gh libXext-1.3.6 "https://www.x.org/releases/individual/lib/libXext-1.3.6.tar.xz" libXext-1.3.6.tar.xz
acbuild $PREFIX/lib/libXext.a libXext-1.3.6

gh libXrender-0.9.11 "https://www.x.org/releases/individual/lib/libXrender-0.9.11.tar.xz" libXrender-0.9.11.tar.xz
acbuild $PREFIX/lib/libXrender.a libXrender-0.9.11

gh cairo-1.18.0 "https://cairographics.org/releases/cairo-1.18.0.tar.xz" cairo-1.18.0.tar.xz
# libXrender 0.9.11 defines gradient structs that cairo's static-link has_function
# probe can't confirm in cross builds → force HAVE_XRENDERCREATE* to avoid a
# redefinition clash with cairo-xlib-xrender-private.h.
sed -i "s/  if cc.has_function(name, dependencies: deps)/  if name.startswith('XRenderCreate') or cc.has_function(name, dependencies: deps)/" cairo-1.18.0/meson.build || true
mesonbuild $PREFIX/lib/libcairo.a cairo-1.18.0 -Dtests=disabled -Dxlib=enabled -Dxcb=disabled -Dpng=enabled -Dfreetype=enabled -Dfontconfig=enabled -Dglib=enabled -Dxlib-xcb=disabled -Dquartz=disabled -Dtee=disabled

echo "CAIRO_STACK_OK"
ls -la $PREFIX/lib/libcairo.a $PREFIX/lib/libfreetype.a $PREFIX/lib/libfontconfig.a $PREFIX/lib/libharfbuzz.a $PREFIX/lib/libpixman-1.a 2>/dev/null
