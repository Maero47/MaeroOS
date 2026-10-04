#ifndef ARCH_I686_PERCPU_H
#define ARCH_I686_PERCPU_H

#include <stdint.h>

#define MAX_CPUS 8

/* Per-CPU kernel state, indexed by LOGICAL CPU number: the BSP is 0 and each
 * AP gets the next free slot when it is started (percpu_map_apic).  Never by
 * raw Local APIC id — those need not be contiguous (0,2,4,6 on real
 * hardware), and an AP that fails to start must not leave a hole that a loop
 * over 0..count-1 would treat as a CPU. */
struct cpu {
    uint32_t apicid;
    volatile int online;    /* running kernel code; a shootdown target      */
    volatile int boot_state;    /* AP handshake: 0 wait, 1 alive, 2 abandoned */
    int      bkl_depth;     /* recursive Big-Kernel-Lock nesting on this CPU   */
    struct proc *proc;      /* process this CPU is currently running (S4)       */
    struct context *sched_ctx;  /* this CPU's scheduler context (S4)            */
    /* S7 TLB shootdown: a sender bumps tlb_req_gen; this CPU samples it,
     * flushes, and publishes the sample in tlb_ack_gen.  The sender waits for
     * ack >= its own request, so an ack can never cover a request the flush
     * did not (a single pending flag could be cleared by a flush that began
     * before the newer request was posted). */
    volatile uint32_t tlb_req_gen;
    volatile uint32_t tlb_ack_gen;
    /* Scheduler (proc/scheduler.c): need_resched asks this CPU to switch
     * away from its thread at the next return to user mode (Linux
     * TIF_NEED_RESCHED) or, when idle, to re-scan at once.  idle: in the idle
     * wait with nothing to run.  run_t0: clock_mono_ns() at the dispatch of
     * `proc`, so its running vruntime can be read from another CPU. */
    volatile int need_resched;
    volatile int idle;
    uint64_t run_t0;
    /* Seqcount over run_t0 (stage 2e): a 64-bit store is two 32-bit stores
     * on i686, so another CPU could read half of an old and half of a new
     * value.  Only this CPU writes it (sched_set_run_t0); readers retry while
     * the count is odd or changed. */
    volatile uint32_t run_seq;
    /* A syscall on this CPU woke threads that no idle CPU took (sync wake):
     * wake_vr is the largest vruntime among them (0: none), and the syscall
     * exit queues the waker behind them.  in_irq: set while a hardware
     * interrupt handler runs (its wakes are not the running thread's own);
     * cleared before the IRQ's signal delivery and at every dispatch. */
    uint64_t wake_vr;
    int in_irq;
    /* /proc/stat accounting: time this CPU ran threads (busy_ns) and sat in
     * the idle wait (idle_ns), from the scheduler's dispatch timestamps; and
     * the timer ticks that found it in user mode, in the kernel, or idle,
     * which split busy_ns into user and system time. */
    uint64_t busy_ns, idle_ns;
    uint32_t tick_user, tick_sys, tick_idle;
};

extern struct cpu cpus[MAX_CPUS];

/* Logical index of the calling CPU (0 before the LAPIC is up, and always 0 on
 * the BSP). */
uint32_t this_cpu_id(void);

/* Bind Local APIC id `apicid` to logical slot `idx`.  Called for the BSP when
 * its LAPIC is enabled, and for each AP before it is sent INIT-SIPI-SIPI. */
void percpu_map_apic(uint32_t apicid, uint32_t idx);

/* Stop caching the calling CPU's id.  MUST be called before the first AP is
 * started: until it is, this_cpu_id() answers from a value read once, which is
 * only correct while a single CPU executes. */
void smp_percpu_go_multi(void);

/*
 * Big Kernel Lock.  A single global spinlock held whenever a CPU executes
 * kernel code; user-mode runs lock-free and concurrently across CPUs.  The
 * lock is RECURSIVE per CPU (nested traps re-enter without re-locking) and is
 * held continuously across swtch() — released only at iret-to-user and around
 * the idle hlt.  No-op until the LAPIC is enabled (single-CPU early boot).
 */
void bkl_enter(void);   /* trap entry: acquire (or nest)        */
void bkl_leave(void);   /* trap exit:  release (or un-nest)     */
void bkl_state(int *locked, int *depth);  /* diagnostic: lock word + depth */
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

/* The same, for a change to the USER half of one address space: only CPUs
 * running a thread on page directory `pgdir_phys` are flushed (CPUs idle or
 * in another address space hold none of its user entries).
 * tlb_shootdown_user() names the directory in this CPU's CR3, which is the
 * one every pte_set()/recursive-mapping change modifies.  Kernel-half
 * changes (kernel stacks, kmaps) still need tlb_shootdown(). */
void tlb_shootdown_mm(uint32_t pgdir_phys);
void tlb_shootdown_user(void);

/* Service a pending flush request on the calling CPU (flush + ack).
 * Called at every trap entry and from spin loops (bkl wait, AP idle) so a CPU
 * that can't take the shootdown IPI (interrupts off) still flushes. */
void tlb_serve_pending(void);

#endif /* ARCH_I686_PERCPU_H */
