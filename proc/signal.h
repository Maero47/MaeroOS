#pragma once
#include <stdint.h>

#define NSIGS   32

typedef void (*sighandler_t)(int);

#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)

/* POSIX signal numbers */
#define SIGHUP   1
#define SIGINT   2
#define SIGQUIT  3
#define SIGILL   4
#define SIGTRAP  5
#define SIGABRT  6
#define SIGBUS   7
#define SIGFPE   8
#define SIGKILL  9
#define SIGUSR1  10
#define SIGSEGV  11
#define SIGUSR2  12
#define SIGPIPE  13
#define SIGALRM  14
#define SIGTERM  15
#define SIGCHLD  17
#define SIGCONT  18
#define SIGSTOP  19
#define SIGTSTP  20
#define SIGTTIN  21
#define SIGTTOU  22

/* sa_flags bits */
#define SA_SIGINFO    0x00000004
#define SA_RESTART    0x10000000
#define SA_ONSTACK    0x08000000
#define SA_NOCLDSTOP  0x00000001
#define SA_NODEFER    0x40000000
#define SA_RESETHAND  0x80000000

/* si_code values (asm-generic/siginfo.h).  A signal raised by kill()/tgkill
 * carries SI_USER; a synchronous fault carries the code for its cause. */
#define SI_USER       0
#define SI_KERNEL     0x80
#define SEGV_MAPERR   1        /* address not mapped to object */
#define SEGV_ACCERR   2        /* invalid permissions for mapped object */
#define BUS_ADRALN    1        /* invalid address alignment */
#define ILL_ILLOPN    2        /* illegal operand */
#define FPE_INTDIV    1        /* integer divide by zero */

/* sigaltstack ss_flags (include/uapi/signal.h). */
#define SS_ONSTACK    1
#define SS_DISABLE    2

/*
 * siginfo_t — matches Linux i386 ABI (si_signo, si_errno, si_code + union).
 * We only fill the first 12 bytes; the rest are zeroed.
 */
typedef struct {
    int  si_signo;
    int  si_errno;
    int  si_code;
    union {
        /* Padding to 128 bytes total (standard Linux siginfo_t size) */
        char _pad[116];
        struct { uint32_t si_addr; } _sigfault;   /* SIGSEGV/BUS/ILL/FPE */
        struct { int32_t si_pid; uint32_t si_uid; } _kill;
    } _u;
} siginfo_t;

/* Kernel-internal restart codes (never visible to user space; the values are
 * Linux's include/linux/errno.h ones).  A blocking syscall that is interrupted
 * by a signal returns one of these; signal_return_to_user() decides what the
 * caller sees, exactly as Linux's handle_signal()/arch do_signal() do:
 *   -EINTR (4)            : behaves like Linux -ERESTARTSYS.  If a handler runs
 *                           and was installed with SA_RESTART, the syscall is
 *                           re-executed with its original number; otherwise the
 *                           user sees EINTR.  If no handler runs (the signal was
 *                           a stop signal or has since become ignored) the
 *                           syscall is always restarted.
 *   -ERESTARTNOHAND (514) : restarted only if NO handler runs; when a handler
 *                           runs the user always sees EINTR regardless of
 *                           SA_RESTART.  This is what Linux returns from poll,
 *                           select, epoll_wait, nanosleep and sigsuspend
 *                           (fs/select.c do_sys_poll, kernel/time/hrtimer.c
 *                           nanosleep, kernel/signal.c sigsuspend). */
#define ERESTARTNOHAND  514

/* Shared signal-handler table (Linux struct sighand_struct, kernel/fork.c
 * copy_sighand).  Threads created with CLONE_SIGHAND (which CLONE_THREAD
 * requires) share ONE table, so a sigaction() issued by any thread is in force
 * for every thread of the process; fork() copies it; exec resets it. */
struct sighand {
    int          refcount;
    sighandler_t handlers[NSIGS];   /* per-signal: SIG_DFL/SIG_IGN/fn */
    uint32_t     flags[NSIGS];      /* per-signal sa_flags (SA_RESTART etc.) */
    uint32_t     mask[NSIGS];       /* per-signal sa_mask, blocked while it runs */
};
struct sighand *sighand_alloc(void);                 /* zeroed table, refcount=1 */
struct sighand *sighand_copy(struct sighand *src);   /* private copy, refcount=1 */
void            sighand_put(struct sighand *sh);     /* decref; free at 0 */

/* Forward declarations */
struct proc;
struct registers;

/* Queue sig on the single thread p (Linux send_signal for a thread-directed
 * signal: tgkill, a synchronous fault).  Wakes p only when the signal is
 * deliverable to it (unblocked and neither ignored nor default-ignored); an
 * ignored signal is discarded like Linux sig_ignored(). */
void signal_send(struct proc *p, int sig);

/* Queue a SYNCHRONOUS fault signal on p, carrying the siginfo detail Linux's
 * force_sig_fault() attaches: si_code (SEGV_MAPERR, BUS_ADRALN, …) and the
 * address that caused it.  A SA_SIGINFO handler for that signal sees both; the
 * detail is consumed by the delivery and never leaks to another signal. */
void signal_send_fault(struct proc *p, int sig, int code, uint32_t addr);

/* The user sigset_t numbers bit (sig - 1) for signal sig (Linux sigmask()),
 * whereas pending_sigs/blocked_sigs use bit sig.  Convert at every boundary —
 * the sigaction sa_mask and the signal frame's uc_sigmask included. */
static inline uint32_t sigset_from_user(uint32_t uset) { return uset << 1; }
static inline uint32_t sigset_to_user(uint32_t kset)   { return kset >> 1; }

/* Process-directed signal (kill, SIGCHLD, tty signals): queue sig on ONE thread
 * of p's thread group that does not block it, preferring the group leader, as
 * Linux complete_signal() does.  If every thread blocks it, it stays pending on
 * the leader until unblocked. */
void signal_send_group(struct proc *p, int sig);

/* Deliver sig to every process whose pgrp is pg (one thread per process).
 * Returns the number of processes signalled. */
int signal_send_pgrp(int pg, int sig);

/*
 * True if p has a pending unblocked signal that will actually do something
 * (not SIG_IGN, not a default-ignored signal like SIGCHLD/SIGCONT).
 * Blocking I/O paths use this to abort their sleep loops so the signal can
 * be delivered — without it, processes blocked in pipe/PTY reads are
 * unkillable.
 */
int signal_interrupt_pending(struct proc *p);

/*
 * Check and deliver pending signals for current_proc before it returns to
 * user mode.  regs is the trapframe; may not return if a fatal signal ends the
 * process.  syscall_nr is the number of the syscall being returned from (so an
 * interrupted call can be restarted), or -1 when returning from an interrupt
 * or exception.
 */
void signal_return_to_user(struct registers *regs, int syscall_nr);
static inline void signal_deliver_pending(struct registers *regs) {
    signal_return_to_user(regs, -1);
}
int  sigreturn_restore(struct registers *regs, uint32_t addr, uint32_t marker);

/* Start a group exit (Linux do_group_exit): freeze `status` as the process's
 * exit status, SIGKILL every other thread of the calling thread's group, then
 * exit the calling thread.  status is already wait-encoded (see proc_exit). */
void proc_group_exit(int status) __attribute__((noreturn));
