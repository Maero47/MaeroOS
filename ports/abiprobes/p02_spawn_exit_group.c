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
 */
#define PROBE_NAME "p02_spawn_exit_group"
#include "probe.h"
#include <spawn.h>
#include <sys/wait.h>

extern char **environ;

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

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--helper-pid") == 0) {
        printf("pid=%d\n", (int)getpid());
        fflush(stdout);
        return 0;
    }

    char self[4096];
    if (!probe_self_path(argv[0], self, sizeof self))
        probe_skip("cannot find own executable path (argv[0]=%s)", argv[0]);

    probe_watchdog(60);

    pthread_t t;
    if (pthread_create(&t, NULL, ticker, NULL) != 0)
        probe_fail("pthread_create: %s", strerror(errno));
    sleep_ms(50);

    /* Case A */
    pid_t pid = -1;
    char *argv_a[] = { "abiprobe-nonexistent", NULL };
    int rc = posix_spawn(&pid, "/nonexistent/abiprobe-helper", NULL, NULL, argv_a, environ);
    if (rc != ENOENT)
        probe_fail("posix_spawn(/nonexistent) returned %d (%s), expected ENOENT",
                   rc, strerror(rc));
    int before = ticks;
    sleep_ms(200);
    if (ticks == before)
        probe_fail("second thread stopped running after the failed posix_spawn");
    probe_info("posix_spawn(/nonexistent) = ENOENT, process and threads alive");

    /* Case B */
    int pfd[2];
    if (pipe(pfd) != 0)
        probe_fail("pipe: %s", strerror(errno));
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, pfd[1], 1);
    posix_spawn_file_actions_addclose(&fa, pfd[0]);
    posix_spawn_file_actions_addclose(&fa, pfd[1]);
    char *argv_b[] = { self, "--helper-pid", NULL };
    fflush(stdout);
    rc = posix_spawn(&pid, self, &fa, NULL, argv_b, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(pfd[1]);
    if (rc != 0)
        probe_fail("posix_spawn(%s --helper-pid) failed: %s", self, strerror(rc));

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
    if (waitpid(pid, &st, 0) != pid)
        probe_fail("waitpid(%d): %s", (int)pid, strerror(errno));
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
        probe_fail("helper did not exit 0 (status 0x%x)", st);
    int hpid = -1;
    if (sscanf(buf, "pid=%d", &hpid) != 1)
        probe_fail("helper output unparsable: %.60s", buf);
    if (hpid == (int)getpid())
        probe_fail("helper's getpid() %d equals the parent's pid", hpid);
    if (hpid != (int)pid)
        probe_fail("helper's getpid() %d differs from the spawned pid %d", hpid, (int)pid);
    probe_info("spawned helper pid %d (parent %d)", hpid, (int)getpid());

    stop = 1;
    pthread_join(t, NULL);
    probe_pass();
}
