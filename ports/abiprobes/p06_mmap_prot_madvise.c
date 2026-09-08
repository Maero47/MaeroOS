/*
 * P6 mmap-prot and madvise - tests M1, M6, M4.
 *
 * Linux:
 *  - mmap(PROT_NONE) and mprotect(PROT_NONE / PROT_READ) are enforced by the
 *    page tables for every mapping size: a read of a PROT_NONE page and a
 *    write to a PROT_READ page fault with SIGSEGV (mm/mprotect.c,
 *    change_protection);
 *  - madvise(MADV_DONTNEED) zaps the range, including COW pages after a
 *    fork; the next read returns zeroes (mm/madvise.c:860-871) while the
 *    other process still sees its data;
 *  - MAP_SHARED | MAP_ANONYMOUS is shared across fork (mm/mmap.c,
 *    shmem_zero_setup); MAP_FIXED_NOREPLACE over an existing mapping
 *    returns EEXIST (mm/mmap.c, since 4.17).
 *
 * MaeroOS (audit): anonymous mappings below 4 MiB are eagerly mapped
 * PRESENT|WRITABLE|USER regardless of prot (proc/syscall.c:3013-3014,
 * :3075-3088) and mprotect only toggles the write bit (:3226-3229) (M1);
 * MADV_DONTNEED skips read-only, i.e. COW, pages (:4972-4983) (M6);
 * MAP_SHARED|MAP_ANONYMOUS becomes private and MAP_FIXED_NOREPLACE is a
 * hint (:2978, M4).
 */
#define PROBE_NAME "p06_mmap_prot_madvise"
#include "probe.h"
#include <setjmp.h>
#include <sys/mman.h>
#include <sys/wait.h>

#define SZ (64 * 1024)

static sigjmp_buf jb;
static volatile sig_atomic_t faulted;

static void on_segv(int sig)
{
    (void)sig;
    faulted = 1;
    siglongjmp(jb, 1);
}

static volatile unsigned char sink;

/* Returns 1 if the access faulted. */
static int try_read(volatile unsigned char *p)
{
    faulted = 0;
    if (sigsetjmp(jb, 1) == 0) {
        sink = *p;
        return 0;
    }
    return 1;
}

static int try_write(volatile unsigned char *p)
{
    faulted = 0;
    if (sigsetjmp(jb, 1) == 0) {
        *p = 0x5a;
        return 0;
    }
    return 1;
}

int main(void)
{
    probe_watchdog(60);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_segv;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_NODEFER;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);

    /* M1 (a): PROT_NONE mapping must not be readable. */
    unsigned char *p = mmap(NULL, SZ, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        probe_fail("mmap(PROT_NONE): %s", strerror(errno));
    if (!try_read(p + 4096))
        probe_fail("read of a PROT_NONE page did not fault (read 0x%02x)", sink);
    munmap(p, SZ);
    probe_info("PROT_NONE read faults");

    /* M1 (b): RW then mprotect(PROT_READ): write must fault, read must not. */
    p = mmap(NULL, SZ, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        probe_fail("mmap(RW): %s", strerror(errno));
    memset(p, 0x11, SZ);
    if (mprotect(p, SZ, PROT_READ) != 0)
        probe_fail("mprotect(PROT_READ): %s", strerror(errno));
    if (try_read(p + 8192) || sink != 0x11)
        probe_fail("read after mprotect(PROT_READ) faulted or wrong (0x%02x)", sink);
    if (!try_write(p + 8192))
        probe_fail("write to a PROT_READ page did not fault");
    /* M1 (c): mprotect(PROT_NONE) on present pages: read must fault. */
    if (mprotect(p, SZ, PROT_NONE) != 0)
        probe_fail("mprotect(PROT_NONE): %s", strerror(errno));
    if (!try_read(p + 8192))
        probe_fail("read after mprotect(PROT_NONE) did not fault (read 0x%02x)", sink);
    if (mprotect(p, SZ, PROT_READ | PROT_WRITE) != 0)
        probe_fail("mprotect(RW): %s", strerror(errno));
    if (try_write(p + 8192) || p[8192] != 0x5a)
        probe_fail("write after restoring PROT_WRITE failed");
    munmap(p, SZ);
    probe_info("mprotect PROT_READ blocks writes, PROT_NONE blocks reads");

    /* M6: MADV_DONTNEED on a COW page after fork. */
    p = mmap(NULL, SZ, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        probe_fail("mmap(RW): %s", strerror(errno));
    memset(p, 0xe5, SZ);
    int sync[2];
    if (pipe(sync) != 0)
        probe_fail("pipe: %s", strerror(errno));
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0)
        probe_fail("fork: %s", strerror(errno));
    if (pid == 0) {
        char c;
        close(sync[1]);
        while (read(sync[0], &c, 1) < 0 && errno == EINTR)
            ;
        /* parent has done its madvise: our copy must be untouched */
        for (size_t i = 0; i < SZ; i += 4096)
            if (p[i] != 0xe5)
                _exit(5);
        _exit(0);
    }
    close(sync[0]);
    if (madvise(p, SZ, MADV_DONTNEED) != 0)
        probe_fail("madvise(MADV_DONTNEED): %s", strerror(errno));
    unsigned char seen = p[4096];
    unsigned char seen0 = p[0];
    char c = 'g';
    write(sync[1], &c, 1);
    close(sync[1]);
    int st;
    waitpid(pid, &st, 0);
    if (seen != 0 || seen0 != 0)
        probe_fail("after MADV_DONTNEED on a COW page the parent reads 0x%02x, expected 0", seen);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
        probe_fail("child's copy was affected by the parent's madvise (status 0x%x)", st);
    munmap(p, SZ);
    probe_info("MADV_DONTNEED after fork: parent reads 0, child keeps 0xe5");

    /* M4: MAP_SHARED|MAP_ANONYMOUS is shared with a forked child. */
    p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        probe_fail("mmap(MAP_SHARED|MAP_ANONYMOUS): %s", strerror(errno));
    p[0] = 0;
    fflush(stdout);
    pid = fork();
    if (pid < 0)
        probe_fail("fork: %s", strerror(errno));
    if (pid == 0) {
        p[0] = 0x42;
        _exit(0);
    }
    waitpid(pid, &st, 0);
    if (p[0] != 0x42)
        probe_fail("MAP_SHARED|MAP_ANONYMOUS: parent sees 0x%02x after the child wrote 0x42", p[0]);
    /* M4: MAP_FIXED_NOREPLACE over an existing mapping. */
    void *q = mmap(p, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (q != MAP_FAILED) {
        if (q == (void *)p)
            probe_fail("MAP_FIXED_NOREPLACE replaced an existing mapping");
        munmap(q, 4096);
        probe_fail("MAP_FIXED_NOREPLACE over a mapped page returned %p instead of EEXIST", q);
    }
    if (errno != EEXIST)
        probe_fail("MAP_FIXED_NOREPLACE over a mapped page: %s, expected EEXIST", strerror(errno));
    munmap(p, 4096);
    probe_info("shared anonymous mapping shared across fork; MAP_FIXED_NOREPLACE = EEXIST");

    probe_pass();
}
