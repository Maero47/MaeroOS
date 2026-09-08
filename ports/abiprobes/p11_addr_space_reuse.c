/*
 * P11 address-space reuse - tests M3.
 *
 * Linux: munmap() returns the range to the free-space search, so an
 * unbounded mmap/munmap loop never runs out of virtual addresses
 * (mm/mmap.c, unmapped_area_topdown / vm_unmapped_area).
 *
 * MaeroOS (audit): mmap_next only grows and munmap never lowers it
 * (proc/syscall.c:2926-2951, :3170-3201): 8 MiB mappings fail with ENOMEM
 * after roughly 250 iterations in a 2 GiB window.
 *
 * 100 000 iterations of mmap(8 MiB)/munmap (demand VMAs on MaeroOS) and
 * 100 000 of mmap(4 KiB)+touch/munmap (eager path).
 */
#define PROBE_NAME "p11_addr_space_reuse"
#include "probe.h"
#include <sys/mman.h>

#define ITER 100000

static void loop(size_t len, int touch, const char *what)
{
    void *first = NULL;
    int reused = 0;
    double t0 = now_ms();
    for (int i = 0; i < ITER; i++) {
        void *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED)
            probe_fail("%s: mmap failed at iteration %d: %s", what, i, strerror(errno));
        if (touch)
            *(volatile char *)p = 1;
        if (i == 0)
            first = p;
        else if (p == first)
            reused++;
        if (munmap(p, len) != 0)
            probe_fail("%s: munmap failed at iteration %d: %s", what, i, strerror(errno));
    }
    probe_info("%s: %d mmap/munmap cycles in %.0f ms, first address reused %d times",
               what, ITER, now_ms() - t0, reused);
}

int main(void)
{
    probe_watchdog(120);
    loop(8u << 20, 0, "8 MiB");
    loop(4096, 1, "4 KiB touched");
    probe_pass();
}
