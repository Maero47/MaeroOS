#ifndef ARCH_I686_CPUID_H
#define ARCH_I686_CPUID_H

#include <stdint.h>

/*
 * CPUID leaf 1, EDX feature flags.  On i386 the Linux ELF auxv AT_HWCAP value
 * IS exactly this register (bit 0 = FPU, 4 = TSC, 8 = CX8, 15 = CMOV,
 * 23 = MMX, 24 = FXSR, 25 = SSE, 26 = SSE2, …).  glibc's ifunc resolvers read
 * AT_HWCAP to pick optimized memcpy/strlen variants; without it _dl_hwcap is 0
 * and glibc falls back to the slow generic path.
 */
static inline uint32_t cpuid_hwcap(void) {
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(1));
    (void)eax; (void)ebx; (void)ecx;
    return edx;
}

#endif /* ARCH_I686_CPUID_H */
