#include "idt.h"
#include "isr.h"
#include <stdint.h>

/* IDT gate descriptor (8 bytes) */
typedef struct {
    uint16_t offset_low;       /* Handler address bits  0-15 */
    uint16_t selector;         /* Code segment selector */
    uint8_t  zero;             /* Always 0 */
    uint8_t  type_attributes;  /* Gate type + DPL + Present */
    uint16_t offset_high;      /* Handler address bits 16-31 */
} __attribute__((packed)) idt_entry_t;

/* IDT pointer (loaded with lidt) */
typedef struct {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed)) idt_ptr_t;

static idt_entry_t idt[256];
static idt_ptr_t   idt_ptr;

void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags) {
    idt[num].offset_low      = (uint16_t)(base & 0xFFFF);
    idt[num].offset_high     = (uint16_t)((base >> 16) & 0xFFFF);
    idt[num].selector        = sel;
    idt[num].zero            = 0;
    idt[num].type_attributes = flags;
}

/*
 * Gate type flags:
 *   0x8E = 32-bit interrupt gate  (clears IF — use for hardware IRQs)
 *   0x8F = 32-bit trap gate       (preserves IF — use for CPU exceptions)
 *   0xEF = trap gate, DPL=3      (ring 3 can call — use for int 0x80 syscall)
 */
void idt_init(void) {
    idt_ptr.limit = (uint16_t)(sizeof(idt) - 1);
    idt_ptr.base  = (uint32_t)&idt[0];

    /* CPU exceptions 0-31 */
    idt_set_gate( 0, (uint32_t)isr0,  0x08, 0x8F);  /* #DE Divide-by-zero */
    idt_set_gate( 1, (uint32_t)isr1,  0x08, 0x8F);  /* #DB Debug */
    idt_set_gate( 2, (uint32_t)isr2,  0x08, 0x8F);  /* NMI */
    idt_set_gate( 3, (uint32_t)isr3,  0x08, 0x8F);  /* #BP Breakpoint */
    idt_set_gate( 4, (uint32_t)isr4,  0x08, 0x8F);  /* #OF Overflow */
    idt_set_gate( 5, (uint32_t)isr5,  0x08, 0x8F);  /* #BR Bound range exceeded */
    idt_set_gate( 6, (uint32_t)isr6,  0x08, 0x8F);  /* #UD Invalid opcode */
    idt_set_gate( 7, (uint32_t)isr7,  0x08, 0x8F);  /* #NM Device not available */
    idt_set_gate( 8, (uint32_t)isr8,  0x08, 0x8E);  /* #DF Double fault (errcode) */
    idt_set_gate( 9, (uint32_t)isr9,  0x08, 0x8F);  /* Coprocessor segment overrun */
    idt_set_gate(10, (uint32_t)isr10, 0x08, 0x8F);  /* #TS Invalid TSS (errcode) */
    idt_set_gate(11, (uint32_t)isr11, 0x08, 0x8F);  /* #NP Segment not present (ec)*/
    idt_set_gate(12, (uint32_t)isr12, 0x08, 0x8F);  /* #SS Stack fault (errcode) */
    idt_set_gate(13, (uint32_t)isr13, 0x08, 0x8F);  /* #GP General protection (ec) */
    idt_set_gate(14, (uint32_t)isr14, 0x08, 0x8F);  /* #PF Page fault (errcode) */
    idt_set_gate(15, (uint32_t)isr15, 0x08, 0x8F);  /* Reserved */
    idt_set_gate(16, (uint32_t)isr16, 0x08, 0x8F);  /* #MF x87 FPU error */
    idt_set_gate(17, (uint32_t)isr17, 0x08, 0x8F);  /* #AC Alignment check (ec) */
    idt_set_gate(18, (uint32_t)isr18, 0x08, 0x8F);  /* #MC Machine check */
    idt_set_gate(19, (uint32_t)isr19, 0x08, 0x8F);  /* #XF SIMD fp exception */
    idt_set_gate(20, (uint32_t)isr20, 0x08, 0x8F);  /* #VE Virtualization */
    idt_set_gate(21, (uint32_t)isr21, 0x08, 0x8F);  /* #CP Control protection (ec) */
    idt_set_gate(22, (uint32_t)isr22, 0x08, 0x8F);
    idt_set_gate(23, (uint32_t)isr23, 0x08, 0x8F);
    idt_set_gate(24, (uint32_t)isr24, 0x08, 0x8F);
    idt_set_gate(25, (uint32_t)isr25, 0x08, 0x8F);
    idt_set_gate(26, (uint32_t)isr26, 0x08, 0x8F);
    idt_set_gate(27, (uint32_t)isr27, 0x08, 0x8F);
    idt_set_gate(28, (uint32_t)isr28, 0x08, 0x8F);
    idt_set_gate(29, (uint32_t)isr29, 0x08, 0x8F);  /* HV Hypervisor injection */
    idt_set_gate(30, (uint32_t)isr30, 0x08, 0x8F);  /* #VC VMM communication (ec) */
    idt_set_gate(31, (uint32_t)isr31, 0x08, 0x8F);

    /* Hardware IRQs 0-15 (remapped to vectors 32-47) */
    idt_set_gate(32, (uint32_t)irq0,  0x08, 0x8E);
    idt_set_gate(33, (uint32_t)irq1,  0x08, 0x8E);
    idt_set_gate(34, (uint32_t)irq2,  0x08, 0x8E);
    idt_set_gate(35, (uint32_t)irq3,  0x08, 0x8E);
    idt_set_gate(36, (uint32_t)irq4,  0x08, 0x8E);
    idt_set_gate(37, (uint32_t)irq5,  0x08, 0x8E);
    idt_set_gate(38, (uint32_t)irq6,  0x08, 0x8E);
    idt_set_gate(39, (uint32_t)irq7,  0x08, 0x8E);
    idt_set_gate(40, (uint32_t)irq8,  0x08, 0x8E);
    idt_set_gate(41, (uint32_t)irq9,  0x08, 0x8E);
    idt_set_gate(42, (uint32_t)irq10, 0x08, 0x8E);
    idt_set_gate(43, (uint32_t)irq11, 0x08, 0x8E);
    idt_set_gate(44, (uint32_t)irq12, 0x08, 0x8E);
    idt_set_gate(45, (uint32_t)irq13, 0x08, 0x8E);
    idt_set_gate(46, (uint32_t)irq14, 0x08, 0x8E);
    idt_set_gate(47, (uint32_t)irq15, 0x08, 0x8E);

    /* NMI: its own stub, which does NOT take the Big Kernel Lock — see
     * nmi_isr in isr.asm.  An interrupt gate, so the dump runs with interrupts
     * off. */
    {
        extern void nmi_isr(void);
        idt_set_gate(2, (uint32_t)nmi_isr, 0x08, 0x8E);
    }

    /* Syscall entry: interrupt gate (clears IF), DPL=3 so ring 3 can invoke it */
    idt_set_gate(0x80, (uint32_t)isr128, 0x08, 0xEE);

    /* LAPIC timer (vector 0xF0): per-CPU preemption clock for APs. */
    extern void lapic_timer_isr(void);
    idt_set_gate(0xF0, (uint32_t)lapic_timer_isr, 0x08, 0x8E);

    /* TLB shootdown IPI (vector 0xFD): bare handler, flushes this CPU's TLB. */
    extern void tlb_ipi_isr(void);
    idt_set_gate(0xFD, (uint32_t)tlb_ipi_isr, 0x08, 0x8E);

    __asm__ volatile("lidt (%0)" :: "r"(&idt_ptr));
}

/* Load the (already-built, shared) IDT on the calling CPU — used by APs, which
 * reuse the BSP's IDT but must each execute their own lidt. */
void idt_load(void) {
    __asm__ volatile("lidt (%0)" :: "r"(&idt_ptr));
}
