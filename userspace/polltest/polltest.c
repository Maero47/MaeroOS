#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <unistd.h>

int main(void) {
    int fd = open("/dev/input/event0", O_RDONLY);
    if (fd < 0) {
        printf("polltest: /dev/input/event0 unavailable\n");
        return 1;
    }

    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    int ret = poll(&pfd, 1, 20);
    if (ret < 0) {
        printf("polltest: poll failed\n");
        close(fd);
        return 1;
    }
    if (ret != 0) {
        printf("polltest: expected timeout, got %d revents=0x%x\n",
               ret, pfd.revents);
        close(fd);
        return 1;
    }

    pfd.fd = 99;
    pfd.events = POLLIN;
    pfd.revents = 0;
    ret = poll(&pfd, 1, 0);
    if (ret != 1 || !(pfd.revents & POLLNVAL)) {
        printf("polltest: POLLNVAL failed ret=%d revents=0x%x\n",
               ret, pfd.revents);
        close(fd);
        return 1;
    }

    printf("polltest ok\n");
    close(fd);
    return 0;
}

