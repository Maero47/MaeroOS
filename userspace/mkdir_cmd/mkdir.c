#include "../include/unistd.h"
#include "../include/stdio.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        puts("usage: mkdir <dir> [...]");
        return 1;
    }
    int ret = 0;
    for (int i = 1; i < argc; i++) {
        if (mkdir(argv[i], 0755) < 0) {
            printf("mkdir: cannot create '%s'\n", argv[i]);
            ret = 1;
        }
    }
    return ret;
}
