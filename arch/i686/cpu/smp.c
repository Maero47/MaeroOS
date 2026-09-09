#include "smp.h"
#include "apic.h"
#include "gdt.h"
#include "tss.h"
#include "idt.h"
#include "fpu.h"
#include "percpu.h"
#include "../mm/paging.h"
#include "../../../kernel/printk.h"
#include "../../../mm/heap.h"
#include "../../../proc/scheduler.h"
#include "pit.h"
#include <stddef.h>

/* The trampoline blob + its patch words (defined in ap_trampoline.asm). */
extern char     ap_trampoline_start[];
extern char     ap_trampoline_end[];
extern uint32_t ap_tramp_cr3;
extern uint32_t ap_tramp_stack;
extern uint32_t ap_tramp_entry;

extern uint32_t kernel_pgdir_phys;

#ifndef PAGE_PRESENT
#define PAGE_PRESENT  0x1U
#define PAGE_WRITABLE 0x2U
#endif

#define TRAMP_PHYS   0x8000U
#define TRAMP_VIRT   (0xC0000000U + TRAMP_PHYS)   /* direct map of phys 0x8000 */
#define AP_STACK_SZ  (16U * 1024U)
#define MAX_CPUS     8

static volatile int      g_ap_alive = 0;     /* set by the AP that just booted  */
static volatile uint32_t g_cpu_count = 1;    /* BSP only, until APs come online  */

/* The BSP sets this once the process table is fully populated (init created) and
 * it is about to enter the scheduler.  APs spin until then so they never scan a
 * half-built ptable that the BSP is still modifying without the BKL. */
volatile int g_smp_go = 0;

uint32_t smp_cpu_count(void) { return g_cpu_count; }

/* ── S7: TLB shootdown ───────────────────────────────────────────────────── */
#define TLB_IPI_VECTOR  0xFDU

static inline void flush_local_tlb(void) {
    uint32_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");
}

/* Called from the bare TLB IPI stub (no BKL) and from spin loops. */
void tlb_serve_pending(void) {
    struct cpu *c = &cpus[this_cpu_id()];
    if (c->tlb_pending) {
        flush_local_tlb();
        c->tlb_pending = 0;
        __sync_synchronize();
    }
}

/* C half of the TLB IPI (called from tlb_ipi_isr; must NOT take the BKL — the
 * sender holds it). */
void tlb_ipi_handler(void) {
    tlb_serve_pending();
    apic_eoi();
}

/* DEBUG counters: confirm shootdowns are delivered + acked, not timing out. */
volatile uint32_t g_tlb_sends = 0, g_tlb_timeouts = 0, g_tlb_acks = 0;

/* Force every other online CPU to flush its TLB before we return.  Holds while
 * spinning: a target either takes the IPI (running user, IF=1) or clears its
 * flag via tlb_serve_pending() in its bkl/idle spin (IF=0) — deadlock-free. */
void tlb_shootdown(void) {
    if (!apic_available() || g_cpu_count < 2) return;
    uint32_t self = this_cpu_id();
    g_tlb_sends++;

    for (uint32_t id = 0; id < g_cpu_count && id < MAX_CPUS; id++)
        if (id != self) cpus[id].tlb_pending = 1;
    __sync_synchronize();

    /* IPI all-excluding-self, fixed delivery, edge, vector TLB_IPI_VECTOR. */
    apic_write(LAPIC_REG_ICR_HI, 0);
    apic_write(LAPIC_REG_ICR_LO, TLB_IPI_VECTOR | (1U << 14) | (3U << 18));

    /* Wait for every other CPU to acknowledge.  CRITICAL FOR CORRECTNESS: the
     * caller is about to free/reuse the flushed frames, so we must NOT proceed
     * until the target has actually flushed — giving up on a stale entry is a
     * use-after-free (memory corruption).  The LAPIC coalesces rapid same-vector
     * edge IPIs, so a target running in user mode can miss the edge and never
     * flush; therefore RE-SEND the IPI periodically to any laggard instead of
     * timing out.  Deadlock-free: targets spinning in the kernel (IF=0) clear
     * their flag via tlb_serve_pending() in their bkl/idle spins. */
    for (uint32_t id = 0; id < g_cpu_count && id < MAX_CPUS; id++) {
        if (id == self) continue;
        uint32_t s = 0, resends = 0;
        while (cpus[id].tlb_pending) {
            /* One quick re-send early (handles a coalesced/missed edge IPI to a
             * user-mode target) then bounded wait — re-sending repeatedly here
             * holds the BKL too long and explodes stall latency, and the residual
             * corruption is NOT from shootdown timeouts anyway. */
            if (s == 0x40000 && resends == 0) {
                resends = 1; g_tlb_timeouts++;
                apic_write(LAPIC_REG_ICR_HI, 0);
                apic_write(LAPIC_REG_ICR_LO,
                           TLB_IPI_VECTOR | (1U << 14) | (3U << 18));
            }
            /* Give up the active wait after a bounded spin — but DO NOT clear
             * tlb_pending.  Leaving it set means the target CPU will flush this
             * request itself the next time it serves pending: in any kernel spin
             * (bkl/idle) AND now at every trap entry (tlb_serve_pending in the
             * ISR stubs), i.e. by its next timer tick (≤10 ms) at the latest, and
             * GUARANTEED (the flag persists until serviced).  Previously we
             * cleared the flag here, so a target that missed the IPI never
             * flushed until its next CR3 reload (a context switch, up to a full
             * timeslice later) → long-lived stale TLB → the residual corruption.
             * Backstop heals it fast without holding the BKL longer (no stalls). */
            if (++s >= 2000000U) break;
            __asm__ volatile("pause");
        }
        if (!cpus[id].tlb_pending) g_tlb_acks++; else g_tlb_timeouts++;
    }
}

/* ── timing ─────────────────────────────────────────────────────────────── */
static void delay_ms(uint32_t ms) {
    uint32_t start = pit_ticks();
    uint32_t ticks = (ms + 9) / 10;          /* PIT runs at 100 Hz */
    if (!ticks) ticks = 1;
    while ((uint32_t)(pit_ticks() - start) < ticks) __asm__ volatile("pause");
}
static void delay_short(void) {              /* ~a few hundred microseconds      */
    for (volatile int i = 0; i < 200000; i++) __asm__ volatile("");
}

/* ── IPI helpers ────────────────────────────────────────────────────────── */
static void ipi_wait_delivery(void) {
    /* ICR low bit 12 = delivery status (1 = send pending). */
    for (int i = 0; i < 1000000 && (apic_read(LAPIC_REG_ICR_LO) & (1U << 12)); i++)
        __asm__ volatile("pause");
}
static void send_ipi(uint8_t apicid, uint32_t icr_lo) {
    apic_write(LAPIC_REG_ICR_HI, (uint32_t)apicid << 24);
    apic_write(LAPIC_REG_ICR_LO, icr_lo);
    ipi_wait_delivery();
}

/* ── AP C entry (higher half; kernel pgdir + trampoline GDT) ─────────────── */
void ap_entry(void) {
    apic_init();                 /* per-CPU LAPIC enable (AP masks LINT0/1) */
    gdt_init_ap();               /* this CPU's own GDT (per-CPU TSS + TLS)   */
    tss_init();                  /* ltr this CPU's TSS (ring3→ring0 esp0)    */
    idt_load();                  /* load the shared IDT on this CPU          */
    fpu_init();                  /* CR0/CR4 are per-CPU: enable SSE+OSFXSR   *
                                  * here too, else user SSE #UDs on the AP.  */

    g_ap_alive = 1;              /* tell the BSP we made it                  */

    /* Wait until the BSP has finished building the process table and is about
     * to enter the scheduler — only then is it safe to scan ptable.  Spin
     * WITHOUT the BKL (we don't touch shared kernel state yet). */
    while (!g_smp_go) __asm__ volatile("pause");

    /* The PIT only interrupts the BSP, so this AP has no preemption clock.
     * Calibrate (shared bus freq) + arm the LAPIC timer at 100 Hz so the AP
     * preempts user threads just like the BSP's PIT does — without it a
     * CPU-bound user thread on the AP would never yield. */
    lapic_timer_calibrate();
    lapic_timer_start_periodic(100);

    /* Enter the scheduler holding the BKL, exactly like the BSP does.  From
     * here on this CPU runs kernel code only under the BKL and user code
     * concurrently.  scheduler_start never returns. */
    bkl_acquire();
    __asm__ volatile("sti");     /* allow this CPU's own exceptions/syscalls */
    scheduler_start();
    for (;;) __asm__ volatile("hlt");   /* unreachable */
}

/* ── bring up one AP via INIT-SIPI-SIPI ─────────────────────────────────── */
static int boot_one_ap(uint8_t apicid) {
    /* Stage the trampoline at physical 0x8000 (reached via the direct map). */
    uint32_t len = (uint32_t)(ap_trampoline_end - ap_trampoline_start);
    for (uint32_t i = 0; i < len; i++)
        ((volatile uint8_t *)TRAMP_VIRT)[i] = (uint8_t)ap_trampoline_start[i];

    /* Patch CR3 / stack / entry into the staged copy. */
    uint32_t o_cr3   = (uint32_t)((char *)&ap_tramp_cr3   - ap_trampoline_start);
    uint32_t o_stack = (uint32_t)((char *)&ap_tramp_stack - ap_trampoline_start);
    uint32_t o_entry = (uint32_t)((char *)&ap_tramp_entry - ap_trampoline_start);

    void *stack = kmalloc(AP_STACK_SZ);
    if (!stack) return 0;
    uint32_t stack_top = (uint32_t)(uintptr_t)stack + AP_STACK_SZ;

    *(volatile uint32_t *)(TRAMP_VIRT + o_cr3)   = kernel_pgdir_phys;
    *(volatile uint32_t *)(TRAMP_VIRT + o_stack) = stack_top;
    *(volatile uint32_t *)(TRAMP_VIRT + o_entry) = (uint32_t)(uintptr_t)&ap_entry;

    /* The AP turns paging on while still executing at linear 0x8000, so that
     * page must be valid in the kernel pgdir until it reaches higher-half C.
     * Identity-map it (kernel pgdir only — never copied into user pgdirs). */
    paging_map(TRAMP_PHYS, TRAMP_PHYS, PAGE_PRESENT | PAGE_WRITABLE);

    g_ap_alive = 0;

    /* INIT (assert), wait 10 ms, then two STARTUP IPIs with the vector = the
     * trampoline page number (0x8000 >> 12 = 0x08). */
    send_ipi(apicid, 0x00004500U);                 /* INIT, assert, edge      */
    delay_ms(10);
    send_ipi(apicid, 0x00004600U | (TRAMP_PHYS >> 12));   /* STARTUP #1       */
    delay_short();
    send_ipi(apicid, 0x00004600U | (TRAMP_PHYS >> 12));   /* STARTUP #2       */

    /* Wait up to ~200 ms for the AP to signal alive. */
    for (int i = 0; i < 20 && !g_ap_alive; i++) delay_ms(10);

    if (!g_ap_alive) { kfree(stack); return 0; }
    return 1;
}

uint32_t smp_boot_aps(void) {
    if (!apic_available()) { printk("[SMP]  no LAPIC — staying uniprocessor\n"); return 1; }

    uint32_t hint = apic_cpu_count_hint();
    if (hint > MAX_CPUS) hint = MAX_CPUS;
    uint32_t bsp = apic_id();

    /* APIC IDs for QEMU -smp N are 0..N-1; probe every id except the BSP's. */
    for (uint32_t id = 0; id < hint; id++) {
        if (id == bsp) continue;
        /* From here a second CPU may execute kernel code, so this_cpu_id() must
         * go back to asking the hardware.  Before the AP is started, not after:
         * the AP's very first kernel code calls it. */
        smp_percpu_go_multi();
        if (boot_one_ap((uint8_t)id)) {
            g_cpu_count++;
            printk("[SMP]  CPU %u (apic id %u) online\n", (unsigned)g_cpu_count - 1, (unsigned)id);
        } else {
            printk("[SMP]  CPU apic id %u did NOT come up\n", (unsigned)id);
        }
    }
    printk("[SMP]  %u CPU(s) online\n", (unsigned)g_cpu_count);
    return g_cpu_count;
}
