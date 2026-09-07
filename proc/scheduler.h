#pragma once

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

/* Wake all processes sleeping on chan */
void wake_up(void *chan);

/* Wake at most n processes sleeping on chan; returns the number woken */
int wake_up_n(void *chan, int n);

/* Wake at most n threads of ONE thread group sleeping on chan (oldest-first).
 * For futex-style wakes on USER virtual addresses issued from kernel paths
 * outside sys_futex (exit-time CLEARTID, robust-mutex death): those semantics
 * are intra-process (pthread_join, robust mutexes), and with ASLR off the same
 * vaddr exists in every Firefox process — an unscoped wake mis-delivers to
 * other processes and feeds glibc-2.36's BZ#25847 condvar signal-steal.
 * Mirrors sys_futex's private-key (tgid, addr) matching. */
int wake_up_n_tgid(void *chan, int n, int tgid);

/*
 * Global I/O-activity channel: producers (pipes, PTYs, input drivers, NIC
 * RX) call io_wake() so blocked poll/select sleepers re-check readiness
 * immediately instead of tick-polling.
 */
void io_wake(void);
extern int io_activity;   /* sleep channel: sleep_on(&io_activity) */

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
