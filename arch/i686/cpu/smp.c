#include "smp.h"
#include "apic.h"
#include "gdt.h"
#include "tss.h"
#include "idt.h"
#include "fpu.h"
#include "percpu.h"
#include "../mm/paging.h"
#include "../../../kernel/printk.h"
#include "../../../mm/kstack.h"
#include <kernel/config.h>
#include "../../../proc/scheduler.h"
#include "pit.h"
#include "../../../drivers/acpi.h"
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

/* Called from the bare TLB IPI stub (no BKL), at every trap entry and from
 * spin loops.  Sample the request generation BEFORE flushing and ack exactly
 * that sample: every request posted before the sample is covered by the flush
 * that follows it, and a request posted after it stays unacked until the next
 * serve.  Interrupts are held off so a nested IPI cannot ack a newer
 * generation and then have this frame overwrite it with the older one. */
void tlb_serve_pending(void) {
    uint32_t fl;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(fl) :: "memory");
    struct cpu *c = &cpus[this_cpu_id()];
    uint32_t req = c->tlb_req_gen;
    if (req != c->tlb_ack_gen) {
        __sync_synchronize();
        flush_local_tlb();
        __sync_synchronize();
        c->tlb_ack_gen = req;
    }
    if (fl & 0x200) __asm__ volatile("sti" ::: "memory");
}

/* C half of the TLB IPI (called from tlb_ipi_isr; must NOT take the BKL — the
 * sender holds it). */
void tlb_ipi_handler(void) {
    tlb_serve_pending();
    apic_eoi();
}

/* DEBUG counters: shootdowns sent, IPIs re-sent to a slow target, acks. */
volatile uint32_t g_tlb_sends = 0, g_tlb_timeouts = 0, g_tlb_acks = 0;

static void tlb_ipi_to(uint32_t apicid) {
    /* Fixed delivery, physical destination, edge, assert. */
    apic_write(LAPIC_REG_ICR_HI, apicid << 24);
    apic_write(LAPIC_REG_ICR_LO, TLB_IPI_VECTOR | (1U << 14));
}

/* Force every other online CPU to flush its TLB before we return.
 *
 * CRITICAL FOR CORRECTNESS: the caller is about to free/reuse the flushed
 * frames, so this never returns before every target has acked a generation at
 * least as new as the one posted here — giving up on a laggard is a
 * use-after-free.  A target always gets there: running user code (IF=1) it
 * takes the IPI; in the kernel it is either spinning for the BKL we hold or
 * idling, and both loops call tlb_serve_pending(); every trap entry serves as
 * well.  The LAPIC coalesces a same-vector edge IPI that arrives while one is
 * still pending, so a slow target is re-sent the IPI periodically instead. */
void tlb_shootdown(void) {
    if (!apic_available() || g_cpu_count < 2) return;
    uint32_t self = this_cpu_id();
    uint32_t want[MAX_CPUS];
    int      target[MAX_CPUS];
    g_tlb_sends++;

    for (uint32_t id = 0; id < MAX_CPUS; id++) {
        target[id] = id != self && cpus[id].online;
        if (target[id])
            want[id] = __sync_add_and_fetch(&cpus[id].tlb_req_gen, 1);
    }

    /* IPI all-excluding-self, fixed delivery, edge, vector TLB_IPI_VECTOR. */
    apic_write(LAPIC_REG_ICR_HI, 0);
    apic_write(LAPIC_REG_ICR_LO, TLB_IPI_VECTOR | (1U << 14) | (3U << 18));

    for (uint32_t id = 0; id < MAX_CPUS; id++) {
        if (!target[id]) continue;
        uint32_t s = 0;
        int warned = 0;
        while ((int32_t)(cpus[id].tlb_ack_gen - want[id]) < 0) {
            /* A concurrent sender (none today: callers hold the BKL) would be
             * waiting on us; serving our own requests keeps that deadlock-free. */
            tlb_serve_pending();
            if ((++s & 0x3FFFFU) == 0) {
                g_tlb_timeouts++;
                tlb_ipi_to(cpus[id].apicid);
                if (s >= 0x10000000U && !warned) {
                    warned = 1;
                    printk("[SMP]  TLB shootdown: CPU %u slow to ack (gen %u/%u), still waiting\n",
                           (unsigned)id, (unsigned)cpus[id].tlb_ack_gen,
                           (unsigned)want[id]);
                }
            }
            __asm__ volatile("pause");
        }
        g_tlb_acks++;
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

    /* Tell the BSP we made it — unless it already gave up on us.  An AP the
     * BSP has written off is not counted online, so no TLB shootdown would
     * ever reach it: it must never run threads.  Park it for good. */
    struct cpu *me = &cpus[this_cpu_id()];
    if (__sync_val_compare_and_swap(&me->boot_state, 0, 1) != 0)
        for (;;) __asm__ volatile("cli; hlt");

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
    /* Only now does this CPU become a TLB-shootdown target.  Until here it ran
     * on the kernel pgdir alone, so it cannot hold a stale user translation,
     * and it spun with interrupts off (calibrating against the PIT) where it
     * could not ack: counting it earlier let a shootdown on the BSP wait for
     * it with the BKL held and IF=0 — no PIT tick, so no calibration, so no
     * ack, forever.  A sender that snapshotted its targets just before this
     * store is still safe: we acquire the BKL (which it holds) before our
     * first user CR3 load, and that load starts us with a clean TLB. */
    me->online = 1;
    __sync_synchronize();
    bkl_acquire();
    __asm__ volatile("sti");     /* allow this CPU's own exceptions/syscalls */
    scheduler_start();
    for (;;) __asm__ volatile("hlt");   /* unreachable */
}

/* ── bring up one AP via INIT-SIPI-SIPI ─────────────────────────────────── */
static int boot_one_ap(uint8_t apicid, uint32_t idx) {
    /* Stage the trampoline at physical 0x8000 (reached via the direct map). */
    uint32_t len = (uint32_t)(ap_trampoline_end - ap_trampoline_start);
    for (uint32_t i = 0; i < len; i++)
        ((volatile uint8_t *)TRAMP_VIRT)[i] = (uint8_t)ap_trampoline_start[i];

    /* Patch CR3 / stack / entry into the staged copy. */
    uint32_t o_cr3   = (uint32_t)((char *)&ap_tramp_cr3   - ap_trampoline_start);
    uint32_t o_stack = (uint32_t)((char *)&ap_tramp_stack - ap_trampoline_start);
    uint32_t o_entry = (uint32_t)((char *)&ap_tramp_entry - ap_trampoline_start);

    /* A guarded kernel stack (mm/kstack.c): this CPU's scheduler loop runs
     * on it for good, so an overflow there must fault, not corrupt. */
    void *stack = kstack_alloc();
    if (!stack) return 0;
    uint32_t stack_top = (uint32_t)(uintptr_t)stack + KSTACKSIZE;

    *(volatile uint32_t *)(TRAMP_VIRT + o_cr3)   = kernel_pgdir_phys;
    *(volatile uint32_t *)(TRAMP_VIRT + o_stack) = stack_top;
    *(volatile uint32_t *)(TRAMP_VIRT + o_entry) = (uint32_t)(uintptr_t)&ap_entry;

    /* The AP turns paging on while still executing at linear 0x8000, so that
     * page must be valid in the kernel pgdir until it reaches higher-half C.
     * Identity-map it (kernel pgdir only — never copied into user pgdirs). */
    if (paging_map(TRAMP_PHYS, TRAMP_PHYS, PAGE_PRESENT | PAGE_WRITABLE) != 0) {
        kstack_free(stack);
        return 0;                  /* this AP stays offline; the BSP runs on */
    }

    /* Give the AP its logical slot before it runs any kernel code: its first
     * this_cpu_id() (gdt_init_ap) must already name it. */
    cpus[idx].boot_state = 0;
    percpu_map_apic(apicid, idx);

    /* INIT (assert), wait 10 ms, then two STARTUP IPIs with the vector = the
     * trampoline page number (0x8000 >> 12 = 0x08). */
    send_ipi(apicid, 0x00004500U);                 /* INIT, assert, edge      */
    delay_ms(10);
    send_ipi(apicid, 0x00004600U | (TRAMP_PHYS >> 12));   /* STARTUP #1       */
    delay_short();
    send_ipi(apicid, 0x00004600U | (TRAMP_PHYS >> 12));   /* STARTUP #2       */

    /* Wait up to ~200 ms for the AP to signal alive. */
    for (int i = 0; i < 20 && cpus[idx].boot_state != 1; i++) delay_ms(10);

    /* Give up — atomically, so an AP arriving right now either made it (1) or
     * will see 2 and park itself.  An abandoned AP may still be running on its
     * stack, so that is deliberately leaked, and its slot is never reused. */
    if (__sync_val_compare_and_swap(&cpus[idx].boot_state, 0, 2) != 1)
        return 0;
    return 1;
}

uint32_t smp_boot_aps(void) {
    if (!apic_available()) { printk("[SMP]  no LAPIC — staying uniprocessor\n"); return 1; }

    uint32_t hint = apic_cpu_count_hint();
    if (hint > MAX_CPUS) hint = MAX_CPUS;
    uint32_t bsp = apic_id();

    /* The MADT names every enabled CPU's APIC id (acpi_init ran before us);
     * real machines need not number them 0..N-1.  Without ACPI, fall back to
     * the ids QEMU uses for -smp N: 0..N-1 for the CPUID count hint. */
    uint8_t ids[32];
    uint32_t nids = acpi_madt_lapic_ids(ids, sizeof(ids));
    if (nids) {
        printk("[SMP]  %u CPU(s) listed in the MADT\n", (unsigned)nids);
    } else {
        for (uint32_t id = 0; id < hint && id < sizeof(ids); id++) ids[nids++] = (uint8_t)id;
    }

    uint32_t next_idx = 1;              /* logical slot 0 is the BSP */
    for (uint32_t k = 0; k < nids && next_idx < MAX_CPUS; k++) {
        uint32_t id = ids[k];
        if (id == bsp) continue;
        /* From here a second CPU may execute kernel code, so this_cpu_id() must
         * go back to asking the hardware.  Before the AP is started, not after:
         * the AP's very first kernel code calls it. */
        smp_percpu_go_multi();
        uint32_t idx = next_idx++;
        if (boot_one_ap((uint8_t)id, idx)) {
            g_cpu_count++;           /* it marks itself online (ap_entry) */
            printk("[SMP]  CPU %u (apic id %u) online\n", (unsigned)idx, (unsigned)id);
        } else {
            printk("[SMP]  CPU apic id %u did NOT come up\n", (unsigned)id);
        }
    }
    printk("[SMP]  %u CPU(s) online\n", (unsigned)g_cpu_count);
    return g_cpu_count;
}
