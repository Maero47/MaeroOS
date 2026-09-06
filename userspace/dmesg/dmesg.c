#include "../include/unistd.h"
#include "../include/stdio.h"

/*
 * dmesg — dump the kernel log ring buffer exposed at /proc/kmsg.
 */
int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int fd = open("/proc/kmsg", 0 /* O_RDONLY */);
    if (fd < 0) {
        puts("dmesg: cannot open /proc/kmsg");
        return 1;
    }

    char buf[1024];
    int n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        write(1, buf, n);

    close(fd);
    return 0;
}
