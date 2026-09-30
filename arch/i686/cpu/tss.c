#include "tss.h"
#include "percpu.h"
#include <stdint.h>

/* One TSS per CPU — each CPU needs its own ring0 stack (esp0). */
static tss_entry_t tss[MAX_CPUS];

/*
 * Double-fault task, one per CPU.  #DF is delivered through a task gate
 * (idt.c), so the CPU saves the faulting context into that CPU's tss[] and
 * starts the handler on a stack of its own with a known-good CR3.  An ordinary
 * interrupt gate would push the #DF frame onto the very stack whose overflow
 * usually caused it, and the machine would triple-fault and reset with nothing
 * in the log.
 */
#define DF_STACK_SIZE 8192
static tss_entry_t df_tss[MAX_CPUS];
static uint8_t df_stack[MAX_CPUS][DF_STACK_SIZE] __attribute__((aligned(16)));
static uint32_t df_cr3;         /* kernel page directory (tss_set_df_cr3) */

extern void double_fault_task(void);   /* isr.asm */

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

static void tss_descriptor(void *gdt_entry_ptr, tss_entry_t *t) {
    uint32_t base  = (uint32_t)t;
    uint32_t limit = (uint32_t)sizeof(*t) - 1;

    gdt_entry_raw_t *e = (gdt_entry_raw_t *)gdt_entry_ptr;
    e->limit_low   = (uint16_t)(limit & 0xFFFF);
    e->base_low    = (uint16_t)(base  & 0xFFFF);
    e->base_middle = (uint8_t)((base >> 16) & 0xFF);
    e->access      = 0x89;  /* Present, DPL=0, 32-bit available TSS */
    e->granularity = (uint8_t)(((limit >> 16) & 0x0F) | 0x40);
    e->base_high   = (uint8_t)((base >> 24) & 0xFF);
}

/* Install a TSS descriptor for CPU `cpu` into the given GDT slot. */
void tss_install(int cpu, void *gdt_entry_ptr) {
    tss_descriptor(gdt_entry_ptr, &tss[cpu]);
}

void tss_install_df(int cpu, void *gdt_entry_ptr) {
    tss_descriptor(gdt_entry_ptr, &df_tss[cpu]);
}

/* Load THIS CPU's TSS (its GDT has the TSS at entry 5 → selector 0x28). */
void tss_init(void) {
    int c = (int)this_cpu_id();
    __builtin_memset(&tss[c], 0, sizeof(tss[c]));
    tss[c].ss0  = 0x10;    /* Kernel data segment */
    tss[c].esp0 = 0;       /* Updated by tss_set_kernel_stack before first use */
    tss[c].iomap_base = (uint16_t)sizeof(tss[c]); /* No I/O permission bitmap */
    __asm__ volatile("ltr %0" :: "r"((uint16_t)0x28));

    /* The double-fault task: never switched away from, so these are the
     * values it starts with every time. */
    tss_entry_t *d = &df_tss[c];
    __builtin_memset(d, 0, sizeof(*d));
    uint32_t cr3 = df_cr3;
    if (!cr3) __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    d->cr3    = cr3;
    d->eip    = (uint32_t)double_fault_task;
    d->eflags = 0x2;                        /* IF=0 */
    d->esp    = (uint32_t)&df_stack[c][DF_STACK_SIZE];
    d->ss0    = 0x10;
    d->esp0   = d->esp;
    d->cs     = 0x08;
    d->ss = d->ds = d->es = d->fs = d->gs = 0x10;
    d->iomap_base = (uint16_t)sizeof(*d);
}

void tss_set_df_cr3(uint32_t cr3) {
    df_cr3 = cr3;
    for (int c = 0; c < MAX_CPUS; c++)
        if (df_tss[c].eip) df_tss[c].cr3 = cr3;
}

const tss_entry_t *tss_saved_state(int cpu) {
    return &tss[cpu];
}

/* Set the ring0 stack for the CPU we're running on (every context switch). */
void tss_set_kernel_stack(uint32_t stack_top) {
    tss[this_cpu_id()].esp0 = stack_top;
}
