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
    KPE_MAX
};

/* Charge the cycles since the last switch to the current bucket, make `bucket`
 * current, and return the bucket that was replaced (to restore on the way out). */
int  kprof_switch(int bucket);
static inline int kprof_sys_bucket(uint32_t nr) {
    return KPB_SYSBASE + (int)(nr < KPROF_NSYS ? nr : KPROF_NSYS - 1);
}

void kprof_count(int ev);
void kprof_add(int ev, uint32_t n);

/* One entry into syscall `nr` from user mode.  Separate from the bucket
 * switches, which also fire when a parked thread is re-dispatched. */
void kprof_syscall_enter(uint32_t nr);

/* Blocked (not runnable) wall time, attributed to the syscall that blocked.
 * The token is the caller's, so concurrent sleepers do not share state. */
uint64_t kprof_sleep_begin(void);
void     kprof_sleep_end(uint64_t token, int syscall_nr);

void kprof_dump(const char *tag);
void kprof_reset(void);
void kprof_tick(void);          /* called from scheduler_tick: periodic dump */
