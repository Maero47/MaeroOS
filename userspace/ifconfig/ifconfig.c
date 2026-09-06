#include "../include/stdio.h"
#include "../include/unistd.h"
#include "../include/fcntl.h"

int main(void) {
    char buf[512];
    int fd = open("/proc/netif", O_RDONLY);
    if (fd < 0) {
        perror("ifconfig");
        return 1;
    }

    for (;;) {
        int n = read(fd, buf, sizeof(buf));
        if (n < 0) {
            perror("ifconfig");
            close(fd);
            return 1;
        }
        if (n == 0)
            break;
        if (write(1, buf, n) != n) {
            close(fd);
            return 1;
        }
    }

    close(fd);
    return 0;
}

