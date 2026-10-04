/*
 * P70 SCM_RIGHTS batches queued on an AF_UNIX socket are bounded.
 *
 * Linux charges every fd-carrying skb's truesize to the sender's sk_sndbuf,
 * so a sender that keeps passing fds with 1-byte messages nobody reads gets
 * EAGAIN after a few hundred messages (or ETOOMANYREFS once the user's
 * in-flight count passes RLIMIT_NOFILE).  Reading them all back delivers
 * every fd, and the socket is writable again.
 *
 * MaeroOS: each batch was a 5 KiB kernel allocation charged to nothing; the
 * 64 KiB ring only counted the data bytes, so one socketpair queued ~65,000
 * batches (330 MB) and kmalloc failed across the whole kernel.
 *
 * Both a stream socketpair and a SOCK_SEQPACKET one are flooded; a blocked
 * sender is woken by the reader draining.
 */
#define PROBE_NAME "p70_scm_rights_budget"
#include "probe.h"
#include <poll.h>
#include <sys/socket.h>

#define LIMIT 8192          /* far more than Linux queues, far less than 65536 */

static int send_fd(int s, int fd, int flags)
{
    char c = 'x';
    struct iovec iov = { &c, 1 };
    union { struct cmsghdr h; char b[CMSG_SPACE(sizeof(int))]; } u;
    struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1,
                         .msg_control = u.b, .msg_controllen = sizeof u.b };
    struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
    cm->cmsg_level = SOL_SOCKET;
    cm->cmsg_type = SCM_RIGHTS;
    cm->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cm), &fd, sizeof fd);
    return (int)sendmsg(s, &mh, flags);
}

/* One message back: returns the fd it carried, -1 without one, -2 on error. */
static int recv_fd(int s)
{
    char c;
    struct iovec iov = { &c, 1 };
    union { struct cmsghdr h; char b[CMSG_SPACE(sizeof(int) * 4)]; } u;
    struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1,
                         .msg_control = u.b, .msg_controllen = sizeof u.b };
    ssize_t n = recvmsg(s, &mh, MSG_DONTWAIT);
    if (n != 1) return -2;
    struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
    if (!cm || cm->cmsg_type != SCM_RIGHTS) return -1;
    int fd;
    memcpy(&fd, CMSG_DATA(cm), sizeof fd);
    return fd;
}

static int flooder_fd;
static volatile int flooder_done, flooder_rc;

static void *blocked_sender(void *arg)
{
    (void)arg;
    flooder_rc = send_fd(flooder_fd, 0, 0);       /* blocking */
    flooder_done = 1;
    return NULL;
}

static void flood(int type, const char *what)
{
    int sv[2];
    if (socketpair(AF_UNIX, type | SOCK_CLOEXEC, 0, sv) != 0)
        probe_fail("%s socketpair: %s", what, strerror(errno));
    int sent = 0, err = 0;
    while (sent < LIMIT) {
        if (send_fd(sv[0], sv[0], MSG_DONTWAIT) == 1) { sent++; continue; }
        err = errno;
        break;
    }
    if (sent >= LIMIT)
        probe_fail("%s: %d fd-passing messages queued unread, no EAGAIN", what, sent);
    if (err != EAGAIN && err != ETOOMANYREFS && err != ENOBUFS)
        probe_fail("%s: flood ended after %d with %s", what, sent, strerror(err));
    if (sent < 16)
        probe_fail("%s: only %d messages before %s", what, sent, strerror(err));
    probe_info("%s: %d messages queued, then %s", what, sent, strerror(err));

    struct pollfd p = { sv[0], POLLOUT, 0 };
    if (poll(&p, 1, 0) == 1 && err == EAGAIN && type == SOCK_STREAM)
        probe_info("%s: POLLOUT while full (data room left)", what);

    /* A blocking sender sleeps until the reader makes room (ETOOMANYREFS
     * would fail it at once instead: no thread then). */
    int blocking = err == EAGAIN;
    flooder_fd = sv[0];
    flooder_done = 0;
    flooder_rc = 1;
    pthread_t t;
    if (blocking && pthread_create(&t, NULL, blocked_sender, NULL) != 0)
        probe_fail("pthread_create");
    sleep_ms(200);
    int early = blocking && flooder_done;

    int got = 0;
    for (;;) {
        int fd = recv_fd(sv[1]);
        if (fd == -2) break;
        if (fd < 0) probe_fail("%s: message %d came without its fd", what, got);
        close(fd);
        got++;
    }
    if (blocking) pthread_join(t, NULL);
    if (flooder_rc != 1)
        probe_fail("%s: blocked sender woke with %d (%s)", what, flooder_rc,
                   strerror(errno));
    if (early)
        probe_info("%s: the blocking send did not have to wait", what);
    /* Its message may have landed after the drain loop stopped. */
    int fd = recv_fd(sv[1]);
    if (fd >= 0) { close(fd); got++; }
    if (got != sent + blocking)
        probe_fail("%s: %d fds sent, %d received", what, sent + blocking, got);

    p.revents = 0;
    if (poll(&p, 1, 0) != 1 || !(p.revents & POLLOUT))
        probe_fail("%s: not writable after the queue drained", what);
    if (send_fd(sv[0], sv[0], MSG_DONTWAIT) != 1)
        probe_fail("%s: send after drain: %s", what, strerror(errno));
    close(sv[0]);
    close(sv[1]);
}

int main(void)
{
    probe_watchdog(120);
    flood(SOCK_STREAM, "stream");
    flood(SOCK_SEQPACKET, "seqpacket");
    probe_pass();
}
