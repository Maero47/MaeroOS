#include "../include/errno.h"
#include "../include/stdio.h"
#include "../include/string.h"
#include "../include/sys/socket.h"
#include "../include/netinet/in.h"
#include "../include/arpa/inet.h"
#include "../include/unistd.h"
#include "../include/stdlib.h"

/* sockprobe tcpshut <port>: connect to the host (10.0.2.2), shutdown(SHUT_WR),
 * then send().  The send must fail with EPIPE at once; it used to be reported
 * as "buffer full" and the blocking send slept forever. */
static int tcpshut(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        printf("sockprobe: tcp socket failed errno=%d\n", errno);
        return 1;
    }
    struct sockaddr_in host;
    host.sin_family = AF_INET;
    host.sin_port = htons((uint16_t)port);
    host.sin_addr.s_addr = inet_addr("10.0.2.2");
    if (connect(fd, (struct sockaddr *)&host, sizeof(host)) < 0) {
        printf("sockprobe: tcp connect failed errno=%d\n", errno);
        close(fd);
        return 1;
    }
    if (shutdown(fd, SHUT_WR) < 0) {
        printf("sockprobe: shutdown failed errno=%d\n", errno);
        close(fd);
        return 1;
    }
    int r = (int)send(fd, "x", 1, 0);
    int err = errno;
    close(fd);
    if (r != -1 || err != EPIPE) {
        printf("sockprobe: send after SHUT_WR returned %d errno=%d\n", r, err);
        return 1;
    }
    printf("sockprobe tcpshut ok\n");
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "tcpshut") == 0)
        return tcpshut(atoi(argv[2]));

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        printf("sockprobe: udp socket failed errno=%d\n", errno);
        return 1;
    }

    struct sockaddr_in gw;
    gw.sin_family = AF_INET;
    gw.sin_port = htons(9);
    gw.sin_addr.s_addr = inet_addr("10.0.2.2");

    if (sendto(fd, "x", 1, 0, (struct sockaddr *)&gw, sizeof(gw)) != 1) {
        printf("sockprobe: udp sendto failed errno=%d\n", errno);
        close(fd);
        return 1;
    }

    char ch;
    if (recv(fd, &ch, 1, 0) != -1 || errno != EAGAIN) {
        printf("sockprobe: empty udp recv errno=%d\n", errno);
        close(fd);
        return 1;
    }

    close(fd);
    printf("sockprobe udp ok\n");
    return 0;
}
