#!/bin/sh
# run-maeros.sh — launch MaeroOS in QEMU at a resolution that fits the host
# screen.  The kernel boots "no preference" and adopts whatever the emulated
# display advertises via EDID, so we just tell QEMU the mode to advertise.
#
#   tools/run-maeros.sh             # auto-pick the largest fitting resolution
#   tools/run-maeros.sh 1280x720    # force a specific resolution
#   tools/run-maeros.sh --list      # show the host screen size and the auto-pick
#   tools/run-maeros.sh --free-port 8000   # kill whatever listens on TCP 8000
#
# Env: ISO (default maeros.iso), DISK (default disk.img), MEM (default 512M),
#      SERVE_REPO=1 to also serve the app repo on :8000,
#      QEMU_DISPLAY / QEMU_AUDIO to override the auto-detected QEMU backends.
#
# Works on macOS (Finder bounds, cocoa, coreaudio) and Linux (xrandr/xdpyinfo,
# gtk or sdl, pipewire/pa/alsa).

set -e
cd "$(dirname "$0")/.."

ISO="${ISO:-maeros.iso}"
DISK="${DISK:-disk.img}"
MEM="${MEM:-512M}"
QEMU="${QEMU:-qemu-system-i386}"
# SNAPSHOT=1 routes guest disk writes to a throwaway overlay so the OS can't
# corrupt the real image (the ext2 write path has at times damaged the FS).
# Recommended for testing; off by default so normal use can persist changes.
SNAPSHOT="${SNAPSHOT:-0}"
DISK_OPTS="format=raw,if=ide"
[ "$SNAPSHOT" = "1" ] && DISK_OPTS="format=raw,if=ide,snapshot=on"

# All resolutions MaeroOS supports (the desktop adapts to any of them).
STD="1920x1080 1600x900 1440x900 1280x800 1280x720 1024x768 800x600"

HOST_OS=$(uname -s 2>/dev/null || echo unknown)

# ── Port helper: kill whatever is listening on TCP $1.  lsof is not always
# installed on Linux, so fall back to ss and then fuser. ─────────────────────
free_port() {
    port="$1"
    if command -v lsof >/dev/null 2>&1; then
        lsof -ti "tcp:$port" 2>/dev/null | xargs kill 2>/dev/null || true
    elif command -v ss >/dev/null 2>&1; then
        ss -ltnpH "sport = :$port" 2>/dev/null \
            | sed -n 's/.*pid=\([0-9]*\).*/\1/p' | sort -u \
            | xargs kill 2>/dev/null || true
    elif command -v fuser >/dev/null 2>&1; then
        fuser -k "$port/tcp" >/dev/null 2>&1 || true
    fi
}

if [ "$1" = "--free-port" ]; then
    [ -n "$2" ] || { echo "usage: $0 --free-port PORT" >&2; exit 2; }
    free_port "$2"
    exit 0
fi

# ── Host screen size ──────────────────────────────────────────────────────────
SW=""; SH=""
case "$HOST_OS" in
Darwin)
    # Usable logical screen in points (Finder desktop bounds = x1,y1,x2,y2).
    BOUNDS=$(osascript -e 'tell application "Finder" to get bounds of window of desktop' 2>/dev/null || true)
    SW=$(printf '%s' "$BOUNDS" | awk -F', ' '{print $3}')
    SH=$(printf '%s' "$BOUNDS" | awk -F', ' '{print $4}')
    UNITS="points"
    ;;
*)
    UNITS="pixels"
    if [ -n "${DISPLAY:-}" ] && command -v xrandr >/dev/null 2>&1; then
        # Prefer the primary monitor's mode ("DP-1 connected primary 1920x1080+0+0"),
        # then any connected output, then the whole virtual screen
        # ("Screen 0: ..., current 2560 x 1440, ...") which spans all monitors.
        XR=$(xrandr --current 2>/dev/null || true)
        DIM=$(printf '%s\n' "$XR" | awk '/ connected primary [0-9]+x[0-9]+\+/ { for (i = 1; i <= NF; i++) if ($i ~ /^[0-9]+x[0-9]+\+/) { sub(/\+.*/, "", $i); print $i; exit } }')
        [ -z "$DIM" ] && DIM=$(printf '%s\n' "$XR" | awk '/ connected [0-9]+x[0-9]+\+/ { for (i = 1; i <= NF; i++) if ($i ~ /^[0-9]+x[0-9]+\+/) { sub(/\+.*/, "", $i); print $i; exit } }')
        [ -z "$DIM" ] && DIM=$(printf '%s\n' "$XR" | sed -n 's/.*current \([0-9][0-9]*\) x \([0-9][0-9]*\).*/\1x\2/p' | head -n 1)
        SW=${DIM%x*}; SH=${DIM#*x}
    fi
    if [ -z "$SW" ] && [ -n "${DISPLAY:-}" ] && command -v xdpyinfo >/dev/null 2>&1; then
        # "  dimensions:    2560x1440 pixels (677x381 millimeters)"
        DIM=$(xdpyinfo 2>/dev/null | sed -n 's/.*dimensions: *\([0-9]*x[0-9]*\) pixels.*/\1/p' | head -n 1)
        SW=${DIM%x*}; SH=${DIM#*x}
    fi
    ;;
esac
case "$SW" in ''|*[!0-9]*) SW=1280 ;; esac
case "$SH" in ''|*[!0-9]*) SH=800 ;; esac
# minus the menu bar / panel + QEMU window title bar + breathing room.
MAXW=$((SW - 20))
MAXH=$((SH - 80))

pick_res() {
    for r in $STD; do
        w=${r%x*}; h=${r#*x}
        if [ "$w" -le "$MAXW" ] && [ "$h" -le "$MAXH" ]; then echo "$r"; return; fi
    done
    echo "800x600"
}

if [ "$1" = "--list" ]; then
    echo "Host screen     : ${SW}x${SH} $UNITS (usable ${MAXW}x${MAXH})"
    echo "Supported modes : $STD"
    echo "Auto-pick (largest that fits): $(pick_res)"
    exit 0
fi

if [ -n "$1" ]; then
    RES="$1"
else
    RES=$(pick_res)
fi
W=${RES%x*}; H=${RES#*x}

# ── QEMU display + audio backends ─────────────────────────────────────────────
# Pick the first backend QEMU was built with; `-display help` / `-audiodev help`
# list them one per line.
qemu_has() { "$QEMU" "$1" help 2>/dev/null | grep -qx "$2"; }
pick_backend() {
    opt="$1"; shift
    for b in "$@"; do
        if qemu_has "$opt" "$b"; then echo "$b"; return; fi
    done
    echo "none"
}
case "$HOST_OS" in
Darwin) DISPLAY_BACKEND="${QEMU_DISPLAY:-cocoa}"; AUDIO_BACKEND="${QEMU_AUDIO:-coreaudio}" ;;
*)      DISPLAY_BACKEND="${QEMU_DISPLAY:-$(pick_backend -display gtk sdl)}"
        AUDIO_BACKEND="${QEMU_AUDIO:-$(pick_backend -audiodev pipewire pa alsa sdl)}" ;;
esac
if [ "$DISPLAY_BACKEND" = "none" ]; then
    echo "run-maeros: $QEMU has no gtk/sdl display backend; install qemu-system-gui" >&2
    exit 1
fi

echo "Host screen     : ${SW}x${SH} $UNITS (usable ${MAXW}x${MAXH})"
echo "Supported modes : $STD"
echo "QEMU backends   : display=$DISPLAY_BACKEND audio=$AUDIO_BACKEND"
echo "Launching MaeroOS at ${W}x${H}  (override: make start RES=WxH)"
echo

REPO_PID=""
if [ "${SERVE_REPO:-0}" = "1" ]; then
    free_port 8000
    ( cd repo && python3 -m http.server 8000 >/dev/null 2>&1 ) &
    REPO_PID=$!
    echo "App repo serving on :8000 (guest sees it at 10.0.2.2:8000)"
fi

cleanup() {
    [ -n "$REPO_PID" ] && kill "$REPO_PID" 2>/dev/null || true
    free_port 8000
}
trap cleanup EXIT INT TERM

"$QEMU" \
    -cdrom "$ISO" \
    -drive file="$DISK",$DISK_OPTS \
    -m "$MEM" \
    -device VGA,edid=on,xres="$W",yres="$H" \
    -display "$DISPLAY_BACKEND" \
    -serial file:/tmp/maeros_serial.log \
    -netdev user,id=n0 -device rtl8139,netdev=n0 \
    -audiodev "$AUDIO_BACKEND",id=snd0 -device AC97,audiodev=snd0 \
    -no-reboot
