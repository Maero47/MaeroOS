#include "gdt.h"
#include "tss.h"
#include "percpu.h"
#include <stdint.h>

/* GDT entry (segment descriptor) */
typedef struct {
    uint16_t limit_low;     /* Limit bits  0-15 */
    uint16_t base_low;      /* Base  bits  0-15 */
    uint8_t  base_middle;   /* Base  bits 16-23 */
    uint8_t  access;        /* Access byte */
    uint8_t  granularity;   /* Granularity + limit bits 16-19 */
    uint8_t  base_high;     /* Base  bits 24-31 */
} __attribute__((packed)) gdt_entry_t;

/* GDT pointer (loaded with lgdt) */
typedef struct {
    uint16_t limit;         /* sizeof(gdt) - 1 */
    uint32_t base;          /* &gdt[0] */
} __attribute__((packed)) gdt_ptr_t;

/*
 * One GDT per CPU.  The fixed segments (null, kernel/user code+data) are
 * identical across CPUs, but entry 5 (TSS) and entry 6 (user TLS / %gs) are
 * per-CPU: each CPU has its own ring0 stack (its TSS) and runs a different
 * thread (its TLS base).  Same selectors (0x08..0x33) on every CPU; they just
 * resolve through whichever GDT that CPU loaded.
 */
static gdt_entry_t gdt[MAX_CPUS][7];
static gdt_ptr_t   gdt_ptr[MAX_CPUS];

/* Defined in gdt_flush.asm */
extern void gdt_flush(gdt_ptr_t *ptr);

static void gdt_set_entry(int cpu, int i, uint32_t base, uint32_t limit,
                          uint8_t access, uint8_t gran) {
    gdt[cpu][i].base_low    = (base & 0xFFFF);
    gdt[cpu][i].base_middle = (base >> 16) & 0xFF;
    gdt[cpu][i].base_high   = (base >> 24) & 0xFF;
    gdt[cpu][i].limit_low   = (limit & 0xFFFF);
    gdt[cpu][i].granularity = ((limit >> 16) & 0x0F) | (gran & 0xF0);
    gdt[cpu][i].access      = access;
}

/* Build + load the GDT for the CPU we're running on. */
static void gdt_build_and_load(void) {
    int cpu = (int)this_cpu_id();
    /*
     * Access byte: P(1) | DPL(2) | S(1) | E(1) | DC(1) | RW(1) | A(1)
     * Granularity: G(1) | DB(1) | L(0) | AVL(0) | Limit[19:16](4)
     * 0xCF = 4 KiB granularity, 32-bit, limit top nibble = 0xF → full 4 GiB
     */
    gdt_set_entry(cpu, 0, 0, 0,          0x00, 0x00); /* Null */
    gdt_set_entry(cpu, 1, 0, 0xFFFFFFFF, 0x9A, 0xCF); /* Kernel code */
    gdt_set_entry(cpu, 2, 0, 0xFFFFFFFF, 0x92, 0xCF); /* Kernel data */
    gdt_set_entry(cpu, 3, 0, 0xFFFFFFFF, 0xFA, 0xCF); /* User code */
    gdt_set_entry(cpu, 4, 0, 0xFFFFFFFF, 0xF2, 0xCF); /* User data */
    tss_install(cpu, &gdt[cpu][5]);                    /* TSS for this CPU */
    gdt_set_entry(cpu, 6, 0, 0xFFFFFFFF, 0xF2, 0xCF);  /* User TLS (%gs) */

    gdt_ptr[cpu].limit = (uint16_t)(sizeof(gdt[cpu]) - 1);
    gdt_ptr[cpu].base  = (uint32_t)&gdt[cpu][0];
    gdt_flush(&gdt_ptr[cpu]);
}

void gdt_init(void)    { gdt_build_and_load(); }   /* BSP (this_cpu_id()==0) */
void gdt_init_ap(void) { gdt_build_and_load(); }   /* AP (apic id known)     */

/*
 * Point the user TLS segment (selector 0x33) at a new base, for the CPU we're
 * running on.  Called by the scheduler on context switch.
 */
void gdt_set_tls(uint32_t base) {
    gdt_set_entry((int)this_cpu_id(), 6, base, 0xFFFFFFFF, 0xF2, 0xCF);
}

/* DEBUG: read back the TLS base programmed into this CPU's GDT entry 6. */
uint32_t gdt_get_tls(void) {
    int cpu = (int)this_cpu_id();
    return (uint32_t)gdt[cpu][6].base_low |
           ((uint32_t)gdt[cpu][6].base_middle << 16) |
           ((uint32_t)gdt[cpu][6].base_high << 24);
}
