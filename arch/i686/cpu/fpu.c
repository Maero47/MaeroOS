#include "fpu.h"
#include "../../../kernel/printk.h"
#include <stdint.h>

/*
 * FPU/SSE context switching support.
 *
 * Real (Linux-built) binaries use x87/SSE freely — musl memcpy is SSE on
 * i686 — so the scheduler must preserve that state per task.  We use eager
 * fxsave/fxrstor around every switch: simple, correct, ~100 cycles each.
 * The 512-byte areas are 16-byte aligned (fxsave faults otherwise).
 */

/* A clean post-finit state captured at boot; new tasks start from this. */
static uint8_t fpu_initial[512] __attribute__((aligned(16)));

void fpu_init(void) {
    uint32_t cr0, cr4;

    /* CR0: clear EM (no emulation) + TS (no trap), set MP + NE. */
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~0x0CU;            /* EM | TS off */
    cr0 |= 0x22U;             /* NE | MP on */
    __asm__ volatile("mov %0, %%cr0" :: "r"(cr0));

    /* CR4: OSFXSR (fxsave/fxrstor + SSE) and OSXMMEXCPT. */
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= 0x600U;
    __asm__ volatile("mov %0, %%cr4" :: "r"(cr4));

    __asm__ volatile("fninit");
    __asm__ volatile("fxsave %0" : "=m"(fpu_initial));
    printk("[FPU]  x87/SSE context switching enabled (eager fxsave).\n");
}

void fpu_state_init(uint8_t *area) {
    for (int i = 0; i < 512; i++)
        area[i] = fpu_initial[i];
}

void fpu_save(uint8_t *area) {
    __asm__ volatile("fxsave (%0)" :: "r"(area) : "memory");
}

void fpu_restore(const uint8_t *area) {
    __asm__ volatile("fxrstor (%0)" :: "r"(area));
}
