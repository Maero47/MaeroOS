#ifndef ARCH_I686_BKLSTAT_H
#define ARCH_I686_BKLSTAT_H

/*
 * BKL contention statistics (docs/smp-plan.md), built only with
 * `make BKLSTAT=1`.  Every function here is an empty inline otherwise.
 *
 * Each CPU charges the time it holds the Big Kernel Lock to a REASON: the
 * syscall number or interrupt vector that entered the kernel, the scheduler
 * loop, or a kernel thread.  A CPU that has to spin for the lock charges the
 * spin both to its own reason ("waiter") and to the reason the holder had when
 * the spin began ("blocked by").  Counters are per CPU, written only by their
 * CPU with interrupts off, and summed at dump time.
 *
 * Syscalls 507 (dump to the console) and 508 (reset) drive it from user space
 * (`schedlat bkl dump|reset`); tools/bench_bkl.py runs the workloads.
 */

#ifdef BKLSTAT
struct registers;
void bklstat_sched_in(int slot, int kthread);   /* scheduler -> thread   */
void bklstat_sched_out(int slot, int depth_ok); /* thread -> scheduler   */
void bklstat_reset(void);
void bklstat_dump(void);
#else
static inline void bklstat_sched_in(int slot, int kthread) { (void)slot; (void)kthread; }
static inline void bklstat_sched_out(int slot, int depth_ok) { (void)slot; (void)depth_ok; }
#endif

#endif /* ARCH_I686_BKLSTAT_H */
