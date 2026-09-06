/* top — one-shot process snapshot (use `busybox top` for interactive). */
#include "../include/stdio.h"
#include "../include/fcntl.h"
#include "../include/unistd.h"

static void dump(const char *path) {
    char buf[4096];
    int fd = open(path, O_RDONLY);
    if (fd < 0) return;
    int n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) write(1, buf, n);
    close(fd);
}

int main(void) {
    dump("/proc/meminfo");
    printf("\n");
    dump("/proc/processes");
    return 0;
}
