#include "../include/unistd.h"
#include "../include/stdio.h"
#include "../include/string.h"
#include "../include/stdlib.h"

static void print_line(const char *line, int count, int show_count) {
    if (show_count) {
        char nbuf[16];
        int n = count, nlen = 0;
        if (!n) { nbuf[nlen++] = '0'; }
        else {
            char tmp[16]; int tl = 0;
            while (n) { tmp[tl++] = '0' + n % 10; n /= 10; }
            for (int i = tl - 1; i >= 0; i--) nbuf[nlen++] = tmp[i];
        }
        nbuf[nlen] = '\0';
        write(1, nbuf, nlen);
        write(1, " ", 1);
    }
    write(1, line, strlen(line));
    write(1, "\n", 1);
}

static void process(int fd, int show_count) {
    char buf[65536];
    int total = 0, n;
    while ((n = read(fd, buf + total, sizeof(buf) - total - 1)) > 0)
        total += n;
    buf[total] = '\0';

    char prev[1024] = "";
    int  count = 0;
    char *p = buf;

    while (*p) {
        char *end = p;
        while (*end && *end != '\n') end++;
        int len = (int)(end - p);
        if (len >= 1024) len = 1023;
        char cur[1024];
        for (int i = 0; i < len; i++) cur[i] = p[i];
        cur[len] = '\0';

        if (count == 0 || strcmp(cur, prev) != 0) {
            if (count > 0) print_line(prev, count, show_count);
            for (int i = 0; i <= len; i++) prev[i] = cur[i];
            count = 1;
        } else {
            count++;
        }

        if (*end == '\n') p = end + 1;
        else break;
    }
    if (count > 0) print_line(prev, count, show_count);
}

int main(int argc, char *argv[]) {
    int show_count = 0;
    int ai = 1;
    while (ai < argc && argv[ai][0] == '-' && argv[ai][1]) {
        char *f = argv[ai] + 1;
        while (*f) { if (*f == 'c') show_count = 1; f++; }
        ai++;
    }

    if (ai >= argc) {
        process(0, show_count);
    } else {
        int fd = open(argv[ai], 0);
        if (fd < 0) { write(2, argv[ai], strlen(argv[ai])); write(2, ": open failed\n", 14); exit(1); }
        process(fd, show_count);
        close(fd);
    }
    exit(0);
    return 0;
}
