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
 *
 * U6-U8 cover the review of the U2-U5 fix:
 *  U6 the fds must ride with their own message even when the sender BLOCKS
 *     part-way through it: a kernel that tags the batch before writing but
 *     queues it afterwards lets a concurrent reader drain past the tag while
 *     the batch is invisible, and the fds then land on another message's bytes
 *     (or are closed by a plain read that consumes them);
 *  U7 a sendmsg carrying only fds and no data transfers nothing on a stream
 *     socket (unix_stream_sendmsg never enters its loop for len 0, so no skb
 *     holds them and scm_destroy closes them) and the peer must never see a
 *     zero-length message — on SOCK_STREAM a 0 return is EOF;
 *  U8 a control buffer too small to name the fd must leave it CLOSED and set
 *     MSG_CTRUNC (net/core/scm.c scm_detach_fds), never installed in the
 *     receiver's fd table where nothing can name or close it.
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

/* recvmsg with a caller-chosen control buffer size; reports msg_flags. */
static ssize_t recv_ctl(int sock, void *buf, size_t len, void *ctl, size_t ctllen,
                        int *fd_out, int *flags_out)
{
    struct iovec iov = { buf, len };
    struct msghdr m;
    memset(&m, 0, sizeof m);
    m.msg_iov = &iov;
    m.msg_iovlen = 1;
    m.msg_control = ctl;
    m.msg_controllen = ctllen;
    ssize_t r = recvmsg(sock, &m, 0);
    *fd_out = -1;
    if (flags_out) *flags_out = m.msg_flags;
    if (r >= 0)
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c))
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS)
                memcpy(fd_out, CMSG_DATA(c), sizeof(int));
    return r;
}

/* The lowest fd the process would be handed next.  Used to prove that a
 * descriptor was NOT quietly installed behind the caller's back. */
static int lowest_free_fd(void)
{
    int fd = dup(0);
    if (fd < 0)
        probe_fail("dup(0): %s", strerror(errno));
    close(fd);
    return fd;
}

/* U6 back-pressure: the reader side.  Drains `filler` bytes that carry no
 * ancillary data, then asserts the fd arrives with the FIRST piece of the
 * message that follows and with no piece after it. */
#define U6_PIECE 4096
static int u6_sock, u6_filler, u6_msglen;
static void *u6_reader(void *arg)
{
    (void)arg;
    static char rbuf[U6_PIECE];
    union { struct cmsghdr h; char b[CMSG_SPACE(sizeof(int) * 4)]; } cm;
    int fd, got = 0;

    /* Let the sender fill the ring and block inside its sendmsg. */
    sleep_ms(300);

    while (got < u6_filler) {
        int want = u6_filler - got;
        if (want > U6_PIECE) want = U6_PIECE;
        ssize_t r = recv_ctl(u6_sock, rbuf, (size_t)want, &cm, sizeof cm, &fd, NULL);
        if (r <= 0)
            probe_fail("U6: draining the filler returned %zd (%s)", r,
                       r < 0 ? strerror(errno) : "EOF");
        if (fd >= 0)
            probe_fail("U6: an fd was delivered with filler byte %d of %d, before "
                       "the message it was sent with", got, u6_filler);
        got += (int)r;
    }

    int mgot = 0, piece = 0, fd_piece = -1, mfd = -1;
    while (mgot < u6_msglen) {
        int want = u6_msglen - mgot;
        if (want > U6_PIECE) want = U6_PIECE;
        ssize_t r = recv_ctl(u6_sock, rbuf, (size_t)want, &cm, sizeof cm, &fd, NULL);
        if (r <= 0)
            probe_fail("U6: draining the message returned %zd (%s)", r,
                       r < 0 ? strerror(errno) : "EOF");
        if (rbuf[0] != 'M')
            probe_fail("U6: message piece %d does not hold the message bytes", piece);
        if (fd >= 0) {
            if (fd_piece >= 0)
                probe_fail("U6: a second fd arrived with piece %d", piece);
            fd_piece = piece;
            mfd = fd;
        }
        mgot += (int)r;
        piece++;
    }
    if (fd_piece < 0)
        probe_fail("U6: the fd never arrived with its %d-byte message", u6_msglen);
    if (fd_piece != 0)
        probe_fail("U6: the fd arrived with piece %d of its message, not the first",
                   fd_piece);
    struct stat st;
    if (fstat(mfd, &st) != 0 || !S_ISFIFO(st.st_mode))
        probe_fail("U6: the fd delivered with the message is not the sent pipe");
    close(mfd);
    return NULL;
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

    /* U6: sender blocked mid-message by back-pressure. */
    int bp[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, bp) != 0)
        probe_fail("U6 socketpair: %s", strerror(errno));
    static char chunk[U6_PIECE], mchunk[U6_PIECE];
    memset(chunk, 'F', sizeof chunk);
    memset(mchunk, 'M', sizeof mchunk);
    /* Fill the send buffer so the message that follows cannot be written in
     * one go: the sender has to sleep inside sendmsg while the reader drains. */
    fl = fcntl(bp[0], F_GETFL);
    fcntl(bp[0], F_SETFL, fl | O_NONBLOCK);
    int filler = 0;
    for (;;) {
        ssize_t w = write(bp[0], chunk, sizeof chunk);
        if (w <= 0) break;
        filler += (int)w;
        if (filler >= 256 * 1024) break;
    }
    fcntl(bp[0], F_SETFL, fl);
    if (filler <= 0)
        probe_fail("U6: could not fill the send buffer (%s)", strerror(errno));

    int niov = (filler + 65536 + U6_PIECE - 1) / U6_PIECE;
    if (niov > 512) niov = 512;
    u6_sock   = bp[1];
    u6_filler = filler;
    u6_msglen = niov * U6_PIECE;
    pthread_t u6t;
    if (pthread_create(&u6t, NULL, u6_reader, NULL) != 0)
        probe_fail("U6: pthread_create: %s", strerror(errno));
    static struct iovec miov[512];
    for (int i = 0; i < niov; i++) {
        miov[i].iov_base = mchunk;
        miov[i].iov_len  = U6_PIECE;
    }
    union { struct cmsghdr h; char b[CMSG_SPACE(sizeof(int))]; } bcm;
    memset(&bcm, 0, sizeof bcm);
    struct msghdr bm;
    memset(&bm, 0, sizeof bm);
    bm.msg_iov = miov;
    bm.msg_iovlen = (size_t)niov;
    bm.msg_control = &bcm;
    bm.msg_controllen = CMSG_LEN(sizeof(int));
    struct cmsghdr *bc = CMSG_FIRSTHDR(&bm);
    bc->cmsg_level = SOL_SOCKET;
    bc->cmsg_type  = SCM_RIGHTS;
    bc->cmsg_len   = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(bc), &pfd[0], sizeof(int));
    ssize_t sent = sendmsg(bp[0], &bm, 0);
    if (sent != (ssize_t)u6_msglen)
        probe_fail("U6: sendmsg(%d bytes + fd) after %d bytes of filler returned %zd (%s)",
                   u6_msglen, filler, sent, sent < 0 ? strerror(errno) : "-");
    pthread_join(u6t, NULL);
    close(bp[0]);
    close(bp[1]);
    probe_info("fd rides with its own message even with the sender blocked mid-message "
               "(%d B filler, %d B message)", filler, u6_msglen);

    /* U7: a sendmsg with fds but no data. */
    int zs[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, zs) != 0)
        probe_fail("U7 socketpair: %s", strerror(errno));
    r = send_with_fd(zs[0], "", 0, pfd[0]);
    if (r != 0)
        probe_fail("U7: sendmsg(0 bytes + fd) returned %zd (%s), expected 0", r,
                   r < 0 ? strerror(errno) : "-");
    fl = fcntl(zs[1], F_GETFL);
    fcntl(zs[1], F_SETFL, fl | O_NONBLOCK);
    r = recv_with_fd(zs[1], buf, 16, &fd, 0);
    e = errno;
    fcntl(zs[1], F_SETFL, fl);
    if (r == 0)
        probe_fail("U7: recvmsg returned 0 on a connected stream socket%s — every IPC "
                   "library reads that as EOF", fd >= 0 ? " with a control message" : "");
    if (r > 0)
        probe_fail("U7: recvmsg returned %zd bytes after a zero-length sendmsg", r);
    if (e != EAGAIN && e != EWOULDBLOCK)
        probe_fail("U7: recvmsg on the empty socket: %s, expected EAGAIN", strerror(e));
    close(zs[0]);
    close(zs[1]);
    probe_info("a data-less SCM_RIGHTS sendmsg transfers nothing and yields no 0 return");

    /* U8: control buffer too small to name the fd. */
    int ts[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, ts) != 0)
        probe_fail("U8 socketpair: %s", strerror(errno));
    if (send_with_fd(ts[0], "y", 1, pfd[0]) != 1)
        probe_fail("U8: sendmsg(1 byte + fd): %s", strerror(errno));
    int before = lowest_free_fd();
    char small[CMSG_SPACE(sizeof(int))];
    int mflags = 0;
    memset(small, 0, sizeof small);
    r = recv_ctl(ts[1], buf, 16, small, 12, &fd, &mflags);   /* CMSG_LEN(0) */
    if (r != 1)
        probe_fail("U8: recvmsg with a 12-byte control buffer returned %zd (%s)", r,
                   r < 0 ? strerror(errno) : "-");
    if (fd >= 0)
        probe_fail("U8: an fd was named in a control buffer with no room for it");
    if (!(mflags & MSG_CTRUNC))
        probe_fail("U8: msg_flags 0x%x does not report MSG_CTRUNC (0x%x)", mflags, MSG_CTRUNC);
    int after = lowest_free_fd();
    if (after != before)
        probe_fail("U8: the fd was installed anyway (lowest free fd %d -> %d): nothing in "
                   "the process can name or close it", before, after);
    close(ts[0]);
    close(ts[1]);
    probe_info("a control buffer too small for the fd closes it and reports MSG_CTRUNC");

    probe_pass();
}
