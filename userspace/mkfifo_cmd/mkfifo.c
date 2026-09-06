/*
 * mkfifo — create named pipes
 * Usage: mkfifo [-m MODE] NAME...
 */
#include "../include/unistd.h"
#include "../include/stdio.h"
#include "../include/stdlib.h"
#include "../include/string.h"

int main(int argc, char *argv[]) {
    int mode = 0666;
    int ai = 1;
    if (ai < argc && strcmp(argv[ai], "-m") == 0 && ai + 1 < argc) {
        mode = (int)strtol(argv[ai + 1], NULL, 8);
        ai += 2;
    }
    if (ai >= argc) { write(2, "usage: mkfifo [-m MODE] NAME...\n", 32); return 1; }
    int ret = 0;
    for (; ai < argc; ai++) {
        if (mkfifo(argv[ai], mode) < 0) {
            printf("mkfifo: %s: failed\n", argv[ai]);
            ret = 1;
        }
    }
    return ret;
}
