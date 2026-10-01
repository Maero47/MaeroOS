/*
 * srvprobe: AF_INET server sockets, driven by tools/smoke_tcpsrv.py through
 * QEMU user-net hostfwd (the host connects to 127.0.0.1:<fwd> and slirp
 * connects on to 10.0.2.15:<port>).
 *
 *   srvprobe tcp <port>   listen/accept: non-blocking accept is EAGAIN, a
 *                         blocking one times out per SO_RCVTIMEO, poll and
 *                         epoll report the listener readable, several host
 *                         clients wait in the backlog at once and are taken
 *                         with accept4(SOCK_NONBLOCK|SOCK_CLOEXEC), each
 *                         served one echo line; then the port is rebound
 *                         (EADDRINUSE without SO_REUSEADDR while the closed
 *                         connections sit in TIME_WAIT, fine with it) to
 *                         the guest's own address and one more client is
 *                         accepted with a blocking accept.
 *   srvprobe udp <port>   a blocking recvfrom: SO_RCVTIMEO ends an idle wait
 *                         with EAGAIN, MSG_DONTWAIT does at once, and
 *                         without a timeout it waits for the host's
 *                         datagram, which is echoed back.
 *
 * The host waits for the "srvprobe: ..." lines before each step.
 */
#include "../include/errno.h"
#include "../include/stdio.h"
#include "../include/string.h"
#include "../include/stdlib.h"
#include "../include/unistd.h"
#include "../include/fcntl.h"
#include "../include/poll.h"
#include "../include/time.h"
#include "../include/syscall.h"
#include "../include/sys/socket.h"
#include "../include/sys/time.h"
#include "../include/netinet/in.h"
#include "../include/netinet/tcp.h"
#include "../include/arpa/inet.h"

#define NCLIENTS 3

static int fails;

static void check(const char *what, int ok, int detail) {
    if (!ok) {
        printf("srvprobe: FAIL %s (%d, errno=%d)\n", what, detail, errno);
        fails++;
    }
}

static long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

static int set_int(int fd, int level, int name, int v) {
    return setsockopt(fd, level, name, &v, sizeof(v));
}

static int get_int(int fd, int level, int name) {
    int v = -1;
    socklen_t l = sizeof(v);
    if (getsockopt(fd, level, name, &v, &l) < 0 || l != sizeof(v))
        return -1;
    return v;
}

static int set_timeo(int fd, int name, int ms) {
    struct timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    return setsockopt(fd, SOL_SOCKET, name, &tv, sizeof(tv));
}

static int bind_to(int fd, const char *ip, int port) {
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = ip ? inet_addr(ip) : htonl(INADDR_ANY);
    return bind(fd, (struct sockaddr *)&a, sizeof(a));
}

/* epoll through raw i386 syscalls (this libc has no wrappers). */
struct ep_event { uint32_t events; uint32_t data_lo, data_hi; } __attribute__((packed));

static int epoll_readable(int fd, int timeout_ms) {
    int ep = syscall1(329, 0);                            /* epoll_create1 */
    if (ep < 0) return -1;
    struct ep_event ev = { POLLIN, (uint32_t)fd, 0 }, out;
    int r = syscall4(255, ep, 1 /*EPOLL_CTL_ADD*/, fd, (int)&ev);
    if (r == 0)
        r = syscall4(256, ep, (int)&out, 1, timeout_ms);  /* epoll_wait */
    close(ep);
    return r == 1 && (out.events & POLLIN) && out.data_lo == (uint32_t)fd;
}

/* Read one '\n'-terminated line from a non-blocking socket, polling. */
static int read_line(int fd, char *buf, int cap) {
    int n = 0;
    long end = now_ms() + 10000;
    while (n < cap - 1 && now_ms() < end) {
        struct pollfd p = { fd, POLLIN, 0 };
        if (poll(&p, 1, 1000) <= 0) continue;
        int r = (int)recv(fd, buf + n, (size_t)(cap - 1 - n), 0);
        if (r <= 0) break;
        n += r;
        if (memchr(buf, '\n', (size_t)n)) break;
    }
    buf[n] = 0;
    return n;
}

/* Answer a client's line with "ECHO <line>". */
static int serve_echo(int c) {
    char line[128], reply[160];
    int n = read_line(c, line, sizeof(line));
    if (n <= 0) return -1;
    int m = snprintf(reply, sizeof(reply), "ECHO %s", line);
    return (int)send(c, reply, (size_t)m, 0) == m ? 0 : -1;
}

static int tcp_probe(int port) {
    struct sockaddr_in a;
    socklen_t al;

    /* listen() on UDP is EOPNOTSUPP. */
    int u = socket(AF_INET, SOCK_DGRAM, 0);
    check("listen on a UDP socket is EOPNOTSUPP",
          listen(u, 1) == -1 && errno == EOPNOTSUPP, 0);
    close(u);

    /* Binding an address that is not this host's is EADDRNOTAVAIL. */
    int s = socket(AF_INET, SOCK_STREAM, 0);
    check("bind to a foreign address is EADDRNOTAVAIL",
          bind_to(s, "10.9.9.9", port) == -1 && errno == 99, 0);
    close(s);

    s = socket(AF_INET, SOCK_STREAM, 0);
    check("SO_REUSEADDR", set_int(s, SOL_SOCKET, SO_REUSEADDR, 1) == 0, 0);
    check("SO_REUSEADDR reads back 1", get_int(s, SOL_SOCKET, SO_REUSEADDR) == 1, 0);
    check("TCP_NODELAY", set_int(s, IPPROTO_TCP, TCP_NODELAY, 1) == 0 &&
          get_int(s, IPPROTO_TCP, TCP_NODELAY) == 1, 0);
    check("SO_KEEPALIVE", set_int(s, SOL_SOCKET, SO_KEEPALIVE, 1) == 0 &&
          get_int(s, SOL_SOCKET, SO_KEEPALIVE) == 1, 0);
    check("bind 0.0.0.0", bind_to(s, NULL, port) == 0, 0);
    check("a second bind is EINVAL", bind_to(s, NULL, port + 1) == -1 && errno == EINVAL, 0);
    check("SO_ACCEPTCONN 0 before listen", get_int(s, SOL_SOCKET, SO_ACCEPTCONN) == 0, 0);
    check("listen", listen(s, NCLIENTS + 1) == 0, 0);
    check("SO_ACCEPTCONN 1 after listen", get_int(s, SOL_SOCKET, SO_ACCEPTCONN) == 1, 0);
    al = sizeof(a);
    check("getsockname on the listener",
          getsockname(s, (struct sockaddr *)&a, &al) == 0 && a.sin_port == htons((uint16_t)port), 0);
    check("getpeername on the listener is ENOTCONN",
          getpeername(s, (struct sockaddr *)&a, &al) == -1 && errno == ENOTCONN, 0);
    char ch;
    check("recv on the listener is ENOTCONN",
          recv(s, &ch, 1, 0) == -1 && errno == ENOTCONN, 0);

    /* Nothing queued: SO_RCVTIMEO ends a blocking accept with EAGAIN. */
    set_timeo(s, SO_RCVTIMEO, 300);
    long t0 = now_ms();
    int r = accept(s, NULL, NULL);
    int e = errno;                  /* this libc clears errno on success */
    long dt = now_ms() - t0;
    check("blocking accept with SO_RCVTIMEO times out with EAGAIN",
          r == -1 && e == EAGAIN && dt >= 250 && dt < 2000, (int)dt);
    set_timeo(s, SO_RCVTIMEO, 0);

    fcntl(s, F_SETFL, O_NONBLOCK);
    check("non-blocking accept with nothing queued is EAGAIN",
          accept4(s, NULL, NULL, SOCK_NONBLOCK) == -1 && errno == EAGAIN, 0);
    struct pollfd p = { s, POLLIN, 0 };
    check("idle listener does not poll readable", poll(&p, 1, 0) == 0, p.revents);

    printf("srvprobe: listening\n");

    /* The host now connects NCLIENTS times at once; wait until they all sit
     * in the backlog before taking any. */
    p.revents = 0;
    r = poll(&p, 1, 20000);
    check("poll reports the listener readable", r == 1 && (p.revents & POLLIN), p.revents);
    check("epoll reports the listener readable", epoll_readable(s, 5000) == 1, 0);
    int c[NCLIENTS];
    int got = 0;
    long end = now_ms() + 15000;
    while (got < NCLIENTS && now_ms() < end) {
        struct sockaddr_in peer;
        socklen_t pl = sizeof(peer);
        /* Give every client time to queue before taking the first. */
        if (got == 0) usleep(1500000);
        int fd = accept4(s, (struct sockaddr *)&peer, &pl, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno != EAGAIN) { check("accept4", 0, fd); break; }
            if (got == 0) { usleep(100000); continue; }
            /* Linux-style backlog: the queue held more than one at once. */
            break;
        }
        check("accept4 peer is the slirp host", pl == sizeof(peer) &&
              peer.sin_family == AF_INET && peer.sin_addr.s_addr == inet_addr("10.0.2.2"), (int)pl);
        struct sockaddr_in gp, me;
        socklen_t gl = sizeof(gp);
        check("getpeername matches accept's address",
              getpeername(fd, (struct sockaddr *)&gp, &gl) == 0 &&
              gp.sin_port == peer.sin_port && gp.sin_addr.s_addr == peer.sin_addr.s_addr, 0);
        gl = sizeof(me);
        check("getsockname of an accepted socket is the listening port",
              getsockname(fd, (struct sockaddr *)&me, &gl) == 0 &&
              me.sin_port == htons((uint16_t)port) &&
              me.sin_addr.s_addr == inet_addr("10.0.2.15"), ntohs(me.sin_port));
        check("accept4 SOCK_NONBLOCK", (fcntl(fd, F_GETFL) & O_NONBLOCK) != 0, 0);
        check("accept4 SOCK_CLOEXEC", (fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0, 0);
        check("accepted socket inherits TCP_NODELAY", get_int(fd, IPPROTO_TCP, TCP_NODELAY) == 1, 0);
        c[got++] = fd;
    }
    check("all clients waited in the backlog together", got == NCLIENTS, got);
    for (int i = 0; i < got; i++) {
        check("echo to a client", serve_echo(c[i]) == 0, i);
    }
    /* Our side closes first, so these connections end in TIME_WAIT. */
    for (int i = 0; i < got; i++)
        close(c[i]);
    usleep(500000);
    close(s);
    printf("srvprobe: served %d\n", got);

    /* Rebind the port: TIME_WAIT connections hold it unless SO_REUSEADDR. */
    s = socket(AF_INET, SOCK_STREAM, 0);
    check("rebind without SO_REUSEADDR is EADDRINUSE",
          bind_to(s, NULL, port) == -1 && errno == EADDRINUSE, 0);
    close(s);
    s = socket(AF_INET, SOCK_STREAM, 0);
    set_int(s, SOL_SOCKET, SO_REUSEADDR, 1);
    check("rebind with SO_REUSEADDR to the guest's address",
          bind_to(s, "10.0.2.15", port) == 0, 0);
    check("listen after rebind", listen(s, 1) == 0, 0);
    printf("srvprobe: rebound\n");
    struct sockaddr_in peer;
    socklen_t pl = sizeof(peer);
    int fd = accept(s, (struct sockaddr *)&peer, &pl);       /* blocking */
    check("blocking accept on the rebound listener", fd >= 0, fd);
    check("accept without SOCK_NONBLOCK is blocking", fd >= 0 &&
          (fcntl(fd, F_GETFL) & O_NONBLOCK) == 0, 0);
    if (fd >= 0) {
        char line[128], reply[160];
        int n = 0;
        while (n < (int)sizeof(line) - 1) {          /* blocking recv */
            int k = (int)recv(fd, line + n, sizeof(line) - 1 - (size_t)n, 0);
            if (k <= 0) break;
            n += k;
            if (memchr(line, '\n', (size_t)n)) break;
        }
        line[n] = 0;
        int m = snprintf(reply, sizeof(reply), "ECHO %s", line);
        check("blocking echo", n > 0 && send(fd, reply, (size_t)m, 0) == m, n);
        /* The peer closes: recv reports EOF. */
        check("EOF after the client closes", recv(fd, line, sizeof(line), 0) == 0, 0);
        close(fd);
    }
    /* shutdown() on a listener stops it: accept is then EINVAL. */
    check("shutdown(SHUT_RD) on a listener", shutdown(s, SHUT_RD) == 0, 0);
    check("accept after shutdown is EINVAL", accept(s, NULL, NULL) == -1 && errno == EINVAL, 0);
    close(s);

    if (fails) {
        printf("srvprobe tcp: %d failures\n", fails);
        return 1;
    }
    printf("srvprobe tcp ok\n");
    return 0;
}

static int udp_probe(int port) {
    int u = socket(AF_INET, SOCK_DGRAM, 0);
    check("udp bind", bind_to(u, NULL, port) == 0, 0);
    char buf[256];
    struct sockaddr_in from;
    socklen_t fl = sizeof(from);

    check("MSG_DONTWAIT recvfrom on an empty socket is EAGAIN",
          recvfrom(u, buf, sizeof(buf), MSG_DONTWAIT, NULL, NULL) == -1 && errno == EAGAIN, 0);
    check("SO_RCVTIMEO", set_timeo(u, SO_RCVTIMEO, 400) == 0, 0);
    struct timeval tv;
    socklen_t tl = sizeof(tv);
    check("SO_RCVTIMEO reads back", getsockopt(u, SOL_SOCKET, SO_RCVTIMEO, &tv, &tl) == 0 &&
          tl == sizeof(tv) && tv.tv_sec == 0 && tv.tv_usec == 400000, (int)tv.tv_usec);
    long t0 = now_ms();
    int r = (int)recvfrom(u, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
    int e = errno;
    long dt = now_ms() - t0;
    check("blocking recvfrom times out with EAGAIN per SO_RCVTIMEO",
          r == -1 && e == EAGAIN && dt >= 350 && dt < 3000, (int)dt);

    /* No timeout: wait for the host's datagram, however long it takes. */
    set_timeo(u, SO_RCVTIMEO, 0);
    printf("srvprobe: udp ready\n");
    fl = sizeof(from);
    r = (int)recvfrom(u, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from, &fl);
    check("blocking recvfrom got the datagram", r > 0, r);
    if (r > 0) {
        buf[r] = 0;
        char reply[300];
        int m = snprintf(reply, sizeof(reply), "UDPECHO %s", buf);
        check("udp reply", sendto(u, reply, (size_t)m, 0, (struct sockaddr *)&from, fl) == m, 0);
    }
    close(u);
    if (fails) {
        printf("srvprobe udp: %d failures\n", fails);
        return 1;
    }
    printf("srvprobe udp ok\n");
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "tcp") == 0)
        return tcp_probe(atoi(argv[2]));
    if (argc == 3 && strcmp(argv[1], "udp") == 0)
        return udp_probe(atoi(argv[2]));
    printf("usage: srvprobe tcp|udp <port>\n");
    return 2;
}
