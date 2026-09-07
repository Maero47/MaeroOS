/*
 * P18 high memory - tests M10 (and M8 timing, informational).
 *
 * Linux: a process can calloc() and touch hundreds of MiB (mm/memory.c
 * demand paging from the whole physical memory), and a fork() with 100 MiB
 * of dirty anonymous memory gives both processes their own COW copies
 * (mm/memory.c, do_wp_page) that can be written and verified independently.
 *
 * MaeroOS (audit): the direct map is capped at 256 MiB and high frames are
 * reached through temporary mappings (arch/i686/mm/paging.c:172-208); the
 * audit expects this probe to PASS when QEMU boots with -m 1024M / 2048M and
 * lists it as the confirmation of the ">512 MiB" story.  COW breaks send a
 * TLB shootdown IPI per fault (M8), so the fork phase timing is printed.
 *
 * argv[1] optionally sets the calloc size in MiB (default 700); the fork
 * phase always dirties 100 MiB.
 */
#define PROBE_NAME "p18_high_memory"
#include "probe.h"
#include <sys/wait.h>

#define PAGE 4096

static void fill(unsigned char *p, size_t len, uint32_t seed)
{
    for (size_t off = 0; off < len; off += PAGE)
        *(uint32_t *)(p + off) = (uint32_t)(off / PAGE) ^ seed;
}

static long verify(const unsigned char *p, size_t len, uint32_t seed)
{
    for (size_t off = 0; off < len; off += PAGE)
        if (*(const uint32_t *)(p + off) != ((uint32_t)(off / PAGE) ^ seed))
            return (long)(off / PAGE);
    return -1;
}

int main(int argc, char **argv)
{
    size_t mib = 700;
    if (argc > 1)
        mib = (size_t)strtoul(argv[1], NULL, 10);
    size_t len = mib << 20;
    /* Touching the whole allocation is the slow part, so the watchdog scales
     * with the size: 700 MiB -> 470 s, 1400 MiB -> 820 s.  tools/smoke_abi.py
     * mirrors this expression in p18_watchdog() and sets its own timeout
     * above it; keep the two in step (the driver checks them). */
    probe_watchdog(120 + (long)mib / 2);

    double t0 = now_ms();
    unsigned char *big = calloc(1, len);
    if (!big)
        probe_fail("calloc(%zu MiB): %s", mib, strerror(errno));
    for (size_t off = 0; off < len; off += PAGE)
        if (big[off] != 0)
            probe_fail("calloc memory not zero at page %zu", off / PAGE);
    fill(big, len, 0xC0DEC0DEu);
    long bad = verify(big, len, 0xC0DEC0DEu);
    if (bad >= 0)
        probe_fail("pattern mismatch at page %ld of %zu MiB", bad, mib);
    probe_info("%zu MiB calloc'd, touched and verified in %.0f ms", mib, now_ms() - t0);
    free(big);

    size_t flen = 100u << 20;
    unsigned char *shared = malloc(flen);
    if (!shared)
        probe_fail("malloc(100 MiB): %s", strerror(errno));
    fill(shared, flen, 0x11111111u);
    fflush(stdout);
    t0 = now_ms();
    pid_t pid = fork();
    if (pid < 0)
        probe_fail("fork with 100 MiB dirty: %s", strerror(errno));
    if (pid == 0) {
        if (verify(shared, flen, 0x11111111u) >= 0)
            _exit(5);
        fill(shared, flen, 0x22222222u);
        _exit(verify(shared, flen, 0x22222222u) >= 0 ? 6 : 0);
    }
    fill(shared, flen, 0x33333333u);
    bad = verify(shared, flen, 0x33333333u);
    int st = 0;
    waitpid(pid, &st, 0);
    double dt = now_ms() - t0;
    if (bad >= 0)
        probe_fail("parent's COW copy corrupt at page %ld", bad);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
        probe_fail("child's COW copy check failed (status 0x%x: 5 = inherited data wrong, "
                   "6 = own writes wrong)", st);
    probe_info("fork with 100 MiB dirty: both sides rewrote and verified their copy in %.0f ms", dt);
    free(shared);
    probe_pass();
}
