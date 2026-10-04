/*
 * P54 auxv of a set-uid exec: AT_SECURE=1 and the new effective uid.
 *
 * As root, the probe copies itself to /tmp as a set-uid-root file, drops to
 * uid/gid 65534 in a child and execs the copy.  The new image reads its
 * auxiliary vector: AT_UID must be 65534, AT_EUID 0 and AT_SECURE 1 (Linux
 * secureexec), so the dynamic loader ignores LD_PRELOAD/LD_LIBRARY_PATH.  A
 * plain exec of the same file without the bit must give AT_SECURE 0.
 *
 * MaeroOS before: the auxv was built from the caller's credentials before
 * the set-uid bit was applied, so the set-uid-root image saw AT_EUID 65534
 * and AT_SECURE 0 while running with euid 0: ld.so would honour LD_PRELOAD
 * from an unprivileged caller (local root).
 */
#define PROBE_NAME "p54_setid_auxv"
#include "probe.h"
#include <sys/auxv.h>
#include <sys/stat.h>
#include <sys/wait.h>

static const char *copy_path = "/tmp/p54_setid";

static int child_report(const char *mode)
{
    unsigned long sec = getauxval(AT_SECURE), uid = getauxval(AT_UID);
    unsigned long euid = getauxval(AT_EUID);
    printf("info %s: %s: AT_UID=%lu AT_EUID=%lu AT_SECURE=%lu euid=%d\n",
           PROBE_NAME, mode, uid, euid, sec, (int)geteuid());
    fflush(stdout);
    if (strcmp(mode, "setuid") == 0) {
        if (geteuid() != 0) return 2;            /* bit not honoured here */
        return (sec == 1 && euid == 0 && uid == 65534) ? 0 : 1;
    }
    return (sec == 0 && euid == 65534 && uid == 65534) ? 0 : 1;
}

static int run_as_nobody(const char *mode)
{
    pid_t c = fork();
    if (c < 0) probe_fail("fork: %s", strerror(errno));
    if (c == 0) {
        if (setgid(65534) != 0 || setuid(65534) != 0) _exit(4);
        char *argv[] = { (char *)copy_path, (char *)mode, NULL };
        char *envp[] = { NULL };
        execve(copy_path, argv, envp);
        _exit(127);
    }
    int st;
    if (waitpid(c, &st, 0) != c) probe_fail("waitpid: %s", strerror(errno));
    if (!WIFEXITED(st)) probe_fail("%s child died with status %#x", mode, st);
    return WEXITSTATUS(st);
}

int main(int argc, char **argv)
{
    if (argc > 1) return child_report(argv[1]);
    probe_watchdog(60);
    if (geteuid() != 0) probe_skip("needs root to create a set-uid-root file");

    char self[512];
    if (!probe_self_path(argv[0], self, sizeof self)) probe_fail("own path unknown");
    int in = open(self, O_RDONLY);
    if (in < 0) probe_fail("open %s: %s", self, strerror(errno));
    unlink(copy_path);
    int out = open(copy_path, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (out < 0) probe_fail("create %s: %s", copy_path, strerror(errno));
    char buf[8192];
    ssize_t n;
    while ((n = read(in, buf, sizeof buf)) > 0)
        if (write(out, buf, (size_t)n) != n) probe_fail("copy: %s", strerror(errno));
    close(in);
    close(out);
    if (chown(copy_path, 0, 0) != 0) probe_fail("chown: %s", strerror(errno));

    if (chmod(copy_path, 0755) != 0) probe_fail("chmod: %s", strerror(errno));
    int r = run_as_nobody("plain");
    if (r != 0) probe_fail("plain exec as uid 65534: wrong auxv (status %d)", r);

    if (chmod(copy_path, 04755) != 0) probe_fail("chmod 04755: %s", strerror(errno));
    r = run_as_nobody("setuid");
    unlink(copy_path);
    if (r == 2) probe_skip("the set-uid bit is not honoured on /tmp (nosuid)");
    if (r != 0)
        probe_fail("set-uid-root exec from uid 65534: auxv does not say AT_SECURE=1 / "
                   "AT_EUID=0 (status %d)", r);
    probe_pass();
}
