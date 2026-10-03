#include "percpu.h"
#include "apic.h"
#include "spinlock.h"
#include <stdint.h>

struct cpu cpus[MAX_CPUS];

static spinlock_t g_bkl = { 0 };

/*
 * `current_proc` is `cpus[this_cpu_id()].proc`, so this function is on the path
 * of every single reference to the running thread -- and it read the Local APIC
 * ID register to answer.  That register lives in the LAPIC's MMIO page, and a
 * guest access to it exits to the hypervisor: measured on this kernel, one
 * `current_proc` dereference cost about 1.6 us.  `preempt_disable()` names it
 * twice and `preempt_enable()` three times, so guarding one block-cache lookup
 * cost 8 us against 0.2 us of actual work, and the exits added up to roughly a
 * third of a Firefox startup.
 *
 * While only one CPU is executing, the answer cannot change, so it is read once
 * and remembered.  The moment an AP is about to be started, smp_percpu_go_multi
 * turns the cache off and every call reads the register again, exactly as
 * before -- the fast path is not an assumption about the machine, it is a fact
 * about how many CPUs are running.
 */
static uint32_t g_solo_id;
static int      g_solo_valid;
static volatile int g_multi_cpu;

/* Local APIC id -> logical CPU index.  0xFF = never started by us. */
static uint8_t g_apic_to_cpu[256] = { [0 ... 255] = 0xFF };

void smp_percpu_go_multi(void) { g_multi_cpu = 1; }

void percpu_map_apic(uint32_t apicid, uint32_t idx) {
    if (apicid > 255 || idx >= MAX_CPUS) return;
    cpus[idx].apicid = apicid;
    g_apic_to_cpu[apicid] = (uint8_t)idx;
}

static inline uint32_t apic_to_cpu(uint32_t apicid) {
    uint8_t idx = g_apic_to_cpu[apicid & 0xFF];
    /* Only CPUs we started execute kernel code, and each is mapped before it
     * starts, so an unmapped id cannot occur; 0 keeps the old fallback. */
    return idx < MAX_CPUS ? idx : 0;
}

uint32_t this_cpu_id(void) {
    if (!apic_available()) return 0;
    if (!g_multi_cpu) {
        if (!g_solo_valid) {
            g_solo_id    = apic_to_cpu(apic_id());
            g_solo_valid = 1;
        }
        return g_solo_id;
    }
    return apic_to_cpu(apic_id());
}

#ifdef BKLSTAT
#include "bklstat.h"
#include "tsc.h"
#include <registers.h>
#include <kernel/config.h>
#include "../../../kernel/printk.h"

#define R_VEC     448                   /* 0..447: syscall number          */
#define R_SCHED   (R_VEC + 256)         /* scheduler loop, idle re-acquire */
#define R_KTHREAD (R_SCHED + 1)         /* a kernel thread ran             */
#define R_KERNEL  (R_SCHED + 2)         /* bkl_enter() from C, a thread    */
#define NREASON   (R_SCHED + 3)
#define HSTACK    8
#define NHIST     16                    /* log2(us) buckets                */

/* Live state: only its CPU writes it, with interrupts off. */
struct bkl_live {
    uint64_t t_mark;                    /* start of the interval being charged */
    uint64_t t_acq;                     /* outer acquisition                 */
    int      cur;                       /* reason now holding                */
    uint16_t stack[HSTACK];             /* reasons of interrupted nest levels */
} __attribute__((aligned(64)));

/* Counters, zeroed by bklstat_reset. */
struct bkl_cnt {
    uint64_t hold, spin, max_hold, max_spin;
    unsigned acq, contended, nested, bad_switch;
    uint64_t hold_by[NREASON];
    unsigned hold_n[NREASON];
    uint64_t spin_for[NREASON];         /* spun while wanting it for reason r */
    uint64_t spin_on[NREASON];          /* spun while the holder was in r     */
    unsigned hhist[NHIST], shist[NHIST];
} __attribute__((aligned(64)));

static struct bkl_live g_live[MAX_CPUS];
static struct bkl_cnt  g_cnt[MAX_CPUS];
static volatile int    g_holder_reason = -1;
static uint64_t        g_t0;
static uint16_t        g_slot_reason[MAX_PROCS];

static inline uint64_t bst_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static unsigned bst_cyc_per_us(void) {
    unsigned c = (unsigned)(tsc_cycles_per_tick() / 10000U);
    return c ? c : 1U;
}

static inline void bst_hist(unsigned *h, uint64_t cyc) {
    uint64_t t = bst_cyc_per_us();          /* bucket b: <= 2^b us */
    uint32_t b = 0;
    while (cyc > t && b < NHIST - 1) { t <<= 1; b++; }
    h[b]++;
}

static inline void bst_charge(uint32_t id, uint64_t now) {
    g_cnt[id].hold_by[g_live[id].cur] += now - g_live[id].t_mark;
    g_live[id].t_mark = now;
}

static inline void bst_set(uint32_t id, int r, int count) {
    g_live[id].cur = r;
    if (count) g_cnt[id].hold_n[r]++;
    g_holder_reason = r;
}

static int bst_reason(const registers_t *r) {
    if (r->int_no == 0x80) return r->eax < R_VEC ? (int)r->eax : R_KERNEL;
    return R_VEC + (int)(r->int_no & 0xFF);
}
#endif

/*
 * The lock is inactive until the LAPIC is enabled (single-CPU early boot, before
 * apic_init).  After that it is active on every CPU — uncontended on a single
 * CPU, real on SMP.
 *
 * Both halves run with interrupts off, and that is not an optimisation: the
 * lock word and this CPU's depth are two separate objects, and the moment
 * between changing one and the other is a hole an interrupt on THIS CPU can
 * fall into.  bkl_enter takes the lock while the depth still reads 0, so an
 * interrupt landing in those few instructions runs its own bkl_enter, sees
 * depth 0, finds the lock held by itself, and spins for it forever with
 * interrupts already off; the interrupted half can never run the increment
 * that would have told it the lock was its own.  bkl_leave has the mirror
 * hole between the decrement to 0 and the unlock.
 *
 * Interrupts are not reliably off here.  CPU exceptions use TRAP gates, which
 * preserve IF, so a page fault taken from user code enters the stub with
 * interrupts ENABLED and runs straight into bkl_enter's hole.  And every
 * syscall that slept returns with IF set — sleep_on() and yield() sti after
 * the switch back — so the stub reaches bkl_leave's hole with interrupts on
 * even though int 0x80 is an interrupt gate.  Between them that is tens of
 * thousands of exposures per Firefox startup, which is what turned a
 * few-instruction window into a hang about one boot in sixty: silent, because
 * the spinner holds interrupts off forever and the 8259 keeps IRQ0 in service
 * with no EOI ever sent.
 *
 * Saving and restoring the caller's IF (rather than a bare cli/sti) keeps the
 * exception path's interrupts-enabled behaviour intact everywhere else.
 */
static void bkl_enter_reason(int reason) {
    if (!apic_available()) return;
    uint32_t fl;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(fl) :: "memory");
    uint32_t id = this_cpu_id();
    struct cpu *c = &cpus[id];
    if (c->bkl_depth == 0) {
#ifdef BKLSTAT
        if (reason < 0) reason = c->proc ? R_KERNEL : R_SCHED;
        if (!spin_trylock(&g_bkl)) {
            uint64_t t = bst_rdtsc();
            int holder = g_holder_reason;
            while (!spin_trylock(&g_bkl)) {
                tlb_serve_pending();
                __asm__ volatile("pause");
            }
            uint64_t dt = bst_rdtsc() - t;
            struct bkl_cnt *k = &g_cnt[id];
            k->contended++;
            k->spin += dt;
            if (dt > k->max_spin) k->max_spin = dt;
            k->spin_for[reason] += dt;
            k->spin_on[holder >= 0 && holder < NREASON ? holder : R_KERNEL] += dt;
            bst_hist(k->shist, dt);
        }
        uint64_t now = bst_rdtsc();
        g_cnt[id].acq++;
        g_live[id].t_acq = g_live[id].t_mark = now;
        bst_set(id, reason, 1);
#else
        (void)reason;
        /* Spin for the lock, but keep servicing TLB-shootdown requests while we
         * wait: a CPU spinning here has interrupts off, so it can't take the
         * shootdown IPI — without this it could never flush and the sender
         * would wait forever (deadlock). */
        while (!spin_trylock(&g_bkl)) {
            tlb_serve_pending();
            __asm__ volatile("pause");
        }
#endif
    }
#ifdef BKLSTAT
    else {
        if (reason < 0) reason = R_KERNEL;
        bst_charge(id, bst_rdtsc());
        if (c->bkl_depth < HSTACK) g_live[id].stack[c->bkl_depth] = (uint16_t)g_live[id].cur;
        g_cnt[id].nested++;
        bst_set(id, reason, 1);
    }
#endif
    c->bkl_depth++;
    if (fl & 0x200) __asm__ volatile("sti" ::: "memory");
}

void bkl_enter(void) { bkl_enter_reason(-1); }

#ifdef BKLSTAT
/* Trap entry (isr.asm, BKLSTAT builds): the reason is the vector or syscall. */
void bkl_enter_trap(registers_t *r) { bkl_enter_reason(bst_reason(r)); }
#endif

void bkl_leave(void) {
    if (!apic_available()) return;
    uint32_t fl;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(fl) :: "memory");
    uint32_t id = this_cpu_id();
    struct cpu *c = &cpus[id];
    if (c->bkl_depth > 0) {                 /* never underflow */
#ifdef BKLSTAT
        uint64_t now = bst_rdtsc();
        bst_charge(id, now);
#endif
        c->bkl_depth--;
#ifdef BKLSTAT
        if (c->bkl_depth == 0) {
            struct bkl_cnt *k = &g_cnt[id];
            uint64_t h = now - g_live[id].t_acq;
            k->hold += h;
            if (h > k->max_hold) k->max_hold = h;
            bst_hist(k->hhist, h);
            g_holder_reason = -1;
        } else {
            bst_set(id, c->bkl_depth < HSTACK ? g_live[id].stack[c->bkl_depth] : R_KERNEL, 0);
        }
#endif
        if (c->bkl_depth == 0) spin_unlock(&g_bkl);
    }
    if (fl & 0x200) __asm__ volatile("sti" ::: "memory");
}

/* Diagnostic: the lock word and this CPU's nesting depth.  A wedge with the
 * lock held and the depth at 0 is a CPU deadlocked against itself. */
void bkl_state(int *locked, int *depth) {
    if (locked) *locked = g_bkl.locked ? 1 : 0;
    if (depth)  *depth  = apic_available() ? cpus[this_cpu_id()].bkl_depth : 0;
}

#ifdef BKLSTAT
/* The scheduler is about to switch to the thread in ptable slot `slot`: the
 * time from here on is that thread's (its reason when it was switched out). */
void bklstat_sched_in(int slot, int kthread) {
    if (!apic_available() || slot < 0 || slot >= MAX_PROCS) return;
    uint32_t fl;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(fl) :: "memory");
    uint32_t id = this_cpu_id();
    bst_charge(id, bst_rdtsc());
    bst_set(id, kthread ? R_KTHREAD : g_slot_reason[slot], 0);
    if (fl & 0x200) __asm__ volatile("sti" ::: "memory");
}

/* The thread in `slot` switched back to the scheduler.  `depth_ok` is 0 when it
 * switched with the BKL nested more than once (the nesting is per CPU, so the
 * next thread would inherit it). */
void bklstat_sched_out(int slot, int depth_ok) {
    if (!apic_available() || slot < 0 || slot >= MAX_PROCS) return;
    uint32_t fl;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(fl) :: "memory");
    uint32_t id = this_cpu_id();
    bst_charge(id, bst_rdtsc());
    g_slot_reason[slot] = (uint16_t)g_live[id].cur;
    if (!depth_ok) g_cnt[id].bad_switch++;
    bst_set(id, R_SCHED, 0);
    if (fl & 0x200) __asm__ volatile("sti" ::: "memory");
}

void bklstat_reset(void) {
    uint32_t fl;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(fl) :: "memory");
    for (int i = 0; i < MAX_CPUS; i++) {
        uint8_t *b = (uint8_t *)&g_cnt[i];
        for (uint32_t j = 0; j < sizeof g_cnt[i]; j++) b[j] = 0;
    }
    uint64_t now = bst_rdtsc();
    g_t0 = now;
    /* Our own hold started before the reset: count it from now. */
    uint32_t id = this_cpu_id();
    g_live[id].t_acq = g_live[id].t_mark = now;
    if (fl & 0x200) __asm__ volatile("sti" ::: "memory");
}

static void bst_name(int r, char *buf) {
    const char *n = r == R_SCHED ? "sched" : r == R_KTHREAD ? "kthread" :
                    r == R_KERNEL ? "kernel" : 0;
    if (n) { int i = 0; while ((buf[i] = n[i])) i++; return; }
    const char *pfx = r < R_VEC ? "sys" : "vec";
    unsigned v = (unsigned)(r < R_VEC ? r : r - R_VEC);
    int i = 0;
    while (pfx[i]) { buf[i] = pfx[i]; i++; }
    buf[i++] = ':';
    char t[12]; int k = 0;
    do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (k) buf[i++] = t[--k];
    buf[i] = 0;
}

static unsigned bst_us(uint64_t c) {
    uint64_t u = c / bst_cyc_per_us();
    return u > 0xFFFFFFFFULL ? 0xFFFFFFFFU : (unsigned)u;
}

/* Permille of `part` in `whole`. */
static unsigned bst_pm(uint64_t part, uint64_t whole) {
    if (!whole) return 0;
    while (whole > 0xFFFFFFFFULL / 1000U) { whole >>= 1; part >>= 1; }
    return (unsigned)(part * 1000U / whole);
}

static void bst_top(const char *what, uint64_t (*v)(int), uint64_t total, int n) {
    static uint8_t used[NREASON];
    for (int i = 0; i < NREASON; i++) used[i] = 0;
    for (int k = 0; k < n; k++) {
        int best = -1;
        uint64_t bv = 0;
        for (int i = 0; i < NREASON; i++)
            if (!used[i] && v(i) > bv) { bv = v(i); best = i; }
        if (best < 0) break;
        used[best] = 1;
        unsigned cnt = 0;
        for (int c = 0; c < MAX_CPUS; c++) cnt += g_cnt[c].hold_n[best];
        char nm[16];
        bst_name(best, nm);
        unsigned pm = bst_pm(bv, total);
        printk("[bklstat] %s %s us=%u pm=%u.%u n=%u\n", what, nm, bst_us(bv),
               pm / 10, pm % 10, cnt);
    }
}

static uint64_t v_hold(int r) { uint64_t s = 0; for (int c = 0; c < MAX_CPUS; c++) s += g_cnt[c].hold_by[r]; return s; }
static uint64_t v_on(int r)   { uint64_t s = 0; for (int c = 0; c < MAX_CPUS; c++) s += g_cnt[c].spin_on[r]; return s; }
static uint64_t v_for(int r)  { uint64_t s = 0; for (int c = 0; c < MAX_CPUS; c++) s += g_cnt[c].spin_for[r]; return s; }

/* Print the counters since the last reset.  pm = permille. */
void bklstat_dump(void) {
    uint64_t el = bst_rdtsc() - g_t0;
    uint64_t th = 0, ts = 0;
    unsigned ta = 0, tc = 0, tb = 0, hh[NHIST] = {0}, sh[NHIST] = {0};
    /* What one this_cpu_id() costs right now (a Local APIC read once the
     * APs run; every current_proc reference pays it). */
    uint64_t t0 = bst_rdtsc();
    volatile unsigned sink = 0;
    for (int i = 0; i < 1000; i++) sink += this_cpu_id();
    (void)sink;
    unsigned id_ns = (unsigned)((bst_rdtsc() - t0) / bst_cyc_per_us());
    printk("[bklstat] begin elapsed_us=%u cyc_per_us=%u this_cpu_id_ns=%u\n", bst_us(el),
           bst_cyc_per_us(), id_ns);
    for (int c = 0; c < MAX_CPUS; c++) {
        struct bkl_cnt *k = &g_cnt[c];
        if (!k->acq) continue;
        unsigned ph = bst_pm(k->hold, el), ps = bst_pm(k->spin, el);
        printk("[bklstat] cpu%d acq=%u contended=%u nested=%u hold_us=%u (%u.%u%%) "
               "spin_us=%u (%u.%u%%) max_hold_us=%u max_spin_us=%u bad_switch=%u\n",
               c, k->acq, k->contended, k->nested, bst_us(k->hold), ph / 10, ph % 10,
               bst_us(k->spin), ps / 10, ps % 10, bst_us(k->max_hold),
               bst_us(k->max_spin), k->bad_switch);
        th += k->hold; ts += k->spin; ta += k->acq; tc += k->contended; tb += k->bad_switch;
        for (int i = 0; i < NHIST; i++) { hh[i] += k->hhist[i]; sh[i] += k->shist[i]; }
    }
    unsigned pb = bst_pm(th, el);
    printk("[bklstat] total acq=%u contended=%u busy=%u.%u%% spin_us=%u hold_us=%u bad_switch=%u\n",
           ta, tc, pb / 10, pb % 10, bst_us(ts), bst_us(th), tb);
    for (int i = 0; i < NHIST; i++)
        if (hh[i] || sh[i])
            printk("[bklstat] hist le_us=%u hold=%u spin=%u\n", 1U << i, hh[i], sh[i]);
    bst_top("hold", v_hold, th, 20);
    bst_top("blocked_by", v_on, ts, 15);
    bst_top("waiter", v_for, ts, 15);
    printk("[bklstat] end\n");
}
#endif
