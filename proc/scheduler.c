#include "scheduler.h"
#include "process.h"
#include "signal.h"

extern void vma_clear(struct proc *p);   /* free demand-paged VMAs (syscall.c) */
#include "pipe.h"
#include "usocket.h"
#include "shm.h"
#include "../fs/devfs.h"
#include "../fs/vfs.h"
#include "../net/socket.h"
#include "../arch/i686/cpu/tss.h"
#include "../arch/i686/cpu/gdt.h"
#include "../arch/i686/cpu/fpu.h"
#include "../arch/i686/cpu/pit.h"
#include "../arch/i686/cpu/percpu.h"
#include "../arch/i686/mm/paging.h"
#include "../mm/heap.h"
#include "../kernel/printk.h"
#include <kernel/config.h>
#include <stdint.h>
#include <stddef.h>

/* swtch is defined in swtch.asm */
extern void swtch(struct context **old, struct context *new_ctx);

/* The scheduler's own saved context is PER-CPU (each CPU runs its own scheduler
 * loop).  `scheduler_ctx` expands to the calling CPU's slot, so the existing
 * swtch(&scheduler_ctx, ...) / swtch(..., scheduler_ctx) all act per-CPU. */
#define scheduler_ctx (cpus[this_cpu_id()].sched_ctx)

#define DEFAULT_TIMESLICE  2   /* ticks per quantum (20ms): lower round-robin
                                * latency for multithreaded/interactive apps.
                                * (Verified NOT the cause of the GTK heap race —
                                * it reproduces identically at 50ms.) */

void scheduler_init(void) {
    /* Nothing to initialize — ptable is already set up by proc_init */
}

void scheduler_start(void) {
    for (;;) {
        int ran = 0;

        for (int i = 0; i < MAX_PROCS; i++) {
            struct proc *p = &ptable[i];
            if (p->state != PROC_RUNNABLE) continue;

            ran = 1;
            /* DEBUG (SMP S4): prove each CPU actually dispatches work. */
            {
                static int seen[8];
                int c = (int)this_cpu_id();
                if (c > 0 && c < 8 && !seen[c]) {
                    seen[c] = 1;
                    printk("[SMP]  CPU %d dispatched pid=%d (AP scheduling live)\n", c, p->pid);
                }
            }
            current_proc   = p;
            p->state       = PROC_RUNNING;
            p->time_slice  = DEFAULT_TIMESLICE;
            p->sched_count++;

            tss_set_kernel_stack((uint32_t)(uintptr_t)(p->kstack + KSTACKSIZE));
            /* ALWAYS reprogram this CPU's TLS (%gs) base — even to 0.  Skipping
             * it when p->tls_base==0 left the PREVIOUS thread's base in this CPU's
             * GDT entry 6; the next iret reloads %gs from it, so a no-TLS thread
             * would read/write another thread's TLS → wild-pointer corruption. */
            gdt_set_tls(p->tls_base);

            if (p->pgdir_phys)
                __asm__ volatile("mov %0, %%cr3" :: "r"(p->pgdir_phys) : "memory");

            fpu_restore(fpu_area(p));
            swtch(&scheduler_ctx, p->context);
            fpu_save(fpu_area(p));

            __asm__ volatile("mov %0, %%cr3" :: "r"(kernel_pgdir_phys) : "memory");
            current_proc = NULL;
            __asm__ volatile("sti");
        }

        /* Nothing runnable: halt until the next interrupt (PIT tick, key,
         * IRQ) instead of spinning — drops host CPU to ~0 when idle.  RELEASE
         * the Big Kernel Lock first so the other CPU(s) and the waking IRQ can
         * run kernel code; re-acquire on wake before re-scanning the shared
         * ptable. */
        if (!ran) {
            bkl_release();
            if (this_cpu_id() == 0) {
                /* BSP: woken by the PIT/keyboard/IRQ (all routed here via the
                 * PIC→LINT0).  hlt drops the host CPU to ~0 when idle. */
                __asm__ volatile("sti; hlt");
            } else {
                /* AP: the PIC delivers only to the BSP, but the AP DOES have
                 * its own LAPIC timer (S6) and takes IPIs, so service any TLB
                 * shootdown while idling and spin-back-off before re-scanning
                 * the shared ptable for newly-runnable work. */
                for (volatile int i = 0; i < 200000; i++) {
                    tlb_serve_pending();
                    __asm__ volatile("pause");
                }
            }
            bkl_acquire();
        }
    }
}

extern volatile int g_ipc_launch_started;   /* set when Firefox starts an IPC launch */
extern volatile uint32_t g_futex_progress_tick;  /* pit_ticks of last real futex handoff */

void scheduler_tick(int user_mode) {
    uint32_t now = pit_ticks();
    for (int i = 0; i < MAX_PROCS; i++) {
        struct proc *p = &ptable[i];
        if (p->state == PROC_SLEEPING && p->wake_tick &&
            (int32_t)(now - p->wake_tick) >= 0) {
            p->wake_tick = 0;
            p->sleep_chan = (void *)0;
            p->state = PROC_RUNNABLE;
        }
    }

    /* ── glibc-2.36 condvar lost-wakeup safety net (BZ#25847) ────────────────
     * The glibc on disk (2.36) has the pthread_cond_signal lost-wakeup bug
     * (fixed in glibc 2.41): "undoing stealing" across condvar group rotations
     * can deliver a signal to a g_signals group with NO waiter (seen as
     * FUTEX_WAKE woke=0 loops) while the real waiters sit in the other group —
     * the wakeup is lost and they park forever.  POSIX explicitly permits
     * spurious condvar wakeups and glibc re-checks its predicate in a loop, so
     * spuriously waking a parked futex waiter is ALWAYS safe — at worst it
     * re-sleeps.  Now that SMP runs the producer concurrently the predicate
     * genuinely gets set, so a spurious wake recovers the lost notification
     * (pre-SMP this failed because the producer itself was stuck).
     *
     * Fire only on a detected STALL: several futex waiters parked AND no
     * successful condvar handoff (FUTEX_WAKE woke>0) for ~100 ms.  This covers
     * every phase (content-process launch AND compositor), costs nothing during
     * normal operation, and self-terminates the instant progress resumes (a
     * recovered handoff bumps g_futex_progress_tick).  Covers BOTH timed
     * (pthread_cond_timedwait) and untimed waiters. */
    {
        /* PER-WAITER recovery: a global "no handoff" metric is masked by the
         * pipeline's background condvar traffic (~half the wakes still land,
         * woke>0), so detect the stall PER WAITER via how long it's been parked.
         * Spurious-wake any CONDVAR (op=9) waiter parked >= PARK_TICKS; glibc
         * re-checks its predicate, so for a BZ#25847 lost signal (predicate
         * already set by the now-concurrent producer) it proceeds, and for a
         * genuinely-idle waiter it just re-sleeps.  Self-rate-limited: re-sleep
         * resets sleep_tick, so a still-blocked waiter isn't re-pinged for
         * another PARK_TICKS.  Gated to >=4 condvar waiters so it stays off
         * during the smokes / idle single-threaded apps. */
        const uint32_t PARK_TICKS = 15;   /* 150 ms parked → re-deliver */
        int cwaiters = 0;
        for (int i = 0; i < MAX_PROCS; i++) {
            struct proc *p = &ptable[i];
            if (p->state == PROC_SLEEPING && p->futex_cond) cwaiters++;
        }
        /* Spurious-wake only CONDVAR (op=9) waiters: POSIX guarantees condvar
         * waiters re-check their predicate, so this can NEVER corrupt correct
         * code, and it recovers a genuine BZ#25847 lost signal (predicate set,
         * notification lost).  NB: waking mutex/raw-futex (op=0) waiters too was
         * tried (to chase a deeper mutex-domain ordering logjam) — the gentle
         * per-waiter form didn't advance and the aggressive global form crashed
         * (over-churn races), so it's NOT done: that residual stall is an
         * emergent scheduling/ordering issue, not a lost wakeup, and is properly
         * fixed by glibc 2.41 (see [[glibc-condvar-lost-wakeup]]). */
        if (cwaiters >= 4) {
            for (int i = 0; i < MAX_PROCS; i++) {
                struct proc *p = &ptable[i];
                if (p->state != PROC_SLEEPING || !p->futex_cond) continue;
                if ((uint32_t)(now - p->sleep_tick) < PARK_TICKS) continue;
                uint32_t ch = (uint32_t)(uintptr_t)p->sleep_chan;
                if (ch >= 0x1000 && ch < 0xC0000000U) {
                    p->sleep_chan = (void *)0;
                    p->wake_tick  = 0;
                    p->state      = PROC_RUNNABLE;
                }
            }
        }

        /* ── Aggressive multiprocess-deadlock recovery ───────────────────────
         * The remaining Firefox blocker is a multiprocess STARTUP deadlock: the
         * IPC-launch parent waits forever on a handshake condvar that a CHILD
         * thread should signal, but the child is itself wedged on a futex (a
         * glibc-2.36 condvar/mutex ordering issue under our scheduler) and never
         * signals → both sides park forever.  The op=9-only net above can't break
         * it: the stuck child waits on a MUTEX (op=0), and the parent's predicate
         * is genuinely unset (the signal never came).  So, ONLY while a Firefox
         * IPC launch is in flight AND nothing has handed off for a while, wake
         * EVERY long-parked futex waiter (op=0 mutex + op=9 condvar) so each
         * re-checks its predicate and re-sorts — forcing the wedged child to make
         * progress and eventually signal the parent.  Safe: every glibc futex
         * re-checks on wake and re-sleeps if its predicate is unmet (a mutex
         * waiter whose lock is still held just re-blocks).  This was tried before
         * and "crashed", but that was the now-fixed shared-address-space
         * corruption ([[smp-fill-before-map]]), not the wake itself — re-enabled
         * now that the corruption root cause is gone.  Gated tight (launch active
         * + 250 ms global stall) so it never runs during normal operation. */
        if (g_ipc_launch_started &&
            (uint32_t)(now - g_futex_progress_tick) >= 25) {   /* 250 ms no handoff */
            {   /* [wbt] dump: on a persistent stall, print each parked firefox
                 * thread's captured user backtrace (call-preceded libxul return
                 * addresses stashed at block time by fx_capture_wait_bt) for
                 * offline symbolization against the Mozilla .sym file.  Every
                 * 8th sweep ≈ one dump per ~2 s of continuous stall; capped. */
                static uint32_t wbt_sweeps = 0, wbt_dumps = 0;
                wbt_sweeps++;
                if ((wbt_sweeps % 8) == 1 && wbt_dumps < 10) {
                    wbt_dumps++;
                    for (int i = 0; i < MAX_PROCS; i++) {
                        struct proc *p = &ptable[i];
                        if (p->state != PROC_SLEEPING || !p->wait_bt_n) continue;
                        if (!(p->name[0]=='f' && p->name[1]=='i' &&
                              p->name[4]=='f')) continue;
                        printk("[wbt] pid=%d t%d cond=%d fx=%d chan=%x park=%u:",
                               p->pid, p->tgid, p->futex_cond, p->futex_wait,
                               (unsigned)(uintptr_t)p->sleep_chan,
                               (unsigned)(now - p->sleep_tick));
                        for (int k = 0; k < p->wait_bt_n; k++)
                            printk(" %x", (unsigned)p->wait_bt[k]);
                        printk("\n");
                    }
                }
            }
            const uint32_t AGG_PARK = 20;                       /* parked ≥200 ms */
            for (int i = 0; i < MAX_PROCS; i++) {
                struct proc *p = &ptable[i];
                if (p->state != PROC_SLEEPING || !p->futex_wait) continue;
                /* Wake ONLY MUTEX (op=0) waiters, never CONDVAR (op=9) ones.
                 * Waking a mutex waiter is provably safe: it re-checks the lock
                 * word and re-sleeps if still held, or acquires (data consistent
                 * because the previous holder released).  The wedged launch
                 * child is stuck on a MUTEX, so this breaks the deadlock.
                 * Spuriously waking a CONDVAR waiter, by contrast, can make it
                 * proceed past an ORDERING dependency before a sibling thread has
                 * initialised shared state → NULL deref (the content-process
                 * SIGSEGV seen when the blunt all-futex sweep broke through).
                 * Condvar waiters are still covered by the gentle op=9 net above
                 * (which only recovers genuine BZ#25847 lost signals). */
                if (p->futex_cond) continue;
                if ((uint32_t)(now - p->sleep_tick) < AGG_PARK) continue;
                uint32_t ch = (uint32_t)(uintptr_t)p->sleep_chan;
                if (ch >= 0x1000 && ch < 0xC0000000U) {
                    p->sleep_chan = (void *)0;
                    p->wake_tick  = 0;
                    p->state      = PROC_RUNNABLE;
                }
            }
            g_futex_progress_tick = now;   /* rate-limit: one sweep per stall window */
        }
    }

    if (!current_proc) return;
    current_proc->utime_ticks++;
    /* Never preempt kernel-mode execution (see pit_handler) or a held
     * preemption guard — both protect non-reentrant kernel state. */
    if (!user_mode || current_proc->no_preempt) return;
    if (--current_proc->time_slice <= 0)
        yield();
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

void yield(void) {
    if (!current_proc) return;
    current_proc->state = PROC_RUNNABLE;
    __asm__ volatile("cli");
    swtch(&current_proc->context, scheduler_ctx);
    __asm__ volatile("sti");
}

/* Monotonic counter assigning each sleep an enqueue order, so wake_up_n can wake
 * waiters oldest-first (FIFO) — matching Linux's futex plist wake order, which
 * glibc's condvar relies on (wake the waiter that "happened before" the signal).
 * Waking an arbitrary waiter feeds glibc-2.36's BZ#25847 signal-steal. */
static uint32_t g_sleep_seq = 0;

void sleep_on(void *chan) {
    if (!current_proc) return;
    current_proc->sleep_chan = chan;
    current_proc->sleep_tick = pit_ticks();
    current_proc->sleep_seq  = ++g_sleep_seq;
    current_proc->state     = PROC_SLEEPING;
    __asm__ volatile("cli");
    swtch(&current_proc->context, scheduler_ctx);
    __asm__ volatile("sti");
}

void proc_stop_self(void) {
    if (!current_proc) return;
    current_proc->state = PROC_STOPPED;
    __asm__ volatile("cli");
    swtch(&current_proc->context, scheduler_ctx);
    __asm__ volatile("sti");
}

void wake_up(void *chan) {
    for (int i = 0; i < MAX_PROCS; i++) {
        struct proc *p = &ptable[i];
        if (p->state == PROC_SLEEPING && p->sleep_chan == chan) {
            p->sleep_chan = (void *)0;
            p->wake_tick  = 0;
            p->state     = PROC_RUNNABLE;
        }
    }
}

/* Linux-style wakeup preemption (try_to_wake_up → check_preempt_curr): when a
 * waker makes another thread runnable, Linux can run it almost immediately.  Our
 * cooperative scheduler otherwise lets the waker keep running until its 20ms
 * quantum ends — a wake-to-run delay long enough for glibc-2.36's Riegel condvar
 * to "steal" a signal meant for the just-woken thread.  We don't preempt mid-
 * syscall (non-reentrant kernel); instead we set this flag and the syscall
 * dispatcher yields at the safe return-to-user boundary so the woken thread runs
 * next. */
volatile int g_resched_pending = 0;

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
    for (int i = 0; i < MAX_PROCS; i++)
        if (ptable[i].state == PROC_SLEEPING && ptable[i].sleep_chan == chan)
            matches++;

    if (n >= matches) {                 /* wake all matching (single pass) */
        for (int i = 0; i < MAX_PROCS; i++) {
            struct proc *p = &ptable[i];
            if (p->state == PROC_SLEEPING && p->sleep_chan == chan) {
                p->sleep_chan = (void *)0;
                p->wake_tick  = 0;
                p->state      = PROC_RUNNABLE;
                woken++;
            }
        }
    } else {                            /* wake the n OLDEST (min sleep_seq) */
        while (woken < n) {
            struct proc *best = (void *)0;
            for (int i = 0; i < MAX_PROCS; i++) {
                struct proc *p = &ptable[i];
                if (p->state == PROC_SLEEPING && p->sleep_chan == chan &&
                    (!best || p->sleep_seq < best->sleep_seq))
                    best = p;
            }
            if (!best) break;
            best->sleep_chan = (void *)0;
            best->wake_tick  = 0;
            best->state      = PROC_RUNNABLE;
            woken++;
        }
    }
    if (woken > 0) g_resched_pending = 1;
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
        for (int i = 0; i < MAX_PROCS; i++) {
            struct proc *p = &ptable[i];
            if (p->state == PROC_SLEEPING && p->sleep_chan == chan &&
                p->tgid == tgid && (!best || p->sleep_seq < best->sleep_seq))
                best = p;
        }
        if (!best) break;
        best->sleep_chan = (void *)0;
        best->wake_tick  = 0;
        best->state      = PROC_RUNNABLE;
        woken++;
    }
    if (woken > 0) g_resched_pending = 1;
    return woken;
}

/*
 * Linux checks TIF_NEED_RESCHED on every kernel→user exit and reschedules if
 * set.  We mirror that: the syscall dispatcher calls this at the return-to-user
 * boundary (after all syscall work + signal delivery, NOT mid-syscall, so no
 * non-reentrant kernel state is in flight).  If a wake happened during this
 * syscall (g_resched_pending), the waker yields so the just-woken thread runs
 * promptly — closing the wake-to-run gap that lets glibc-2.36's Riegel condvar
 * steal a signal from the IPC Launch thread.  Guarded by no_preempt so lwIP and
 * other non-reentrant sections are never interrupted. */
extern volatile int g_ipc_launch_started;
void resched_on_return(void) {
    if (!current_proc || current_proc->no_preempt) return;
    if (!g_resched_pending) return;
    g_resched_pending = 0;
    /* Only apply wakeup-preemption during Firefox's content-process launch.
     * Linux preempts on wake only for higher-priority tasks; doing it for every
     * equal-priority round-robin wake system-wide over-yields and disturbs boot
     * / disk I/O.  Gating on the launch phase (same as the scheduler_tick
     * self-heal) confines it to exactly where the glibc condvar steal hurts. */
    if (!g_ipc_launch_started) return;
    yield();
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
    if (!(*paging_get_pde(a) & 1) || !(*paging_get_pte(a) & 1)) return 0;
    if (((a & 0xFFF) > 0xFFC) &&
        (!(*paging_get_pde(a + 3) & 1) || !(*paging_get_pte(a + 3) & 1)))
        return 0;
    return 1;
}

void proc_exit(int status) {
    __asm__ volatile("cli");
    if (!current_proc) for (;;) __asm__ volatile("hlt");

    /* [ff-exit-trace] When a firefox thread exits, dump its user EIP + a scan of
     * its stack for libxul/code return addresses.  If the thread running a
     * std::call_once static init exits mid-init, the once-flag stays stuck and
     * the main thread (a contender) hangs forever — this catches that case.
     * The exiting thread's pgdir is still active, so we read the stack directly. */
    if (current_proc->tf) {
        const char *nm = current_proc->name;
        int isff = (nm[0]=='f'&&nm[1]=='i'&&nm[2]=='r'&&nm[3]=='e'&&nm[4]=='f');
        if (isff) {
            printk("[ff-exit] pid=%d tgid=%d status=%d eip=%x stk:",
                   current_proc->pid, current_proc->tgid, status,
                   (unsigned)current_proc->tf->eip);
            uint32_t sp = current_proc->tf->useresp & ~3U;
            int n = 0;
            for (uint32_t a = sp; a < sp + 2048 && n < 16; a += 4) {
                if (!(*paging_get_pde(a) & 1)) { a = (a & ~0x3FFFFFU) + 0x400000U - 4; continue; }
                if (!(*paging_get_pte(a) & 1)) continue;
                uint32_t v = *(volatile uint32_t *)(uintptr_t)a;
                if (v >= 0x10000000U && v < 0x60000000U) { printk(" %x", (unsigned)v); n++; }
            }
            printk("\n");
        }
    }

    /* CLONE_VFORK: if a parent is blocked waiting for us to exec-or-exit, wake
     * it now (we're exiting without having exec'd, e.g. a failed child spawn). */
    if (current_proc->vfork_parent) {
        struct proc *vp = current_proc->vfork_parent;
        current_proc->vfork_parent = NULL;
        vp->vfork_waiting = 0;
        wake_up((void *)&vp->vfork_waiting);
    }

    /* CLONE_CHILD_CLEARTID: a joinable thread is exiting — zero its tid word
     * and futex-wake any pthread_join() waiter.  The address space is shared
     * and still mapped (other threads hold pgdir refs), so the write lands in
     * the same memory the joiner is polling.  BUT when the watchdog SIGKILLs a
     * whole process tree, a thread can reach proc_exit after its own TLS/robust
     * pages are already unmapped (teardown races) — a raw deref then faults in
     * KERNEL mode and PANICS the box.  Gate every user deref on the page being
     * present (checks the dying proc's live pgdir via the recursive mapping). */
    if (current_proc->clear_child_tid) {
        uint32_t *ctid = (uint32_t *)(uintptr_t)current_proc->clear_child_tid;
        if ((uintptr_t)ctid < 0xC0000000U && user_word_present((uint32_t)(uintptr_t)ctid)) {
            *ctid = 0;
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
     * address space is still mapped here, so we walk it directly. */
    if (current_proc->robust_list_head &&
        current_proc->robust_list_head < 0xC0000000U &&
        user_word_present(current_proc->robust_list_head) &&
        user_word_present(current_proc->robust_list_head + 8)) {
        uint32_t headp = current_proc->robust_list_head;
        uint32_t list   = *(volatile uint32_t *)(uintptr_t)headp;        /* head->list */
        int32_t  offset = *(volatile int32_t  *)(uintptr_t)(headp + 4);  /* futex_offset */
        uint32_t pend   = *(volatile uint32_t *)(uintptr_t)(headp + 8);  /* op_pending */
        uint32_t tid    = (uint32_t)current_proc->pid & 0x3FFFFFFFU;
        uint32_t cur    = list;
        int limit       = 2048;
        while (cur && cur != headp && cur < 0xC0000000U && limit-- > 0) {
            uint32_t fa = cur + (uint32_t)offset;     /* offset may be negative */
            if (fa < 0xC0000000U && user_word_present(fa)) {
                uint32_t fv = *(volatile uint32_t *)(uintptr_t)fa;
                if ((fv & 0x3FFFFFFFU) == tid) {
                    *(volatile uint32_t *)(uintptr_t)fa =
                        (fv & 0x80000000U) | 0x40000000U;  /* keep WAITERS, set OWNER_DIED */
                    /* tgid-scoped: robust-mutex waiters share our address space */
                    if (fv & 0x80000000U)
                        wake_up_n_tgid((void *)(uintptr_t)fa, 1, current_proc->tgid);
                }
            }
            if (!user_word_present(cur)) break;           /* teardown race */
            cur = *(volatile uint32_t *)(uintptr_t)cur;   /* → next entry */
        }
        if (pend && pend < 0xC0000000U) {
            uint32_t fa = pend + (uint32_t)offset;
            if (fa < 0xC0000000U && user_word_present(fa)) {
                uint32_t fv = *(volatile uint32_t *)(uintptr_t)fa;
                if ((fv & 0x3FFFFFFFU) == tid) {
                    *(volatile uint32_t *)(uintptr_t)fa =
                        (fv & 0x80000000U) | 0x40000000U;
                    if (fv & 0x80000000U)
                        wake_up_n_tgid((void *)(uintptr_t)fa, 1, current_proc->tgid);
                }
            }
        }
        current_proc->robust_list_head = 0;
    }

    current_proc->exit_status = status;
    current_proc->state       = PROC_ZOMBIE;

    vma_clear(current_proc);   /* free demand-paged anon VMAs (no-op for threads) */

    if (current_proc->sid == current_proc->pid && current_proc->ctty)
        devfs_session_tty_hangup(current_proc->sid, current_proc->ctty);

    /* Drop this thread's reference to the shared fd table.  The fds are closed
     * only when the last thread of the group exits (refcount → 0) — a thread
     * exiting must NOT close fds its siblings still use. */
    fdtable_put(current_proc);
    if (current_proc->ctty) {
        vfs_close(current_proc->ctty);
        current_proc->ctty = (void *)0;
    }

    /* Drop shared-memory bookkeeping (frame refs released when the pgdir is
     * torn down at reap time). */
    shm_proc_cleanup(current_proc);

    /* Reparent children to init (PID 1) */
    struct proc *init = (void *)0;
    for (int i = 0; i < MAX_PROCS; i++)
        if (ptable[i].pid == 1) { init = &ptable[i]; break; }
    for (int i = 0; i < MAX_PROCS; i++)
        if (ptable[i].parent == current_proc)
            ptable[i].parent = init;

    /* Promptly free any orphan zombies now parented to init — init reaps
     * orphans but can be blocked in a per-child waitpid() while a burst of them
     * (a watchdog SIGKILL of Firefox's process tree) accumulates and exhausts
     * the process table. */
    { extern void reap_orphan_zombies(void); reap_orphan_zombies(); }

    /* Notify parent */
    if (current_proc->parent) {
        signal_send(current_proc->parent, SIGCHLD);
        wake_up(current_proc->parent);  /* wake parent from waitpid sleep */
    }

    swtch(&current_proc->context, scheduler_ctx);
    for (;;) __asm__ volatile("hlt");
}
