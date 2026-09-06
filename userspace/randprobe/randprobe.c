#include "../include/fcntl.h"
#include "../include/stdio.h"
#include "../include/unistd.h"

static int all_zero(const unsigned char *buf, int len) {
    for (int i = 0; i < len; i++) {
        if (buf[i] != 0)
            return 0;
    }
    return 1;
}

static int same_bytes(const unsigned char *a, const unsigned char *b, int len) {
    for (int i = 0; i < len; i++) {
        if (a[i] != b[i])
            return 0;
    }
    return 1;
}

int main(void) {
    unsigned char a[32];
    unsigned char b[32];

    if (getrandom(a, sizeof(a), 0) != (int)sizeof(a)) {
        printf("randprobe: getrandom failed\n");
        return 1;
    }
    if (getrandom(b, sizeof(b), 0) != (int)sizeof(b)) {
        printf("randprobe: second getrandom failed\n");
        return 1;
    }
    if (all_zero(a, sizeof(a)) || same_bytes(a, b, sizeof(a))) {
        printf("randprobe: getrandom weak output\n");
        return 1;
    }
    printf("randprobe getrandom ok\n");

    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        printf("randprobe: open /dev/urandom failed\n");
        return 1;
    }
    if (read(fd, a, sizeof(a)) != (int)sizeof(a)) {
        printf("randprobe: read /dev/urandom failed\n");
        close(fd);
        return 1;
    }
    if (read(fd, b, sizeof(b)) != (int)sizeof(b)) {
        printf("randprobe: second read /dev/urandom failed\n");
        close(fd);
        return 1;
    }
    close(fd);
    if (all_zero(a, sizeof(a)) || same_bytes(a, b, sizeof(a))) {
        printf("randprobe: /dev/urandom weak output\n");
        return 1;
    }
    printf("randprobe urandom ok\n");
    return 0;
}

