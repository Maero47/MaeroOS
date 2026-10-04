/*
 * P72 recvmsg with an unwritable control buffer installs no fds.
 *
 * Linux (scm_detach_fds / receive_fd_user): a received fd is installed only
 * after its number has been written to the caller's control buffer; when that
 * write faults the fds are dropped and MSG_CTRUNC is set, and the data is
 * still returned.  Nothing appears in the fd table that the caller cannot
 * name.
 *
 * MaeroOS: the fds were installed first and the failed copy ignored, so each
 * such recvmsg leaked descriptors the caller never learned about.
 *
 * Checked on a stream and a seqpacket socketpair, with msg_control pointing
 * at a read-only page.
 */
#define PROBE_NAME "p72_recvmsg_bad_control"
#include "probe.h"
#include <sys/mman.h>
#include <sys/socket.h>

static int open_fds(void)
{
    int n = 0;
    for (int fd = 0; fd < 1024; fd++)
        if (fcntl(fd, F_GETFD) >= 0) n++;
    return n;
}

static void check(int type, const char *what, void *ro)
{
    int sv[2];
    if (socketpair(AF_UNIX, type | SOCK_CLOEXEC, 0, sv) != 0)
        probe_fail("%s socketpair: %s", what, strerror(errno));
    int passed = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (passed < 0) probe_fail("open /dev/null: %s", strerror(errno));

    char c = 'x';
    struct iovec iov = { &c, 1 };
    union { struct cmsghdr h; char b[CMSG_SPACE(sizeof(int) * 2)]; } u;
    struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1,
                         .msg_control = u.b, .msg_controllen = CMSG_SPACE(sizeof(int) * 2) };
    struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
    cm->cmsg_level = SOL_SOCKET;
    cm->cmsg_type = SCM_RIGHTS;
    cm->cmsg_len = CMSG_LEN(sizeof(int) * 2);
    int two[2] = { passed, passed };
    memcpy(CMSG_DATA(cm), two, sizeof two);
    if (sendmsg(sv[0], &mh, 0) != 1) probe_fail("%s sendmsg: %s", what, strerror(errno));
    close(passed);

    int before = open_fds();
    struct msghdr rh = { .msg_iov = &iov, .msg_iovlen = 1,
                         .msg_control = ro, .msg_controllen = 64 };
    c = 0;
    ssize_t n = recvmsg(sv[1], &rh, MSG_DONTWAIT);
    int after = open_fds();
    probe_info("%s: recvmsg -> %zd (%s), msg_flags %#x, fds %d -> %d", what, n,
               n < 0 ? strerror(errno) : "ok", n < 0 ? 0 : rh.msg_flags, before, after);
    if (after != before)
        probe_fail("%s: %d fd(s) installed that the control buffer could not name",
                   what, after - before);
    if (n == 1 && c != 'x') probe_fail("%s: wrong data", what);
    if (n == 1 && !(rh.msg_flags & MSG_CTRUNC))
        probe_fail("%s: fds dropped without MSG_CTRUNC", what);
    close(sv[0]);
    close(sv[1]);
}

int main(void)
{
    probe_watchdog(60);
    void *ro = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ro == MAP_FAILED) probe_fail("mmap: %s", strerror(errno));
    check(SOCK_STREAM, "stream", ro);
    check(SOCK_SEQPACKET, "seqpacket", ro);
    probe_pass();
}
