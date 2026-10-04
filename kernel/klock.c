#include <kernel/klock.h>
#include "printk.h"
#include "panic.h"
#include "../arch/i686/cpu/percpu.h"
#include "../proc/process.h"
#include "../proc/scheduler.h"
#include <kernel/config.h>

/*
 * Lock primitives and the lock-order checker; see include/kernel/klock.h and
 * docs/smp-plan.md (stage 0).  The spinlock and mutex scheme follows xv6's
 * acquire/release and sleep(chan, lk) (MIT, https://github.com/mit-pdos/xv6-riscv)
 * in design only; the checker is a much reduced form of the idea behind
 * Linux lockdep and FreeBSD WITNESS: a graph of "taken while holding" edges
 * between lock classes, checked for a cycle on every new edge.
 */

static inline uint32_t irq_save(void) {
    uint32_t fl;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(fl) :: "memory");
    return fl;
}

static inline void irq_restore(uint32_t fl) {
    if (fl & 0x200) __asm__ volatile("sti" ::: "memory");
}

/* Test-and-test-and-set: spin on a plain read so the waiters do not bounce
 * the line with locked writes, and serve TLB shootdowns meanwhile — a CPU
 * spinning with interrupts off cannot take the shootdown IPI (percpu.h). */
static inline void raw_acquire(spinlock_t *r) {
    for (;;) {
        if (!r->locked && spin_trylock(r)) return;
        tlb_serve_pending();
        __asm__ volatile("pause");
    }
}

/* ── lock-order checker (KLOCKDEP) ─────────────────────────────────────── */
#if KLOCKDEP
#include "../lib/string.h"

#define KLD_CLASSES 64
#define KLD_DEPTH   8           /* spinlocks held at once on one CPU        */
#define KLD_MDEPTH  4           /* kmutexes held at once by one thread      */
#define F_IN_IRQ    1           /* taken inside a hardware interrupt handler */
#define F_IRQ_ON    2           /* taken with interrupts enabled            */

static spinlock_t  g_kld;                       /* the checker's own state */

/* g_kld is taken from any context, interrupt handlers included (a handler's
 * first kspin_lock_irqsave registers its class), so every holder keeps
 * interrupts off: an interrupt on the holding CPU would otherwise spin on it
 * forever.  The spin serves TLB shootdowns like every IF=0 spin. */
static inline uint32_t kld_lock(void) {
    uint32_t fl = irq_save();
    raw_acquire(&g_kld);
    return fl;
}

static inline void kld_unlock(uint32_t fl) {
    spin_unlock(&g_kld);
    irq_restore(fl);
}
static const char *g_cls_name[KLD_CLASSES];
static unsigned    g_ncls;
static uint64_t    g_after[KLD_CLASSES];        /* bit b: b taken under this */
static uint64_t    g_reported[KLD_CLASSES];     /* (a,b) pairs reported      */
static uint8_t     g_flags[KLD_CLASSES];
static uint8_t     g_flag_reported[KLD_CLASSES];
static volatile unsigned g_reports;

struct kld_cpu { uint8_t held[KLD_DEPTH]; uint8_t n; };
static struct kld_cpu g_cpu_held[MAX_CPUS];
static uint8_t g_thr_held[MAX_PROCS][KLD_MDEPTH];   /* kmutex classes + 1   */

static int name_eq(const char *a, const char *b) {
    if (a == b) return 1;
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* Class index for a lock name, registered on first use. -1: table full. */
static int kld_class(uint16_t *slot, const char *name) {
    if (*slot) return *slot - 1;
    int c = -1;
    uint32_t fl = kld_lock();
    for (unsigned i = 0; i < g_ncls; i++)
        if (name_eq(g_cls_name[i], name)) { c = (int)i; break; }
    if (c < 0 && g_ncls < KLD_CLASSES) {
        c = (int)g_ncls;
        g_cls_name[g_ncls++] = name;
    }
    kld_unlock(fl);
    if (c >= 0) *slot = (uint16_t)(c + 1);
    return c;
}

static void kld_report(const char *what, const char *a, const char *b) {
    __sync_add_and_fetch(&g_reports, 1);
    if (b)
        printk("[lockdep] %s: '%s' and '%s' (cpu %u, pid %d)\n", what, a, b,
               (unsigned)this_cpu_id(), current_proc ? current_proc->pid : -1);
    else
        printk("[lockdep] %s: '%s' (cpu %u, pid %d)\n", what, a,
               (unsigned)this_cpu_id(), current_proc ? current_proc->pid : -1);
}

static int lowbit(uint64_t v) {
    uint32_t lo = (uint32_t)v;
    return lo ? __builtin_ctz(lo) : 32 + __builtin_ctz((uint32_t)(v >> 32));
}

/* Is `to` reachable from `from` along recorded edges?  Caller holds g_kld. */
static int kld_reaches(int from, int to) {
    uint64_t seen = 0, front = 1ULL << from;
    while (front) {
        int i = lowbit(front);
        front &= front - 1;
        if (seen & (1ULL << i)) continue;
        seen |= 1ULL << i;
        front |= g_after[i] & ~seen;
    }
    return (seen >> to) & 1;
}

/* Record that class b is being taken while class a is held. */
static void kld_edge(int a, int b) {
    if (a == b) {
        kld_report("recursive acquisition of lock class", g_cls_name[a], 0);
        return;
    }
    int inverted = 0;
    uint32_t fl = kld_lock();
    if (!((g_after[a] >> b) & 1)) {
        if (kld_reaches(b, a) && !((g_reported[a] >> b) & 1)) {
            g_reported[a] |= 1ULL << b;
            inverted = 1;
        }
        g_after[a] |= 1ULL << b;
    }
    kld_unlock(fl);
    if (inverted)
        kld_report("lock order inversion (taken in both orders)", g_cls_name[a], g_cls_name[b]);
}

/* The flags are shared by all CPUs: update them under g_kld so two CPUs
 * setting different bits cannot lose one, and report outside it. */
static void kld_flag(int c, uint8_t f) {
    int report = 0;
    uint32_t fl = kld_lock();
    uint8_t nf = (uint8_t)(g_flags[c] | f);
    g_flags[c] = nf;
    if ((nf & (F_IN_IRQ | F_IRQ_ON)) == (F_IN_IRQ | F_IRQ_ON) && !g_flag_reported[c]) {
        g_flag_reported[c] = 1;
        report = 1;
    }
    kld_unlock(fl);
    if (report) {
        kld_report("lock taken in an interrupt handler and with interrupts enabled",
                   g_cls_name[c], 0);
    }
}

static int thr_slot(void) {
    struct proc *p = current_proc;
    return p ? (int)(p - ptable) : -1;
}

/* Every class this context holds, for the edge checks. */
static void kld_acquire(int c, uint32_t caller_flags, int is_mutex) {
    uint32_t fl = irq_save();
    uint32_t id = this_cpu_id();
    struct kld_cpu *h = &g_cpu_held[id];
    int s = thr_slot();
    if (s >= 0)
        for (int i = 0; i < KLD_MDEPTH; i++)
            if (g_thr_held[s][i]) kld_edge(g_thr_held[s][i] - 1, c);
    for (int i = 0; i < h->n; i++) kld_edge(h->held[i], c);
    if (cpus[id].in_irq) kld_flag(c, F_IN_IRQ);
    else if (caller_flags & 0x200) kld_flag(c, F_IRQ_ON);
    if (is_mutex) {
        if (cpus[id].in_irq) kld_report("kmutex taken in an interrupt handler", g_cls_name[c], 0);
        if (h->n) kld_report("kmutex taken with a spinlock held", g_cls_name[h->held[h->n - 1]],
                             g_cls_name[c]);
        int depth = 0;
        bkl_state(0, &depth);
        if (!depth) kld_report("kmutex taken without the BKL (needed until stage 4)",
                               g_cls_name[c], 0);
    } else if (h->n < KLD_DEPTH) {
        h->held[h->n++] = (uint8_t)c;
    }
    irq_restore(fl);
}

static void kld_release_spin(int c) {
    uint32_t fl = irq_save();
    struct kld_cpu *h = &g_cpu_held[this_cpu_id()];
    for (int i = h->n - 1; i >= 0; i--)
        if (h->held[i] == c) {
            for (int j = i; j + 1 < h->n; j++) h->held[j] = h->held[j + 1];
            h->n--;
            break;
        }
    irq_restore(fl);
}

static void kld_mutex_owned(int c, int add) {
    int s = thr_slot();
    if (s < 0) return;
    for (int i = 0; i < KLD_MDEPTH; i++) {
        if (add && !g_thr_held[s][i]) { g_thr_held[s][i] = (uint8_t)(c + 1); return; }
        if (!add && g_thr_held[s][i] == c + 1) { g_thr_held[s][i] = 0; return; }
    }
}

void klock_might_sleep(const char *what) {
    uint32_t fl = irq_save();
    struct kld_cpu *h = &g_cpu_held[this_cpu_id()];
    if (h->n) kld_report(what, g_cls_name[h->held[h->n - 1]], "sleeps with this spinlock held");
    irq_restore(fl);
}

unsigned klockdep_reports(void) { return g_reports; }

static void spin_pre(kspinlock_t *l, uint32_t caller_flags) {
    int c = kld_class(&l->cls, l->name ? l->name : "?");
    uint32_t me = this_cpu_id() + 1;
    if (l->owner == me) {
        kld_report("spinlock taken twice on one CPU (deadlock)", l->name, 0);
        panic("lockdep: recursive spinlock", 0);
    }
    if (c >= 0) kld_acquire(c, caller_flags, 0);
}

static void spin_post(kspinlock_t *l) { l->owner = (uint16_t)(this_cpu_id() + 1); }

static void spin_unlock_check(kspinlock_t *l) {
    if (l->owner != this_cpu_id() + 1)
        kld_report("spinlock released by a CPU that does not hold it", l->name, 0);
    l->owner = 0;
    if (l->cls) kld_release_spin(l->cls - 1);
}
#endif /* KLOCKDEP */

/* ── spinlocks ──────────────────────────────────────────────────────────── */

void kspin_init(kspinlock_t *l, const char *name) {
    l->raw.locked = 0;
    l->name = name;
#if KLOCKDEP
    l->cls = 0;
    l->owner = 0;
#endif
}

uint32_t kspin_lock_irqsave(kspinlock_t *l) {
    uint32_t fl = irq_save();
#if KLOCKDEP
    /* Held with interrupts off whatever the caller had: never "taken with
     * interrupts enabled", so an irqsave lock may be shared with handlers. */
    spin_pre(l, 0);
#endif
    raw_acquire(&l->raw);
#if KLOCKDEP
    spin_post(l);
#endif
    return fl;
}

void kspin_unlock_irqrestore(kspinlock_t *l, uint32_t flags) {
#if KLOCKDEP
    spin_unlock_check(l);
#endif
    spin_unlock(&l->raw);
    irq_restore(flags);
}

int kspin_trylock_irqsave(kspinlock_t *l, uint32_t *flags) {
    uint32_t fl = irq_save();
    if (l->raw.locked || !spin_trylock(&l->raw)) {
        irq_restore(fl);
        return 0;
    }
#if KLOCKDEP
    /* Checked after the fact: a trylock cannot deadlock, but its order
     * still constrains every blocking acquisition. */
    if (l->owner == this_cpu_id() + 1)
        kld_report("spinlock taken twice on one CPU (trylock)", l->name, 0);
    int c = kld_class(&l->cls, l->name ? l->name : "?");
    if (c >= 0) kld_acquire(c, 0, 0);
    spin_post(l);
#endif
    *flags = fl;
    return 1;
}

void kspin_lock(kspinlock_t *l) {
#if KLOCKDEP
    uint32_t fl;
    __asm__ volatile("pushf; pop %0" : "=r"(fl));
    spin_pre(l, fl);
#endif
    raw_acquire(&l->raw);
#if KLOCKDEP
    spin_post(l);
#endif
}

void kspin_unlock(kspinlock_t *l) {
#if KLOCKDEP
    spin_unlock_check(l);
#endif
    spin_unlock(&l->raw);
}

int kspin_held(const kspinlock_t *l) { return l->raw.locked != 0; }

/* ── sleeping mutexes ───────────────────────────────────────────────────── */

void kmutex_init(kmutex_t *m, const char *name) {
    kspin_init(&m->guard, "kmutex.guard");
    m->owner = 0;
    m->waiters = 0;
    m->name = name;
#if KLOCKDEP
    m->cls = 0;
#endif
}

/* Before any scheduler runs there is no thread to own the lock; boot code
 * that takes a kmutex is recorded under this placeholder. */
#define BOOT_OWNER ((struct proc *)1)

void kmutex_lock(kmutex_t *m) {
#if KLOCKDEP
    int c = kld_class(&m->cls, m->name ? m->name : "?");
    if (c >= 0) {
        uint32_t fl;
        __asm__ volatile("pushf; pop %0" : "=r"(fl));
        kld_acquire(c, fl, 1);
    }
    klock_might_sleep("kmutex_lock");
    if (current_proc && m->owner == current_proc)
        kld_report("kmutex taken twice by one thread (deadlock)", m->name, 0);
#endif
    struct proc *me = current_proc ? current_proc : BOOT_OWNER;
    uint32_t fl = kspin_lock_irqsave(&m->guard);
    while (m->owner) {
        if (me == BOOT_OWNER) {
            kspin_unlock_irqrestore(&m->guard, fl);
            __asm__ volatile("pause");
            fl = kspin_lock_irqsave(&m->guard);
            continue;
        }
        m->waiters++;
        /* The guard is released only once this thread is asleep on m, and
         * kmutex_unlock wakes under the guard, so its wake cannot fall
         * between the owner test and the sleep (stage 2a). */
        sleep_locked(m, &m->guard);
        m->waiters--;
    }
    m->owner = me;
    kspin_unlock_irqrestore(&m->guard, fl);
#if KLOCKDEP
    if (c >= 0) kld_mutex_owned(c, 1);
#endif
}

int kmutex_trylock(kmutex_t *m) {
    struct proc *me = current_proc ? current_proc : BOOT_OWNER;
    uint32_t fl = kspin_lock_irqsave(&m->guard);
    int got = !m->owner;
    if (got) m->owner = me;
    kspin_unlock_irqrestore(&m->guard, fl);
#if KLOCKDEP
    if (got) {
        int c = kld_class(&m->cls, m->name ? m->name : "?");
        if (c >= 0) { kld_acquire(c, fl, 1); kld_mutex_owned(c, 1); }
    }
#endif
    return got;
}

void kmutex_unlock(kmutex_t *m) {
    struct proc *me = current_proc ? current_proc : BOOT_OWNER;
#if KLOCKDEP
    if (m->owner != me) kld_report("kmutex released by a thread that does not own it", m->name, 0);
    if (m->cls) kld_mutex_owned(m->cls - 1, 0);
#endif
    (void)me;
    uint32_t fl = kspin_lock_irqsave(&m->guard);
    m->owner = 0;
    if (m->waiters) wake_up(m);         /* under the guard: see kmutex_lock */
    kspin_unlock_irqrestore(&m->guard, fl);
}

int kmutex_owned(const kmutex_t *m) {
    return m->owner && m->owner == (current_proc ? current_proc : BOOT_OWNER);
}

/* ── torture test (`make KLOCK_TEST=1`) ─────────────────────────────────── */
#if defined(KLOCK_TEST) && KLOCK_TEST

#define KT_THREADS   4
#define KT_ITERS     200000
#define KT_MUTEX_EVERY 512
/* Stage 2 additions (docs/smp-plan.md): every KT_MEM_EVERY iterations a
 * spin thread allocates and frees heap blocks and frames (filled through its
 * CPU's temp-map slot) outside the BKL; every KT_SD_EVERY it remaps its test
 * page and shoots it down while the other threads read every test page; and
 * two more threads ping-pong a token KT_PP_ROUNDS times through wait queues
 * (sleep_locked), with a deadline on each sleep to catch a lost wakeup. */
#define KT_MEM_EVERY 64
#define KT_SD_EVERY  256
#define KT_SD_READ_EVERY 16
#define KT_PP_ROUNDS 20000
#define KT_ALL       (KT_THREADS + 2)

static kspinlock_t kt_a = KSPINLOCK_INIT("klocktest.a");
static kspinlock_t kt_b = KSPINLOCK_INIT("klocktest.b");
static kspinlock_t kt_c = KSPINLOCK_INIT("klocktest.c");
static kspinlock_t kt_d = KSPINLOCK_INIT("klocktest.d");
static kmutex_t    kt_m = KMUTEX_INIT("klocktest.m");

/* x1/x2 and y move together under kt_a (and kt_b); m1/m2 under kt_m.  A
 * reader that ever sees them apart caught two CPUs inside at once. */
static volatile unsigned kt_x1, kt_x2, kt_y, kt_m1, kt_m2;
static volatile int kt_started, kt_done, kt_inside, kt_running, kt_max_running, kt_fail;
static volatile unsigned kt_cpu_mask, kt_contended, kt_spins, kt_mutexes;

static inline uint64_t kt_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static void kt_failf(const char *what, unsigned a, unsigned b) {
    if (__sync_add_and_fetch(&kt_fail, 1) == 1)
        printk("[KLOCK-TEST] FAIL %s (%u vs %u) on cpu %u\n", what, a, b,
               (unsigned)this_cpu_id());
}

/* ── stage 2: memory, shootdowns, sleep/wakeup ── */
static void kt_finish(void) __attribute__((noreturn));
#include "../mm/heap.h"
#include "../mm/pmm.h"
#include "../mm/kstack.h"
#include "../arch/i686/mm/paging.h"
#include "../arch/i686/mm/tlb.h"
#include "../arch/i686/cpu/pit.h"

/* Test pages for the shootdown check: the upper half of the KMAP window,
 * which the per-CPU slots (lower 16 pages) never reach. */
#define KT_SD_VA(i) ((uint32_t)KMAP_WINDOW_START + (256U + (uint32_t)(i)) * PAGE_SIZE)
static uint32_t kt_sd_frame[KT_THREADS][2];
static volatile uint32_t kt_sd_pub[KT_THREADS];   /* generation every CPU must see */
static volatile unsigned kt_sd_rounds, kt_sd_checks, kt_mem_rounds, kt_kstacks;

static void kt_frame_fill(uint32_t f, uint32_t v) {
    uint32_t *k = (uint32_t *)paging_temp_map2(f);
    for (int i = 0; i < 1024; i++) k[i] = v ^ (uint32_t)i;
    paging_temp_unmap2();
}

static int kt_frame_check(uint32_t f, uint32_t v) {
    uint32_t *k = (uint32_t *)paging_temp_map(f);
    int ok = 1;
    for (int i = 0; i < 1024; i++) if (k[i] != (v ^ (uint32_t)i)) { ok = 0; break; }
    paging_temp_unmap();
    return ok;
}

struct kt_mem {
    uint32_t *blk[8];
    uint32_t  words[8], tag[8];
    uint32_t  frame[4], ftag[4];
    uint32_t  seed;
};

static uint32_t kt_rand(uint32_t *s) { *s = *s * 1103515245U + 12345U; return *s >> 8; }

/* One allocation round, BKL not held: replace one heap block and one frame,
 * checking that what was written into the old ones is still there (two
 * allocators handing out the same memory would overwrite each other). */
static void kt_mem_round(struct kt_mem *m, unsigned id) {
    unsigned b = kt_rand(&m->seed) & 7;
    if (m->blk[b]) {
        for (uint32_t w = 0; w < m->words[b]; w++)
            if (m->blk[b][w] != (m->tag[b] ^ w)) {
                kt_failf("heap block overwritten (two CPUs got one block)", w, id);
                break;
            }
        kfree(m->blk[b]);
    }
    uint32_t words = 4 + kt_rand(&m->seed) % 1000;
    m->blk[b] = (uint32_t *)kmalloc(words * 4);
    m->words[b] = words;
    m->tag[b] = kt_rand(&m->seed) ^ (id << 28);
    if (m->blk[b]) for (uint32_t w = 0; w < words; w++) m->blk[b][w] = m->tag[b] ^ w;

    unsigned f = kt_rand(&m->seed) & 3;
    if (m->frame[f]) {
        if (!kt_frame_check(m->frame[f], m->ftag[f]))
            kt_failf("frame overwritten (two CPUs got one frame)", m->frame[f], id);
        pmm_free_frame(m->frame[f]);
    }
    m->frame[f] = pmm_alloc_frame();
    m->ftag[f] = kt_rand(&m->seed) ^ (id << 28);
    if (m->frame[f]) kt_frame_fill(m->frame[f], m->ftag[f]);

    /* A kernel stack now and then (kstack_lock; reusing freed slots sends a
     * shootdown from whichever CPU runs out first). */
    if ((kt_rand(&m->seed) & 7) == 0) {
        uint32_t *ks = (uint32_t *)kstack_alloc();
        if (ks) {
            uint32_t words = KSTACKSIZE / 4, tag = kt_rand(&m->seed);
            ks[0] = tag; ks[words / 2] = tag + 1; ks[words - 1] = tag + 2;
            if (ks[0] != tag || ks[words / 2] != tag + 1 || ks[words - 1] != tag + 2)
                kt_failf("kernel stack pages shared", ks[0], tag);
            kstack_free(ks);
            __sync_add_and_fetch(&kt_kstacks, 1);
        }
    }
    __sync_add_and_fetch(&kt_mem_rounds, 1);
}

static void kt_mem_drain(struct kt_mem *m) {
    for (int b = 0; b < 8; b++) if (m->blk[b]) kfree(m->blk[b]);
    for (int f = 0; f < 4; f++) if (m->frame[f]) pmm_free_frame(m->frame[f]);
}

/* Remap this thread's test page to the frame holding the next generation and
 * shoot it down; only then publish the generation.  Several threads do this
 * at once, so the shootdown has several concurrent senders. */
static void kt_sd_round(unsigned id) {
    uint32_t g = kt_sd_pub[id] + 1;
    uint32_t f = kt_sd_frame[id][g & 1];
    uint32_t *k = (uint32_t *)paging_temp_map2(f);
    k[0] = g;
    paging_temp_unmap2();
    pte_set(KT_SD_VA(id), f | PAGE_PRESENT | PAGE_WRITABLE | PAGE_NX);
    tlb_flush_single(KT_SD_VA(id));
    tlb_shootdown();
    __sync_synchronize();
    kt_sd_pub[id] = g;
    __sync_add_and_fetch(&kt_sd_rounds, 1);
}

/* Every test page must read at least the generation published for it: a
 * smaller value is a translation the shootdown should have flushed. */
static void kt_sd_check(void) {
    for (unsigned j = 0; j < KT_THREADS; j++) {
        uint32_t pub = kt_sd_pub[j];
        __sync_synchronize();
        uint32_t v = *(volatile uint32_t *)KT_SD_VA(j);
        if ((int32_t)(v - pub) < 0) kt_failf("stale TLB entry after a shootdown", v, pub);
    }
    __sync_add_and_fetch(&kt_sd_checks, 1);
}

static void kt_sd_setup(void) {
    for (unsigned i = 0; i < KT_THREADS; i++) {
        for (int k = 0; k < 2; k++) {
            kt_sd_frame[i][k] = pmm_alloc_frame();
            uint32_t *p = (uint32_t *)paging_temp_map(kt_sd_frame[i][k]);
            p[0] = 0;
            paging_temp_unmap();
        }
        pte_set(KT_SD_VA(i), kt_sd_frame[i][0] | PAGE_PRESENT | PAGE_WRITABLE | PAGE_NX);
        tlb_flush_single(KT_SD_VA(i));
        kt_sd_pub[i] = 0;
    }
}

/* Ping-pong: the token says whose turn it is; each side waits on its own
 * queue, under the shared lock, for its turn, then hands the turn over and
 * wakes the other side under the lock (sleep_locked's rule).  A sleep that
 * times out while the turn is already ours is a lost wakeup. */
static kspinlock_t kt_pp_lock = KSPINLOCK_INIT("klocktest.pp");
static volatile int kt_pp_turn;
static volatile unsigned kt_pp_done[2], kt_pp_lost, kt_pp_sleeps;
static int kt_pp_chan[2];

static void kt_pp_side(int me) {
    __sync_add_and_fetch(&kt_started, 1);
    for (unsigned r = 0; r < KT_PP_ROUNDS && !kt_fail; r++) {
        uint32_t fl = kspin_lock_irqsave(&kt_pp_lock);
        while (kt_pp_turn != me && !kt_fail) {
            current_proc->wake_tick = pit_ticks() + 300;     /* 3 s */
            kt_pp_sleeps++;
            int timed_out = sleep_locked(&kt_pp_chan[me], &kt_pp_lock);
            if (timed_out && kt_pp_turn == me) {
                kt_pp_lost++;
                kt_failf("sleep_locked lost a wakeup", r, (unsigned)me);
            } else if (timed_out) {
                kt_failf("ping-pong partner stalled for 3 s", r, (unsigned)me);
            }
        }
        kt_pp_turn = !me;
        wake_up(&kt_pp_chan[!me]);
        kspin_unlock_irqrestore(&kt_pp_lock, fl);
        kt_pp_done[me]++;
    }
    /* Let a partner that is still waiting see the failure flag and leave. */
    uint32_t fl = kspin_lock_irqsave(&kt_pp_lock);
    wake_up(&kt_pp_chan[!me]);
    kspin_unlock_irqrestore(&kt_pp_lock, fl);
    kt_finish();
}

static void kt_pp0(void) { kt_pp_side(0); }
static void kt_pp1(void) { kt_pp_side(1); }

static void kt_report(void) {
    unsigned before = klockdep_reports();
    /* The deliberate inversion: c then d, later d then c, on one thread (so
     * it cannot actually deadlock).  The checker must flag it exactly once. */
    uint32_t f1 = kspin_lock_irqsave(&kt_c), f2 = kspin_lock_irqsave(&kt_d);
    kspin_unlock_irqrestore(&kt_d, f2);
    kspin_unlock_irqrestore(&kt_c, f1);
    f1 = kspin_lock_irqsave(&kt_d);
    f2 = kspin_lock_irqsave(&kt_c);
    kspin_unlock_irqrestore(&kt_c, f2);
    kspin_unlock_irqrestore(&kt_d, f1);
    unsigned caught = klockdep_reports() - before;
    unsigned cpus_seen = (unsigned)__builtin_popcount(kt_cpu_mask);

    if (before) kt_failf("lockdep reported the consistent a->b order", before, 0);
    if (caught != 1) kt_failf("lockdep missed the deliberate c/d inversion", caught, 1);
    if (kt_x1 != kt_x2 || kt_x1 != (unsigned)KT_THREADS * KT_ITERS)
        kt_failf("spinlock lost an update", kt_x1, kt_x2);
    if (kt_m1 != kt_m2 || kt_m1 != kt_mutexes) kt_failf("kmutex lost an update", kt_m1, kt_m2);
    if (kt_pp_done[0] != KT_PP_ROUNDS || kt_pp_done[1] != KT_PP_ROUNDS)
        kt_failf("ping-pong rounds missing", kt_pp_done[0], kt_pp_done[1]);
    if (kt_sd_rounds != (unsigned)KT_THREADS * (KT_ITERS / KT_SD_EVERY) && !kt_fail)
        kt_failf("shootdown rounds missing", kt_sd_rounds, 0);
    printk("[KLOCK-TEST] stage2 pingpong=%u+%u sleeps=%u lost_wakeups=%u shootdowns=%u "
           "tlb_checks=%u mem_rounds=%u kstacks=%u\n", kt_pp_done[0], kt_pp_done[1],
           kt_pp_sleeps, kt_pp_lost, kt_sd_rounds, kt_sd_checks, kt_mem_rounds, kt_kstacks);
    printk("[KLOCK-TEST] threads=%d cpus=%u max_parallel=%d spin_iters=%u contended=%u "
           "mutex_iters=%u lockdep_inversion_caught=%u\n", KT_THREADS, cpus_seen,
           kt_max_running, kt_spins, kt_contended, kt_mutexes, caught);
    if (!kt_fail) printk("[KLOCK-TEST] PASS\n");
}

static void kt_finish(void) {
    if (__sync_add_and_fetch(&kt_done, 1) == KT_ALL) kt_report();
    for (;;) sleep_on((void *)&kt_done);    /* never woken: park for good */
}

static volatile unsigned kt_next_id;

static void kt_thread(void) {
    unsigned id = __sync_fetch_and_add(&kt_next_id, 1);
    struct kt_mem mem = { .seed = 0x9E3779B9U * (id + 1) };
    __sync_add_and_fetch(&kt_started, 1);
    /* Step out from under the Big Kernel Lock (kernel threads run holding
     * it), so this CPU and the others really run the loop at once. */
    bkl_leave();
    uint64_t t0 = kt_rdtsc();
    while (kt_started < KT_ALL && kt_rdtsc() - t0 < 3000000000ULL)
        __asm__ volatile("pause");
    __sync_fetch_and_or(&kt_cpu_mask, 1U << this_cpu_id());
    int r = __sync_add_and_fetch(&kt_running, 1);
    for (int m = kt_max_running; r > m; m = kt_max_running)
        if (__sync_bool_compare_and_swap(&kt_max_running, m, r)) break;

    for (unsigned i = 0; i < KT_ITERS && !kt_fail; i++) {
        uint32_t f;
        if (!kspin_trylock_irqsave(&kt_a, &f)) {
            __sync_add_and_fetch(&kt_contended, 1);
            f = kspin_lock_irqsave(&kt_a);
        }
        int in = __sync_add_and_fetch(&kt_inside, 1);
        if (in != 1) kt_failf("two CPUs inside klocktest.a", (unsigned)in, 1);
        unsigned v = kt_x1;
        kt_x1 = v + 1;
        for (volatile int d = 0; d < (int)(i & 7); d++) ;
        kt_x2 = kt_x2 + 1;
        if (kt_x1 != kt_x2) kt_failf("klocktest.a pair torn", kt_x1, kt_x2);
        if ((i & 63) == 0) {
            uint32_t g = kspin_lock_irqsave(&kt_b);      /* always a, then b */
            kt_y++;
            kspin_unlock_irqrestore(&kt_b, g);
        }
        kt_spins++;
        __sync_sub_and_fetch(&kt_inside, 1);
        kspin_unlock_irqrestore(&kt_a, f);

        if (i % KT_MEM_EVERY == 0) kt_mem_round(&mem, id);
        if (i % KT_SD_EVERY == KT_SD_EVERY - 1) kt_sd_round(id);
        if (i % KT_SD_READ_EVERY == 0) kt_sd_check();

        if (i % KT_MUTEX_EVERY == 0) {
            /* Sleeping locks need the BKL until the scheduler stage. */
            bkl_enter();
            kmutex_lock(&kt_m);
            unsigned w = kt_m1;
            kt_m1 = w + 1;
            yield();                    /* sleep-ish while holding it */
            kt_m2 = kt_m2 + 1;
            if (kt_m1 != kt_m2) kt_failf("klocktest.m pair torn", kt_m1, kt_m2);
            kt_mutexes++;
            kmutex_unlock(&kt_m);
            bkl_leave();
        }
    }
    kt_mem_drain(&mem);
    __sync_sub_and_fetch(&kt_running, 1);
    bkl_enter();                        /* back to how kernel threads run */
    kt_finish();
}

void klock_test_start(void) {
    printk("[KLOCK-TEST] starting %d threads x %d iterations\n", KT_THREADS, KT_ITERS);
    kt_sd_setup();
    for (int i = 0; i < KT_THREADS; i++)
        proc_create_kthread(kt_thread, "klocktest");
    proc_create_kthread(kt_pp0, "klocktest-pp");
    proc_create_kthread(kt_pp1, "klocktest-pp");
}
#endif /* KLOCK_TEST */
