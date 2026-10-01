/*
 * net6probe: loopback and IPv6 sockets, run by tools/smoke_net6.py.
 *
 *   net6probe lo          TCP over 127.0.0.1 and [::1] (a forked client and
 *                         the parent's listener, data both ways, names);
 *                         an AF_INET6 dual-stack listener reached by an
 *                         AF_INET client (peer ::ffff:127.0.0.1); an
 *                         IPV6_V6ONLY listener refusing it; UDP over ::1;
 *                         ICMP echo over ping sockets to 127.0.0.1 and ::1,
 *                         each owning its identifier (EADDRINUSE on reuse);
 *                         SO_LINGER and IPV6_V6ONLY read back.
 *   net6probe gai <name>  getaddrinfo(AF_UNSPEC, SOCK_STREAM): one
 *                         "gai: <family> <address>" line per result, in order.
 *   net6probe tcp <addr> <port> <line>   connect (getaddrinfo, any family),
 *                         send the line, print what comes back.
 *
 * Prints "net6probe lo ok" when every check passed.
 */
#include "../include/errno.h"
#include "../include/stdio.h"
#include "../include/string.h"
#include "../include/stdlib.h"
#include "../include/unistd.h"
#include "../include/poll.h"
#include "../include/netdb.h"
#include "../include/sys/socket.h"
#include "../include/sys/wait.h"
#include "../include/sys/time.h"
#include "../include/netinet/in.h"
#include "../include/arpa/inet.h"

static int fails;

static void check(const char *what, int ok) {
    if (!ok) {
        printf("net6probe: FAIL %s (errno=%d)\n", what, errno);
        fails++;
    }
}

typedef union {
    struct sockaddr sa;
    struct sockaddr_in in;
    struct sockaddr_in6 in6;
} anyaddr;

static socklen_t loopback(int fam, anyaddr *a, int port) {
    memset(a, 0, sizeof(*a));
    if (fam == AF_INET) {
        a->in.sin_family = AF_INET;
        a->in.sin_port = htons((uint16_t)port);
        a->in.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        return sizeof(a->in);
    }
    a->in6.sin6_family = AF_INET6;
    a->in6.sin6_port = htons((uint16_t)port);
    a->in6.sin6_addr = in6addr_loopback;
    return sizeof(a->in6);
}

static int port_of(const anyaddr *a) {
    return ntohs(a->sa.sa_family == AF_INET ? a->in.sin_port : a->in6.sin6_port);
}

static const char *ntop(const anyaddr *a, char *buf, int cap) {
    if (a->sa.sa_family == AF_INET)
        return inet_ntop(AF_INET, &a->in.sin_addr, buf, (socklen_t)cap);
    return inet_ntop(AF_INET6, &a->in6.sin6_addr, buf, (socklen_t)cap);
}

static void set_timeout(int fd, int sec) {
    struct timeval tv = { sec, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

static int read_line(int fd, char *buf, int cap) {
    int n = 0;
    while (n < cap - 1) {
        int r = (int)recv(fd, buf + n, 1, 0);
        if (r <= 0) break;
        if (buf[n++] == '\n') break;
    }
    buf[n] = 0;
    return n;
}

/* A listener of family `lfam` (bound to the loopback, or the wildcard with
 * `wild`), a forked client of family `cfam` connecting to the loopback of
 * its own family, one line each way.  `want_peer` is the peer address the
 * listener must see (text form). */
static void tcp_pair(const char *tag, int lfam, int wild, int cfam,
                     const char *want_peer) {
    char t[160], ab[64];
    int srv = socket(lfam, SOCK_STREAM, 0);
    snprintf(t, sizeof(t), "%s socket", tag);
    check(t, srv >= 0);
    if (srv < 0) return;
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    anyaddr a;
    socklen_t al = loopback(lfam, &a, 0);
    if (wild) {
        if (lfam == AF_INET) a.in.sin_addr.s_addr = 0;
        else a.in6.sin6_addr = in6addr_any;
    }
    snprintf(t, sizeof(t), "%s bind", tag);
    check(t, bind(srv, &a.sa, al) == 0);
    snprintf(t, sizeof(t), "%s listen", tag);
    check(t, listen(srv, 4) == 0);
    anyaddr me;
    socklen_t ml = sizeof(me);
    getsockname(srv, &me.sa, &ml);
    int port = port_of(&me);
    snprintf(t, sizeof(t), "%s getsockname len", tag);
    check(t, ml == (lfam == AF_INET ? 16u : 28u) && port > 0);

    int pid = fork();
    if (pid == 0) {
        int c = socket(cfam, SOCK_STREAM, 0);
        anyaddr d;
        socklen_t dl = loopback(cfam, &d, port);
        set_timeout(c, 5);
        if (connect(c, &d.sa, dl) < 0) {
            printf("net6probe: %s client connect errno=%d\n", tag, errno);
            _exit(2);
        }
        const char *msg = "hello from client\n";
        send(c, msg, strlen(msg), 0);
        char buf[64];
        read_line(c, buf, sizeof(buf));
        _exit(strcmp(buf, "hello from server\n") ? 3 : 0);
    }
    set_timeout(srv, 5);
    anyaddr peer;
    socklen_t pl = sizeof(peer);
    int c = accept(srv, &peer.sa, &pl);
    snprintf(t, sizeof(t), "%s accept", tag);
    check(t, c >= 0);
    if (c >= 0) {
        ntop(&peer, ab, sizeof(ab));
        snprintf(t, sizeof(t), "%s peer %s (want %s)", tag, ab, want_peer);
        check(t, !strcmp(ab, want_peer) && pl == (lfam == AF_INET ? 16u : 28u));
        set_timeout(c, 5);
        char buf[64];
        read_line(c, buf, sizeof(buf));
        snprintf(t, sizeof(t), "%s server got '%s'", tag, buf);
        check(t, !strcmp(buf, "hello from client\n"));
        const char *msg = "hello from server\n";
        send(c, msg, strlen(msg), 0);
        close(c);
    }
    int st = -1;
    waitpid(pid, &st, 0);
    snprintf(t, sizeof(t), "%s client status %d", tag, st);
    check(t, WIFEXITED(st) && WEXITSTATUS(st) == 0);
    close(srv);
    if (!fails) printf("net6probe: %s ok (port %d, peer %s)\n", tag, port, want_peer);
}

/* An IPV6_V6ONLY listener on [::]: an AF_INET client is refused, and the
 * same port is free for an AF_INET listener. */
static void v6only_check(void) {
    int s = socket(AF_INET6, SOCK_STREAM, 0);
    int one = 1, v = -1;
    socklen_t vl = sizeof(v);
    check("v6only default", getsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &v, &vl) == 0 && v == 0);
    check("v6only set", setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof(one)) == 0);
    vl = sizeof(v);
    check("v6only get", getsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &v, &vl) == 0 && v == 1);
    anyaddr a;
    socklen_t al = loopback(AF_INET6, &a, 0);
    a.in6.sin6_addr = in6addr_any;
    check("v6only bind", bind(s, &a.sa, al) == 0);
    check("v6only listen", listen(s, 2) == 0);
    check("v6only set after bind is EINVAL",
          setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof(one)) < 0 && errno == EINVAL);
    anyaddr me;
    socklen_t ml = sizeof(me);
    getsockname(s, &me.sa, &ml);
    int port = port_of(&me);

    int c = socket(AF_INET, SOCK_STREAM, 0);
    anyaddr d;
    socklen_t dl = loopback(AF_INET, &d, port);
    int r = connect(c, &d.sa, dl);
    check("v4 client to a v6-only listener is refused", r < 0 && errno == ECONNREFUSED);
    close(c);

    int s4 = socket(AF_INET, SOCK_STREAM, 0);
    al = loopback(AF_INET, &a, port);
    a.in.sin_addr.s_addr = 0;
    check("v4 bind beside a v6-only listener", bind(s4, &a.sa, al) == 0);
    close(s4);
    close(s);

    /* A v4-mapped name on a v6-only socket. */
    s = socket(AF_INET6, SOCK_DGRAM, 0);
    setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof(one));
    al = loopback(AF_INET6, &a, 9);
    inet_pton(AF_INET6, "::ffff:127.0.0.1", &a.in6.sin6_addr);
    check("v4-mapped sendto on v6-only fails",
          sendto(s, "x", 1, 0, &a.sa, al) < 0);
    close(s);
    if (!fails) printf("net6probe: v6only ok\n");
}

static void udp6_check(void) {
    int s = socket(AF_INET6, SOCK_DGRAM, 0);
    anyaddr a;
    socklen_t al = loopback(AF_INET6, &a, 0);
    check("udp6 bind ::1", bind(s, &a.sa, al) == 0);
    anyaddr me;
    socklen_t ml = sizeof(me);
    getsockname(s, &me.sa, &ml);
    set_timeout(s, 3);
    check("udp6 sendto self", sendto(s, "dgram6", 6, 0, &me.sa, ml) == 6);
    char buf[16];
    anyaddr from;
    socklen_t fl = sizeof(from);
    int n = (int)recvfrom(s, buf, sizeof(buf), 0, &from.sa, &fl);
    char ab[64];
    ntop(&from, ab, sizeof(ab));
    check("udp6 recvfrom", n == 6 && !memcmp(buf, "dgram6", 6) &&
          fl == 28 && !strcmp(ab, "::1") && port_of(&from) == port_of(&me));
    close(s);

    /* A dual-stack UDP socket hears an IPv4 sender as ::ffff:127.0.0.1. */
    s = socket(AF_INET6, SOCK_DGRAM, 0);
    al = loopback(AF_INET6, &a, 0);
    a.in6.sin6_addr = in6addr_any;
    check("udp dual bind", bind(s, &a.sa, al) == 0);
    ml = sizeof(me);
    getsockname(s, &me.sa, &ml);
    int s4 = socket(AF_INET, SOCK_DGRAM, 0);
    anyaddr d;
    socklen_t dl = loopback(AF_INET, &d, port_of(&me));
    check("udp v4 -> dual sendto", sendto(s4, "v4", 2, 0, &d.sa, dl) == 2);
    set_timeout(s, 3);
    fl = sizeof(from);
    n = (int)recvfrom(s, buf, sizeof(buf), 0, &from.sa, &fl);
    ntop(&from, ab, sizeof(ab));
    check("udp dual recvfrom v4-mapped", n == 2 && !strcmp(ab, "::ffff:127.0.0.1"));
    close(s4);
    close(s);
    if (!fails) printf("net6probe: udp6 ok\n");
}

static void ping_check(int fam) {
    int s = socket(fam, SOCK_DGRAM, fam == AF_INET ? IPPROTO_ICMP : IPPROTO_ICMPV6);
    check(fam == AF_INET ? "ping socket v4" : "ping socket v6", s >= 0);
    if (s < 0) return;
    unsigned char pkt[16];
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = fam == AF_INET ? 8 : 128;
    pkt[7] = 7;                               /* sequence 7 */
    memcpy(pkt + 8, "maerping", 8);
    anyaddr d;
    socklen_t dl = loopback(fam, &d, 0);
    check("ping sendto", sendto(s, pkt, sizeof(pkt), 0, &d.sa, dl) == (int)sizeof(pkt));
    struct pollfd p = { s, POLLIN, 0 };
    check("ping reply arrives", poll(&p, 1, 3000) == 1);
    unsigned char r[64];
    int n = (int)recv(s, r, sizeof(r), MSG_DONTWAIT);
    check("ping reply", n == (int)sizeof(pkt) && r[0] == (fam == AF_INET ? 0 : 129) &&
          r[7] == 7 && !memcmp(r + 8, "maerping", 8));
    close(s);
    if (!fails) printf("net6probe: ping %s ok\n", fam == AF_INET ? "127.0.0.1" : "::1");
}

/* A ping socket's identifier is its own: a second socket binding the
 * first's (from getsockname) gets EADDRINUSE, both before and after the
 * first sends; once the first is closed the identifier is free again. */
static void ping_ident_check(int fam) {
    int a = socket(fam, SOCK_DGRAM, fam == AF_INET ? IPPROTO_ICMP : IPPROTO_ICMPV6);
    int b = socket(fam, SOCK_DGRAM, fam == AF_INET ? IPPROTO_ICMP : IPPROTO_ICMPV6);
    check("ping ident sockets", a >= 0 && b >= 0);
    if (a < 0 || b < 0) return;
    anyaddr me, ba;
    socklen_t ml = sizeof(me);
    check("ping getsockname", getsockname(a, &me.sa, &ml) == 0 && port_of(&me) != 0);
    int id = port_of(&me);
    socklen_t bl = loopback(fam, &ba, id);
    int r = bind(b, &ba.sa, bl);
    check("ping bind to a taken identifier is EADDRINUSE", r < 0 && errno == EADDRINUSE);
    anyaddr bm;
    socklen_t bml = sizeof(bm);
    getsockname(b, &bm.sa, &bml);
    check("ping identifiers differ", port_of(&bm) != id);
    close(a);
    check("ping bind to a released identifier", bind(b, &ba.sa, bl) == 0);
    close(b);
    if (!fails) printf("net6probe: ping ident %s ok\n", fam == AF_INET ? "v4" : "v6");
}

static void linger_check(void) {
    int s = socket(AF_INET6, SOCK_STREAM, 0);
    struct { int on, secs; } l = { 1, 0 }, g = { -1, -1 };
    socklen_t gl = sizeof(g);
    check("SO_LINGER set", setsockopt(s, SOL_SOCKET, SO_LINGER, &l, sizeof(l)) == 0);
    check("SO_LINGER get", getsockopt(s, SOL_SOCKET, SO_LINGER, &g, &gl) == 0 &&
          gl == 8 && g.on == 1 && g.secs == 0);
    close(s);
}

static int do_lo(void) {
    tcp_pair("tcp 127.0.0.1", AF_INET, 0, AF_INET, "127.0.0.1");
    tcp_pair("tcp [::1]", AF_INET6, 0, AF_INET6, "::1");
    tcp_pair("tcp dual-stack [::] <- v4", AF_INET6, 1, AF_INET, "::ffff:127.0.0.1");
    tcp_pair("tcp dual-stack [::] <- v6", AF_INET6, 1, AF_INET6, "::1");
    v6only_check();
    udp6_check();
    ping_check(AF_INET);
    ping_check(AF_INET6);
    ping_ident_check(AF_INET);
    ping_ident_check(AF_INET6);
    linger_check();
    printf(fails ? "net6probe lo FAILED (%d)\n" : "net6probe lo ok\n", fails);
    return fails ? 1 : 0;
}

static int do_gai(const char *name) {
    struct addrinfo hints, *res, *ai;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int r = getaddrinfo(name, "80", &hints, &res);
    if (r) {
        printf("gai: error %d %s\n", r, gai_strerror(r));
        return 1;
    }
    for (ai = res; ai; ai = ai->ai_next) {
        char ab[64];
        ntop((anyaddr *)ai->ai_addr, ab, sizeof(ab));
        printf("gai: %s %s\n", ai->ai_family == AF_INET6 ? "inet6" : "inet", ab);
    }
    freeaddrinfo(res);
    return 0;
}

static int do_tcp(const char *host, const char *port, const char *line) {
    struct addrinfo hints, *res, *ai;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res)) {
        printf("net6probe tcp: cannot resolve %s\n", host);
        return 1;
    }
    int fd = -1, err = 0;
    for (ai = res; ai && fd < 0; ai = ai->ai_next) {
        fd = socket(ai->ai_family, SOCK_STREAM, 0);
        if (fd >= 0 && connect(fd, ai->ai_addr, ai->ai_addrlen) < 0) {
            err = errno;
            close(fd);
            fd = -1;
        }
    }
    freeaddrinfo(res);
    if (fd < 0) {
        printf("net6probe tcp: connect failed errno=%d\n", err);
        return 1;
    }
    set_timeout(fd, 5);
    send(fd, line, strlen(line), 0);
    send(fd, "\n", 1, 0);
    char buf[128];
    read_line(fd, buf, sizeof(buf));
    printf("net6probe tcp got: %s", buf);
    close(fd);
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 2 && !strcmp(argv[1], "lo")) return do_lo();
    if (argc >= 3 && !strcmp(argv[1], "gai")) return do_gai(argv[2]);
    if (argc >= 5 && !strcmp(argv[1], "tcp")) return do_tcp(argv[2], argv[3], argv[4]);
    printf("usage: net6probe lo | gai <name> | tcp <host> <port> <line>\n");
    return 2;
}
