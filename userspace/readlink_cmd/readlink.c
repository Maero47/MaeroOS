/*
 * readlink — print target of a symbolic link
 * Usage: readlink [-f] LINK
 */
#include "../include/unistd.h"
#include "../include/stdio.h"
#include "../include/string.h"

int main(int argc, char *argv[]) {
    int follow = 0;  /* -f: canonicalize (not fully implemented) */
    int ai = 1;
    if (ai < argc && strcmp(argv[ai], "-f") == 0) { follow = 1; ai++; }
    if (ai >= argc) { write(2, "usage: readlink [-f] LINK\n", 26); return 1; }

    char buf[512];
    int n = readlink(argv[ai], buf, 511);
    if (n < 0) { printf("readlink: %s: not a symlink\n", argv[ai]); return 1; }
    buf[n] = '\0';
    (void)follow;  /* -f would resolve further, not implemented */
    write(1, buf, (unsigned)n);
    write(1, "\n", 1);
    return 0;
}
