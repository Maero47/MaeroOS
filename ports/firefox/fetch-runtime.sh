#!/bin/sh
# Rebuild the on-disk Firefox runtime tree from public sources.
#
# Produces (all gitignored except testfiles/lib):
#   testfiles/firefox/            official Firefox 115 ESR linux-i686 tarball,
#                                 extracted as-is (firefox-bin, libxul.so,
#                                 omni.ja, browser/, gmp-clearkey/, fonts/, ...)
#     + lib*.so.*                 the glibc GTK3 stack from Debian i386 .debs,
#                                 pruned to the DT_NEEDED closure of Firefox
#     + pixbuf-loaders/           gdk-pixbuf loader modules + loaders.cache
#                                 with /disk/firefox/pixbuf-loaders/ paths
#     + libGL.so.1 libEGL.so.1 libGLESv2.so.2 libpci.so.3 libdrm.so.2
#                                 empty stubs for Firefox's glxtest GPU probe
#   testfiles/lib/                glibc runtime (ld-linux.so.2, libc.so.6, ...)
#                                 + libgcc_s / libstdc++ from the same suite
#
# Everything is fetched by URL: the Firefox tarball from ftp.mozilla.org
# (verified against Mozilla's SHA256SUMS) and Debian packages from
# deb.debian.org, resolved through the suite's Packages.xz index (verified
# against the SHA256 recorded there).  .debs are unpacked with dpkg-deb -x;
# no apt, no dpkg -i, no root, no Docker.
#
# Downloads are cached under ports/firefox/prebuilt/ and the script is
# idempotent: re-running it re-uses every cached download and stamps.
#
# Usage:  sh ports/firefox/fetch-runtime.sh
#   SUITE=trixie|bookworm   Debian suite for glibc AND the GTK stack
#                           (default trixie: glibc 2.41 fixes the condvar
#                           lost-wakeup bug BZ#25847 that bookworm's 2.36 has)
#   FF_VERSION=115.15.0esr  Firefox release to fetch
#   DEBIAN_MIRROR=...       default https://deb.debian.org/debian
#   REFRESH_INDEX=1         re-download the Packages.xz index
#   SKIP_CHECK=1            skip the final closure check
#   NO_I386_EXEC=1          pretend the host cannot run i386 binaries (exercises
#                           the loaders.cache.template fallback)
#
# Afterwards: ports/firefox/check-runtime.sh re-verifies the tree at any time.
set -eu

SUITE=${SUITE:-trixie}
FF_VERSION=${FF_VERSION:-115.15.0esr}
DEBIAN_MIRROR=${DEBIAN_MIRROR:-https://deb.debian.org/debian}
MOZ_BASE=${MOZ_BASE:-https://ftp.mozilla.org/pub/firefox/releases}
ARCH=i386
FF_ARCH=linux-i686
FF_LOCALE=en-US

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
CACHE="$HERE/prebuilt"
SUITEDIR="$CACHE/$SUITE"
DEBS="$SUITEDIR/debs"
UNPACK="$SUITEDIR/root"          # dpkg-deb -x target (merged for all packages)
POOL="$SUITEDIR/pool"            # flat soname -> real file
FFDIR="$ROOT/testfiles/firefox"
LIBDIR="$ROOT/testfiles/lib"
FONTDIR="$ROOT/testfiles/usr/share/fonts"
PKGLIST="$HERE/debian-packages.txt"
MANIFEST="$FFDIR/.fetch-runtime.manifest"   # files this script placed in FFDIR

# Runtime library search path on MaeroOS is /lib:/disk/lib:/disk/firefox.
DISK_PIXBUF_DIR=/disk/firefox/pixbuf-loaders

# glibc + toolchain runtime that lives in testfiles/lib (= /lib on the initrd).
GLIBC_LIBS="ld-linux.so.2 libc.so.6 libm.so.6 libdl.so.2 libpthread.so.0 librt.so.1
libresolv.so.2 libnss_dns.so.2 libnss_files.so.2 libanl.so.1 libutil.so.1
libthread_db.so.1 libgcc_s.so.1 libstdc++.so.6"
# Subset mirrored into testfiles/lib/i386-linux-gnu (glibc's own default dir).
GLIBC_MULTIARCH_LIBS="ld-linux.so.2 libc.so.6 libm.so.6 libdl.so.2 libpthread.so.0
librt.so.1 libresolv.so.2 libnss_dns.so.2 libnss_files.so.2 libanl.so.1
libutil.so.1 libthread_db.so.1"

# gdk-pixbuf loaders NOT shipped: tiff needs libtiff + 7 codecs, svg needs
# librsvg; Firefox decodes content images itself and the GTK icon theme on
# disk is PNG (built into gdk-pixbuf >= 2.42 together with JPEG).
PIXBUF_LOADER_SKIP="tiff svg"

# Stubs for the glxtest GPU probe (see build-glstubs.sh for the rationale).
GL_STUBS="libGL.so.1 libEGL.so.1 libGLESv2.so.2 libpci.so.3 libdrm.so.2"

log()  { printf '[fetch-runtime] %s\n' "$*"; }
warn() { printf '[fetch-runtime] WARNING: %s\n' "$*" >&2; }
die()  { printf '[fetch-runtime] ERROR: %s\n' "$*" >&2; exit 1; }

need() { command -v "$1" >/dev/null 2>&1 || die "required tool '$1' not found${2:+ ($2)}"; }
need curl "or set FETCH=wget below"
need dpkg-deb "package dpkg on Debian/Ubuntu, dpkg on Fedora/Arch/Homebrew"
need readelf "package binutils"
need xz
need bzip2
need sha256sum
need awk

download() {  # download URL DEST
    [ -s "$2" ] && return 0
    log "fetching $(basename "$2")"
    curl -fsSL --retry 3 -o "$2.part" "$1" || { rm -f "$2.part"; die "download failed: $1"; }
    mv "$2.part" "$2"
}

sha256_check() {  # sha256_check FILE EXPECTED
    printf '%s  %s\n' "$2" "$1" | sha256sum -c --quiet >/dev/null 2>&1
}

is_elf() { [ "$(head -c 4 "$1" 2>/dev/null | od -An -c | tr -d ' \n')" = "177ELF" ]; }

soname_of() {  # DT_SONAME or empty
    readelf -d "$1" 2>/dev/null | awk '/\(SONAME\)/ { s=$NF; gsub(/[\[\]]/, "", s); print s; exit }'
}

needed_of() {  # DT_NEEDED list, one per line
    readelf -d "$1" 2>/dev/null | awk '/\(NEEDED\)/ { s=$NF; gsub(/[\[\]]/, "", s); print s }'
}

# Can this host execute i386 glibc binaries through our copied ld.so?  If so
# we run gdk-pixbuf-query-loaders for real and smoke-test the result.
host_runs_i386() {
    [ "${NO_I386_EXEC:-0}" = 1 ] && return 1     # test hook for the fallback paths
    [ -x "$LIBDIR/ld-linux.so.2" ] || return 1
    [ -x "$ROOT/testfiles/glibc-hello" ] || return 1
    out=$("$LIBDIR/ld-linux.so.2" --library-path "$LIBDIR" "$ROOT/testfiles/glibc-hello" 2>/dev/null) || return 1
    [ "$out" = "GLIBC_HELLO_OK" ]
}

mkdir -p "$CACHE" "$SUITEDIR" "$DEBS" "$UNPACK" "$POOL" "$FFDIR" "$LIBDIR" "$LIBDIR/i386-linux-gnu"

# ---------------------------------------------------------------------------
# 1. Firefox
# ---------------------------------------------------------------------------
FF_TARBALL="firefox-$FF_VERSION.tar.bz2"
FF_URL="$MOZ_BASE/$FF_VERSION/$FF_ARCH/$FF_LOCALE/$FF_TARBALL"
FF_SUMS="$CACHE/SHA256SUMS-$FF_VERSION"
FF_STAMP="$FFDIR/.fetch-runtime.firefox"

download "$MOZ_BASE/$FF_VERSION/SHA256SUMS" "$FF_SUMS"
download "$FF_URL" "$CACHE/$FF_TARBALL"
expected=$(awk -v f="$FF_ARCH/$FF_LOCALE/$FF_TARBALL" '$2==f {print $1}' "$FF_SUMS")
[ -n "$expected" ] || die "$FF_TARBALL not listed in Mozilla's SHA256SUMS"
sha256_check "$CACHE/$FF_TARBALL" "$expected" || {
    rm -f "$CACHE/$FF_TARBALL"
    die "$FF_TARBALL failed SHA256 verification (deleted; re-run to fetch again)"
}
log "firefox tarball $FF_TARBALL verified (sha256 $expected)"

if [ "$(cat "$FF_STAMP" 2>/dev/null)" = "$FF_VERSION" ] && [ -f "$FFDIR/firefox-bin" ] && [ -f "$FFDIR/libxul.so" ]; then
    log "firefox $FF_VERSION already extracted in ${FFDIR#"$ROOT"/}"
else
    log "extracting $FF_TARBALL -> ${FFDIR#"$ROOT"/} (nothing stripped)"
    tar -xjf "$CACHE/$FF_TARBALL" -C "$FFDIR" --strip-components=1
    printf '%s\n' "$FF_VERSION" > "$FF_STAMP"
fi
for f in firefox-bin libxul.so omni.ja dependentlibs.list application.ini browser/omni.ja \
         gmp-clearkey defaults fonts; do
    [ -e "$FFDIR/$f" ] || die "firefox tree is incomplete: $f missing"
done
while IFS= read -r l; do
    [ -f "$FFDIR/$l" ] || die "dependentlibs.list names $l but it is not in the tree"
done < "$FFDIR/dependentlibs.list"
grep -q "^Version=${FF_VERSION%esr}" "$FFDIR/application.ini" || die "application.ini does not say Version=${FF_VERSION%esr}"
log "dependentlibs.list ($(wc -l < "$FFDIR/dependentlibs.list") entries) and application.ini are intact"

# ---------------------------------------------------------------------------
# 2. Debian package index
# ---------------------------------------------------------------------------
INDEX_XZ="$SUITEDIR/Packages.xz"
INDEX="$SUITEDIR/Packages.tsv"      # package \t version \t filename \t sha256
[ "${REFRESH_INDEX:-0}" = 1 ] && rm -f "$INDEX_XZ" "$INDEX"
download "$DEBIAN_MIRROR/dists/$SUITE/main/binary-$ARCH/Packages.xz" "$INDEX_XZ"
if [ ! -s "$INDEX" ]; then
    log "indexing $SUITE/main/binary-$ARCH"
    xz -dc "$INDEX_XZ" | awk -v RS= -F'\n' '
        { p=v=f=h=""
          for (i = 1; i <= NF; i++) {
              if      ($i ~ /^Package: /)  p = substr($i, 10)
              else if ($i ~ /^Version: /)  v = substr($i, 10)
              else if ($i ~ /^Filename: /) f = substr($i, 11)
              else if ($i ~ /^SHA256: /)   h = substr($i, 9)
          }
          if (p != "" && f != "") print p "\t" v "\t" f "\t" h }' > "$INDEX.part"
    mv "$INDEX.part" "$INDEX"
fi

index_lookup() {  # index_lookup PKG -> "version\tfilename\tsha256" or nothing
    awk -F'\t' -v p="$1" '$1==p { print $2 "\t" $3 "\t" $4; exit }' "$INDEX"
}

# Resolve one debian-packages.txt entry ("a|b|c") to the first name in the index.
resolve_pkg() {
    old_ifs=$IFS; IFS='|'
    for cand in $1; do
        if [ -n "$(index_lookup "$cand")" ]; then IFS=$old_ifs; printf '%s\n' "$cand"; return 0; fi
    done
    IFS=$old_ifs
    return 1
}

# ---------------------------------------------------------------------------
# 3. Fetch + unpack the packages
# ---------------------------------------------------------------------------
fetch_pkg() {  # fetch_pkg PKG : download, verify, dpkg-deb -x (stamped)
    pkg=$1
    entry=$(index_lookup "$pkg")
    ver=$(printf '%s' "$entry" | cut -f1)
    file=$(printf '%s' "$entry" | cut -f2)
    sum=$(printf '%s' "$entry" | cut -f3)
    deb="$DEBS/$(basename "$file")"
    download "$DEBIAN_MIRROR/$file" "$deb"
    sha256_check "$deb" "$sum" || { rm -f "$deb"; die "$(basename "$deb") failed SHA256 verification (deleted; re-run)"; }
    stamp="$UNPACK/.unpacked/$pkg"
    if [ "$(cat "$stamp" 2>/dev/null)" != "$ver" ]; then
        log "unpacking $pkg $ver"
        dpkg-deb -x "$deb" "$UNPACK"
        mkdir -p "$UNPACK/.unpacked"
        printf '%s\n' "$ver" > "$stamp"
    fi
    printf '%s %s\n' "$pkg" "$ver" >> "$SUITEDIR/installed.txt"
}

: > "$SUITEDIR/installed.txt"
sed -e 's/#.*//' -e '/^[[:space:]]*$/d' "$PKGLIST" | while IFS= read -r entry; do
    entry=$(printf '%s' "$entry" | tr -d '[:space:]')
    pkg=$(resolve_pkg "$entry") || die "none of '$entry' exists in $SUITE/$ARCH (edit debian-packages.txt)"
    fetch_pkg "$pkg"
done
log "$(wc -l < "$SUITEDIR/installed.txt") packages from Debian $SUITE/$ARCH unpacked under ${UNPACK#"$ROOT"/}"

# Library directories inside the unpacked tree (bookworm .debs still ship
# glibc under /lib, trixie is fully merged-/usr).
LIBSRC="$UNPACK/usr/lib/$ARCH-linux-gnu $UNPACK/lib/$ARCH-linux-gnu $UNPACK/usr/lib $UNPACK/lib"

# Flat pool: every ELF shared object, named by its DT_SONAME (basename if none).
rm -rf "$POOL"; mkdir -p "$POOL"
for d in $LIBSRC; do
    [ -d "$d" ] || continue
    find "$d" -maxdepth 1 -type f \( -name '*.so' -o -name '*.so.*' \) | while IFS= read -r f; do
        is_elf "$f" || continue
        so=$(soname_of "$f"); [ -n "$so" ] || so=$(basename "$f")
        [ -e "$POOL/$so" ] || cp "$f" "$POOL/$so"
    done
done
log "pool holds $(find "$POOL" -type f | wc -l) shared objects"

# ---------------------------------------------------------------------------
# 4. glibc -> testfiles/lib
# ---------------------------------------------------------------------------
glibc_ver=$(awk '$1=="libc6" {print $2}' "$SUITEDIR/installed.txt")
gcc_ver=$(awk '$1=="libgcc-s1" {print $2}' "$SUITEDIR/installed.txt")
for so in $GLIBC_LIBS; do
    [ -f "$POOL/$so" ] || die "glibc runtime file $so not found in the $SUITE packages"
    cp "$POOL/$so" "$LIBDIR/$so.tmp" && mv "$LIBDIR/$so.tmp" "$LIBDIR/$so"
done
for so in $GLIBC_MULTIARCH_LIBS; do
    cp "$LIBDIR/$so" "$LIBDIR/i386-linux-gnu/$so.tmp" && mv "$LIBDIR/i386-linux-gnu/$so.tmp" "$LIBDIR/i386-linux-gnu/$so"
done
chmod 755 "$LIBDIR/ld-linux.so.2" "$LIBDIR/libc.so.6" "$LIBDIR/i386-linux-gnu/ld-linux.so.2" "$LIBDIR/i386-linux-gnu/libc.so.6"
cat > "$LIBDIR/GLIBC-VERSION" <<EOT
Debian $SUITE $ARCH: libc6 $glibc_ver, libgcc-s1/libstdc++6 $gcc_ver
Installed by ports/firefox/fetch-runtime.sh (SUITE=$SUITE); do not edit by hand.
EOT
log "glibc $glibc_ver (Debian $SUITE) installed into ${LIBDIR#"$ROOT"/}"

# ---------------------------------------------------------------------------
# 5. Remove what a previous run placed in testfiles/firefox (suite switches)
# ---------------------------------------------------------------------------
if [ -f "$MANIFEST" ]; then
    while IFS= read -r rel; do rm -f "$FFDIR/$rel"; done < "$MANIFEST"
fi
: > "$MANIFEST"
mkdir -p "$FFDIR/pixbuf-loaders"
install_ff() {  # install_ff SRC RELNAME
    cp "$1" "$FFDIR/$2.tmp" && mv "$FFDIR/$2.tmp" "$FFDIR/$2"
    printf '%s\n' "$2" >> "$MANIFEST"
}

# ---------------------------------------------------------------------------
# 6. gdk-pixbuf loader modules
# ---------------------------------------------------------------------------
loaderdir=$(find "$UNPACK/usr/lib/$ARCH-linux-gnu/gdk-pixbuf-2.0" -maxdepth 2 -type d -name loaders | head -n 1)
[ -n "$loaderdir" ] || die "gdk-pixbuf loaders directory not found in the unpacked packages"
nload=0
for f in "$loaderdir"/libpixbufloader-*.so; do
    name=$(basename "$f" .so); fmt=${name#libpixbufloader-}
    skip=0; for s in $PIXBUF_LOADER_SKIP; do [ "$fmt" = "$s" ] && skip=1; done
    [ $skip = 1 ] && continue
    install_ff "$f" "pixbuf-loaders/$(basename "$f")"
    nload=$((nload + 1))
done
log "$nload gdk-pixbuf loader modules installed (skipped: $PIXBUF_LOADER_SKIP)"

# ---------------------------------------------------------------------------
# 7. GL stubs
# ---------------------------------------------------------------------------
build_stubs_with_cc() {  # build_stubs_with_cc "compiler command"
    tmp=$(mktemp -d); printf 'int __maeros_glstub_marker = 1;\n' > "$tmp/stub.c"
    for n in $GL_STUBS; do
        # shellcheck disable=SC2086
        $1 -shared -fPIC -nostdlib -Wl,-soname,"$n" -o "$tmp/$n" "$tmp/stub.c" 2>/dev/null || { rm -rf "$tmp"; return 1; }
        [ "$(readelf -h "$tmp/$n" 2>/dev/null | awk '/Machine:/ {print $NF}')" = "80386" ] || { rm -rf "$tmp"; return 1; }
    done
    for n in $GL_STUBS; do install_ff "$tmp/$n" "$n"; done
    rm -rf "$tmp"
}
stubs_done=""
for cc in "i686-linux-musl-gcc" "i686-linux-gnu-gcc" "gcc -m32" "cc -m32" "clang -m32 --target=i386-linux-gnu"; do
    command -v "${cc%% *}" >/dev/null 2>&1 || continue
    if build_stubs_with_cc "$cc"; then stubs_done="$cc"; break; fi
done
if [ -z "$stubs_done" ] && command -v python3 >/dev/null 2>&1; then
    tmp=$(mktemp -d)
    for n in $GL_STUBS; do python3 "$HERE/mkstub.py" "$tmp/$n" "$n"; install_ff "$tmp/$n" "$n"; done
    rm -rf "$tmp"
    stubs_done="mkstub.py (no i686 C compiler found; hand-assembled ELF)"
fi
if [ -n "$stubs_done" ]; then
    log "GL stubs ($GL_STUBS) built with $stubs_done"
else
    warn "no i686 C compiler and no python3: GL stubs NOT built (glxtest will hit its 4 s timeout)"
fi

# ---------------------------------------------------------------------------
# 8. DT_NEEDED closure: copy from the pool only what Firefox can reach
# ---------------------------------------------------------------------------
queue=$(mktemp); seen=$(mktemp); missing=$(mktemp)
trap 'rm -f "$queue" "$queue.rest" "$seen" "$missing"' EXIT
{
    find "$FFDIR" -type f \( -name '*.so' -o -name '*.so.*' -o -name 'firefox-bin' \
        -o -name plugin-container -o -name glxtest -o -name vaapitest -o -name crashreporter \
        -o -name minidump-analyzer -o -name pingsender -o -name updater \) \
        | while IFS= read -r f; do is_elf "$f" && needed_of "$f"; done
} | sort -u > "$queue"
: > "$seen"; : > "$missing"
ncopied=0
while [ -s "$queue" ]; do
    so=$(head -n 1 "$queue"); tail -n +2 "$queue" > "$queue.rest"; mv "$queue.rest" "$queue"
    grep -qx "$so" "$seen" && continue
    printf '%s\n' "$so" >> "$seen"
    if [ -f "$LIBDIR/$so" ]; then
        continue                                     # satisfied by /lib
    elif [ -f "$FFDIR/$so" ]; then
        src="$FFDIR/$so"                             # Firefox's own or a stub
    elif [ -f "$POOL/$so" ]; then
        install_ff "$POOL/$so" "$so"; src="$FFDIR/$so"; ncopied=$((ncopied + 1))
    else
        printf '%s\n' "$so" >> "$missing"; continue
    fi
    needed_of "$src" >> "$queue"
done
if [ -s "$missing" ]; then
    die "sonames not provided by any listed package: $(tr '\n' ' ' < "$missing")(add the package to debian-packages.txt)"
fi
log "$ncopied Debian shared objects copied into ${FFDIR#"$ROOT"/} ($(wc -l < "$seen") sonames reachable, rest pruned)"

# ---------------------------------------------------------------------------
# 9. loaders.cache with the on-disk paths
# ---------------------------------------------------------------------------
CACHEFILE="$FFDIR/pixbuf-loaders/loaders.cache"
query=$(find "$UNPACK/usr/lib/$ARCH-linux-gnu/gdk-pixbuf-2.0" "$UNPACK/usr/bin" -type f -name gdk-pixbuf-query-loaders 2>/dev/null | head -n 1)
if [ -n "$query" ] && host_runs_i386; then
    log "generating loaders.cache with the suite's gdk-pixbuf-query-loaders"
    GDK_PIXBUF_MODULEDIR="$FFDIR/pixbuf-loaders" \
        "$LIBDIR/ld-linux.so.2" --library-path "$LIBDIR:$FFDIR" "$query" > "$CACHEFILE.tmp"
    sed -e "s|$FFDIR/pixbuf-loaders|$DISK_PIXBUF_DIR|g" \
        -e "s|^# LoaderDir = .*|# LoaderDir = $DISK_PIXBUF_DIR|" "$CACHEFILE.tmp" > "$CACHEFILE"
    rm -f "$CACHEFILE.tmp"
elif [ -f "$HERE/loaders.cache.template" ]; then
    warn "host cannot run i386 binaries; using loaders.cache.template (filtered to present loaders)"
    # Blocks are separated by blank lines; keep header + blocks whose module file exists.
    awk -v RS= -v ORS='\n\n' -v dir="$FFDIR/pixbuf-loaders" -v disk="$DISK_PIXBUF_DIR" '
        NR == 1 { print; next }
        { split($0, l, "\n"); f = l[1]; gsub(/"/, "", f); sub("^" disk "/", "", f)
          if (system("[ -f \"" dir "/" f "\" ]") == 0) print }' "$HERE/loaders.cache.template" > "$CACHEFILE"
else
    warn "cannot generate loaders.cache (no i386 execution, no template): only built-in PNG/JPEG will load"
    printf '# GdkPixbuf Image Loader Modules file\n# (empty: built-in loaders only)\n' > "$CACHEFILE"
fi
printf '%s\n' "pixbuf-loaders/loaders.cache" >> "$MANIFEST"
# Module lines are a lone quoted path ending in .so; every one must be on-disk.
nmod=$(grep -c '^"[^"]*\.so"$' "$CACHEFILE" || true)
nbad=$(grep '^"[^"]*\.so"$' "$CACHEFILE" | grep -vc "^\"$DISK_PIXBUF_DIR/" || true)
[ "$nbad" = 0 ] || die "loaders.cache has $nbad module path(s) outside $DISK_PIXBUF_DIR"
log "loaders.cache lists $nmod modules under $DISK_PIXBUF_DIR"

# ---------------------------------------------------------------------------
# 10. Fonts (DejaVu Sans is committed; refetch it only if it went missing)
# ---------------------------------------------------------------------------
if [ ! -f "$FONTDIR/DejaVuSans.ttf" ]; then
    log "DejaVuSans.ttf missing; fetching fonts-dejavu-core"
    fetch_pkg fonts-dejavu-core
    mkdir -p "$FONTDIR"
    cp "$(find "$UNPACK/usr/share/fonts" -name DejaVuSans.ttf | head -n 1)" "$FONTDIR/DejaVuSans.ttf"
fi

# ---------------------------------------------------------------------------
# 11. Verify
# ---------------------------------------------------------------------------
if [ "${SKIP_CHECK:-0}" != 1 ]; then
    sh "$HERE/check-runtime.sh"
    if host_runs_i386; then
        # Firefox locates libxul.so next to /proc/self/exe, which under an
        # explicit ld.so invocation is the loader itself - so run a temporary
        # copy of the loader from inside the Firefox directory.  LD_BIND_NOW
        # forces every symbol reference to resolve up front, so a library with
        # a missing symbol version fails here instead of on the target.
        cp "$LIBDIR/ld-linux.so.2" "$FFDIR/.hostsmoke-ld.so"
        ver=$(LD_BIND_NOW=1 "$FFDIR/.hostsmoke-ld.so" --library-path "$LIBDIR:$FFDIR" "$FFDIR/firefox-bin" --version 2>&1 || true)
        rm -f "$FFDIR/.hostsmoke-ld.so"
        case "$ver" in
            *"Firefox ${FF_VERSION%esr}"*) log "host smoke: LD_BIND_NOW=1 firefox-bin --version -> '$ver'" ;;
            *) warn "host smoke: firefox-bin --version printed '$ver'" ;;
        esac
        for n in $GL_STUBS; do
            [ -f "$FFDIR/$n" ] || continue
            "$LIBDIR/ld-linux.so.2" --preload "$FFDIR/$n" --library-path "$LIBDIR" "$ROOT/testfiles/glibc-hello" >/dev/null 2>&1 \
                || warn "host smoke: glibc's loader rejected stub $n"
        done
        log "host smoke: the $GL_STUBS stubs load under the suite's ld.so"
    fi
fi

log "done: $(du -sh "$FFDIR" | cut -f1) in ${FFDIR#"$ROOT"/}, $(du -sh "$LIBDIR" | cut -f1) in ${LIBDIR#"$ROOT"/} (Debian $SUITE, Firefox $FF_VERSION)"
