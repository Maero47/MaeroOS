/*
 * P17 signal-to-busy-thread - tests S2 (delivery to a CPU-bound thread)
 * and S4 (process-directed signal picks a thread that does not block it).
 *
 * Linux: a signal queued to a thread running in user mode is delivered on
 * the next return to user space; the sender kicks the target CPU
 * (kernel/signal.c signal_wake_up -> kick_process), so a spinning thread that
 * makes no syscalls sees its handler within a scheduler tick at most.
 * kill(getpid(), sig) with the signal blocked in the main thread is
 * delivered to the spinner (complete_signal, kernel/signal.c:945-975).
 *
 * MaeroOS (audit): signals are delivered only at a syscall boundary;
 * scheduler_tick never delivers (proc/scheduler.c:254-260), so a spinner
 * gets the signal at its next syscall or never; kill(pid) hits the one
 * ptable entry with that pid (proc/syscall.c:1777-1810).
 */
#define PROBE_NAME "p17_signal_busy_thread"
#include "probe.h"

static volatile int stop;
static volatile sig_atomic_t got;
static volatile double got_ms;
static volatile long got_tid;
static volatile long spinner_tid;
static volatile unsigned long spins;

static void on_term(int s)
{
    (void)s;
    got_ms = now_ms();
    got_tid = raw_gettid();
    got = 1;
}

static void *spinner(void *arg)
{
    (void)arg;
    /* The creating thread may have SIGTERM blocked (case 2); the mask is
     * inherited, so make this thread eligible explicitly. */
    sigset_t s;
    sigemptyset(&s);
    sigaddset(&s, SIGTERM);
    pthread_sigmask(SIG_UNBLOCK, &s, NULL);
    spinner_tid = raw_gettid();
    while (!stop)
        spins++;                 /* pure user mode, no syscalls */
    return NULL;
}

static void one_case(int process_directed)
{
    const char *what = process_directed ? "kill(getpid(), SIGTERM)" : "pthread_kill(spinner, SIGTERM)";
    stop = 0;
    got = 0;
    spinner_tid = 0;
    pthread_t t;
    if (pthread_create(&t, NULL, spinner, NULL) != 0)
        probe_fail("pthread_create: %s", strerror(errno));
    while (!spinner_tid)
        sleep_ms(1);
    sleep_ms(100);                /* let it settle into the loop */

    double t0 = now_ms();
    if (process_directed) {
        if (kill(getpid(), SIGTERM) != 0)
            probe_fail("kill: %s", strerror(errno));
    } else {
        if (pthread_kill(t, SIGTERM) != 0)
            probe_fail("pthread_kill: %s", strerror(errno));
    }
    while (!got && now_ms() - t0 < 3000)
        sleep_ms(1);
    int delivered = got;
    double lat = delivered ? got_ms - t0 : -1;
    long tid = got_tid;
    stop = 1;
    pthread_join(t, NULL);
    if (!delivered && got)
        probe_fail("%s: handler ran only when the spinner left its loop (%.0f ms), "
                   "not while it was spinning", what, got_ms - t0);
    if (!delivered)
        probe_fail("%s: handler did not run within 3 s while the thread spun in user mode", what);
    probe_info("%s: delivered after %.3f ms in tid %ld (spinner tid %ld)", what, lat, tid, (long)spinner_tid);
    if (tid != spinner_tid)
        probe_fail("%s: handler ran in tid %ld, expected the spinner %ld", what, tid, (long)spinner_tid);
    if (lat > 100)
        probe_fail("%s: delivery latency %.1f ms to a spinning thread", what, lat);
}

int main(void)
{
    probe_watchdog(60);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_term;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);

    one_case(0);

    /* Main blocks SIGTERM so a process-directed one must land on the spinner. */
    sigset_t s;
    sigemptyset(&s);
    sigaddset(&s, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &s, NULL);
    one_case(1);
    pthread_sigmask(SIG_UNBLOCK, &s, NULL);

    probe_pass();
}
