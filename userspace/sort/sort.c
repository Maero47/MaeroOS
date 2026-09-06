#include "../include/unistd.h"
#include "../include/stdio.h"
#include "../include/string.h"
#include "../include/stdlib.h"

#define MAX_LINES  4096
#define MAX_LINELEN 256

static char  lines_buf[MAX_LINES][MAX_LINELEN];
static char *lines[MAX_LINES];
static int   nlines = 0;

static int reverse_flag = 0;
static int numeric_flag = 0;

static int cmp_lines(const void *a, const void *b) {
    const char *sa = *(const char **)a;
    const char *sb = *(const char **)b;
    int r;
    if (numeric_flag) {
        long na = atoi(sa), nb = atoi(sb);
        r = (na > nb) - (na < nb);
    } else {
        r = strcmp(sa, sb);
    }
    return reverse_flag ? -r : r;
}

static void qsort_lines(char **arr, int n) {
    /* insertion sort — simple, small kernel; can upgrade if needed */
    for (int i = 1; i < n; i++) {
        char *key = arr[i];
        int j = i - 1;
        while (j >= 0 && cmp_lines(&arr[j], &key) > 0) {
            arr[j + 1] = arr[j];
            j--;
        }
        arr[j + 1] = key;
    }
}

static void read_lines(int fd) {
    int total = 0, n;
    static char tmp[MAX_LINES * MAX_LINELEN];
    while ((n = read(fd, tmp + total, sizeof(tmp) - total - 1)) > 0)
        total += n;
    tmp[total] = '\0';

    char *p = tmp;
    while (*p && nlines < MAX_LINES) {
        char *end = p;
        while (*end && *end != '\n') end++;
        int len = (int)(end - p);
        if (len >= MAX_LINELEN) len = MAX_LINELEN - 1;
        for (int i = 0; i < len; i++) lines_buf[nlines][i] = p[i];
        lines_buf[nlines][len] = '\0';
        lines[nlines] = lines_buf[nlines];
        nlines++;
        if (*end == '\n') p = end + 1;
        else break;
    }
}

int main(int argc, char *argv[]) {
    int ai = 1;
    while (ai < argc && argv[ai][0] == '-' && argv[ai][1]) {
        char *f = argv[ai] + 1;
        while (*f) {
            if (*f == 'r') reverse_flag = 1;
            else if (*f == 'n') numeric_flag = 1;
            f++;
        }
        ai++;
    }

    if (ai >= argc) {
        read_lines(0);
    } else {
        for (int i = ai; i < argc; i++) {
            int fd = open(argv[i], 0);
            if (fd < 0) { write(2, argv[i], strlen(argv[i])); write(2, ": open failed\n", 14); continue; }
            read_lines(fd);
            close(fd);
        }
    }

    qsort_lines(lines, nlines);

    for (int i = 0; i < nlines; i++) {
        write(1, lines[i], strlen(lines[i]));
        write(1, "\n", 1);
    }
    exit(0);
    return 0;
}
