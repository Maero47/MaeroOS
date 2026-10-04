/*
 * P55 search (x) permission on the directories of a path.
 *
 * Linux (may_lookup): every directory a path walk looks a name up in needs
 * search permission, so a 0644 file inside another user's 0700 directory
 * cannot be opened, stat()ed, listed by name or used as a link target, and
 * chdir() into such a directory fails, all with EACCES.  A directory with
 * x but no r can be walked through but not listed.  root walks anything.
 *
 * MaeroOS: the walk checked nothing on the way, so only the last component's
 * own mode mattered: /home/alice/notes.txt (0644) was readable by everyone
 * even with /home/alice at 0700.
 *
 * Runs in the directory given as the argument (smoke-abi passes /disk, the
 * ext2 volume) and in /tmp (tmpfs).  Runs as root; the checks as another
 * user run in a child with uid/gid 65534.
 */
#define PROBE_NAME "p55_path_search"
#include "probe.h"
#include <dirent.h>
#include <grp.h>
#include <sys/stat.h>
#include <sys/wait.h>

static char priv[200], file[256], sub[256], subfile[256], xonly[200],
            xfile[256], lnk[256];

static int fail_at(int code, const char *what, int got, int want)
{
    printf("info p55_path_search: %s: %s, want %s\n", what,
           got == 0 ? "success" : strerror(got),
           want == 0 ? "success" : strerror(want));
    return code;
}

#define EXPECT(code, what, expr, want) do {                                \
        errno = 0;                                                         \
        int ok_ = (expr);                                                  \
        int got_ = ok_ ? 0 : (errno ? errno : EIO);                        \
        if (got_ != (want)) return fail_at(code, what, got_, want);        \
    } while (0)

static int as_nobody(void)
{
    gid_t none[1] = { 65534 };
    if (setgroups(1, none) || setgid(65534) || setuid(65534))
        return 99;
    struct stat st;
    char buf[8];
    EXPECT(1, "open file in a 0700 dir", open(file, O_RDONLY) >= 0, EACCES);
    EXPECT(2, "stat file in a 0700 dir", stat(file, &st) == 0, EACCES);
    EXPECT(3, "open file two levels below a 0700 dir",
           open(subfile, O_RDONLY) >= 0, EACCES);
    EXPECT(4, "chdir into a 0700 dir", chdir(priv) == 0, EACCES);
    EXPECT(5, "readlink of a symlink into a 0700 dir... (follow)",
           open(lnk, O_RDONLY) >= 0, EACCES);
    EXPECT(6, "create in a 0700 dir",
           open(subfile, O_CREAT | O_WRONLY, 0644) >= 0, EACCES);
    EXPECT(7, "readlink in a 0700 dir",
           readlink(file, buf, sizeof buf) >= 0, EACCES);
    /* x without r: walk through, not list */
    EXPECT(8, "open file in a 0711 dir", open(xfile, O_RDONLY) >= 0, 0);
    EXPECT(9, "chdir into a 0711 dir", chdir(xonly) == 0, 0);
    EXPECT(10, "relative open after chdir to a 0711 dir",
           open("f", O_RDONLY) >= 0, 0);
    EXPECT(11, "opendir of a 0711 dir", opendir(xonly) != NULL, EACCES);
    /* a 0700 directory reached through its parent's open fd (openat) */
    int pfd = open(priv, O_RDONLY | O_DIRECTORY);
    if (pfd >= 0) return fail_at(12, "open 0700 dir itself", 0, EACCES);
    return 0;
}

static void setup(const char *dir)
{
    int pid = (int)getpid();
    snprintf(priv, sizeof priv, "%s/p55p.%d", dir, pid);
    snprintf(file, sizeof file, "%s/notes.txt", priv);
    snprintf(sub, sizeof sub, "%s/sub", priv);
    snprintf(subfile, sizeof subfile, "%s/sub/f", priv);
    snprintf(xonly, 200, "%s/p55x.%d", dir, pid);
    snprintf(xfile, sizeof xfile, "%s/f", xonly);
    snprintf(lnk, sizeof lnk, "%s/p55l.%d", dir, pid);

    if (mkdir(priv, 0700) || mkdir(sub, 0755) || mkdir(xonly, 0711))
        probe_fail("mkdir in %s: %s", dir, strerror(errno));
    const char *paths[] = { file, subfile, xfile };
    for (int i = 0; i < 3; i++) {
        int fd = open(paths[i], O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd < 0 || write(fd, "secret", 6) != 6)
            probe_fail("create %s: %s", paths[i], strerror(errno));
        close(fd);
        chmod(paths[i], 0644);
    }
    chmod(priv, 0700);
    chmod(xonly, 0711);
    if (symlink(file, lnk) != 0)
        probe_fail("symlink %s: %s", lnk, strerror(errno));
}

static void cleanup(void)
{
    unlink(lnk);
    unlink(xfile);
    rmdir(xonly);
    unlink(subfile);
    rmdir(sub);
    unlink(file);
    rmdir(priv);
}

static void run_in(const char *dir)
{
    setup(dir);
    pid_t pid = fork();
    if (pid == 0) {
        fflush(stdout);
        _exit(as_nobody());
    }
    int ws = 0;
    if (waitpid(pid, &ws, 0) != pid) probe_fail("waitpid: %s", strerror(errno));
    int code = WIFEXITED(ws) ? WEXITSTATUS(ws) : -1;
    /* root walks anything */
    int fd = open(subfile, O_RDONLY);
    if (fd < 0) probe_fail("root: open %s: %s", subfile, strerror(errno));
    close(fd);
    cleanup();
    if (code != 0)
        probe_fail("in %s: check %d as nobody failed (see the info line)", dir,
                   code);
}

int main(int argc, char **argv)
{
    probe_watchdog(60);
    if (geteuid() != 0) probe_skip("needs root");
    run_in("/tmp");
    for (int i = 1; i < argc; i++) run_in(argv[i]);
    probe_pass();
}
