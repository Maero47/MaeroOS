/* Host-side driver for pkg's index signature check (indexsig.c, ed25519.c
 * over third_party/tweetnacl), used by tools/test_pkg_sign.py.  Built with
 * the host compiler:
 *
 *   test_indexsig verify <pk hex> <sig hex> <msg file>   exit 0 if valid
 *   test_indexsig index <pk hex> <index file>            exit 0 if trusted,
 *        printing "serial=<n> signed=<bytes>" or the rejection reason
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ed25519.h"
#include "indexsig.h"

static char *slurp(const char *path, int *len) {
    FILE *f = fopen(path, "rb");
    char *buf;
    long n;
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)n + 1);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); return 0; }
    fclose(f);
    *len = (int)n;
    return buf;
}

static int unhex(const char *s, uint8_t *out, size_t n) {
    if (strlen(s) != 2 * n) return -1;
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        if (sscanf(s + 2 * i, "%2x", &v) != 1) return -1;
        out[i] = (uint8_t)v;
    }
    return 0;
}

int main(int argc, char **argv) {
    uint8_t pk[32], sig[64];
    char *buf;
    int len;

    if (argc < 4 || unhex(argv[2], pk, 32)) return 2;
    if (argc == 5 && !strcmp(argv[1], "verify")) {
        if (unhex(argv[3], sig, 64) || !(buf = slurp(argv[4], &len))) return 2;
        return ed25519_verify(sig, buf, (size_t)len, pk) ? 1 : 0;
    }
    if (argc == 4 && !strcmp(argv[1], "index")) {
        unsigned long serial;
        int signed_len;
        const char *why;
        if (!(buf = slurp(argv[3], &len))) return 2;
        if (index_verify(buf, len, pk, &serial, &signed_len, &why)) {
            printf("%s\n", why);
            return 1;
        }
        printf("serial=%lu signed=%d\n", serial, signed_len);
        return 0;
    }
    return 2;
}
