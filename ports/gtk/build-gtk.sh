#!/bin/sh
# Cross-build GTK3 (+libepoxy + remaining X libs) for i686-linux-musl into
# /sysroot. Idempotent. Run inside maeros-gtkbuild. Depends on glib/cairo/pango.
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
export xorg_cv_malloc0_returns_null=yes
export GDK_PIXBUF_PIXDATA=/usr/bin/gdk-pixbuf-pixdata
CROSS=/build/cross-musl.txt
cd /work
gh(){ [ -f "$3" ] || wget -q "$2" -O "$3"; [ -d "$1" ] || tar xf "$3"; }
acbuild(){ m="$1"; d="$2"; shift 2
  if [ -e "$m" ]; then echo "skip $d"; return; fi
  echo "=== $d ==="
  ( cd "$d" && ./configure --host=$TARGET --prefix=$PREFIX --disable-shared --enable-static "$@" >/tmp/$d.log 2>&1 \
    && make -j2 >>/tmp/$d.log 2>&1 && make install >>/tmp/$d.log 2>&1 ) || { echo "FAIL $d"; tail -25 /tmp/$d.log; exit 1; }
  echo "ok $d"; }
mb(){ m="$1"; d="$2"; shift 2
  if [ -e "$m" ]; then echo "skip $d"; return; fi
  echo "=== $d (meson) ==="
  ( cd "$d" && rm -rf _b && meson setup _b --cross-file $CROSS --prefix=$PREFIX "$@" >/tmp/$d.log 2>&1 \
    && ninja -C _b >>/tmp/$d.log 2>&1 && ninja -C _b install >>/tmp/$d.log 2>&1 ) || { echo "FAIL $d"; tail -30 /tmp/$d.log; exit 1; }
  echo "ok $d"; }

XL=https://www.x.org/releases/individual/lib
gh libXfixes-6.0.1   "$XL/libXfixes-6.0.1.tar.xz"   libXfixes-6.0.1.tar.xz   ; acbuild $PREFIX/lib/libXfixes.a   libXfixes-6.0.1
gh libXi-1.8.1       "$XL/libXi-1.8.1.tar.xz"       libXi-1.8.1.tar.xz       ; acbuild $PREFIX/lib/libXi.a       libXi-1.8.1
gh libXrandr-1.5.4   "$XL/libXrandr-1.5.4.tar.xz"   libXrandr-1.5.4.tar.xz   ; acbuild $PREFIX/lib/libXrandr.a   libXrandr-1.5.4
gh libXcursor-1.2.2  "$XL/libXcursor-1.2.2.tar.xz"  libXcursor-1.2.2.tar.xz  ; acbuild $PREFIX/lib/libXcursor.a  libXcursor-1.2.2
gh libXcomposite-0.4.6 "$XL/libXcomposite-0.4.6.tar.xz" libXcomposite-0.4.6.tar.xz ; acbuild $PREFIX/lib/libXcomposite.a libXcomposite-0.4.6
gh libXdamage-1.1.6  "$XL/libXdamage-1.1.6.tar.xz"  libXdamage-1.1.6.tar.xz  ; acbuild $PREFIX/lib/libXdamage.a  libXdamage-1.1.6
gh libXinerama-1.1.5 "$XL/libXinerama-1.1.5.tar.xz" libXinerama-1.1.5.tar.xz ; acbuild $PREFIX/lib/libXinerama.a libXinerama-1.1.5
gh libXtst-1.2.4     "$XL/libXtst-1.2.4.tar.xz"     libXtst-1.2.4.tar.xz     ; acbuild $PREFIX/lib/libXtst.a     libXtst-1.2.4

gh libepoxy-1.5.10 "https://download.gnome.org/sources/libepoxy/1.5/libepoxy-1.5.10.tar.xz" libepoxy-1.5.10.tar.xz
mb $PREFIX/lib/libepoxy.a libepoxy-1.5.10 -Dtests=false -Dglx=yes -Degl=no -Dx11=true

gh atk-2.38.0 "https://download.gnome.org/sources/atk/2.38/atk-2.38.0.tar.xz" atk-2.38.0.tar.xz
mb $PREFIX/lib/libatk-1.0.a atk-2.38.0 -Dintrospection=false

# atk-bridge stub: GTK only calls atk_bridge_adaptor_init(); real AT-SPI needs
# dbus+at-spi2 which we do not need for rendering.  Provide a no-op so GTK links.
if [ ! -f $PREFIX/lib/libatk-bridge-2.0.a ]; then
  echo "=== atk-bridge stub ==="
  mkdir -p $PREFIX/include/at-spi2-atk/2.0
  cat > /tmp/atk-bridge.h <<'H'
#ifndef ATK_BRIDGE_H_
#define ATK_BRIDGE_H_
#include <glib.h>
G_BEGIN_DECLS
int  atk_bridge_adaptor_init (gint *argc, gchar ***argv);
void atk_bridge_adaptor_cleanup (void);
G_END_DECLS
#endif
H
  cp /tmp/atk-bridge.h $PREFIX/include/at-spi2-atk/2.0/atk-bridge.h
  cat > /tmp/atkbridgestub.c <<'C'
int  atk_bridge_adaptor_init (int *argc, char ***argv){ (void)argc;(void)argv; return 0; }
void atk_bridge_adaptor_cleanup (void){}
C
  $CC -O2 -c /tmp/atkbridgestub.c -o /tmp/atkbridgestub.o
  $AR rcs $PREFIX/lib/libatk-bridge-2.0.a /tmp/atkbridgestub.o
  cat > $PREFIX/lib/pkgconfig/atk-bridge-2.0.pc <<P
prefix=$PREFIX
libdir=\${prefix}/lib
includedir=\${prefix}/include
Name: atk-bridge-2.0
Description: AT-SPI atk bridge (stub)
Version: 2.38.0
Requires: atk glib-2.0
Libs: -L\${libdir} -latk-bridge-2.0
Cflags: -I\${includedir}/at-spi2-atk/2.0
P
  echo "ok atk-bridge stub"
fi

gh gtk+-3.24.41 "https://download.gnome.org/sources/gtk+/3.24/gtk+-3.24.41.tar.xz" gtk+-3.24.41.tar.xz
mb $PREFIX/lib/libgtk-3.a gtk+-3.24.41 \
  -Dx11_backend=true -Dwayland_backend=false -Dbroadway_backend=false \
  -Dintrospection=false -Ddemos=false -Dexamples=false -Dtests=false \
  -Dman=false -Dgtk_doc=false -Dcolord=no -Dcloudproviders=false

echo "GTK_STACK_OK"
ls -la $PREFIX/lib/libgtk-3.a $PREFIX/lib/libgdk-3.a $PREFIX/lib/libepoxy.a 2>/dev/null
