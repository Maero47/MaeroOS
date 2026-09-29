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

/* Service a pending flush request on the calling CPU (flush + ack).
 * Called at every trap entry and from spin loops (bkl wait, AP idle) so a CPU
 * that can't take the shootdown IPI (interrupts off) still flushes. */
void tlb_serve_pending(void);

#endif /* ARCH_I686_PERCPU_H */
