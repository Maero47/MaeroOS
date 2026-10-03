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
    spin_pre(l, fl);
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
    if (c >= 0) kld_acquire(c, fl, 0);
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
        /* Interrupts stay off from the guard's release to the switch inside
         * sleep_on, and the BKL keeps every other CPU's waker out until then,
         * so the wake in kmutex_unlock cannot fall between the two. */
        kspin_unlock(&m->guard);
        sleep_on(m);
        kspin_lock_irqsave(&m->guard);
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
    int w = m->waiters;
    kspin_unlock_irqrestore(&m->guard, fl);
    if (w) wake_up(m);
}

int kmutex_owned(const kmutex_t *m) {
    return m->owner && m->owner == (current_proc ? current_proc : BOOT_OWNER);
}

/* ── torture test (`make KLOCK_TEST=1`) ─────────────────────────────────── */
#if defined(KLOCK_TEST) && KLOCK_TEST

#define KT_THREADS   4
#define KT_ITERS     200000
#define KT_MUTEX_EVERY 512

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
    printk("[KLOCK-TEST] threads=%d cpus=%u max_parallel=%d spin_iters=%u contended=%u "
           "mutex_iters=%u lockdep_inversion_caught=%u\n", KT_THREADS, cpus_seen,
           kt_max_running, kt_spins, kt_contended, kt_mutexes, caught);
    if (!kt_fail) printk("[KLOCK-TEST] PASS\n");
}

static void kt_thread(void) {
    __sync_add_and_fetch(&kt_started, 1);
    /* Step out from under the Big Kernel Lock (kernel threads run holding
     * it), so this CPU and the others really run the loop at once. */
    bkl_leave();
    uint64_t t0 = kt_rdtsc();
    while (kt_started < KT_THREADS && kt_rdtsc() - t0 < 3000000000ULL)
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
    __sync_sub_and_fetch(&kt_running, 1);
    bkl_enter();                        /* back to how kernel threads run */
    if (__sync_add_and_fetch(&kt_done, 1) == KT_THREADS) kt_report();
    for (;;) sleep_on((void *)&kt_done);    /* never woken: park for good */
}

void klock_test_start(void) {
    printk("[KLOCK-TEST] starting %d threads x %d iterations\n", KT_THREADS, KT_ITERS);
    for (int i = 0; i < KT_THREADS; i++)
        proc_create_kthread(kt_thread, "klocktest");
}
#endif /* KLOCK_TEST */
