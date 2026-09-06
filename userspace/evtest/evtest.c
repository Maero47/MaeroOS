#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    int fd = open("/dev/input/event0", O_RDONLY);
    if (fd < 0) {
        printf("evtest: /dev/input/event0 unavailable\n");
        return 1;
    }

    if (argc > 1 && strcmp(argv[1], "--probe") == 0) {
        printf("evtest: /dev/input/event0 ok\n");
        close(fd);
        return 0;
    }

    printf("evtest: press keys in the QEMU window\n");
    for (;;) {
        struct input_event ev;
        int n = read(fd, &ev, sizeof(ev));
        if (n == (int)sizeof(ev)) {
            printf("event type=%u code=%u value=%d time=%d.%06d\n",
                   ev.type, ev.code, ev.value, ev.tv_sec, ev.tv_usec);
        }
    }
}
