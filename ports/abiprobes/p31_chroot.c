/*
 * P31 chroot(2): a process and its children resolve every path inside the
 * new root.
 *
 * Linux: chroot(dir) (root only, -EPERM otherwise) makes `dir` the "/" of
 * the caller's path lookups: absolute paths, absolute symlink targets and
 * ".." at the top all stay inside it, and fork() inherits it.  A file or a
 * missing path fails with -ENOTDIR / -ENOENT.
 *
 * MaeroOS: syscall 61 did not exist (ENOSYS), and the in-tree libc's chroot()
 * was a stub, so toybox chroot failed.  Alpine Linux is run from a directory
 * on the disk with `chroot /disk/alpine` (ports/alpine/README.md).
 *
 * On a Linux host this needs root (else SKIP).  MaeroOS keeps /dev and /proc
 * reachable inside a chroot (nothing can mount them there); Linux does not,
 * so that is not checked here but in tools/smoke_alpine.py.
 */
#define PROBE_NAME "p31_chroot"
#include "probe.h"
#include <sys/stat.h>
#include <sys/wait.h>

#define TOP "/tmp/p31root"

static void put(const char *path, const char *text)
{
    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0 || write(fd, text, strlen(text)) != (ssize_t)strlen(text))
        probe_fail("create %s: %s", path, strerror(errno));
    close(fd);
}

static void expect(const char *path, const char *text)
{
    char buf[64] = {0};
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        probe_fail("open(%s) in the chroot: %s", path, strerror(errno));
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n < 0 || strcmp(buf, text) != 0)
        probe_fail("%s in the chroot reads \"%s\", want \"%s\"", path, buf, text);
}

int main(void)
{
    probe_watchdog(60);
    if (geteuid() != 0)
        probe_skip("needs root");

    mkdir(TOP, 0755);
    mkdir(TOP "/etc", 0755);
    mkdir(TOP "/sub", 0755);
    put(TOP "/etc/hello", "inside");
    put(TOP "/plain", "x");
    unlink(TOP "/abs");
    if (symlink("/etc/hello", TOP "/abs") != 0)
        probe_fail("symlink: %s", strerror(errno));

    if (chroot(TOP "/plain") != -1 || errno != ENOTDIR)
        probe_fail("chroot(file) = %s, want ENOTDIR", strerror(errno));
    if (chroot(TOP "/missing") != -1 || errno != ENOENT)
        probe_fail("chroot(missing) = %s, want ENOENT", strerror(errno));

    /* A non-root caller is refused. */
    pid_t pid = fork();
    if (pid == 0) {
        if (setuid(1000) != 0)
            _exit(3);
        _exit(chroot(TOP) == -1 && errno == EPERM ? 0 : 1);
    }
    int st;
    if (waitpid(pid, &st, 0) != pid || !WIFEXITED(st))
        probe_fail("the uid 1000 child did not exit (status %#x)", st);
    if (WEXITSTATUS(st) == 3)        /* a user namespace maps only uid 0 */
        probe_info("cannot become uid 1000 here, EPERM check skipped");
    else if (WEXITSTATUS(st) != 0)
        probe_fail("chroot as uid 1000 was not refused with EPERM");

    if (chdir(TOP) != 0 || chroot(".") != 0)
        probe_fail("chdir+chroot(\".\"): %s", strerror(errno));
    if (chdir("/") != 0)
        probe_fail("chdir(/): %s", strerror(errno));
    char cwd[256];
    if (!getcwd(cwd, sizeof cwd) || strcmp(cwd, "/") != 0)
        probe_fail("getcwd after chroot = %s, want /", cwd);

    expect("/etc/hello", "inside");
    expect("/abs", "inside");                     /* absolute symlink target */
    expect("/../../../etc/hello", "inside");      /* ".." stops at the root */
    if (chdir("/sub/../..") != 0)
        probe_fail("chdir(/sub/../..): %s", strerror(errno));
    expect("etc/hello", "inside");                /* relative, from the top */
    if (access("/p31root", F_OK) == 0)
        probe_fail("/p31root is visible inside its own chroot");

    pid = fork();
    if (pid == 0) {
        int fd = open("/etc/hello", O_RDONLY);
        char b[8] = {0};
        _exit(fd >= 0 && read(fd, b, 6) == 6 && strcmp(b, "inside") == 0 ? 0 : 1);
    }
    if (waitpid(pid, &st, 0) != pid || !WIFEXITED(st) || WEXITSTATUS(st) != 0)
        probe_fail("a forked child does not see the parent's chroot");

    /* Nested: chroot("/sub") makes /sub the root. */
    put("/sub/marker", "nested");
    if (chroot("/sub") != 0)
        probe_fail("nested chroot(/sub): %s", strerror(errno));
    if (chdir("/") != 0)
        probe_fail("chdir(/) after nested chroot: %s", strerror(errno));
    expect("/marker", "nested");
    if (access("/etc/hello", F_OK) == 0)
        probe_fail("the outer root's /etc/hello is visible after a nested chroot");
    probe_pass();
}
