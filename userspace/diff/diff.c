/*
 * diff — compare two files line by line
 * Outputs unified-ish diff (< old  > new)
 */
#include "../include/unistd.h"
#include "../include/stdio.h"
#include "../include/string.h"
#include "../include/stdlib.h"

#define MAXLINES 2048
#define LMAX     512

static char la[MAXLINES][LMAX];
static char lb[MAXLINES][LMAX];
static int na = 0, nb = 0;

static int read_file(const char *path, char lines[][LMAX], int *count) {
    int fd = 0;
    if (path[0] != '-' || path[1]) {
        fd = open(path, 0);
        if (fd < 0) { printf("diff: %s: not found\n", path); return -1; }
    }
    int llen = 0, n;
    char buf[256];
    *count = 0;
    while ((n = (int)read(fd, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n' || llen == LMAX - 1) {
                lines[*count][llen] = '\0';
                if (*count < MAXLINES - 1) (*count)++;
                llen = 0;
            } else {
                lines[*count][llen++] = c;
            }
        }
    }
    if (llen > 0) {
        lines[*count][llen] = '\0';
        if (*count < MAXLINES - 1) (*count)++;
    }
    if (fd != 0) close(fd);
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc < 3) { write(2, "usage: diff FILE1 FILE2\n", 24); return 2; }
    if (read_file(argv[1], la, &na) < 0) return 2;
    if (read_file(argv[2], lb, &nb) < 0) return 2;

    int different = 0;
    int max = na > nb ? na : nb;
    for (int i = 0; i < max; i++) {
        const char *a = i < na ? la[i] : NULL;
        const char *b = i < nb ? lb[i] : NULL;
        if (a && b && strcmp(a, b) == 0) continue;
        different = 1;
        if (a) printf("< %s\n", a);
        if (b) printf("> %s\n", b);
    }
    return different ? 1 : 0;
}
