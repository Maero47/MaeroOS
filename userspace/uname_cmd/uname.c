/* uname — system info (subset of coreutils flags). */
#include "../include/stdio.h"
#include "../include/string.h"
#include "../include/fcntl.h"
#include "../include/unistd.h"

static void read_first(const char *path, char *out, int cap) {
    out[0] = 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return;
    int n = read(fd, out, cap - 1);
    close(fd);
    if (n < 0) n = 0;
    out[n] = 0;
    for (int i = 0; out[i]; i++) if (out[i] == '\n') { out[i] = 0; break; }
}

int main(int argc, char *argv[]) {
    char host[64];
    read_first("/etc/hostname", host, sizeof(host));
    if (!host[0]) strcpy(host, "maeros");

    int all = 0, s = 0, n = 0, r = 0, m = 0, o = 0, any = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-a")) all = 1;
        else if (!strcmp(argv[i], "-s")) s = 1;
        else if (!strcmp(argv[i], "-n")) n = 1;
        else if (!strcmp(argv[i], "-r")) r = 1;
        else if (!strcmp(argv[i], "-m")) m = 1;
        else if (!strcmp(argv[i], "-o")) o = 1;
        any = 1;
    }
    if (!any) s = 1;
    char line[256]; int p = 0;
    if (all || s) p += snprintf(line+p, sizeof(line)-p, "MaeroOS ");
    if (all || n) p += snprintf(line+p, sizeof(line)-p, "%s ", host);
    if (all || r) p += snprintf(line+p, sizeof(line)-p, "1.0 ");
    if (all || m) p += snprintf(line+p, sizeof(line)-p, "i686 ");
    if (all || o) p += snprintf(line+p, sizeof(line)-p, "MaeroOS ");
    if (p && line[p-1] == ' ') line[p-1] = 0;
    printf("%s\n", line);
    return 0;
}
