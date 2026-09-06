#include "../include/unistd.h"
#include "../include/stdio.h"

#define O_WRONLY  1
#define O_CREAT   0x40

int main(int argc, char *argv[]) {
    if (argc < 2) {
        puts("usage: touch <file> [...]");
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_WRONLY | O_CREAT);
        if (fd < 0) {
            printf("touch: cannot create '%s'\n", argv[i]);
        } else {
            close(fd);
        }
    }
    return 0;
}
