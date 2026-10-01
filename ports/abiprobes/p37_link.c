/*
 * P37 link(2) / linkat(2): hard links on a disk filesystem.
 *
 * Linux: link(old, new) gives the inode a second name.  Both names show the
 * same st_ino and st_nlink 2, writes through one are read through the
 * other, and unlinking one leaves the other with st_nlink 1.  linkat links
 * a symlink itself unless AT_SYMLINK_FOLLOW is given.  An existing new name
 * is EEXIST, a directory EPERM, and a missing old path ENOENT.
 *
 * MaeroOS: link (9) answered -EPERM and linkat (303) was missing; every stat
 * reported st_nlink 1.  apk-tools 3 links files it installs, and packages
 * that ship hard links (git's libexec) could not be installed in the guest.
 *
 * Runs in each directory given as an argument (smoke-abi passes /disk, the
 * ext2 volume), or in /tmp without one.  MaeroOS's tmpfs has no hard links
 * (EPERM, as a Linux filesystem without them answers), so it is not used
 * there.
 */
#define PROBE_NAME "p37_link"
#include "probe.h"
#include <sys/stat.h>

static void run_in(const char *dir)
{
    char a[256], b[256], l[256], lb[256], d[256], db[256];
    int pid = (int)getpid();
    snprintf(a, sizeof a, "%s/p37a.%d", dir, pid);
    snprintf(b, sizeof b, "%s/p37b.%d", dir, pid);
    snprintf(l, sizeof l, "%s/p37l.%d", dir, pid);
    snprintf(lb, sizeof lb, "%s/p37lb.%d", dir, pid);
    snprintf(d, sizeof d, "%s/p37d.%d", dir, pid);
    snprintf(db, sizeof db, "%s/p37db.%d", dir, pid);

    int fd = open(a, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0 || write(fd, "one", 3) != 3)
        probe_fail("create %s: %s", a, strerror(errno));
    close(fd);
    if (link(a, b) != 0)
        probe_fail("link(%s, %s): %s", a, b, strerror(errno));
    struct stat sa, sb;
    if (stat(a, &sa) != 0 || stat(b, &sb) != 0)
        probe_fail("stat after link: %s", strerror(errno));
    if (sa.st_ino != sb.st_ino || sa.st_nlink != 2 || sb.st_nlink != 2)
        probe_fail("after link: ino %lu/%lu nlink %lu/%lu, want one inode, nlink 2",
                   (unsigned long)sa.st_ino, (unsigned long)sb.st_ino,
                   (unsigned long)sa.st_nlink, (unsigned long)sb.st_nlink);
    fd = open(b, O_WRONLY | O_APPEND);
    if (fd < 0 || write(fd, "two", 3) != 3)
        probe_fail("append through %s: %s", b, strerror(errno));
    close(fd);
    char buf[16] = {0};
    fd = open(a, O_RDONLY);
    if (fd < 0 || read(fd, buf, sizeof buf - 1) != 6 || strcmp(buf, "onetwo") != 0)
        probe_fail("%s reads \"%s\" after a write through %s", a, buf, b);
    close(fd);

    if (link(a, b) != -1 || errno != EEXIST)
        probe_fail("link onto an existing name: %s, want EEXIST", strerror(errno));
    if (mkdir(d, 0755) != 0)
        probe_fail("mkdir %s: %s", d, strerror(errno));
    if (link(d, db) != -1 || errno != EPERM)
        probe_fail("link of a directory: %s, want EPERM", strerror(errno));
    rmdir(d);
    if (link(l, lb) != -1 || errno != ENOENT)
        probe_fail("link of a missing path: %s, want ENOENT", strerror(errno));

    /* linkat: the symlink itself by default, its target with FOLLOW. */
    if (symlink(a, l) != 0)
        probe_fail("symlink %s: %s", l, strerror(errno));
    if (linkat(AT_FDCWD, l, AT_FDCWD, lb, 0) != 0)
        probe_fail("linkat(symlink, 0): %s", strerror(errno));
    if (lstat(lb, &sb) != 0 || !S_ISLNK(sb.st_mode))
        probe_fail("linkat without AT_SYMLINK_FOLLOW did not link the symlink");
    unlink(lb);
    if (linkat(AT_FDCWD, l, AT_FDCWD, lb, AT_SYMLINK_FOLLOW) != 0)
        probe_fail("linkat(symlink, FOLLOW): %s", strerror(errno));
    if (lstat(lb, &sb) != 0 || !S_ISREG(sb.st_mode) || sb.st_ino != sa.st_ino ||
        sb.st_nlink != 3)
        probe_fail("linkat FOLLOW: mode %o ino %lu nlink %lu, want the file, nlink 3",
                   (unsigned)sb.st_mode, (unsigned long)sb.st_ino,
                   (unsigned long)sb.st_nlink);
    unlink(lb);
    unlink(l);

    if (unlink(a) != 0)
        probe_fail("unlink %s: %s", a, strerror(errno));
    if (stat(b, &sb) != 0 || sb.st_nlink != 1 || sb.st_size != 6)
        probe_fail("after unlinking %s: %s nlink %lu size %lld, want 1 and 6", a, b,
                   (unsigned long)sb.st_nlink, (long long)sb.st_size);
    unlink(b);
}

int main(int argc, char **argv)
{
    probe_watchdog(60);
    if (argc < 2)
        run_in("/tmp");
    for (int i = 1; i < argc; i++) {
        if (access(argv[i], W_OK) == 0)
            run_in(argv[i]);
        else
            probe_skip("%s is not writable", argv[i]);
    }
    probe_pass();
}
