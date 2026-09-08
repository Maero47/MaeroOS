/*
 * P15 socket cloexec - tests C5.
 *
 * Linux: SOCK_CLOEXEC on socket()/socketpair()/accept4() sets FD_CLOEXEC on
 * the new descriptor (net/socket.c, sock_map_fd with O_CLOEXEC), and
 * SOCK_NONBLOCK sets O_NONBLOCK; EPOLL_CLOEXEC, EFD_CLOEXEC and
 * pipe2(O_CLOEXEC) likewise.  Without the flag FD_CLOEXEC is clear.
 *
 * MaeroOS (audit): socket()/socketpair()/accept4() ignore SOCK_CLOEXEC
 * (proc/syscall.c:5892, :5952-5958, :6002-6008).
 */
#define PROBE_NAME "p15_socket_cloexec"
#include "probe.h"
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/un.h>

static void expect_cloexec(int fd, int want, const char *what)
{
    int fl = fcntl(fd, F_GETFD);
    if (fl < 0)
        probe_fail("%s: fcntl(F_GETFD): %s", what, strerror(errno));
    if (!!(fl & FD_CLOEXEC) != want)
        probe_fail("%s: FD_CLOEXEC is %s, expected %s", what,
                   (fl & FD_CLOEXEC) ? "set" : "clear", want ? "set" : "clear");
}

int main(void)
{
    probe_watchdog(60);
    int s, sv[2], p[2];

    s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s < 0)
        probe_fail("socket(SOCK_CLOEXEC): %s", strerror(errno));
    expect_cloexec(s, 1, "socket(SOCK_STREAM|SOCK_CLOEXEC)");
    close(s);
    s = socket(AF_UNIX, SOCK_STREAM, 0);
    expect_cloexec(s, 0, "socket(SOCK_STREAM)");
    close(s);
    s = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (s < 0 || !(fcntl(s, F_GETFL) & O_NONBLOCK))
        probe_fail("socket(SOCK_NONBLOCK) did not set O_NONBLOCK");
    close(s);

    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0)
        probe_fail("socketpair(SOCK_CLOEXEC): %s", strerror(errno));
    expect_cloexec(sv[0], 1, "socketpair(SOCK_CLOEXEC)[0]");
    expect_cloexec(sv[1], 1, "socketpair(SOCK_CLOEXEC)[1]");
    close(sv[0]);
    close(sv[1]);
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
        probe_fail("socketpair: %s", strerror(errno));
    expect_cloexec(sv[0], 0, "socketpair()[0]");
    close(sv[0]);
    close(sv[1]);
    probe_info("socket/socketpair honour SOCK_CLOEXEC and SOCK_NONBLOCK");

    /* accept4 */
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof a.sun_path, "/tmp/p15.sock.%d", (int)getpid());
    unlink(a.sun_path);
    int l = socket(AF_UNIX, SOCK_STREAM, 0);
    if (bind(l, (struct sockaddr *)&a, sizeof a) != 0)
        probe_fail("bind(%s): %s", a.sun_path, strerror(errno));
    if (listen(l, 1) != 0)
        probe_fail("listen: %s", strerror(errno));
    int c = socket(AF_UNIX, SOCK_STREAM, 0);
    if (connect(c, (struct sockaddr *)&a, sizeof a) != 0)
        probe_fail("connect: %s", strerror(errno));
    int acc = accept4(l, NULL, NULL, SOCK_CLOEXEC);
    if (acc < 0)
        probe_fail("accept4(SOCK_CLOEXEC): %s", strerror(errno));
    expect_cloexec(acc, 1, "accept4(SOCK_CLOEXEC)");
    close(acc);
    close(c);
    close(l);
    unlink(a.sun_path);
    probe_info("accept4 honours SOCK_CLOEXEC");

    int ep = epoll_create1(EPOLL_CLOEXEC);
    if (ep < 0)
        probe_fail("epoll_create1: %s", strerror(errno));
    expect_cloexec(ep, 1, "epoll_create1(EPOLL_CLOEXEC)");
    close(ep);
    int ev = eventfd(0, EFD_CLOEXEC);
    if (ev < 0)
        probe_fail("eventfd: %s", strerror(errno));
    expect_cloexec(ev, 1, "eventfd(EFD_CLOEXEC)");
    close(ev);
    if (pipe2(p, O_CLOEXEC) != 0)
        probe_fail("pipe2: %s", strerror(errno));
    expect_cloexec(p[0], 1, "pipe2(O_CLOEXEC)[0]");
    expect_cloexec(p[1], 1, "pipe2(O_CLOEXEC)[1]");
    close(p[0]);
    close(p[1]);
    probe_info("epoll_create1, eventfd and pipe2 honour their CLOEXEC flags");

    probe_pass();
}
