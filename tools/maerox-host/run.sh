#!/bin/sh
# maeroX on the build host, for protocol debugging (docs/alpinex.md).
#
#   tools/maerox-host/run.sh build          build build/maerox-host/maerox
#   tools/maerox-host/run.sh <app> [args]   run maeroX and <app> from ROOTFS
#
# ROOTFS (default build/hxroot) is an Alpine x86 root with the app installed,
# e.g. a copy of build/alpine/stage/alpine plus
#   unshare -r build/alpine/apk.static --root $ROOTFS --arch x86 --no-network \
#     --repositories-file /dev/null --repository $ROOTFS/repo/main \
#     --repository $ROOTFS/repo/community add galculator
# Both run in a private user, mount, network and pid namespace, so the
# abstract socket @/tmp/.X11-unix/X0 does not meet the host's own X server.
# maeroX logs to build/maerox-host/maerox.log (-x: request trace), the app to
# app.log; "echo 'd shot.ppm' > build/maerox-host/ctl" dumps the screen.
set -e
TOP=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$TOP/build/maerox-host
ROOTFS=${ROOTFS:-$TOP/build/hxroot}
mkdir -p "$OUT"
if [ "$1" = build ]; then
    cd "$TOP/userspace"
    for f in maerox/*.c libdraw/draw.c libdraw/fonts.c "$TOP/tools/maerox-host/hostgui.c"; do
        i686-linux-musl-gcc -O1 -g -w -idirafter include -c "$f" -o "$OUT/$(basename "$f" .c).o"
    done
    i686-linux-musl-gcc -static -o "$OUT/maerox" "$OUT"/*.o -lm
    exit 0
fi
if [ "$1" = inner ]; then
    shift
    mkdir -p /tmp/.X11-unix && mount -t tmpfs none /tmp/.X11-unix
    mount --rbind /dev "$ROOTFS/dev"; mount -t proc proc "$ROOTFS/proc"
    mount -t tmpfs none "$ROOTFS/tmp"
    mkdir -p "$ROOTFS/tmp/.X11-unix" && mount --bind /tmp/.X11-unix "$ROOTFS/tmp/.X11-unix"
    HX_CTL=$OUT/ctl "$OUT/maerox" -x -g 1000x700 -L "$OUT/maerox.log" 1 > "$OUT/maerox.out" 2>&1 &
    sleep 0.5
    chroot "$ROOTFS" /usr/bin/env -i PATH=/usr/bin:/bin HOME=/root DISPLAY=:0 LANG=C.UTF-8 \
        NO_AT_BRIDGE=1 GTK_A11Y=none DBUS_SESSION_BUS_ADDRESS=disabled: GDK_BACKEND=x11 \
        XDG_RUNTIME_DIR=/tmp TERM=xterm "$@" > "$OUT/app.log" 2>&1 &
    wait
    exit 0
fi
exec unshare -r -n -m -p -f --kill-child "$0" inner "$@"
