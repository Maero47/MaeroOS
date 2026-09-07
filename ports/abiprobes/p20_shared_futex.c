/*
 * P20 shared futex - tests F5, F6.
 *
 * Linux: a shared (non-private) futex is keyed by (inode, page offset)
 * (kernel/futex/core.c get_futex_key), so two processes mapping the same
 * memfd agree on the key however their page tables look; the futex word is
 * read with get_user(), which faults the page in, so waiting on a page the
 * waiter never touched works (F5), and a waker whose page table has no PTE
 * for the page still finds and wakes the waiter (F6): FUTEX_WAKE returns 1.
 *
 * MaeroOS (audit): copy_from_user returns EFAULT when the page is merely not
 * present (proc/syscall.c:5206, :5225, F5), and a shared futex whose page is
 * not present in the waker falls back to the private key so the wake is
 * lost (:5332-5336, F6).
 *
 * Case A (F5): parent waits on page 3 without touching it; child writes 1
 *              there after 300 ms and wakes.
 * Case B (F6): child waits on page 2; parent, which never touched page 2,
 *              wakes it after 300 ms and must get 1 back.
 * The memfd is sized with posix_fallocate, which MaeroOS implements, rather
 * than ftruncate64 (P7).
 */
#define PROBE_NAME "p20_shared_futex"
#include "probe.h"
#include <linux/futex.h>
#include <sys/mman.h>
#include <sys/wait.h>

#define PAGES 4

static long futex(int *uaddr, int op, int val, const struct kernel_old_timespec *ts)
{
    return syscall(SYS_futex, uaddr, op, val, ts, NULL, FUTEX_BITSET_MATCH_ANY);
}

int main(void)
{
    probe_watchdog(60);
    int fd = memfd_create("p20", 0);
    if (fd < 0)
        probe_fail("memfd_create: %s", strerror(errno));
    int rc = posix_fallocate(fd, 0, PAGES * 4096);
    if (rc != 0)
        probe_fail("posix_fallocate: %s", strerror(rc));
    unsigned char *base = mmap(NULL, PAGES * 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED)
        probe_fail("mmap(memfd, MAP_SHARED): %s", strerror(errno));
    int *w3 = (int *)(base + 3 * 4096);   /* never touched by the parent before its wait */
    int *w2 = (int *)(base + 2 * 4096);   /* never touched by the parent at all */
    struct kernel_old_timespec to = { 5, 0 };

    /* Case A */
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0)
        probe_fail("fork: %s", strerror(errno));
    if (pid == 0) {
        sleep_ms(300);
        __atomic_store_n(w3, 1, __ATOMIC_RELEASE);
        long n = futex(w3, FUTEX_WAKE, 1, NULL);
        _exit(n == 1 ? 0 : (n == 0 ? 10 : 11));
    }
    double t0 = now_ms();
    errno = 0;
    long r = futex(w3, FUTEX_WAIT, 0, &to);
    int e = errno;
    double dt = now_ms() - t0;
    int st = 0;
    waitpid(pid, &st, 0);
    probe_info("case A: parent FUTEX_WAIT(shared, untouched page) r=%ld errno=%s after %.0f ms; "
               "child wake status 0x%x", r, r < 0 ? strerror(e) : "-", dt, st);
    if (r < 0 && e == EFAULT)
        probe_fail("FUTEX_WAIT on an untouched shared page returned EFAULT (F5)");
    if (r < 0 && e == ETIMEDOUT)
        probe_fail("FUTEX_WAKE from the other process was lost: waiter timed out after %.0f ms", dt);
    if (r < 0 && e != EAGAIN)
        probe_fail("FUTEX_WAIT: %s", strerror(e));
    if (__atomic_load_n(w3, __ATOMIC_ACQUIRE) != 1)
        probe_fail("shared word reads %d after the child wrote 1", *w3);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
        probe_fail("child's FUTEX_WAKE did not report exactly one woken waiter (status 0x%x)", st);
    if (dt > 2000)
        probe_fail("wake took %.0f ms", dt);

    /* Case B */
    fflush(stdout);
    pid = fork();
    if (pid < 0)
        probe_fail("fork: %s", strerror(errno));
    if (pid == 0) {
        errno = 0;
        long rr = futex(w2, FUTEX_WAIT, 0, &to);
        if (rr == 0)
            _exit(0);
        _exit(errno == ETIMEDOUT ? 20 : errno == EFAULT ? 21 : 22);
    }
    sleep_ms(300);
    errno = 0;
    long n = futex(w2, FUTEX_WAKE, 1, NULL);   /* parent never touched page 2 */
    e = errno;
    waitpid(pid, &st, 0);
    probe_info("case B: parent FUTEX_WAKE(shared, page never touched by parent) = %ld (%s); "
               "child wait status 0x%x", n, n < 0 ? strerror(e) : "-", st);
    if (n < 0)
        probe_fail("FUTEX_WAKE on an untouched shared page failed: %s", strerror(e));
    if (n != 1)
        probe_fail("FUTEX_WAKE woke %ld waiters, expected 1 (private-key fallback, F6)", n);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
        probe_fail("child waiter status 0x%x (20 = timed out, 21 = EFAULT)", st);

    munmap(base, PAGES * 4096);
    close(fd);
    probe_pass();
}
