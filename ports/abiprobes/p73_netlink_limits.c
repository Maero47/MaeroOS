/*
 * P73 netlink: one user cannot pin unbounded reply memory or take every
 * socket, and configuration changes need the opener's privilege too.
 *
 * Linux: each netlink socket queues at most sk_rcvbuf of replies (then
 * ENOBUFS), and socket() is bounded per process by RLIMIT_NOFILE, so root
 * can always open a socket and get its replies.  RTM_NEWADDR/DELADDR and
 * friends take CAP_NET_ADMIN of both the socket's opener and the sender
 * (netlink_net_capable): root writing through a socket nobody opened gets
 * EPERM.
 *
 * MaeroOS before: every socket queued 256 KB of unread replies with no total
 * (128 sockets pinned ~58 MB), one user's sockets could fill the 128-slot
 * table so socket() failed for root too, and netlink checked only the
 * sender.
 *
 * Run as root, the user side runs as nobody (65534); as any other user, only
 * the reply-memory bound is checked.
 */
#define PROBE_NAME "p73_netlink_limits"
#include "probe.h"
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/wait.h>

#define NSOCK_MAX 200
#define NFLOOD 16
#define FLOOD_BYTES (64 * 1024)
#define QUEUE_LIMIT (2 * 1024 * 1024)   /* MaeroOS allows 1 MiB per user */

static int nl_socket(void)
{
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0) return -1;
    struct sockaddr_nl sa = { .nl_family = AF_NETLINK };
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0) { close(fd); return -1; }
    return fd;
}

/* NOOP requests with NLM_F_ACK, FLOOD_BYTES at a time, never read. */
static void flood(int fd)
{
    static char buf[FLOOD_BYTES];
    int n = FLOOD_BYTES / sizeof(struct nlmsghdr);
    for (int i = 0; i < n; i++) {
        struct nlmsghdr *h = (struct nlmsghdr *)buf + i;
        memset(h, 0, sizeof *h);
        h->nlmsg_len = sizeof *h;
        h->nlmsg_type = NLMSG_NOOP;
        h->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
        h->nlmsg_seq = (unsigned)i;
    }
    for (int k = 0; k < 5; k++)
        if (send(fd, buf, sizeof buf, MSG_DONTWAIT) < 0 && errno != ENOBUFS &&
            errno != EAGAIN)
            break;
}

/* Read back what a socket holds; ENOBUFS (lost replies) is fine. */
static long drain(int fd)
{
    static char buf[65536];
    long total = 0;
    for (;;) {
        ssize_t n = recv(fd, buf, sizeof buf, MSG_DONTWAIT);
        if (n > 0) { total += n; continue; }
        if (n < 0 && errno == ENOBUFS) continue;
        return total;
    }
}

/* RTM_DELADDR of 192.0.2.77/32 on interface 1 (lo): harmless anywhere.
 * Returns the NLMSG_ERROR code (0 or -errno). */
static int deladdr(int fd)
{
    struct { struct nlmsghdr h; struct ifaddrmsg a; struct rtattr r; uint32_t ip; } req;
    memset(&req, 0, sizeof req);
    req.h.nlmsg_len = sizeof req;
    req.h.nlmsg_type = RTM_DELADDR;
    req.h.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    req.h.nlmsg_seq = 7373;
    req.a.ifa_family = AF_INET;
    req.a.ifa_prefixlen = 32;
    req.a.ifa_index = 1;
    req.r.rta_len = RTA_LENGTH(4);
    req.r.rta_type = IFA_LOCAL;
    req.ip = inet_addr("192.0.2.77");
    drain(fd);
    if (send(fd, &req, sizeof req, 0) != (ssize_t)sizeof req) return -1000 - errno;
    char buf[4096];
    ssize_t n = recv(fd, buf, sizeof buf, 0);
    if (n < (ssize_t)(sizeof(struct nlmsghdr) + 4)) return -2000;
    struct nlmsghdr *h = (struct nlmsghdr *)buf;
    if (h->nlmsg_type != NLMSG_ERROR) return -3000;
    return ((struct nlmsgerr *)NLMSG_DATA(h))->error;
}

/* The user side.  `ready` is written once its sockets are held and queues
 * full; it then waits for a byte on `go` before reading back.  Exit codes:
 * 0 ok, 10+ a failed step. */
static int user_side(int ready, int go)
{
    static int fds[NSOCK_MAX];
    int n = 0;
    while (n < NSOCK_MAX) {
        int fd = nl_socket();
        if (fd < 0) break;
        fds[n++] = fd;
    }
    if (n < NFLOOD) return 10;
    for (int i = 0; i < NFLOOD; i++) flood(fds[i]);
    char c = (char)(n > 127 ? 127 : n);
    if (ready >= 0) {
        if (write(ready, &c, 1) != 1) return 11;
        if (read(go, &c, 1) != 1) return 12;
    }
    long total = 0;
    for (int i = 0; i < NFLOOD; i++) total += drain(fds[i]);
    printf("info %s: user held %d sockets, %ld bytes of replies queued on %d\n",
           PROBE_NAME, n, total, NFLOOD);
    fflush(stdout);
    if (total > QUEUE_LIMIT) return 13;
    for (int i = 0; i < n; i++) close(fds[i]);
    return 0;
}

int main(void)
{
    probe_watchdog(120);
    if (geteuid() != 0) {
        int r = user_side(-1, -1);
        if (r) probe_fail("as uid %d, step %d failed", (int)geteuid(), r);
        probe_pass();
    }

    /* A socket nobody opened, used by root: EPERM; root's own: not EPERM. */
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) probe_fail("socketpair");
    pid_t c = fork();
    if (c == 0) {
        if (setgid(65534) != 0 || setuid(65534) != 0) _exit(99);
        int fd = nl_socket();
        if (fd < 0) _exit(98);
        char x = 'x';
        struct iovec iov = { &x, 1 };
        union { struct cmsghdr h; char b[CMSG_SPACE(sizeof(int))]; } u;
        struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1,
                             .msg_control = u.b, .msg_controllen = sizeof u.b };
        struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
        cm->cmsg_level = SOL_SOCKET;
        cm->cmsg_type = SCM_RIGHTS;
        cm->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cm), &fd, sizeof fd);
        _exit(sendmsg(sv[1], &mh, 0) == 1 ? 0 : 97);
    }
    int st;
    waitpid(c, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st))
        probe_fail("passing nobody's netlink socket: child status %#x", st);
    {
        char x;
        struct iovec iov = { &x, 1 };
        union { struct cmsghdr h; char b[CMSG_SPACE(sizeof(int))]; } u;
        struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1,
                             .msg_control = u.b, .msg_controllen = sizeof u.b };
        if (recvmsg(sv[0], &mh, 0) != 1 || !CMSG_FIRSTHDR(&mh))
            probe_fail("receiving nobody's netlink socket");
        int ufd;
        memcpy(&ufd, CMSG_DATA(CMSG_FIRSTHDR(&mh)), sizeof ufd);
        int e = deladdr(ufd);
        if (e != -EPERM)
            probe_fail("root writing through a socket nobody opened: %d, want EPERM", e);
        close(ufd);
        int rfd = nl_socket();
        if (rfd < 0) probe_fail("root netlink socket: %s", strerror(errno));
        e = deladdr(rfd);
        if (e == -EPERM || e <= -1000)
            probe_fail("root's own socket: RTM_DELADDR answered %d", e);
        probe_info("RTM_DELADDR: nobody's socket EPERM, root's own %d", e);
        close(rfd);
    }
    close(sv[0]);
    close(sv[1]);

    /* nobody holds as many netlink sockets as it can, with full queues. */
    int ready[2], go[2];
    if (pipe(ready) != 0 || pipe(go) != 0) probe_fail("pipe");
    c = fork();
    if (c == 0) {
        close(ready[0]);
        close(go[1]);
        if (setgid(65534) != 0 || setuid(65534) != 0) _exit(99);
        _exit(user_side(ready[1], go[0]));
    }
    close(ready[1]);
    close(go[0]);
    char held;
    if (read(ready[0], &held, 1) != 1) {
        waitpid(c, &st, 0);
        probe_fail("the unprivileged child gave up (status %#x)", st);
    }
    int rfd = nl_socket();
    int ufd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    int ok_nl = rfd >= 0, ok_udp = ufd >= 0;
    int e = ok_nl ? deladdr(rfd) : 0;
    if (rfd >= 0) close(rfd);
    if (ufd >= 0) close(ufd);
    if (write(go[1], "g", 1) != 1) probe_fail("pipe write");
    waitpid(c, &st, 0);
    if (!ok_nl) probe_fail("root's socket(AF_NETLINK) failed while nobody holds %d", held);
    if (!ok_udp) probe_fail("root's socket(AF_INET) failed while nobody holds %d", held);
    if (e == -EPERM || e <= -1000)
        probe_fail("root got no reply while nobody's queues are full (%d)", e);
    if (!WIFEXITED(st)) probe_fail("the unprivileged child died (status %#x)", st);
    if (WEXITSTATUS(st) == 99) probe_fail("setuid(65534) failed");
    if (WEXITSTATUS(st) == 13)
        probe_fail("nobody kept more than %d bytes of unread replies", QUEUE_LIMIT);
    if (WEXITSTATUS(st)) probe_fail("as nobody, step %d failed", WEXITSTATUS(st));
    probe_pass();
}
