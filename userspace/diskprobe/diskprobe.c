#include "../include/fcntl.h"
#include "../include/stdio.h"
#include "../include/string.h"
#include "../include/unistd.h"

static void make_long_name(char *out, int n) {
    strcpy(out, "/disk/dpdir/file-");
    int len = strlen(out);
    int hundreds = (n / 100) % 10;
    int tens = (n / 10) % 10;
    int ones = n % 10;
    out[len++] = (char)('0' + hundreds);
    out[len++] = (char)('0' + tens);
    out[len++] = (char)('0' + ones);
    out[len++] = '-';
    for (int i = 0; i < 200; i++)
        out[len++] = (char)('a' + (i % 26));
    out[len] = '\0';
}

static int check_large_file(void) {
    const int total = 14000;
    char buf[257];
    int fd = open("/disk/dp-large.bin", O_CREAT | O_TRUNC | O_RDWR);
    if (fd < 0) {
        puts("diskprobe: large open failed");
        return 1;
    }

    for (int off = 0; off < total; ) {
        int chunk = total - off;
        if (chunk > (int)sizeof(buf)) chunk = (int)sizeof(buf);
        for (int i = 0; i < chunk; i++)
            buf[i] = (char)('A' + ((off + i) % 26));
        if (write(fd, buf, chunk) != chunk) {
            puts("diskprobe: large write failed");
            close(fd);
            return 1;
        }
        off += chunk;
    }

    if (lseek(fd, 12280, 0) < 0) {
        puts("diskprobe: large seek failed");
        close(fd);
        return 1;
    }
    memset(buf, 0, sizeof(buf));
    if (read(fd, buf, 32) != 32) {
        puts("diskprobe: large read failed");
        close(fd);
        return 1;
    }
    for (int i = 0; i < 32; i++) {
        if (buf[i] != (char)('A' + ((12280 + i) % 26))) {
            puts("diskprobe: large verify failed");
            close(fd);
            return 1;
        }
    }

    if (ftruncate(fd, 4096) < 0) {
        puts("diskprobe: truncate failed");
        close(fd);
        return 1;
    }
    if (lseek(fd, 4090, 0) < 0) {
        puts("diskprobe: truncate seek failed");
        close(fd);
        return 1;
    }
    memset(buf, 0, sizeof(buf));
    if (read(fd, buf, 32) != 6) {
        puts("diskprobe: truncate verify failed");
        close(fd);
        return 1;
    }
    close(fd);
    unlink("/disk/dp-large.bin");
    return 0;
}

static int check_large_dir(void) {
    char name[256];

    for (int i = 0; i < 70; i++) {
        make_long_name(name, i);
        unlink(name);
    }
    unlink("/disk/dpdir");

    if (mkdir("/disk/dpdir", 0755) < 0) {
        puts("diskprobe: mkdir large dir failed");
        return 1;
    }

    for (int i = 0; i < 70; i++) {
        make_long_name(name, i);
        int fd = open(name, O_CREAT | O_TRUNC | O_RDWR);
        if (fd < 0) {
            puts("diskprobe: large dir create failed");
            return 1;
        }
        if (write(fd, "x", 1) != 1) {
            puts("diskprobe: large dir write failed");
            close(fd);
            return 1;
        }
        close(fd);
    }

    make_long_name(name, 69);
    int fd = open(name, O_RDONLY);
    if (fd < 0) {
        puts("diskprobe: large dir lookup failed");
        return 1;
    }
    close(fd);

    for (int i = 0; i < 70; i++) {
        make_long_name(name, i);
        unlink(name);
    }
    unlink("/disk/dpdir");
    return 0;
}

int main(void) {
    char buf[5];
    int fd = open("/disk/hello.txt", O_RDWR);
    if (fd < 0) {
        puts("diskprobe: open failed");
        return 1;
    }

    if (write(fd, "DISK", 4) != 4) {
        puts("diskprobe: write failed");
        close(fd);
        return 1;
    }

    if (lseek(fd, 0, 0) < 0) {
        puts("diskprobe: seek failed");
        close(fd);
        return 1;
    }

    memset(buf, 0, sizeof(buf));
    if (read(fd, buf, 4) != 4) {
        puts("diskprobe: read failed");
        close(fd);
        return 1;
    }

    close(fd);

    if (memcmp(buf, "DISK", 4) != 0) {
        puts("diskprobe: verify failed");
        return 1;
    }

    if (check_large_file())
        return 1;
    if (check_large_dir())
        return 1;

    puts("diskprobe ok");
    return 0;
}
