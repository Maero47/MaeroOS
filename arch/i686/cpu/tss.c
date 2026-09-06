#include "tss.h"
#include "percpu.h"
#include <stdint.h>

/* One TSS per CPU — each CPU needs its own ring0 stack (esp0). */
static tss_entry_t tss[MAX_CPUS];

/*
 * GDT entry encoding for a TSS descriptor.
 * Access byte 0x89 = Present | DPL=0 | Type=9 (32-bit available TSS).
 * Granularity 0x40 = byte granularity, 32-bit.
 */
typedef struct {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __attribute__((packed)) gdt_entry_raw_t;

/* Install a TSS descriptor for CPU `cpu` into the given GDT slot. */
void tss_install(int cpu, void *gdt_entry_ptr) {
    uint32_t base  = (uint32_t)&tss[cpu];
    uint32_t limit = (uint32_t)sizeof(tss[cpu]) - 1;

    gdt_entry_raw_t *e = (gdt_entry_raw_t *)gdt_entry_ptr;
    e->limit_low   = (uint16_t)(limit & 0xFFFF);
    e->base_low    = (uint16_t)(base  & 0xFFFF);
    e->base_middle = (uint8_t)((base >> 16) & 0xFF);
    e->access      = 0x89;  /* Present, DPL=0, 32-bit available TSS */
    e->granularity = (uint8_t)(((limit >> 16) & 0x0F) | 0x40);
    e->base_high   = (uint8_t)((base >> 24) & 0xFF);
}

/* Load THIS CPU's TSS (its GDT has the TSS at entry 5 → selector 0x28). */
void tss_init(void) {
    int c = (int)this_cpu_id();
    __builtin_memset(&tss[c], 0, sizeof(tss[c]));
    tss[c].ss0  = 0x10;    /* Kernel data segment */
    tss[c].esp0 = 0;       /* Updated by tss_set_kernel_stack before first use */
    tss[c].iomap_base = (uint16_t)sizeof(tss[c]); /* No I/O permission bitmap */
    __asm__ volatile("ltr %0" :: "r"((uint16_t)0x28));
}

/* Set the ring0 stack for the CPU we're running on (every context switch). */
void tss_set_kernel_stack(uint32_t stack_top) {
    tss[this_cpu_id()].esp0 = stack_top;
}
