#!/bin/sh
# run-maeros.sh — launch MaeroOS in QEMU at a resolution that fits this Mac's
# screen.  The kernel boots "no preference" and adopts whatever the emulated
# display advertises via EDID, so we just tell QEMU the mode to advertise.
#
#   tools/run-maeros.sh             # auto-pick the largest fitting resolution
#   tools/run-maeros.sh 1280x720    # force a specific resolution
#
# Env: ISO (default maeros.iso), DISK (default disk.img), MEM (default 512M),
#      SERVE_REPO=1 to also serve the app repo on :8000.

set -e
cd "$(dirname "$0")/.."

ISO="${ISO:-maeros.iso}"
DISK="${DISK:-disk.img}"
MEM="${MEM:-512M}"
# SNAPSHOT=1 routes guest disk writes to a throwaway overlay so the OS can't
# corrupt the real image (the ext2 write path has at times damaged the FS).
# Recommended for testing; off by default so normal use can persist changes.
SNAPSHOT="${SNAPSHOT:-0}"
DISK_OPTS="format=raw,if=ide"
[ "$SNAPSHOT" = "1" ] && DISK_OPTS="format=raw,if=ide,snapshot=on"

# All resolutions MaeroOS supports (the desktop adapts to any of them).
STD="1920x1080 1600x900 1440x900 1280x800 1280x720 1024x768 800x600"

# Usable logical screen in points (Finder desktop bounds = x1,y1,x2,y2),
# minus the macOS menu bar + QEMU window title bar + breathing room.
BOUNDS=$(osascript -e 'tell application "Finder" to get bounds of window of desktop' 2>/dev/null || true)
SW=$(printf '%s' "$BOUNDS" | awk -F', ' '{print $3}')
SH=$(printf '%s' "$BOUNDS" | awk -F', ' '{print $4}')
[ -z "$SW" ] && SW=1280
[ -z "$SH" ] && SH=800
MAXW=$((SW - 20))
MAXH=$((SH - 80))

if [ "$1" = "--list" ]; then
    echo "Host screen     : ${SW}x${SH} points (usable ${MAXW}x${MAXH})"
    echo "Supported modes : $STD"
    for r in $STD; do
        w=${r%x*}; h=${r#*x}
        if [ "$w" -le "$MAXW" ] && [ "$h" -le "$MAXH" ]; then
            echo "Auto-pick (largest that fits): $r"; break
        fi
    done
    exit 0
fi

if [ -n "$1" ]; then
    RES="$1"
else
    RES=""
    for r in $STD; do
        w=${r%x*}; h=${r#*x}
        if [ "$w" -le "$MAXW" ] && [ "$h" -le "$MAXH" ]; then RES="$r"; break; fi
    done
    [ -z "$RES" ] && RES="800x600"
fi
W=${RES%x*}; H=${RES#*x}

echo "Host screen     : ${SW}x${SH} points (usable ${MAXW}x${MAXH})"
echo "Supported modes : $STD"
echo "Launching MaeroOS at ${W}x${H}  (override: make start RES=WxH)"
echo

REPO_PID=""
if [ "${SERVE_REPO:-0}" = "1" ]; then
    lsof -ti tcp:8000 2>/dev/null | xargs kill 2>/dev/null || true
    ( cd repo && python3 -m http.server 8000 >/dev/null 2>&1 ) &
    REPO_PID=$!
    echo "App repo serving on :8000 (guest sees it at 10.0.2.2:8000)"
fi

cleanup() {
    [ -n "$REPO_PID" ] && kill "$REPO_PID" 2>/dev/null || true
    lsof -ti tcp:8000 2>/dev/null | xargs kill 2>/dev/null || true
}
trap cleanup EXIT INT TERM

qemu-system-i386 \
    -cdrom "$ISO" \
    -drive file="$DISK",$DISK_OPTS \
    -m "$MEM" \
    -device VGA,edid=on,xres="$W",yres="$H" \
    -display cocoa \
    -serial file:/tmp/maeros_serial.log \
    -netdev user,id=n0 -device rtl8139,netdev=n0 \
    -audiodev coreaudio,id=snd0 -device AC97,audiodev=snd0 \
    -no-reboot
