/*
 * P1 fatal-signal-scope - tests RC1, S2.
 *
 * Linux: a fatal signal with SIG_DFL disposition delivered to ONE thread ends
 * the whole thread group.  complete_signal() SIGKILLs the siblings
 * (kernel/signal.c:985-993, zap_other_threads :1328-1348) and get_signal()
 * calls do_group_exit(signr) (kernel/signal.c:3039).  A sibling that is
 * sleeping therefore never wakes up to print "still alive", and the parent
 * that waits for the process sees WIFSIGNALED with that signal.
 *
 * MaeroOS (audit): signal_deliver_pending runs proc_exit(128 + sig) for the
 * current thread only (proc/signal.c:124-127); the page-fault path does the
 * same (arch/i686/mm/paging.c:641-666).
 *
 * Three cases, each in a forked child with a worker thread that dies 100 ms
 * after start while the main thread sleeps 2 s:
 *   raise(SIGSEGV)  - musl raise() is tkill(gettid(), sig), like glibc's
 *                     tgkill(getpid(), gettid(), sig) (nptl/pthread_kill.c:42-44)
 *   abort()         - SIGABRT
 *   write to NULL   - a real page fault, SIGSEGV
 * A child that exits normally with status 3 was still alive after its worker
 * died, which is the MaeroOS failure mode.
 */
#define PROBE_NAME "p01_fatal_signal_scope"
#include "probe.h"
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/wait.h>

static int *null_ptr;   /* stays NULL; the compiler cannot prove it */

static void *victim(void *arg)
{
    int mode = *(int *)arg;
    sleep_ms(100);
    if (mode == 0)
        raise(SIGSEGV);
    else if (mode == 1)
        abort();
    else
        *(volatile int *)null_ptr = 1;
    /* Only reached if the fatal signal was not fatal for this thread. */
    return NULL;
}

static const char *mode_name[] = { "raise(SIGSEGV)", "abort()", "write to NULL" };
static const int mode_sig[] = { SIGSEGV, SIGABRT, SIGSEGV };

static void run_case(int mode)
{
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0)
        probe_fail("fork: %s", strerror(errno));
    if (pid == 0) {
        struct rlimit rl = { 0, 0 };
        setrlimit(RLIMIT_CORE, &rl);       /* no core files on the host */
        prctl(PR_SET_DUMPABLE, 0, 0, 0, 0); /* nor a core_pattern helper */
        pthread_t t;
        if (pthread_create(&t, NULL, victim, &mode) != 0)
            _exit(4);
        sleep_ms(2000);
        _exit(3);                          /* still alive: wrong */
    }
    int st = 0;
    double t0 = now_ms();
    if (waitpid(pid, &st, 0) != pid)
        probe_fail("%s: waitpid: %s", mode_name[mode], strerror(errno));
    double dt = now_ms() - t0;
    if (WIFEXITED(st) && WEXITSTATUS(st) == 3)
        probe_fail("%s: process still alive 2 s after its worker thread died "
                   "(exit status 3)", mode_name[mode]);
    if (WIFEXITED(st) && WEXITSTATUS(st) == 4)
        probe_fail("%s: pthread_create failed in child", mode_name[mode]);
    if (!WIFSIGNALED(st))
        probe_fail("%s: expected termination by signal %d, got exit status %d",
                   mode_name[mode], mode_sig[mode], WIFEXITED(st) ? WEXITSTATUS(st) : -1);
    if (WTERMSIG(st) != mode_sig[mode])
        probe_fail("%s: terminated by signal %d, expected %d", mode_name[mode],
                   WTERMSIG(st), mode_sig[mode]);
    probe_info("%s: whole process died with signal %d after %.0f ms",
               mode_name[mode], WTERMSIG(st), dt);
}

int main(void)
{
    probe_watchdog(60);
    for (int m = 0; m < 3; m++)
        run_case(m);
    probe_pass();
}
