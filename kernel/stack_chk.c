#include <stdint.h>
#include <io.h>

/*
 * Stack canary support for -fstack-protector-strong.
 *
 * __stack_chk_guard is read by GCC-generated canary checks.  It starts as a
 * constant and stack_chk_seed() replaces it at boot.
 */
uintptr_t __stack_chk_guard = (uintptr_t)0xDEAD5AFEUL;

/*
 * Replace the boot-time constant with a per-boot value: RDRAND when the CPU
 * has it, mixed with the TSC.  Any protected frame live across the change
 * would fail its check on return, so this must be the first call in
 * kernel_main (which never returns) and must itself carry no canary.  The low
 * byte is kept zero, as glibc does, so a string overflow that reads the
 * canary stops at it and one that writes it cannot reproduce it.
 */
__attribute__((no_stack_protector))
void stack_chk_seed(void) {
    uint32_t lo, hi, a, b, c, d;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    uint32_t v = lo ^ (hi * 0x9E3779B9U) ^ (uint32_t)__stack_chk_guard;

    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    if (c & (1U << 30)) {                         /* CPUID.1:ECX.RDRAND */
        for (int i = 0; i < 10; i++) {
            uint32_t r;
            uint8_t ok;
            __asm__ volatile("rdrand %0; setc %1" : "=r"(r), "=qm"(ok) :: "cc");
            if (ok) { v ^= r; break; }
        }
    }
    /* murmur3 finaliser: spread the TSC's low-entropy high bits around. */
    v ^= v >> 16; v *= 0x85EBCA6BU; v ^= v >> 13; v *= 0xC2B2AE35U; v ^= v >> 16;
    v &= ~0xFFU;
    if (!v) v = 0xDEAD5A00U;
    __stack_chk_guard = (uintptr_t)v;
}

__attribute__((noreturn))
void __stack_chk_fail(void) {
    __asm__ volatile("cli");
    /* Write to COM1 directly (serial.c may not be available yet) */
    static const char msg[] = "\r\n*** KERNEL PANIC: stack smashing detected ***\r\n";
    for (const char *p = msg; *p; p++) {
        while (!(inb(0x3FD) & 0x20))   /* Wait for THRE (LSR bit 5) */
            ;
        outb(0x3F8, (uint8_t)*p);
    }
    for (;;) __asm__ volatile("hlt");
}
