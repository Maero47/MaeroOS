#include "../include/fcntl.h"
#include "../include/stdio.h"
#include "../include/unistd.h"

int main(void) {
    int fd = open("/proc/processes", O_RDONLY);
    if (fd < 0) {
        puts("ps: cannot open /proc/processes");
        return 1;
    }

    char buf[512];
    int n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        write(1, buf, n);

    close(fd);
    return 0;
}
