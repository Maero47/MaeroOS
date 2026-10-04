/*
 * P59 Record locks are bounded per user, and one user's locks do not stop
 * another's.
 *
 * As uid 65534 the probe takes up to 6000 non-adjacent read locks (they
 * cannot merge) on one file.  Linux grants them all (it bounds them only by
 * memory); MaeroOS grants 4096 per user and then answers ENOLCK.  Either way
 * uid 65533 must still be able to lock a byte afterwards, and closing the
 * file releases everything.
 *
 * MaeroOS before: one global unbounded list - the kernel heap could be
 * filled with lock records, and every close() in the system walked them all.
 */
#define PROBE_NAME "p59_lock_limits"
#include "probe.h"
#include <sys/stat.h>
#include <sys/wait.h>

static const char *path = "/tmp/p59_locks";

static int lock_byte(int fd, off_t at)
{
    struct flock fl;
    memset(&fl, 0, sizeof fl);
    fl.l_type = F_RDLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = at;
    fl.l_len = 1;
    return fcntl(fd, F_SETLK, &fl);
}

static int as_user(uid_t uid, int many)
{
    pid_t c = fork();
    if (c < 0) probe_fail("fork: %s", strerror(errno));
    if (c == 0) {
        if (geteuid() == 0 && (setgid(uid) != 0 || setuid(uid) != 0)) _exit(4);
        int fd = open(path, O_RDONLY);
        if (fd < 0) _exit(5);
        int n = 0;
        for (; n < many; n++)
            if (lock_byte(fd, 2 * (off_t)n) != 0) break;
        if (n < many && errno != ENOLCK) _exit(6);
        printf("info %s: uid %d took %d locks%s\n", PROBE_NAME, (int)uid, n,
               n < many ? " (then ENOLCK)" : "");
        fflush(stdout);
        if (many > 1) {
            sleep_ms(1500);          /* hold them while the other user tries */
        }
        close(fd);
        _exit(n >= 1 ? 0 : 8);
    }
    return c;
}

int main(void)
{
    probe_watchdog(120);
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) probe_fail("create: %s", strerror(errno));
    if (ftruncate(fd, 65536) != 0) probe_fail("ftruncate: %s", strerror(errno));
    close(fd);
    chmod(path, 0644);

    pid_t hog = as_user(65534, 6000);
    sleep_ms(500);
    /* Wait until the hog holds the lock at byte 8000. */
    for (int i = 0; i < 100; i++) {
        int t = open(path, O_RDWR);
        struct flock fl;
        memset(&fl, 0, sizeof fl);
        fl.l_type = F_WRLCK;
        fl.l_start = 2 * 4000;
        fl.l_len = 1;
        int busy = t >= 0 && fcntl(t, F_GETLK, &fl) == 0 && fl.l_type != F_UNLCK;
        if (t >= 0) close(t);
        if (busy) break;
        sleep_ms(100);
    }
    pid_t other = as_user(65533, 1);
    int st;
    if (waitpid(other, &st, 0) != other) probe_fail("waitpid: %s", strerror(errno));
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
        probe_fail("a second user could not take one lock while the first held many "
                   "(status %#x)", st);
    if (waitpid(hog, &st, 0) != hog) probe_fail("waitpid: %s", strerror(errno));
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
        probe_fail("lock-taking user failed (status %#x; 6 = error other than ENOLCK)", st);

    /* All released on close: a write lock over the whole file is free. */
    fd = open(path, O_RDWR);
    struct flock fl;
    memset(&fl, 0, sizeof fl);
    fl.l_type = F_WRLCK;
    if (fcntl(fd, F_SETLK, &fl) != 0)
        probe_fail("locks survived their owner's close: %s", strerror(errno));
    close(fd);
    unlink(path);
    probe_pass();
}
