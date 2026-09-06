#include "../include/unistd.h"
#include "../include/string.h"
#include "../include/stdlib.h"

static void print_num(long v, int base, int width) {
    static const char hex[] = "0123456789abcdef";
    char tmp[32];
    int neg = 0, tl = 0;
    if (base == 10 && v < 0) { neg = 1; v = -v; }
    unsigned long uv = (unsigned long)v;
    if (!uv) { tmp[tl++] = '0'; }
    else { while (uv) { tmp[tl++] = hex[uv % base]; uv /= base; } }
    if (neg) tmp[tl++] = '-';
    /* pad */
    for (int i = tl; i < width; i++) write(1, " ", 1);
    /* reverse */
    char out[32];
    for (int i = 0; i < tl; i++) out[i] = tmp[tl - 1 - i];
    write(1, out, tl);
}

int main(int argc, char *argv[]) {
    if (argc < 2) { exit(0); }
    const char *fmt = argv[1];
    int ai = 2; /* current arg index */

    for (const char *p = fmt; *p; p++) {
        if (*p == '\\') {
            p++;
            switch (*p) {
            case 'n': write(1, "\n", 1); break;
            case 't': write(1, "\t", 1); break;
            case '\\': write(1, "\\", 1); break;
            case '0': write(1, "\0", 1); break;
            default: write(1, "\\", 1); write(1, p, 1); break;
            }
        } else if (*p == '%') {
            p++;
            if (*p == '%') { write(1, "%", 1); continue; }
            const char *arg = (ai < argc) ? argv[ai++] : "";
            switch (*p) {
            case 's': write(1, arg, strlen(arg)); break;
            case 'd': case 'i': print_num(atoi(arg), 10, 0); break;
            case 'u': print_num((long)(unsigned long)atoi(arg), 10, 0); break;
            case 'x': print_num(atoi(arg), 16, 0); break;
            default: write(1, "%", 1); write(1, p, 1); break;
            }
        } else {
            write(1, p, 1);
        }
    }
    exit(0);
    return 0;
}
