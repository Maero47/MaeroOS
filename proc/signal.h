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
#define SA_NOCLDSTOP  0x00000001
#define SA_NODEFER    0x40000000
#define SA_RESETHAND  0x80000000

/*
 * siginfo_t — matches Linux i386 ABI (si_signo, si_errno, si_code + union).
 * We only fill the first 12 bytes; the rest are zeroed.
 */
typedef struct {
    int  si_signo;
    int  si_errno;
    int  si_code;
    /* Padding to 128 bytes total (standard Linux siginfo_t size) */
    char _pad[116];
} siginfo_t;

/* Forward declarations */
struct proc;
struct registers;

/* Set signal sig pending on process p; wake it if sleeping */
void signal_send(struct proc *p, int sig);

/*
 * True if p has a pending unblocked signal that will actually do something
 * (not SIG_IGN, not a default-ignored signal like SIGCHLD/SIGCONT).
 * Blocking I/O paths use this to abort their sleep loops so the signal can
 * be delivered — without it, processes blocked in pipe/PTY reads are
 * unkillable.
 */
int signal_interrupt_pending(struct proc *p);

/*
 * Check and deliver pending signals for current_proc.
 * Called at the end of every syscall before returning to user mode.
 * regs is the trapframe; may not return if a fatal signal kills the process.
 */
void signal_deliver_pending(struct registers *regs);
int  sigreturn_restore(struct registers *regs, uint32_t addr, uint32_t marker);
