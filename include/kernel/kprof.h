#pragma once
/*
 * kprof — an exact, non-overlapping TSC accounting of where the CPU's cycles go.
 *
 * Every cycle the machine executes belongs to exactly one bucket.  A bucket
 * change charges the cycles since the previous change to the bucket that was
 * current and starts counting for the new one, so the buckets always sum to the
 * elapsed TSC (kprof_dump prints both, so the closure is checkable).
 *
 * Buckets 0..KPB_FIXED-1 are fixed; bucket KPB_SYSBASE+n is "kernel time inside
 * syscall n", which splits kernel time per syscall for free.
 *
 * The bucket in effect belongs to the running thread, so it is saved into the
 * thread on a context switch (scheduler.c) and restored when it is dispatched.
 * That makes the accounting exclusive: a thread that blocks in a syscall stops
 * charging that syscall while another thread runs.  Time a thread spends
 * BLOCKED is measured separately, by kprof_sleep_*.
 *
 * Accurate on one CPU (the kernel is BKL-serialised, but two CPUs can execute
 * user code at once and then share one bucket); every measurement in
 * docs/perf/firefox-startup.md was taken with -smp 1.
 */
#include <stdint.h>

#define KPB_USER      0   /* ring 3                                          */
#define KPB_IDLE      1   /* halted in the scheduler, nothing runnable       */
#define KPB_SCHED     2   /* scheduler dispatch loop                          */
#define KPB_IRQ       3   /* hardware interrupt handlers                      */
#define KPB_PGFAULT   4   /* #PF handler (minus any nested ATA)               */
#define KPB_ATA       5   /* ATA PIO transfers (interrupts off)               */
#define KPB_EXC       6   /* other CPU exceptions                             */
#define KPB_FB        7   /* memcpy into the framebuffer (uncached MMIO)      */
#define KPB_FIXED     8
#define KPB_SYSBASE   KPB_FIXED
#define KPROF_NSYS    448              /* Linux i386 table + our 500-505      */
#define KPROF_NBUCKET (KPB_SYSBASE + KPROF_NSYS)

/* Exact event counters. */
enum {
    KPE_SYSCALL, KPE_CTXSW,
    KPE_PF_COW, KPE_PF_ANON, KPE_PF_FILE, KPE_PF_STACK, KPE_PF_OTHER,
    KPE_ATA_RD, KPE_ATA_RD_SECT, KPE_ATA_WR, KPE_ATA_WR_SECT,
    KPE_EXT2_BLK, KPE_EXT2_HIT, KPE_EXT2_MISS,
    KPE_WAKE, KPE_RESCHED, KPE_FB_KB,
    KPE_EXT2_DISK,          /* blocks actually fetched off the platter      */
    KPE_EXT2_DISTINCT,      /* of those, blocks fetched for the first time  */
    KPE_EXT2_RA,            /* blocks fetched purely as read-ahead          */
    KPE_EXT2_RA_USED,       /* read-ahead blocks later served from the cache */
    KPE_PF_FILE_SEQ,        /* file fault on the page after the previous one */
    KPE_PF_FILE_AROUND,     /* pages populated by fault-around, not faulted  */
    KPE_MAX
};

/*
 * Probes — named (cycles, count) accumulators for zooming into one code span.
 *
 * They are ADDITIVE and deliberately outside the exclusive bucket accounting:
 * a probe may nest inside another probe or inside any bucket, so probe totals
 * do not sum to anything and must never be added to the bucket table.  They
 * answer "how much of syscall X is this function", which the buckets cannot.
 */
enum {
    KPP_SYS_PRO,        /* syscall_dispatch prologue (before the switch)     */
    KPP_SYS_EPI,        /* syscall_dispatch epilogue (signals + resched)     */
    KPP_YIELD_PRE,      /* yield() up to the park                            */
    KPP_SCHED_SCAN,     /* scheduler ptable scan up to picking a thread      */
    KPP_SCHED_DISP,     /* dispatch prologue: tss, tls, cr3, fpu             */
    KPP_GAP_FIND,       /* vma_gap_find                                      */
    KPP_FIRST_MAPPED,   /* first_mapped_page                                 */
    KPP_MMAP_POP,       /* eager population inside mmap2                     */
    KPP_MMAP_UNMAP,     /* unmap_range inside mmap2                          */
    KPP_FAULT_READ,     /* vfs_read inside a file-backed fault               */
    KPP_FAULT_ZERO,     /* the memset of a freshly allocated fault frame     */
    KPP_ATA_SMALL,      /* ata_read of <= 4 sectors                          */
    KPP_ATA_BIG,        /* ata_read of  > 4 sectors                          */
    KPP_E2_HEAD,        /* ext2_cache_lookup before the set scan             */
    KPP_E2_TAIL,        /* ext2_cache_lookup after the copy                  */
    KPP_E2_NULL,        /* an empty span next to e2_copy: probe cost in situ  */
    KPP_E2_FIND,        /* ext2_cache_find alone (the set scan)              */
    KPP_E2_MEMCPY,      /* the memcpy out of a cache slot                    */
    KPP_E2_ALLOC,       /* the kmalloc/kfree pair in ext2_read_node          */
    KPP_E2_INODE,       /* ext2_read_inode per read                          */
    KPP_E2_BMAP,        /* ext2_file_blk_cached (the block map walk)         */
    KPP_E2_COPY,        /* fetching one block into the destination           */
    KPP_SYS_BODY,       /* the syscall's own work (the dispatch switch)      */
    KPP_SYS_RESCHED,    /* resched_on_return (may yield: not CPU time)       */
    KPP_DISP_TSS,       /* tss_set_kernel_stack + gdt_set_tls per dispatch   */
    KPP_DISP_CR3,       /* the cr3 reload per dispatch                       */
    KPP_DISP_FPU,       /* fxrstor per dispatch                              */
    KPP_DUMP,           /* the profiler's own periodic dump (printk to serial) */
    KPP_CALIB,          /* an empty span: the cost of a probe pair itself      */
    KPP_MAX
};

/* Charge the cycles since the last switch to the current bucket, make `bucket`
 * current, and return the bucket that was replaced (to restore on the way out). */
int  kprof_switch(int bucket);
static inline int kprof_sys_bucket(uint32_t nr) {
    return KPB_SYSBASE + (int)(nr < KPROF_NSYS ? nr : KPROF_NSYS - 1);
}

/* The event counters are hit several times per block-cache operation and
 * millions of times per startup, so they are incremented in place rather than
 * through a call into kprof.c. */
extern uint64_t kprof_ev[KPE_MAX];
static inline void kprof_count(int ev) { kprof_ev[ev]++; }
static inline void kprof_add(int ev, uint32_t n) { kprof_ev[ev] += n; }

/* One entry into syscall `nr` from user mode.  Separate from the bucket
 * switches, which also fire when a parked thread is re-dispatched. */
void kprof_syscall_enter(uint32_t nr);

/* Blocked (not runnable) wall time, attributed to the syscall that blocked.
 * The token is the caller's, so concurrent sleepers do not share state. */
uint64_t kprof_sleep_begin(void);
void     kprof_sleep_end(uint64_t token, int syscall_nr);

/* Probe timing.  kprof_probe_begin() is a bare rdtsc; the end call charges the
 * delta to `id`.  Cheap enough to leave in place (two rdtsc per span). */
uint64_t kprof_probe_begin(void);
void     kprof_probe_end(int id, uint64_t t0);

void kprof_dump(const char *tag);
void kprof_reset(void);
void kprof_tick(void);          /* called from scheduler_tick: periodic dump */
