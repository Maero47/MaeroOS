# ports/firefox — the prebuilt Firefox runtime tree

MaeroOS runs the **official Firefox 115.15.0 ESR linux-i686 build** unmodified,
on top of a glibc GTK3 stack taken from Debian i386 packages.  Nothing here is
compiled from source: `fetch-runtime.sh` downloads, verifies and unpacks
everything, so any fresh clone can rebuild the (gitignored) runtime tree.

```sh
sh ports/firefox/fetch-runtime.sh            # Debian trixie (glibc 2.41), default
SUITE=bookworm sh ports/firefox/fetch-runtime.sh   # Debian bookworm (glibc 2.36)
sh ports/firefox/check-runtime.sh            # re-verify the tree any time
make disk-ff && make run-firefox             # then boot it
```

Requirements on the host: `curl`, `dpkg-deb` (package `dpkg`), `readelf`
(binutils), `xz`, `bzip2`, `sha256sum`, `awk`, `tar`.  No apt, no root, no
Docker.  Network: about 100 MB on the first run (84 MB Firefox tarball, 9 MB
package index, 17 MB of `.deb` files); everything is cached under
`ports/firefox/prebuilt/` and later runs are offline and take a few seconds.

## What it produces

| Path | Contents | Size |
|------|----------|------|
| `testfiles/firefox/` | Firefox tarball as-is (`firefox-bin`, `libxul.so`, `omni.ja`, `browser/`, `gmp-clearkey/`, `fonts/`, `defaults/`, `dependentlibs.list`, ...) plus the Debian shared objects, `pixbuf-loaders/` and the GL stubs, all flat | 283 MB |
| `testfiles/lib/` | glibc runtime (`ld-linux.so.2`, `libc.so.6`, `libm`, `libdl`, `libpthread`, `librt`, `libresolv`, `libnss_*`, `libanl`, `libutil`, `libthread_db`) plus `libgcc_s.so.1`, `libstdc++.so.6`, mirrored into `i386-linux-gnu/`; `GLIBC-VERSION` records the suite | 11 MB |

`testfiles/lib/` is committed (the initrd needs it); `testfiles/firefox/` is
gitignored and only goes onto the 1 GiB `disk-ff.img`, which has room to spare
(all of `testfiles/` is about 335 MB).

The runtime layout mirrors what the launchers (`userspace/ff/ff.c`,
`login.c`, `desktop.c`) expect:

* `LD_LIBRARY_PATH=/lib:/disk/lib:/disk/firefox` — glibc in `/lib`, the GTK
  stack next to `firefox-bin`.
* `GDK_PIXBUF_MODULE_FILE=/disk/firefox/pixbuf-loaders/loaders.cache` — the
  cache is produced by the suite's own `gdk-pixbuf-query-loaders` (run on
  the host through the fetched `ld-linux.so.2`) and rewritten to `/disk/...`
  paths.  PNG and JPEG are built into gdk-pixbuf 2.42; the module set is the
  Debian default minus `tiff` (drags in libtiff plus seven codecs) and `svg`
  (librsvg).  If the host cannot execute i386 binaries the committed
  `loaders.cache.template` is used instead.
* `XDG_DATA_DIRS=/disk/usr/share` — the hicolor icon theme and DejaVu Sans in
  `testfiles/usr/share/` are committed; the script only refetches the font if
  it is missing.
* `libGL.so.1`, `libEGL.so.1`, `libGLESv2.so.2`, `libpci.so.3`, `libdrm.so.2`
  are empty stubs so Firefox's `glxtest` probe fails fast instead of timing
  out (see `build-glstubs.sh` for the story).  They are compiled with the
  first i686-capable compiler found (`i686-linux-musl-gcc`, `gcc -m32`, ...)
  or, when there is none, emitted directly as ELF by `mkstub.py`.

## How the library set is chosen

`debian-packages.txt` lists the Debian packages (with `old|new` alternatives
for names that changed between suites).  The script unpacks them all into a
pool keyed by `DT_SONAME`, then walks the `DT_NEEDED` chains starting from
`firefox-bin`, `libxul.so`, every Firefox `.so`, the helper executables, the
pixbuf loaders and the GL stubs, copying only what is reachable.  Anything a
listed package ships but nothing links (e.g. `libbrotlienc`, `libexpatw`,
`libgcrypt` on trixie) is pruned.  A soname that no listed package provides
aborts the run with its name, so extending the list is a one-line change.

## Trust model

What the script verifies, and what it merely relies on:

* **Firefox tarball**: must match the sha256 pinned in `fetch-runtime.sh`
  (`FF_SHA256_PINNED`, taken from Mozilla's `SHA256SUMS` for 115.15.0esr).
  A swapped mirror or a tampered download fails hard. For another
  `FF_VERSION` pass `FF_SHA256=<hex>` as well; without it the script falls
  back to Mozilla's `SHA256SUMS` fetched over https and warns that the hash
  is unpinned (that only detects corruption, not substitution).
* **Debian index**: the suite's `InRelease` file is the anchor. Its OpenPGP
  signature is verified with `gpgv` against the Debian archive keyring when
  both exist on the host (`/usr/share/keyrings/debian-archive-keyring.gpg`,
  or `DEBIAN_KEYRING=<file>`; the `debian-archive-keyring` package provides
  it). Without them the script prints a warning and the index is trusted on
  the strength of https to the mirror alone. In both cases `Packages.xz` is
  fetched through `by-hash/SHA256/<sum>` using the sum recorded in
  `InRelease` and must match it, and every `.deb` must match the sha256
  recorded in `Packages.xz`. So with a keyring the chain is
  signature → InRelease → Packages.xz → .deb; without one it is
  https → InRelease → Packages.xz → .deb.
* **Transport**: `DEBIAN_MIRROR` and `MOZ_BASE` must be https. Set
  `ALLOW_INSECURE_MIRROR=1` to use a plain-http mirror (e.g. a local cache);
  the hash chain above still applies.
* Not verified: Mozilla's `SHA256SUMS.asc` (only relevant in the unpinned
  fallback), and the contents of the Debian packages beyond their recorded
  hashes.

## Idempotence

Re-running is safe and cheap. Downloads are cached; each package unpacks
into its own directory under `prebuilt/<suite>/pkgs/<package>/`, replaced
wholesale when its version changes, so no file from an older version can
shadow a newer one in the pool. Before assembling, the script deletes what
its previous run installed (recorded in `.fetch-runtime.manifest`) and then
every shared object in `testfiles/firefox/` that is not part of the Firefox
tarball, logging each removal. That means a tree left behind by an earlier,
differently assembled build is reset to "tarball only" and rebuilt from the
chosen suite; a stale library can never win over the pool copy.

## Verification

`check-runtime.sh` scans every ELF object in `testfiles/firefox/` and
`testfiles/lib/` with `readelf -d` and resolves each `DT_NEEDED` soname
against `testfiles/lib:testfiles/firefox` (the on-target search path).  It
exits non-zero and lists the offenders if anything is unresolved.
`fetch-runtime.sh` runs it at the end and, on hosts that can execute i386
binaries, also runs `LD_BIND_NOW=1 firefox-bin --version` through the fetched
loader (which resolves every symbol reference eagerly) and preloads each GL
stub to make sure glibc accepts them.

## Why trixie by default

glibc 2.36 (bookworm) has the pthread condvar lost-wakeup bug (glibc
BZ#25847) that `proc/scheduler.c` works around with a spurious-wake safety
net; it is fixed in glibc 2.41, which Debian trixie ships.  The GTK stack is
taken from the same suite so the whole runtime is one coherent Debian
release.  `SUITE=bookworm` reproduces the previous glibc 2.36 tree.
