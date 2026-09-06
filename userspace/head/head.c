#include "../include/unistd.h"
#include "../include/stdio.h"
#include "../include/stdlib.h"
#include "../include/string.h"

static void head_fd(int fd, int nlines) {
    int  lines = 0;
    int  n;
    /* Read char by char to count newlines accurately */
    char c;
    while (lines < nlines && (n = read(fd, &c, 1)) == 1) {
        write(1, &c, 1);
        if (c == '\n') {
            lines++;
        }
    }
}

int main(int argc, char *argv[]) {
    int nlines = 10;
    int file_start = 1;

    /* Parse optional -n N */
    if (argc >= 3 && argv[1][0] == '-' && argv[1][1] == 'n') {
        nlines = atoi(argv[2]);
        file_start = 3;
    } else if (argc >= 2 && argv[1][0] == '-' &&
               argv[1][1] >= '1' && argv[1][1] <= '9') {
        nlines = atoi(argv[1] + 1);
        file_start = 2;
    }

    if (file_start >= argc) {
        head_fd(0, nlines);
        return 0;
    }

    for (int i = file_start; i < argc; i++) {
        int fd = open(argv[i], 0);
        if (fd < 0) {
            printf("head: cannot open '%s'\n", argv[i]);
            continue;
        }
        head_fd(fd, nlines);
        close(fd);
    }
    return 0;
}
