#!/bin/sh
export PKG_CONFIG_LIBDIR=/sysroot/lib/pkgconfig:/sysroot/share/pkgconfig
export PKG_CONFIG_PATH=
export PKG_CONFIG_SYSROOT_DIR=
exec pkg-config "$@"
