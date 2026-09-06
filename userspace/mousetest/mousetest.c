#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char *event_name(uint16_t type, uint16_t code) {
    if (type == EV_REL && code == REL_X) return "REL_X";
    if (type == EV_REL && code == REL_Y) return "REL_Y";
    if (type == EV_KEY && code == BTN_LEFT) return "BTN_LEFT";
    if (type == EV_KEY && code == BTN_RIGHT) return "BTN_RIGHT";
    if (type == EV_KEY && code == BTN_MIDDLE) return "BTN_MIDDLE";
    if (type == EV_SYN && code == SYN_REPORT) return "SYN";
    return "EVENT";
}

int main(int argc, char **argv) {
    int fd = open("/dev/input/event1", O_RDONLY);
    if (fd < 0) {
        printf("mousetest: /dev/input/event1 unavailable\n");
        return 1;
    }

    if (argc > 1 && strcmp(argv[1], "--probe") == 0) {
        printf("mousetest: /dev/input/event1 ok\n");
        close(fd);
        return 0;
    }

    printf("mousetest: move/click in the QEMU window\n");
    for (;;) {
        struct input_event ev;
        int n = read(fd, &ev, sizeof(ev));
        if (n == (int)sizeof(ev)) {
            printf("%s type=%u code=%u value=%d time=%d.%06d\n",
                   event_name(ev.type, ev.code), ev.type, ev.code, ev.value,
                   ev.tv_sec, ev.tv_usec);
        }
    }
}

