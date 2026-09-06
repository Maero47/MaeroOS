#pragma once
#include <stdint.h>

/* x86 Task State Segment (only the fields we actually need) */
typedef struct tss_entry {
    uint32_t prev_tss;  /* Previous TSS link — unused */
    uint32_t esp0;      /* Kernel stack pointer (loaded on ring 3 → 0 switch) */
    uint32_t ss0;       /* Kernel stack segment (0x10 = kernel data) */
    /* Fields below unused but must exist for the struct size */
    uint32_t esp1, ss1;
    uint32_t esp2, ss2;
    uint32_t cr3, eip, eflags;
    uint32_t eax, ecx, edx, ebx;
    uint32_t esp, ebp, esi, edi;
    uint32_t es, cs, ss, ds, fs, gs;
    uint32_t ldt;
    uint16_t trap;
    uint16_t iomap_base;
} __attribute__((packed)) tss_entry_t;

/*
 * tss_install — fill GDT entry `n` with a TSS descriptor for the TSS struct.
 * Called by gdt_init() before gdt_flush().
 * `gdt_entry` points to the GDT slot to fill.
 */
void tss_install(int n, void *gdt_entry);

/* tss_init — called after GDT is loaded; executes ltr to load TSS selector */
void tss_init(void);

/* tss_set_kernel_stack — must be called on every context switch */
void tss_set_kernel_stack(uint32_t stack_top);
