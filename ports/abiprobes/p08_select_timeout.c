/*
 * P8 select-timeout - tests E2.
 *
 * Linux: select()/pselect() sleep for exactly the given timeout and return 0
 * when nothing became ready (fs/select.c, core_sys_select -> do_select with
 * an hrtimer-based schedule_hrtimeout_range); a zero timeout returns at once
 * and a ready descriptor returns immediately with the count.
 *
 * MaeroOS (audit): select ignores the timeout except for zero and polls
 * every 50 ms for up to 600 attempts, i.e. about 30 s (proc/syscall.c:
 * 3852-3883).
 */
#define PROBE_NAME "p08_select_timeout"
#include "probe.h"
#include <sys/select.h>

static double timed_select(int fd, int use_pselect, long ms, int *ret)
{
    fd_set rf;
    FD_ZERO(&rf);
    FD_SET(fd, &rf);
    double t0 = now_ms();
    if (use_pselect) {
        struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
        *ret = pselect(fd + 1, &rf, NULL, NULL, &ts, NULL);
    } else {
        struct timeval tv = { ms / 1000, (ms % 1000) * 1000L };
        *ret = select(fd + 1, &rf, NULL, NULL, &tv);
    }
    return now_ms() - t0;
}

int main(void)
{
    probe_watchdog(60);
    int pfd[2];
    if (pipe(pfd) != 0)
        probe_fail("pipe: %s", strerror(errno));
    int r;
    double dt;

    dt = timed_select(pfd[0], 0, 100, &r);
    probe_info("select(idle pipe, 100 ms) = %d after %.1f ms", r, dt);
    if (r != 0)
        probe_fail("select returned %d (%s), expected 0", r, r < 0 ? strerror(errno) : "-");
    if (dt < 95 || dt > 1000)
        probe_fail("select 100 ms timeout took %.1f ms", dt);

    dt = timed_select(pfd[0], 1, 100, &r);
    probe_info("pselect(idle pipe, 100 ms) = %d after %.1f ms", r, dt);
    if (r != 0 || dt < 95 || dt > 1000)
        probe_fail("pselect 100 ms timeout returned %d after %.1f ms", r, dt);

    dt = timed_select(pfd[0], 0, 0, &r);
    if (r != 0 || dt > 50)
        probe_fail("select with zero timeout returned %d after %.1f ms", r, dt);

    if (write(pfd[1], "x", 1) != 1)
        probe_fail("write: %s", strerror(errno));
    dt = timed_select(pfd[0], 0, 5000, &r);
    if (r != 1 || dt > 50)
        probe_fail("select on a readable pipe returned %d after %.1f ms", r, dt);
    probe_info("zero timeout and ready fd return immediately");

    probe_pass();
}
