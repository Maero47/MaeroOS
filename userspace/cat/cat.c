#include "../include/unistd.h"
#include "../include/stdio.h"

static void cat_fd(int fd) {
    char buf[512];
    int n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        write(1, buf, n);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        /* No args: read stdin */
        cat_fd(0);
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], 0);  /* O_RDONLY */
        if (fd < 0) {
            puts("cat: cannot open file");
            continue;
        }
        cat_fd(fd);
        close(fd);
    }
    return 0;
}
