#include "scheduler.h"
#include "process.h"
#include "signal.h"
#include "seccomp.h"
#include "ktimer.h"

extern void vma_clear(struct proc *p);   /* free demand-paged VMAs (syscall.c) */
#include "pipe.h"
#include "usocket.h"
#include "shm.h"
#include "syscall.h"       /* copy_to_user / copy_from_user */
#include "../fs/devfs.h"
#include "../fs/vfs.h"
#include "../net/socket.h"
#include "../arch/i686/cpu/tss.h"
#include "../arch/i686/cpu/gdt.h"
#include "../arch/i686/cpu/fpu.h"
#include "../arch/i686/cpu/pit.h"
#include "../arch/i686/cpu/tsc.h"
#include "../arch/i686/cpu/percpu.h"
#include "../arch/i686/cpu/apic.h"
#include "../arch/i686/cpu/smp.h"
#include "../arch/i686/mm/paging.h"
#include "../mm/heap.h"
#include "../kernel/printk.h"
#include <kernel/config.h>
#include <kernel/kprof.h>
#include <kernel/kwatch.h>
#include <stdint.h>
#include <stddef.h>

/* swtch is defined in swtch.asm */
extern void swtch(struct context **old, struct context *new_ctx);

/* The scheduler's own saved context is PER-CPU (each CPU runs its own scheduler
 * loop).  `scheduler_ctx` expands to the calling CPU's slot, so the existing
 * swtch(&scheduler_ctx, ...) / swtch(..., scheduler_ctx) all act per-CPU. */
#define scheduler_ctx (cpus[this_cpu_id()].sched_ctx)

/*
 * Fair scheduling with wakeup preemption (Linux CFS's model, simplified).
 *
 * Every thread carries a vruntime: the ns it has spent on a CPU, scaled by
 * its nice weight.  Each CPU's loop runs the RUNNABLE thread with the least
 * vruntime, so CPU time is shared out evenly and a thread that sleeps a lot
 * (input, audio, compositor, a pipe reader) is always behind a CPU hog.
 *
 *  - Placement.  A thread that blocked is put back at no less than the queue
 *    minimum minus the time it slept, capped at SCHED_SLEEP_CREDIT, when it
 *    wakes: a sleeper is ahead of the threads that kept running, but cannot
 *    bank an hour of sleep and then monopolise the CPU — and a thread that
 *    blocks for microseconds at a time (a server answering a client that
 *    floods it) earns microseconds, so it batches instead of preempting the
 *    client on every request.  A new thread starts at the minimum.
 *  - Wakeup preemption.  A wake (from a syscall, an IRQ, the tick or another
 *    CPU) compares the woken thread with what the CPUs are running.  If a CPU
 *    is idle it is kicked; otherwise, when the woken thread is ahead of the
 *    running one by SCHED_WAKEUP_GRAN, that CPU's need_resched is set — and a
 *    reschedule IPI sent if it is another CPU.  The switch itself happens only
 *    at a return to user mode (syscall exit, IRQ exit), never in the middle of
 *    kernel code: the kernel is not preemptible (BKL, lwIP, temp maps).
 *  - Slice.  The tick preempts a thread that has run SCHED_SLICE_NS since its
 *    dispatch when another runnable thread is behind it.
 *  - yield() (sched_yield, kernel spin-waits) passes the yielder over once,
 *    so it cannot keep the CPU from the thread it waits for.
 */
#define SCHED_SLICE_NS      4000000ULL   /* 4 ms; the 100 Hz tick rounds up   */
#define SCHED_SLEEP_CREDIT  3000000ULL   /* sleeper bonus (CFS: latency / 2)  */
#define SCHED_WAKEUP_GRAN    500000ULL   /* lead needed to preempt on wakeup  */
#define RESCHED_IPI_VECTOR  0xFCU

/* Linux sched_prio_to_weight: nice 0 = 1024, each step ~1.25x. */
static const uint32_t nice_weight[40] = {
    88761, 71755, 56483, 46273, 36291, 29154, 23254, 18705, 14949, 11916,
    9548,  7620,  6100,  4904,  3906,  3121,  2501,  1991,  1586,  1277,
    1024,  820,   655,   526,   423,   335,   272,   215,   172,   137,
    110,   87,    70,    56,    45,    36,    29,    23,    18,    15,
};

static uint64_t sched_min_vr;   /* monotonic floor of the queue's vruntime */

static inline uint64_t vr_scale(const struct proc *p, uint64_t ns) {
    int n = p->nice;
    if (n == 0) return ns;
    if (n < -20) n = -20;
    if (n > 19) n = 19;
    return ns * 1024U / nice_weight[n + 20];
}

/* The vruntime a running thread has now, including its current stint. */
static uint64_t cpu_curr_vr(uint32_t c, uint64_t now) {
    struct proc *p = cpus[c].proc;
    if (!p) return 0;
    uint64_t t0 = cpus[c].run_t0;
    return p->vruntime + (now > t0 ? vr_scale(p, now - t0) : 0);
}

/* Put a thread that is being made runnable where it belongs in the queue. */
static void sched_place(struct proc *p) {
    if (!p->vr_placed) {
        p->vruntime  = sched_min_vr;
        p->vr_placed = 1;
        p->vr_slept  = 0;
    } else if (p->vr_slept) {
        uint64_t now = clock_mono_ns();
        uint64_t credit = now > p->vr_sleep_t0 ? now - p->vr_sleep_t0 : 0;
        if (credit > SCHED_SLEEP_CREDIT) credit = SCHED_SLEEP_CREDIT;
        uint64_t floor = sched_min_vr > credit ? sched_min_vr - credit : 0;
        if (p->vruntime < floor) p->vruntime = floor;
        p->vr_slept = 0;
    }
}

static void resched_ipi(uint32_t c) {
    if (!apic_available() || !cpus[c].online) return;
    apic_write(LAPIC_REG_ICR_HI, cpus[c].apicid << 24);
    apic_write(LAPIC_REG_ICR_LO, RESCHED_IPI_VECTOR | (1U << 14));
}

/* Ask CPU c to reschedule: a flag for itself, an IPI for another CPU (the
 * idle APs poll the flag; the BSP halts and needs the interrupt). */
static void resched_cpu(uint32_t c) {
    if (cpus[c].need_resched) return;
    cpus[c].need_resched = 1;
    if (c != this_cpu_id() && !(cpus[c].idle && c != 0))
        resched_ipi(c);
}

/* p was just made RUNNABLE (caller holds the BKL).  Place it and preempt
 * whatever CPU it should displace, if any. */
static void sched_wakeup(struct proc *p, int sync) {
    sched_place(p);
    uint32_t ncpu = smp_cpu_count();
    if (ncpu > MAX_CPUS) ncpu = MAX_CPUS;
    uint32_t self = this_cpu_id();
    /* An idle CPU takes it at once.  Prefer this one if idle (a wake from an
     * IRQ that interrupted the idle wait). */
    if (cpus[self].idle) { cpus[self].need_resched = 1; return; }
    for (uint32_t c = 0; c < ncpu; c++)
        if (cpus[c].online || c == 0)
            if (cpus[c].idle && !cpus[c].need_resched) { resched_cpu(c); return; }
    /* A sync wake from a syscall (futex wake, wake_up_n: a condvar signal,
     * a mutex hand-off, pthread_join): the waker steps behind the woken
     * thread at the syscall's exit, so it runs before the waker can act
     * again.  glibc-2.36's Riegel condvar relies on the signalled waiter
     * running before the signaller comes back to steal the signal. */
    if (sync && !cpus[self].in_irq && current_proc && current_proc != p &&
        p->vruntime + 1 > cpus[self].wake_vr)
        cpus[self].wake_vr = p->vruntime + 1;
    /* Otherwise the CPU running the thread furthest ahead of p. */
    uint64_t now = clock_mono_ns();
    int best = -1;
    uint64_t best_lead = 0;
    for (uint32_t c = 0; c < ncpu; c++) {
        if (!(cpus[c].online || c == 0) || !cpus[c].proc) continue;
        if (cpus[c].proc == p) continue;
        uint64_t cv = cpu_curr_vr(c, now);
        if (cv > p->vruntime + SCHED_WAKEUP_GRAN && cv - p->vruntime > best_lead) {
            best_lead = cv - p->vruntime;
            best = (int)c;
        }
    }
    if (best >= 0) resched_cpu((uint32_t)best);
}

/* Make a sleeping/stopped thread runnable: the one wake primitive. */
void sched_make_runnable(struct proc *p) {
    p->state = PROC_RUNNABLE;
    sched_wakeup(p, 0);
}

/* The same for a targeted hand-off from a syscall (futex wake, wake_up_n):
 * the waker also yields to it at the syscall's exit. */
void sched_make_runnable_sync(struct proc *p) {
    p->state = PROC_RUNNABLE;
    sched_wakeup(p, 1);
}

/* The RUNNABLE thread to run next: least vruntime, passing over a yielder
 * once (its skip flag is consumed).  NULL if nothing is runnable. */
static struct proc *sched_pick(void) {
    struct proc *best = NULL, *skipped = NULL;
    for (int i = 0; i < ptable_hwm; i++) {
        struct proc *p = &ptable[i];
        if (p->state != PROC_RUNNABLE) continue;
        sched_place(p);
        if (p->vr_skip) {
            p->vr_skip = 0;
            if (!skipped || p->vruntime < skipped->vruntime) skipped = p;
            continue;
        }
        if (!best || p->vruntime < best->vruntime) best = p;
    }
    if (!best) best = skipped;
    if (best) {
        /* Advance the floor to the least vruntime in play: the pick and
         * whatever the other CPUs are running. */
        uint64_t m = best->vruntime;
        uint64_t now = clock_mono_ns();
        for (uint32_t c = 0; c < MAX_CPUS; c++)
            if (cpus[c].proc) {
                uint64_t cv = cpu_curr_vr(c, now);
                if (cv < m) m = cv;
            }
        if (m > sched_min_vr) sched_min_vr = m;
    }
    return best;
}

void scheduler_init(void) {
    /* Nothing to initialize — ptable is already set up by proc_init */
}

/* Time the CPUs spent halted with nothing to run, in us (/proc/cputime). */
uint32_t sched_idle_us;
/* Sync hand-offs taken at syscall exit (/proc/cputime "handoffs"): lets a
 * test see that futex wakes still yield to the woken thread. */
uint32_t sched_handoffs;
static uint32_t sched_idle_ns_rem;

/* Add an interval to a us counter, carrying the sub-us part.  32-bit only:
 * one interval is a time slice or one idle halt, far below 4 s. */
static void add_ns(uint32_t *us, uint32_t *rem, uint64_t ns) {
    uint32_t n = ns > 0xFFFFFFFFULL ? 0xFFFFFFFFU : (uint32_t)ns;
    uint32_t q = n / 1000U, r = n % 1000U + *rem;
    *us += q + r / 1000U;
    *rem = r % 1000U;
}

void scheduler_start(void) {
    struct cpu *me = &cpus[this_cpu_id()];
    for (;;) {
        kwatch_poll();          /* emit a stall the timer tick spotted */
        uint64_t scan_t0 = kprof_probe_begin();
        me->need_resched = 0;
        me->wake_vr = 0;        /* a kthread's sync wake owes no one a yield */
        me->in_irq = 0;         /* no handler is in progress between threads */
        struct proc *p = sched_pick();

        if (p) {
            kprof_probe_end(KPP_SCHED_SCAN, scan_t0);
            uint64_t disp_t0 = kprof_probe_begin();
            current_proc   = p;
            p->state       = PROC_RUNNING;
            p->sched_count++;

            uint64_t d_t = kprof_probe_begin();
            tss_set_kernel_stack((uint32_t)(uintptr_t)(p->kstack + KSTACKSIZE));
            /* ALWAYS reprogram this CPU's TLS (%gs) base — even to 0.  Skipping
             * it when p->tls_base==0 left the PREVIOUS thread's base in this CPU's
             * GDT entry 6; the next iret reloads %gs from it, so a no-TLS thread
             * would read/write another thread's TLS → wild-pointer corruption. */
            gdt_set_tls(p->tls_base);
            kprof_probe_end(KPP_DISP_TSS, d_t);

            uint64_t d_c = kprof_probe_begin();
            if (p->pgdir_phys)
                __asm__ volatile("mov %0, %%cr3" :: "r"(p->pgdir_phys) : "memory");
            kprof_probe_end(KPP_DISP_CR3, d_c);

            uint64_t d_f = kprof_probe_begin();
            fpu_restore(fpu_area(p));
            kprof_probe_end(KPP_DISP_FPU, d_f);
            kprof_probe_end(KPP_SCHED_DISP, disp_t0);
            kprof_count(KPE_CTXSW);
            kprof_switch(p->kprof_bucket);   /* charge the dispatch to KPB_SCHED */
            uint64_t run_t0 = clock_mono_ns();
            me->run_t0 = run_t0;
            swtch(&scheduler_ctx, p->context);
            uint64_t ran = clock_mono_ns() - run_t0;
            add_ns(&p->run_us, &p->run_ns_rem, ran);
            p->vruntime += vr_scale(p, ran);
            /* Back in the scheduler: the outgoing thread already parked its own
             * bucket and left KPB_SCHED current (see kprof_park below). */
            uint64_t s_f = kprof_probe_begin();
            fpu_save(fpu_area(p));
            kprof_probe_end(KPP_SCHED_FPUSAVE, s_f);

            uint64_t s_c = kprof_probe_begin();
            __asm__ volatile("mov %0, %%cr3" :: "r"(kernel_pgdir_phys) : "memory");
            kprof_probe_end(KPP_SCHED_KCR3, s_c);
            current_proc = NULL;
            /* A non-leader thread that just exited is released here, on the
             * scheduler's own stack, now that its kernel stack is no longer in
             * use.  Linux release_task()s such threads immediately in
             * exit_notify(): they are never reported by wait(), only the group
             * leader is (once every thread is gone). */
            if (p->state == PROC_ZOMBIE && p->pid != p->tgid)
                proc_release(p);
            __asm__ volatile("sti");
            continue;
        }

        /* Nothing runnable: halt until the next interrupt (PIT tick, key,
         * IRQ, reschedule IPI) instead of spinning — drops host CPU to ~0 when
         * idle.  RELEASE the Big Kernel Lock first so the other CPU(s) and the
         * waking IRQ can run kernel code; re-acquire on wake before re-scanning
         * the shared ptable.  `idle` is published under the lock, so a waker
         * (which holds it) either sees it and kicks us, or ran before our scan
         * and we found its thread. */
        int kp_old = kprof_switch(KPB_IDLE);
        uint64_t idle_t0 = clock_mono_ns();
        __asm__ volatile("cli");
        me->idle = 1;
        bkl_release();
        if (this_cpu_id() == 0) {
            /* BSP: woken by the PIT/keyboard/IRQ (all routed here via the
             * PIC→LINT0) or a reschedule IPI.  Interrupts stay off from the
             * need_resched test to the hlt (sti's one-instruction shadow), so
             * a kick cannot slip in between and leave us halted until the
             * next tick. */
            if (!me->need_resched)
                __asm__ volatile("sti; hlt");
            __asm__ volatile("sti");
        } else {
            /* AP: the PIC delivers only to the BSP, but the AP DOES have
             * its own LAPIC timer (S6) and takes IPIs, so service any TLB
             * shootdown while idling and spin-back-off before re-scanning
             * the shared ptable for newly-runnable work — at once when a
             * waker kicks us through need_resched. */
            __asm__ volatile("sti");
            for (volatile int i = 0; i < 200000 && !me->need_resched; i++) {
                tlb_serve_pending();
                __asm__ volatile("pause");
            }
        }
        bkl_acquire();
        me->idle = 0;
        add_ns(&sched_idle_us, &sched_idle_ns_rem, clock_mono_ns() - idle_t0);
        kprof_switch(kp_old);
    }
}

void scheduler_tick(int user_mode) {
    uint32_t now = pit_ticks();
    kwatch_tick();
    struct proc *cur = current_proc;
    uint64_t min_runnable = ~0ULL;
    for (int i = 0; i < ptable_hwm; i++) {
        struct proc *p = &ptable[i];
        if (p->state == PROC_SLEEPING && p->wake_tick &&
            (int32_t)(now - p->wake_tick) >= 0) {
            /* Deadline expiry is the only wake that means "timed out"; record
             * it so sleep_on() can tell its caller (FUTEX_WAIT -> -ETIMEDOUT,
             * poll/select -> 0) instead of the caller guessing from the clock. */
            p->wake_tick = 0;
            p->sleep_chan = (void *)0;
            p->sleep_timed_out = 1;
            sched_make_runnable(p);
        }
        if (p->state == PROC_RUNNABLE && p->vr_placed && p->vruntime < min_runnable)
            min_runnable = p->vruntime;
    }

    /* Expire alarm/setitimer/POSIX timers: queues their signals only. */
    ktimer_tick(user_mode);

    /* Advance the queue floor with the threads on the CPUs, as Linux's
     * update_curr does: picks alone leave it stale while one thread runs
     * unopposed, and a thread waking (or forked) after that would be placed
     * so far behind that it could hold the CPU for as long as the other ran. */
    {
        uint64_t t = clock_mono_ns(), m = min_runnable;
        int any = min_runnable != ~0ULL;
        for (uint32_t c = 0; c < MAX_CPUS; c++)
            if (cpus[c].proc) {
                uint64_t cv = cpu_curr_vr(c, t);
                if (cv < m) m = cv;
                any = 1;
            }
        if (any && m != ~0ULL && m > sched_min_vr) sched_min_vr = m;
    }

    if (!cur) return;
    cur->utime_ticks++;
    /* Slice expiry: the thread has had SCHED_SLICE_NS and another runnable
     * one is behind it.  The switch happens at the IRQ's return to user mode
     * (sched_irq_exit); kernel-mode execution is never preempted. */
    struct cpu *me = &cpus[this_cpu_id()];
    uint64_t t = clock_mono_ns();
    if (min_runnable != ~0ULL && t - me->run_t0 >= SCHED_SLICE_NS &&
        cpu_curr_vr(this_cpu_id(), t) > min_runnable)
        me->need_resched = 1;
    (void)user_mode;
}

/* Involuntary switch: back to the scheduler without the yield skip. */
static void sched_preempt(void) {
    current_proc->state = PROC_RUNNABLE;
    kprof_count(KPE_RESCHED);
    if (current_proc) current_proc->kprof_bucket = kprof_switch(KPB_SCHED);
    __asm__ volatile("cli");
    swtch(&current_proc->context, scheduler_ctx);
    __asm__ volatile("sti");
}

/* Return-to-user point of an interrupt or exception (Linux
 * exit_to_user_mode): switch away if a wake or the tick asked this CPU to.
 * Only when the trap came from ring 3 — then no kernel state is in flight —
 * and never inside a preemption guard. */
void sched_irq_exit(int from_user) {
    struct proc *cur = current_proc;
    if (!from_user || !cur || cur->no_preempt) return;
    struct cpu *me = &cpus[this_cpu_id()];
    if (!me->need_resched || cur->state != PROC_RUNNING) return;
    me->need_resched = 0;
    sched_preempt();
}

/*
 * Preemption guard: lwIP is not reentrant, but the PIT tick can yield()
 * mid-syscall and schedule knetd (which also enters lwIP).  Sections that
 * call into lwIP hold this; the tick then skips preemption.  Per-process
 * and nesting; voluntary sleeps must not happen while held.
 */
void preempt_disable(void) {
    if (current_proc) current_proc->no_preempt++;
}

void preempt_enable(void) {
    if (current_proc && current_proc->no_preempt > 0)
        current_proc->no_preempt--;
}

/* Park the running thread's kprof bucket in the thread and hand the cycles
 * over to the scheduler, so kernel time is charged to whoever is really on the
 * CPU (see include/kernel/kprof.h). */
static inline void kprof_park(void) {
    if (current_proc) current_proc->kprof_bucket = kprof_switch(KPB_SCHED);
    else kprof_switch(KPB_SCHED);
}

void yield(void) {
    if (!current_proc) return;
    uint64_t yp = kprof_probe_begin();
    current_proc->state = PROC_RUNNABLE;
    current_proc->vr_skip = 1;
    kprof_probe_end(KPP_YIELD_PRE, yp);
    kprof_park();
    __asm__ volatile("cli");
    swtch(&current_proc->context, scheduler_ctx);
    __asm__ volatile("sti");
}

/* Monotonic counter assigning each sleep an enqueue order, so wake_up_n can wake
 * waiters oldest-first (FIFO) — matching Linux's futex plist wake order, which
 * glibc's condvar relies on (wake the waiter that "happened before" the signal).
 * Waking an arbitrary waiter feeds glibc-2.36's BZ#25847 signal-steal. */
static uint32_t g_sleep_seq = 0;

int sleep_on(void *chan) {
    if (!current_proc) return 0;
    current_proc->sleep_chan = chan;
    current_proc->sleep_tick = pit_ticks();
    current_proc->sleep_seq  = ++g_sleep_seq;
    current_proc->sleep_timed_out = 0;
    current_proc->vr_slept  = 1;
    current_proc->vr_sleep_t0 = clock_mono_ns();
    current_proc->state     = PROC_SLEEPING;
    int slp_sys = current_proc->last_syscall;
    uint64_t slp_t0 = kprof_sleep_begin();
    kprof_park();
    __asm__ volatile("cli");
    swtch(&current_proc->context, scheduler_ctx);
    __asm__ volatile("sti");
    kprof_sleep_end(slp_t0, slp_sys);
    /* Every wake path clears wake_tick, but make it unconditional here so a
     * deadline set for THIS sleep can never fire into a later untimed sleep
     * (Linux timeouts are per call: a stale one is simply not a thing). */
    current_proc->wake_tick = 0;
    int timed_out = current_proc->sleep_timed_out;
    current_proc->sleep_timed_out = 0;
    return timed_out;
}

void proc_stop_self(void) {
    if (!current_proc) return;
    current_proc->vr_slept = 1;
    current_proc->vr_sleep_t0 = clock_mono_ns();
    current_proc->state = PROC_STOPPED;
    kprof_park();
    __asm__ volatile("cli");
    swtch(&current_proc->context, scheduler_ctx);
    __asm__ volatile("sti");
}

void wake_up(void *chan) {
    for (int i = 0; i < ptable_hwm; i++) {
        struct proc *p = &ptable[i];
        if (p->state == PROC_SLEEPING && p->sleep_chan == chan) {
            p->sleep_chan = (void *)0;
            p->wake_tick  = 0;
            sched_make_runnable(p);
        }
    }
}

/* Wake up to n waiters on `chan`, OLDEST-FIRST (ascending sleep_seq) — FIFO, as
 * Linux's futex wakes its plist chain in enqueue order.  glibc's condvar (and its
 * BZ#25847 signal-steal logic) depends on the waiter that arrived BEFORE the
 * signal being the one woken; waking an arbitrary ptable-order waiter caused our
 * startup stalls.  For a full wake (n huge) order is irrelevant, so wake all in
 * one pass; for a bounded n (the common condvar signal: n==1) pick the n oldest. */
int wake_up_n(void *chan, int n) {
    int woken = 0;
    if (n <= 0) return 0;

    /* count matching waiters; if n covers them all, order doesn't matter */
    int matches = 0;
    for (int i = 0; i < ptable_hwm; i++)
        if (ptable[i].state == PROC_SLEEPING && ptable[i].sleep_chan == chan)
            matches++;

    if (n >= matches) {                 /* wake all matching (single pass) */
        for (int i = 0; i < ptable_hwm; i++) {
            struct proc *p = &ptable[i];
            if (p->state == PROC_SLEEPING && p->sleep_chan == chan) {
                p->sleep_chan = (void *)0;
                p->wake_tick  = 0;
                sched_make_runnable_sync(p);
                woken++;
            }
        }
    } else {                            /* wake the n OLDEST (min sleep_seq) */
        while (woken < n) {
            struct proc *best = (void *)0;
            for (int i = 0; i < ptable_hwm; i++) {
                struct proc *p = &ptable[i];
                if (p->state == PROC_SLEEPING && p->sleep_chan == chan &&
                    (!best || p->sleep_seq < best->sleep_seq))
                    best = p;
            }
            if (!best) break;
            best->sleep_chan = (void *)0;
            best->wake_tick  = 0;
            sched_make_runnable_sync(best);
            woken++;
        }
    }
    if (woken > 0) kprof_add(KPE_WAKE, (uint32_t)woken);
    return woken;
}

/* Address-space-scoped variant of wake_up_n: only wakes threads of `tgid`.
 * See scheduler.h — used for CLEARTID/robust-futex wakes on user vaddrs so a
 * same-vaddr waiter in ANOTHER process is never mis-woken (glibc-2.36 condvar
 * signal-steal).  Oldest-first, like wake_up_n. */
int wake_up_n_tgid(void *chan, int n, int tgid) {
    int woken = 0;
    if (n <= 0) return 0;
    while (woken < n) {
        struct proc *best = (void *)0;
        for (int i = 0; i < ptable_hwm; i++) {
            struct proc *p = &ptable[i];
            if (p->state == PROC_SLEEPING && p->sleep_chan == chan &&
                p->tgid == tgid && (!best || p->sleep_seq < best->sleep_seq))
                best = p;
        }
        if (!best) break;
        best->sleep_chan = (void *)0;
        best->wake_tick  = 0;
        sched_make_runnable_sync(best);
        woken++;
    }
    return woken;
}

/*
 * Linux checks TIF_NEED_RESCHED on every kernel→user exit and reschedules if
 * set.  We mirror that: the syscall dispatcher calls this at the return-to-user
 * boundary (after all syscall work + signal delivery, NOT mid-syscall, so no
 * non-reentrant kernel state is in flight).  need_resched is set when a wake
 * made a thread runnable that should displace this one (sched_wakeup: the
 * wakee is SCHED_WAKEUP_GRAN ahead in vruntime — the usual case for a thread
 * that was blocked), or when the tick ended the slice; and a futex or
 * wake_up_n wake of threads no idle CPU took queues the waker behind them and
 * yields (sync wake, see sched_wakeup).  Guarded by no_preempt so lwIP and other non-reentrant sections
 * are never interrupted. */
void resched_on_return(void) {
    struct cpu *me = &cpus[this_cpu_id()];
    uint64_t wvr = me->wake_vr;
    me->wake_vr = 0;
    if (!current_proc || current_proc->no_preempt) return;
    if (wvr) {
        /* Sync wake: queue behind the woken threads, then let them run. */
        uint64_t cv = cpu_curr_vr(this_cpu_id(), clock_mono_ns());
        uint64_t d = wvr > cv ? wvr - cv : 0;
        if (d > SCHED_SLICE_NS) d = SCHED_SLICE_NS;   /* the skip does the rest */
        current_proc->vruntime += d;
        me->need_resched = 0;
        sched_handoffs++;
        kprof_count(KPE_RESCHED);
        yield();
        return;
    }
    if (me->need_resched) sched_irq_exit(1);
}

int io_activity;

void io_wake(void) {
    wake_up(&io_activity);
}

/* True iff the 4 bytes at user vaddr `a` are backed by a present page in the
 * CURRENTLY active address space (via the recursive PDE/PTE mapping).  Used by
 * proc_exit to avoid faulting on a dying thread's already-unmapped TLS/robust
 * pages during a mass SIGKILL teardown (a raw kernel-mode deref there panics).
 * Note: a 4-byte read can straddle a page boundary, so check both ends. */
static int user_word_present(uint32_t a) {
    if (a >= 0xC0000000U || (a + 3) >= 0xC0000000U) return 0;
    if (!(pte_read(a) & PAGE_PRESENT)) return 0;
    if (((a & 0xFFF) > 0xFFC) &&
        !(pte_read(a + 3) & PAGE_PRESENT))
        return 0;
    return 1;
}

/* One robust-futex word at exit: if the dying thread `tid` owns it, mark it
 * FUTEX_OWNER_DIED (keeping FUTEX_WAITERS) and wake a waiter.  The word is
 * user memory, so it is read and written with the fault-safe copies. */
static void robust_owner_died(uint32_t fa, uint32_t tid) {
    uint32_t fv;
    if (fa >= 0xC0000000U || !user_word_present(fa)) return;
    if (copy_from_user(&fv, (void *)(uintptr_t)fa, sizeof(fv)) < 0) return;
    if ((fv & 0x3FFFFFFFU) != tid) return;
    uint32_t nv = (fv & 0x80000000U) | 0x40000000U;   /* keep WAITERS, set OWNER_DIED */
    if (copy_to_user((void *)(uintptr_t)fa, &nv, sizeof(nv)) < 0) return;
    /* tgid-scoped: robust-mutex waiters share our address space */
    if (fv & 0x80000000U)
        wake_up_n_tgid((void *)(uintptr_t)fa, 1, current_proc->tgid);
}

void proc_exit(int status) {
    __asm__ volatile("cli");
    if (!current_proc) for (;;) __asm__ volatile("hlt");

    /* CLONE_VFORK: if a parent is blocked waiting for us to exec-or-exit, wake
     * it now (we're exiting without having exec'd, e.g. a failed child spawn). */
    if (current_proc->vfork_parent) {
        struct proc *vp = current_proc->vfork_parent;
        current_proc->vfork_parent = NULL;
        vp->vfork_waiting = 0;
        wake_up((void *)&vp->vfork_waiting);
    }
    current_proc->vm_owner = NULL;

    /* CLONE_CHILD_CLEARTID: a joinable thread is exiting — zero its tid word
     * and futex-wake any pthread_join() waiter.  The address space is shared
     * and still mapped (other threads hold pgdir refs), so the write lands in
     * the same memory the joiner is polling.  BUT when the watchdog SIGKILLs a
     * whole process tree, a thread can reach proc_exit after its own TLS/robust
     * pages are already unmapped (teardown races) — a raw deref then faults in
     * KERNEL mode and PANICS the box.  Gate every user deref on the page being
     * present (checks the dying proc's live pgdir via the recursive mapping),
     * and store through copy_to_user(): present is not writable, and a tid
     * pointer into a read-only page must not fault in ring 0 either. */
    if (current_proc->clear_child_tid) {
        uint32_t *ctid = (uint32_t *)(uintptr_t)current_proc->clear_child_tid;
        uint32_t zero = 0;
        if ((uintptr_t)ctid < 0xC0000000U && user_word_present((uint32_t)(uintptr_t)ctid) &&
            copy_to_user(ctid, &zero, sizeof(zero)) == 0) {
            /* Scope to OUR thread group: pthread_join waits are intra-process,
             * and an unscoped wake on this user vaddr mis-wakes same-vaddr
             * futex waiters in other processes (ASLR off → same layout). */
            wake_up_n_tgid((void *)ctid, 0x7fffffff, current_proc->tgid);
        }
        current_proc->clear_child_tid = 0;
    }

    /* Robust-futex death handling: glibc keeps a per-thread linked list of the
     * robust mutexes this thread currently holds.  If the thread exits while
     * holding one, the kernel must mark that futex FUTEX_OWNER_DIED and wake a
     * waiter, or every other thread blocking on it deadlocks forever.  The
     * address space is still mapped here, so we walk it — every word through
     * copy_from_user/copy_to_user, since the list is entirely user-controlled
     * and a present page may still be read-only (a ring-0 store would panic). */
    if (current_proc->robust_list_head &&
        current_proc->robust_list_head < 0xC0000000U &&
        user_word_present(current_proc->robust_list_head) &&
        user_word_present(current_proc->robust_list_head + 8)) {
        uint32_t headp = current_proc->robust_list_head;
        uint32_t hdr[3];                           /* list, futex_offset, op_pending */
        if (copy_from_user(hdr, (void *)(uintptr_t)headp, sizeof(hdr)) == 0) {
            uint32_t list   = hdr[0];
            int32_t  offset = (int32_t)hdr[1];
            uint32_t pend   = hdr[2];
            uint32_t tid    = (uint32_t)current_proc->pid & 0x3FFFFFFFU;
            uint32_t cur    = list;
            int limit       = 2048;
            while (cur && cur != headp && cur < 0xC0000000U && limit-- > 0) {
                robust_owner_died((uint32_t)(cur + (uint32_t)offset), tid);  /* offset may be negative */
                if (!user_word_present(cur)) break;       /* teardown race */
                if (copy_from_user(&cur, (void *)(uintptr_t)cur, sizeof(cur)) < 0)
                    break;                                /* → next entry */
            }
            if (pend && pend < 0xC0000000U)
                robust_owner_died((uint32_t)(pend + (uint32_t)offset), tid);
        }
        current_proc->robust_list_head = 0;
    }

    /* Exit status, wait-encoded like Linux (exit code << 8, or the signal
     * number): the callers pass it in that form.  Once a group exit has fixed
     * the process's status on the leader (exit_group, fatal signal), a later
     * death of the leader itself (SIGKILL from thread_group_kill) must not
     * overwrite it: waitpid reports signal->group_exit_code in Linux. */
    if (!current_proc->group_exit)
        current_proc->exit_status = status;
    current_proc->state = PROC_ZOMBIE;

    vma_clear(current_proc);   /* free demand-paged anon VMAs (no-op for threads) */

    if (current_proc->sid == current_proc->pid && current_proc->ctty)
        devfs_session_tty_hangup(current_proc->sid, current_proc->ctty);
    /* The serial console is never held in ->ctty (see devfs.c tty_sid). */
    if (current_proc->sid == current_proc->pid)
        devfs_console_session_exit(current_proc->sid);

    /* Drop this thread's reference to the shared fd table.  The fds are closed
     * only when the last thread of the group exits (refcount → 0) — a thread
     * exiting must NOT close fds its siblings still use. */
    fdtable_put(current_proc);
    if (current_proc->ctty) {
        vfs_close(current_proc->ctty);
        current_proc->ctty = (void *)0;
    }
    if (current_proc->root_node) {           /* chroot(2)'s pinned root */
        vfs_close(current_proc->root_node);
        current_proc->root_node = (void *)0;
    }
    seccomp_release(current_proc);
    /* Likewise the shared handler table. */
    sighand_put(current_proc->sighand);
    current_proc->sighand = (struct sighand *)0;
    /* Process-wide pending signals stay with the process; one this thread
     * would have taken is handed to a sibling that can (Linux exit_signals). */
    signal_retarget_shared(current_proc, ~0u);
    /* If the rest of the group is stopped, this exit completes a group stop. */
    signal_group_stop_check(current_proc);
    sigshared_put(current_proc->sigshared);
    current_proc->sigshared = (struct sigshared *)0;

    /* Drop the address space's shm attachments if this was the last thread
     * running in it (frame refs are released when the pgdir is torn down at
     * reap time). */
    shm_proc_exit(current_proc);

    /* Linux exit_notify(): children belong to the PROCESS, and the parent is
     * told about the PROCESS.  A non-leader thread exiting says nothing to
     * anyone (no SIGCHLD, nothing to wait for) unless it was the last thread
     * of a group whose leader already exited, in which case the leader's death
     * becomes reportable now.  A leader exiting while siblings live likewise
     * stays a zombie in silence until the last sibling is gone
     * (kernel/exit.c release_task -> do_notify_parent(leader)). */
    struct proc *leader   = proc_group_leader(current_proc);
    int          is_leader = (current_proc->pid == current_proc->tgid);
    int          notify    = 0;
    if (is_leader) {
        notify = proc_group_empty(current_proc);
    } else if (leader && leader != current_proc && leader->state == PROC_ZOMBIE) {
        notify = proc_group_empty(leader);
    }

    /* The process is gone once its last thread is: so are its timers. */
    if (notify && leader)
        ktimer_group_exit(leader->tgid);

    if (notify && leader) {
        /* The process is gone: reparent its children to init (PID 1) and free
         * any orphan zombies now parented to init — init reaps orphans but can
         * be blocked in a per-child waitpid() while a burst of them (a watchdog
         * SIGKILL of a process tree) accumulates and exhausts the table. */
        struct proc *init = (void *)0;
        for (int i = 0; i < ptable_hwm; i++)
            if (ptable[i].pid == 1 && ptable[i].state != PROC_UNUSED) { init = &ptable[i]; break; }
        for (int i = 0; i < ptable_hwm; i++)
            if (ptable[i].state != PROC_UNUSED && ptable[i].parent == leader)
                ptable[i].parent = init;
        { extern void reap_orphan_zombies(void); reap_orphan_zombies(); }

        /* SIGCHLD to the parent PROCESS (any thread of it that does not block
         * it), and wake whichever of its threads sleeps in waitpid. */
        if (leader->parent) {
            signal_send_group(leader->parent, SIGCHLD);
            wake_up(leader->parent);
        }
    }

    kprof_park();
    swtch(&current_proc->context, scheduler_ctx);
    for (;;) __asm__ volatile("hlt");
}
