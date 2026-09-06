#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define WMCTL_PATH "/tmp/wmctl"

static void usage(void) {
    printf("usage: wmctl COMMAND [TEXT]\n");
    printf("commands: app [1-3] TEXT, title [1-3] TEXT, close [1-3]\n");
    printf("          geom [1-3] X Y W H\n");
    printf("          bg [1-3] COLOR, rect [1-3] X Y W H COLOR\n");
    printf("          text [1-3] LINE COLOR TEXT, clear [1-3]\n");
    printf("          log TEXT, status TEXT, clear, quit, focus, shell\n");
}

static int append_arg(char *buf, unsigned size, const char *arg) {
    unsigned len = strlen(buf);
    unsigned i = 0;

    if (len + 1 >= size) return -1;
    if (len) buf[len++] = ' ';
    while (arg[i] && len + 1 < size)
        buf[len++] = arg[i++];
    buf[len] = 0;
    return arg[i] ? -1 : 0;
}

int main(int argc, char **argv) {
    char line[160];
    int fd;

    if (argc < 2) {
        usage();
        return 1;
    }

    line[0] = 0;
    for (int i = 1; i < argc; i++) {
        if (append_arg(line, sizeof(line), argv[i]) < 0) {
            printf("wmctl: command too long\n");
            return 1;
        }
    }
    if (strlen(line) + 2 >= sizeof(line)) {
        printf("wmctl: command too long\n");
        return 1;
    }
    strcat(line, "\n");

    fd = open(WMCTL_PATH, O_WRONLY);
    if (fd < 0) {
        printf("wmctl: desktop control fifo unavailable\n");
        return 1;
    }
    if (write(fd, line, (int)strlen(line)) < 0) {
        printf("wmctl: write failed\n");
        close(fd);
        return 1;
    }
    close(fd);
    return 0;
}
