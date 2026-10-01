/*
 * P40 advisory locks: flock(2) and fcntl F_SETLK/F_SETLKW/F_GETLK exclude.
 *
 * Linux: flock locks belong to the open file description (dup and fork
 * share them, the last close drops them), LOCK_NB answers EWOULDBLOCK on a
 * conflict and a blocking flock waits for the holder.  fcntl record locks
 * belong to the process: F_SETLK on a conflicting range fails with
 * EAGAIN/EACCES, F_GETLK names the holder, F_SETLKW waits, and closing ANY
 * descriptor of the file drops the process's locks on it.  The two kinds do
 * not see each other.
 *
 * MaeroOS: flock (143) and the fcntl lock commands succeeded without
 * excluding anyone, so two apk instances were not kept apart.
 */
#define PROBE_NAME "p40_flock"
#include "probe.h"
#include <sys/file.h>
#include <sys/wait.h>

static long ms_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static void child_result(int fd, char c)
{
    if (write(fd, &c, 1) != 1) _exit(2);
    _exit(0);
}

static char wait_child(int rfd, pid_t pid)
{
    char c = '?';
    if (read(rfd, &c, 1) != 1) c = '?';
    waitpid(pid, NULL, 0);
    return c;
}

static int setlk(int fd, int cmd, short type, off_t start, off_t len)
{
    struct flock fl = { .l_type = type, .l_whence = SEEK_SET,
                        .l_start = start, .l_len = len };
    return fcntl(fd, cmd, &fl);
}

int main(int argc, char **argv)
{
    probe_watchdog(60);
    char path[256];
    snprintf(path, sizeof path, "%s/p40_lock.%d",
             argc > 1 ? argv[1] : "/tmp", (int)getpid());
    int a = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    int b = open(path, O_RDWR);
    if (a < 0 || b < 0) probe_fail("open %s: %s", path, strerror(errno));

    /* flock: one open file description excludes another. */
    if (flock(a, LOCK_EX) != 0) probe_fail("flock(a, LOCK_EX): %s", strerror(errno));
    if (flock(b, LOCK_EX | LOCK_NB) == 0 || errno != EWOULDBLOCK)
        probe_fail("flock(b, EX|NB) under a's lock: want EWOULDBLOCK, got %s",
                   strerror(errno));
    if (flock(b, LOCK_SH | LOCK_NB) == 0 || errno != EWOULDBLOCK)
        probe_fail("flock(b, SH|NB) under a's EX lock: want EWOULDBLOCK");

    /* A dup shares the lock: closing the original keeps it. */
    int a2 = dup(a);
    close(a);
    if (flock(b, LOCK_EX | LOCK_NB) == 0)
        probe_fail("the lock went away with a dup still open");
    close(a2);
    if (flock(b, LOCK_EX | LOCK_NB) != 0)
        probe_fail("the last close did not drop the flock lock: %s", strerror(errno));
    if (flock(b, LOCK_UN) != 0) probe_fail("LOCK_UN: %s", strerror(errno));

    /* Shared locks coexist. */
    a = open(path, O_RDWR);
    if (flock(a, LOCK_SH) != 0 || flock(b, LOCK_SH | LOCK_NB) != 0)
        probe_fail("two LOCK_SH locks must coexist: %s", strerror(errno));
    flock(a, LOCK_UN);
    flock(b, LOCK_UN);

    /* A blocking flock waits until the holder unlocks. */
    if (flock(a, LOCK_EX) != 0) probe_fail("flock(a): %s", strerror(errno));
    int p[2];
    if (pipe(p) != 0) probe_fail("pipe: %s", strerror(errno));
    pid_t pid = fork();
    if (pid == 0) {
        int c = open(path, O_RDWR);
        long t0 = ms_now();
        if (flock(c, LOCK_EX) != 0) child_result(p[1], 'e');
        child_result(p[1], ms_now() - t0 >= 150 ? 'w' : 'n');
    }
    usleep(300 * 1000);
    flock(a, LOCK_UN);
    char r = wait_child(p[0], pid);
    if (r != 'w')
        probe_fail("blocking flock: %s", r == 'n' ? "did not wait for the holder"
                                                  : "failed");

    /* fcntl record locks: the child is another owner. */
    if (setlk(a, F_SETLK, F_WRLCK, 0, 10) != 0)
        probe_fail("F_SETLK [0,10): %s", strerror(errno));
    pid = fork();
    if (pid == 0) {
        int c = open(path, O_RDWR);
        if (setlk(c, F_SETLK, F_WRLCK, 5, 10) == 0 ||
            (errno != EAGAIN && errno != EACCES))
            child_result(p[1], '1');
        struct flock fl = { .l_type = F_WRLCK, .l_whence = SEEK_SET,
                            .l_start = 0, .l_len = 100 };
        if (fcntl(c, F_GETLK, &fl) != 0 || fl.l_type != F_WRLCK ||
            fl.l_start != 0 || fl.l_len != 10 || fl.l_pid != getppid())
            child_result(p[1], '2');
        if (setlk(c, F_SETLK, F_WRLCK, 10, 10) != 0)
            child_result(p[1], '3');
        /* flock does not see record locks. */
        if (flock(c, LOCK_EX | LOCK_NB) != 0)
            child_result(p[1], '4');
        long t0 = ms_now();
        if (setlk(c, F_SETLKW, F_RDLCK, 0, 5) != 0)
            child_result(p[1], '5');
        child_result(p[1], ms_now() - t0 >= 150 ? 'w' : 'n');
    }
    usleep(300 * 1000);
    /* Closing any descriptor of the file drops this process's locks. */
    int other = open(path, O_RDONLY);
    close(other);
    r = wait_child(p[0], pid);
    switch (r) {
    case 'w': break;
    case '1': probe_fail("F_SETLK over a held range did not fail with EAGAIN/EACCES");
    case '2': probe_fail("F_GETLK did not report the holder's [0,10) write lock and pid");
    case '3': probe_fail("F_SETLK on a free range failed");
    case '4': probe_fail("flock was blocked by an fcntl record lock");
    case '5': probe_fail("F_SETLKW failed");
    case 'n': probe_fail("F_SETLKW did not wait (or close() did not release the lock)");
    default:  probe_fail("child reported '%c'", r);
    }
    struct flock fl = { .l_type = F_WRLCK, .l_whence = SEEK_SET };
    if (fcntl(a, F_GETLK, &fl) != 0 || fl.l_type != F_UNLCK)
        probe_fail("F_GETLK with no other holder: want F_UNLCK");
    close(a);
    close(b);
    unlink(path);
    probe_pass();
}
