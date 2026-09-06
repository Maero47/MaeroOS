#include "../include/errno.h"
#include "../include/fcntl.h"
#include "../include/stdio.h"
#include "../include/string.h"
#include "../include/unistd.h"

static int read_netif(char *buf, int cap) {
    int fd = open("/proc/netif", O_RDONLY);
    if (fd < 0) {
        perror("netprobe");
        return -1;
    }
    int n = read(fd, buf, cap - 1);
    close(fd);
    if (n < 0) {
        perror("netprobe");
        return -1;
    }
    buf[n] = '\0';
    return n;
}

int main(void) {
    char before[512];
    char after[512];

    if (read_netif(before, sizeof(before)) < 0)
        return 1;
    if (!strstr(before, "eth0: rtl8139 up")) {
        printf("netprobe: eth0 missing\n");
        return 1;
    }
    if (!strstr(before, "ip=") || strstr(before, "ip=0.0.0.0")) {
        printf("netprobe: dhcp address missing\n%s", before);
        return 1;
    }

    int fd = open("/proc/netif", O_RDWR);
    if (fd < 0) {
        perror("netprobe");
        return 1;
    }
    if (write(fd, "probe\n", 6) != 6) {
        printf("netprobe: diagnostic tx failed errno=%d\n", errno);
        close(fd);
        return 1;
    }
    close(fd);

    if (read_netif(after, sizeof(after)) < 0)
        return 1;
    if (!strstr(after, "txpkts=1") && !strstr(after, "txpkts=2") &&
        !strstr(after, "txpkts=3") && !strstr(after, "txpkts=4")) {
        printf("netprobe: tx counter did not advance\n%s", after);
        return 1;
    }

    printf("netprobe ok\n");
    return 0;
}
