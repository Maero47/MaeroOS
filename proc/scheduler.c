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
#include <kernel/kprof.h>
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
        uint64_t scan_t0 = kprof_probe_begin();

        for (int i = 0; i < MAX_PROCS; i++) {
            struct proc *p = &ptable[i];
            if (p->state != PROC_RUNNABLE) continue;

            ran = 1;
            kprof_probe_end(KPP_SCHED_SCAN, scan_t0);
            uint64_t disp_t0 = kprof_probe_begin();
            current_proc   = p;
            p->state       = PROC_RUNNING;
            p->time_slice  = DEFAULT_TIMESLICE;
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
            swtch(&scheduler_ctx, p->context);
            /* Back in the scheduler: the outgoing thread already parked its own
             * bucket and left KPB_SCHED current (see kprof_park below). */
            fpu_save(fpu_area(p));

            __asm__ volatile("mov %0, %%cr3" :: "r"(kernel_pgdir_phys) : "memory");
            current_proc = NULL;
            /* A non-leader thread that just exited is released here, on the
             * scheduler's own stack, now that its kernel stack is no longer in
             * use.  Linux release_task()s such threads immediately in
             * exit_notify(): they are never reported by wait(), only the group
             * leader is (once every thread is gone). */
            if (p->state == PROC_ZOMBIE && p->pid != p->tgid)
                proc_release(p);
            __asm__ volatile("sti");
            scan_t0 = kprof_probe_begin();
        }

        /* Nothing runnable: halt until the next interrupt (PIT tick, key,
         * IRQ) instead of spinning — drops host CPU to ~0 when idle.  RELEASE
         * the Big Kernel Lock first so the other CPU(s) and the waking IRQ can
         * run kernel code; re-acquire on wake before re-scanning the shared
         * ptable. */
        if (!ran) {
            int kp_old = kprof_switch(KPB_IDLE);
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
            kprof_switch(kp_old);
        }
    }
}

void scheduler_tick(int user_mode) {
    uint32_t now = pit_ticks();
    for (int i = 0; i < MAX_PROCS; i++) {
        struct proc *p = &ptable[i];
        if (p->state == PROC_SLEEPING && p->wake_tick &&
            (int32_t)(now - p->wake_tick) >= 0) {
            /* Deadline expiry is the only wake that means "timed out"; record
             * it so sleep_on() can tell its caller (FUTEX_WAIT -> -ETIMEDOUT,
             * poll/select -> 0) instead of the caller guessing from the clock. */
            p->wake_tick = 0;
            p->sleep_chan = (void *)0;
            p->sleep_timed_out = 1;
            p->state = PROC_RUNNABLE;
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
    current_proc->state = PROC_STOPPED;
    kprof_park();
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
    if (woken > 0) { g_resched_pending = 1; kprof_add(KPE_WAKE, (uint32_t)woken); }
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
 * promptly (Linux try_to_wake_up -> check_preempt_curr).  Applies to every
 * wake, not just to one application's launch phase.  Guarded by no_preempt so
 * lwIP and other non-reentrant sections are never interrupted. */
void resched_on_return(void) {
    if (!current_proc || current_proc->no_preempt) return;
    if (!g_resched_pending) return;
    g_resched_pending = 0;
    kprof_count(KPE_RESCHED);
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

    /* Drop this thread's reference to the shared fd table.  The fds are closed
     * only when the last thread of the group exits (refcount → 0) — a thread
     * exiting must NOT close fds its siblings still use. */
    fdtable_put(current_proc);
    if (current_proc->ctty) {
        vfs_close(current_proc->ctty);
        current_proc->ctty = (void *)0;
    }
    /* Likewise the shared handler table. */
    sighand_put(current_proc->sighand);
    current_proc->sighand = (struct sighand *)0;

    /* Drop shared-memory bookkeeping (frame refs released when the pgdir is
     * torn down at reap time). */
    shm_proc_cleanup(current_proc);

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

    if (notify && leader) {
        /* The process is gone: reparent its children to init (PID 1) and free
         * any orphan zombies now parented to init — init reaps orphans but can
         * be blocked in a per-child waitpid() while a burst of them (a watchdog
         * SIGKILL of a process tree) accumulates and exhausts the table. */
        struct proc *init = (void *)0;
        for (int i = 0; i < MAX_PROCS; i++)
            if (ptable[i].pid == 1 && ptable[i].state != PROC_UNUSED) { init = &ptable[i]; break; }
        for (int i = 0; i < MAX_PROCS; i++)
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
