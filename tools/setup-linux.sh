#!/bin/sh
# setup-linux.sh — one-shot, idempotent bootstrap of a MaeroOS build host on
# Linux (Debian/Ubuntu).  After it finishes, `make`, `make initrd`, `make disk`,
# `make iso`, `make smoke*` and `make run-firefox` work like they do on macOS.
#
#   tools/setup-linux.sh            # print apt list, build/fetch toolchains, verify
#   tools/setup-linux.sh --apt      # also run `sudo apt-get install ...` first
#   tools/setup-linux.sh --check    # only verify which tools are present/missing
#   tools/setup-linux.sh --dry-run  # print the plan, download/build nothing
#
# What it does:
#   1. prints the exact apt package list (installs it with --apt, via sudo);
#   2. builds an i686-elf binutils + gcc (C only, --without-headers, + libgcc)
#      into $PREFIX (default $HOME/opt/cross) with all cores, skipping what is
#      already there;
#   3. downloads musl.cc's i686-linux-musl-cross.tgz into ports/ (where the
#      Docker recipes expect it) and extracts it under $OPT_DIR (default
#      $HOME/opt) so i686-linux-musl-gcc is usable natively as well;
#   4. prints the `export PATH=...` line you need and checks every tool.
#
# Options:
#   --apt         run `sudo apt-get update && sudo apt-get install -y <list>`
#   --optional    with --apt, also install the optional packages (docker.io,
#                 gcc-multilib, gdb-multiarch)
#   --check       verify tools and exit (nothing is built or downloaded)
#   --dry-run     print the plan and exit (nothing is built or downloaded)
#   --no-cross    skip the i686-elf toolchain step
#   --no-musl     skip the musl.cc toolchain step
#   --jobs N      parallel make jobs (default: nproc)
#   -h, --help    this text
#
# Environment overrides:
#   PREFIX        i686-elf install prefix        (default $HOME/opt/cross)
#   OPT_DIR       where the musl tree is unpacked (default $HOME/opt)
#   SRC_DIR       tarballs + build dirs          (default $PREFIX/src)
#   BINUTILS_VER  binutils release               (default 2.44)
#   GCC_VER       gcc release                    (default 14.2.0)
#   GNU_MIRROR    base URL for GNU tarballs      (default https://ftpmirror.gnu.org)
#   MUSL_URL      musl.cc tarball URL
#   JOBS          same as --jobs

set -eu

# ── Defaults ──────────────────────────────────────────────────────────────────
REPO_ROOT=$(cd "$(dirname "$0")/.." && pwd)
PREFIX="${PREFIX:-$HOME/opt/cross}"
OPT_DIR="${OPT_DIR:-$HOME/opt}"
SRC_DIR="${SRC_DIR:-$PREFIX/src}"
BINUTILS_VER="${BINUTILS_VER:-2.44}"
GCC_VER="${GCC_VER:-14.2.0}"
GNU_MIRROR="${GNU_MIRROR:-https://ftpmirror.gnu.org}"
MUSL_URL="${MUSL_URL:-https://musl.cc/i686-linux-musl-cross.tgz}"
MUSL_TGZ="$REPO_ROOT/ports/i686-linux-musl-cross.tgz"
MUSL_DIR="$OPT_DIR/i686-linux-musl-cross"
TARGET=i686-elf
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"

MODE=build          # build | check | dry-run
DO_APT=0
APT_OPTIONAL=0
DO_CROSS=1
DO_MUSL=1

# Required apt packages: GCC cross build deps, assembler, emulator, ISO and
# disk tooling, script runtimes.
APT_REQUIRED="build-essential bison flex texinfo libgmp-dev libmpfr-dev libmpc-dev libisl-dev
nasm qemu-system-x86 qemu-system-gui grub-pc-bin grub-common xorriso mtools e2fsprogs
python3 wget xz-utils zstd librsvg2-bin"
# Optional: docker.io only for rebuilding ports/, gcc-multilib for
# ports/firefox/build-glstubs.sh without Docker, gdb-multiarch for `make gdb`.
APT_OPTIONAL_PKGS="docker.io gcc-multilib gdb-multiarch"

usage() { sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'; }

while [ $# -gt 0 ]; do
    case "$1" in
    --apt)      DO_APT=1 ;;
    --optional) APT_OPTIONAL=1 ;;
    --check)    MODE=check ;;
    --dry-run)  MODE=dry-run ;;
    --no-cross) DO_CROSS=0 ;;
    --no-musl)  DO_MUSL=0 ;;
    --jobs)     shift; JOBS="${1:?--jobs needs a number}" ;;
    --jobs=*)   JOBS="${1#--jobs=}" ;;
    -h|--help)  usage; exit 0 ;;
    *) echo "setup-linux: unknown option '$1' (try --help)" >&2; exit 2 ;;
    esac
    shift
done

# ── Helpers ───────────────────────────────────────────────────────────────────
say()  { printf '%s\n' "$*"; }
step() { printf '\n== %s\n' "$*"; }
die()  { printf 'setup-linux: error: %s\n' "$*" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }

# The PATH the verification and the gcc build use: our prefixes first, and
# the sbin dirs because Debian keeps mke2fs/debugfs there.
BUILD_PATH="$PREFIX/bin:$MUSL_DIR/bin:$PATH:/usr/sbin:/sbin"

# fetch URL DEST — resumable download with wget (or curl), atomic on success.
fetch() {
    url="$1"; dest="$2"
    [ -f "$dest" ] && return 0
    mkdir -p "$(dirname "$dest")"
    say "   fetching $url"
    if have wget; then
        wget -q --show-progress -c -O "$dest.part" "$url" || { rm -f "$dest.part"; die "download failed: $url"; }
    elif have curl; then
        curl -fL -C - -o "$dest.part" "$url" || { rm -f "$dest.part"; die "download failed: $url"; }
    else
        die "need wget or curl to download $url"
    fi
    mv "$dest.part" "$dest"
}

# run_logged LOG CMD... — run a build command with output in LOG; on failure
# show the tail of the log and stop.
run_logged() {
    log="$1"; shift
    say "   \$ $* (log: $log)"
    if ! "$@" >"$log" 2>&1; then
        say "---- last 30 lines of $log ----" >&2
        tail -n 30 "$log" >&2
        die "command failed: $*"
    fi
}

cross_gcc_ok() {
    [ -x "$PREFIX/bin/$TARGET-gcc" ] || return 1
    libgcc=$("$PREFIX/bin/$TARGET-gcc" -print-libgcc-file-name 2>/dev/null) || return 1
    [ -f "$libgcc" ]
}
cross_binutils_ok() {
    [ -x "$PREFIX/bin/$TARGET-ld" ] && [ -x "$PREFIX/bin/$TARGET-as" ] && [ -x "$PREFIX/bin/$TARGET-strip" ]
}
musl_ok() { [ -x "$MUSL_DIR/bin/i686-linux-musl-gcc" ]; }

apt_lines() {
    say "Required packages (one command, needs sudo):"
    say ""
    say "    sudo apt-get update && sudo apt-get install -y $(printf '%s' "$APT_REQUIRED" | tr '\n' ' ')"
    say ""
    say "Optional packages:"
    say "    sudo apt-get install -y $APT_OPTIONAL_PKGS"
    say "      docker.io     only to rebuild anything under ports/"
    say "      gcc-multilib  ports/firefox/build-glstubs.sh without Docker (gcc -m32)"
    say "      gdb-multiarch for 'make gdb'"
}

# ── Verification ──────────────────────────────────────────────────────────────
# check_tool NAME REQUIRED HINT
MISSING_REQUIRED=0
check_tool() {
    name="$1"; required="$2"; hint="$3"
    if path=$(PATH="$BUILD_PATH" command -v "$name" 2>/dev/null); then
        printf '  [ ok ]   %-24s %s\n' "$name" "$path"
    elif [ "$required" = 1 ]; then
        printf '  [MISSING] %-23s %s\n' "$name" "$hint"
        MISSING_REQUIRED=1
    else
        printf '  [ -- ]   %-24s optional: %s\n' "$name" "$hint"
    fi
}

verify() {
    step "Tool check (PATH includes $PREFIX/bin and $MUSL_DIR/bin)"
    check_tool "$TARGET-gcc"     1 "built by this script into $PREFIX"
    check_tool "$TARGET-ar"      1 "built by this script into $PREFIX"
    check_tool "$TARGET-ld"      1 "built by this script into $PREFIX"
    check_tool "$TARGET-strip"   1 "built by this script into $PREFIX"
    check_tool cc                1 "apt: build-essential (toybox HOSTCC)"
    check_tool make              1 "apt: build-essential"
    check_tool nasm              1 "apt: nasm"
    check_tool qemu-system-i386  1 "apt: qemu-system-x86"
    check_tool grub-mkrescue     1 "apt: grub-common"
    check_tool xorriso           1 "apt: xorriso"
    check_tool mformat           1 "apt: mtools"
    check_tool mke2fs            1 "apt: e2fsprogs"
    check_tool debugfs           1 "apt: e2fsprogs"
    check_tool python3           1 "apt: python3"
    check_tool tar               1 "apt: tar"
    check_tool sed               1 "apt: sed"
    check_tool wget              1 "apt: wget"
    check_tool xz                1 "apt: xz-utils"
    check_tool zstd              1 "apt: zstd"
    check_tool i686-linux-musl-gcc 0 "fetched by this script into $MUSL_DIR (ports/ without Docker)"
    check_tool rsvg-convert      0 "apt: librsvg2-bin (make icons)"
    check_tool docker            0 "apt: docker.io (rebuilding ports/)"
    check_tool gdb-multiarch     0 "apt: gdb-multiarch (make gdb)"
    check_tool lsof              0 "apt: lsof (ss/fuser are used otherwise)"
    check_tool xrandr            0 "apt: x11-xserver-utils (screen-size auto-fit)"

    if [ -d /usr/lib/grub/i386-pc ]; then
        printf '  [ ok ]   %-24s %s\n' "grub i386-pc modules" "/usr/lib/grub/i386-pc"
    else
        printf '  [MISSING] %-23s %s\n' "grub i386-pc modules" "apt: grub-pc-bin (grub-mkrescue needs them for a BIOS ISO)"
        MISSING_REQUIRED=1
    fi
    if have qemu-system-i386; then
        if qemu-system-i386 -display help 2>/dev/null | grep -qxE 'gtk|sdl'; then
            printf '  [ ok ]   %-24s %s\n' "qemu gui display" "$(qemu-system-i386 -display help 2>/dev/null | grep -xE 'gtk|sdl' | head -n 1)"
        else
            printf '  [ -- ]   %-24s optional: %s\n' "qemu gui display" "apt: qemu-system-gui (make start needs a window)"
        fi
    fi
    if have gcc; then
        if printf 'int main(void){return 0;}' | gcc -m32 -x c - -o /dev/null 2>/dev/null; then
            printf '  [ ok ]   %-24s %s\n' "gcc -m32" "works"
        else
            printf '  [ -- ]   %-24s optional: %s\n' "gcc -m32" "apt: gcc-multilib (build-glstubs.sh without Docker)"
        fi
    fi
}

print_path_line() {
    step "Add this to your shell (e.g. ~/.bashrc), then open a new shell:"
    say ""
    say "    export PATH=\"$PREFIX/bin:$MUSL_DIR/bin:\$PATH\""
    say ""
    say "  or:  . $PREFIX/maeros-env.sh"
}

# ── Plan / check modes ────────────────────────────────────────────────────────
step "MaeroOS Linux host setup"
say "  repo      : $REPO_ROOT"
say "  PREFIX    : $PREFIX   (i686-elf binutils $BINUTILS_VER + gcc $GCC_VER)"
say "  OPT_DIR   : $OPT_DIR  (musl.cc i686-linux-musl-cross)"
say "  SRC_DIR   : $SRC_DIR"
say "  JOBS      : $JOBS"
say "  mode      : $MODE"

if [ "$MODE" = check ]; then
    verify
    [ "$MISSING_REQUIRED" = 0 ] || { print_path_line; die "required tools are missing (see above)"; }
    print_path_line
    say ""
    say "All required tools present."
    exit 0
fi

step "1. apt packages"
apt_lines
if [ "$MODE" = dry-run ]; then
    step "2. i686-elf cross toolchain -> $PREFIX"
    if [ "$DO_CROSS" = 0 ]; then say "   skipped (--no-cross)"; else
        if cross_binutils_ok; then say "   binutils: already installed, skip"; else
            say "   binutils $BINUTILS_VER: fetch $GNU_MIRROR/binutils/binutils-$BINUTILS_VER.tar.xz, build in $SRC_DIR/build-binutils, install"; fi
        if cross_gcc_ok; then say "   gcc: already installed, skip"; else
            say "   gcc $GCC_VER: fetch $GNU_MIRROR/gcc/gcc-$GCC_VER/gcc-$GCC_VER.tar.xz, build all-gcc all-target-libgcc in $SRC_DIR/build-gcc, install"; fi
    fi
    step "3. musl.cc toolchain -> $MUSL_TGZ, $MUSL_DIR"
    if [ "$DO_MUSL" = 0 ]; then say "   skipped (--no-musl)"; else
        if [ -f "$MUSL_TGZ" ]; then say "   tarball present, skip download"; else say "   fetch $MUSL_URL"; fi
        if musl_ok; then say "   already extracted, skip"; else say "   extract into $OPT_DIR"; fi
    fi
    step "4. verify"
    say "   run: $0 --check"
    print_path_line
    exit 0
fi

# ── Build mode ────────────────────────────────────────────────────────────────
if [ "$DO_APT" = 1 ]; then
    step "Installing apt packages (sudo will prompt for your password)"
    have sudo || die "sudo not found; install the packages above as root instead"
    have apt-get || die "apt-get not found; this is not a Debian/Ubuntu host"
    pkgs="$APT_REQUIRED"
    [ "$APT_OPTIONAL" = 1 ] && pkgs="$pkgs $APT_OPTIONAL_PKGS"
    sudo apt-get update
    # shellcheck disable=SC2086
    sudo apt-get install -y $pkgs
else
    say ""
    say "(not installing: re-run with --apt to install them, or paste the command)"
fi

# Host prerequisites for building the cross toolchain.
if [ "$DO_CROSS" = 1 ] && ! { cross_binutils_ok && cross_gcc_ok; }; then
    missing=""
    for t in gcc g++ make tar xz bison flex makeinfo; do have "$t" || missing="$missing $t"; done
    have wget || have curl || missing="$missing wget"
    for h in gmp.h mpfr.h mpc.h; do
        [ -f "/usr/include/$h" ] || [ -f "/usr/include/x86_64-linux-gnu/$h" ] || missing="$missing $h"
    done
    [ -z "$missing" ] || die "cannot build the cross toolchain, host is missing:$missing
  install the packages above (tools/setup-linux.sh --apt) and re-run"
fi

if [ "$DO_CROSS" = 1 ]; then
    step "2. i686-elf cross toolchain -> $PREFIX"
    mkdir -p "$PREFIX" "$SRC_DIR"
    export PATH="$BUILD_PATH"

    if cross_binutils_ok; then
        say "   binutils: $PREFIX/bin/$TARGET-ld present, skipping"
    else
        say "   binutils $BINUTILS_VER"
        tarball="$SRC_DIR/binutils-$BINUTILS_VER.tar.xz"
        fetch "$GNU_MIRROR/binutils/binutils-$BINUTILS_VER.tar.xz" "$tarball"
        xz -t "$tarball" || { rm -f "$tarball"; die "corrupt download $tarball (removed; re-run)"; }
        [ -d "$SRC_DIR/binutils-$BINUTILS_VER" ] || run_logged "$SRC_DIR/binutils-extract.log" tar -xJf "$tarball" -C "$SRC_DIR"
        rm -rf "$SRC_DIR/build-binutils"
        mkdir -p "$SRC_DIR/build-binutils"
        cd "$SRC_DIR/build-binutils"
        run_logged "$SRC_DIR/binutils-configure.log" "$SRC_DIR/binutils-$BINUTILS_VER/configure" \
            --target="$TARGET" --prefix="$PREFIX" --with-sysroot --disable-nls --disable-werror
        run_logged "$SRC_DIR/binutils-make.log" make -j"$JOBS"
        run_logged "$SRC_DIR/binutils-install.log" make install
        cd "$REPO_ROOT"
        cross_binutils_ok || die "binutils install did not produce $PREFIX/bin/$TARGET-ld"
        say "   binutils installed"
    fi

    if cross_gcc_ok; then
        say "   gcc: $PREFIX/bin/$TARGET-gcc present (with libgcc), skipping"
    else
        say "   gcc $GCC_VER (C only, --without-headers, + libgcc)"
        tarball="$SRC_DIR/gcc-$GCC_VER.tar.xz"
        fetch "$GNU_MIRROR/gcc/gcc-$GCC_VER/gcc-$GCC_VER.tar.xz" "$tarball"
        xz -t "$tarball" || { rm -f "$tarball"; die "corrupt download $tarball (removed; re-run)"; }
        [ -d "$SRC_DIR/gcc-$GCC_VER" ] || run_logged "$SRC_DIR/gcc-extract.log" tar -xJf "$tarball" -C "$SRC_DIR"
        rm -rf "$SRC_DIR/build-gcc"
        mkdir -p "$SRC_DIR/build-gcc"
        cd "$SRC_DIR/build-gcc"
        run_logged "$SRC_DIR/gcc-configure.log" "$SRC_DIR/gcc-$GCC_VER/configure" \
            --target="$TARGET" --prefix="$PREFIX" --disable-nls --disable-werror \
            --enable-languages=c --without-headers \
            --disable-shared --disable-threads --disable-libssp --disable-libquadmath
        run_logged "$SRC_DIR/gcc-make.log" make -j"$JOBS" all-gcc all-target-libgcc
        run_logged "$SRC_DIR/gcc-install.log" make install-gcc install-target-libgcc
        cd "$REPO_ROOT"
        cross_gcc_ok || die "gcc install did not produce a working $PREFIX/bin/$TARGET-gcc"
        say "   gcc installed: $("$PREFIX/bin/$TARGET-gcc" --version | head -n 1)"
    fi
fi

if [ "$DO_MUSL" = 1 ]; then
    step "3. musl.cc i686-linux-musl toolchain"
    if [ -f "$MUSL_TGZ" ]; then
        say "   $MUSL_TGZ present, skipping download"
    else
        fetch "$MUSL_URL" "$MUSL_TGZ"
    fi
    gzip -t "$MUSL_TGZ" 2>/dev/null || { rm -f "$MUSL_TGZ"; die "corrupt download $MUSL_TGZ (removed; re-run)"; }
    if musl_ok; then
        say "   $MUSL_DIR present, skipping extract"
    else
        mkdir -p "$OPT_DIR"
        run_logged "$OPT_DIR/musl-extract.log" tar -xzf "$MUSL_TGZ" -C "$OPT_DIR"
        musl_ok || die "extracting $MUSL_TGZ did not produce $MUSL_DIR/bin/i686-linux-musl-gcc"
        say "   extracted to $MUSL_DIR"
    fi
fi

# Environment file for convenience.
mkdir -p "$PREFIX"
cat >"$PREFIX/maeros-env.sh" <<ENV
# Generated by tools/setup-linux.sh — source this to use the MaeroOS toolchains.
export PATH="$PREFIX/bin:$MUSL_DIR/bin:\$PATH"
ENV

verify
print_path_line
if [ "$MISSING_REQUIRED" = 0 ]; then
    say ""
    say "Done. Next: make && make initrd && make disk && make iso && make smoke"
else
    say ""
    die "required tools are still missing (see above)"
fi
