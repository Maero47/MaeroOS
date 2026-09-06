#include "../include/unistd.h"
#include "../include/stdio.h"

/* O_WRONLY | O_CREAT | O_TRUNC */
#define O_WRONLY  1
#define O_CREAT   0x40
#define O_TRUNC   0x200

int main(int argc, char *argv[]) {
    if (argc < 3) {
        puts("usage: cp <src> <dst>");
        return 1;
    }
    int src = open(argv[1], 0);   /* O_RDONLY */
    if (src < 0) {
        puts("cp: cannot open source");
        return 1;
    }
    int dst = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC);
    if (dst < 0) {
        puts("cp: cannot open destination");
        close(src);
        return 1;
    }
    char buf[512];
    int n;
    while ((n = read(src, buf, sizeof(buf))) > 0)
        write(dst, buf, n);

    close(src);
    close(dst);
    return 0;
}
