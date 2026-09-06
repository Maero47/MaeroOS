#include <unistd.h>
#include <string.h>
#include <stdio.h>

/* Expand simple escape sequences and character classes */
static int expand(const char *s, unsigned char *out, int outsz) {
    int n = 0;
    while (*s && n < outsz - 1) {
        if (s[0] == '\\') {
            s++;
            switch (*s) {
            case 'n': out[n++] = '\n'; break;
            case 't': out[n++] = '\t'; break;
            case 'r': out[n++] = '\r'; break;
            case '\\': out[n++] = '\\'; break;
            default:  out[n++] = *s; break;
            }
            s++;
        } else if (s[0] == '[' && s[1] == ':') {
            /* character classes: [:alpha:] [:digit:] [:lower:] [:upper:] [:space:] */
            const char *cls = s + 2;
            if (strncmp(cls, "alpha:]", 7) == 0) {
                for (int c = 'a'; c <= 'z' && n < outsz-1; c++) out[n++] = c;
                for (int c = 'A'; c <= 'Z' && n < outsz-1; c++) out[n++] = c;
                s += 9;
            } else if (strncmp(cls, "digit:]", 7) == 0) {
                for (int c = '0'; c <= '9' && n < outsz-1; c++) out[n++] = c;
                s += 9;
            } else if (strncmp(cls, "lower:]", 7) == 0) {
                for (int c = 'a'; c <= 'z' && n < outsz-1; c++) out[n++] = c;
                s += 9;
            } else if (strncmp(cls, "upper:]", 7) == 0) {
                for (int c = 'A'; c <= 'Z' && n < outsz-1; c++) out[n++] = c;
                s += 9;
            } else if (strncmp(cls, "space:]", 7) == 0) {
                out[n++] = ' '; out[n++] = '\t'; out[n++] = '\n';
                out[n++] = '\r'; out[n++] = '\f'; out[n++] = '\v';
                s += 9;
            } else {
                out[n++] = *s++;
            }
        } else if (s[1] == '-' && s[2] && (unsigned char)s[2] >= (unsigned char)s[0]) {
            /* range: a-z */
            for (unsigned char c = (unsigned char)s[0]; c <= (unsigned char)s[2] && n < outsz-1; c++)
                out[n++] = c;
            s += 3;
        } else {
            out[n++] = (unsigned char)*s++;
        }
    }
    out[n] = '\0';
    return n;
}

int main(int argc, char *argv[]) {
    int opt_d = 0, opt_s = 0;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (strchr(argv[i], 'd')) opt_d = 1;
        if (strchr(argv[i], 's')) opt_s = 1;
    }

    if (i >= argc) {
        fprintf(stderr, "usage: tr [-ds] SET1 [SET2]\n");
        return 1;
    }

    unsigned char set1[512], set2[512];
    int n1 = expand(argv[i], set1, sizeof(set1));
    int n2 = (i + 1 < argc) ? expand(argv[i+1], set2, sizeof(set2)) : 0;

    /* Build lookup table */
    unsigned char xlat[256];
    unsigned char del[256];
    for (int c = 0; c < 256; c++) { xlat[c] = c; del[c] = 0; }

    if (opt_d) {
        for (int j = 0; j < n1; j++) del[(unsigned)set1[j]] = 1;
    } else {
        for (int j = 0; j < n1; j++) {
            int to = (j < n2) ? set2[j] : (n2 > 0 ? set2[n2-1] : set1[j]);
            xlat[(unsigned)set1[j]] = (unsigned char)to;
        }
    }

    unsigned char buf[4096];
    int n;
    unsigned char last = 0;
    while ((n = read(0, buf, sizeof(buf))) > 0) {
        int wr = 0;
        unsigned char out[4096];
        for (int j = 0; j < n; j++) {
            unsigned char c = buf[j];
            if (opt_d && del[c]) continue;
            c = xlat[c];
            if (opt_s && c == last) continue;
            out[wr++] = c;
            last = c;
        }
        write(1, out, wr);
    }
    return 0;
}
