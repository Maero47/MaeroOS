#ifndef ARCH_I686_APIC_H
#define ARCH_I686_APIC_H

#include <stdint.h>

/* Local APIC MMIO is identity-mapped at its physical default base. */
#define LAPIC_PHYS_BASE   0xFEE00000U

/* LAPIC register offsets (from the MMIO base). */
#define LAPIC_REG_ID       0x020   /* Local APIC ID (bits 24-31)        */
#define LAPIC_REG_VER      0x030   /* Local APIC version                */
#define LAPIC_REG_TPR      0x080   /* Task Priority                     */
#define LAPIC_REG_EOI      0x0B0   /* End Of Interrupt                  */
#define LAPIC_REG_SVR      0x0F0   /* Spurious Interrupt Vector         */
#define LAPIC_REG_ESR      0x280   /* Error Status                      */
#define LAPIC_REG_ICR_LO   0x300   /* Interrupt Command (low)           */
#define LAPIC_REG_ICR_HI   0x310   /* Interrupt Command (high / dest)   */
#define LAPIC_REG_LVT_TMR  0x320   /* LVT Timer                         */
#define LAPIC_REG_LVT_LINT0 0x350
#define LAPIC_REG_LVT_LINT1 0x360
#define LAPIC_REG_LVT_ERR  0x370   /* LVT Error                         */
#define LAPIC_REG_TMR_INIT 0x380   /* Timer initial count               */
#define LAPIC_REG_TMR_CUR  0x390   /* Timer current count               */
#define LAPIC_REG_TMR_DIV  0x3E0   /* Timer divide config               */

#define LAPIC_SVR_ENABLE   0x100U  /* bit 8: APIC software enable        */
#define LAPIC_SPURIOUS_VEC 0xFFU   /* spurious interrupt vector          */
#define LAPIC_LVT_MASKED   0x10000U/* bit 16: LVT masked                 */

/* Enable + configure the calling CPU's Local APIC.  Call once on the BSP (maps
 * the MMIO page) and again on each AP (per-CPU enable). */
void apic_init(void);

/* Per-CPU LAPIC register access. */
uint32_t apic_read(uint32_t reg);
void     apic_write(uint32_t reg, uint32_t val);

/* This CPU's Local APIC ID (xAPIC: bits 24-31 of the ID register). */
uint32_t apic_id(void);

/* Signal End-Of-Interrupt to the Local APIC. */
void apic_eoi(void);

/* Was a usable Local APIC found + enabled on the BSP? */
int apic_available(void);

/* Best-effort logical-CPU count hint from CPUID leaf 1 (1 if unknown). */
uint32_t apic_cpu_count_hint(void);

/* ── LAPIC timer (per-CPU preemption source for APs) ─────────────────────────
 * The PIT only interrupts the BSP, so APs have no preemption clock.  Calibrate
 * the LAPIC timer against the PIT once (shared bus frequency), then arm it in
 * periodic mode on each CPU that needs preemptive scheduling.  The BSP keeps
 * using the PIT; APs use this. */
#define LAPIC_TIMER_VECTOR  0xF0U   /* IDT vector for the LAPIC timer interrupt */

/* Measure LAPIC timer ticks per 100 ms using the PIT.  Call on any CPU after
 * the PIT is running (the bus frequency is the same on all CPUs). */
void lapic_timer_calibrate(void);

/* Arm THIS CPU's LAPIC timer in periodic mode at `hz`, delivering to
 * LAPIC_TIMER_VECTOR.  Requires lapic_timer_calibrate() to have run. */
void lapic_timer_start_periodic(uint32_t hz);

#endif /* ARCH_I686_APIC_H */
