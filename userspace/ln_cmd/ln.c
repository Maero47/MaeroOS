/*
 * ln — create links
 * Usage: ln [-s] TARGET LINK_NAME
 */
#include "../include/unistd.h"
#include "../include/stdio.h"
#include "../include/string.h"

int main(int argc, char *argv[]) {
    int symbolic = 0;
    int ai = 1;
    if (ai < argc && strcmp(argv[ai], "-s") == 0) { symbolic = 1; ai++; }
    if (argc - ai < 2) { write(2, "usage: ln [-s] TARGET LINK_NAME\n", 32); return 1; }
    const char *target = argv[ai];
    const char *linkname = argv[ai + 1];
    if (symbolic) {
        if (symlink(target, linkname) < 0) {
            printf("ln: failed to create symlink %s\n", linkname);
            return 1;
        }
    } else {
        /* Hard links require a separate syscall (link/linkat) — stub */
        printf("ln: hard links not supported\n");
        return 1;
    }
    return 0;
}
