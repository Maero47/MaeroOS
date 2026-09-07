#include "signal.h"
#include "process.h"
#include "scheduler.h"
#include "../arch/i686/mm/paging.h"   /* paging_get_pde/pte for sigframe prefault */

#include "../kernel/printk.h"
#include <registers.h>
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
    }
    return sh;
}

void sighand_put(struct sighand *sh) {
    if (!sh) return;
    if (--sh->refcount > 0) return;
    kfree(sh);
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
        p->state      = PROC_RUNNABLE;
    } else if (p->state == PROC_STOPPED && sig == SIGKILL) {
        p->state      = PROC_RUNNABLE;   /* a stopped task can still be killed */
    }
}

void signal_send(struct proc *p, int sig) {
    if (!p || sig < 1 || sig >= NSIGS) return;
    if (p->state == PROC_UNUSED || p->state == PROC_ZOMBIE) return;
    /* SIGCONT resumes a stopped process even when it is ignored (Linux
     * prepare_signal: SIGCONT always wakes the group out of a stop). */
    if (sig == SIGCONT && p->state == PROC_STOPPED)
        p->state = PROC_RUNNABLE;
    if (sig_ignored(p, sig)) return;
    p->pending_sigs |= (1u << sig);
    if (sig_wakes(p, sig))
        sig_wake_sleeper(p, sig);
}

void signal_send_group(struct proc *p, int sig) {
    if (!p || sig < 1 || sig >= NSIGS) return;
    int tg = p->tgid;
    struct proc *leader = (struct proc *)0, *open_thread = (struct proc *)0,
                *any = (struct proc *)0;
    for (int i = 0; i < MAX_PROCS; i++) {
        struct proc *q = &ptable[i];
        if (q->state == PROC_UNUSED || q->state == PROC_ZOMBIE) continue;
        if (q->tgid != tg) continue;
        if (q->pid == q->tgid) leader = q;
        if (!any) any = q;
        if (!(q->blocked_sigs & (1u << sig))) {
            /* Prefer the leader when it can take the signal, else the first
             * thread that does not block it (Linux complete_signal: the main
             * thread first, then wants_signal() over the others). */
            if (q == leader) { open_thread = q; break; }
            if (!open_thread) open_thread = q;
        }
    }
    struct proc *target = open_thread ? open_thread : (leader ? leader : any);
    if (target) signal_send(target, sig);
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
    uint32_t pending = p->pending_sigs & ~p->blocked_sigs;
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
        current_proc->blocked_sigs    = current_proc->saved_sigmask;
        current_proc->restore_sigmask = 0;
    }
}

void signal_return_to_user(registers_t *regs, int syscall_nr) {
    if (!current_proc) return;
    /* Only deliver to user-mode frames */
    if ((regs->cs & 3) != 3) return;

    int32_t  ret = (int32_t)regs->eax;
    int restartable = syscall_nr >= 0 && (ret == -4 || ret == -ERESTARTNOHAND);

    uint32_t pending = current_proc->pending_sigs & ~current_proc->blocked_sigs;
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

    current_proc->pending_sigs &= ~(1u << sig);

    struct sighand *sh = current_proc->sighand;
    sighandler_t handler = sh ? sh->handlers[sig] : SIG_DFL;
    uint32_t     sflags  = sh ? sh->flags[sig]    : 0;

    /* SIGKILL can never be caught or ignored */
    if (sig == SIGKILL) {
        handler = SIG_DFL;
        sflags  = 0;
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
            /* Wake parent so waitpid(WUNTRACED) returns */
            if (current_proc->parent)
                wake_up(current_proc->parent);
            /* Stop and yield — won't return until SIGCONT makes us RUNNABLE.
             * The interrupted syscall is then restarted (no handler ran). */
            proc_stop_self();
            if (restartable) syscall_restart(regs, syscall_nr);
            restore_saved_sigmask();
            return;
        default:
            /* Fatal signal with the default disposition: Linux get_signal()
             * calls do_group_exit(signr), so the WHOLE thread group dies with
             * this signal as its wait status (kernel/signal.c complete_signal
             * + zap_other_threads), not just the thread it was queued on. */
            printk("[SIG] pid=%d tgid=%d killed by signal %d\n",
                   current_proc->pid, current_proc->tgid, sig);
            proc_group_exit(sig & 0x7f);
        }
    }

    /*
     * User-installed handler.
     *
     * Without SA_SIGINFO — C ABI: [esp+0]=retaddr, [esp+4]=signo
     *
     *   [ trampoline (8 bytes) ]  ← tramp_addr
     *   [ saved registers_t   ]  ← saved_addr
     *   [ retaddr → tramp     ]
     *   [ signo               ]  ← new useresp
     *
     * With SA_SIGINFO — three-arg ABI: [esp+0]=retaddr, [esp+4]=signo,
     *                                  [esp+8]=&siginfo, [esp+12]=NULL (ucontext)
     *
     *   [ trampoline (8 bytes) ]  ← tramp_addr
     *   [ saved registers_t   ]  ← saved_addr
     *   [ siginfo_t (128 bytes) ] ← siginfo_addr
     *   [ retaddr → tramp     ]
     *   [ signo               ]
     *   [ &siginfo            ]
     *   [ NULL (ucontext)     ]  ← new useresp
     */
    uint32_t sp = regs->useresp;

    /* Pre-fault the user-stack pages the signal frame will occupy.  Thread
     * stacks are demand-paged (large anon VMAs), so the pages just BELOW the
     * interrupted esp — where the frame is built — are often not present yet.
     * The frame is written with raw kernel memcpy, which PANICS on a not-present
     * user page (observed: [SIG] delivery → write to 0xbf7fffa0 → kernel page
     * fault → System halted).  Fault them in via the demand-pager first; if a
     * page can't be made present (real stack overflow / not in any VMA), fall
     * through to `fatal` (kill the process with SIGSEGV — Linux behaviour — not a
     * kernel panic).  Worst-case frame (SA_SIGINFO: tramp+regs+ucontext+siginfo+
     * args) is < 1 KiB, so covering the interrupted page + 2 below is ample. */
    {
        extern int vma_handle_fault(uint32_t addr);
        uint32_t lo = (sp - 0x2000) & ~0xFFFU;
        uint32_t hi = (sp - 1)      & ~0xFFFU;
        for (uint32_t a = lo; a <= hi && a >= 0x08000000U; a += 0x1000) {
            if ((*paging_get_pde(a) & 1) && (*paging_get_pte(a) & 1)) continue;
            if (!vma_handle_fault(a)) goto fatal;   /* unmappable → kill, not panic */
        }
    }

    /* Reserve trampoline space; it's written below once the restore-frame
     * address + type marker are known (20 bytes: mov eax,0x77; mov ecx,addr;
     * mov edx,marker; int 0x80; pad). */
    sp -= 20;
    uint32_t tramp_addr = sp;
    if (sp < 0x08000000U) goto fatal;
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

    /* Save (possibly modified) trapframe */
    sp -= sizeof(registers_t);
    uint32_t saved_addr = sp;
    if (sp < 0x08000000U) goto fatal;
    __builtin_memcpy((void *)sp, &saved_regs, sizeof(registers_t));
    current_proc->sigframe_addr = saved_addr;

    if (sflags & SA_SIGINFO) {
        /* Build a real ucontext_t on the user stack so SA_SIGINFO handlers that
         * read the faulting register state (Breakpad, WasmTrapHandler, …) work
         * instead of dereferencing a NULL ucontext and crashing. */
        sp -= sizeof(struct k_ucontext);
        uctx_addr = sp;
        if (sp < 0x08000000U) goto fatal;
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
        uc.uc_mcontext.cr2    = 0;  /* fault addr also delivered via siginfo */
        __builtin_memcpy((void *)sp, &uc, sizeof(uc));

        /* Build siginfo_t on user stack */
        sp -= sizeof(siginfo_t);
        uint32_t siginfo_addr = sp;
        if (sp < 0x08000000U) goto fatal;
        siginfo_t si;
        __builtin_memset(&si, 0, sizeof(si));
        si.si_signo = sig;
        si.si_code  = 0;  /* SI_USER */
        __builtin_memcpy((void *)sp, &si, sizeof(si));

        /* Push frame: retaddr, signo, &siginfo, &ucontext.  The i386 SysV ABI
         * requires the stack 16-byte aligned at the call site, so the handler's
         * entry esp (pointing at the return address) must be ≡ 12 (mod 16) —
         * GCC-compiled handlers emit aligned SSE (movaps) to the stack, which
         * #GPs on a misaligned frame.  Round the 4-dword arg block down to that. */
        sp = ((sp - 16) & ~0xFU) - 4;
        if (sp < 0x08000000U) goto fatal;
        ((uint32_t *)sp)[0] = tramp_addr;              /* retaddr */
        ((uint32_t *)sp)[1] = (uint32_t)sig;           /* signo */
        ((uint32_t *)sp)[2] = siginfo_addr;            /* &siginfo */
        ((uint32_t *)sp)[3] = uctx_addr;               /* &ucontext */
    } else {
        /* Simple one-arg frame: retaddr, signo — same 16-byte alignment rule. */
        sp = ((sp - 16) & ~0xFU) - 4;
        if (sp < 0x08000000U) goto fatal;
        ((uint32_t *)sp)[0] = tramp_addr;              /* retaddr */
        ((uint32_t *)sp)[1] = (uint32_t)sig;           /* signo */
    }

    /* Write the sigreturn trampoline.  It loads the restore-frame ADDRESS into
     * ecx and a TYPE marker into edx, then traps into sigreturn:
     *   marker 1 → restore from the ucontext's mcontext (which the handler may
     *              have MODIFIED — WasmTrapHandler / the SpiderMonkey JIT redirect
     *              the PC past a WASM trap by writing uc_mcontext.gregs[REG_EIP];
     *              ignoring that was making Firefox resume at the faulting insn
     *              and crash/corrupt).
     *   marker 0 → restore from the plain saved registers_t (no ucontext).
     * Encoding the address per-frame (instead of one kernel field) also makes
     * sigreturn correct under NESTED signals (a fault inside a handler). */
    {
        uint32_t restore_addr, marker;
        if (sflags & SA_SIGINFO) { restore_addr = uctx_addr; marker = 1; }
        else                                           { restore_addr = saved_addr; marker = 0; }
        uint8_t tr[20] = { 0xB8, 0x77,0,0,0,    /* mov  eax, 0x77 (sigreturn) */
                           0xB9, 0,0,0,0,       /* mov  ecx, restore_addr     */
                           0xBA, 0,0,0,0,       /* mov  edx, marker           */
                           0xCD, 0x80,          /* int  0x80                  */
                           0x90, 0x90 };        /* pad                        */
        tr[6]=restore_addr; tr[7]=restore_addr>>8; tr[8]=restore_addr>>16; tr[9]=restore_addr>>24;
        tr[11]=marker; tr[12]=marker>>8; tr[13]=marker>>16; tr[14]=marker>>24;
        __builtin_memcpy((void *)tramp_addr, tr, 20);
    }

    /* A handler frame is committed, so sigsuspend's job is done: put the
     * caller's mask back (Linux does this in signal_delivered(), and restores
     * it from the frame's uc_sigmask at sigreturn).  We reinstate it here
     * rather than at sigreturn because this kernel does not alter the mask
     * across handler entry at all (sa_mask and SA_NODEFER are unimplemented —
     * audit item S6), so there is no per-frame mask to save and restore. */
    restore_saved_sigmask();

    /* Redirect trapframe to the user handler */
    regs->eip     = (uint32_t)(uintptr_t)handler;
    regs->useresp = sp;
    return;

fatal:
    printk("[SIG] pid=%d: stack overflow in signal delivery\n", current_proc->pid);
    proc_group_exit(SIGSEGV);
}

/* Restore a trapframe at sigreturn.  `addr`/`marker` come from the per-frame
 * trampoline (ecx/edx), so this is correct under nested signals.
 *   marker 1 → restore from the ucontext's mcontext, HONOURING any modifications
 *              the handler made (e.g. WasmTrapHandler / the SpiderMonkey JIT
 *              rewrite gregs[REG_EIP] to redirect past a WASM trap).  User
 *              CS/SS/DS/ES are forced and IF kept set so a handler can't escalate.
 *   marker 0 → restore from a plain saved registers_t.
 * Returns 0 on success, -1 if `addr` is not a valid user range (caller kills). */
int sigreturn_restore(registers_t *regs, uint32_t addr, uint32_t marker) {
    uint32_t need = (marker == 1) ? (uint32_t)sizeof(struct k_ucontext)
                                  : (uint32_t)sizeof(registers_t);
    if (addr < 0x08000000U || addr + need < addr || addr + need > 0xC0000000U)
        return -1;
    if (marker == 1) {
        struct k_ucontext   *uc = (struct k_ucontext *)(uintptr_t)addr;
        struct k_sigcontext *m  = &uc->uc_mcontext;
        regs->edi = m->edi; regs->esi = m->esi; regs->ebp = m->ebp;
        regs->ebx = m->ebx; regs->edx = m->edx; regs->ecx = m->ecx;
        regs->eax = m->eax; regs->eip = m->eip; regs->useresp = m->esp;
        regs->gs = m->gs;   regs->fs = m->fs;             /* TLS selectors */
        regs->ds = 0x23; regs->es = 0x23; regs->cs = 0x1B; regs->ss = 0x23;
        regs->eflags = (m->eflags & 0x000008D5U) | 0x00000202U;
    } else {
        __builtin_memcpy(regs, (void *)(uintptr_t)addr, sizeof(registers_t));
    }
    return 0;
}
