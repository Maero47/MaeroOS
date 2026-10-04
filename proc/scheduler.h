#pragma once
#include <kernel/klock.h>

/* The scheduler's saved context is per-CPU (see cpus[].sched_ctx in scheduler.c). */

/* Initialize scheduler (must call after proc_init) */
void scheduler_init(void);

/* Called by sys_exit: mark current ZOMBIE and switch back to scheduler */
void proc_exit(int status) __attribute__((noreturn));

/* Called from PIT IRQ handler — decrement timeslice, yield if expired */
/* PIT tick: wakes timed sleepers, accounts CPU, and preempts the current
 * process only when user_mode is non-zero (the tick interrupted ring 3). */
void scheduler_tick(int user_mode);

/* Yield the current process back to the scheduler */
void yield(void);

/* Make a SLEEPING/STOPPED thread RUNNABLE: places it in the fair queue and
 * preempts the CPU it should displace (wakeup preemption).  BKL held. */
struct proc;
void sched_make_runnable(struct proc *p);
/* ... and, called from a syscall, have the caller yield to it at the syscall's
 * exit (futex wake, wake_up_n: a targeted hand-off). */
void sched_make_runnable_sync(struct proc *p);
/* Wake an idle CPU for a thread just made RUNNABLE directly (fork, clone,
 * kthread creation). */
void sched_kick_idle(void);

/* Return-to-user point of an IRQ or exception: switch away if this CPU's
 * need_resched is set and the trap came from ring 3 (from_user). */
void sched_irq_exit(int from_user);

/* Reschedule at the kernel→user return boundary if a wake happened during this
 * syscall (Linux TIF_NEED_RESCHED on kernel exit).  Called by syscall_dispatch
 * after all syscall work + signal delivery. */
void resched_on_return(void);

/*
 * Sleep on a channel until wake_up(chan) is called, a deliverable signal
 * arrives, or the deadline the caller stored in current_proc->wake_tick passes.
 * Sets state = PROC_SLEEPING, saves context, returns when woken.  Returns 1 if
 * the sleep ended because the wake_tick deadline fired (the caller then reports
 * a timeout, e.g. FUTEX_WAIT -> -ETIMEDOUT), 0 for any other wake.  wake_tick is
 * always consumed: no stale deadline survives into a later untimed sleep.
 */
int sleep_on(void *chan);

/*
 * The same, for a condition guarded by the spinlock `lk` (xv6 sleep(chan, lk),
 * docs/smp-plan.md stage 2a).  The caller holds lk, taken with
 * kspin_lock_irqsave, and has just found the condition false; the thread is
 * put to sleep on chan before lk is released, and lk is held again (IF still
 * off) when this returns.  Wakers change the condition under lk and call
 * wake_up(chan) before releasing it, so no wake falls between the test and
 * the sleep.  Returns what sleep_on returns.
 */
int sleep_locked(void *chan, kspinlock_t *lk);

/*
 * Wait queue: a sleep channel with its own lock (stage 2a).  The lock guards
 * whatever condition the waiters test; wq_wait is sleep_locked on the queue.
 *
 *     uint32_t fl = kspin_lock_irqsave(&wq->lock);
 *     while (!cond) wq_wait(wq);
 *     ... consume under the lock ...
 *     kspin_unlock_irqrestore(&wq->lock, fl);
 *
 * and the producer changes cond under wq->lock and calls wq_wake_all(wq)
 * before unlocking.
 */
typedef struct waitq { kspinlock_t lock; } waitq_t;
#define WAITQ_INIT(n) { KSPINLOCK_INIT(n) }
static inline void waitq_init(waitq_t *wq, const char *name) { kspin_init(&wq->lock, name); }
static inline int  wq_wait(waitq_t *wq) { return sleep_locked(wq, &wq->lock); }

/* Wake all processes sleeping on chan */
void wake_up(void *chan);

/* Wake at most n processes sleeping on chan; returns the number woken */
int wake_up_n(void *chan, int n);
static inline void wq_wake_all(waitq_t *wq) { wake_up(wq); }
static inline int  wq_wake_n(waitq_t *wq, int n) { return wake_up_n(wq, n); }

/* Wake at most n threads of ONE thread group sleeping on chan (oldest-first).
 * For futex-style wakes on USER virtual addresses issued from kernel paths
 * outside sys_futex (exit-time CLEARTID, robust-mutex death): those semantics
 * are intra-process (pthread_join, robust mutexes), and with ASLR off the same
 * vaddr exists in every Firefox process — an unscoped wake mis-delivers to
 * other processes and feeds glibc-2.36's BZ#25847 condvar signal-steal.
 * Mirrors sys_futex's private-key (tgid, addr) matching. */
int wake_up_n_tgid(void *chan, int n, int tgid);

/*
 * Global I/O-activity channel: producers (input drivers, NIC RX, sockets,
 * audio) call io_wake() so knetd, socket waits and blocked poll/select
 * sleepers re-check readiness immediately instead of tick-polling.
 * Producers whose only other waiters sleep on the object itself (pipes,
 * eventfds, ptys) call io_wake_poll(), which wakes poll/select/epoll alone.
 * io_poll_sleep() is the poll/select/epoll sleep (returns sleep_on's value).
 */
void io_wake(void);
void io_wake_poll(void);
int  io_poll_sleep(void);
extern int io_activity;   /* sleep channel: sleep_on(&io_activity) */
extern int io_poll_chan;  /* where io_poll_sleep() sleeps (kwatch names it) */

/*
 * Suppress/allow PIT preemption of the current process (nests).  Hold around
 * non-reentrant critical sections (the lwIP stack); never sleep while held.
 */
void preempt_disable(void);
void preempt_enable(void);

/*
 * Stop the current process (sets PROC_STOPPED) and yield to scheduler.
 * Returns when SIGCONT resumes the process.
 */
void proc_stop_self(void);

/* Enter the scheduler loop — never returns */
void scheduler_start(void) __attribute__((noreturn));
