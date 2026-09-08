/*
 * P3 sigchld-thread - tests RC3, S3, S4, S11, C2.
 *
 * Linux:
 *  - threads share one signal-handler table (CLONE_SIGHAND, kernel/fork.c:
 *    1690-1694): a sigaction() done by the main thread AFTER a worker was
 *    created is in force for children forked by that worker;
 *  - SIGCHLD is sent to the parent PROCESS when a child process exits and
 *    is delivered to any thread that does not block it (kernel/signal.c:
 *    945-975, complete_signal);
 *  - any thread of the parent may waitpid() for the process's children
 *    (they belong to the process, not to the forking thread);
 *  - a thread exit sends no SIGCHLD;
 *  - in a forked child gettid() == getpid(), and clone() honours
 *    CLONE_CHILD_SETTID / CLONE_PARENT_SETTID also for fork-style clones
 *    (kernel/fork.c:2156-2160).
 *
 * MaeroOS (audit): sig_handlers are copied per thread at clone (proc/
 * syscall.c:5732-5735); proc_exit sends SIGCHLD to the creating THREAD, for
 * thread exits too (proc/scheduler.c:585-588); sys_waitpid matches
 * parent == current thread (proc/syscall.c:942); do_fork ignores the
 * *_SETTID flags (proc/syscall.c:5695-5702).
 */
#define PROBE_NAME "p03_sigchld_thread"
#include "probe.h"
#include <sched.h>
#include <sys/wait.h>

static volatile sig_atomic_t sigchld_count;
static volatile int go_fork, forked, done;
static volatile pid_t child_pid;

static void on_sigchld(int sig)
{
    (void)sig;
    sigchld_count++;
}

static void *worker(void *arg)
{
    (void)arg;
    while (!go_fork)
        sleep_ms(5);
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        /* C2: the forked child's tid must be its own pid. */
        long tid = raw_gettid();
        long pid_ = syscall(SYS_getpid);
        _exit(tid == pid_ ? 0 : 7);
    }
    child_pid = pid;
    forked = 1;
    while (!done)               /* stay alive: no reparenting of the child */
        sleep_ms(5);
    return NULL;
}

int main(void)
{
    probe_watchdog(60);

    pthread_t t;
    if (pthread_create(&t, NULL, worker, NULL) != 0)
        probe_fail("pthread_create: %s", strerror(errno));
    sleep_ms(50);

    /* Install the handler AFTER the worker exists (S3). */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigchld;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGCHLD, &sa, NULL) != 0)
        probe_fail("sigaction: %s", strerror(errno));

    go_fork = 1;
    while (!forked)
        sleep_ms(5);
    if (child_pid < 0)
        probe_fail("fork in worker thread failed");

    /* S4/S11: the main thread waits for a child forked by another thread. */
    int st = 0;
    pid_t w = waitpid(child_pid, &st, 0);
    if (w != child_pid)
        probe_fail("waitpid(%d) from main thread: %s (returned %d)",
                   (int)child_pid, strerror(errno), (int)w);
    if (!WIFEXITED(st))
        probe_fail("child did not exit normally (status 0x%x)", st);
    if (WEXITSTATUS(st) == 7)
        probe_fail("in the forked child gettid() != getpid() (C2)");
    if (WEXITSTATUS(st) != 0)
        probe_fail("child exit status %d", WEXITSTATUS(st));
    sleep_ms(100);
    if (sigchld_count < 1)
        probe_fail("SIGCHLD handler installed by main after the worker was "
                   "created did not run for the worker's child (S3/S4)");
    probe_info("SIGCHLD handler ran %d time(s), waitpid from main succeeded",
               (int)sigchld_count);

    /* C2 directly: fork-style raw clone with the tid pointers.  i386 order:
     * clone(flags, newsp, parent_tid, tls, child_tid).  The child only makes
     * raw syscalls (its libc thread descriptor is stale). */
    pid_t ctid = 0, ptid = 0;
    fflush(stdout);
    long r = syscall(SYS_clone, SIGCHLD | CLONE_CHILD_SETTID | CLONE_PARENT_SETTID,
                     0, &ptid, 0, &ctid);
    if (r == 0) {
        long tid = syscall(SYS_gettid);
        syscall(SYS_exit_group, ctid == tid ? 0 : 8);
    }
    if (r < 0)
        probe_fail("raw clone(SIGCHLD|CHILD_SETTID|PARENT_SETTID): %s", strerror(errno));
    if (waitpid((pid_t)r, &st, 0) != (pid_t)r)
        probe_fail("waitpid(raw clone child %ld): %s", r, strerror(errno));
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
        probe_fail("CLONE_CHILD_SETTID not honoured for a fork-style clone "
                   "(child status 0x%x)", st);
    if (ptid != (pid_t)r)
        probe_fail("CLONE_PARENT_SETTID wrote %d, expected child pid %ld", (int)ptid, r);
    probe_info("fork-style clone: CHILD_SETTID and PARENT_SETTID honoured");

    /* A thread exit must not raise SIGCHLD. */
    sigchld_count = 0;
    done = 1;
    pthread_join(t, NULL);
    sleep_ms(200);
    if (sigchld_count != 0)
        probe_fail("thread exit raised SIGCHLD (%d)", (int)sigchld_count);

    probe_pass();
}
