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

/*
 * SMEP (CPUID.(EAX=7,ECX=0):EBX bit 7; CR4 bit 20): the CPU refuses to execute
 * a user page in ring 0, so a kernel jump through a NULL or user-controlled
 * function pointer faults instead of running user code with kernel rights.
 * Set per CPU (CR4 is per-CPU): the BSP after fpu_init(), every AP in
 * ap_entry().  Returns 1 if SMEP is now on.  The kernel never executes user
 * pages (the sigreturn trampoline runs in ring 3), so nothing else changes.
 *
 * SMAP (bit 20 / CR4 bit 21) is not enabled: it needs stac/clac around every
 * user access, and the kernel still has direct user-memory accesses outside
 * copy_to_user/copy_from_user.
 */
static inline int cpu_enable_smep(void) {
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(0));
    if (eax < 7) return 0;
    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(7), "c"(0));
    (void)eax; (void)ecx; (void)edx;
    if (!(ebx & (1U << 7))) return 0;
    uint32_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= 1U << 20;
    __asm__ volatile("mov %0, %%cr4" :: "r"(cr4) : "memory");
    return 1;
}

#endif /* ARCH_I686_CPUID_H */
