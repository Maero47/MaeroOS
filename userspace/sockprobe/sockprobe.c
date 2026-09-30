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

/* Connect a new TCP socket to the host's HTTP port; -1 on failure. */
static int http_connect(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_in host;
    host.sin_family = AF_INET;
    host.sin_port = htons((uint16_t)port);
    host.sin_addr.s_addr = inet_addr("10.0.2.2");
    if (connect(fd, (struct sockaddr *)&host, sizeof(host)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* HTTP/1.0 GET on a connected socket: request, shutdown(SHUT_WR), read to
 * EOF.  Our FIN goes first, so the pcb ends in TIME_WAIT.  1 when the reply
 * was read to EOF and contained MAEROS_HTTP_OK.  The fd stays open. */
static int http_halfclose(int fd) {
    static const char req[] = "GET /index.html HTTP/1.0\r\n\r\n";
    if (send(fd, req, sizeof(req) - 1, 0) != (int)sizeof(req) - 1 ||
        shutdown(fd, SHUT_WR) < 0)
        return 0;
    char buf[1024];
    int n, total = 0;
    while ((n = recv(fd, buf + total, sizeof(buf) - 1 - total, 0)) > 0) {
        total += n;
        if (total >= (int)sizeof(buf) - 1)
            total = 0;              /* keep only the tail; the body is short */
    }
    buf[total] = 0;
    return n == 0 && strstr(buf, "MAEROS_HTTP_OK") != NULL;
}

/* sockprobe tcptw <port> <n>: n half-closed fetches whose fds stay open while
 * their pcbs sit in TIME_WAIT, which lwIP frees without an err callback.
 * With n = MEMP_NUM_TCP_PCB (16), the next socket's pcb can only come from
 * recycling one of them (tcp_kill_timewait).  All the old fds are closed
 * while that new connection is live; a socket that still held its freed pcb
 * would detach and close the new connection's pcb.  The new connection must
 * then complete a fetch. */
static int tcptw(int port, int n) {
    int fds[32];
    if (n > 32) n = 32;
    for (int i = 0; i < n; i++) {
        fds[i] = http_connect(port);
        if (fds[i] < 0 || !http_halfclose(fds[i])) {
            printf("sockprobe: half-closed fetch %d failed errno=%d\n", i, errno);
            return 1;
        }
    }
    int fd = http_connect(port);
    if (fd < 0) {
        printf("sockprobe: connect with the pcb pool full failed errno=%d\n", errno);
        return 1;
    }
    for (int i = 0; i < n; i++)
        close(fds[i]);
    int ok = http_halfclose(fd);
    close(fd);
    if (!ok) {
        printf("sockprobe: fetch on a recycled pcb failed after closing TIME_WAIT sockets\n");
        return 1;
    }
    printf("sockprobe tcptw ok\n");
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "tcpshut") == 0)
        return tcpshut(atoi(argv[2]));
    if (argc == 4 && strcmp(argv[1], "tcptw") == 0)
        return tcptw(atoi(argv[2]), atoi(argv[3]));

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
