#include "../include/arpa/inet.h"
#include "../include/errno.h"
#include "../include/fcntl.h"
#include "../include/netinet/in.h"
#include "../include/string.h"
#include "../include/sys/socket.h"
#include "../include/time.h"
#include "../include/unistd.h"

/*
 * Minimal DNS stub resolver: one A-record query over UDP to the server in
 * /etc/resolv.conf (default 10.0.2.3 — QEMU user-net's DNS).
 * Returns the IPv4 address in network byte order, or 0 on failure.
 */

static unsigned dotted_quad(const char *s) {
    unsigned parts[4];
    int i = 0;

    for (i = 0; i < 4; i++) {
        unsigned v = 0;
        int any = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (unsigned)(*s - '0');
            any = 1;
            s++;
        }
        if (!any || v > 255) return 0;
        parts[i] = v;
        if (i < 3) {
            if (*s != '.') return 0;
            s++;
        }
    }
    if (*s) return 0;
    return htonl((parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3]);
}

static unsigned resolver_server(void) {
    char buf[256];
    int fd = open("/etc/resolv.conf", O_RDONLY);
    int n;

    if (fd >= 0) {
        n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n > 0) {
            buf[n] = 0;
            char *p = strstr(buf, "nameserver");
            if (p) {
                p += 10;
                while (*p == ' ' || *p == '\t') p++;
                char *e = p;
                while (*e && *e != '\n' && *e != ' ') e++;
                *e = 0;
                unsigned ip = dotted_quad(p);
                if (ip) return ip;
            }
        }
    }
    return htonl(0x0A000203u);   /* 10.0.2.3 */
}

unsigned int resolve_a(const char *host) {
    unsigned char q[512], r[512];
    int qlen = 0, fd, n;
    struct sockaddr_in sa;
    unsigned ip;

    if (!host || !*host) return 0;
    ip = dotted_quad(host);
    if (ip) return ip;

    /* Header: id=0x4d4f, flags=RD, one question. */
    memset(q, 0, 12);
    q[0] = 0x4d; q[1] = 0x4f;
    q[2] = 0x01;             /* RD */
    q[5] = 0x01;             /* QDCOUNT = 1 */
    qlen = 12;

    /* QNAME: dotted labels. */
    {
        const char *p = host;
        while (*p) {
            const char *dot = p;
            int len = 0;
            while (*dot && *dot != '.') { dot++; len++; }
            if (len < 1 || len > 63 || qlen + len + 2 > 500) return 0;
            q[qlen++] = (unsigned char)len;
            memcpy(q + qlen, p, (size_t)len);
            qlen += len;
            p = *dot ? dot + 1 : dot;
        }
        q[qlen++] = 0;
    }
    q[qlen++] = 0; q[qlen++] = 1;   /* QTYPE  = A  */
    q[qlen++] = 0; q[qlen++] = 1;   /* QCLASS = IN */

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return 0;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(53);
    sa.sin_addr.s_addr = resolver_server();

    ip = 0;
    for (int attempt = 0; attempt < 3 && !ip; attempt++) {
        if (sendto(fd, q, (size_t)qlen, 0,
                   (struct sockaddr *)&sa, sizeof(sa)) < 0)
            break;
        /*
         * Wait for the reply (recvfrom is non-blocking: EAGAIN = not yet).
         * Sleep in 10ms steps so the wait is wall-clock based — a real DNS
         * round trip takes tens of ms, far longer than a yield-spin loop.
         */
        n = -1;
        for (int spin = 0; spin < 200; spin++) {
            n = recvfrom(fd, r, sizeof(r), 0, 0, 0);
            if (n >= 0 || errno != EAGAIN) break;
            struct timespec ts = { 0, 10 * 1000000L };
            nanosleep(&ts, 0);
        }
        if (n < 12 + qlen - 12) continue;
        if (r[0] != q[0] || r[1] != q[1]) continue;
        {
            int ancount = (r[6] << 8) | r[7];
            int off = qlen;            /* skip identical question section */
            for (int a = 0; a < ancount && off + 12 <= n; a++) {
                /* NAME: compression pointer or labels */
                if ((r[off] & 0xC0) == 0xC0) off += 2;
                else {
                    while (off < n && r[off]) off += r[off] + 1;
                    off++;
                }
                if (off + 10 > n) break;
                int type = (r[off] << 8) | r[off + 1];
                int rdlen = (r[off + 8] << 8) | r[off + 9];
                off += 10;
                if (type == 1 && rdlen == 4 && off + 4 <= n) {
                    memcpy(&ip, r + off, 4);   /* already network order */
                    break;
                }
                off += rdlen;
            }
        }
    }
    close(fd);
    return ip;
}
