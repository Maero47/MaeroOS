/*
 * P2 spawn-exit-group - tests RC2, C1, S12.
 *
 * Linux: a CLONE_VM child created without CLONE_THREAD gets its own thread
 * group (kernel/fork.c:2039-2047: CLONE_THREAD needs CLONE_SIGHAND; without
 * CLONE_THREAD a new signal_struct is made), so exit_group() in that child
 * ends only the child.  posix_spawn relies on this: glibc 2.36 does
 * clone3(CLONE_VM | CLONE_VFORK, exit_signal = SIGCHLD) and _exit(127) on
 * exec failure (sysdeps/unix/sysv/linux/spawni.c:306, :383-390); musl does
 * __clone(child, stack, CLONE_VM | CLONE_VFORK | SIGCHLD, ...) and
 * _exit(127) which is SYS_exit_group (src/process/posix_spawn.c,
 * src/exit/_Exit.c).  The kernel-visible sequence is the same: clone (not
 * clone3) here, so a CLONE_VM-without-CLONE_THREAD fix covers both.
 *
 * MaeroOS (audit): sys_clone puts every CLONE_VM child into the parent's
 * thread group (proc/syscall.c:5751-5757) and sys_exit_group kills every
 * thread with that tgid (:3301-3307): the whole parent dies.
 *
 * Case A: posix_spawn("/nonexistent") from a two-threaded process must return
 *         ENOENT and both threads must still be alive afterwards.
 * Case B: posix_spawn of this binary with --helper-pid; the helper's getpid()
 *         must equal the pid posix_spawn returned and differ from ours.
 *
 * Both cases run in a forked child that reports through a pipe, because on a
 * kernel with RC2 the case-A process is SIGKILLed by its own failed spawn.
 * Observing that from the parent turns a silent death into a FAIL line
 * naming the finding; running the spawn in the probe's own process would
 * leave the harness with no verdict at all.
 */
#define PROBE_NAME "p02_spawn_exit_group"
#include "probe.h"
#include <spawn.h>
#include <sys/wait.h>

extern char **environ;

static char self[4096];

static volatile int ticks;
static volatile int stop;

static void *ticker(void *arg)
{
    (void)arg;
    while (!stop) {
        ticks++;
        sleep_ms(10);
    }
    return NULL;
}

/* Child side: report through the pipe only.  A verdict line printed here
 * would be parsed as the probe's result by tools/smoke_abi.py. */
static void report(int fd, const char *kind, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    dprintf(fd, "%s %s\n", kind, buf);
}

static void case_a(int fd)
{
    pthread_t t;
    if (pthread_create(&t, NULL, ticker, NULL) != 0) {
        report(fd, "ERR", "pthread_create: %s", strerror(errno));
        return;
    }
    sleep_ms(50);

    pid_t pid = -1;
    char *argv_a[] = { "abiprobe-nonexistent", NULL };
    int rc = posix_spawn(&pid, "/nonexistent/abiprobe-helper", NULL, NULL, argv_a, environ);
    if (rc != ENOENT) {
        report(fd, "ERR", "posix_spawn(/nonexistent) returned %d (%s), expected ENOENT",
               rc, strerror(rc));
        return;
    }
    int before = ticks;
    sleep_ms(200);
    if (ticks == before) {
        report(fd, "ERR", "the second thread stopped running after the failed posix_spawn");
        return;
    }
    stop = 1;
    pthread_join(t, NULL);
    report(fd, "OK", "posix_spawn(/nonexistent) = ENOENT, process and both threads alive");
}

static void case_b(int fd)
{
    int pfd[2];
    if (pipe(pfd) != 0) {
        report(fd, "ERR", "pipe: %s", strerror(errno));
        return;
    }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, pfd[1], 1);
    posix_spawn_file_actions_addclose(&fa, pfd[0]);
    posix_spawn_file_actions_addclose(&fa, pfd[1]);
    char *argv_b[] = { self, "--helper-pid", NULL };
    pid_t pid = -1;
    int rc = posix_spawn(&pid, self, &fa, NULL, argv_b, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(pfd[1]);
    if (rc != 0) {
        report(fd, "ERR", "posix_spawn(%s --helper-pid) failed: %s", self, strerror(rc));
        return;
    }

    char buf[128];
    size_t n = 0;
    for (;;) {
        ssize_t r = read(pfd[0], buf + n, sizeof buf - 1 - n);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            break;
        n += (size_t)r;
        if (n >= sizeof buf - 1)
            break;
    }
    buf[n] = 0;
    close(pfd[0]);

    int st = 0;
    if (waitpid(pid, &st, 0) != pid) {
        report(fd, "ERR", "waitpid(%d): %s", (int)pid, strerror(errno));
        return;
    }
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        report(fd, "ERR", "helper did not exit 0 (status 0x%x)", st);
        return;
    }
    int hpid = -1;
    if (sscanf(buf, "pid=%d", &hpid) != 1) {
        report(fd, "ERR", "helper output unparsable: '%.60s'", buf);
        return;
    }
    if (hpid == (int)getpid()) {
        report(fd, "ERR", "the helper's getpid() %d equals the spawning process's pid", hpid);
        return;
    }
    if (hpid != (int)pid) {
        report(fd, "ERR", "the helper's getpid() %d differs from the spawned pid %d",
               hpid, (int)pid);
        return;
    }
    report(fd, "OK", "spawned helper reported pid %d, spawner is %d", hpid, (int)getpid());
}

static void run_case(const char *label, void (*fn)(int))
{
    int pfd[2];
    if (pipe(pfd) != 0)
        probe_fail("%s: pipe: %s", label, strerror(errno));
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0)
        probe_fail("%s: fork: %s", label, strerror(errno));
    if (pid == 0) {
        close(pfd[0]);
        fn(pfd[1]);
        close(pfd[1]);
        _exit(0);
    }
    close(pfd[1]);

    char buf[512];
    size_t n = 0;
    for (;;) {
        ssize_t r = read(pfd[0], buf + n, sizeof buf - 1 - n);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            break;
        n += (size_t)r;
        if (n >= sizeof buf - 1)
            break;
    }
    buf[n] = 0;
    close(pfd[0]);
    for (char *c = buf; *c; c++)
        if (*c == '\n')
            *c = 0;

    int st = 0;
    if (waitpid(pid, &st, 0) != pid)
        probe_fail("%s: waitpid(%d): %s", label, (int)pid, strerror(errno));
    if (WIFSIGNALED(st))
        probe_fail("%s: the process running the spawn was killed by signal %d "
                   "(RC2/C1: the CLONE_VM child of posix_spawn shares the parent's "
                   "thread group, so its exit_group killed the parent)%s%s",
                   label, WTERMSIG(st), n ? "; last words: " : "", n ? buf : "");
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
        probe_fail("%s: the process running the spawn exited with status %d%s%s",
                   label, WIFEXITED(st) ? WEXITSTATUS(st) : -1,
                   n ? "; last words: " : "", n ? buf : "");
    if (strncmp(buf, "ERR ", 4) == 0)
        probe_fail("%s: %s", label, buf + 4);
    if (strncmp(buf, "OK ", 3) != 0)
        probe_fail("%s: the process running the spawn produced no result line "
                   "(got '%.80s')", label, buf);
    probe_info("%s: %s", label, buf + 3);
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--helper-pid") == 0) {
        printf("pid=%d\n", (int)getpid());
        fflush(stdout);
        return 0;
    }

    if (!probe_self_path(argv[0], self, sizeof self))
        probe_skip("cannot find own executable path (argv[0]=%s)", argv[0]);

    probe_watchdog(60);
    run_case("case A, posix_spawn of a missing binary", case_a);
    run_case("case B, pid of a posix_spawn'd helper", case_b);
    probe_pass();
}
