/*
 * P10 unix-socket semantics - tests U2, U3, U4, U5.
 *
 * Linux AF_UNIX stream sockets (net/unix/af_unix.c):
 *  U2 SCM_RIGHTS fds ride on the skb of the bytes they were sent with and
 *     are delivered by the first recvmsg() that consumes any of those bytes
 *     (unix_stream_read_generic, :2643-2669), so an 8 KiB sendmsg with one
 *     fd read in 4 KiB pieces yields the fd with the FIRST piece;
 *  U3 once the bytes were consumed by a plain recv() the fd is gone: a
 *     later recvmsg() on the empty socket blocks or returns EAGAIN, never 0
 *     with a control message;
 *  U4 MSG_PEEK leaves the data in place; MSG_DONTWAIT returns EAGAIN on an
 *     empty socket; MSG_NOSIGNAL turns SIGPIPE into a plain EPIPE;
 *  U5 poll() reports POLLHUP after the peer closed.
 *
 * MaeroOS (audit): fds are tagged at the END of the sendmsg data
 * (proc/usocket.c:288-303) and delivered with the last piece; a recvmsg that
 * finds fds but no data returns 0 (proc/syscall.c:6071-6148); MSG_PEEK,
 * MSG_DONTWAIT and MSG_NOSIGNAL are ignored (:6027-6148, usocket.c:235);
 * poll never reports POLLHUP for sockets (:4073-4076).
 */
#define PROBE_NAME "p10_unix_socket"
#include "probe.h"
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

static volatile sig_atomic_t sigpipes;
static void on_pipe(int s) { (void)s; sigpipes++; }

static ssize_t send_with_fd(int sock, const void *buf, size_t len, int fd)
{
    struct iovec iov = { (void *)buf, len };
    union { struct cmsghdr h; char b[CMSG_SPACE(sizeof(int))]; } cm;
    memset(&cm, 0, sizeof cm);
    struct msghdr m;
    memset(&m, 0, sizeof m);
    m.msg_iov = &iov;
    m.msg_iovlen = 1;
    m.msg_control = &cm;
    m.msg_controllen = CMSG_LEN(sizeof(int));
    struct cmsghdr *c = CMSG_FIRSTHDR(&m);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &fd, sizeof fd);
    return sendmsg(sock, &m, 0);
}

/* recvmsg into buf; *fd_out = received fd or -1. */
static ssize_t recv_with_fd(int sock, void *buf, size_t len, int *fd_out, int flags)
{
    struct iovec iov = { buf, len };
    union { struct cmsghdr h; char b[CMSG_SPACE(sizeof(int) * 4)]; } cm;
    memset(&cm, 0, sizeof cm);
    struct msghdr m;
    memset(&m, 0, sizeof m);
    m.msg_iov = &iov;
    m.msg_iovlen = 1;
    m.msg_control = &cm;
    m.msg_controllen = sizeof cm;
    ssize_t r = recvmsg(sock, &m, flags);
    *fd_out = -1;
    if (r >= 0) {
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c))
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS)
                memcpy(fd_out, CMSG_DATA(c), sizeof(int));
    }
    return r;
}

int main(void)
{
    probe_watchdog(60);
    signal(SIGPIPE, on_pipe);
    int sv[2], pfd[2], fd;
    static char big[8192], buf[8192];
    memset(big, 'A', sizeof big);
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
        probe_fail("socketpair: %s", strerror(errno));
    if (pipe(pfd) != 0)
        probe_fail("pipe: %s", strerror(errno));

    /* U2 */
    if (send_with_fd(sv[0], big, 8192, pfd[0]) != 8192)
        probe_fail("sendmsg(8 KiB + fd): %s", strerror(errno));
    ssize_t r = recv_with_fd(sv[1], buf, 4096, &fd, 0);
    if (r != 4096)
        probe_fail("first recvmsg(4 KiB) returned %zd (%s)", r, r < 0 ? strerror(errno) : "-");
    if (fd < 0)
        probe_fail("fd was not delivered with the FIRST 4 KiB piece of an 8 KiB message");
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISFIFO(st.st_mode))
        probe_fail("received fd %d is not the sent pipe", fd);
    close(fd);
    r = recv_with_fd(sv[1], buf, 4096, &fd, 0);
    if (r != 4096)
        probe_fail("second recvmsg(4 KiB) returned %zd (%s)", r, r < 0 ? strerror(errno) : "-");
    if (fd >= 0)
        probe_fail("fd delivered again with the second piece");
    probe_info("SCM_RIGHTS fd arrives with the first piece of the message");

    /* U3 */
    if (send_with_fd(sv[0], "z", 1, pfd[0]) != 1)
        probe_fail("sendmsg(1 byte + fd): %s", strerror(errno));
    if (recv(sv[1], buf, 1, 0) != 1)
        probe_fail("recv(1): %s", strerror(errno));
    int fl = fcntl(sv[1], F_GETFL);
    fcntl(sv[1], F_SETFL, fl | O_NONBLOCK);
    r = recv_with_fd(sv[1], buf, 16, &fd, 0);
    int e = errno;
    fcntl(sv[1], F_SETFL, fl);
    if (r == 0)
        probe_fail("recvmsg after the data was consumed by recv() returned 0%s",
                   fd >= 0 ? " with an fd attached" : "");
    if (r > 0)
        probe_fail("recvmsg after recv() returned %zd bytes of data", r);
    if (e != EAGAIN && e != EWOULDBLOCK)
        probe_fail("recvmsg on the drained socket: %s, expected EAGAIN", strerror(e));
    probe_info("fd consumed together with its byte: later recvmsg gets EAGAIN, not 0");

    /* U4 MSG_PEEK */
    if (send(sv[0], "hello", 5, 0) != 5)
        probe_fail("send: %s", strerror(errno));
    memset(buf, 0, 16);
    r = recv(sv[1], buf, 16, MSG_PEEK);
    if (r != 5 || memcmp(buf, "hello", 5) != 0)
        probe_fail("recv(MSG_PEEK) returned %zd '%.5s'", r, buf);
    memset(buf, 0, 16);
    r = recv(sv[1], buf, 16, 0);
    if (r != 5 || memcmp(buf, "hello", 5) != 0)
        probe_fail("recv after MSG_PEEK returned %zd '%.5s' (peek consumed the data)", r, buf);
    /* U4 MSG_DONTWAIT on an empty socket */
    double t0 = now_ms();
    r = recv(sv[1], buf, 16, MSG_DONTWAIT);
    e = errno;
    if (r != -1 || (e != EAGAIN && e != EWOULDBLOCK))
        probe_fail("recv(MSG_DONTWAIT) on empty socket returned %zd (%s) after %.0f ms",
                   r, r < 0 ? strerror(e) : "-", now_ms() - t0);
    probe_info("MSG_PEEK does not consume; MSG_DONTWAIT gives EAGAIN");

    /* U5 POLLHUP after peer close, then U4 MSG_NOSIGNAL. */
    close(sv[0]);
    struct pollfd p = { sv[1], POLLIN, 0 };
    r = poll(&p, 1, 1000);
    if (r != 1)
        probe_fail("poll after peer close returned %zd", r);
    if (!(p.revents & POLLHUP))
        probe_fail("poll after peer close reports revents 0x%x without POLLHUP", p.revents);
    if (read(sv[1], buf, 16) != 0)
        probe_fail("read after peer close did not return 0");
    sigpipes = 0;
    r = send(sv[1], "x", 1, MSG_NOSIGNAL);
    e = errno;
    if (r != -1 || e != EPIPE)
        probe_fail("send(MSG_NOSIGNAL) to closed peer returned %zd (%s), expected EPIPE",
                   r, r < 0 ? strerror(e) : "-");
    if (sigpipes != 0)
        probe_fail("SIGPIPE raised despite MSG_NOSIGNAL");
    probe_info("POLLHUP reported after peer close; MSG_NOSIGNAL suppresses SIGPIPE");

    probe_pass();
}
