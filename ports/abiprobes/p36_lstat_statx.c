/*
 * P36 lstat() and fstatat(dirfd, ...): the link itself, relative to dirfd.
 *
 * Linux: lstat() on a symlink reports S_IFLNK and the length of the target;
 * fstatat(dirfd, "name", AT_SYMLINK_NOFOLLOW) does the same for a name
 * relative to dirfd, whatever the cwd is.
 *
 * MaeroOS: musl on i386 implements stat, lstat and fstatat with statx (383),
 * and sys_statx ignored both its dirfd and AT_SYMLINK_NOFOLLOW: every lstat
 * followed the link (GNU ls -l showed Alpine's busybox applet symlinks as
 * copies of busybox, `test -L` was false) and a relative fstatat looked the
 * name up in the cwd.
 *
 * Runs on /tmp and on each directory given as an argument (smoke-abi passes
 * /disk, so ext2 symlinks are covered too).
 */
#define PROBE_NAME "p36_lstat_statx"
#include "probe.h"
#include <sys/stat.h>

static void run_in(const char *dir)
{
    char sub[256], file[300], link[300];
    snprintf(sub, sizeof sub, "%s/p36.%d", dir, (int)getpid());
    snprintf(file, sizeof file, "%s/target", sub);
    snprintf(link, sizeof link, "%s/link", sub);
    if (mkdir(sub, 0755) != 0)
        probe_fail("mkdir %s: %s", sub, strerror(errno));
    int fd = open(file, O_CREAT | O_WRONLY, 0644);
    if (fd < 0 || write(fd, "12345", 5) != 5)
        probe_fail("create %s: %s", file, strerror(errno));
    close(fd);
    if (symlink("target", link) != 0)
        probe_fail("symlink %s: %s", link, strerror(errno));

    struct stat st;
    if (lstat(link, &st) != 0)
        probe_fail("lstat %s: %s", link, strerror(errno));
    if (!S_ISLNK(st.st_mode) || st.st_size != 6)
        probe_fail("lstat %s: mode %o size %lld, want a symlink of size 6",
                   link, (unsigned)st.st_mode, (long long)st.st_size);
    if (stat(link, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size != 5)
        probe_fail("stat %s does not follow to the 5-byte file", link);

    /* Relative to a directory fd while the cwd is elsewhere. */
    int dfd = open(sub, O_RDONLY | O_DIRECTORY);
    if (dfd < 0)
        probe_fail("open %s: %s", sub, strerror(errno));
    if (chdir("/") != 0)
        probe_fail("chdir /: %s", strerror(errno));
    if (fstatat(dfd, "link", &st, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISLNK(st.st_mode))
        probe_fail("fstatat(dirfd, link, NOFOLLOW): %s, mode %o",
                   strerror(errno), (unsigned)st.st_mode);
    if (fstatat(dfd, "link", &st, 0) != 0 || !S_ISREG(st.st_mode) || st.st_size != 5)
        probe_fail("fstatat(dirfd, link, 0) does not reach the target");
    if (fstatat(dfd, "", &st, 0) != -1 || errno != ENOENT)
        probe_fail("fstatat(dirfd, \"\", 0): %s, want ENOENT", strerror(errno));
    if (fstatat(dfd, "", &st, AT_EMPTY_PATH) != 0 || !S_ISDIR(st.st_mode))
        probe_fail("fstatat(dirfd, \"\", AT_EMPTY_PATH) is not the directory");
    close(dfd);
    unlink(link);
    unlink(file);
    rmdir(sub);
}

int main(int argc, char **argv)
{
    probe_watchdog(60);
    run_in("/tmp");
    for (int i = 1; i < argc; i++) {
        if (access(argv[i], W_OK) == 0)
            run_in(argv[i]);
        else
            probe_info("%s is not writable, skipped", argv[i]);
    }
    probe_pass();
}
