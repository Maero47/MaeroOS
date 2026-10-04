/*
 * P71 the AF_UNIX garbage collector sees a listener's unaccepted connections.
 *
 * Linux: L listens on an abstract name, C connects (never accepted, so the
 * server end S waits in L's backlog), C sends L itself over the connection
 * (SCM_RIGHTS) and both descriptors are closed.  L is then reachable only
 * through a message queued on S, and S only through L: unix_gc's
 * scan_children walks the embryo's queue, finds the cycle and frees it, so
 * the name can be bound again.
 *
 * MaeroOS: the collector only scanned candidates' own receive queues, never a
 * listener's backlog, so L, S, two 64 KiB rings and the bound name leaked for
 * good: the bind below failed with EADDRINUSE forever.
 */
#define PROBE_NAME "p71_unix_gc_backlog"
#include "probe.h"
#include <stddef.h>
#include <sys/socket.h>
#include <sys/un.h>

static socklen_t name_of(struct sockaddr_un *a, int round)
{
    memset(a, 0, sizeof *a);
    a->sun_family = AF_UNIX;
    int n = snprintf(a->sun_path + 1, sizeof a->sun_path - 1,
                     "p71-gc-%d-%d", (int)getpid(), round);
    return (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + n);
}

static void cycle(int round)
{
    struct sockaddr_un a;
    socklen_t al = name_of(&a, round);
    int L = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (L < 0) probe_fail("socket: %s", strerror(errno));
    if (bind(L, (struct sockaddr *)&a, al) != 0)
        probe_fail("round %d: bind: %s", round, strerror(errno));
    if (listen(L, 4) != 0) probe_fail("listen: %s", strerror(errno));
    int C = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (connect(C, (struct sockaddr *)&a, al) != 0)
        probe_fail("connect: %s", strerror(errno));

    char c = 'x';
    struct iovec iov = { &c, 1 };
    union { struct cmsghdr h; char b[CMSG_SPACE(sizeof(int))]; } u;
    struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1,
                         .msg_control = u.b, .msg_controllen = sizeof u.b };
    struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
    cm->cmsg_level = SOL_SOCKET;
    cm->cmsg_type = SCM_RIGHTS;
    cm->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cm), &L, sizeof L);
    if (sendmsg(C, &mh, 0) != 1) probe_fail("sendmsg: %s", strerror(errno));
    close(L);
    close(C);
}

/* The name comes free once the cycle is collected; Linux collects from a
 * work queue, so give it a moment. */
static void rebind(int round)
{
    struct sockaddr_un a;
    socklen_t al = name_of(&a, round);
    for (int tries = 0; ; tries++) {
        int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (bind(s, (struct sockaddr *)&a, al) == 0) { close(s); return; }
        int e = errno;
        close(s);
        if (e != EADDRINUSE)
            probe_fail("round %d: rebind: %s", round, strerror(e));
        if (tries >= 40)
            probe_fail("round %d: the name stays bound: the listener in the "
                       "cycle was never collected", round);
        /* Linux runs the collector on the next close of an in-flight
         * socket; poke it with a throwaway pair. */
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0) { close(sv[0]); close(sv[1]); }
        sleep_ms(50);
    }
}

int main(void)
{
    probe_watchdog(60);
    for (int round = 0; round < 3; round++) {
        cycle(round);
        rebind(round);
    }
    probe_pass();
}
