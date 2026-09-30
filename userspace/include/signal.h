#pragma once

#include <stddef.h>

typedef void (*sighandler_t)(int);

/* Signals 1..32, signal n at bit (n - 1) — the kernel's user sigset layout.
 * The kernel handles no signal above 31, so one word covers every signal it
 * can deliver; libc widens it to the 8-byte kernel sigset at the syscall. */
typedef unsigned long sigset_t;

typedef struct {
    int si_signo;
    int si_errno;
    int si_code;
    union {
        char __pad[128 - 3 * sizeof(int)];
        struct { int si_pid; unsigned si_uid; int si_status; } __kill;
        struct { void *si_addr; } __fault;
    } __si_fields;
} siginfo_t;

#define si_pid    __si_fields.__kill.si_pid
#define si_uid    __si_fields.__kill.si_uid
#define si_status __si_fields.__kill.si_status
#define si_addr   __si_fields.__fault.si_addr

struct sigaction {
    union {
        sighandler_t sa_handler;
        void (*sa_sigaction)(int, siginfo_t *, void *);
    } __sa_handler;
    sigset_t sa_mask;
    int sa_flags;
    void (*sa_restorer)(void);
};
#define sa_handler   __sa_handler.sa_handler
#define sa_sigaction __sa_handler.sa_sigaction

typedef struct {
    void  *ss_sp;
    int    ss_flags;
    size_t ss_size;
} stack_t;

#define SIG_ERR ((sighandler_t)-1)
#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)

#define SA_NOCLDSTOP 0x00000001
#define SA_NOCLDWAIT 0x00000002
#define SA_SIGINFO   0x00000004
#define SA_RESTORER  0x04000000
#define SA_ONSTACK   0x08000000
#define SA_RESTART   0x10000000
#define SA_NODEFER   0x40000000
#define SA_RESETHAND 0x80000000
#define SA_NOMASK    SA_NODEFER
#define SA_ONESHOT   SA_RESETHAND

#define SS_ONSTACK  1
#define SS_DISABLE  2
#define MINSIGSTKSZ 2048
#define SIGSTKSZ    8192

#define SI_USER   0
#define SI_KERNEL 0x80

#define SIGHUP   1
#define SIGINT   2
#define SIGQUIT  3
#define SIGILL   4
#define SIGTRAP  5
#define SIGABRT  6
#define SIGIOT   SIGABRT
#define SIGBUS   7
#define SIGFPE   8
#define SIGKILL  9
#define SIGUSR1  10
#define SIGSEGV  11
#define SIGUSR2  12
#define SIGPIPE  13
#define SIGALRM  14
#define SIGTERM  15
#define SIGSTKFLT 16
#define SIGCHLD  17
#define SIGCONT  18
#define SIGSTOP  19
#define SIGTSTP  20
#define SIGTTIN  21
#define SIGTTOU  22
#define SIGURG   23
#define SIGXCPU  24
#define SIGXFSZ  25
#define SIGVTALRM 26
#define SIGPROF  27
#define SIGWINCH 28
#define SIGIO    29
#define SIGPOLL  SIGIO
#define SIGPWR   30
#define SIGSYS   31
#define NSIG     32

/* struct sigevent, Linux i386 layout (64 bytes).  The kernel handles
 * SIGEV_SIGNAL, SIGEV_NONE and SIGEV_THREAD_ID; this libc has no SIGEV_THREAD. */
union sigval { int sival_int; void *sival_ptr; };
struct sigevent {
    union sigval sigev_value;
    int sigev_signo;
    int sigev_notify;
    union {
        int  __pad[13];
        int  __tid;
    } __sev_fields;
};
#define sigev_notify_thread_id __sev_fields.__tid
#define SIGEV_SIGNAL    0
#define SIGEV_NONE      1
#define SIGEV_THREAD    2
#define SIGEV_THREAD_ID 4

#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

sighandler_t signal(int signum, sighandler_t handler);
int          kill(int pid, int sig);
int          raise(int sig);
int          sigaction(int signum, const struct sigaction *act,
                       struct sigaction *oldact);
int          sigprocmask(int how, const sigset_t *set, sigset_t *oldset);
int          pthread_sigmask(int how, const sigset_t *set, sigset_t *oldset);
int          sigpending(sigset_t *set);
int          sigsuspend(const sigset_t *mask);
int          sigaltstack(const stack_t *ss, stack_t *old_ss);
int          sigemptyset(sigset_t *set);
int          sigfillset(sigset_t *set);
int          sigaddset(sigset_t *set, int signum);
int          sigdelset(sigset_t *set, int signum);
int          sigismember(const sigset_t *set, int signum);
