/*
 * unixprobe — AF_UNIX lifetime and semantics (built against musl, so the same
 * binary also runs on a Linux host as the reference; see userspace/Makefile).
 *
 *  close   closing a descriptor while another thread sleeps in accept(),
 *          read() or write() on it wakes that thread cleanly (EBADF, never a
 *          use-after-free), 100 times each.  Linux keeps such a thread asleep,
 *          so this part runs on MaeroOS only.
 *  names   bind() makes a socket inode: S_ISSOCK, EADDRINUSE on an existing
 *          name (also a stale one), ENOENT/ECONNREFUSED on connect, ENXIO on
 *          open(), EACCES without write permission on the socket or the
 *          directory; abstract names are a separate namespace of raw bytes;
 *          getsockname/getpeername/accept report the names.
 *  cred    SO_PEERCRED is the peer's real pid/uid/gid; unknown options are
 *          ENOPROTOOPT.
 *  shut    shutdown(SHUT_WR) gives the peer EOF and us EPIPE; SHUT_RD makes our
 *          reads return 0.
 *  record  SOCK_DGRAM and SOCK_SEQPACKET keep message boundaries (truncation,
 *          MSG_TRUNC, zero-length messages, named datagram senders).
 *  gc      a socket sent over its own connection and closed is collected, not
 *          leaked forever.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int fails, on_linux;

#define CHECK(cond, ...) do {                                           \
        if (!(cond)) {                                                  \
            printf("unixprobe FAIL %s:%d: ", __func__, __LINE__);       \
            printf(__VA_ARGS__); printf(" (errno %d)\n", errno);        \
            fails++;                                                    \
        }                                                               \
    } while (0)

static void msleep(int ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, 0);
}

static socklen_t path_addr(struct sockaddr_un *a, const char *path) {
    memset(a, 0, sizeof(*a));
    a->sun_family = AF_UNIX;
    memcpy(a->sun_path, path, strlen(path) < sizeof(a->sun_path) ? strlen(path)
                                                                  : sizeof(a->sun_path));
    return (socklen_t)(offsetof(struct sockaddr_un, sun_path) + strlen(path));
}

/* An abstract name: NUL, then `n` bytes of name. */
static socklen_t abs_addr(struct sockaddr_un *a, const char *name, size_t n) {
    memset(a, 0, sizeof(*a));
    a->sun_family = AF_UNIX;
    memcpy(a->sun_path + 1, name, n);
    return (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + n);
}

static long free_kb(void) {
    char buf[512];
    int fd = open("/proc/meminfo", O_RDONLY);
    if (fd < 0) return -1;
    int n = (int)read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = 0;
    char *p = strstr(buf, "MemFree:");
    return p ? atol(p + 8) : -1;
}

/* ── close while blocked ─────────────────────────────────────────────────── */

struct blk { int fd; int op; volatile int started; int ret; int err; };

static void *blocker(void *arg) {
    struct blk *b = arg;
    char buf[4096];
    memset(buf, 'x', sizeof(buf));
    b->started = 1;
    if (b->op == 0)      b->ret = accept(b->fd, 0, 0);
    else if (b->op == 1) b->ret = (int)read(b->fd, buf, sizeof(buf));
    else                 b->ret = (int)send(b->fd, buf, sizeof(buf), MSG_NOSIGNAL);
    b->err = errno;
    return 0;
}

static void test_close_blocked(void) {
    static const char *opname[] = { "accept", "read", "write" };
    for (int op = 0; op < 3; op++) {
        int bad = 0;
        for (int i = 0; i < 100; i++) {
            int fd = -1, other = -1;
            if (op == 0) {
                struct sockaddr_un a;
                char nm[32];
                snprintf(nm, sizeof(nm), "unixprobe-acc-%d", i);
                fd = socket(AF_UNIX, SOCK_STREAM, 0);
                if (bind(fd, (struct sockaddr *)&a, abs_addr(&a, nm, strlen(nm))) ||
                    listen(fd, 4)) { CHECK(0, "listener %d", i); return; }
            } else {
                int sv[2];
                if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) { CHECK(0, "socketpair"); return; }
                fd = sv[0]; other = sv[1];
                if (op == 2) {
                    /* Fill the ring so the next send blocks. */
                    char chunk[4096];
                    memset(chunk, 'f', sizeof(chunk));
                    fcntl(fd, F_SETFL, O_NONBLOCK);
                    while (send(fd, chunk, sizeof(chunk), MSG_NOSIGNAL) > 0) { }
                    fcntl(fd, F_SETFL, 0);
                }
            }
            struct blk b = { fd, op, 0, 0, 0 };
            pthread_t th;
            pthread_create(&th, 0, blocker, &b);
            while (!b.started) sched_yield();
            msleep(i % 3 == 0 ? 0 : 15);       /* mostly: really asleep */
            close(fd);
            pthread_join(th, 0);
            if (b.ret >= 0 && !(op == 1 && b.ret == 0)) bad++;
            else if (b.ret < 0 && b.err != EBADF) bad++;
            if (b.ret > 0 && op == 0) close(b.ret);
            if (other >= 0) close(other);
        }
        CHECK(!bad, "close while blocked in %s: %d of 100 returns were not EBADF/EOF",
              opname[op], bad);
    }
}

/* ── names ───────────────────────────────────────────────────────────────── */

#define SOCKDIR  "/tmp/unixprobe.d"
#define SOCKPATH "/tmp/unixprobe.d/s"

static void test_names(void) {
    struct sockaddr_un a, got;
    socklen_t al, gl;
    struct stat st;
    mkdir(SOCKDIR, 0755);
    unlink(SOCKPATH);

    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    al = path_addr(&a, SOCKPATH);
    CHECK(bind(s, (struct sockaddr *)&a, al) == 0, "bind %s", SOCKPATH);
    CHECK(stat(SOCKPATH, &st) == 0 && S_ISSOCK(st.st_mode), "bound name is not S_IFSOCK");
    CHECK(open(SOCKPATH, O_RDWR) < 0 && errno == ENXIO, "open() of a socket not ENXIO");

    int c = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(connect(c, (struct sockaddr *)&a, al) < 0 && errno == ECONNREFUSED,
          "connect to a bound, non-listening socket not ECONNREFUSED");
    close(c);

    int s2 = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(bind(s2, (struct sockaddr *)&a, al) < 0 && errno == EADDRINUSE,
          "second bind to an existing path not EADDRINUSE");
    CHECK(bind(s, (struct sockaddr *)&a, al) < 0 && errno == EADDRINUSE,
          "rebinding a bound socket to its own name not EADDRINUSE");
    struct sockaddr_un other;
    socklen_t ol = path_addr(&other, SOCKDIR "/other");
    CHECK(bind(s, (struct sockaddr *)&other, ol) < 0 && errno == EINVAL,
          "rebinding a bound socket not EINVAL");
    CHECK(stat(SOCKDIR "/other", &st) < 0, "a refused bind left its inode behind");

    /* getsockname reports the path with its NUL. */
    gl = sizeof(got);
    CHECK(getsockname(s, (struct sockaddr *)&got, &gl) == 0 &&
          gl == offsetof(struct sockaddr_un, sun_path) + strlen(SOCKPATH) + 1 &&
          strcmp(got.sun_path, SOCKPATH) == 0, "getsockname: len %u", (unsigned)gl);

    CHECK(listen(s, 4) == 0, "listen");
    c = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(connect(c, (struct sockaddr *)&a, al) == 0, "connect");
    gl = sizeof(got);
    int acc = accept(s, (struct sockaddr *)&got, &gl);
    CHECK(acc >= 0 && gl == offsetof(struct sockaddr_un, sun_path),
          "accept of an unnamed client: len %u", (unsigned)gl);
    gl = sizeof(got);
    CHECK(getpeername(c, (struct sockaddr *)&got, &gl) == 0 &&
          strcmp(got.sun_path, SOCKPATH) == 0, "client getpeername");
    gl = sizeof(got);
    CHECK(getsockname(acc, (struct sockaddr *)&got, &gl) == 0 &&
          strcmp(got.sun_path, SOCKPATH) == 0, "accepted socket's name is the listener's");
    CHECK(write(c, "hi", 2) == 2, "write");
    char buf[8];
    CHECK(read(acc, buf, sizeof(buf)) == 2 && !memcmp(buf, "hi", 2), "read");
    close(c); close(acc);

    /* The server goes: its name stays (stale) until unlinked. */
    close(s);
    close(s2);
    c = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(connect(c, (struct sockaddr *)&a, al) < 0 && errno == ECONNREFUSED,
          "connect to a stale socket file not ECONNREFUSED");
    close(c);
    s = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(bind(s, (struct sockaddr *)&a, al) < 0 && errno == EADDRINUSE,
          "bind over a stale socket file not EADDRINUSE");
    CHECK(unlink(SOCKPATH) == 0, "unlink the stale socket");
    CHECK(bind(s, (struct sockaddr *)&a, al) == 0, "bind after unlink");
    CHECK(listen(s, 4) == 0, "listen");
    c = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un none;
    socklen_t nl = path_addr(&none, "/tmp/unixprobe.d/nonexistent");
    CHECK(connect(c, (struct sockaddr *)&none, nl) < 0 && errno == ENOENT,
          "connect to a missing path not ENOENT");
    close(c);

    /* Permissions, as an unprivileged user: connect needs write permission
     * on the socket, bind needs it on the directory. */
    if (getuid() == 0) {
        chmod(SOCKPATH, 0755);
        int pid = fork();
        if (pid == 0) {
            if (setgid(1000) || setuid(1000)) _exit(9);
            int res = 0;
            int cc = socket(AF_UNIX, SOCK_STREAM, 0);
            if (!(connect(cc, (struct sockaddr *)&a, al) < 0 && errno == EACCES)) res |= 1;
            close(cc);
            struct sockaddr_un b;
            socklen_t bl = path_addr(&b, SOCKDIR "/user");
            cc = socket(AF_UNIX, SOCK_STREAM, 0);
            if (!(bind(cc, (struct sockaddr *)&b, bl) < 0 && errno == EACCES)) res |= 2;
            close(cc);
            _exit(res);
        }
        int stt = 0;
        waitpid(pid, &stt, 0);
        int res = WIFEXITED(stt) ? WEXITSTATUS(stt) : 99;
        CHECK(!(res & 1), "connect without write permission not EACCES");
        CHECK(!(res & 2), "bind in a directory without write permission not EACCES");
        CHECK(res == 0 || res == 1 || res == 2 || res == 3, "permission child: %d", res);
        chmod(SOCKPATH, 0777);
        pid = fork();
        if (pid == 0) {
            if (setgid(1000) || setuid(1000)) _exit(9);
            int cc = socket(AF_UNIX, SOCK_STREAM, 0);
            _exit(connect(cc, (struct sockaddr *)&a, al) == 0 ? 0 : 1);
        }
        waitpid(pid, &stt, 0);
        CHECK(WIFEXITED(stt) && WEXITSTATUS(stt) == 0,
              "connect with write permission failed");
        int drop = accept(s, 0, 0);
        if (drop >= 0) close(drop);
    }
    close(s);
    unlink(SOCKPATH);

    /* Abstract names: raw bytes, full length, a namespace of their own. */
    char n1[107], n2[107];
    memset(n1, 'A', sizeof(n1));
    memcpy(n2, n1, sizeof(n2));
    n2[106] = 'B';                         /* differ in the very last byte */
    struct sockaddr_un a1, a2;
    socklen_t l1 = abs_addr(&a1, n1, sizeof(n1)), l2 = abs_addr(&a2, n2, sizeof(n2));
    int x1 = socket(AF_UNIX, SOCK_STREAM, 0), x2 = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(bind(x1, (struct sockaddr *)&a1, l1) == 0, "bind 108-byte abstract name");
    CHECK(bind(x2, (struct sockaddr *)&a2, l2) == 0,
          "bind a second 108-byte abstract name differing in the last byte");
    listen(x1, 2); listen(x2, 2);
    int x3 = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(bind(x3, (struct sockaddr *)&a1, l1) < 0 && errno == EADDRINUSE,
          "abstract name bound twice");
    close(x3);
    c = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(connect(c, (struct sockaddr *)&a2, l2) == 0, "connect to the second name");
    int ac2 = accept(x2, 0, 0);
    CHECK(ac2 >= 0, "the second name's listener got the connection");
    gl = sizeof(got);
    CHECK(getpeername(c, (struct sockaddr *)&got, &gl) == 0 && gl == l2 &&
          !memcmp(got.sun_path, a2.sun_path, sizeof(n2) + 1), "abstract getpeername");
    close(c); close(ac2); close(x1); close(x2);

    /* A path name and an abstract name with the same text are different. */
    struct sockaddr_un ab;
    socklen_t abl = abs_addr(&ab, SOCKPATH, strlen(SOCKPATH));
    int y = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(bind(y, (struct sockaddr *)&ab, abl) == 0 && listen(y, 1) == 0, "abstract twin");
    CHECK(stat(SOCKPATH, &st) < 0, "an abstract bind created a file");
    c = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(connect(c, (struct sockaddr *)&a, al) < 0 && errno == ENOENT,
          "the path namespace saw the abstract name");
    close(c); close(y);
    rmdir(SOCKDIR);
}

/* ── credentials ─────────────────────────────────────────────────────────── */

static void test_cred(void) {
    struct ucred cr;
    socklen_t l;
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    l = sizeof(cr);
    CHECK(getsockopt(sv[0], SOL_SOCKET, SO_PEERCRED, &cr, &l) == 0 && l == sizeof(cr) &&
          cr.pid == getpid() && cr.uid == geteuid() && cr.gid == getegid(),
          "socketpair SO_PEERCRED: pid %d uid %d gid %d len %u",
          (int)cr.pid, (int)cr.uid, (int)cr.gid, (unsigned)l);
    int v = 0;
    l = sizeof(v);
    CHECK(getsockopt(sv[0], SOL_SOCKET, 12345, &v, &l) < 0 && errno == ENOPROTOOPT,
          "unknown option not ENOPROTOOPT");
    l = sizeof(v);
    CHECK(getsockopt(sv[0], SOL_SOCKET, SO_TYPE, &v, &l) == 0 && v == SOCK_STREAM, "SO_TYPE");
    l = sizeof(v);
    CHECK(getsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &v, &l) == 0 && v > 0, "SO_SNDBUF");
    close(sv[0]); close(sv[1]);

    if (getuid() != 0) return;
    struct sockaddr_un a;
    socklen_t al = abs_addr(&a, "unixprobe-cred", 14);
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    bind(s, (struct sockaddr *)&a, al);
    listen(s, 2);
    int pid = fork();
    if (pid == 0) {
        if (setgid(1001) || setuid(1000)) _exit(9);
        int c = socket(AF_UNIX, SOCK_STREAM, 0);
        if (connect(c, (struct sockaddr *)&a, al)) _exit(8);
        struct ucred pc;
        socklen_t pl = sizeof(pc);
        if (getsockopt(c, SOL_SOCKET, SO_PEERCRED, &pc, &pl) ||
            pc.uid != 0 || pc.pid != getppid()) _exit(7);
        char ch;
        read(c, &ch, 1);                   /* hold until the server looked */
        _exit(0);
    }
    int acc = accept(s, 0, 0);
    l = sizeof(cr);
    memset(&cr, 0, sizeof(cr));
    CHECK(acc >= 0 && getsockopt(acc, SOL_SOCKET, SO_PEERCRED, &cr, &l) == 0 &&
          cr.pid == pid && cr.uid == 1000 && cr.gid == 1001,
          "accepted SO_PEERCRED: pid %d (want %d) uid %d gid %d",
          (int)cr.pid, pid, (int)cr.uid, (int)cr.gid);
    if (acc >= 0) { write(acc, "k", 1); close(acc); }
    int st = 0;
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0,
          "client-side SO_PEERCRED (the listener's creds) wrong: %d", WEXITSTATUS(st));
    close(s);
}

/* ── shutdown ────────────────────────────────────────────────────────────── */

static void test_shutdown(void) {
    int sv[2];
    char buf[16];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    CHECK(write(sv[0], "abc", 3) == 3, "write");
    CHECK(shutdown(sv[0], SHUT_WR) == 0, "shutdown(SHUT_WR)");
    CHECK(read(sv[1], buf, sizeof(buf)) == 3, "data before EOF");
    CHECK(read(sv[1], buf, sizeof(buf)) == 0, "peer did not see EOF after SHUT_WR");
    CHECK(send(sv[0], "x", 1, MSG_NOSIGNAL) < 0 && errno == EPIPE, "write after SHUT_WR not EPIPE");
    CHECK(write(sv[1], "back", 4) == 4, "the other direction still works");
    CHECK(read(sv[0], buf, sizeof(buf)) == 4, "reading the other direction");
    CHECK(shutdown(sv[0], SHUT_RD) == 0, "shutdown(SHUT_RD)");
    CHECK(read(sv[0], buf, sizeof(buf)) == 0, "read after SHUT_RD not 0");
    CHECK(send(sv[1], "y", 1, MSG_NOSIGNAL) < 0 && errno == EPIPE,
          "peer write after our SHUT_RD not EPIPE");
    CHECK(shutdown(sv[0], 7) < 0 && errno == EINVAL, "bad how not EINVAL");
    close(sv[0]); close(sv[1]);
}

/* ── records ─────────────────────────────────────────────────────────────── */

static void test_records(int type, const char *tn) {
    int sv[2];
    char buf[64];
    if (socketpair(AF_UNIX, type, 0, sv)) { CHECK(0, "%s socketpair", tn); return; }
    CHECK(send(sv[0], "a", 1, 0) == 1 && send(sv[0], "bb", 2, 0) == 2 &&
          send(sv[0], "ccc", 3, 0) == 3, "%s sends", tn);
    CHECK(recv(sv[1], buf, sizeof(buf), 0) == 1 && buf[0] == 'a', "%s record 1", tn);
    CHECK(recv(sv[1], buf, sizeof(buf), 0) == 2 && buf[0] == 'b', "%s record 2", tn);
    CHECK(recv(sv[1], buf, sizeof(buf), 0) == 3 && buf[0] == 'c', "%s record 3", tn);

    /* Truncation drops the rest of the record, and says so. */
    send(sv[0], "0123456789", 10, 0);
    send(sv[0], "next", 4, 0);
    struct iovec iov = { buf, 4 };
    struct msghdr m;
    memset(&m, 0, sizeof(m));
    m.msg_iov = &iov; m.msg_iovlen = 1;
    CHECK(recvmsg(sv[1], &m, 0) == 4 && (m.msg_flags & MSG_TRUNC), "%s truncation", tn);
    CHECK(recv(sv[1], buf, sizeof(buf), 0) == 4 && !memcmp(buf, "next", 4),
          "%s record after a truncated one", tn);
    send(sv[0], "0123456789", 10, 0);
    CHECK(recv(sv[1], buf, 2, MSG_TRUNC) == 10, "%s recv(MSG_TRUNC) real length", tn);

    /* Scatter/gather: one record over several iovecs. */
    struct iovec g[2] = { { "hel", 3 }, { "lo", 2 } };
    memset(&m, 0, sizeof(m));
    m.msg_iov = g; m.msg_iovlen = 2;
    CHECK(sendmsg(sv[0], &m, 0) == 5, "%s sendmsg gather", tn);
    CHECK(send(sv[0], "!", 1, 0) == 1, "%s", tn);
    char p1[2], p2[8];
    struct iovec sc[2] = { { p1, 2 }, { p2, 8 } };
    memset(&m, 0, sizeof(m));
    m.msg_iov = sc; m.msg_iovlen = 2;
    CHECK(recvmsg(sv[1], &m, 0) == 5 && !memcmp(p1, "he", 2) && !memcmp(p2, "llo", 3),
          "%s recvmsg scatter", tn);
    CHECK(read(sv[1], buf, sizeof(buf)) == 1 && buf[0] == '!', "%s read() of a record", tn);

    /* A zero-length record is a record. */
    CHECK(send(sv[0], "", 0, 0) == 0, "%s zero-length send", tn);
    fcntl(sv[1], F_SETFL, O_NONBLOCK);
    CHECK(recv(sv[1], buf, sizeof(buf), 0) == 0, "%s zero-length record", tn);
    CHECK(recv(sv[1], buf, sizeof(buf), 0) < 0 && errno == EAGAIN, "%s queue empty", tn);
    fcntl(sv[1], F_SETFL, 0);

    close(sv[0]);
    if (type == SOCK_SEQPACKET)
        CHECK(recv(sv[1], buf, sizeof(buf), 0) == 0, "SEQPACKET EOF after peer close");
    close(sv[1]);
}

static void test_dgram_named(void) {
    struct sockaddr_un sa, ca, from;
    socklen_t sl = abs_addr(&sa, "unixprobe-dsrv", 14);
    socklen_t cl = abs_addr(&ca, "unixprobe-dcli", 14);
    int s = socket(AF_UNIX, SOCK_DGRAM, 0), c = socket(AF_UNIX, SOCK_DGRAM, 0);
    CHECK(bind(s, (struct sockaddr *)&sa, sl) == 0 && bind(c, (struct sockaddr *)&ca, cl) == 0,
          "dgram binds");
    CHECK(sendto(c, "one", 3, 0, (struct sockaddr *)&sa, sl) == 3, "sendto");
    CHECK(sendto(c, "two!", 4, 0, (struct sockaddr *)&sa, sl) == 4, "sendto");
    char buf[16];
    socklen_t fl = sizeof(from);
    CHECK(recvfrom(s, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl) == 3 &&
          fl == cl && !memcmp(from.sun_path, ca.sun_path, 15), "recvfrom sender name");
    CHECK(recv(s, buf, sizeof(buf), 0) == 4, "second datagram");
    CHECK(connect(c, (struct sockaddr *)&sa, sl) == 0, "dgram connect");
    CHECK(send(c, "3", 1, 0) == 1 && recv(s, buf, sizeof(buf), 0) == 1, "connected send");
    int st = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(connect(st, (struct sockaddr *)&sa, sl) < 0 && errno == ECONNREFUSED,
          "stream connect to a dgram name not ECONNREFUSED");
    close(st);
    close(s);
    CHECK(send(c, "x", 1, 0) < 0 && errno == ECONNREFUSED, "send to a closed receiver");
    close(c);
}

/* ── SCM_RIGHTS cycles ───────────────────────────────────────────────────── */

static int send_fd(int sock, int fd) {
    char c = 'z';
    struct iovec iov = { &c, 1 };
    union { struct cmsghdr h; char b[CMSG_SPACE(sizeof(int))]; } u;
    struct msghdr m;
    memset(&m, 0, sizeof(m));
    memset(&u, 0, sizeof(u));
    m.msg_iov = &iov; m.msg_iovlen = 1;
    m.msg_control = u.b; m.msg_controllen = CMSG_LEN(sizeof(int));
    struct cmsghdr *h = CMSG_FIRSTHDR(&m);
    h->cmsg_level = SOL_SOCKET; h->cmsg_type = SCM_RIGHTS;
    h->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(h), &fd, sizeof(int));
    return (int)sendmsg(sock, &m, 0);
}

static void test_gc(void) {
    /* Each round leaks two 64 KiB rings and two sockets without collection. */
    const int rounds = 200;
    long f0 = -1;
    for (int r = 0; r < rounds + 20; r++) {
        if (r == 20) f0 = free_kb();           /* after a warm-up */
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) { CHECK(0, "socketpair %d", r); return; }
        int ok;
        switch (r % 3) {
        case 0: ok = send_fd(sv[0], sv[0]) == 1; break;   /* itself, to the peer */
        case 1: ok = send_fd(sv[0], sv[1]) == 1; break;   /* the peer, to itself */
        default: ok = send_fd(sv[0], sv[0]) == 1 && send_fd(sv[1], sv[1]) == 1; break;
        }
        if (!ok) { CHECK(0, "sendmsg SCM_RIGHTS %d", r); return; }
        close(sv[0]); close(sv[1]);
    }
    long f1 = free_kb();
    if (!on_linux)
        CHECK(f0 - f1 < 4096, "SCM_RIGHTS self-send cycles leaked %ld kB over %d rounds",
              f0 - f1, rounds);
    /* And a received socket still works after all that. */
    int sv[2], pv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    socketpair(AF_UNIX, SOCK_STREAM, 0, pv);
    CHECK(send_fd(sv[0], pv[1]) == 1, "send a live socket");
    close(pv[1]);
    char c;
    union { struct cmsghdr h; char b[CMSG_SPACE(sizeof(int))]; } u;
    struct iovec iov = { &c, 1 };
    struct msghdr m;
    memset(&m, 0, sizeof(m));
    m.msg_iov = &iov; m.msg_iovlen = 1;
    m.msg_control = u.b; m.msg_controllen = sizeof(u.b);
    int got = -1;
    if (recvmsg(sv[1], &m, 0) == 1 && CMSG_FIRSTHDR(&m))
        memcpy(&got, CMSG_DATA(CMSG_FIRSTHDR(&m)), sizeof(int));
    CHECK(got >= 0 && write(got, "q", 1) == 1 && read(pv[0], &c, 1) == 1 && c == 'q',
          "a socket passed over SCM_RIGHTS works");
    close(got); close(pv[0]); close(sv[0]); close(sv[1]);
}

static void test_dgram(void)     { test_records(SOCK_DGRAM, "DGRAM"); }
static void test_seqpacket(void) { test_records(SOCK_SEQPACKET, "SEQPACKET"); }

/* unixprobe [section...]: all sections, or only the named ones. */
int main(int argc, char **argv) {
    static const struct { const char *name; void (*fn)(void); } secs[] = {
        { "close", test_close_blocked }, { "names", test_names },
        { "cred", test_cred }, { "shut", test_shutdown },
        { "dgram", test_dgram }, { "seqpacket", test_seqpacket },
        { "dgram-named", test_dgram_named }, { "gc", test_gc },
    };
    struct utsname u;
    signal(SIGPIPE, SIG_IGN);
    /* MaeroOS calls itself Linux in sysname; its version string says. */
    on_linux = !(uname(&u) == 0 && strstr(u.version, "MaeroOS"));
    alarm(120);

    for (unsigned i = 0; i < sizeof(secs) / sizeof(secs[0]); i++) {
        int want = argc < 2;
        for (int a = 1; a < argc; a++)
            if (strcmp(argv[a], secs[i].name) == 0) want = 1;
        if (!want) continue;
        if (secs[i].fn == test_close_blocked && on_linux) {
            printf("unixprobe: close skipped on Linux (the sleeper stays asleep)\n");
            continue;
        }
        secs[i].fn();
    }

    if (fails) {
        printf("unixprobe: %d check(s) FAILED\n", fails);
        return 1;
    }
    printf("unixprobe ok\n");
    return 0;
}
