/*
 * P44 /proc/<pid>/fd/N links: one's own, and another user's.
 *
 * Linux: readlink("/proc/self/fd/N") names the open file (a path,
 * "pipe:[...]", "socket:[...]"); musl's ttyname() relies on it.  Another
 * user's process's links are refused with EACCES (ptrace read access), so an
 * unprivileged process cannot list what root daemons have open.
 *
 * MaeroOS: the links were EINVAL (not symlinks), then readable for every
 * process by anyone.
 */
#define PROBE_NAME "p44_proc_fd_link"
#include "probe.h"
#include <sys/wait.h>

int main(void)
{
    probe_watchdog(60);
    char link[256], buf[256];
    int fd = open("/dev/null", O_RDONLY);
    snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
    ssize_t n = readlink(link, buf, sizeof buf - 1);
    if (n < 0) probe_fail("readlink %s: %s", link, strerror(errno));
    buf[n] = 0;
    if (strcmp(buf, "/dev/null") != 0) probe_fail("%s -> \"%s\"", link, buf);
    int p[2];
    if (pipe(p) != 0) probe_fail("pipe: %s", strerror(errno));
    snprintf(link, sizeof link, "/proc/%d/fd/%d", (int)getpid(), p[0]);
    n = readlink(link, buf, sizeof buf - 1);
    if (n < 0) probe_fail("readlink %s: %s", link, strerror(errno));
    buf[n] = 0;
    if (strncmp(buf, "pipe:[", 6) != 0) probe_fail("%s -> \"%s\"", link, buf);

    if (geteuid() != 0) {
        /* Not root: init's descriptors are another user's. */
        if (readlink("/proc/1/fd/0", buf, sizeof buf) >= 0 || errno != EACCES)
            probe_fail("/proc/1/fd/0 as a user: want EACCES, got %s",
                       errno == EACCES ? "a link" : strerror(errno));
        probe_pass();
    }
    pid_t parent = getpid();
    pid_t pid = fork();
    if (pid == 0) {
        if (setgid(65534) != 0 || setuid(65534) != 0) _exit(3);
        char l[64], b[256];
        snprintf(l, sizeof l, "/proc/%d/fd/%d", (int)parent, fd);
        if (readlink(l, b, sizeof b) >= 0) _exit(1);
        if (errno != EACCES) _exit(2);
        snprintf(l, sizeof l, "/proc/self/fd/%d", fd);   /* its own: fine */
        _exit(readlink(l, b, sizeof b) > 0 ? 0 : 4);
    }
    int st;
    waitpid(pid, &st, 0);
    if (!WIFEXITED(st)) probe_fail("child died");
    switch (WEXITSTATUS(st)) {
    case 0: break;
    case 1: probe_fail("an unprivileged process read root's /proc/<pid>/fd link");
    case 2: probe_fail("another user's fd link: want EACCES");
    case 3: probe_fail("setuid(65534) failed");
    default: probe_fail("the child could not read its own fd link");
    }
    probe_pass();
}
