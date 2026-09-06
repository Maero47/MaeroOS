#include "../include/fcntl.h"
#include "../include/stdio.h"
#include "../include/string.h"
#include "../include/unistd.h"

static int print_file(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;

    char buf[256];
    int n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        write(1, buf, n);
    close(fd);
    return 1;
}

int main(int argc, char *argv[]) {
    if (argc > 1 && strcmp(argv[1], "status") != 0) {
        printf("session: usage: session [status]\n");
        return 1;
    }

    if (!print_file("/tmp/sessions.status")) {
        printf("session: no session manager status\n");
        return 1;
    }

    return 0;
}
