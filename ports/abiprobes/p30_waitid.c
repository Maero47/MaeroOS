/*
 * P30 waitid(2): WNOWAIT reports without reaping, WNOHANG clears si_pid.
 *
 * Linux: waitid(P_PID, pid, &si, WEXITED | WNOWAIT | WNOHANG) says whether
 * the child has exited - si_pid is the child and si_code CLD_EXITED /
 * CLD_KILLED with si_status the exit code / signal - and leaves it a zombie,
 * so a later waitpid() still collects it with the same status.  With nothing
 * to report under WNOHANG it returns 0 with si_pid cleared.
 *
 * MaeroOS: syscall 284 did not exist ("[SYSCALL] unimplemented 284", ENOSYS).
 * Firefox's IPC process watcher (base::IsProcessDead) asks exactly the
 * question above about each content process it retires, on every page load
 * that switches processes.  getrusage (77, Firefox's CPU-time telemetry) was
 * missing too and is checked for a sane RUSAGE_SELF answer.
 */
#define PROBE_NAME "p30_waitid"
#include "probe.h"
#include <sys/resource.h>
#include <sys/wait.h>

static void wnowait_until_exit(pid_t pid, int want_code, int want_status)
{
    siginfo_t si;
    for (int tries = 0; ; tries++) {
        memset(&si, 0x55, sizeof si);
        if (waitid(P_PID, pid, &si, WEXITED | WNOWAIT | WNOHANG) != 0)
            probe_fail("waitid(P_PID %d, WNOWAIT|WNOHANG): %s", pid, strerror(errno));
        if (si.si_pid == pid)
            break;
        if (si.si_pid != 0)
            probe_fail("WNOHANG with nothing to report left si_pid %d, want 0", si.si_pid);
        if (tries > 500)
            probe_fail("child %d never reported exited", pid);
        usleep(10000);
    }
    if (si.si_signo != SIGCHLD || si.si_code != want_code || si.si_status != want_status)
        probe_fail("siginfo signo %d code %d status %d, want %d/%d/%d",
                   si.si_signo, si.si_code, si.si_status, SIGCHLD, want_code, want_status);
    /* WNOWAIT: still there, and a second look says the same. */
    memset(&si, 0, sizeof si);
    if (waitid(P_PID, pid, &si, WEXITED | WNOWAIT) != 0 || si.si_pid != pid)
        probe_fail("second WNOWAIT look at %d failed (%s, si_pid %d)",
                   pid, strerror(errno), si.si_pid);
    int st = 0;
    if (waitpid(pid, &st, 0) != pid)
        probe_fail("waitpid after WNOWAIT did not reap %d: %s", pid, strerror(errno));
    if (want_code == CLD_EXITED ? !(WIFEXITED(st) && WEXITSTATUS(st) == want_status)
                                : !(WIFSIGNALED(st) && WTERMSIG(st) == want_status))
        probe_fail("waitpid status 0x%x does not match the waitid report", st);
}

int main(void)
{
    probe_watchdog(60);
    siginfo_t si;

    /* A running child: WNOHANG reports nothing. */
    int pfd[2];
    if (pipe(pfd) != 0)
        probe_fail("pipe: %s", strerror(errno));
    pid_t a = fork();
    if (a < 0)
        probe_fail("fork: %s", strerror(errno));
    if (a == 0) {
        char c;
        close(pfd[1]);
        (void)!read(pfd[0], &c, 1);
        _exit(7);
    }
    close(pfd[0]);
    memset(&si, 0x55, sizeof si);
    if (waitid(P_PID, a, &si, WEXITED | WNOHANG) != 0)
        probe_fail("waitid on a running child: %s", strerror(errno));
    if (si.si_pid != 0)
        probe_fail("running child: si_pid %d, want 0", si.si_pid);
    close(pfd[1]);                                   /* let it exit 7 */
    wnowait_until_exit(a, CLD_EXITED, 7);

    /* A SIGKILLed child reports CLD_KILLED with the signal. */
    pid_t b = fork();
    if (b < 0)
        probe_fail("fork: %s", strerror(errno));
    if (b == 0) {
        for (;;)
            pause();
    }
    kill(b, SIGKILL);
    wnowait_until_exit(b, CLD_KILLED, SIGKILL);

    /* Plain WEXITED (no WNOWAIT) through P_ALL reaps. */
    pid_t c = fork();
    if (c == 0)
        _exit(3);
    memset(&si, 0, sizeof si);
    if (waitid(P_ALL, 0, &si, WEXITED) != 0 || si.si_pid != c || si.si_status != 3)
        probe_fail("waitid(P_ALL, WEXITED): %s, si_pid %d status %d",
                   strerror(errno), si.si_pid, si.si_status);
    if (waitpid(c, NULL, WNOHANG) != -1 || errno != ECHILD)
        probe_fail("child %d still waitable after a reaping waitid", c);

    /* No children at all, and a bad option set. */
    if (waitid(P_ALL, 0, &si, WEXITED | WNOHANG) != -1 || errno != ECHILD)
        probe_fail("no children: want ECHILD, got %s", strerror(errno));
    if (waitid(P_ALL, 0, &si, WNOHANG) != -1 || errno != EINVAL)
        probe_fail("options without WEXITED/WSTOPPED/WCONTINUED: want EINVAL, got %s",
                   strerror(errno));

    struct rusage ru;
    memset(&ru, 0x55, sizeof ru);
    if (getrusage(RUSAGE_SELF, &ru) != 0)
        probe_fail("getrusage(RUSAGE_SELF): %s", strerror(errno));
    if (ru.ru_utime.tv_sec < 0 || ru.ru_utime.tv_usec < 0 || ru.ru_utime.tv_usec >= 1000000)
        probe_fail("getrusage utime %ld.%06ld is not a time",
                   (long)ru.ru_utime.tv_sec, (long)ru.ru_utime.tv_usec);
    if (getrusage(12345, &ru) != -1 || errno != EINVAL)
        probe_fail("getrusage(12345): want EINVAL, got %s", strerror(errno));
    probe_pass();
}
