/*
 * P32 utimensat(2), futimens, utimes, utime: setting file timestamps.
 *
 * Linux: utimensat(dirfd, path, times, flags) sets atime and mtime; a NULL
 * times means "now", UTIME_NOW / UTIME_OMIT in tv_nsec mean "now" and "leave
 * alone", a NULL path acts on dirfd (futimens), and a nanosecond field out of
 * range is EINVAL.  utimes(path, timeval[2]) and utime(path, utimbuf) set the
 * same two times.  A newly created file's mtime is the current wall-clock
 * time.
 *
 * MaeroOS: syscalls 320 (utimensat) and 299 (futimesat, musl's fallback)
 * were missing, so apk failed every file it installed with "Failed to
 * preserve modification time ... Function not implemented"; utime (30) and
 * utimes (271) were missing too.  ext2 stamped new files with the seconds
 * since boot (1970) instead of the wall-clock time.
 *
 * Runs on /tmp and, when given a directory argument, there too (smoke-abi
 * passes /disk so the ext2 path, which writes the inode, is covered).
 */
#define PROBE_NAME "p32_utimensat"
#include "probe.h"
#include <sys/stat.h>
#include <sys/time.h>
#include <utime.h>

static void check_times(const char *path, time_t at, time_t mt, const char *what)
{
    struct stat st;
    if (stat(path, &st) != 0)
        probe_fail("%s: stat %s: %s", what, path, strerror(errno));
    if (st.st_atime != at || st.st_mtime != mt)
        probe_fail("%s: %s atime %lld mtime %lld, want %lld %lld", what, path,
                   (long long)st.st_atime, (long long)st.st_mtime,
                   (long long)at, (long long)mt);
}

static void near_now(const char *path, int which_m, const char *what)
{
    struct stat st;
    if (stat(path, &st) != 0)
        probe_fail("%s: stat %s: %s", what, path, strerror(errno));
    time_t t = which_m ? st.st_mtime : st.st_atime, now = time(NULL);
    if (t < now - 5 || t > now + 5)
        probe_fail("%s: %s time %lld is not now (%lld)", what, path,
                   (long long)t, (long long)now);
}

static void run_in(const char *dir)
{
    char path[256];
    snprintf(path, sizeof path, "%s/p32.%d", dir, (int)getpid());
    unlink(path);
    int fd = open(path, O_CREAT | O_RDWR, 0644);
    if (fd < 0)
        probe_fail("create %s: %s", path, strerror(errno));
    near_now(path, 1, "new file");

    struct timespec ts[2] = { { 1000000000, 0 }, { 1100000000, 0 } };
    if (utimensat(AT_FDCWD, path, ts, 0) != 0)
        probe_fail("utimensat(%s): %s", path, strerror(errno));
    check_times(path, 1000000000, 1100000000, "utimensat");

    ts[0].tv_nsec = UTIME_OMIT;
    ts[1].tv_sec = 1200000000;
    if (utimensat(AT_FDCWD, path, ts, 0) != 0)
        probe_fail("utimensat(UTIME_OMIT): %s", strerror(errno));
    check_times(path, 1000000000, 1200000000, "UTIME_OMIT atime");

    struct timespec fts[2] = { { 1300000000, 0 }, { 1400000000, 0 } };
    if (futimens(fd, fts) != 0)
        probe_fail("futimens: %s", strerror(errno));
    check_times(path, 1300000000, 1400000000, "futimens");

    struct timeval tv[2] = { { 1500000000, 0 }, { 1600000000, 0 } };
    if (utimes(path, tv) != 0)
        probe_fail("utimes: %s", strerror(errno));
    check_times(path, 1500000000, 1600000000, "utimes");

    struct utimbuf ub = { 1650000000, 1700000000 };
    if (utime(path, &ub) != 0)
        probe_fail("utime: %s", strerror(errno));
    check_times(path, 1650000000, 1700000000, "utime");

    if (utimensat(AT_FDCWD, path, NULL, 0) != 0)
        probe_fail("utimensat(NULL times): %s", strerror(errno));
    near_now(path, 1, "times = NULL (mtime)");
    near_now(path, 0, "times = NULL (atime)");

    struct timespec bad[2] = { { 0, 1000000000 }, { 0, 0 } };
    if (utimensat(AT_FDCWD, path, bad, 0) != -1 || errno != EINVAL)
        probe_fail("tv_nsec 1e9 accepted (%s), want EINVAL", strerror(errno));
    snprintf(path + strlen(path), 8, ".no");
    if (utimensat(AT_FDCWD, path, ts, 0) != -1 || errno != ENOENT)
        probe_fail("utimensat on a missing file: %s, want ENOENT", strerror(errno));
    path[strlen(path) - 3] = '\0';
    close(fd);
    unlink(path);
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
