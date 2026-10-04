/*
 * P57 mode 000 means no access; /etc/shadow is root's.
 *
 * Linux: a file or directory chmod'ed to 000 (or created with mode 0) can be
 * opened, listed or searched by nobody but root.  /etc/shadow is 0600 or
 * 0640 root, never readable by an ordinary user.
 *
 * MaeroOS: a mask of 0 was read as "synthetic node without a mode" and
 * granted 0555, so `chmod 000 ~/secret` left the file readable by every
 * user; the initrd made every file 0755, /etc/shadow (the password hashes)
 * included, and a diskless boot resolves /etc/shadow there.
 *
 * Runs in /tmp (tmpfs) and in each directory given (smoke-abi: /disk).
 * smoke-cmds runs it on the diskless initrd boot.  As root; the checks as
 * another user run in a child with uid/gid 65534.
 */
#define PROBE_NAME "p57_mode_zero"
#include "probe.h"
#include <grp.h>
#include <sys/stat.h>
#include <sys/wait.h>

static char f0[200], fc[200], d0[200], d0f[256];

static int as_nobody(void)
{
    gid_t none[1] = { 65534 };
    if (setgroups(1, none) || setgid(65534) || setuid(65534)) return 99;
    int fd;
    if ((fd = open(f0, O_RDONLY)) >= 0 || errno != EACCES) {
        printf("info p57_mode_zero: %s (chmod 000): %s\n", f0,
               fd >= 0 ? "opened" : strerror(errno));
        return 1;
    }
    if ((fd = open(fc, O_RDONLY)) >= 0 || errno != EACCES) {
        printf("info p57_mode_zero: %s (created mode 0): %s\n", fc,
               fd >= 0 ? "opened" : strerror(errno));
        return 2;
    }
    if ((fd = open(d0f, O_RDONLY)) >= 0 || errno != EACCES) {
        printf("info p57_mode_zero: %s (in a 000 dir): %s\n", d0f,
               fd >= 0 ? "opened" : strerror(errno));
        return 3;
    }
    if ((fd = open("/etc/shadow", O_RDONLY)) >= 0) {
        printf("info p57_mode_zero: /etc/shadow opened by uid 65534\n");
        return 4;
    }
    return 0;
}

static void run_in(const char *dir)
{
    int pid = (int)getpid();
    snprintf(f0, sizeof f0, "%s/p57a.%d", dir, pid);
    snprintf(fc, sizeof fc, "%s/p57c.%d", dir, pid);
    snprintf(d0, sizeof d0, "%s/p57d.%d", dir, pid);
    snprintf(d0f, sizeof d0f, "%s/f", d0);
    int fd = open(f0, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0 || write(fd, "x", 1) != 1) probe_fail("create %s: %s", f0, strerror(errno));
    close(fd);
    if (chmod(f0, 0) != 0) probe_fail("chmod 000 %s: %s", f0, strerror(errno));
    fd = open(fc, O_CREAT | O_WRONLY | O_TRUNC, 0);
    if (fd < 0) probe_fail("create %s mode 0: %s", fc, strerror(errno));
    close(fd);
    if (mkdir(d0, 0755) != 0) probe_fail("mkdir %s: %s", d0, strerror(errno));
    fd = open(d0f, O_CREAT | O_WRONLY, 0644);
    if (fd < 0) probe_fail("create %s: %s", d0f, strerror(errno));
    close(fd);
    if (chmod(d0, 0) != 0) probe_fail("chmod 000 %s: %s", d0, strerror(errno));
    pid_t c = fork();
    if (c == 0) { fflush(stdout); _exit(as_nobody()); }
    int ws = 0;
    waitpid(c, &ws, 0);
    /* root still reads all of it */
    fd = open(f0, O_RDONLY);
    if (fd < 0) probe_fail("root: open %s: %s", f0, strerror(errno));
    close(fd);
    unlink(f0); unlink(fc); unlink(d0f); rmdir(d0);
    if (!WIFEXITED(ws) || WEXITSTATUS(ws) != 0)
        probe_fail("in %s: check %d as nobody failed (see the info line)", dir,
                   WIFEXITED(ws) ? WEXITSTATUS(ws) : -1);
}

int main(int argc, char **argv)
{
    probe_watchdog(60);
    if (geteuid() != 0) probe_skip("needs root");
    run_in("/tmp");
    for (int i = 1; i < argc; i++) run_in(argv[i]);
    probe_pass();
}
