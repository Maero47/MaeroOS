#include "irq.h"
#include "isr.h"
#include "pic.h"
#include "apic.h"
#include <io.h>
#include <stddef.h>

/* Signal delivery on return from an interrupt to ring 3 (Linux
 * exit_to_user_mode_loop -> arch_do_signal_or_restart on every IRQ return):
 * a thread that never enters the kernel on its own, e.g. one spinning in user
 * mode, otherwise sees a signal — including the SIGKILL of a group exit —
 * only at its next syscall.  Done after the EOI so that a fatal signal, which
 * switches away for good in proc_exit, cannot leave the interrupt in service. */
extern void signal_return_to_user(registers_t *regs, int syscall_nr);
static void irq_return_signals(registers_t *regs) {
    if ((regs->cs & 3) == 3)
        signal_return_to_user(regs, -1);
}

/*
 * C handler chains for each hardware IRQ (0-15).  PCI devices share lines
 * (e.g. RTL8139 + AC97 both on IRQ 11 under QEMU), so each IRQ supports a
 * small chain; every handler must check its own device's status register
 * and treat "not mine" as a no-op.
 */
#define IRQ_CHAIN 4
static isr_handler_t irq_handlers[16][IRQ_CHAIN];

void irq_install_handler(uint8_t irq, isr_handler_t handler) {
    if (irq >= 16) return;
    for (int i = 0; i < IRQ_CHAIN; i++) {
        if (irq_handlers[irq][i] == handler) return;   /* already chained */
        if (!irq_handlers[irq][i]) {
            irq_handlers[irq][i] = handler;
            return;
        }
    }
}

void irq_remove_handler(uint8_t irq) {
    if (irq >= 16) return;
    for (int i = 0; i < IRQ_CHAIN; i++)
        irq_handlers[irq][i] = (isr_handler_t)0;
}

/*
 * irq_handler — called from irq_common_stub in isr.asm.
 *
 * Dispatches to the registered C handler (if any), then sends End-Of-Interrupt
 * to the PIC.  Spurious IRQ7 (master) and IRQ15 (slave) are detected by reading
 * the In-Service Register before sending EOI — they must NOT receive EOI.
 */
void irq_handler(registers_t *regs) {
    /* ── LAPIC timer (vector 0xF0): the AP's preemption clock ────────────────
     * Not a PIC IRQ — acknowledge via the Local APIC, never the 8259.  Drive
     * the scheduler tick exactly like the PIT does on the BSP (preempt only
     * ring 3; scheduler_tick enforces that via user_mode). */
    if (regs->int_no == LAPIC_TIMER_VECTOR) {
        apic_eoi();
        int user_mode = (regs->cs & 3) != 0;
        extern void scheduler_tick(int user_mode);
        scheduler_tick(user_mode);
        irq_return_signals(regs);
        return;
    }

    uint8_t irq = (uint8_t)(regs->int_no - 32);

    /* ── Spurious IRQ detection ─────────────────────────────────────────────
     * A spurious interrupt does not correspond to a real hardware event and
     * must not be acknowledged with EOI.  Read the ISR to check.
     */
    if (irq == 7) {
        /* Read master ISR */
        outb(0x20, 0x0B);
        if (!(inb(0x20) & 0x80))
            return;                 /* Spurious: no EOI */
    } else if (irq == 15) {
        /* Read slave ISR */
        outb(0xA0, 0x0B);
        if (!(inb(0xA0) & 0x80)) {
            outb(0x20, 0x20);       /* Spurious slave: EOI to master ONLY */
            return;
        }
    }

    /* Dispatch to every chained handler (shared PCI lines) */
    if (irq < 16) {
        for (int i = 0; i < IRQ_CHAIN; i++)
            if (irq_handlers[irq][i])
                irq_handlers[irq][i](regs);
    }

    /* Send EOI */
    if (irq >= 8)
        outb(0xA0, 0x20);   /* Slave EOI */
    outb(0x20, 0x20);       /* Master EOI (always) */

    irq_return_signals(regs);
}
