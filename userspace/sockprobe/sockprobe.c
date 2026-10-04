#include "../include/errno.h"
#include "../include/stdio.h"
#include "../include/signal.h"
#include "../include/string.h"
#include "../include/sys/socket.h"
#include "../include/netinet/in.h"
#include "../include/arpa/inet.h"
#include "../include/unistd.h"
#include "../include/stdlib.h"
#include "../include/fcntl.h"
#include "../include/poll.h"
#include "../include/syscall.h"
#include "../include/sys/uio.h"
#include "../include/time.h"

/* sockprobe tcpshut <port>: connect to the host (10.0.2.2), shutdown(SHUT_WR),
 * then send().  The send must fail with EPIPE at once; it used to be reported
 * as "buffer full" and the blocking send slept forever.  As on Linux it also
 * raises SIGPIPE, which would kill the probe, so that is ignored here. */
static int tcpshut(int port) {
    signal(SIGPIPE, SIG_IGN);
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
        int e = errno;          /* close() clears errno on success */
        close(fd);
        errno = e;
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

/* sockprobe tcptw <port> <n>: n half-closed fetches, whose pcbs then sit in
 * TIME_WAIT, which lwIP frees without an err callback.  The first KEEP of them
 * keep their fds open (they are the oldest TIME_WAIT pcbs); the rest are
 * closed at once.  With n = MEMP_NUM_TCP_PCB (64), the next socket's pcb can
 * only come from recycling the oldest TIME_WAIT pcb (tcp_kill_timewait) - one
 * whose fd is still open.  All the old fds are closed while that new
 * connection is live; a socket that still held its freed pcb would detach and
 * close the new connection's pcb.  The new connection must then complete a
 * fetch. */
#define TCPTW_KEEP 24
static int tcptw(int port, int n) {
    int fds[TCPTW_KEEP];
    int keep = n < TCPTW_KEEP ? n : TCPTW_KEEP;
    for (int i = 0; i < n; i++) {
        int fd = http_connect(port);
        if (fd < 0 || !http_halfclose(fd)) {
            printf("sockprobe: half-closed fetch %d failed errno=%d\n", i, errno);
            return 1;
        }
        if (i < keep) fds[i] = fd;
        else close(fd);
    }
    int fd = http_connect(port);
    if (fd < 0) {
        printf("sockprobe: connect with the pcb pool full failed errno=%d\n", errno);
        return 1;
    }
    for (int i = 0; i < keep; i++)
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

/* sockprobe shutrd <port> <nbytes>: the host server greets on connect; leave
 * the greeting unread, shutdown(SHUT_RD), send nbytes, then shutdown(SHUT_WR).
 * The second shutdown completes a two-step close and must still be orderly:
 * the host must receive all nbytes and then a FIN.  With unread data in the
 * ring, closing the pcb without sending the FIN first was lwIP's abortive
 * close (RST, queued data purged).  The host side checks the byte count. */
static int shutrd(int port, int nbytes) {
    int fd = http_connect(port);
    if (fd < 0) {
        printf("sockprobe: connect failed errno=%d\n", errno);
        return 1;
    }
    usleep(1000000);                /* let the greeting arrive, unread */
    if (shutdown(fd, SHUT_RD) < 0) {
        printf("sockprobe: shutdown(SHUT_RD) failed errno=%d\n", errno);
        close(fd);
        return 1;
    }
    static char chunk[1024];
    for (int i = 0; i < (int)sizeof(chunk); i++)
        chunk[i] = (char)('a' + i % 26);
    int sent = 0;
    while (sent < nbytes) {
        int n = nbytes - sent < (int)sizeof(chunk) ? nbytes - sent : (int)sizeof(chunk);
        int r = (int)send(fd, chunk, (size_t)n, 0);
        if (r <= 0) {
            printf("sockprobe: send failed at %d errno=%d\n", sent, errno);
            close(fd);
            return 1;
        }
        sent += r;
    }
    if (shutdown(fd, SHUT_WR) < 0) {
        printf("sockprobe: shutdown(SHUT_WR) failed errno=%d\n", errno);
        close(fd);
        return 1;
    }
    close(fd);
    printf("sockprobe shutrd sent %d\n", sent);
    return 0;
}


/* socketcall(102) directly, for the calls this libc does not wrap. */
static int sockcall(int call, uint32_t *args) {
    int r = syscall2(102, call, (int)(uintptr_t)args);
    if (r < 0) { errno = -r; return -1; }
    return r;
}
static int getname(int fd, int peer, struct sockaddr_in *sa) {
    uint32_t len = sizeof(*sa);
    uint32_t a[3] = { (uint32_t)fd, (uint32_t)(uintptr_t)sa, (uint32_t)(uintptr_t)&len };
    memset(sa, 0, sizeof(*sa));
    return sockcall(peer ? 7 : 6, a);
}
static int so_error(int fd) {
    int v = -1;
    uint32_t len = sizeof(v);
    uint32_t a[5] = { (uint32_t)fd, 1 /*SOL_SOCKET*/, 4 /*SO_ERROR*/,
                      (uint32_t)(uintptr_t)&v, (uint32_t)(uintptr_t)&len };
    return sockcall(15, a) < 0 ? -1 : v;
}

/* Start a non-blocking connect to 10.0.2.2:port and wait for it to settle.
 * Returns the fd (-1 on a setup failure) and the poll revents in *rev. */
static int nb_connect(int port, short *rev) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0 || fcntl(fd, F_SETFL, O_NONBLOCK) < 0)
        return -1;
    struct sockaddr_in host;
    host.sin_family = AF_INET;
    host.sin_port = htons((uint16_t)port);
    host.sin_addr.s_addr = inet_addr("10.0.2.2");
    if (connect(fd, (struct sockaddr *)&host, sizeof(host)) != -1 ||
        errno != 115 /*EINPROGRESS*/) {
        printf("sockprobe nb: connect did not report EINPROGRESS errno=%d\n", errno);
        close(fd);
        return -1;
    }
    struct pollfd p = { fd, POLLOUT, 0 };
    if (poll(&p, 1, 5000) != 1) {
        printf("sockprobe nb: connect never became writable\n");
        close(fd);
        return -1;
    }
    *rev = p.revents;
    return fd;
}

/* sockprobe nb <port> <closed-port>: the socket calls a non-blocking client
 * (NSPR, GLib) is built on.  connect returns EINPROGRESS and completes behind
 * the caller (POLLOUT, SO_ERROR 0); getsockname/getpeername name both ends;
 * recv on an idle connection is EAGAIN instead of sleeping, also with
 * MSG_DONTWAIT on a blocking socket; a refused connect is POLLERR with
 * SO_ERROR ECONNREFUSED.  Firefox's socket thread parked for good in the
 * blocking recv, and dropped every connection whose getsockname failed. */
static int nb(int port, int closed_port) {
    struct sockaddr_in sa;
    short rev = 0;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0 || getname(fd, 1, &sa) != -1 || errno != ENOTCONN) {
        printf("sockprobe nb: getpeername before connect errno=%d\n", errno);
        return 1;
    }
    close(fd);

    fd = nb_connect(port, &rev);
    if (fd < 0) return 1;
    int e = so_error(fd);
    if (!(rev & POLLOUT) || (rev & POLLERR) || e != 0) {
        printf("sockprobe nb: connect settled revents=%x SO_ERROR=%d\n", rev, e);
        return 1;
    }
    if (getname(fd, 0, &sa) < 0 || sa.sin_family != AF_INET ||
        sa.sin_addr.s_addr != inet_addr("10.0.2.15") || sa.sin_port == 0) {
        printf("sockprobe nb: getsockname errno=%d family=%d addr=%x port=%d\n",
               errno, sa.sin_family, (unsigned)sa.sin_addr.s_addr, ntohs(sa.sin_port));
        return 1;
    }
    if (getname(fd, 1, &sa) < 0 || sa.sin_addr.s_addr != inet_addr("10.0.2.2") ||
        ntohs(sa.sin_port) != port) {
        printf("sockprobe nb: getpeername errno=%d addr=%x port=%d\n",
               errno, (unsigned)sa.sin_addr.s_addr, ntohs(sa.sin_port));
        return 1;
    }
    char buf[512];
    if (recv(fd, buf, sizeof(buf), 0) != -1 || errno != EAGAIN) {
        printf("sockprobe nb: idle O_NONBLOCK recv errno=%d\n", errno);
        return 1;
    }
    static const char req[] = "GET /index.html HTTP/1.0\r\n\r\n";
    if (send(fd, req, sizeof(req) - 1, 0) != (int)sizeof(req) - 1) {
        printf("sockprobe nb: send errno=%d\n", errno);
        return 1;
    }
    int total = 0, n;
    for (;;) {
        struct pollfd p = { fd, POLLIN, 0 };
        if (poll(&p, 1, 5000) != 1) break;
        n = (int)recv(fd, buf + total, sizeof(buf) - 1 - total, 0);
        if (n <= 0) break;
        total += n;
    }
    buf[total] = 0;
    close(fd);
    if (!strstr(buf, "MAEROS_HTTP_OK")) {
        printf("sockprobe nb: reply not received (%d bytes)\n", total);
        return 1;
    }

    /* MSG_DONTWAIT on a blocking socket. */
    fd = http_connect(port);
    if (fd < 0 || recv(fd, buf, sizeof(buf), 0x40 /*MSG_DONTWAIT*/) != -1 ||
        errno != EAGAIN) {
        printf("sockprobe nb: MSG_DONTWAIT recv errno=%d\n", errno);
        return 1;
    }
    close(fd);

    fd = nb_connect(closed_port, &rev);
    if (fd < 0) return 1;
    e = so_error(fd);
    int again = so_error(fd);
    close(fd);
    if (!(rev & POLLERR) || e != 111 /*ECONNREFUSED*/ || again != 0) {
        printf("sockprobe nb: refused connect revents=%x SO_ERROR=%d then %d\n",
               rev, e, again);
        return 1;
    }
    printf("sockprobe nb ok\n");
    return 0;
}

/* sockprobe many <port> <ntcp> <nudp>: hold ntcp connected TCP sockets and
 * nudp UDP sockets open at once, well past the 32 slots the kernel socket
 * table used to have, then use every one of them: a datagram out of each UDP
 * socket and an HTTP fetch on each TCP connection, oldest first so the first
 * connections have idled while the rest were opened. */
#define MANY_MAX 120
static int many(int port, int ntcp, int nudp) {
    static int tfd[MANY_MAX], ufd[MANY_MAX];
    if (ntcp > MANY_MAX || nudp > MANY_MAX) {
        printf("sockprobe: many: at most %d of each\n", MANY_MAX);
        return 1;
    }
    long worst_us = 0;
    int slow = 0;
    for (int i = 0; i < ntcp; i++) {
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        tfd[i] = http_connect(port);
        int e = errno;
        clock_gettime(CLOCK_MONOTONIC, &t1);
        long us = (t1.tv_sec - t0.tv_sec) * 1000000L + (t1.tv_nsec - t0.tv_nsec) / 1000;
        if (us > worst_us) worst_us = us;
        if (us > 500000) slow++;
        if (tfd[i] < 0) {
            printf("sockprobe: many: tcp connect %d failed errno=%d after %ld ms\n",
                   i, e, us / 1000);
            return 1;
        }
    }
    printf("sockprobe many: slowest connect %ld ms, %d over 500 ms\n", worst_us / 1000, slow);
    for (int i = 0; i < nudp; i++) {
        ufd[i] = socket(AF_INET, SOCK_DGRAM, 0);
        if (ufd[i] < 0) {
            printf("sockprobe: many: udp socket %d failed errno=%d\n", i, errno);
            return 1;
        }
    }
    struct sockaddr_in discard;
    discard.sin_family = AF_INET;
    discard.sin_port = htons(9);
    discard.sin_addr.s_addr = inet_addr("10.0.2.2");
    for (int i = 0; i < nudp; i++) {
        if (sendto(ufd[i], "x", 1, 0, (struct sockaddr *)&discard,
                   sizeof(discard)) != 1) {
            printf("sockprobe: many: udp sendto %d failed errno=%d\n", i, errno);
            return 1;
        }
    }
    for (int i = 0; i < ntcp; i++) {
        if (!http_halfclose(tfd[i])) {
            printf("sockprobe: many: fetch on tcp socket %d failed errno=%d\n", i, errno);
            return 1;
        }
    }
    for (int i = 0; i < ntcp; i++)
        close(tfd[i]);
    for (int i = 0; i < nudp; i++)
        close(ufd[i]);
    printf("sockprobe many ok %d\n", ntcp + nudp);
    return 0;
}

/* sendmsg/recvmsg on AF_INET (socketcall 16/17; this libc wraps neither).
 * i386 struct msghdr and struct iovec, by hand. */
struct kmsghdr {
    void *name; uint32_t namelen;
    struct iovec *iov; uint32_t iovlen;
    void *control; uint32_t controllen;
    int flags;
};
static int xsendmsg(int fd, struct kmsghdr *m, int flags) {
    uint32_t a[3] = { (uint32_t)fd, (uint32_t)(uintptr_t)m, (uint32_t)flags };
    return sockcall(16, a);
}
static int xrecvmsg(int fd, struct kmsghdr *m, int flags) {
    uint32_t a[3] = { (uint32_t)fd, (uint32_t)(uintptr_t)m, (uint32_t)flags };
    return sockcall(17, a);
}

/* sockprobe msg <http_port> <udp_echo_port>: a TCP request gathered from two
 * iovecs and its reply scattered over two; a UDP datagram sent to msg_name
 * from two iovecs and its echo received with the source in msg_name.  Both
 * used to be EINVAL. */
static int msgprobe(int port, int uport) {
    int fd = http_connect(port);
    if (fd < 0) { printf("sockprobe: msg connect failed errno=%d\n", errno); return 1; }
    char l1[] = "GET /index.html HTTP/1.0\r\n", l2[] = "\r\n";
    struct iovec siov[2] = { { l1, sizeof(l1) - 1 }, { l2, 2 } };
    struct kmsghdr m;
    memset(&m, 0, sizeof(m));
    m.iov = siov; m.iovlen = 2;
    int r = xsendmsg(fd, &m, 0);
    if (r != (int)(sizeof(l1) - 1 + 2)) {
        printf("sockprobe: tcp sendmsg returned %d errno=%d\n", r, errno);
        close(fd); return 1;
    }
    char h[4], rest[64];
    struct iovec riov[2] = { { h, 4 }, { rest, sizeof(rest) } };
    struct sockaddr_in from;
    memset(&m, 0, sizeof(m));
    m.iov = riov; m.iovlen = 2;
    m.name = &from; m.namelen = sizeof(from);
    m.controllen = 99;
    r = xrecvmsg(fd, &m, 0);
    close(fd);
    if (r <= 4 || memcmp(h, "HTTP", 4) != 0 || m.namelen != 0 || m.controllen != 0) {
        printf("sockprobe: tcp recvmsg returned %d errno=%d namelen=%u controllen=%u\n",
               r, errno, (unsigned)m.namelen, (unsigned)m.controllen);
        return 1;
    }

    int u = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in to;
    to.sin_family = AF_INET;
    to.sin_port = htons((uint16_t)uport);
    to.sin_addr.s_addr = inet_addr("10.0.2.2");
    char a[] = "ab", b[] = "cd";
    struct iovec uiov[2] = { { a, 2 }, { b, 2 } };
    memset(&m, 0, sizeof(m));
    m.name = &to; m.namelen = sizeof(to);
    m.iov = uiov; m.iovlen = 2;
    r = xsendmsg(u, &m, 0);
    if (r != 4) {
        printf("sockprobe: udp sendmsg returned %d errno=%d\n", r, errno);
        close(u); return 1;
    }
    char e1[2], e2[16];
    struct iovec eiov[2] = { { e1, 2 }, { e2, sizeof(e2) } };
    for (int tries = 0; tries < 300; tries++) {
        memset(&m, 0, sizeof(m));
        memset(&from, 0, sizeof(from));
        m.name = &from; m.namelen = sizeof(from);
        m.iov = eiov; m.iovlen = 2;
        r = xrecvmsg(u, &m, MSG_DONTWAIT);
        if (r >= 0 || errno != EAGAIN) break;
        usleep(10000);
    }
    close(u);
    if (r != 4 || memcmp(e1, "ab", 2) != 0 || memcmp(e2, "cd", 2) != 0 ||
        m.namelen != sizeof(from) || from.sin_port != htons((uint16_t)uport)) {
        printf("sockprobe: udp recvmsg returned %d errno=%d namelen=%u port=%u\n",
               r, errno, (unsigned)m.namelen, (unsigned)ntohs(from.sin_port));
        return 1;
    }
    printf("sockprobe msg ok\n");
    return 0;
}

/* sockprobe conn <port> [restart]: the host's listener on port does not
 * complete a handshake at once, so a blocking connect() waits (3 s here).
 * SIGUSR1 arrives from a child after 1 s.
 *   default: the handler has no SA_RESTART, so the wait ends with EINTR,
 *            promptly.
 *   restart: the handler has SA_RESTART, so connect() is restarted and must
 *            go on waiting for the handshake under way (Linux
 *            __inet_stream_connect on SS_CONNECTING) — ending in success once
 *            the host frees its accept queue (smoke_net.py does after 2 s),
 *            or the real error — never EALREADY. */
static volatile int conn_alarms;
static void conn_alarm(int s) { (void)s; conn_alarms++; }
static int connintr(int port, int restart) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = conn_alarm;
    sa.sa_flags = restart ? SA_RESTART : 0;
    sigaction(SIGUSR1, &sa, 0);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in host;
    host.sin_family = AF_INET;
    host.sin_port = htons((uint16_t)port);
    host.sin_addr.s_addr = inet_addr("10.0.2.2");
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int parent = getpid();
    int kid = fork();                        /* no alarm(): signal from a child */
    if (kid == 0) {
        usleep(1000000);
        kill(parent, SIGUSR1);
        _exit(0);
    }
    int r = connect(fd, (struct sockaddr *)&host, sizeof(host));
    int err = r < 0 ? errno : 0;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    waitpid(kid, 0, 0);
    int ms = (int)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000);
    int ok;
    if (restart) {
        /* Waited past the signal, and either connected or failed for real. */
        ok = conn_alarms == 1 && ms > 1300 && err != EALREADY && err != EINTR &&
             (r == 0 || err == ETIMEDOUT || err == ECONNREFUSED);
        if (ok && r == 0) {
            struct sockaddr_in peer;
            ok = getname(fd, 1, &peer) == 0 && peer.sin_port == host.sin_port;
        }
    } else {
        ok = r == -1 && err == EINTR && ms <= 2000 && conn_alarms == 1;
    }
    close(fd);
    if (!ok) {
        printf("sockprobe: %s connect returned %d errno=%d after %d ms (signals %d)\n",
               restart ? "restarted" : "interrupted", r, err, ms, conn_alarms);
        return 1;
    }
    printf("sockprobe conn%s ok %d ms r=%d errno=%d\n", restart ? " restart" : "", ms, r, err);
    return 0;
}

/* sockprobe bulk tx|rx <port> <nbytes>: a throughput run against the host
 * (10.0.2.2:port).  tx sends nbytes in 16 KiB writes then closes; rx reads
 * until EOF.  Prints the byte count and the elapsed monotonic time. */
static int bulk(const char *dir, int port, int nbytes) {
    int tx = strcmp(dir, "tx") == 0;
    int fd = http_connect(port);
    if (fd < 0) {
        printf("sockprobe: connect failed errno=%d\n", errno);
        return 1;
    }
    static char buf[16384];
    for (int i = 0; i < (int)sizeof(buf); i++)
        buf[i] = (char)('a' + i % 26);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    long long done = 0;
    for (;;) {
        int r;
        if (tx) {
            if (done >= nbytes)
                break;
            int n = nbytes - done < (long long)sizeof(buf) ? (int)(nbytes - done) : (int)sizeof(buf);
            r = (int)send(fd, buf, (size_t)n, 0);
        } else {
            r = (int)recv(fd, buf, sizeof(buf), 0);
            if (r == 0)
                break;
        }
        if (r < 0) {
            printf("sockprobe: bulk %s failed at %lld errno=%d\n", dir, done, errno);
            close(fd);
            return 1;
        }
        done += r;
    }
    close(fd);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long long ms = (t1.tv_sec - t0.tv_sec) * 1000LL + (t1.tv_nsec - t0.tv_nsec) / 1000000;
    if (ms <= 0)
        ms = 1;
    printf("sockprobe bulk %s %lld bytes %lld ms %lld KB/s\n", dir, done, ms, done * 1000 / ms / 1024);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "tcpshut") == 0)
        return tcpshut(atoi(argv[2]));
    if (argc == 4 && strcmp(argv[1], "shutrd") == 0)
        return shutrd(atoi(argv[2]), atoi(argv[3]));
    if (argc == 4 && strcmp(argv[1], "tcptw") == 0)
        return tcptw(atoi(argv[2]), atoi(argv[3]));
    if (argc == 5 && strcmp(argv[1], "many") == 0)
        return many(atoi(argv[2]), atoi(argv[3]), atoi(argv[4]));
    if (argc == 4 && strcmp(argv[1], "nb") == 0)
        return nb(atoi(argv[2]), atoi(argv[3]));
    if (argc == 4 && strcmp(argv[1], "msg") == 0)
        return msgprobe(atoi(argv[2]), atoi(argv[3]));
    if (argc == 3 && strcmp(argv[1], "conn") == 0)
        return connintr(atoi(argv[2]), 0);
    if (argc == 4 && strcmp(argv[1], "conn") == 0 && strcmp(argv[3], "restart") == 0)
        return connintr(atoi(argv[2]), 1);
    if (argc == 5 && strcmp(argv[1], "bulk") == 0)
        return bulk(argv[2], atoi(argv[3]), atoi(argv[4]));

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

    /* A blocking UDP recv now waits for a datagram, as on Linux; the
     * empty-queue answer is EAGAIN with MSG_DONTWAIT. */
    char ch;
    if (recv(fd, &ch, 1, MSG_DONTWAIT) != -1 || errno != EAGAIN) {
        printf("sockprobe: empty udp recv errno=%d\n", errno);
        close(fd);
        return 1;
    }

    close(fd);
    printf("sockprobe udp ok\n");
    return 0;
}
