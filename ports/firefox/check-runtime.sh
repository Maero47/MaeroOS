#!/bin/sh
# Verify that the on-disk Firefox runtime tree is closed under dynamic linking.
#
# Walks the DT_NEEDED chains (readelf -d) of firefox-bin, libxul.so and every
# ELF shared object under testfiles/firefox (including pixbuf-loaders/) and
# testfiles/lib, resolving each requested soname against the same search path
# the MaeroOS launcher uses at runtime:
#
#     LD_LIBRARY_PATH=/lib:/disk/lib:/disk/firefox
#     -> testfiles/lib : testfiles/lib : testfiles/firefox
#
# Exits 0 when every soname resolves inside the tree, 1 otherwise.  Prints a
# short summary and the full list of unresolved sonames (with the objects that
# request them) so the missing package can be added to debian-packages.txt.
#
# Usage: sh ports/firefox/check-runtime.sh [-v]
#   -v   also print the resolved soname -> file table
set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
FF="$ROOT/testfiles/firefox"
LIB="$ROOT/testfiles/lib"
SEARCH="$LIB:$FF"
VERBOSE=0
[ "${1:-}" = "-v" ] && VERBOSE=1

command -v readelf >/dev/null 2>&1 || { echo "check-runtime: readelf not found (install binutils)" >&2; exit 2; }
[ -f "$FF/firefox-bin" ] || { echo "check-runtime: $FF/firefox-bin is missing (run ports/firefox/fetch-runtime.sh)" >&2; exit 2; }

# Every ELF object in the tree: the two named roots first, then all .so files.
# Firefox's helper executables (plugin-container, glxtest, ...) are included
# too since Firefox spawns them with the same environment.
is_elf() { [ "$(head -c 4 "$1" 2>/dev/null | od -An -c | tr -d ' \n')" = "177ELF" ]; }

OBJS=$(mktemp)
trap 'rm -f "$OBJS" "$OBJS.needs" "$OBJS.sonames" "$OBJS.unres"' EXIT
{
    printf '%s\n' "$FF/firefox-bin" "$FF/libxul.so"
    find "$FF" "$LIB" -type f \( -name '*.so' -o -name '*.so.*' \) | sort
    for x in plugin-container glxtest vaapitest crashreporter minidump-analyzer pingsender updater; do
        [ -f "$FF/$x" ] && printf '%s\n' "$FF/$x"
    done
} | awk '!seen[$0]++' | while IFS= read -r f; do is_elf "$f" && printf '%s\n' "$f"; done > "$OBJS"

nobj=$(wc -l < "$OBJS")
: > "$OBJS.needs"
while IFS= read -r f; do
    readelf -d "$f" 2>/dev/null | awk -v f="$f" '/\(NEEDED\)/ { s=$NF; gsub(/[\[\]]/, "", s); print f "\t" s }' >> "$OBJS.needs"
done < "$OBJS"

# Resolve each distinct soname once against the search path.
resolve() {
    IFS=:
    for d in $SEARCH; do
        if [ -f "$d/$1" ]; then printf '%s\n' "$d/$1"; unset IFS; return 0; fi
    done
    unset IFS
    return 1
}

nneed=0; nunres=0
: > "$OBJS.unres"
cut -f2 "$OBJS.needs" | sort -u > "$OBJS.sonames"
while IFS= read -r so; do
    nneed=$((nneed + 1))
    if r=$(resolve "$so"); then
        [ $VERBOSE = 1 ] && printf '  %-32s -> %s\n' "$so" "${r#"$ROOT"/}"
    else
        nunres=$((nunres + 1))
        printf '%s\n' "$so" >> "$OBJS.unres"
    fi
done < "$OBJS.sonames"

echo "check-runtime: search path = ${SEARCH#"$ROOT"/}" | sed "s|:$ROOT/|:|"
echo "check-runtime: $nobj ELF objects scanned, $nneed distinct DT_NEEDED sonames, $nunres unresolved"
if [ "$nunres" -gt 0 ]; then
    echo "check-runtime: UNRESOLVED:"
    while IFS= read -r so; do
        printf '  %s  (needed by: %s)\n' "$so" \
            "$(awk -F'\t' -v s="$so" '$2==s {sub(/.*\//, "", $1); print $1}' "$OBJS.needs" | sort -u | tr '\n' ' ')"
    done < "$OBJS.unres"
    exit 1
fi
echo "check-runtime: OK - the tree is closed under dynamic linking"
