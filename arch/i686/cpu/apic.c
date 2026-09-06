#include "apic.h"
#include "pit.h"
#include "../mm/paging.h"
#include "../../../kernel/printk.h"

/* PAGE_* flags (match arch/i686/mm/paging.h conventions). */
#ifndef PAGE_PRESENT
#define PAGE_PRESENT   0x1U
#define PAGE_WRITABLE  0x2U
#endif
#define PAGE_PCD       0x10U   /* cache-disable: required for MMIO */

#define IA32_APIC_BASE_MSR 0x1BU
#define APIC_BASE_GLOBAL_ENABLE (1U << 11)

static volatile uint32_t *g_lapic = (void *)0;   /* MMIO window (identity) */
static int g_apic_ok = 0;

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}
static inline void wrmsr(uint32_t msr, uint64_t val) {
    uint32_t lo = (uint32_t)val, hi = (uint32_t)(val >> 32);
    __asm__ volatile("wrmsr" :: "c"(msr), "a"(lo), "d"(hi));
}

static inline void cpuid(uint32_t leaf, uint32_t *a, uint32_t *b,
                         uint32_t *c, uint32_t *d) {
    __asm__ volatile("cpuid"
                     : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                     : "a"(leaf));
}

uint32_t apic_read(uint32_t reg)            { return g_lapic[reg / 4]; }
void     apic_write(uint32_t reg, uint32_t v){ g_lapic[reg / 4] = v; }
uint32_t apic_id(void)   { return apic_read(LAPIC_REG_ID) >> 24; }
void     apic_eoi(void)  { apic_write(LAPIC_REG_EOI, 0); }
int      apic_available(void) { return g_apic_ok; }

uint32_t apic_cpu_count_hint(void) {
    uint32_t a, b, c, d;
    cpuid(1, &a, &b, &c, &d);
    if (!(d & (1U << 28))) return 1;          /* HTT clear → count invalid */
    uint32_t n = (b >> 16) & 0xFF;            /* logical processors / pkg  */
    return n ? n : 1;
}

/* Configure the LVTs for a PIC-based ("virtual wire") system.  Masking LINT0
 * was WRONG: with the LAPIC software-enabled, the 8259 PIC delivers its IRQs to
 * the BSP core through LINT0 in ExtINT mode — masking it silently breaks PIC
 * interrupt delivery.  So on the BSP set LINT0 = ExtINT (delivery mode 7) and
 * LINT1 = NMI (delivery mode 4); only the timer + error LVTs are masked (we
 * configure those explicitly later).  APs (which take no PIC IRQs) mask LINT0. */
#define LVT_DELIVERY_EXTINT 0x700U   /* bits 8-10 = 111b */
#define LVT_DELIVERY_NMI    0x400U   /* bits 8-10 = 100b */

static void lapic_setup_lvts(int is_bsp) {
    apic_write(LAPIC_REG_LVT_TMR, LAPIC_LVT_MASKED);
    apic_write(LAPIC_REG_LVT_ERR, LAPIC_LVT_MASKED);
    if (is_bsp) {
        apic_write(LAPIC_REG_LVT_LINT0, LVT_DELIVERY_EXTINT);
        apic_write(LAPIC_REG_LVT_LINT1, LVT_DELIVERY_NMI);
    } else {
        apic_write(LAPIC_REG_LVT_LINT0, LAPIC_LVT_MASKED);
        apic_write(LAPIC_REG_LVT_LINT1, LAPIC_LVT_MASKED);
    }
}

void apic_init(void) {
    int is_bsp = (g_lapic == (void *)0);
    /* First call (BSP): verify CPUID reports a LAPIC and map the MMIO page. */
    if (!g_lapic) {
        uint32_t a, b, c, d;
        cpuid(1, &a, &b, &c, &d);
        if (!(d & (1U << 9))) {           /* CPUID.1:EDX.APIC */
            printk("[APIC] no Local APIC reported by CPUID — SMP disabled\n");
            return;
        }
        /* Ensure the LAPIC is globally enabled in IA32_APIC_BASE. */
        uint64_t base = rdmsr(IA32_APIC_BASE_MSR);
        base |= APIC_BASE_GLOBAL_ENABLE;
        wrmsr(IA32_APIC_BASE_MSR, base);

        /* Identity-map the MMIO page, cache-disabled. */
        paging_map(LAPIC_PHYS_BASE, LAPIC_PHYS_BASE,
                   PAGE_PRESENT | PAGE_WRITABLE | PAGE_PCD);
        g_lapic = (volatile uint32_t *)LAPIC_PHYS_BASE;
    }

    /* Per-CPU software enable: accept all priorities, mask LVTs, set the
     * spurious vector + enable bit. */
    apic_write(LAPIC_REG_TPR, 0);
    lapic_setup_lvts(is_bsp);
    apic_write(LAPIC_REG_SVR, LAPIC_SVR_ENABLE | LAPIC_SPURIOUS_VEC);

    g_apic_ok = 1;
}

/* ── LAPIC timer ─────────────────────────────────────────────────────────── */

#define LAPIC_TMR_DIV_16   0x3U      /* divide config: bus clock / 16          */
#define LAPIC_TMR_PERIODIC (1U << 17)/* LVT timer bit 17: periodic mode        */

/* LAPIC timer ticks counted in 100 ms (bus-clock dependent; same on all CPUs).
 * 0 until lapic_timer_calibrate() runs. */
static volatile uint32_t g_lapic_per_100ms = 0;

void lapic_timer_calibrate(void) {
    if (!g_apic_ok) return;
    /* Count down from max with the timer MASKED (no interrupt), measuring how
     * far it gets in 100 ms of PIT time (10 ticks at 100 Hz). */
    apic_write(LAPIC_REG_TMR_DIV, LAPIC_TMR_DIV_16);
    apic_write(LAPIC_REG_LVT_TMR, LAPIC_LVT_MASKED);   /* masked, one-shot */

    uint32_t t0 = pit_ticks();
    while (pit_ticks() == t0) { __asm__ volatile("pause"); }  /* edge-align */
    t0 = pit_ticks();
    apic_write(LAPIC_REG_TMR_INIT, 0xFFFFFFFFU);       /* start counting down */
    while ((uint32_t)(pit_ticks() - t0) < 10) { __asm__ volatile("pause"); }
    uint32_t remaining = apic_read(LAPIC_REG_TMR_CUR);
    apic_write(LAPIC_REG_TMR_INIT, 0);                 /* stop */

    g_lapic_per_100ms = 0xFFFFFFFFU - remaining;
    printk("[APIC] LAPIC timer calibrated: %u ticks/100ms (div16)\n",
           (unsigned)g_lapic_per_100ms);
}

void lapic_timer_start_periodic(uint32_t hz) {
    if (!g_apic_ok || !g_lapic_per_100ms || !hz) return;
    /* count per period = per_100ms * (1000/hz) / 100 = per_100ms * 10 / hz.
     * Stay in 32-bit (no __udivdi3): per_100ms*10 fits easily for any real bus
     * (e.g. 1 GHz/16 → ~6.25M/100ms → *10 = 62.5M ≪ 2^32). */
    uint32_t count = (g_lapic_per_100ms * 10U) / hz;
    if (!count) count = 1;
    apic_write(LAPIC_REG_TMR_DIV, LAPIC_TMR_DIV_16);
    apic_write(LAPIC_REG_LVT_TMR, LAPIC_TIMER_VECTOR | LAPIC_TMR_PERIODIC);
    apic_write(LAPIC_REG_TMR_INIT, count);
}
