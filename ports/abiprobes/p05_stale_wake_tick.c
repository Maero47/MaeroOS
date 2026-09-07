/*
 * P5 stale-wake-tick - tests F4 (with T4 and E1 as the triggers).
 *
 * Linux: a timeout belongs to the syscall that set it.  A thread whose timed
 * sleep was cut short by a signal and that then blocks in an UNTIMED
 * FUTEX_WAIT stays blocked until a real FUTEX_WAKE (kernel/futex/waitwake.c:
 * 719-738; the hrtimer is armed per futex_wait call).
 *
 * MaeroOS (audit): signal_send does not clear wake_tick and sleep_on does not
 * reset it (proc/signal.c:58-61, proc/scheduler.c:292-301), so the earlier
 * deadline fires into the later untimed wait (proc/scheduler.c:116-121).
 *
 * Two variants, each: worker enters a 3 s timed sleep, main sends a handled
 * SIGUSR1 at 100 ms, the worker then does an untimed FUTEX_WAIT that nobody
 * wakes for 5 s (a stale 3 s deadline would fire inside that window).
 *   A: nanosleep(3 s)         (MaeroOS T4: returns early on any wake)
 *   B: poll(idle pipe, 3 s)   (the audit's original scenario)
 * Linux: the FUTEX_WAIT never returns within the window in either variant.
 */
#define PROBE_NAME "p05_stale_wake_tick"
#include "probe.h"
#include <linux/futex.h>
#include <poll.h>

#define SLEEP_MS 3000
#define WATCH_MS 5000

static volatile int in_futex, futex_returned;
static volatile long futex_ret, futex_errno;
static volatile double futex_ret_ms;
static volatile sig_atomic_t got_usr1;
static int word;
static int pfd[2];

static void on_usr1(int s) { (void)s; got_usr1 = 1; }

static long futex(int *uaddr, int op, int val, const struct kernel_old_timespec *ts)
{
    return syscall(SYS_futex, uaddr, op, val, ts, NULL, 0);
}

static void *worker(void *arg)
{
    int variant = *(int *)arg;
    double t0 = now_ms();
    long r, e;
    if (variant == 0) {
        struct timespec ts = { SLEEP_MS / 1000, (SLEEP_MS % 1000) * 1000000L };
        r = nanosleep(&ts, NULL);
    } else {
        struct pollfd p = { pfd[0], POLLIN, 0 };
        r = poll(&p, 1, SLEEP_MS);
    }
    e = errno;
    probe_info("variant %c: timed %s returned %ld (errno %ld %s) after %.0f ms, handler ran: %d",
               variant ? 'B' : 'A', variant ? "poll" : "nanosleep", r, e,
               r < 0 ? strerror((int)e) : "-", now_ms() - t0, (int)got_usr1);

    __atomic_store_n(&word, 0, __ATOMIC_RELEASE);
    in_futex = 1;
    double t1 = now_ms();
    r = futex(&word, FUTEX_WAIT_PRIVATE, 0, NULL);
    futex_ret = r;
    futex_errno = errno;
    futex_ret_ms = now_ms() - t1;
    futex_returned = 1;
    return NULL;
}

static void run_variant(int variant)
{
    in_futex = futex_returned = 0;
    got_usr1 = 0;
    pthread_t t;
    if (pthread_create(&t, NULL, worker, &variant) != 0)
        probe_fail("pthread_create: %s", strerror(errno));
    sleep_ms(100);
    pthread_kill(t, SIGUSR1);

    double t0 = now_ms();
    while (!in_futex) {
        if (now_ms() - t0 > SLEEP_MS + 2000)
            probe_fail("variant %c: worker never reached the futex wait", variant ? 'B' : 'A');
        sleep_ms(5);
    }
    double t1 = now_ms();
    while (now_ms() - t1 < WATCH_MS) {
        if (futex_returned)
            probe_fail("variant %c: untimed FUTEX_WAIT returned after %.0f ms with no waker "
                       "(ret %ld, errno %ld %s)", variant ? 'B' : 'A',
                       (double)futex_ret_ms, (long)futex_ret, (long)futex_errno,
                       strerror((int)futex_errno));
        sleep_ms(20);
    }
    __atomic_store_n(&word, 1, __ATOMIC_RELEASE);
    futex(&word, FUTEX_WAKE_PRIVATE, 1, NULL);
    pthread_join(t, NULL);
    if (futex_ret != 0 && futex_errno != EAGAIN)
        probe_fail("variant %c: final wake returned %ld errno %ld", variant ? 'B' : 'A',
                   (long)futex_ret, (long)futex_errno);
    probe_info("variant %c: untimed FUTEX_WAIT stayed blocked for %d ms after a %d ms timed sleep",
               variant ? 'B' : 'A', WATCH_MS, SLEEP_MS);
}

int main(void)
{
    probe_watchdog(60);
    if (pipe(pfd) != 0)
        probe_fail("pipe: %s", strerror(errno));
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_usr1;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;                 /* no SA_RESTART */
    sigaction(SIGUSR1, &sa, NULL);

    run_variant(0);
    run_variant(1);
    probe_pass();
}
