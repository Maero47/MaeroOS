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
 * Review round 1: the root was first kept as a path and looked up again on
 * every lookup, so renaming the jail directory (or putting a symlink to /
 * where it was) moved a running jail; Linux pins the directory.  And a
 * symlink inside the jail to /proc/.. or /dev/.. (which MaeroOS passes
 * through to the global /proc and /dev) led to the real root.  Both are
 * checked below: the jail lives in a child while the parent renames it.
 *
 * Review round 2: /proc/self/fd/N handed out the descriptor's own node, so a
 * directory opened before chroot() was a way back into the old tree through
 * the passed-through /proc ("/proc/self/fd/3/etc/hello").  Linux allows that
 * (its /proc is not in the jail unless mounted there); MaeroOS refuses to
 * walk through a directory descriptor for a chrooted process.  The probe
 * checks that only on MaeroOS (where the jail's /proc is the global one).
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
#define OUTER "/tmp/p31outer"

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

    /* A jail whose path is renamed and replaced by a symlink to / after the
     * chroot: the jailed child keeps its directory. */
    mkdir(OUTER, 0755);
    char stale[64];                       /* leftovers of an earlier run */
    snprintf(stale, sizeof stale, OUTER "/stale.%d", (int)getpid());
    rename(OUTER "/jail.old", stale);
    unlink(OUTER "/jail");
    mkdir(OUTER "/jail", 0755);
    mkdir(OUTER "/jail/etc", 0755);
    mkdir(OUTER "/jail/proc", 0755);
    mkdir(OUTER "/jail/dev", 0755);
    put(OUTER "/jail/etc/hello", "jailed");
    put(OUTER "/jail.marker", "outside");
    unlink(OUTER "/jail/esc1");
    unlink(OUTER "/jail/esc2");
    if (symlink("/proc/..", OUTER "/jail/esc1") != 0 ||
        symlink("/dev/../etc", OUTER "/jail/esc2") != 0)
        probe_fail("symlink in the jail: %s", strerror(errno));
    int go[2], done[2];
    if (pipe(go) != 0 || pipe(done) != 0)
        probe_fail("pipe: %s", strerror(errno));
    pid = fork();
    if (pid == 0) {
        char c;
        char b[16] = {0};
        int fd;
        int outside = open(OUTER, O_RDONLY | O_DIRECTORY);
        if (outside < 0)
            _exit(16);
        if (chroot(OUTER "/jail") != 0 || chdir("/") != 0)
            _exit(10);
        /* Only meaningful where the jail's /proc is the real one: on Linux
         * the jail has an empty /proc directory and this path is ENOENT. */
        char via[64];
        snprintf(via, sizeof via, "/proc/self/fd/%d/jail.marker", outside);
        if (access(via, F_OK) == 0)
            _exit(17);                    /* walked through a pre-chroot fd */
        memset(b, 0, sizeof b);
        fd = open("/esc1/etc/hello", O_RDONLY);
        if (fd < 0 || read(fd, b, 6) != 6 || strcmp(b, "jailed") != 0)
            _exit(14);                    /* /proc/.. left the root */
        close(fd);
        memset(b, 0, sizeof b);
        fd = open("/esc2/hello", O_RDONLY);
        if (fd < 0 || read(fd, b, 6) != 6 || strcmp(b, "jailed") != 0)
            _exit(15);                    /* /dev/../etc left the root */
        close(fd);
        if (write(done[1], "c", 1) != 1 || read(go[0], &c, 1) != 1)
            _exit(11);
        memset(b, 0, sizeof b);
        fd = open("/etc/hello", O_RDONLY);
        if (fd < 0)
            _exit(12);                    /* lost its root after the rename */
        if (read(fd, b, 6) != 6 || strcmp(b, "jailed") != 0)
            _exit(13);                    /* followed the symlink to / */
        close(fd);
        _exit(0);
    }
    char c;
    close(done[1]);
    close(go[0]);
    if (read(done[0], &c, 1) == 1) {      /* else it exited early: see below */
        if (rename(OUTER "/jail", OUTER "/jail.old") != 0)
            probe_fail("rename the jail: %s", strerror(errno));
        if (symlink("/", OUTER "/jail") != 0)
            probe_fail("symlink %s -> /: %s", OUTER "/jail", strerror(errno));
        if (write(go[1], "g", 1) != 1)
            probe_fail("pipe write: %s", strerror(errno));
    }
    if (waitpid(pid, &st, 0) != pid || !WIFEXITED(st))
        probe_fail("jailed child status %#x", st);
    switch (WEXITSTATUS(st)) {
    case 0: break;
    case 12: probe_fail("after the jail was renamed, /etc/hello is gone inside it");
    case 16: probe_fail("open(%s): cannot open the jail's parent", OUTER);
    case 17: probe_fail("/proc/self/fd/N of a pre-chroot directory leads out of the jail");
    case 13: probe_fail("after the jail path became a symlink to /, the jail sees the real root");
    case 14: probe_fail("a symlink to /proc/.. inside the jail leads out of it");
    case 15: probe_fail("a symlink to /dev/../etc inside the jail leads out of it");
    default: probe_fail("jailed child failed (exit %d)", WEXITSTATUS(st));
    }
    unlink(OUTER "/jail");

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
