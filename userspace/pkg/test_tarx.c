/* Host-side driver for pkg's safety code (tarx.c, sha256.c), used by
 * tools/test_pkg_tarx.py.  Built with the host compiler, not the cross one:
 *
 *   test_tarx name <pkgname>        exit 0 if pkg_name_ok()
 *   test_tarx extract <tar> <dir>   untar_mem() the file into dir
 *   test_tarx sha <file>            print the SHA-256 of the file
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sha256.h"
#include "tarx.h"

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

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "name")) {
        int ok = pkg_name_ok(argv[2]);
        printf("%s\n", ok ? "ok" : "bad");
        return ok ? 0 : 1;
    }
    if (argc == 4 && !strcmp(argv[1], "extract")) {
        const char *why;
        int len, files;
        char *data = slurp(argv[2], &len);
        if (!data) { perror(argv[2]); return 2; }
        files = untar_mem(data, len, argv[3], &why);
        if (files < 0) { printf("rejected: %s\n", why); return 1; }
        printf("files=%d\n", files);
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "sha")) {
        char hex[65];
        int len;
        char *data = slurp(argv[2], &len);
        if (!data) { perror(argv[2]); return 2; }
        sha256_hex(data, (size_t)len, hex);
        printf("%s\n", hex);
        return 0;
    }
    fprintf(stderr, "usage: test_tarx name <n> | extract <tar> <dir> | sha <file>\n");
    return 2;
}
