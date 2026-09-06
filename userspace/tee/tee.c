#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int main(int argc, char *argv[]) {
    int append = 0;
    int i = 1;
    if (i < argc && strcmp(argv[i], "-a") == 0) { append = 1; i++; }

    /* Open output files */
    int fds[32];
    int nfds = 0;
    for (; i < argc && nfds < 32; i++) {
        int flags = append ? (O_WRONLY | O_CREAT | O_APPEND) : (O_WRONLY | O_CREAT | O_TRUNC);
        int fd = open(argv[i], flags);
        if (fd < 0) { fprintf(stderr, "tee: %s: open failed\n", argv[i]); }
        else        { fds[nfds++] = fd; }
    }

    char buf[4096];
    int n;
    while ((n = read(0, buf, sizeof(buf))) > 0) {
        write(1, buf, n);
        for (int j = 0; j < nfds; j++)
            write(fds[j], buf, n);
    }
    for (int j = 0; j < nfds; j++)
        close(fds[j]);
    return 0;
}
