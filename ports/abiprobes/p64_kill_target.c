/*
 * P64 MaeroOS syscall 505 (Ctrl+Alt+Backspace kill target) is not a way to
 * have other users' processes killed.
 *
 * An unprivileged caller cannot register init or a process it could not
 * signal itself (EPERM); nobody can register init.  A caller may register
 * its own child, and clear the target with -1.
 *
 * Linux has no syscall 505 (ENOSYS): every check is then skipped.
 *
 * MaeroOS before: any pid was accepted from anyone, and the keyboard
 * interrupt sent SIGKILL to it with no credential check.
 */
#define PROBE_NAME "p64_kill_target"
#include "probe.h"
#include <sys/wait.h>

int main(void)
{
    probe_watchdog(60);
    if (syscall(505, -1) != 0) {
        if (errno == ENOSYS) probe_skip("no syscall 505 (not MaeroOS)");
        probe_fail("clearing the target: %s", strerror(errno));
    }
    if (syscall(505, 1) == 0) {
        syscall(505, -1);
        probe_fail("init (pid 1) was accepted as the kill target");
    }

    pid_t kid = fork();
    if (kid == 0) { for (;;) pause(); }
    if (kid < 0) probe_fail("fork: %s", strerror(errno));
    if (syscall(505, kid) != 0) probe_fail("registering own child: %s", strerror(errno));
    syscall(505, -1);

    pid_t c = fork();
    if (c == 0) {
        if (geteuid() == 0 && (setgid(65534) != 0 || setuid(65534) != 0)) _exit(4);
        if (syscall(505, 1) == 0) _exit(1);
        if (syscall(505, (long)kid) == 0) _exit(2);   /* root's process */
        if (errno != EPERM) _exit(3);
        _exit(0);
    }
    int st;
    waitpid(c, &st, 0);
    kill(kid, SIGKILL);
    waitpid(kid, NULL, 0);
    syscall(505, -1);
    if (!WIFEXITED(st)) probe_fail("child status %#x", st);
    switch (WEXITSTATUS(st)) {
    case 0: probe_pass();
    case 1: probe_fail("uid 65534 registered init");
    case 2: probe_fail("uid 65534 registered a root process");
    case 3: probe_fail("refusal was not EPERM");
    default: probe_fail("setuid failed");
    }
}
