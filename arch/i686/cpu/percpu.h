#ifndef ARCH_I686_PERCPU_H
#define ARCH_I686_PERCPU_H

#include <stdint.h>

#define MAX_CPUS 8

/* Per-CPU kernel state.  Indexed directly by Local APIC id (0..MAX_CPUS-1;
 * QEMU -smp N gives contiguous ids 0..N-1). */
struct cpu {
    uint32_t apicid;
    int      bkl_depth;     /* recursive Big-Kernel-Lock nesting on this CPU   */
    struct proc *proc;      /* process this CPU is currently running (S4)       */
    struct context *sched_ctx;  /* this CPU's scheduler context (S4)            */
    volatile int tlb_pending;   /* S7: another CPU asked this one to flush TLB  */
};

extern struct cpu cpus[MAX_CPUS];

/* Index of the calling CPU (0 before the LAPIC is up). */
uint32_t this_cpu_id(void);

/*
 * Big Kernel Lock.  A single global spinlock held whenever a CPU executes
 * kernel code; user-mode runs lock-free and concurrently across CPUs.  The
 * lock is RECURSIVE per CPU (nested traps re-enter without re-locking) and is
 * held continuously across swtch() — released only at iret-to-user and around
 * the idle hlt.  No-op until the LAPIC is enabled (single-CPU early boot).
 */
void bkl_enter(void);   /* trap entry: acquire (or nest)        */
void bkl_leave(void);   /* trap exit:  release (or un-nest)     */
#define bkl_acquire bkl_enter
#define bkl_release bkl_leave

/*
 * S7: TLB shootdown.  Under the BKL a CPU may modify the *shared* page tables of
 * a multi-threaded process while a sibling thread runs on another CPU with a
 * stale TLB → that CPU would read/write the wrong physical page (corruption).
 * tlb_shootdown() forces every other online CPU to flush before the modifying
 * CPU proceeds (e.g. before a freed frame is reused).  Call AFTER the local
 * PTE change + local invlpg, while holding the BKL, for USER address-space
 * changes (unmap / permission-reduce / COW break) on a possibly-shared pgdir.
 */
void tlb_shootdown(void);

/* Service a pending flush request on the calling CPU (flush + clear flag).
 * Called from spin loops (bkl wait, AP idle) so a CPU that can't take the
 * shootdown IPI (interrupts off while spinning) still flushes — deadlock-free. */
void tlb_serve_pending(void);

#endif /* ARCH_I686_PERCPU_H */
