#include "../include/arpa/inet.h"
#include "../include/errno.h"
#include "../include/netinet/in.h"
#include "../include/stdio.h"
#include "../include/string.h"
#include "../include/sys/socket.h"
#include "../include/time.h"
#include "../include/unistd.h"

static int parse_port(const char *s) {
    int port = 0;
    while (*s >= '0' && *s <= '9') {
        port = port * 10 + (*s - '0');
        s++;
    }
    if (*s || port <= 0 || port > 65535)
        return -1;
    return port;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        printf("usage: httpget <ip> <port> <path>\n");
        return 1;
    }

    int port = parse_port(argv[2]);
    if (port < 0) {
        printf("httpget: bad port\n");
        return 1;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        printf("httpget: socket errno=%d\n", errno);
        return 1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    /* Accepts dotted quads directly; hostnames go through DNS. */
    addr.sin_addr.s_addr = resolve_a(argv[1]);
    if (!addr.sin_addr.s_addr) {
        printf("httpget: cannot resolve %s\n", argv[1]);
        close(fd);
        return 1;
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        printf("httpget: connect errno=%d\n", errno);
        close(fd);
        return 1;
    }

    char req[256];
    snprintf(req, sizeof(req),
             "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n",
             argv[3], argv[1]);
    int req_len = (int)strlen(req);
    int off = 0;
    while (off < req_len) {
        int n = send(fd, req + off, (size_t)(req_len - off), 0);
        if (n < 0) {
            printf("httpget: send errno=%d\n", errno);
            close(fd);
            return 1;
        }
        off += n;
    }

    char buf[256];
    int idle = 0;
    for (;;) {
        int n = recv(fd, buf, sizeof(buf), 0);
        if (n > 0) {
            write(1, buf, n);
            idle = 0;
            continue;
        }
        if (n == 0)
            break;
        if (errno == EAGAIN) {
            /* Non-blocking socket: wait wall-clock for data, not yield spins
             * (an internet round trip outlasts thousands of yields). */
            if (++idle > 800)
                break;
            struct timespec ts = { 0, 10 * 1000000L };
            nanosleep(&ts, 0);
            continue;
        }
        printf("httpget: recv errno=%d\n", errno);
        close(fd);
        return 1;
    }

    close(fd);
    return 0;
}
