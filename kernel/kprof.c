#include <kernel/kprof.h>
#include "printk.h"
#include "../arch/i686/cpu/tsc.h"
#include "../arch/i686/cpu/pit.h"

/*
 * See include/kernel/kprof.h for the model.  Everything here is plain 64-bit
 * addition; the only division is at dump time (the kernel links no libgcc
 * 64-bit helpers, so it is a shift-subtract loop).
 */

static uint64_t g_cyc[KPROF_NBUCKET];
static uint32_t g_cnt[KPROF_NBUCKET];      /* entries into each bucket */
uint64_t kprof_ev[KPE_MAX];
static uint32_t g_syscnt[KPROF_NSYS];      /* true invocations, per syscall   */
static uint64_t g_sleep_cyc[KPROF_NSYS];   /* blocked wall time per syscall  */
static uint32_t g_sleep_cnt[KPROF_NSYS];

static uint64_t g_probe_cyc[KPP_MAX];
static uint32_t g_probe_cnt[KPP_MAX];
static const char *const g_probe_name[KPP_MAX] = {
    "sys_pro", "sys_epi", "yield_pre", "sch_scan", "sch_disp",
    "gap_find", "first_mapped", "mmap_pop", "mmap_unmap",
    "fault_read", "fault_zero", "ata<=4", "ata>4",
    "e2_alloc", "e2_inode", "e2_bmap", "e2_copy",
    "sys_body", "sys_resched", "disp_tss", "disp_cr3", "disp_fpu", "sch_kcr3", "sch_fpusave",
    "kdump", "calib",
};

static int      g_cur = KPB_USER;
static uint64_t g_last;                    /* TSC at the last bucket change  */
static uint64_t g_base;                    /* TSC when counting started      */
static int      g_on = 0;

static inline uint64_t rdtsc64(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

int kprof_switch(int bucket) {
    uint32_t fl;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(fl) :: "memory");
    uint64_t now = rdtsc64();
    int old = g_cur;
    if (g_on) {
        g_cyc[old] += now - g_last;
        g_cnt[bucket]++;
    } else {
        g_on   = 1;
        g_base = now;
    }
    g_last = now;
    g_cur  = bucket;
    if (fl & 0x200) __asm__ volatile("sti" ::: "memory");
    return old;
}


uint64_t kprof_probe_begin(void) { return rdtsc64(); }
void kprof_probe_end(int id, uint64_t t0) {
    g_probe_cyc[id] += rdtsc64() - t0;
    g_probe_cnt[id]++;
}
void kprof_syscall_enter(uint32_t nr) {
    g_syscnt[nr < KPROF_NSYS ? nr : KPROF_NSYS - 1]++;
}

/* Blocked time.  sleep_on() records the TSC before switching away and charges
 * the elapsed wall time to the syscall that blocked when it comes back, so
 * "the browser spent 90 s in futex" is measurable independently of who ran. */
uint64_t kprof_sleep_begin(void) { return rdtsc64(); }
void kprof_sleep_end(uint64_t token, int syscall_nr) {
    if (syscall_nr < 0 || syscall_nr >= KPROF_NSYS) syscall_nr = KPROF_NSYS - 1;
    g_sleep_cyc[syscall_nr] += rdtsc64() - token;
    g_sleep_cnt[syscall_nr]++;
}

/* 64 / 32 -> 64 by shift-subtract (dump path only). */
static uint64_t udiv64(uint64_t n, uint32_t d) {
    uint64_t q = 0, r = 0;
    if (!d) return 0;
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1);
        q <<= 1;
        if (r >= d) { r -= d; q |= 1; }
    }
    return q;
}

/* Cycles -> milliseconds using the calibrated TSC rate. */
static unsigned cyc_ms(uint64_t c) {
    uint32_t cpt = tsc_cycles_per_tick();          /* cycles per 10 ms */
    if (!cpt) return 0;
    uint64_t ms = udiv64(c, cpt / 10U ? cpt / 10U : 1U);
    return ms > 0xFFFFFFFFULL ? 0xFFFFFFFFU : (unsigned)ms;
}

void kprof_reset(void) {
    uint32_t fl;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(fl) :: "memory");
    for (int i = 0; i < KPROF_NBUCKET; i++) { g_cyc[i] = 0; g_cnt[i] = 0; }
    for (int i = 0; i < KPE_MAX; i++) kprof_ev[i] = 0;
    for (int i = 0; i < KPROF_NSYS; i++) {
        g_sleep_cyc[i] = 0; g_sleep_cnt[i] = 0; g_syscnt[i] = 0;
    }
    for (int i = 0; i < KPP_MAX; i++) { g_probe_cyc[i] = 0; g_probe_cnt[i] = 0; }
    g_base = g_last = rdtsc64();
    g_on = 1;
    if (fl & 0x200) __asm__ volatile("sti" ::: "memory");
}

void kprof_dump(const char *tag) {
    /* Close the current bucket so the totals include the cycles up to now. */
    int cur = kprof_switch(KPB_SCHED);
    g_cur = cur;                                   /* keep the caller's bucket */

    uint64_t elapsed = g_last - g_base;
    uint64_t sum = 0;
    for (int i = 0; i < KPROF_NBUCKET; i++) sum += g_cyc[i];
    uint64_t sysc = 0;
    for (int i = 0; i < KPROF_NSYS; i++) sysc += g_cyc[KPB_SYSBASE + i];

    printk("[kprof] --- %s  t=%u.%02us  elapsed=%ums accounted=%ums\n",
           tag, (unsigned)(pit_ticks() / 100U), (unsigned)(pit_ticks() % 100U),
           cyc_ms(elapsed), cyc_ms(sum));
    printk("[kprof] user=%u idle=%u sched=%u irq=%u pgfault=%u ata=%u fb=%u exc=%u sys=%u (ms)\n",
           cyc_ms(g_cyc[KPB_USER]), cyc_ms(g_cyc[KPB_IDLE]), cyc_ms(g_cyc[KPB_SCHED]),
           cyc_ms(g_cyc[KPB_IRQ]), cyc_ms(g_cyc[KPB_PGFAULT]), cyc_ms(g_cyc[KPB_ATA]),
           cyc_ms(g_cyc[KPB_FB]), cyc_ms(g_cyc[KPB_EXC]), cyc_ms(sysc));
    printk("[kprof] ev sysc=%u ctxsw=%u pf(cow=%u anon=%u file=%u stk=%u oth=%u)\n",
           (unsigned)kprof_ev[KPE_SYSCALL], (unsigned)kprof_ev[KPE_CTXSW],
           (unsigned)kprof_ev[KPE_PF_COW], (unsigned)kprof_ev[KPE_PF_ANON],
           (unsigned)kprof_ev[KPE_PF_FILE], (unsigned)kprof_ev[KPE_PF_STACK],
           (unsigned)kprof_ev[KPE_PF_OTHER]);
    printk("[kprof] ev ata(rd=%u sect=%u wr=%u wsect=%u) ext2(blk=%u hit=%u miss=%u) wake=%u resched=%u\n",
           (unsigned)kprof_ev[KPE_ATA_RD], (unsigned)kprof_ev[KPE_ATA_RD_SECT],
           (unsigned)kprof_ev[KPE_ATA_WR], (unsigned)kprof_ev[KPE_ATA_WR_SECT],
           (unsigned)kprof_ev[KPE_EXT2_BLK], (unsigned)kprof_ev[KPE_EXT2_HIT],
           (unsigned)kprof_ev[KPE_EXT2_MISS], (unsigned)kprof_ev[KPE_WAKE],
           (unsigned)kprof_ev[KPE_RESCHED]);
    printk("[kprof] ev fb_kb=%u disk_blk=%u distinct=%u ra=%u ra_used=%u pf_seq=%u pf_around=%u\n",
           (unsigned)kprof_ev[KPE_FB_KB], (unsigned)kprof_ev[KPE_EXT2_DISK],
           (unsigned)kprof_ev[KPE_EXT2_DISTINCT], (unsigned)kprof_ev[KPE_EXT2_RA],
           (unsigned)kprof_ev[KPE_EXT2_RA_USED], (unsigned)kprof_ev[KPE_PF_FILE_SEQ],
           (unsigned)kprof_ev[KPE_PF_FILE_AROUND]);

    /* Probes: additive spans, printed only when used.  Never summed with the
     * buckets above -- see the note in kprof.h. */
    {
        int any = 0;
        for (int i = 0; i < KPP_MAX; i++) if (g_probe_cnt[i]) any = 1;
        if (any) {
            printk("[kprof] probe");
            for (int i = 0; i < KPP_MAX; i++)
                if (g_probe_cnt[i])
                    printk(" %s:%ums/%u", g_probe_name[i],
                           cyc_ms(g_probe_cyc[i]), (unsigned)g_probe_cnt[i]);
            printk("\n");
        }
    }

    /* Top syscalls by kernel CPU time, then by blocked wall time. */
    for (int pass = 0; pass < 2; pass++) {
        uint32_t shown[12];
        int ns = 0;
        for (int k = 0; k < 12; k++) {
            uint64_t best = 0; int bi = -1;
            for (int i = 0; i < KPROF_NSYS; i++) {
                uint64_t v = pass ? g_sleep_cyc[i] : g_cyc[KPB_SYSBASE + i];
                if (!v) continue;
                int dup = 0;
                for (int j = 0; j < ns; j++) if (shown[j] == (uint32_t)i) dup = 1;
                if (dup) continue;
                if (v > best) { best = v; bi = i; }
            }
            if (bi < 0) break;
            shown[ns++] = (uint32_t)bi;
        }
        if (!ns) continue;
        printk("[kprof] %s", pass ? "blocked" : "syscpu ");
        for (int k = 0; k < ns; k++) {
            int i = (int)shown[k];
            printk(" %u:%ums/%u", (unsigned)i,
                   cyc_ms(pass ? g_sleep_cyc[i] : g_cyc[KPB_SYSBASE + i]),
                   (unsigned)(pass ? g_sleep_cnt[i] : g_syscnt[i]));
        }
        printk("\n");
    }
}

void kprof_tick(void) {
    static uint32_t last = 0;
    uint32_t now = pit_ticks();
    if (now - last < 1000) return;            /* every 10 s of tick time */
    last = now;
    /* Calibrate: 256 empty spans, so a probe reading can be corrected for the
     * cost of taking it -- and so the cost of an rdtsc on this host, which is
     * the whole instrument, is a printed number rather than an assumption. */
    for (int i = 0; i < 256; i++) {
        uint64_t c0 = kprof_probe_begin();
        kprof_probe_end(KPP_CALIB, c0);
    }
    uint64_t d0 = kprof_probe_begin();
    kprof_dump("periodic");
    kprof_probe_end(KPP_DUMP, d0);
}
