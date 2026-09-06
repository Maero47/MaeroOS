#include <stdint.h>
#include <io.h>

/*
 * Stack canary support for -fstack-protector-strong.
 *
 * __stack_chk_guard is read by GCC-generated canary checks.
 * In Milestone 13 we re-seed it with RDTSC ^ a constant.
 */
uintptr_t __stack_chk_guard = (uintptr_t)0xDEAD5AFEUL;

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
