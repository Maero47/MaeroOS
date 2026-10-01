#include "signal.h"
#include "process.h"
#include "scheduler.h"
#include "../arch/i686/mm/paging.h"   /* paging_get_pde/pte for sigframe prefault */
#include "syscall.h"                   /* copy_to_user / copy_from_user */

#include "../kernel/printk.h"
#include <registers.h>
#include <kernel/config.h>
#include <stdint.h>

/*
 * Linux i386 sigcontext / ucontext, exactly as glibc's <sys/ucontext.h>
 * overlays them.  SA_SIGINFO handlers (3-arg form) receive a pointer to this
 * ucontext as their third argument and inspect uc_mcontext to read the faulting
 * register state — e.g. SpiderMonkey's WasmTrapHandler reads
 * uc_mcontext.gregs[REG_EIP] (offset 20 + 56 = 0x4c) and Breakpad's handler
 * copies the whole mcontext.  Passing NULL (as we did) makes every such handler
 * dereference address 0x4c and crash — which is precisely the FF91 startup
 * fault.  Segment registers are stored as 16-bit + 16-bit high padding so the
 * 32-bit gregs[] view lines up.
 */
struct k_sigcontext {
    uint16_t gs, __gsh;
    uint16_t fs, __fsh;
    uint16_t es, __esh;
    uint16_t ds, __dsh;
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
    uint32_t trapno, err, eip;
    uint16_t cs, __csh;
    uint32_t eflags, esp_at_signal;
    uint16_t ss, __ssh;
    uint32_t fpstate;       /* struct _fpstate* — NULL (no FPU state) */
    uint32_t oldmask;
    uint32_t cr2;           /* faulting address for SIGSEGV */
} __attribute__((packed));

struct k_ucontext {
    uint32_t uc_flags;
    uint32_t uc_link;
    uint32_t ss_sp;         /* uc_stack.ss_sp */
    uint32_t ss_flags;      /* uc_stack.ss_flags */
    uint32_t ss_size;       /* uc_stack.ss_size */
    struct k_sigcontext uc_mcontext;
    uint8_t  uc_sigmask[128];
    /* glibc's ucontext_t continues past uc_sigmask with __fpregs_mem (a
     * struct _libc_fpstate, 112 bytes) and __ssp[4] (32 bytes).  A handler that
     * touches those fields must land inside THIS struct, not bleed into the
     * adjacent saved trapframe above it on the stack — so reserve the full
     * trailing area (rounded up).  All zeroed (no FPU state captured). */
    uint8_t  __fpregs_mem[112];
    uint8_t  __ssp[32];
    uint8_t  __pad[64];
} __attribute__((packed));

#include "../mm/heap.h"

/* ── Shared signal-handler table ─────────────────────────────────────────── */

struct sighand *sighand_alloc(void) {
    struct sighand *sh = (struct sighand *)kmalloc(sizeof(*sh));
    if (!sh) return (struct sighand *)0;
    __builtin_memset(sh, 0, sizeof(*sh));
    sh->refcount = 1;
    return sh;
}

struct sighand *sighand_copy(struct sighand *src) {
    struct sighand *sh = sighand_alloc();
    if (!sh) return sh;
    if (src) {
        __builtin_memcpy(sh->handlers, src->handlers, sizeof(sh->handlers));
        __builtin_memcpy(sh->flags,    src->flags,    sizeof(sh->flags));
        __builtin_memcpy(sh->mask,     src->mask,     sizeof(sh->mask));
    }
    return sh;
}

void sighand_put(struct sighand *sh) {
    if (!sh) return;
    if (--sh->refcount > 0) return;
    kfree(sh);
}

/* ── Process-wide signal state ───────────────────────────────────────────── */

struct sigshared *sigshared_alloc(void) {
    struct sigshared *ss = (struct sigshared *)kmalloc(sizeof(*ss));
    if (!ss) return ss;
    ss->refcount = 1;
    ss->pending  = 0;
    return ss;
}

void sigshared_put(struct sigshared *ss) {
    if (!ss) return;
    if (--ss->refcount > 0) return;
    kfree(ss);
}

uint32_t signal_pending_set(struct proc *p) {
    if (!p) return 0;
    return p->pending_sigs | (p->sigshared ? p->sigshared->pending : 0);
}

/* ── Sending ─────────────────────────────────────────────────────────────── */

/* Default action of sig when its handler is SIG_DFL: 1 = ignore, 2 = stop,
 * 3 = terminate (Linux sig_kernel_ignore / sig_kernel_stop / the rest). */
static int sig_default_action(int sig) {
    switch (sig) {
    case SIGCHLD: case SIGCONT: case 23 /* SIGURG */: case 28 /* SIGWINCH */:
        return 1;
    case SIGSTOP: case SIGTSTP: case SIGTTIN: case SIGTTOU:
        return 2;
    default:
        return 3;
    }
}

/* Linux sig_ignored(): a signal whose disposition is SIG_IGN, or SIG_DFL with
 * a default action of "ignore", is discarded at send time — unless it is
 * blocked, in which case it stays pending so that a later sigaction() +
 * unblock can still see it.  SIGKILL/SIGSTOP are never ignorable. */
static int sig_ignored(struct proc *p, int sig) {
    if (sig == SIGKILL || sig == SIGSTOP) return 0;
    if (p->blocked_sigs & (1u << sig)) return 0;
    sighandler_t h = p->sighand ? p->sighand->handlers[sig] : SIG_DFL;
    if (h == SIG_IGN) return 1;
    if (h == SIG_DFL && sig_default_action(sig) == 1) return 1;
    return 0;
}

/* Would sig, if pending on p, make p do something on its next return to user
 * mode?  Blocked signals do not; ignored ones were never queued.  Used to
 * decide whether queuing it must wake a sleeping p (Linux signal_wake_up is
 * called only from complete_signal, i.e. for a deliverable signal). */
static int sig_wakes(struct proc *p, int sig) {
    if (sig == SIGKILL || sig == SIGSTOP) return 1;
    if (p->blocked_sigs & (1u << sig)) return 0;
    return !sig_ignored(p, sig);
}

/* Make a sleeping thread run so that it notices its new pending signal.  The
 * timed-wait deadline is consumed here (like every other wake path): the
 * interrupted call reports -EINTR/restart, never a timeout, and the deadline
 * must not fire into whatever the thread sleeps on next. */
static void sig_wake_sleeper(struct proc *p, int sig) {
    if (p->state == PROC_SLEEPING) {
        p->sleep_chan = (void *)0;
        p->wake_tick  = 0;
        sched_make_runnable(p);
    } else if (p->state == PROC_STOPPED && sig == SIGKILL) {
        sched_make_runnable(p);          /* a stopped task can still be killed */
    }
}

static int proc_live(struct proc *q) {
    return q->state != PROC_UNUSED && q->state != PROC_ZOMBIE;
}

/* ── Job control: group stop / continue ──────────────────────────────────── */

static int sig_is_stop(int sig) {
    return sig == SIGSTOP || sig == SIGTSTP || sig == SIGTTIN || sig == SIGTTOU;
}

int proc_group_stopped(struct proc *leader) {
    if (!leader || !leader->group_stop) return 0;
    int stopped = 0;
    for (int i = 0; i < MAX_PROCS; i++) {
        struct proc *q = &ptable[i];
        if (!proc_live(q) || q->tgid != leader->tgid) continue;
        if (q->state != PROC_STOPPED) return 0;
        stopped = 1;
    }
    return stopped;
}

void signal_group_stop_check(struct proc *p) {
    struct proc *leader = proc_group_leader(p);
    if (!leader || !leader->group_stop || leader->stop_reported) return;
    if (!proc_group_stopped(leader)) return;
    /* The last thread has stopped: the process is stopped.  Wake the parent
     * where waitpid(WUNTRACED) sleeps — on its group leader. */
    if (leader->parent)
        wake_up(proc_group_leader(leader->parent));
}

/* Linux prepare_signal(): SIGCONT, however it is sent and even when ignored,
 * ends a group stop — every thread is resumed, any stop still pending is
 * dropped and waitpid(WCONTINUED) will report it; a stop signal drops a
 * pending SIGCONT. */
static void prepare_job_control(struct proc *p, int sig) {
    int tg = p->tgid;
    uint32_t drop;
    if (sig == SIGCONT) {
        drop = (1u << SIGSTOP) | (1u << SIGTSTP) | (1u << SIGTTIN) | (1u << SIGTTOU);
        struct proc *leader = proc_group_leader(p);
        int was_stopped = 0;
        for (int i = 0; i < MAX_PROCS; i++) {
            struct proc *q = &ptable[i];
            if (!proc_live(q) || q->tgid != tg) continue;
            q->jobctl_stop = 0;
            if (q->state == PROC_STOPPED) { sched_make_runnable(q); was_stopped = 1; }
        }
        if (leader && (leader->group_stop || was_stopped)) {
            leader->group_stop      = 0;
            leader->group_continued = 1;
            if (leader->parent) wake_up(proc_group_leader(leader->parent));
        }
    } else if (sig_is_stop(sig)) {
        drop = 1u << SIGCONT;
    } else {
        return;
    }
    for (int i = 0; i < MAX_PROCS; i++) {
        struct proc *q = &ptable[i];
        if (!proc_live(q) || q->tgid != tg) continue;
        q->pending_sigs &= ~drop;
        if (q->sigshared) q->sigshared->pending &= ~drop;
    }
}

/* This thread joins the group stop: it stops, and if it was the last one to
 * do so the parent is told.  Returns once SIGCONT (or SIGKILL) resumes it. */
static void group_stop_participate(void) {
    struct proc *me = current_proc;
    me->jobctl_stop = 0;
    me->state = PROC_STOPPED;          /* counted as stopped by the check */
    signal_group_stop_check(me);
    proc_stop_self();
}

/* Linux do_signal_stop(): a stop signal taken with its default action stops
 * the whole THREAD GROUP, not just the thread that dequeued it.  Every other
 * live thread is asked to stop on its next return to user mode (a sleeping one
 * is woken for it; its interrupted call restarts after SIGCONT), and waitpid
 * (WUNTRACED) reports the process once all of them have. */
static void do_group_stop(int sig) {
    struct proc *me = current_proc;
    struct proc *leader = proc_group_leader(me);
    if (leader) {
        leader->group_stop      = sig;
        leader->group_continued = 0;
        leader->stop_sig        = sig;
        leader->stop_reported   = 0;
    }
    me->stop_sig = sig;
    for (int i = 0; i < MAX_PROCS; i++) {
        struct proc *q = &ptable[i];
        if (q == me || !proc_live(q) || q->tgid != me->tgid) continue;
        if (q->state == PROC_STOPPED) continue;
        q->jobctl_stop = 1;
        sig_wake_sleeper(q, sig);
    }
    group_stop_participate();
}

void signal_send(struct proc *p, int sig) {
    if (!p || sig < 1 || sig >= NSIGS) return;
    if (p->state == PROC_UNUSED || p->state == PROC_ZOMBIE) return;
    prepare_job_control(p, sig);
    if (sig_ignored(p, sig)) return;
    p->pending_sigs |= (1u << sig);
    if (sig_wakes(p, sig))
        sig_wake_sleeper(p, sig);
}

/* Linux force_sig_fault(): a synchronous fault carries its si_code and the
 * address that caused it.  Record both against the signal so the delivery that
 * picks it up can build a real siginfo_t; an unrelated signal delivered first
 * leaves the record alone (it is keyed by signal number). */
void signal_send_fault(struct proc *p, int sig, int code, uint32_t addr) {
    if (!p || sig < 1 || sig >= NSIGS) return;
    p->fault_sig  = sig;
    p->fault_code = code;
    p->fault_addr = addr;
    signal_send(p, sig);
    /* Discarded at send time (SIG_IGN): the detail goes with it, so a later
     * kill() of the same signal cannot inherit this fault's address. */
    if (!(p->pending_sigs & (1u << sig)))
        p->fault_sig = 0;
}

void signal_send_group(struct proc *p, int sig) {
    if (!p || sig < 1 || sig >= NSIGS) return;
    int tg = p->tgid;
    uint32_t bit = 1u << sig;

    /* The live threads of the group.  p itself may be the zombie of a leader
     * that called pthread_exit while its siblings run on: the process is still
     * there and still takes signals (Linux keeps the leader's task as the
     * group's anchor for exactly this). */
    struct proc *leader = (struct proc *)0, *any = (struct proc *)0;
    for (int i = 0; i < MAX_PROCS; i++) {
        struct proc *q = &ptable[i];
        if (!proc_live(q) || q->tgid != tg) continue;
        if (!any) any = q;
        if (q->pid == tg) leader = q;
    }
    if (!any) return;                           /* the whole process is gone */
    struct proc *ref = proc_live(p) ? p : (leader ? leader : any);

    /* SIGKILL is fatal to the group whoever takes it: queue it on every thread
     * at once (Linux complete_signal's fatal path does the same), so no thread
     * runs on in user mode waiting for its turn. */
    if (sig == SIGKILL) {
        for (int i = 0; i < MAX_PROCS; i++) {
            struct proc *q = &ptable[i];
            if (proc_live(q) && q->tgid == tg) signal_send(q, SIGKILL);
        }
        return;
    }
    prepare_job_control(ref, sig);

    struct sigshared *ss = ref->sigshared;
    if (!ss) { signal_send(ref, sig); return; }   /* no process state: kthread */
    if (sig_ignored(ref, sig)) return;
    ss->pending |= bit;

    /* complete_signal(): pick a thread that can take it — the addressed one,
     * then the leader, then any other that does not block it — and make it
     * notice.  If every thread blocks it, nobody is woken and it waits in the
     * process-wide set until one unblocks it. */
    struct proc *target = (struct proc *)0;
    if (!(ref->blocked_sigs & bit)) target = ref;
    else if (leader && !(leader->blocked_sigs & bit)) target = leader;
    else
        for (int i = 0; i < MAX_PROCS && !target; i++) {
            struct proc *q = &ptable[i];
            if (proc_live(q) && q->tgid == tg && !(q->blocked_sigs & bit))
                target = q;
        }
    if (target && sig_wakes(target, sig))
        sig_wake_sleeper(target, sig);
}

void signal_retarget_shared(struct proc *p, uint32_t which) {
    if (!p || !p->sigshared) return;
    uint32_t left = p->sigshared->pending & which;
    for (int i = 0; i < MAX_PROCS && left; i++) {
        struct proc *q = &ptable[i];
        if (q == p || !proc_live(q) || q->tgid != p->tgid) continue;
        uint32_t take = left & ~q->blocked_sigs;
        if (!take) continue;
        for (int sig = 1; sig < NSIGS; sig++)
            if ((take & (1u << sig)) && sig_wakes(q, sig)) {
                sig_wake_sleeper(q, sig);
                break;
            }
        left &= ~take;
    }
}

int signal_send_pgrp(int pg, int sig) {
    int sent = 0;
    for (int i = 0; i < MAX_PROCS; i++) {
        struct proc *q = &ptable[i];
        if (q->state == PROC_UNUSED || q->state == PROC_ZOMBIE) continue;
        if (q->pgrp != pg || q->pid != q->tgid) continue;   /* one per process */
        signal_send_group(q, sig);
        sent++;
    }
    return sent;
}

int signal_interrupt_pending(struct proc *p) {
    if (!p) return 0;
    if (p->jobctl_stop) return 1;      /* must reach the stop on the way out */
    uint32_t pending = signal_pending_set(p) & ~p->blocked_sigs;
    if (!pending) return 0;
    for (int i = 1; i < NSIGS; i++) {
        if (!(pending & (1u << i))) continue;
        sighandler_t h = p->sighand ? p->sighand->handlers[i] : SIG_DFL;
        if (h == SIG_IGN) continue;
        if (h == SIG_DFL && sig_default_action(i) == 1) continue;
        return 1;
    }
    return 0;
}

/* ── Group exit ──────────────────────────────────────────────────────────── */

/* Linux do_group_exit() / zap_other_threads(): record the group's exit status
 * on the leader once (the first exiting thread wins, later SIGKILL deaths of
 * siblings must not overwrite it) and queue SIGKILL on every other live thread
 * of the group.  Sleeping siblings are woken by signal_send and die when their
 * interrupted syscall returns; CPU-bound siblings die on their next timer
 * interrupt (irq_handler delivers signals on return to ring 3). */
static void thread_group_kill(int status) {
    struct proc *me = current_proc;
    struct proc *leader = proc_group_leader(me);
    if (leader && !leader->group_exit) {
        leader->group_exit  = 1;
        leader->exit_status = status;
    }
    for (int i = 0; i < MAX_PROCS; i++) {
        struct proc *q = &ptable[i];
        if (q == me || q->state == PROC_UNUSED || q->state == PROC_ZOMBIE) continue;
        if (q->tgid != me->tgid) continue;
        signal_send(q, SIGKILL);
    }
}

void proc_group_exit(int status) {
    thread_group_kill(status);
    proc_exit(status);
}

/* ── Delivery ────────────────────────────────────────────────────────────── */

/* Re-execute the interrupted syscall: back eip up over the 2-byte `int 0x80`
 * and restore the syscall number that the return value overwrote in eax
 * (Linux arch/x86/kernel/signal.c: regs->ax = regs->orig_ax; regs->ip -= 2). */
static void syscall_restart(registers_t *regs, int syscall_nr) {
    regs->eax  = (uint32_t)syscall_nr;
    regs->eip -= 2;
}

/* Linux restore_saved_sigmask(): put back the mask sigsuspend stashed, once
 * this path has decided what (if anything) is being delivered.  A no-op for
 * every other syscall. */
static void restore_saved_sigmask(void) {
    if (current_proc && current_proc->restore_sigmask) {
        signal_retarget_shared(current_proc,
                               current_proc->saved_sigmask & ~current_proc->blocked_sigs);
        current_proc->blocked_sigs    = current_proc->saved_sigmask;
        current_proc->restore_sigmask = 0;
    }
}

void signal_return_to_user(registers_t *regs, int syscall_nr) {
    if (!current_proc) return;
    /* Only deliver to user-mode frames */
    if ((regs->cs & 3) != 3) return;

    /* A group stop asked this thread to stop (Linux JOBCTL_STOP_PENDING), unless
     * it is being killed.  After SIGCONT it carries on below: whatever is
     * pending then is delivered, and an interrupted call is restarted. */
    if (current_proc->jobctl_stop) {
        if (current_proc->pending_sigs & (1u << SIGKILL))
            current_proc->jobctl_stop = 0;
        else
            group_stop_participate();
    }

    int32_t  ret = (int32_t)regs->eax;
    int restartable = syscall_nr >= 0 && (ret == -4 || ret == -ERESTARTNOHAND);

    /* The thread's own signals are taken before the process-wide ones (Linux
     * dequeue_signal: task->pending, then signal->shared_pending). */
    struct sigshared *ss = current_proc->sigshared;
    uint32_t own    = current_proc->pending_sigs & ~current_proc->blocked_sigs;
    uint32_t shared = ss ? (ss->pending & ~current_proc->blocked_sigs) : 0;
    uint32_t pending = own ? own : shared;
    if (!pending) {
        /* Woken by a signal that is no longer deliverable (or the syscall
         * returned an internal restart code with nothing pending): transparently
         * restart it, exactly as Linux does when get_signal() finds nothing.
         * The restarted sigsuspend re-installs its temporary mask, so the
         * stashed one is put back first (arch/x86 arch_do_signal_or_restart:
         * restore_saved_sigmask() on the no-signal path). */
        if (restartable) syscall_restart(regs, syscall_nr);
        restore_saved_sigmask();
        return;
    }

    /* Find lowest-numbered pending signal */
    int sig = 0;
    for (int i = 1; i < NSIGS; i++) {
        if (pending & (1u << i)) { sig = i; break; }
    }
    if (!sig) { restore_saved_sigmask(); return; }

    if (own) current_proc->pending_sigs &= ~(1u << sig);
    else     ss->pending               &= ~(1u << sig);

    struct sighand *sh = current_proc->sighand;
    sighandler_t handler = sh ? sh->handlers[sig] : SIG_DFL;
    uint32_t     sflags  = sh ? sh->flags[sig]    : 0;
    uint32_t     samask  = sh ? sh->mask[sig]     : 0;

    /* SIGKILL can never be caught or ignored */
    if (sig == SIGKILL) {
        handler = SIG_DFL;
        sflags  = 0;
        samask  = 0;
    }

    /* Detail recorded by a synchronous fault for THIS signal (si_code, the
     * faulting address).  Taken now so it is consumed by exactly one delivery. */
    int      si_code = SI_USER;
    uint32_t si_addr = 0;
    if (current_proc->fault_sig == sig) {
        si_code = current_proc->fault_code;
        si_addr = current_proc->fault_addr;
        current_proc->fault_sig = 0;
    }

    if (handler == SIG_IGN) {
        if (restartable) syscall_restart(regs, syscall_nr);
        restore_saved_sigmask();
        return;
    }

    if (handler == SIG_DFL) {
        switch (sig_default_action(sig)) {
        case 1:
            if (restartable) syscall_restart(regs, syscall_nr);
            restore_saved_sigmask();
            return;  /* default: ignore */
        case 2:
            /* Stop the whole thread group; waitpid(WUNTRACED) reports it once
             * every thread has stopped.  Won't return until SIGCONT makes us
             * RUNNABLE; the interrupted syscall is then restarted (no handler
             * ran). */
            do_group_stop(sig);
            if (restartable) syscall_restart(regs, syscall_nr);
            restore_saved_sigmask();
            return;
        default:
            /* Fatal signal with the default disposition: Linux get_signal()
             * calls do_group_exit(signr), so the WHOLE thread group dies with
             * this signal as its wait status (kernel/signal.c complete_signal
             * + zap_other_threads), not just the thread it was queued on.
             * The SIGKILL a group exit queues on the other threads is not a
             * kill: logging it made every clean exit_group() of a Firefox
             * content process look like a dozen threads SIGKILLed. */
            if (sig != SIGKILL || !proc_group_leader(current_proc)->group_exit)
                printk("[SIG] pid=%d tgid=%d killed by signal %d\n",
                       current_proc->pid, current_proc->tgid, sig);
            proc_group_exit(sig & 0x7f);
        }
    }

    /* SA_RESETHAND (SA_ONESHOT): the disposition goes back to SIG_DFL as the
     * handler is entered, so the next such signal takes the default action
     * (Linux get_signal).  sa_flags and sa_mask stay as they were. */
    if ((sflags & SA_RESETHAND) && sh)
        sh->handlers[sig] = SIG_DFL;

    /*
     * User-installed handler.
     *
     * Without SA_SIGINFO — C ABI: [esp+0]=retaddr, [esp+4]=signo
     *
     *   [ mask to restore      ]
     *   [ saved registers_t    ]  ← saved_addr
     *   [ restore marker = 0   ]  [esp+20]
     *   [ restore addr = saved ]  [esp+16]
     *   [ 0, 0                 ]
     *   [ signo                ]
     *   [ retaddr = SIGPAGE_VA ]  ← new useresp
     *
     * With SA_SIGINFO — three-arg ABI: [esp+0]=retaddr, [esp+4]=signo,
     *                                  [esp+8]=&siginfo, [esp+12]=&ucontext
     *
     *   [ mask, registers_t    ]  ← saved_addr
     *   [ ucontext             ]  ← uctx_addr
     *   [ siginfo_t            ]  ← siginfo_addr
     *   [ restore marker = 1   ]  [esp+20]
     *   [ restore addr = uctx  ]  [esp+16]
     *   [ &ucontext, &siginfo  ]
     *   [ signo                ]
     *   [ retaddr = SIGPAGE_VA ]  ← new useresp
     *
     * The handler's `ret` leaves esp 4 higher, so the trampoline on the signal
     * page finds the restore words at [esp+12] and [esp+16]: past every
     * argument slot, which a handler may reuse but never anything above.
     */
    uint32_t sp = regs->useresp;

    /* SA_ONSTACK: build the frame on the sigaltstack() stack instead of the
     * interrupted one, unless we are already running on it (Linux
     * get_sigframe / on_sig_stack) — which is what lets a handler for a
     * stack-overflow SIGSEGV run at all.
     *
     * A frame built on the alternate stack must STAY on it, which is what
     * sp_floor enforces below: without it the second and third frame of a nest
     * — they do not re-switch, sp already being inside — simply continue past
     * the bottom and the kernel memcpy scribbles over whatever the process put
     * there.  Linux get_sigframe() returns -1L in exactly this case and the
     * process dies with SIGSEGV, which is what `fatal` does here. */
    int onstack = current_proc->sas_size &&
                  sp >= current_proc->sas_sp &&
                  sp <  current_proc->sas_sp + current_proc->sas_size;
    if ((sflags & SA_ONSTACK) && current_proc->sas_size && !onstack) {
        sp = (current_proc->sas_sp + current_proc->sas_size) & ~0xFU;
        onstack = 1;
    }
    const uint32_t sp_floor = onstack ? current_proc->sas_sp : 0x08000000U;

    /* The interrupted esp (and the sigaltstack, which may have been unmapped
     * since sigaltstack() checked it) is whatever the process put there: it can
     * name the kernel half.  Nothing below may assume otherwise — every store
     * into the frame goes through copy_to_user(), which rejects any address at
     * or above 0xC0000000 and turns a fault on a read-only or unmapped page into
     * an error instead of a ring-0 panic.  An esp that is not even a plausible
     * user address is refused before anything is computed from it. */
    if (sp > 0xC0000000U || sp <= sp_floor) goto fatal;

    /* The mask to put back when the handler returns: the interrupted mask, or
     * the one sigsuspend stashed if it is waiting for its restore (Linux
     * sigmask_to_save()).  It is carried in the frame, so nested handlers each
     * restore their own. */
    uint32_t save_mask = current_proc->restore_sigmask
                       ? current_proc->saved_sigmask : current_proc->blocked_sigs;

    /* Pre-fault the user-stack pages the signal frame will occupy.  Thread
     * stacks are demand-paged (large anon VMAs), so the pages just BELOW the
     * interrupted esp — where the frame is built — are often not present yet.
     * copy_to_user() would demand-fault them too, but doing it here means a
     * page that cannot be backed is reported as the stack overflow it is.
     * Fault them in via the demand-pager first; if a
     * page can't be made present (real stack overflow / not in any VMA), fall
     * through to `fatal` (kill the process with SIGSEGV — Linux behaviour — not a
     * kernel panic).  Worst-case frame (SA_SIGINFO: tramp+regs+ucontext+siginfo+
     * args) is < 1 KiB, so covering the interrupted page + 2 below is ample. */
    {
        extern int vma_handle_fault(uint32_t addr);
        uint32_t lo = (sp - 0x2000) & ~0xFFFU;
        uint32_t hi = (sp - 1)      & ~0xFFFU;
        if (lo < (sp_floor & ~0xFFFU)) lo = sp_floor & ~0xFFFU;   /* never below
                                                * the alternate stack */
        for (uint32_t a = lo; a <= hi && a >= 0x08000000U; a += 0x1000) {
            if (pte_read(a) & PAGE_PRESENT) continue;
            if (!vma_handle_fault(a)) goto fatal;   /* unmappable → kill, not panic */
        }
    }

    /* The handler returns into the sigreturn trampoline on the signal page
     * (paging_map_sigpage), which picks the restore-frame address and its
     * type marker up from the two words after the argument block.  It used
     * to be code written onto this stack, which NX now refuses to run. */
    const uint32_t tramp_addr = SIGPAGE_VA;
    uint32_t uctx_addr = 0;   /* set in the SA_SIGINFO branch below */

    /*
     * A handler is about to run.  If it interrupted a blocking syscall, decide
     * what that syscall returns once the handler completes (Linux
     * handle_signal(): -ERESTARTNOHAND always becomes -EINTR; -ERESTARTSYS
     * becomes -EINTR unless SA_RESTART, in which case the call is re-executed
     * with its original number).  The decision is applied to the SAVED frame,
     * which sigreturn restores after the handler returns.
     */
    registers_t saved_regs = *regs;
    if (restartable) {
        if (ret == -4 && (sflags & SA_RESTART))
            syscall_restart(&saved_regs, syscall_nr);
        else
            saved_regs.eax = (uint32_t)-4;   /* -EINTR */
    }

    /* Save (possibly modified) trapframe, with the mask to restore parked
     * directly above it — a plain (non-SA_SIGINFO) frame has no ucontext to
     * carry a uc_sigmask, and sigreturn_restore() reads it back from there. */
    sp -= 4;
    if (sp < sp_floor) goto fatal;
    {
        uint32_t um = sigset_to_user(save_mask);
        if (copy_to_user((void *)(uintptr_t)sp, &um, sizeof(um)) < 0) goto fatal;
    }
    sp -= sizeof(registers_t);
    uint32_t saved_addr = sp;
    if (sp < sp_floor) goto fatal;
    if (copy_to_user((void *)(uintptr_t)sp, &saved_regs, sizeof(registers_t)) < 0)
        goto fatal;

    if (sflags & SA_SIGINFO) {
        /* Build a real ucontext_t on the user stack so SA_SIGINFO handlers that
         * read the faulting register state (Breakpad, WasmTrapHandler, …) work
         * instead of dereferencing a NULL ucontext and crashing. */
        sp -= sizeof(struct k_ucontext);
        uctx_addr = sp;
        if (sp < sp_floor) goto fatal;
        struct k_ucontext uc;
        __builtin_memset(&uc, 0, sizeof(uc));
        uc.uc_mcontext.gs  = (uint16_t)saved_regs.gs;
        uc.uc_mcontext.fs  = (uint16_t)saved_regs.fs;
        uc.uc_mcontext.es  = (uint16_t)saved_regs.es;
        uc.uc_mcontext.ds  = (uint16_t)saved_regs.ds;
        uc.uc_mcontext.edi = saved_regs.edi;
        uc.uc_mcontext.esi = saved_regs.esi;
        uc.uc_mcontext.ebp = saved_regs.ebp;
        uc.uc_mcontext.esp = saved_regs.useresp;
        uc.uc_mcontext.ebx = saved_regs.ebx;
        uc.uc_mcontext.edx = saved_regs.edx;
        uc.uc_mcontext.ecx = saved_regs.ecx;
        uc.uc_mcontext.eax = saved_regs.eax;
        uc.uc_mcontext.trapno = saved_regs.int_no;
        uc.uc_mcontext.err    = saved_regs.err_code;
        uc.uc_mcontext.eip    = saved_regs.eip;
        uc.uc_mcontext.cs     = (uint16_t)saved_regs.cs;
        uc.uc_mcontext.eflags = saved_regs.eflags;
        uc.uc_mcontext.esp_at_signal = saved_regs.useresp;
        uc.uc_mcontext.ss     = (uint16_t)saved_regs.ss;
        uc.uc_mcontext.cr2    = si_addr;   /* also delivered via siginfo */
        uc.uc_mcontext.oldmask = sigset_to_user(save_mask);
        /* uc_sigmask is the mask from BEFORE the handler; sigreturn puts it
         * back, so a handler that edits it (swapcontext, longjmp helpers)
         * changes what is restored. */
        {
            uint32_t um = sigset_to_user(save_mask);
            __builtin_memcpy(uc.uc_sigmask, &um, sizeof(um));
        }
        /* uc_stack describes the alternate stack, flagged if we are on it. */
        uc.ss_sp    = current_proc->sas_sp;
        uc.ss_size  = current_proc->sas_size;
        uc.ss_flags = current_proc->sas_size
                    ? ((sp >= current_proc->sas_sp &&
                        sp <  current_proc->sas_sp + current_proc->sas_size)
                           ? SS_ONSTACK : 0)
                    : SS_DISABLE;
        if (copy_to_user((void *)(uintptr_t)sp, &uc, sizeof(uc)) < 0) goto fatal;

        /* Build siginfo_t on user stack */
        sp -= sizeof(siginfo_t);
        uint32_t siginfo_addr = sp;
        if (sp < sp_floor) goto fatal;
        siginfo_t si;
        __builtin_memset(&si, 0, sizeof(si));
        si.si_signo = sig;
        si.si_code  = si_code;
        si._u._sigfault.si_addr = si_addr;
        if (copy_to_user((void *)(uintptr_t)sp, &si, sizeof(si)) < 0) goto fatal;

        /* Push frame: retaddr, signo, &siginfo, &ucontext.  The i386 SysV ABI
         * requires the stack 16-byte aligned at the call site, so the handler's
         * entry esp (pointing at the return address) must be ≡ 12 (mod 16) —
         * GCC-compiled handlers emit aligned SSE (movaps) to the stack, which
         * #GPs on a misaligned frame.  Round the 4-dword arg block down to that. */
        sp = ((sp - 24) & ~0xFU) - 4;
        if (sp < sp_floor) goto fatal;
        uint32_t args[6] = { tramp_addr,               /* retaddr */
                             (uint32_t)sig,            /* signo */
                             siginfo_addr,             /* &siginfo */
                             uctx_addr,                /* &ucontext */
                             uctx_addr, 1 };           /* for the trampoline:
                                                        * restore from the
                                                        * ucontext (marker 1) */
        if (copy_to_user((void *)(uintptr_t)sp, args, sizeof(args)) < 0) goto fatal;
    } else {
        /* Simple one-arg frame: retaddr, signo — same 16-byte alignment rule. */
        sp = ((sp - 24) & ~0xFU) - 4;
        if (sp < sp_floor) goto fatal;
        uint32_t args[6] = { tramp_addr,               /* retaddr */
                             (uint32_t)sig,            /* signo */
                             0, 0,                     /* not arguments */
                             saved_addr, 0 };          /* for the trampoline:
                                                        * the plain frame
                                                        * (marker 0) */
        if (copy_to_user((void *)(uintptr_t)sp, args, sizeof(args)) < 0) goto fatal;
    }

    /* The trampoline loads the restore-frame ADDRESS into ecx and a TYPE
     * marker into edx (the last two words of the argument block above), then
     * traps into sigreturn:
     *   marker 1 → restore from the ucontext's mcontext (which the handler may
     *              have MODIFIED — WasmTrapHandler / the SpiderMonkey JIT redirect
     *              the PC past a WASM trap by writing uc_mcontext.gregs[REG_EIP];
     *              ignoring that was making Firefox resume at the faulting insn
     *              and crash/corrupt).
     *   marker 0 → restore from the plain saved registers_t (no ucontext).
     * Encoding the address per-frame (instead of one kernel field) also makes
     * sigreturn correct under NESTED signals (a fault inside a handler). */
    current_proc->sigframe_addr = saved_addr;

    /* The frame now carries the mask to restore, so sigsuspend's stash has done
     * its job: drop the flag WITHOUT reinstating the mask here (Linux
     * signal_delivered() leaves the temporary mask in force for the handler and
     * puts the saved one back from uc_sigmask at sigreturn).
     *
     * While the handler runs, sa_mask is blocked on top of the current mask,
     * and so is the signal itself unless SA_NODEFER (kernel/signal.c
     * handle_signal: sigorsets + sigaddset + set_current_blocked). */
    current_proc->restore_sigmask = 0;
    {
        uint32_t blocked = current_proc->blocked_sigs | samask;
        if (!(sflags & SA_NODEFER)) blocked |= (1u << sig);
        blocked &= ~((1u << SIGKILL) | (1u << SIGSTOP));
        /* Process-wide signals this thread now blocks go to another thread
         * (Linux set_current_blocked -> retarget_shared_pending). */
        signal_retarget_shared(current_proc, blocked & ~current_proc->blocked_sigs);
        current_proc->blocked_sigs = blocked;
    }

    /* Redirect trapframe to the user handler */
    regs->eip     = (uint32_t)(uintptr_t)handler;
    regs->useresp = sp;
    return;

fatal:
    /* The frame could not be written: stack overflow, an esp that is not a user
     * address, or a read-only/unmapped page under it.  Linux force_sigsegv():
     * if this WAS the SIGSEGV, nothing more can be done and the group dies of
     * it.  Otherwise the interrupted syscall ends as if the handler had run,
     * and a SIGSEGV that can be neither blocked nor ignored is delivered in its
     * place — to a SIGSEGV handler on a usable (alternate) stack if there is
     * one, else by the default action.  The recursion is bounded: each round
     * consumes one pending signal, and a failing SIGSEGV frame kills. */
    ktrace("[SIG] pid=%d: cannot build frame for signal %d at esp=%08x\n",
           current_proc->pid, sig, (unsigned)regs->useresp);
    if (sig == SIGSEGV)
        proc_group_exit(SIGSEGV);
    if (restartable) {
        if (ret == -4 && (sflags & SA_RESTART)) syscall_restart(regs, syscall_nr);
        else                                     regs->eax = (uint32_t)-4;
    }
    if (current_proc->blocked_sigs & (1u << SIGSEGV)) {
        current_proc->blocked_sigs &= ~(1u << SIGSEGV);
        if (sh) sh->handlers[SIGSEGV] = SIG_DFL;
    } else if (sh && sh->handlers[SIGSEGV] == SIG_IGN) {
        sh->handlers[SIGSEGV] = SIG_DFL;
    }
    signal_send_fault(current_proc, SIGSEGV, SI_KERNEL, 0);
    signal_return_to_user(regs, -1);
}

/* Selectors a frame may name for FS/GS: the null selector, the user data
 * segment and the user TLS entry (gdt.c hands out 0x23 and 0x33).  Any other
 * value would #GP inside the kernel's own `pop gs` on the way out — a ring-0
 * fault, i.e. a panic — so a frame naming one is ignored. */
static int user_seg_ok(uint32_t sel) {
    return sel == 0 || sel == 0x23 || sel == 0x33;
}

/*
 * INVARIANT for every path that returns to user mode from a frame the USER
 * could have written — both sigreturn markers today, and anything added later:
 * the frame supplies the general-purpose registers, EIP and ESP, and NOTHING
 * ELSE.  The privilege state is imposed here, never taken from the frame:
 *
 *   - CS/SS/DS/ES are the ring-3 selectors.  A frame that names its own would
 *     otherwise #GP (or worse) at the `iret`, which runs at CPL 0.
 *   - EFLAGS keeps only the arithmetic bits, with IF and the reserved bit 1
 *     forced on.  Taking it verbatim would let a handler set IOPL=3 and gain
 *     unrestricted port I/O, or return with interrupts disabled.
 *   - FS/GS must name a selector this kernel actually hands out, else the
 *     interrupted context's value is kept.
 */
static void user_frame_sanitise(registers_t *regs, uint32_t eflags,
                                uint32_t fs, uint32_t gs) {
    regs->ds = 0x23; regs->es = 0x23; regs->cs = 0x1B; regs->ss = 0x23;
    regs->eflags = (eflags & 0x000008D5U) | 0x00000202U;
    if (user_seg_ok(fs)) regs->fs = fs;
    if (user_seg_ok(gs)) regs->gs = gs;
}

/* Restore a trapframe at sigreturn.  `addr`/`marker` come from the per-frame
 * trampoline (ecx/edx), so this is correct under nested signals.
 *   marker 1 → restore from the ucontext's mcontext, HONOURING any modifications
 *              the handler made (e.g. WasmTrapHandler / the SpiderMonkey JIT
 *              rewrite gregs[REG_EIP] to redirect past a WASM trap).
 *   marker 0 → restore from a plain saved registers_t (a handler installed
 *              without SA_SIGINFO), with the mask parked above it.
 * Both frames sit on the user stack and both addresses come from user-supplied
 * registers, so BOTH go through user_frame_sanitise() above.
 * Returns 0 on success, -1 if the frame cannot be read — not a user range, or
 * unmapped — so the caller kills with SIGSEGV instead of the kernel faulting. */
int sigreturn_restore(registers_t *regs, uint32_t addr, uint32_t marker) {
    /* The whole frame is copied in before anything is applied, so a frame that
     * is only partly readable changes neither the mask nor the registers. */
    union {
        struct k_ucontext uc;
        struct { registers_t r; uint32_t mask; } plain;   /* mask directly above */
    } f;
    _Static_assert(sizeof(f.plain) == sizeof(registers_t) + 4,
                   "plain sigframe: mask must sit right after registers_t");
    uint32_t need = (marker == 1) ? (uint32_t)sizeof(struct k_ucontext)
                                  : (uint32_t)(sizeof(registers_t) + 4);
    if (addr < 0x08000000U || addr + need < addr || addr + need > 0xC0000000U)
        return -1;
    if (copy_from_user(&f, (const void *)(uintptr_t)addr, need) < 0)
        return -1;
    /* Put back the mask this frame was entered with (Linux restore_sigcontext:
     * set_current_blocked(&uc->uc_sigmask)), honouring a handler that edited
     * it.  SIGKILL/SIGSTOP can never end up blocked. */
    if (current_proc) {
        uint32_t um;
        if (marker == 1) __builtin_memcpy(&um, f.uc.uc_sigmask, sizeof(um));
        else             um = f.plain.mask;
        current_proc->blocked_sigs =
            sigset_from_user(um) & ~((1u << SIGKILL) | (1u << SIGSTOP));
    }
    if (marker == 1) {
        struct k_sigcontext *m = &f.uc.uc_mcontext;
        regs->edi = m->edi; regs->esi = m->esi; regs->ebp = m->ebp;
        regs->ebx = m->ebx; regs->edx = m->edx; regs->ecx = m->ecx;
        regs->eax = m->eax; regs->eip = m->eip; regs->useresp = m->esp;
        user_frame_sanitise(regs, m->eflags, m->fs, m->gs);
    } else {
        registers_t *fr = &f.plain.r;
        uint32_t fs = regs->fs, gs = regs->gs;   /* interrupted context's */
        *regs = *fr;                             /* GPRs, EIP, ESP */
        regs->fs = fs; regs->gs = gs;            /* kept unless the frame's
                                                  * are selectors we hand out */
        user_frame_sanitise(regs, fr->eflags, fr->fs, fr->gs);
    }
    return 0;
}
