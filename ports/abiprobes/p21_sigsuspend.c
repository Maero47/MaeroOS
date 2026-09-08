/*
 * P21 sigsuspend - the canonical "block, then wait for it" idiom.
 *
 * Linux: sigsuspend() installs the given mask, sleeps until a signal with a
 * handler is delivered, runs the handler, and returns -1/EINTR with the
 * CALLER's mask back in force (kernel/signal.c sigsuspend + set_restore_sigmask:
 * the saved mask is reinstated by the signal-return path, not by the syscall,
 * so the awaited signal is still deliverable when that path looks for one).
 *
 * The bug this catches: a kernel that puts the caller's mask back before
 * returning -ERESTARTNOHAND re-blocks the very signal being waited for, so the
 * return-to-user path finds nothing deliverable and transparently restarts the
 * call; the restarted call re-installs the temporary mask, sees the signal
 * pending, does not sleep, and returns the same restart code.  The handler
 * never runs and the process spins in the kernel at 100% CPU.  That is a hang,
 * so the watchdog is what turns it into a FAIL line here.
 *
 * Three cases, each the shape real programs use:
 *   A  signal delivered by another thread while sigsuspend is blocked
 *   B  signal already pending (raised while blocked) BEFORE sigsuspend
 *   C  a second sigsuspend after the first returned, to prove the mask that
 *      case A/B restored is the caller's and not the temporary one
 */
#define PROBE_NAME "p21_sigsuspend"
#include "probe.h"

static volatile sig_atomic_t handler_runs;
static volatile sig_atomic_t handler_sig;

static void on_usr1(int s)
{
    handler_sig = s;
    handler_runs++;
}

/* SIGUSR1 blocked?  Reported through sigprocmask so we read the mask the
 * kernel actually has, not the one we think we set. */
static int usr1_blocked(void)
{
    sigset_t cur;
    sigemptyset(&cur);
    if (sigprocmask(SIG_BLOCK, NULL, &cur) != 0)
        probe_fail("sigprocmask(query): %s", strerror(errno));
    return sigismember(&cur, SIGUSR1);
}

static void *sender(void *arg)
{
    pthread_t target = *(pthread_t *)arg;
    sleep_ms(200);
    pthread_kill(target, SIGUSR1);
    return NULL;
}

int main(void)
{
    struct sigaction sa;
    sigset_t block_usr1, wait_mask, old;
    pthread_t me = pthread_self(), th;
    int r, e;
    double t0;

    /* A hang is the failure mode, so bound the whole probe. */
    probe_watchdog(60);

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_usr1;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGUSR1, &sa, NULL) != 0)
        probe_fail("sigaction: %s", strerror(errno));

    sigemptyset(&block_usr1);
    sigaddset(&block_usr1, SIGUSR1);
    if (sigprocmask(SIG_BLOCK, &block_usr1, &old) != 0)
        probe_fail("sigprocmask(block): %s", strerror(errno));
    if (!usr1_blocked())
        probe_fail("SIGUSR1 is not blocked after sigprocmask(SIG_BLOCK)");

    /* The mask sigsuspend waits under: everything the caller had, minus
     * SIGUSR1.  This is the whole point of the call. */
    wait_mask = old;
    sigdelset(&wait_mask, SIGUSR1);

    /* ── A: another thread signals us while we are suspended ────────────── */
    handler_runs = 0;
    handler_sig = 0;
    if (pthread_create(&th, NULL, sender, &me) != 0)
        probe_fail("pthread_create: %s", strerror(errno));

    t0 = now_ms();
    r = sigsuspend(&wait_mask);
    e = errno;
    probe_info("A: sigsuspend returned %d (errno %d %s) after %.0f ms, handler ran %d time(s)",
               r, e, strerror(e), now_ms() - t0, (int)handler_runs);

    if (r != -1 || e != EINTR)
        probe_fail("A: sigsuspend returned %d/errno %d, expected -1/EINTR", r, e);
    if (handler_runs != 1)
        probe_fail("A: handler ran %d times, expected exactly 1", (int)handler_runs);
    if (handler_sig != SIGUSR1)
        probe_fail("A: handler got signal %d, expected %d", (int)handler_sig, SIGUSR1);
    if (!usr1_blocked())
        probe_fail("A: SIGUSR1 is unblocked after sigsuspend returned — the "
                   "caller's mask was not restored");
    pthread_join(th, NULL);

    /* ── B: the signal is already pending when sigsuspend is called ─────── */
    handler_runs = 0;
    if (raise(SIGUSR1) != 0)
        probe_fail("raise: %s", strerror(errno));
    if (handler_runs != 0)
        probe_fail("B: handler ran while SIGUSR1 was blocked");

    t0 = now_ms();
    r = sigsuspend(&wait_mask);
    e = errno;
    probe_info("B: pending-first sigsuspend returned %d (errno %d %s) after %.0f ms, "
               "handler ran %d time(s)", r, e, strerror(e), now_ms() - t0,
               (int)handler_runs);

    if (r != -1 || e != EINTR)
        probe_fail("B: sigsuspend returned %d/errno %d, expected -1/EINTR", r, e);
    if (handler_runs != 1)
        probe_fail("B: handler ran %d times, expected exactly 1", (int)handler_runs);
    if (!usr1_blocked())
        probe_fail("B: SIGUSR1 is unblocked after sigsuspend returned");

    /* ── C: the restored mask really is the caller's ────────────────────── */
    handler_runs = 0;
    if (pthread_create(&th, NULL, sender, &me) != 0)
        probe_fail("pthread_create: %s", strerror(errno));
    t0 = now_ms();
    r = sigsuspend(&wait_mask);
    e = errno;
    if (r != -1 || e != EINTR || handler_runs != 1)
        probe_fail("C: second sigsuspend returned %d/errno %d with %d handler run(s)",
                   r, e, (int)handler_runs);
    if (!usr1_blocked())
        probe_fail("C: SIGUSR1 is unblocked after the second sigsuspend");
    pthread_join(th, NULL);
    probe_info("C: a second sigsuspend behaves identically (%.0f ms), so the mask "
               "restored by the first was the caller's", now_ms() - t0);

    /* Leave the process as we found it. */
    if (sigprocmask(SIG_SETMASK, &old, NULL) != 0)
        probe_fail("sigprocmask(restore): %s", strerror(errno));

    probe_pass();
}
