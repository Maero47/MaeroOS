#include "../include/unistd.h"
#include "../include/stdio.h"
#include "../include/string.h"
#include "../include/stdlib.h"

/* Parse field spec: "2", "2-4", "1,3,5" → bitmap of 1-based fields (max 64) */
static unsigned long long parse_fields(const char *spec) {
    unsigned long long bits = 0;
    const char *p = spec;
    while (*p) {
        int a = atoi(p);
        while (*p && *p != ',' && *p != '-') p++;
        if (*p == '-') {
            p++;
            int b = *p ? atoi(p) : 64;
            while (*p && *p != ',') p++;
            for (int i = a; i <= b && i <= 64; i++)
                bits |= (1ULL << (i - 1));
        } else {
            if (a >= 1 && a <= 64) bits |= (1ULL << (a - 1));
        }
        if (*p == ',') p++;
    }
    return bits;
}

static void process_line(const char *line, int len, char delim, unsigned long long fields) {
    int field = 1;
    int i = 0;
    int first_out = 1;
    while (i <= len) {
        /* Find next delimiter or end of line */
        int j = i;
        while (j < len && line[j] != delim) j++;
        /* line[i..j-1] is field `field` */
        if (fields & (1ULL << (field - 1))) {
            if (!first_out) write(1, &delim, 1);
            if (j > i) write(1, line + i, j - i);
            first_out = 0;
        }
        field++;
        i = j + 1;
    }
    write(1, "\n", 1);
}

static void process(int fd, char delim, unsigned long long fields) {
    char buf[65536];
    int total = 0, n;
    while ((n = read(fd, buf + total, sizeof(buf) - total - 1)) > 0)
        total += n;
    buf[total] = '\0';

    char *p = buf;
    while (*p) {
        char *end = p;
        while (*end && *end != '\n') end++;
        process_line(p, (int)(end - p), delim, fields);
        if (*end == '\n') p = end + 1;
        else break;
    }
}

int main(int argc, char *argv[]) {
    char delim = '\t';
    unsigned long long fields = 0;
    int ai = 1;
    while (ai < argc && argv[ai][0] == '-' && argv[ai][1]) {
        if (argv[ai][1] == 'd' && argv[ai][2]) {
            delim = argv[ai][2];
        } else if (argv[ai][1] == 'd' && ai + 1 < argc) {
            delim = argv[++ai][0];
        } else if (argv[ai][1] == 'f' && argv[ai][2]) {
            fields = parse_fields(argv[ai] + 2);
        } else if (argv[ai][1] == 'f' && ai + 1 < argc) {
            fields = parse_fields(argv[++ai]);
        }
        ai++;
    }
    if (!fields) { write(2, "cut: missing -f\n", 16); exit(1); }

    if (ai >= argc) {
        process(0, delim, fields);
    } else {
        for (int i = ai; i < argc; i++) {
            int fd = open(argv[i], 0);
            if (fd < 0) { write(2, argv[i], strlen(argv[i])); write(2, ": open failed\n", 14); continue; }
            process(fd, delim, fields);
            close(fd);
        }
    }
    exit(0);
    return 0;
}
