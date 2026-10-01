#include "irq.h"
#include "isr.h"
#include "pic.h"
#include "apic.h"
#include "percpu.h"
#include <io.h>
#include <stddef.h>
#include <kernel/kprof.h>

#define RESCHED_IPI_VECTOR 0xFCU   /* proc/scheduler.c */
extern void sched_irq_exit(int from_user);

/* Signal delivery on return from an interrupt to ring 3 (Linux
 * exit_to_user_mode_loop -> arch_do_signal_or_restart on every IRQ return):
 * a thread that never enters the kernel on its own, e.g. one spinning in user
 * mode, otherwise sees a signal — including the SIGKILL of a group exit —
 * only at its next syscall.  Done after the EOI so that a fatal signal, which
 * switches away for good in proc_exit, cannot leave the interrupt in service. */
extern void signal_return_to_user(registers_t *regs, int syscall_nr);
static void irq_return_signals(registers_t *regs) {
    /* The handler's own work is over: wakes from here on (none, normally)
     * are not interrupt context.  Cleared BEFORE delivery, because a fatal
     * signal exits the thread in proc_exit and a stop signal parks it in
     * proc_stop_self — both switch away and never come back through
     * irq_handler on this CPU, which would leave the flag set and turn off
     * the futex sync hand-off (sched_wakeup) here for good. */
    cpus[this_cpu_id()].in_irq = 0;
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

/* Unchains one handler and keeps the rest: the line may be shared (ACPI's SCI
 * with a PCI NIC's INTx, say). */
void irq_remove_handler(uint8_t irq, isr_handler_t handler) {
    if (irq >= 16) return;
    int j = 0;
    for (int i = 0; i < IRQ_CHAIN; i++)
        if (irq_handlers[irq][i] != handler)
            irq_handlers[irq][j++] = irq_handlers[irq][i];
    while (j < IRQ_CHAIN)
        irq_handlers[irq][j++] = (isr_handler_t)0;
}

static isr_handler_t msi_handlers[MSI_VECTORS];

int msi_install_handler(isr_handler_t handler) {
    if (!apic_available()) return -1;
    for (int i = 0; i < MSI_VECTORS; i++)
        if (!msi_handlers[i]) {
            msi_handlers[i] = handler;
            return (int)MSI_VECTOR_BASE + i;
        }
    return -1;
}

/*
 * irq_handler — called from irq_common_stub in isr.asm.
 *
 * Dispatches to the registered C handler (if any), then sends End-Of-Interrupt
 * to the PIC.  Spurious IRQ7 (master) and IRQ15 (slave) are detected by reading
 * the In-Service Register before sending EOI — they must NOT receive EOI.
 */
static void irq_handler_body(registers_t *regs);

void irq_handler(registers_t *regs) {
    int kp_old = kprof_switch(KPB_IRQ);
    /* A flag, not a nesting count: IRQ gates run with interrupts off, and
     * the paths that leave without irq_return_signals (spurious IRQ7/15)
     * are covered by clearing it again below. */
    cpus[this_cpu_id()].in_irq = 1;
    irq_handler_body(regs);
    cpus[this_cpu_id()].in_irq = 0;
    kprof_switch(kp_old);
    /* A wake from this IRQ (a device's reader, a tick-expired sleeper, a
     * reschedule IPI from another CPU) or the tick's slice expiry asked for a
     * switch: take it on the way back to user mode — Linux's IRQ-exit
     * preemption, so the woken thread runs now, not when the slice ends.  The
     * handler is done (EOI sent, signals delivered), so nothing is in flight. */
    sched_irq_exit((regs->cs & 3) != 0);
}

static void irq_handler_body(registers_t *regs) {
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

    /* ── Reschedule IPI (vector 0xFC): another CPU woke a thread that should
     * displace ours, or kicked us out of the idle halt.  It set our
     * need_resched; the switch is irq_handler's return-to-user check. */
    if (regs->int_no == RESCHED_IPI_VECTOR) {
        apic_eoi();
        irq_return_signals(regs);
        return;
    }

    /* ── MSI (vectors 0xE0-0xE3): edge-triggered messages to the LAPIC. */
    if (regs->int_no >= MSI_VECTOR_BASE &&
        regs->int_no < MSI_VECTOR_BASE + MSI_VECTORS) {
        isr_handler_t h = msi_handlers[regs->int_no - MSI_VECTOR_BASE];
        if (h) h(regs);
        apic_eoi();
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

    /* IRQ0 (PIT) drives scheduler_tick, which may yield() and switch to
     * another thread from inside the handler.  Acknowledge it FIRST, exactly
     * like the LAPIC timer above: the 8259 runs fully nested (no auto-EOI), so
     * an EOI left until after the handler would keep IRQ0 — and with it every
     * lower-priority IRQ — in service for as long as the next thread runs
     * without passing back through here (a fresh child via trapret, a thread
     * woken from sleep).  The tick counter would freeze and no timeout would
     * ever expire.  IRQ0 is edge-triggered and the gate keeps IF=0, so the
     * early EOI cannot re-enter this handler. */
    if (irq == 0)
        outb(0x20, 0x20);

    /* Dispatch to every chained handler (shared PCI lines) */
    if (irq < 16) {
        for (int i = 0; i < IRQ_CHAIN; i++)
            if (irq_handlers[irq][i])
                irq_handlers[irq][i](regs);
    }

    /* Send EOI (IRQ0 was acknowledged before dispatch) */
    if (irq >= 8)
        outb(0xA0, 0x20);   /* Slave EOI */
    if (irq != 0)
        outb(0x20, 0x20);   /* Master EOI */

    irq_return_signals(regs);
}
