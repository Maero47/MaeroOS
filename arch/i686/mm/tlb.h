#pragma once
#include <stdint.h>

/* Invalidate a single page in the TLB */
static inline void tlb_flush_single(uint32_t virt) {
    __asm__ volatile("invlpg (%0)" :: "r"(virt) : "memory");
}

/* Flush the entire TLB by reloading CR3 */
static inline void tlb_flush_all(void) {
    uint32_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");
}
