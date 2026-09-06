/*
 * fwctl — control the kernel firewall via /proc/firewall.
 *
 *   fwctl                 show current rules + hit counters
 *   fwctl list            (same)
 *   fwctl enable|disable
 *   fwctl flush
 *   fwctl allow|drop in|out tcp|udp|icmp|any <ip>[/bits] [lo[-hi]]
 *   fwctl policy in|out allow|drop
 *   fwctl reload          apply /etc/firewall.conf
 *
 * Writing to /proc/firewall requires root (the node is mode 0600), so most
 * subcommands are run via `doas fwctl ...`.
 */
#include "../include/stdio.h"
#include "../include/stdlib.h"
#include "../include/string.h"
#include "../include/unistd.h"
#include "../include/fcntl.h"

#define FWPROC "/proc/firewall"

static int write_line(const char *line) {
    int fd = open(FWPROC, O_WRONLY);
    if (fd < 0) {
        printf("fwctl: cannot open %s (need root? try: doas fwctl ...)\n",
               FWPROC);
        return 1;
    }
    int n = (int)strlen(line);
    int w = write(fd, line, n);
    close(fd);
    return w == n ? 0 : 1;
}

static int show(void) {
    int fd = open(FWPROC, O_RDONLY);
    char buf[2048];
    int n;
    if (fd < 0) { printf("fwctl: cannot read %s\n", FWPROC); return 1; }
    while ((n = read(fd, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = 0;
        printf("%s", buf);
    }
    close(fd);
    return 0;
}

static int reload(void) {
    const char *path = access("/etc/firewall.conf", R_OK) == 0
                       ? "/etc/firewall.conf" : "/disk/etc/firewall.conf";
    FILE *f = fopen(path, "r");
    char line[256];
    if (!f) { printf("fwctl: no %s\n", path); return 1; }
    write_line("flush\n");
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0') continue;
        write_line(line);
    }
    fclose(f);
    return 0;
}

int main(int argc, char *argv[]) {
    char line[256];

    if (argc < 2 || strcmp(argv[1], "list") == 0)
        return show();
    if (strcmp(argv[1], "reload") == 0)
        return reload();

    /* Reassemble argv[1..] into a single control line. */
    int off = 0;
    for (int i = 1; i < argc && off < (int)sizeof(line) - 2; i++) {
        int l = (int)strlen(argv[i]);
        if (i > 1) line[off++] = ' ';
        if (off + l >= (int)sizeof(line) - 1) break;
        memcpy(line + off, argv[i], l);
        off += l;
    }
    line[off++] = '\n';
    line[off] = 0;

    /* "allow"/"drop" are shorthand for "rule allow"/"rule drop". */
    if (strcmp(argv[1], "allow") == 0 || strcmp(argv[1], "drop") == 0) {
        char full[260];
        snprintf(full, sizeof(full), "rule %s", line);
        return write_line(full);
    }
    return write_line(line);
}
