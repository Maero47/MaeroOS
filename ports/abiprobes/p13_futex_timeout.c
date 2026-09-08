/*
 * P13 futex-timeout code - tests F2, T3.
 *
 * Linux: a FUTEX_WAIT whose timeout expires returns -1/ETIMEDOUT
 * (kernel/futex/waitwake.c:729-730), for relative timeouts and for
 * FUTEX_WAIT_BITSET absolute deadlines on CLOCK_MONOTONIC and, with
 * FUTEX_CLOCK_REALTIME, on CLOCK_REALTIME; an already-expired deadline
 * returns ETIMEDOUT immediately; a value mismatch returns EAGAIN.
 *
 * MaeroOS (audit): a timeout that expires returns 0 (proc/syscall.c:5327),
 * so glibc treats it as a spurious wake and retries (RC4 item 4); the
 * already-expired case does return ETIMEDOUT (:5256, T3).
 */
#define PROBE_NAME "p13_futex_timeout"
#include "probe.h"
#include <linux/futex.h>

static int word;

static long futex(int *uaddr, int op, int val, const struct kernel_old_timespec *ts, int val3)
{
    return syscall(SYS_futex, uaddr, op, val, ts, NULL, val3);
}

static void expect_timeout(const char *what, int op, const struct kernel_old_timespec *ts,
                           double lo, double hi)
{
    errno = 0;
    double t0 = now_ms();
    long r = futex(&word, op, 0, ts, FUTEX_BITSET_MATCH_ANY);
    int e = errno;
    double dt = now_ms() - t0;
    probe_info("%s: r=%ld errno=%s after %.0f ms", what, r, r < 0 ? strerror(e) : "-", dt);
    if (r != -1 || e != ETIMEDOUT)
        probe_fail("%s returned %ld (%s) after %.0f ms, expected -1/ETIMEDOUT",
                   what, r, r < 0 ? strerror(e) : "no error", dt);
    if (dt < lo || dt > hi)
        probe_fail("%s timed out after %.0f ms, expected %.0f..%.0f ms", what, dt, lo, hi);
}

static struct kernel_old_timespec deadline(clockid_t clk, long ms)
{
    struct timespec now;
    struct kernel_old_timespec ts;
    clock_gettime(clk, &now);
    ts.tv_sec = (long)now.tv_sec;
    ts.tv_nsec = now.tv_nsec + ms * 1000000L;
    while (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    while (ts.tv_nsec < 0) { ts.tv_sec--; ts.tv_nsec += 1000000000L; }
    return ts;
}

int main(void)
{
    probe_watchdog(60);
    struct kernel_old_timespec ts;

    ts.tv_sec = 0; ts.tv_nsec = 300000000L;
    expect_timeout("FUTEX_WAIT relative 300 ms", FUTEX_WAIT_PRIVATE, &ts, 280, 1500);

    ts = deadline(CLOCK_REALTIME, 300);
    expect_timeout("FUTEX_WAIT_BITSET|CLOCK_REALTIME +300 ms",
                   FUTEX_WAIT_BITSET_PRIVATE | FUTEX_CLOCK_REALTIME, &ts, 280, 1500);

    ts = deadline(CLOCK_MONOTONIC, 300);
    expect_timeout("FUTEX_WAIT_BITSET monotonic +300 ms",
                   FUTEX_WAIT_BITSET_PRIVATE, &ts, 280, 1500);

    ts = deadline(CLOCK_MONOTONIC, -1000);
    expect_timeout("FUTEX_WAIT_BITSET expired deadline", FUTEX_WAIT_BITSET_PRIVATE, &ts, 0, 50);

    word = 1;
    errno = 0;
    long r = futex(&word, FUTEX_WAIT_PRIVATE, 0, NULL, 0);
    if (r != -1 || errno != EAGAIN)
        probe_fail("FUTEX_WAIT with a mismatching value returned %ld (%s), expected EAGAIN",
                   r, strerror(errno));
    probe_info("value mismatch returns EAGAIN");

    probe_pass();
}
