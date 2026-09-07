/*
 * P4 spurious-futex - tests RC4, F1, F3, F11, S1, S8.
 *
 * Linux: FUTEX_WAIT returns only for a real FUTEX_WAKE (0), a timeout
 * (ETIMEDOUT), a value mismatch (EAGAIN) or a signal that has a handler
 * (EINTR).  Spurious wake-ups are retried inside the kernel
 * (kernel/futex/waitwake.c:719-738), and complete_signal() does not wake a
 * sleeper for a blocked or ignored signal (kernel/signal.c:945-975).
 *
 * MaeroOS (audit): the scheduler "nets" mark parked waiters runnable
 * (proc/scheduler.c:142-252, F11); signal_send wakes any sleeper for any
 * signal (proc/signal.c:54-61, S1) and the futex then returns 0 (F3).
 *
 * Six threads park in a raw FUTEX_WAIT (private, expected value 0) on
 * separate words with no waker for 5 s; thread 0 additionally receives an
 * ignored SIGUSR1 and a blocked SIGUSR2 every 500 ms.  Every return before
 * the final wake is counted.  Linux: 0 returns.
 *
 * S8: a FUTEX_WAKE/FUTEX_WAIT ping-pong between two threads measures the
 * wake-to-run round trip; the average must be well below a 10 ms tick.
 */
#define PROBE_NAME "p04_spurious_futex"
#include "probe.h"
#include <linux/futex.h>

#define NW 6
#define PARK_MS 5000

struct waiter {
    int word;
    int pad[15];               /* one word per cache line */
    int spurious, eintr, other, other_errno;
    pthread_t t;
};
static struct waiter W[NW];

static long futex(int *uaddr, int op, int val, const struct kernel_old_timespec *ts)
{
    return syscall(SYS_futex, uaddr, op, val, ts, NULL, 0);
}

static void *waiter_fn(void *arg)
{
    struct waiter *w = arg;
    if (w == &W[0]) {
        sigset_t s;
        sigemptyset(&s);
        sigaddset(&s, SIGUSR2);
        pthread_sigmask(SIG_BLOCK, &s, NULL);
    }
    while (__atomic_load_n(&w->word, __ATOMIC_ACQUIRE) == 0) {
        long r = futex(&w->word, FUTEX_WAIT_PRIVATE, 0, NULL);
        if (r == 0) {
            if (__atomic_load_n(&w->word, __ATOMIC_ACQUIRE) == 0)
                w->spurious++;
        } else if (errno == EAGAIN) {
            /* value already changed: legitimate */
        } else if (errno == EINTR) {
            w->eintr++;
        } else {
            w->other++;
            w->other_errno = errno;
        }
    }
    return NULL;
}

/* S8 ping-pong */
static int ping, pong;
#define ROUNDS 500

static void *pong_fn(void *arg)
{
    (void)arg;
    for (int i = 0; i < ROUNDS; i++) {
        while (__atomic_load_n(&ping, __ATOMIC_ACQUIRE) == 0)
            futex(&ping, FUTEX_WAIT_PRIVATE, 0, NULL);
        __atomic_store_n(&ping, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&pong, 1, __ATOMIC_RELEASE);
        futex(&pong, FUTEX_WAKE_PRIVATE, 1, NULL);
    }
    return NULL;
}

int main(void)
{
    probe_watchdog(60);
    signal(SIGUSR1, SIG_IGN);

    for (int i = 0; i < NW; i++)
        if (pthread_create(&W[i].t, NULL, waiter_fn, &W[i]) != 0)
            probe_fail("pthread_create: %s", strerror(errno));
    sleep_ms(300);

    double t0 = now_ms();
    int kicks = 0;
    while (now_ms() - t0 < PARK_MS) {
        sleep_ms(500);
        pthread_kill(W[0].t, SIGUSR1);   /* ignored */
        pthread_kill(W[0].t, SIGUSR2);   /* blocked in that thread */
        kicks++;
    }
    for (int i = 0; i < NW; i++) {
        __atomic_store_n(&W[i].word, 1, __ATOMIC_RELEASE);
        futex(&W[i].word, FUTEX_WAKE_PRIVATE, 1, NULL);
    }
    for (int i = 0; i < NW; i++)
        pthread_join(W[i].t, NULL);

    int spurious = 0, eintr = 0, other = 0, oerr = 0;
    for (int i = 0; i < NW; i++) {
        spurious += W[i].spurious;
        eintr += W[i].eintr;
        other += W[i].other;
        if (W[i].other)
            oerr = W[i].other_errno;
    }
    probe_info("%d waiters parked %d ms, %d ignored+blocked signal pairs sent: "
               "%d spurious, %d EINTR, %d other returns",
               NW, PARK_MS, kicks, spurious, eintr, other);
    if (other)
        probe_fail("FUTEX_WAIT failed with errno %d (%s)", oerr, strerror(oerr));
    if (spurious)
        probe_fail("%d FUTEX_WAIT return(s) without wake, timeout or signal", spurious);
    if (eintr)
        probe_fail("%d EINTR return(s) for signals that were ignored or blocked", eintr);

    /* S8 */
    pthread_t pt;
    if (pthread_create(&pt, NULL, pong_fn, NULL) != 0)
        probe_fail("pthread_create: %s", strerror(errno));
    sleep_ms(50);
    int hist[4] = { 0, 0, 0, 0 };   /* <100us, <1ms, <10ms, >=10ms */
    double total = 0, worst = 0;
    for (int i = 0; i < ROUNDS; i++) {
        double a = now_ms();
        __atomic_store_n(&ping, 1, __ATOMIC_RELEASE);
        futex(&ping, FUTEX_WAKE_PRIVATE, 1, NULL);
        while (__atomic_load_n(&pong, __ATOMIC_ACQUIRE) == 0)
            futex(&pong, FUTEX_WAIT_PRIVATE, 0, NULL);
        __atomic_store_n(&pong, 0, __ATOMIC_RELEASE);
        double d = now_ms() - a;
        total += d;
        if (d > worst)
            worst = d;
        hist[d < 0.1 ? 0 : d < 1 ? 1 : d < 10 ? 2 : 3]++;
    }
    pthread_join(pt, NULL);
    double avg = total / ROUNDS;
    probe_info("wake round trip over %d rounds: avg %.3f ms, worst %.3f ms; "
               "<100us:%d <1ms:%d <10ms:%d >=10ms:%d",
               ROUNDS, avg, worst, hist[0], hist[1], hist[2], hist[3]);
    if (avg > 5.0)
        probe_fail("average wake-to-run round trip %.2f ms (expected sub-millisecond)", avg);

    probe_pass();
}
