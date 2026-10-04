/*
 * P82 EPOLLET and EPOLLONESHOT.
 *
 * Linux: an edge-triggered registration of /proc/self/mountinfo (always
 * readable; it wakes waiters only when the mount table changes) is reported
 * once, after EPOLL_CTL_ADD, and not again until EPOLL_CTL_MOD re-arms it.
 * This is how libmount's mount monitor (GIO, the GTK file chooser) watches
 * it.  EPOLLONESHOT reports once and then nothing until MOD, even while the
 * fd stays ready.
 *
 * MaeroOS before: both flags were ignored (level-triggered): the mountinfo
 * item was reported on every epoll_wait, so GLib's worker thread spun in
 * epoll_pwait forever and Mousepad hung after Ctrl+S.
 */
#define PROBE_NAME "p82_epoll_et_oneshot"
#include "probe.h"
#include <sys/epoll.h>

static int wait0(int ep)
{
    struct epoll_event ev;
    return epoll_wait(ep, &ev, 1, 0);
}

int main(void)
{
    probe_watchdog(60);
    int ep = epoll_create1(0);
    if (ep < 0) probe_fail("epoll_create1: %s", strerror(errno));
    int mi = open("/proc/self/mountinfo", O_RDONLY);
    if (mi < 0) probe_skip("no /proc/self/mountinfo");
    struct epoll_event ev = { .events = EPOLLIN | EPOLLET };
    ev.data.fd = mi;
    if (epoll_ctl(ep, EPOLL_CTL_ADD, mi, &ev) != 0)
        probe_fail("EPOLL_CTL_ADD mountinfo: %s", strerror(errno));
    int a = wait0(ep), b = wait0(ep), c = wait0(ep);
    probe_info("mountinfo EPOLLET: %d %d %d", a, b, c);
    if (a != 1) probe_fail("the first wait after ADD reports %d, want 1", a);
    if (b != 0 || c != 0)
        probe_fail("EPOLLET mountinfo reported again (%d, %d): level-triggered", b, c);
    if (epoll_ctl(ep, EPOLL_CTL_MOD, mi, &ev) != 0)
        probe_fail("EPOLL_CTL_MOD: %s", strerror(errno));
    if (wait0(ep) != 1) probe_fail("MOD did not re-arm the EPOLLET item");
    epoll_ctl(ep, EPOLL_CTL_DEL, mi, NULL);

    int p[2];
    if (pipe(p) != 0) probe_fail("pipe: %s", strerror(errno));
    if (write(p[1], "x", 1) != 1) probe_fail("write");
    ev.events = EPOLLIN | EPOLLONESHOT;
    ev.data.fd = p[0];
    if (epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev) != 0)
        probe_fail("EPOLL_CTL_ADD pipe: %s", strerror(errno));
    a = wait0(ep); b = wait0(ep);
    probe_info("pipe EPOLLONESHOT: %d %d", a, b);
    if (a != 1 || b != 0)
        probe_fail("EPOLLONESHOT on a ready pipe: %d then %d, want 1 then 0", a, b);
    if (epoll_ctl(ep, EPOLL_CTL_MOD, p[0], &ev) != 0 || wait0(ep) != 1)
        probe_fail("MOD did not re-arm the EPOLLONESHOT item");

    /* A plain level-triggered pipe item keeps reporting while ready. */
    int q[2];
    if (pipe(q) != 0 || write(q[1], "y", 1) != 1) probe_fail("pipe 2");
    int ep2 = epoll_create1(0);
    ev.events = EPOLLIN;
    ev.data.fd = q[0];
    epoll_ctl(ep2, EPOLL_CTL_ADD, q[0], &ev);
    if (wait0(ep2) != 1 || wait0(ep2) != 1)
        probe_fail("a level-triggered pipe item stopped reporting");
    probe_pass();
}
