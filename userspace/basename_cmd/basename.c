#include <stdio.h>
#include <string.h>

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "usage: basename PATH [SUFFIX]\n");
        return 1;
    }
    char *p = argv[1];

    /* Strip trailing slashes */
    int len = strlen(p);
    while (len > 1 && p[len-1] == '/') len--;

    /* Find last slash */
    int start = 0;
    for (int i = len - 1; i >= 0; i--) {
        if (p[i] == '/') { start = i + 1; break; }
    }

    /* The basename */
    char base[512];
    int blen = len - start;
    if (blen <= 0 || blen > 511) { puts("/"); return 0; }
    memcpy(base, p + start, blen);
    base[blen] = '\0';

    /* Strip optional suffix (argv[2]) */
    if (argc >= 3) {
        int slen = strlen(argv[2]);
        if (slen < blen && strcmp(base + blen - slen, argv[2]) == 0)
            base[blen - slen] = '\0';
    }

    puts(base);
    return 0;
}
