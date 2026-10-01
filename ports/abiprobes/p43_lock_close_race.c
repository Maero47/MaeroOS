/*
 * P43 a descriptor closed by another thread while F_SETLKW / flock waits.
 *
 * Linux: the waiter keeps waiting until the holder lets go; then
 * fcntl(F_SETLKW) notices the descriptor is gone, drops the lock it just got
 * and fails with EBADF, and a blocking flock's lock dies with the last
 * reference to the open file.  Either way no lock is left behind: another
 * process can lock the file afterwards.
 *
 * MaeroOS: the waiter inserted its lock after the wait without looking at
 * the descriptor again, so a lock owned by a closed descriptor stayed until
 * reboot.
 */
#define PROBE_NAME "p43_lock_close_race"
#include "probe.h"
#include <sys/file.h>
#include <sys/wait.h>

static const char *path;
static int wfd;
static int use_flock;
static int wres, werr;

static void *waiter(void *arg)
{
    (void)arg;
    if (use_flock) {
        wres = flock(wfd, LOCK_EX);
    } else {
        struct flock fl = { .l_type = F_WRLCK, .l_whence = SEEK_SET };
        wres = fcntl(wfd, F_SETLKW, &fl);
    }
    werr = errno;
    return NULL;
}

/* A child that holds the lock for `ms`, then exits. */
static pid_t holder(int ms)
{
    int p[2];
    if (pipe(p) != 0) probe_fail("pipe: %s", strerror(errno));
    pid_t pid = fork();
    if (pid == 0) {
        int fd = open(path, O_RDWR);
        struct flock fl = { .l_type = F_WRLCK, .l_whence = SEEK_SET };
        if (fd < 0 || (use_flock ? flock(fd, LOCK_EX) : fcntl(fd, F_SETLK, &fl)) != 0)
            _exit(2);
        if (write(p[1], "r", 1) != 1) _exit(2);
        usleep(ms * 1000);
        _exit(0);
    }
    char c;
    if (read(p[0], &c, 1) != 1) probe_fail("lock holder did not start");
    close(p[0]);
    close(p[1]);
    return pid;
}

/* Can a fresh process take the lock right away? */
static int lockable(void)
{
    pid_t pid = fork();
    if (pid == 0) {
        int fd = open(path, O_RDWR);
        struct flock fl = { .l_type = F_WRLCK, .l_whence = SEEK_SET };
        _exit(fd >= 0 && (use_flock ? flock(fd, LOCK_EX | LOCK_NB)
                                    : fcntl(fd, F_SETLK, &fl)) == 0 ? 0 : 1);
    }
    int st;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

static void round_(int flk)
{
    use_flock = flk;
    const char *what = flk ? "flock(LOCK_EX)" : "fcntl(F_SETLKW)";
    wfd = open(path, O_RDWR);
    if (wfd < 0) probe_fail("open: %s", strerror(errno));
    pid_t h = holder(600);
    pthread_t t;
    pthread_create(&t, NULL, waiter, NULL);
    usleep(200 * 1000);
    close(wfd);                         /* while the waiter sleeps */
    pthread_join(t, NULL);
    waitpid(h, NULL, 0);
    if (!flk && (wres == 0 || werr != EBADF))
        probe_fail("%s on a descriptor closed during the wait: want EBADF, got %s",
                   what, wres == 0 ? "success" : strerror(werr));
    if (flk && wres != 0 && werr != EBADF)
        probe_fail("%s: unexpected %s", what, strerror(werr));
    probe_info("%s after close: %s", what, wres == 0 ? "0" : strerror(werr));
    if (!lockable())
        probe_fail("%s left a lock behind for a closed descriptor", what);
}

int main(void)
{
    probe_watchdog(60);
    static char buf[64];
    snprintf(buf, sizeof buf, "/tmp/p43_lock.%d", (int)getpid());
    path = buf;
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) probe_fail("create: %s", strerror(errno));
    close(fd);
    round_(0);
    round_(1);
    unlink(path);
    probe_pass();
}
