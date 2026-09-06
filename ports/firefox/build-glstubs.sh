#!/bin/sh
# Build empty i386 stub shared objects for Firefox's glxtest GPU probe.
#
# glxtest dlopen()s libGL.so.1 / libEGL.so.1 / libGLESv2.so.2 / libpci.so.3 /
# libdrm.so.2 to detect a GPU.  On MaeroOS none of these exist, so each dlopen
# does a slow search-and-fail across the entire loader path; the cumulative cost
# pushes glxtest past Firefox's hard-coded 4 s GFX_TEST timeout, the parent's
# poll() on the result pipe times out, mGlxTestError is set, and Firefox blocks
# most graphics features (no window).  Shipping zero-symbol stubs at the first
# LD_LIBRARY_PATH entry (/disk/firefox) makes each dlopen succeed instantly;
# glxtest's dlsym()s then return NULL and it bails to its normal "no GPU" path
# well within the timeout, so the pipe read succeeds and mGlxTestError stays
# false (software WebRender is configured cleanly).
#
# Output: testfiles/firefox/{libGL.so.1,libEGL.so.1,libGLESv2.so.2,
#                             libpci.so.3,libdrm.so.2}
set -e
OUT="$(cd "$(dirname "$0")/../../testfiles/firefox" && pwd)"
TMP=$(mktemp -d)
cat > "$TMP/stub.c" <<'CEOF'
int __maeros_glstub_marker = 1;
CEOF
cat > "$TMP/build.sh" <<'BEOF'
set -e
for n in libGL.so.1 libEGL.so.1 libGLESv2.so.2 libpci.so.3 libdrm.so.2; do
  gcc -m32 -shared -fPIC -nostdlib -o "/out/$n" /work/stub.c
done
BEOF
docker run --rm -v "$TMP:/work" -v "$OUT:/out" i386/debian:bookworm-slim \
  sh -c "apt-get update -qq >/dev/null 2>&1 && apt-get install -y -qq gcc >/dev/null 2>&1 && sh /work/build.sh"
rm -rf "$TMP"
echo "glstubs installed to $OUT"
