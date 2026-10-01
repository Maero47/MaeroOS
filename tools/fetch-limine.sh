#!/bin/sh
# fetch-limine.sh — fetch the pinned Limine bootloader binaries for
# `make limine-iso` (hybrid BIOS + UEFI ISO, Multiboot 2).
#
# Limine (BSD-2-Clause, https://github.com/limine-bootloader/limine) publishes
# its prebuilt boot files on the vX.Y.Z-binary tags: limine-bios.sys,
# limine-bios-cd.bin, limine-uefi-cd.bin, BOOTX64.EFI, BOOTIA32.EFI and the
# source of the `limine` host tool (limine.c, needed for `bios-install`).
# The tag's tarball is pinned by sha256; the host tool is built with cc.
#
# Result (gitignored): third_party/limine/bin/ with those files + `limine`.
# third_party/limine/LICENSE is the upstream licence, kept in the tree and
# copied into every image.  Idempotent: a matching tree is left alone.
#
#   LIMINE_VER=11.4.1        release to fetch
#   LIMINE_SHA256=<hex>      its tarball digest (pinned for the default)
#   MAEROS_SKIP_HASH=1       accept an unverified download (not recommended)
set -eu

LIMINE_VER=${LIMINE_VER:-11.4.1}
LIMINE_SHA256_PINNED_VER=11.4.1
LIMINE_SHA256_PINNED=82c0653d97b02af122a385028b4a5f334db51f440c0b94a1e7d29ee6532bf202
if [ "$LIMINE_VER" = "$LIMINE_SHA256_PINNED_VER" ]; then
    LIMINE_SHA256=${LIMINE_SHA256:-$LIMINE_SHA256_PINNED}
fi
URL="https://github.com/limine-bootloader/limine/archive/refs/tags/v$LIMINE_VER-binary.tar.gz"

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEST="$ROOT/third_party/limine/bin"
TARBALL="$ROOT/third_party/limine-$LIMINE_VER-binary.tar.gz"
STAMP="$DEST/.version"
FILES="limine-bios.sys limine-bios-cd.bin limine-uefi-cd.bin BOOTX64.EFI BOOTIA32.EFI LICENSE limine.c limine-bios-hdd.h"

die() { printf 'fetch-limine: %s\n' "$*" >&2; exit 1; }

if [ -f "$STAMP" ] && [ "$(cat "$STAMP")" = "$LIMINE_VER" ] && [ -x "$DEST/limine" ]; then
    exit 0
fi

if [ ! -f "$TARBALL" ]; then
    command -v curl >/dev/null 2>&1 || die "need curl to download $URL"
    echo "fetch-limine: downloading $URL"
    curl -fL --proto '=https' --proto-redir '=https' -o "$TARBALL.part" "$URL" \
        || die "download failed: $URL"
    mv "$TARBALL.part" "$TARBALL"
fi

if [ "${MAEROS_SKIP_HASH:-0}" = 1 ]; then
    echo "fetch-limine: MAEROS_SKIP_HASH=1, not verifying $TARBALL"
else
    [ -n "${LIMINE_SHA256:-}" ] || die "no pinned sha256 for Limine $LIMINE_VER; pass LIMINE_SHA256=<hex>"
    got=$(sha256sum "$TARBALL" | cut -d' ' -f1)
    if [ "$got" != "$LIMINE_SHA256" ]; then
        rm -f "$TARBALL"
        die "sha256 mismatch for $TARBALL: got $got, want $LIMINE_SHA256 (deleted; re-run to retry)"
    fi
fi

rm -rf "$DEST"
mkdir -p "$DEST"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
tar xzf "$TARBALL" -C "$tmp"
src="$tmp/Limine-$LIMINE_VER-binary"
[ -d "$src" ] || src=$(find "$tmp" -mindepth 1 -maxdepth 1 -type d | head -n 1)
for f in $FILES; do
    [ -f "$src/$f" ] || die "Limine $LIMINE_VER tarball has no $f"
    cp "$src/$f" "$DEST/"
done
cmp -s "$DEST/LICENSE" "$ROOT/third_party/limine/LICENSE" \
    || echo "fetch-limine: note: upstream LICENSE differs from third_party/limine/LICENSE; review and update it"

CC=${HOST_CC:-cc}
command -v "$CC" >/dev/null 2>&1 || die "need a host C compiler ($CC) to build the limine tool"
"$CC" -O2 -std=c99 -I"$DEST" "$DEST/limine.c" -o "$DEST/limine" || die "building the limine host tool failed"
echo "$LIMINE_VER" > "$STAMP"
echo "fetch-limine: Limine $LIMINE_VER ready in third_party/limine/bin"
