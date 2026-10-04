#pragma once
/*
 * Kernel lock primitives for splitting the Big Kernel Lock (docs/smp-plan.md,
 * stage 0).  Stage 2 put the first users on them (the frame allocator, the
 * kernel heap, pipes, kmutex sleeps); the BKL still serialises every other
 * kernel path.  They exist so that each later stage moves one subsystem
 * onto a lock of its own without inventing the lock at the same time.
 *
 *   kspinlock_t   spinning lock.  kspin_lock_irqsave() is the default: it
 *                 disables interrupts on this CPU first, so it may be taken
 *                 from an interrupt handler too.  kspin_lock() leaves IF
 *                 alone and is only for locks no interrupt handler takes.
 *                 Never held across a sleep, a yield or a context switch.
 *                 Not recursive.  While spinning it serves TLB shootdowns,
 *                 because it spins with interrupts off (see percpu.h).
 *   kmutex_t      sleeping lock for long sections (a filesystem, lwIP): the
 *                 waiter sleeps instead of spinning.  Thread context only,
 *                 never from an interrupt handler or under a spinlock.
 *
 * A kmutex sleeps through sleep_locked() (proc/scheduler.h, stage 2a): the
 * guard is released only after the waiter is marked asleep, and the unlock
 * wakes under the guard, as xv6's sleep(chan, lk) does.  The scheduler state
 * itself is still the BKL's until stage 4 puts it under sched_lock; the API
 * does not change.
 *
 * Lock-order checking (`make KLOCKDEP=1`, off by default, compiled out
 * otherwise): every lock names a class (normally the lock's own name, so all
 * locks initialised from one KSPINLOCK_INIT("x") share class "x").  The
 * checker records "B was taken while A was held" and prints one report the
 * first time any of these is seen:
 *   - an order inversion (B under A somewhere, A under B elsewhere, also
 *     through a chain of classes),
 *   - a recursive acquisition of a class already held on this CPU,
 *   - a class taken both in an interrupt handler and with interrupts enabled
 *     (the interrupt can land on the holder and deadlock it),
 *   - sleeping or yielding with a spinlock held, a kmutex taken in an
 *     interrupt handler or under a spinlock, an unlock by a non-owner.
 * Reports are "[lockdep] ..." lines on the console; klockdep_reports() counts
 * them.  The checker never stops the machine by itself (only the torture test
 * turns a report into a failure).
 *
 * Torture test (`make KLOCK_TEST=1`, which also turns on KLOCKDEP): kernel
 * threads, one per CPU, step out from under the BKL and hammer a spinlock
 * and a kmutex at the same time; the log must end in "[KLOCK-TEST] PASS"
 * (tools/smoke_klock.py, `make smoke-klock`).
 */
#include <stdint.h>
#include "../../arch/i686/cpu/spinlock.h"

#ifndef KLOCKDEP
#if defined(KLOCK_TEST) && KLOCK_TEST
#define KLOCKDEP 1
#else
#define KLOCKDEP 0
#endif
#endif

typedef struct kspinlock {
    spinlock_t raw;
    const char *name;           /* lock class (lockdep); also for reports  */
#if KLOCKDEP
    uint16_t cls;               /* class index + 1, 0 until first use       */
    uint16_t owner;             /* CPU index + 1 while held, 0 when free    */
#endif
} kspinlock_t;

struct proc;
typedef struct kmutex {
    kspinlock_t guard;          /* protects owner/waiters                   */
    struct proc *volatile owner;
    int waiters;
    const char *name;
#if KLOCKDEP
    uint16_t cls;
#endif
} kmutex_t;

#if KLOCKDEP
#define KSPINLOCK_INIT(n) { { 0 }, (n), 0, 0 }
#define KMUTEX_INIT(n)    { KSPINLOCK_INIT("kmutex.guard"), 0, 0, (n), 0 }
#else
#define KSPINLOCK_INIT(n) { { 0 }, (n) }
#define KMUTEX_INIT(n)    { KSPINLOCK_INIT("kmutex.guard"), 0, 0, (n) }
#endif

void kspin_init(kspinlock_t *l, const char *name);
uint32_t kspin_lock_irqsave(kspinlock_t *l);            /* returns EFLAGS */
void kspin_unlock_irqrestore(kspinlock_t *l, uint32_t flags);
void kspin_lock(kspinlock_t *l);
void kspin_unlock(kspinlock_t *l);
int  kspin_trylock_irqsave(kspinlock_t *l, uint32_t *flags);   /* 1: taken */
int  kspin_held(const kspinlock_t *l);   /* by anyone; for assertions only */

void kmutex_init(kmutex_t *m, const char *name);
void kmutex_lock(kmutex_t *m);
int  kmutex_trylock(kmutex_t *m);        /* 1: taken */
void kmutex_unlock(kmutex_t *m);
int  kmutex_owned(const kmutex_t *m);    /* by the calling thread */

#if KLOCKDEP
/* Called where a thread may sleep or switch (sleep_on, yield): a spinlock
 * held here is a bug.  `what` names the call site in the report. */
void klock_might_sleep(const char *what);
unsigned klockdep_reports(void);
#else
static inline void klock_might_sleep(const char *what) { (void)what; }
static inline unsigned klockdep_reports(void) { return 0; }
#endif

#if defined(KLOCK_TEST) && KLOCK_TEST
void klock_test_start(void);             /* kernel/main.c, at boot */
#endif
