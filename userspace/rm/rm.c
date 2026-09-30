#include "../include/unistd.h"
#include "../include/stdio.h"
#include "../include/errno.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        puts("usage: rm <file> [...]");
        return 1;
    }
    int ret = 0;
    for (int i = 1; i < argc; i++) {
        /* unlink() never removes a directory (Linux: EISDIR); an empty one
         * goes with rmdir(). */
        int r = unlink(argv[i]);
        if (r < 0 && errno == EISDIR)
            r = rmdir(argv[i]);
        if (r < 0) {
            printf("rm: cannot remove '%s'\n", argv[i]);
            ret = 1;
        }
    }
    return ret;
}
