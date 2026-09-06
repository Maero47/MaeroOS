/* free — memory usage from /proc/meminfo. */
#include "../include/stdio.h"
#include "../include/fcntl.h"
#include "../include/unistd.h"
#include "../include/string.h"

static long field(const char *buf, const char *key) {
    const char *p = strstr(buf, key);
    if (!p) return -1;
    p += strlen(key);
    while (*p == ' ' || *p == '\t' || *p == ':') p++;
    long v = 0;
    while (*p >= '0' && *p <= '9') v = v*10 + (*p++ - '0');
    return v;
}

int main(void) {
    char buf[1024] = {0};
    int fd = open("/proc/meminfo", O_RDONLY);
    if (fd < 0) { printf("free: /proc/meminfo unavailable\n"); return 1; }
    int n = read(fd, buf, sizeof(buf)-1); close(fd);
    if (n < 0) n = 0; buf[n] = 0;
    long total = field(buf, "MemTotal");
    long freem = field(buf, "MemFree");
    long avail = field(buf, "MemAvailable");
    if (avail < 0) avail = freem;
    printf("%14s %10s %10s %10s\n", "total", "used", "free", "available");
    printf("Mem: %9ld %10ld %10ld %10ld\n",
           total, total - freem, freem, avail);
    return 0;
}
