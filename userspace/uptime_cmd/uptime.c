/* uptime — seconds since boot from /proc/uptime. */
#include "../include/stdio.h"
#include "../include/fcntl.h"
#include "../include/unistd.h"

int main(void) {
    char buf[64] = {0};
    int fd = open("/proc/uptime", O_RDONLY);
    long secs = -1;
    if (fd >= 0) {
        int n = read(fd, buf, 63); close(fd);
        if (n > 0) { buf[n]=0; secs = 0; for (int i=0; buf[i] && buf[i]!='.' && buf[i]!=' '; i++) if (buf[i]>='0'&&buf[i]<='9') secs = secs*10 + (buf[i]-'0'); }
    }
    if (secs < 0) { printf("uptime: unavailable\n"); return 1; }
    long h = secs/3600, m = (secs/60)%60, s = secs%60;
    printf("up %ldh %ldm %lds\n", h, m, s);
    return 0;
}
