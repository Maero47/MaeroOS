/* zprobe — a dynamically-linked PIE that links a REAL third-party shared
 * library, libz.so.1 (zlib, itself a Firefox dependency).  Proves the loader
 * on a genuine external library: prints the version + a compress round-trip. */
#include <stdio.h>
#include <string.h>
#include <zlib.h>

int main(void) {
    const char *ver = zlibVersion();
    const char *src = "MaeroOS dynamic linking works with real shared libs!";
    unsigned char comp[256];
    unsigned char back[256];
    uLongf clen = sizeof(comp), blen = sizeof(back);

    if (compress(comp, &clen, (const unsigned char *)src, strlen(src) + 1) != Z_OK) {
        printf("ZLIB_FAIL compress\n"); return 1;
    }
    if (uncompress(back, &blen, comp, clen) != Z_OK) {
        printf("ZLIB_FAIL uncompress\n"); return 1;
    }
    if (strcmp((char *)back, src) != 0) {
        printf("ZLIB_FAIL roundtrip\n"); return 1;
    }
    printf("ZLIB_OK ver=%s clen=%lu\n", ver, (unsigned long)clen);
    fflush(stdout);
    return 0;
}
