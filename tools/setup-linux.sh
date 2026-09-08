#!/bin/sh
# setup-linux.sh — one-shot, idempotent bootstrap of a MaeroOS build host on
# Linux (Debian/Ubuntu).  After it finishes, `make`, `make initrd`, `make disk`,
# `make iso`, `make smoke*` and `make run-firefox` work like they do on macOS.
#
#   tools/setup-linux.sh            # print apt list, build/fetch toolchains, verify
#   tools/setup-linux.sh --apt      # also run `sudo apt-get install ...` first
#   tools/setup-linux.sh --no-sudo  # no root at all: relocate the apt packages
#                                   # into ~/opt/hostpkgs and use musl.cc's
#                                   # native compiler as the host compiler
#   tools/setup-linux.sh --check    # only verify which tools are present/missing
#   tools/setup-linux.sh --dry-run  # print the plan, download/build nothing
#
# What it does:
#   1. prints the exact apt package list (installs it with --apt, via sudo).
#      Without root (--no-sudo, or automatically when required host packages
#      are missing and --apt was not given) it instead lets apt resolve and
#      download the missing packages (`apt-get -s install` + `apt-get download`,
#      which checks every .deb against the signed archive index), unpacks them
#      with dpkg-deb into $HOSTPKGS_DIR (default $HOME/opt/hostpkgs) and writes
#      wrapper scripts into $BIN_DIR (default $HOME/opt/bin) that supply the
#      library path, data directories and flags the relocated tools need
#      (`qemu -L`, `grub-mkrescue -d`, bison's M4, texinfo's perl modules).
#      If the host has no C/C++ compiler either, musl.cc's self-contained
#      x86_64-linux-musl-native toolchain is fetched into $OPT_DIR and exposed
#      as `cc`/`c++` wrappers that link statically (its dynamic loader is not
#      installed on the host);
#   2. builds an i686-elf binutils + gcc (C only, --without-headers, + libgcc)
#      into $PREFIX (default $HOME/opt/cross) with all cores, skipping what is
#      already there.  Without root gmp/mpfr/mpc/isl are built in-tree via
#      gcc's contrib/download_prerequisites (sha512-verified by that script);
#   3. downloads musl.cc's i686-linux-musl-cross.tgz into ports/ (where the
#      Docker recipes expect it) and extracts it under $OPT_DIR (default
#      $HOME/opt) so i686-linux-musl-gcc is usable natively as well;
#   4. prints the `export PATH=...` line you need and checks every tool.
#
# Options:
#   --apt         run `sudo apt-get update && sudo apt-get install -y <list>`
#   --optional    with --apt, also install the optional packages (docker.io,
#                 gcc-multilib, gdb-multiarch)
#   --no-sudo     never use root: relocate apt packages + musl.cc host compiler
#                 (auto-selected when host packages are missing and --apt is
#                 not given)
#   --check       verify tools and exit (nothing is built or downloaded)
#   --dry-run     print the plan and exit (nothing is built or downloaded)
#   --no-cross    skip the i686-elf toolchain step
#   --no-musl     skip the musl.cc toolchain step
#   --jobs N      parallel make jobs (default: nproc)
#   -h, --help    this text
#
# Environment overrides:
#   PREFIX        i686-elf install prefix        (default $HOME/opt/cross)
#   OPT_DIR       where the musl trees are unpacked (default $HOME/opt)
#   BIN_DIR       wrapper scripts (no-sudo)      (default $OPT_DIR/bin)
#   HOSTPKGS_DIR  relocated apt packages (no-sudo) (default $OPT_DIR/hostpkgs)
#   SRC_DIR       tarballs + build dirs          (default $PREFIX/src)
#   BINUTILS_VER  binutils release               (default 2.44)
#   GCC_VER       gcc release                    (default 14.2.0)
#   GNU_MIRROR    base URL for GNU tarballs      (default https://ftp.gnu.org/gnu, TLS)
#   MUSL_URL      musl.cc i686-linux-musl-cross tarball URL
#   NATIVE_URL    musl.cc x86_64-linux-musl-native tarball URL
#   JOBS          same as --jobs
#   BINUTILS_SHA256, GCC_SHA256, MUSL_SHA512, NATIVE_SHA512
#                 expected digests of the downloads.  The defaults are pinned
#                 for the default versions; if you change BINUTILS_VER/GCC_VER
#                 you must supply the matching digest (or set MAEROS_SKIP_HASH=1
#                 to accept an unverified download).
#   MAEROS_SKIP_HASH=1  skip digest verification (not recommended)

set -eu

# ── Defaults ──────────────────────────────────────────────────────────────────
REPO_ROOT=$(cd "$(dirname "$0")/.." && pwd)
PREFIX="${PREFIX:-$HOME/opt/cross}"
OPT_DIR="${OPT_DIR:-$HOME/opt}"
BIN_DIR="${BIN_DIR:-$OPT_DIR/bin}"
HOSTPKGS_DIR="${HOSTPKGS_DIR:-$OPT_DIR/hostpkgs}"
SRC_DIR="${SRC_DIR:-$PREFIX/src}"
DEFAULT_BINUTILS_VER=2.44
DEFAULT_GCC_VER=14.2.0
BINUTILS_VER="${BINUTILS_VER:-$DEFAULT_BINUTILS_VER}"
GCC_VER="${GCC_VER:-$DEFAULT_GCC_VER}"
# ftp.gnu.org serves over TLS; ftpmirror.gnu.org may redirect to plain HTTP.
GNU_MIRROR="${GNU_MIRROR:-https://ftp.gnu.org/gnu}"
MUSL_URL="${MUSL_URL:-https://musl.cc/i686-linux-musl-cross.tgz}"
NATIVE_URL="${NATIVE_URL:-https://musl.cc/x86_64-linux-musl-native.tgz}"

# Pinned digests of the artifacts the default configuration downloads.
# binutils/gcc: sha256 of the .tar.xz fetched from https://ftp.gnu.org/gnu
# (gcc's matches https://gcc.gnu.org/pub/gcc/releases/gcc-14.2.0/sha512.sum).
# musl/native: the entries from https://musl.cc/SHA512SUMS.  musl.cc
# republishes those tarballs when it rebuilds, so a mismatch there means
# "check SHA512SUMS and update MUSL_SHA512/NATIVE_SHA512", not necessarily
# tampering.
if [ "$BINUTILS_VER" = "$DEFAULT_BINUTILS_VER" ]; then
    BINUTILS_SHA256="${BINUTILS_SHA256:-ce2017e059d63e67ddb9240e9d4ec49c2893605035cd60e92ad53177f4377237}"
else
    BINUTILS_SHA256="${BINUTILS_SHA256:-}"
fi
if [ "$GCC_VER" = "$DEFAULT_GCC_VER" ]; then
    GCC_SHA256="${GCC_SHA256:-a7b39bc69cbf9e25826c5a60ab26477001f7c08d85cec04bc0e29cabed6f3cc9}"
else
    GCC_SHA256="${GCC_SHA256:-}"
fi
MUSL_SHA512="${MUSL_SHA512:-5047afc68170a2910895db2dfa448227e71a984bfa2130a1bc946fd1015d722b80b15e4abf90c64300815aa84fe781cc8b8a72f10174f9dce96169e035911880}"
NATIVE_SHA512="${NATIVE_SHA512:-44d441ad9aa11a06feddf3daa4c9f53ad7d9ca37af1f5a61379aca07793703d179410cea723c1b7fca94c4de19a321228bdb3656bc5cbdb5e3bea8e2d6dac6c7}"
MAEROS_SKIP_HASH="${MAEROS_SKIP_HASH:-0}"
MUSL_TGZ="$REPO_ROOT/ports/i686-linux-musl-cross.tgz"
MUSL_DIR="$OPT_DIR/i686-linux-musl-cross"
NATIVE_TGZ="$SRC_DIR/x86_64-linux-musl-native.tgz"
NATIVE_DIR="$OPT_DIR/x86_64-linux-musl-native"
TARGET=i686-elf
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"

MODE=build          # build | check | dry-run
DO_APT=0
APT_OPTIONAL=0
DO_CROSS=1
DO_MUSL=1
NOSUDO=auto         # auto | 0 | 1
USE_NATIVE=0        # 1: host compiler is the musl.cc native toolchain (cc/c++ wrappers)
INTREE_PREREQS=0    # 1: gmp/mpfr/mpc/isl built inside the gcc tree

# Required apt packages: GCC cross build deps, assembler, emulator, ISO and
# disk tooling, script runtimes.
APT_REQUIRED="build-essential bison flex texinfo libgmp-dev libmpfr-dev libmpc-dev libisl-dev
nasm qemu-system-x86 qemu-system-gui grub-pc-bin grub-common xorriso mtools e2fsprogs
python3 wget xz-utils zstd librsvg2-bin"
# Optional: docker.io only for rebuilding ports/, gcc-multilib for
# ports/firefox/build-glstubs.sh without Docker, gdb-multiarch for `make gdb`.
APT_OPTIONAL_PKGS="docker.io gcc-multilib gdb-multiarch"

# No-sudo mode: the packages apt is asked to resolve.  Whatever is already
# installed system-wide is skipped by apt itself; only the missing ones (and
# their missing dependencies, e.g. qemu's libslirp/libfdt/seabios) get
# downloaded and unpacked.  The compiler is not in this list: gcc's debs are
# not relocatable, musl.cc's native toolchain is (see nosudo_native).
# The shellcheck linter (for tools/*.sh) is included because it is tiny and static.
NOSUDO_PKGS="make nasm bison flex m4 texinfo qemu-system-x86 qemu-system-gui grub-pc-bin
mtools xorriso e2fsprogs xz-utils zstd shellcheck"
# Tools that get a wrapper in $BIN_DIR when the relocated tree provides them.
NOSUDO_WRAP="make nasm bison flex m4 makeinfo texi2any qemu-system-i386 qemu-system-x86_64
grub-mkrescue grub-mkimage grub-file mformat mcopy mmd mdir mdel mtype mattrib minfo mlabel
xorriso mke2fs mkfs.ext2 debugfs xz zstd shellcheck"

usage() { sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'; }

while [ $# -gt 0 ]; do
    case "$1" in
    --apt)      DO_APT=1 ;;
    --optional) APT_OPTIONAL=1 ;;
    --no-sudo)  NOSUDO=1 ;;
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
if [ "$DO_APT" = 1 ] && [ "$NOSUDO" = 1 ]; then
    echo "setup-linux: --apt and --no-sudo contradict each other" >&2; exit 2
fi
[ "$DO_APT" = 1 ] && NOSUDO=0

# ── Helpers ───────────────────────────────────────────────────────────────────
say()  { printf '%s\n' "$*"; }
step() { printf '\n== %s\n' "$*"; }
die()  { printf 'setup-linux: error: %s\n' "$*" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }

# The PATH the verification and the gcc build use: our prefixes first, and
# the sbin dirs because Debian keeps mke2fs/debugfs there.
BUILD_PATH="$BIN_DIR:$PREFIX/bin:$MUSL_DIR/bin:$PATH:/usr/sbin:/sbin"
# The host's own PATH (no wrappers): what apt/no-sudo auto-detection looks at.
HOST_PATH="$PATH:/usr/sbin:/sbin"
host_has() { PATH="$HOST_PATH" command -v "$1" >/dev/null 2>&1; }

# fetch URL DEST — resumable download with wget (or curl), atomic on success.
fetch() {
    url="$1"; dest="$2"
    [ -f "$dest" ] && return 0
    mkdir -p "$(dirname "$dest")"
    say "   fetching $url"
    if have wget; then
        # A bar on a terminal, one dot per MiB when logging to a file/pipe.
        if [ -t 1 ]; then progress=bar:force:noscroll; else progress=dot:mega; fi
        wget -q --show-progress --progress="$progress" -c -O "$dest.part" "$url" || { rm -f "$dest.part"; die "download failed: $url"; }
    elif have curl; then
        curl -fL -C - -o "$dest.part" "$url" || { rm -f "$dest.part"; die "download failed: $url"; }
    else
        die "need wget or curl to download $url"
    fi
    mv "$dest.part" "$dest"
}

# verify_hash FILE ALGO EXPECTED VAR — compare the sha256/sha512 of FILE with
# EXPECTED.  Fails closed: a mismatch moves the file aside as FILE.bad and
# stops; an empty EXPECTED (version changed without a digest) also stops.
# MAEROS_SKIP_HASH=1 turns both into a warning.  VAR names the override.
verify_hash() {
    file="$1"; algo="$2"; expected="$3"; var="$4"
    if [ "$MAEROS_SKIP_HASH" = 1 ]; then
        say "   WARNING: MAEROS_SKIP_HASH=1, not verifying $file"
        return 0
    fi
    [ -n "$expected" ] || die "no pinned $algo digest for $file
  set $var=<digest> (or MAEROS_SKIP_HASH=1 to accept an unverified download)"
    have "${algo}sum" || die "${algo}sum not found (coreutils); cannot verify $file"
    actual=$("${algo}sum" "$file" | cut -d' ' -f1)
    if [ "$actual" != "$expected" ]; then
        mv -f "$file" "$file.bad"
        die "$algo mismatch for $file (moved to $file.bad)
  expected $expected
  actual   $actual
  The download is not the pinned artifact.  Check the upstream checksum
  (GNU: .sig next to the tarball; musl: https://musl.cc/SHA512SUMS) and set
  $var= accordingly, or MAEROS_SKIP_HASH=1 to accept it anyway."
    fi
    say "   $algo OK: $file"
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
musl_ok()   { [ -x "$MUSL_DIR/bin/i686-linux-musl-gcc" ]; }
native_ok() { [ -x "$NATIVE_DIR/bin/gcc" ] && [ -x "$NATIVE_DIR/bin/g++" ]; }
# A host C and C++ compiler that is not one of our wrappers.
host_compiler_ok() {
    { host_has gcc && host_has g++; } || { host_has cc && host_has c++; }
}
grub_modules_dir() {
    for d in /usr/lib/grub/i386-pc "$HOSTPKGS_DIR/usr/lib/grub/i386-pc"; do
        [ -d "$d" ] && { echo "$d"; return 0; }
    done
    return 1
}

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

# ── No-sudo mode: which host tools are missing? ───────────────────────────────
# Prints the names of the required host tools apt would normally provide that
# are not on the host's own PATH (wrappers from an earlier no-sudo run do not
# count; a relocated grub tree does, since it cannot be installed any other way).
host_gaps() {
    gaps=""
    host_compiler_ok || gaps="$gaps cc/c++"
    for t in make nasm qemu-system-i386 grub-mkrescue xorriso mformat mke2fs debugfs bison flex makeinfo m4; do
        host_has "$t" || gaps="$gaps $t"
    done
    grub_modules_dir >/dev/null || gaps="$gaps grub-i386-pc-modules"
    printf '%s' "${gaps# }"
}

# apt without root.  APT_OPTS is empty while the system's own package lists
# are used; if a download fails because they are stale, apt_private_update
# refreshes a private copy of the lists under $HOSTPKGS_DIR/apt and the
# remaining apt calls use that.  `apt-get download` fetches from the
# configured mirror and checks each .deb against the SHA256 in the signed
# Packages index, so no extra pinning is needed for the debs.
APT_OPTS=""
apt_cmd() {
    # shellcheck disable=SC2086
    apt-get $APT_OPTS "$@"
}
apt_private_update() {
    mkdir -p "$HOSTPKGS_DIR/apt/lists/partial" "$HOSTPKGS_DIR/apt/cache/archives/partial"
    APT_OPTS="-o Dir::State::Lists=$HOSTPKGS_DIR/apt/lists -o Dir::Cache=$HOSTPKGS_DIR/apt/cache"
    say "   refreshing a private copy of the package lists in $HOSTPKGS_DIR/apt"
    apt_cmd update >"$HOSTPKGS_DIR/apt/update.log" 2>&1 || say "   WARNING: apt-get update reported errors (see $HOSTPKGS_DIR/apt/update.log), continuing with what it fetched"
}
# hostpkgs_resolve — "pkg version" per line: what apt would install (new or
# upgraded) for $NOSUDO_PKGS on this host.  Output of a simulated install.
hostpkgs_resolve() {
    # shellcheck disable=SC2046,SC2086
    out=$(apt_cmd -s -q --no-install-recommends install $(printf '%s' "$NOSUDO_PKGS" | tr '\n' ' ') 2>&1) \
        || die "apt-get cannot resolve the host packages:
$out"
    printf '%s\n' "$out" | sed -n 's/^Inst \([^ ]*\) \(\[[^]]*\] \)\{0,1\}(\([^ ]*\) .*/\1 \3/p'
}
# hostpkgs_fetch PKG VER — download PKG=VER into $HOSTPKGS_DIR/debs (once);
# echoes the .deb path.  Returns 1 if apt could not download it.
hostpkgs_fetch() {
    pkg="$1"; ver="$2"
    debdir="$HOSTPKGS_DIR/debs"
    mkdir -p "$debdir"
    # apt names the file pkg_version_arch.deb with ':' escaped as %3a.
    enc=$(printf '%s' "$ver" | sed 's/:/%3a/g')
    for f in "$debdir/${pkg}_${enc}_"*.deb; do
        if [ -f "$f" ] && dpkg-deb --info "$f" >/dev/null 2>&1; then echo "$f"; return 0; fi
        rm -f "$f"
    done
    rm -f "$debdir/${pkg}_"*.deb
    ( cd "$debdir" && apt_cmd -q download "$pkg=$ver" ) >"$debdir/$pkg.download.log" 2>&1 || return 1
    for f in "$debdir/${pkg}_"*.deb; do
        if [ -f "$f" ] && dpkg-deb --info "$f" >/dev/null 2>&1; then echo "$f"; return 0; fi
    done
    return 1
}

# ── No-sudo mode: steps ───────────────────────────────────────────────────────
nosudo_hostpkgs() {
    step "1. host packages without root -> $HOSTPKGS_DIR"
    have apt-get || die "apt-get not found; --no-sudo only knows how to relocate Debian/Ubuntu packages"
    have dpkg-deb || die "dpkg-deb not found (dpkg); cannot unpack packages"
    mkdir -p "$HOSTPKGS_DIR/.done"
    attempt=1
    while :; do
        list=$(hostpkgs_resolve)
        if [ -z "$list" ]; then
            say "   apt reports nothing to add: all of ($(printf '%s' "$NOSUDO_PKGS" | tr '\n' ' ')) already installed"
            return 0
        fi
        failed=""
        old_ifs=$IFS; IFS='
'
        for line in $list; do
            IFS=$old_ifs
            pkg=${line% *}; ver=${line#* }
            marker="$HOSTPKGS_DIR/.done/${pkg}_${ver}"
            if [ -f "$marker" ]; then
                say "   $pkg $ver: already unpacked"
                continue
            fi
            if deb=$(hostpkgs_fetch "$pkg" "$ver"); then
                say "   $pkg $ver: unpacking $(basename "$deb")"
                dpkg-deb -x "$deb" "$HOSTPKGS_DIR" || die "dpkg-deb -x failed for $deb"
                rm -f "$HOSTPKGS_DIR/.done/${pkg}_"*
                : >"$marker"
            else
                say "   $pkg $ver: download failed ($(tail -n 1 "$HOSTPKGS_DIR/debs/$pkg.download.log" 2>/dev/null))"
                failed="$failed $pkg"
            fi
        done
        IFS=$old_ifs
        [ -n "$failed" ] || break
        [ "$attempt" = 1 ] || die "could not download:$failed (see $HOSTPKGS_DIR/debs/*.download.log)"
        say "   the system's package lists may be stale; retrying with fresh lists"
        apt_private_update
        attempt=2
    done
}

nosudo_native() {
    step "1b. host C/C++ compiler"
    if host_compiler_ok && ! native_ok; then
        say "   host has a C/C++ compiler, not fetching musl.cc's native toolchain"
        return 0
    fi
    USE_NATIVE=1
    if native_ok; then
        say "   $NATIVE_DIR present, skipping"
    else
        fetch "$NATIVE_URL" "$NATIVE_TGZ"
        verify_hash "$NATIVE_TGZ" sha512 "$NATIVE_SHA512" NATIVE_SHA512
        gzip -t "$NATIVE_TGZ" 2>/dev/null || { rm -f "$NATIVE_TGZ"; die "corrupt download $NATIVE_TGZ (removed; re-run)"; }
        mkdir -p "$OPT_DIR"
        run_logged "$OPT_DIR/native-extract.log" tar -xzf "$NATIVE_TGZ" -C "$OPT_DIR"
        native_ok || die "extracting $NATIVE_TGZ did not produce $NATIVE_DIR/bin/gcc"
        say "   extracted to $NATIVE_DIR ($("$NATIVE_DIR/bin/gcc" --version | head -n 1))"
    fi
}

# write_wrapper NAME PRELUDE EXTRA_ARGS — $BIN_DIR/NAME that execs the
# relocated $HOSTPKGS_DIR binary of the same name, with LD_LIBRARY_PATH set
# only when the binary needs libraries the host does not have.
write_wrapper() {
    name="$1"; prelude="$2"; extra="$3"
    real=""
    for d in usr/bin usr/sbin bin sbin; do
        [ -x "$HOSTPKGS_DIR/$d/$name" ] && { real="$HOSTPKGS_DIR/$d/$name"; break; }
    done
    [ -n "$real" ] || return 0
    libs=""
    if ldd "$real" 2>/dev/null | grep -q 'not found'; then
        libs="export LD_LIBRARY_PATH=\"$HOSTPKGS_LIBS\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}\""
    fi
    {
        say "#!/bin/sh"
        say "# Generated by tools/setup-linux.sh (no-sudo mode): $name from $HOSTPKGS_DIR"
        [ -n "$libs" ] && say "$libs"
        [ -n "$prelude" ] && say "$prelude"
        say "exec \"$real\" $extra\"\$@\""
    } >"$BIN_DIR/$name.tmp"
    chmod +x "$BIN_DIR/$name.tmp"
    mv -f "$BIN_DIR/$name.tmp" "$BIN_DIR/$name"
    WRAPPED="$WRAPPED $name"
}

nosudo_wrappers() {
    step "1c. wrapper scripts -> $BIN_DIR"
    mkdir -p "$BIN_DIR"
    WRAPPED=""
    HOSTPKGS_LIBS=""
    for d in "$HOSTPKGS_DIR"/usr/lib/*-linux-gnu* "$HOSTPKGS_DIR"/lib/*-linux-gnu* "$HOSTPKGS_DIR/usr/lib" "$HOSTPKGS_DIR/lib"; do
        [ -d "$d" ] && HOSTPKGS_LIBS="${HOSTPKGS_LIBS:+$HOSTPKGS_LIBS:}$d"
    done
    # Per-tool knobs the relocated binaries need.
    qemu_l=""
    for d in "$HOSTPKGS_DIR/usr/share/qemu" "$HOSTPKGS_DIR/usr/share/seabios" "$HOSTPKGS_DIR/usr/lib/ipxe/qemu"; do
        [ -d "$d" ] && qemu_l="$qemu_l-L \"$d\" "
    done
    qemu_env=""
    for d in "$HOSTPKGS_DIR"/usr/lib/*-linux-gnu*/qemu; do
        [ -d "$d" ] && qemu_env="export QEMU_MODULE_DIR=\"$d\""
    done
    grub_d=""
    [ -d "$HOSTPKGS_DIR/usr/lib/grub/i386-pc" ] && grub_d="-d \"$HOSTPKGS_DIR/usr/lib/grub/i386-pc\" "
    m4_env=""
    [ -x "$HOSTPKGS_DIR/usr/bin/m4" ] && m4_env="export M4=\"$HOSTPKGS_DIR/usr/bin/m4\""
    bison_env="$m4_env"
    [ -d "$HOSTPKGS_DIR/usr/share/bison" ] && bison_env="$bison_env
export BISON_PKGDATADIR=\"$HOSTPKGS_DIR/usr/share/bison\""
    perl_env=""
    perl_libs=""
    for d in "$HOSTPKGS_DIR/usr/share/perl5" "$HOSTPKGS_DIR"/usr/lib/*-linux-gnu*/perl5/*; do
        [ -d "$d" ] && perl_libs="${perl_libs:+$perl_libs:}$d"
    done
    [ -n "$perl_libs" ] && perl_env="export PERL5LIB=\"$perl_libs\${PERL5LIB:+:\$PERL5LIB}\""

    for name in $(printf '%s' "$NOSUDO_WRAP" | tr '\n' ' '); do
        case "$name" in
        qemu-system-*)      write_wrapper "$name" "$qemu_env" "$qemu_l" ;;
        grub-mkrescue)      write_wrapper "$name" "" "$grub_d" ;;
        bison)              write_wrapper "$name" "$bison_env" "" ;;
        flex)               write_wrapper "$name" "$m4_env" "" ;;
        makeinfo|texi2any)  write_wrapper "$name" "$perl_env" "" ;;
        *)                  write_wrapper "$name" "" "" ;;
        esac
    done

    if [ "$USE_NATIVE" = 1 ]; then
        for pair in cc:gcc c++:g++; do
            wname=${pair%:*}; tool=${pair#*:}
            cat >"$BIN_DIR/$wname.tmp" <<WRAP
#!/bin/sh
# Generated by tools/setup-linux.sh (no-sudo mode): host $wname is musl.cc's
# x86_64-linux-musl-native $tool.  Its dynamic loader /lib/ld-musl-x86_64.so.1 is
# not installed on this host, so executables are linked statically unless this
# is a compile-only, preprocess-only or -shared invocation.
static=-static
for a in "\$@"; do
    case "\$a" in -c|-S|-E|-M|-MM|-shared|-static|-r) static= ;; esac
done
exec "$NATIVE_DIR/bin/$tool" "\$@" \$static
WRAP
            chmod +x "$BIN_DIR/$wname.tmp"
            mv -f "$BIN_DIR/$wname.tmp" "$BIN_DIR/$wname"
            WRAPPED="$WRAPPED $wname"
        done
    fi
    say "   wrote:$WRAPPED"
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
    step "Tool check (PATH includes $BIN_DIR, $PREFIX/bin and $MUSL_DIR/bin)"
    check_tool "$TARGET-gcc"     1 "built by this script into $PREFIX"
    check_tool "$TARGET-ar"      1 "built by this script into $PREFIX"
    check_tool "$TARGET-ld"      1 "built by this script into $PREFIX"
    check_tool "$TARGET-strip"   1 "built by this script into $PREFIX"
    check_tool cc                1 "apt: build-essential, or --no-sudo (musl.cc native gcc wrapper; toybox HOSTCC)"
    check_tool make              1 "apt: build-essential, or --no-sudo"
    check_tool nasm              1 "apt: nasm, or --no-sudo"
    check_tool qemu-system-i386  1 "apt: qemu-system-x86, or --no-sudo"
    check_tool grub-mkrescue     1 "apt: grub-common, or --no-sudo"
    check_tool xorriso           1 "apt: xorriso, or --no-sudo"
    check_tool mformat           1 "apt: mtools, or --no-sudo"
    check_tool mke2fs            1 "apt: e2fsprogs, or --no-sudo"
    check_tool debugfs           1 "apt: e2fsprogs, or --no-sudo"
    check_tool python3           1 "apt: python3"
    check_tool tar               1 "apt: tar"
    check_tool sed               1 "apt: sed"
    check_tool wget              1 "apt: wget"
    check_tool xz                1 "apt: xz-utils, or --no-sudo"
    check_tool zstd              1 "apt: zstd, or --no-sudo"
    check_tool i686-linux-musl-gcc 0 "fetched by this script into $MUSL_DIR (ports/ without Docker)"
    check_tool rsvg-convert      0 "apt: librsvg2-bin (make icons)"
    check_tool docker            0 "apt: docker.io (rebuilding ports/)"
    check_tool gdb-multiarch     0 "apt: gdb-multiarch (make gdb)"
    check_tool lsof              0 "apt: lsof (ss/fuser are used otherwise)"
    check_tool xrandr            0 "apt: x11-xserver-utils (screen-size auto-fit)"
    check_tool shellcheck        0 "apt: shellcheck, or --no-sudo (lint tools/*.sh)"

    if gdir=$(grub_modules_dir); then
        printf '  [ ok ]   %-24s %s\n' "grub i386-pc modules" "$gdir"
    else
        printf '  [MISSING] %-23s %s\n' "grub i386-pc modules" "apt: grub-pc-bin, or --no-sudo (grub-mkrescue needs them for a BIOS ISO)"
        MISSING_REQUIRED=1
    fi
    if qemu=$(PATH="$BUILD_PATH" command -v qemu-system-i386 2>/dev/null); then
        if "$qemu" -display help 2>/dev/null | grep -qxE 'gtk|sdl'; then
            printf '  [ ok ]   %-24s %s\n' "qemu gui display" "$("$qemu" -display help 2>/dev/null | grep -xE 'gtk|sdl' | head -n 1)"
        else
            printf '  [ -- ]   %-24s optional: %s\n' "qemu gui display" "apt: qemu-system-gui (make start needs a window)"
        fi
    fi
    if PATH="$BUILD_PATH" command -v gcc >/dev/null 2>&1; then
        if printf 'int main(void){return 0;}' | PATH="$BUILD_PATH" gcc -m32 -x c - -o /dev/null 2>/dev/null; then
            printf '  [ ok ]   %-24s %s\n' "gcc -m32" "works"
        else
            printf '  [ -- ]   %-24s optional: %s\n' "gcc -m32" "apt: gcc-multilib (build-glstubs.sh without Docker)"
        fi
    fi
}

env_path() {
    if [ "$NOSUDO" = 1 ] || [ -d "$BIN_DIR" ]; then
        printf '%s' "$BIN_DIR:$PREFIX/bin:$MUSL_DIR/bin"
    else
        printf '%s' "$PREFIX/bin:$MUSL_DIR/bin"
    fi
}
print_path_line() {
    step "Add this to your shell (e.g. ~/.bashrc), then open a new shell:"
    say ""
    say "    export PATH=\"$(env_path):\$PATH\""
    say ""
    say "  or:  . $PREFIX/maeros-env.sh"
}

# ── Decide sudo vs. no-sudo ───────────────────────────────────────────────────
GAPS=$(host_gaps)
if [ "$NOSUDO" = auto ]; then
    if [ -d "$HOSTPKGS_DIR/.done" ]; then
        NOSUDO=1        # an earlier run relocated packages here: stay consistent
    elif [ -n "$GAPS" ] && [ "$MODE" != check ]; then
        NOSUDO=1
    else
        NOSUDO=0
    fi
fi
if [ "$NOSUDO" = 1 ]; then
    INTREE_PREREQS=1
    if native_ok || ! host_compiler_ok; then USE_NATIVE=1; fi
fi

# ── Plan / check modes ────────────────────────────────────────────────────────
step "MaeroOS Linux host setup"
say "  repo      : $REPO_ROOT"
say "  PREFIX    : $PREFIX   (i686-elf binutils $BINUTILS_VER + gcc $GCC_VER)"
say "  OPT_DIR   : $OPT_DIR  (musl.cc i686-linux-musl-cross)"
say "  SRC_DIR   : $SRC_DIR"
say "  JOBS      : $JOBS"
say "  mode      : $MODE"
if [ "$NOSUDO" = 1 ]; then
    say "  root      : none (no-sudo): apt packages -> $HOSTPKGS_DIR, wrappers -> $BIN_DIR"
    [ "$USE_NATIVE" = 1 ] && say "  host cc   : musl.cc x86_64-linux-musl-native -> $NATIVE_DIR (static link)"
    [ -n "$GAPS" ] && [ ! -d "$HOSTPKGS_DIR/.done" ] && say "  missing   : $GAPS (--apt would install them with sudo instead)"
fi

if [ "$MODE" = check ]; then
    verify
    [ "$MISSING_REQUIRED" = 0 ] || { print_path_line; die "required tools are missing (see above)"; }
    print_path_line
    say ""
    say "All required tools present."
    exit 0
fi

if [ "$MODE" = dry-run ]; then
    if [ "$NOSUDO" = 1 ]; then
        step "1. host packages without root -> $HOSTPKGS_DIR"
        if have apt-get; then
            list=$(hostpkgs_resolve)
            if [ -z "$list" ]; then say "   nothing to fetch: apt reports every package installed"; else
                say "   apt-get download + dpkg-deb -x (verified by apt against the signed index):"
                printf '%s\n' "$list" | sed 's/^/     /'
            fi
        else
            say "   apt-get not found: --no-sudo needs a Debian/Ubuntu host"
        fi
        if [ "$USE_NATIVE" = 1 ]; then
            if native_ok; then say "   host compiler: $NATIVE_DIR present"; else
                say "   host compiler: fetch $NATIVE_URL -> $NATIVE_TGZ, extract into $OPT_DIR"
                say "     sha512 $NATIVE_SHA512"; fi
        else
            say "   host compiler: the host's own gcc/g++"
        fi
        say "   wrappers: $BIN_DIR/{$(printf '%s' "$NOSUDO_WRAP" | tr '\n' ' ' | tr -s ' ' ',')} (those present)$( [ "$USE_NATIVE" = 1 ] && printf ', cc, c++')"
    else
        step "1. apt packages"
        apt_lines
    fi
    step "2. i686-elf cross toolchain -> $PREFIX"
    if [ "$DO_CROSS" = 0 ]; then say "   skipped (--no-cross)"; else
        if cross_binutils_ok; then say "   binutils: already installed, skip"; else
            say "   binutils $BINUTILS_VER: fetch $GNU_MIRROR/binutils/binutils-$BINUTILS_VER.tar.xz, build in $SRC_DIR/build-binutils, install"
            say "     sha256 ${BINUTILS_SHA256:-<none: set BINUTILS_SHA256 or MAEROS_SKIP_HASH=1>}"; fi
        if cross_gcc_ok; then say "   gcc: already installed, skip"; else
            say "   gcc $GCC_VER: fetch $GNU_MIRROR/gcc/gcc-$GCC_VER/gcc-$GCC_VER.tar.xz, build all-gcc all-target-libgcc in $SRC_DIR/build-gcc, install"
            say "     sha256 ${GCC_SHA256:-<none: set GCC_SHA256 or MAEROS_SKIP_HASH=1>}"
            [ "$INTREE_PREREQS" = 1 ] && say "     gmp/mpfr/mpc/isl: gcc-$GCC_VER/contrib/download_prerequisites --directory=$SRC_DIR (sha512-verified by that script)"; fi
    fi
    step "3. musl.cc toolchain -> $MUSL_TGZ, $MUSL_DIR"
    if [ "$DO_MUSL" = 0 ]; then say "   skipped (--no-musl)"; else
        if [ -f "$MUSL_TGZ" ]; then say "   tarball present, skip download"; else say "   fetch $MUSL_URL"; fi
        say "     sha512 $MUSL_SHA512"
        if musl_ok; then say "   already extracted, skip"; else say "   extract into $OPT_DIR"; fi
    fi
    step "4. verify"
    say "   run: $0 --check"
    print_path_line
    exit 0
fi

# ── Build mode ────────────────────────────────────────────────────────────────
if [ "$NOSUDO" = 1 ]; then
    nosudo_hostpkgs
    nosudo_native
    nosudo_wrappers
else
    step "1. apt packages"
    apt_lines
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
        say "(not installing: re-run with --apt to install them, or --no-sudo to relocate them under $HOSTPKGS_DIR)"
    fi
fi

# Host prerequisites for building the cross toolchain.
if [ "$DO_CROSS" = 1 ] && ! { cross_binutils_ok && cross_gcc_ok; }; then
    missing=""
    if [ "$USE_NATIVE" = 1 ]; then ctools="cc c++"; else ctools="gcc g++"; fi
    for t in $ctools make tar xz bison flex makeinfo m4; do
        PATH="$BUILD_PATH" command -v "$t" >/dev/null 2>&1 || missing="$missing $t"
    done
    have wget || have curl || missing="$missing wget"
    if [ "$INTREE_PREREQS" = 0 ]; then
        for h in gmp.h mpfr.h mpc.h; do
            [ -f "/usr/include/$h" ] || [ -f "/usr/include/x86_64-linux-gnu/$h" ] || missing="$missing $h"
        done
    fi
    [ -z "$missing" ] || die "cannot build the cross toolchain, host is missing:$missing
  install the packages above (tools/setup-linux.sh --apt), or use --no-sudo, and re-run"
fi

if [ "$DO_CROSS" = 1 ]; then
    step "2. i686-elf cross toolchain -> $PREFIX"
    mkdir -p "$PREFIX" "$SRC_DIR"
    export PATH="$BUILD_PATH"
    # With the musl.cc native compiler every host binary is linked statically
    # (see the cc wrapper).  A static ld cannot dlopen, so binutils gets no
    # plugin support and gcc builds neither LTO (liblto_plugin.so, which
    # collect2 would otherwise hand to ld) nor libcc1.
    if [ "$USE_NATIVE" = 1 ]; then
        export CC=cc CXX=c++
        binutils_extra="--disable-plugins"
        gcc_extra="--disable-plugin --disable-lto"
    else
        binutils_extra=""
        gcc_extra=""
    fi

    if cross_binutils_ok; then
        say "   binutils: $PREFIX/bin/$TARGET-ld present, skipping"
    else
        say "   binutils $BINUTILS_VER"
        tarball="$SRC_DIR/binutils-$BINUTILS_VER.tar.xz"
        fetch "$GNU_MIRROR/binutils/binutils-$BINUTILS_VER.tar.xz" "$tarball"
        verify_hash "$tarball" sha256 "$BINUTILS_SHA256" BINUTILS_SHA256
        xz -t "$tarball" || { rm -f "$tarball"; die "corrupt download $tarball (removed; re-run)"; }
        [ -d "$SRC_DIR/binutils-$BINUTILS_VER" ] || run_logged "$SRC_DIR/binutils-extract.log" tar -xJf "$tarball" -C "$SRC_DIR"
        rm -rf "$SRC_DIR/build-binutils"
        mkdir -p "$SRC_DIR/build-binutils"
        cd "$SRC_DIR/build-binutils"
        # shellcheck disable=SC2086
        run_logged "$SRC_DIR/binutils-configure.log" "$SRC_DIR/binutils-$BINUTILS_VER/configure" \
            --target="$TARGET" --prefix="$PREFIX" --with-sysroot --disable-nls --disable-werror $binutils_extra
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
        verify_hash "$tarball" sha256 "$GCC_SHA256" GCC_SHA256
        xz -t "$tarball" || { rm -f "$tarball"; die "corrupt download $tarball (removed; re-run)"; }
        [ -d "$SRC_DIR/gcc-$GCC_VER" ] || run_logged "$SRC_DIR/gcc-extract.log" tar -xJf "$tarball" -C "$SRC_DIR"
        if [ "$INTREE_PREREQS" = 1 ]; then
            # gmp/mpfr/mpc/isl as in-tree subdirectories: the host has no -dev
            # packages (or, with the musl compiler, none it could link).  The
            # script verifies each tarball against contrib/prerequisites.sha512.
            say "   gmp/mpfr/mpc/isl in-tree (contrib/download_prerequisites)"
            cd "$SRC_DIR/gcc-$GCC_VER"
            run_logged "$SRC_DIR/gcc-prerequisites.log" ./contrib/download_prerequisites --directory="$SRC_DIR"
            cd "$REPO_ROOT"
        fi
        rm -rf "$SRC_DIR/build-gcc"
        mkdir -p "$SRC_DIR/build-gcc"
        cd "$SRC_DIR/build-gcc"
        # shellcheck disable=SC2086
        run_logged "$SRC_DIR/gcc-configure.log" "$SRC_DIR/gcc-$GCC_VER/configure" \
            --target="$TARGET" --prefix="$PREFIX" --disable-nls --disable-werror \
            --enable-languages=c --without-headers \
            --disable-shared --disable-threads --disable-libssp --disable-libquadmath $gcc_extra
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
    if musl_ok; then
        say "   $MUSL_DIR present, skipping extract"
    else
        verify_hash "$MUSL_TGZ" sha512 "$MUSL_SHA512" MUSL_SHA512
        gzip -t "$MUSL_TGZ" 2>/dev/null || { rm -f "$MUSL_TGZ"; die "corrupt download $MUSL_TGZ (removed; re-run)"; }
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
export PATH="$(env_path):\$PATH"
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
