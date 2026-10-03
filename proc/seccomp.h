#pragma once
#include <stdint.h>

/*
 * seccomp(2), prctl(PR_SET_SECCOMP) and prctl(PR_SET_NO_NEW_PRIVS): the Linux
 * syscall-filter sandbox Firefox's child processes install (docs/sandbox.md).
 *
 * A filter is a classic BPF program run over a struct seccomp_data before
 * every system call of the thread that holds it.  Filters stack: each new one
 * points at the one that was in force (->prev), the chain is shared by
 * reference between the threads and children that inherited it, and nothing
 * can remove or replace a filter once attached.
 */

#define SECCOMP_MODE_DISABLED 0
#define SECCOMP_MODE_STRICT   1
#define SECCOMP_MODE_FILTER   2

/* seccomp(2) operations and SECCOMP_SET_MODE_FILTER flags. */
#define SECCOMP_SET_MODE_STRICT   0
#define SECCOMP_SET_MODE_FILTER   1
#define SECCOMP_GET_ACTION_AVAIL  2
#define SECCOMP_GET_NOTIF_SIZES   3

#define SECCOMP_FILTER_FLAG_TSYNC        (1u << 0)
#define SECCOMP_FILTER_FLAG_LOG          (1u << 1)
#define SECCOMP_FILTER_FLAG_SPEC_ALLOW   (1u << 2)
#define SECCOMP_FILTER_FLAG_NEW_LISTENER (1u << 3)
#define SECCOMP_FILTER_FLAG_TSYNC_ESRCH  (1u << 4)

/* Filter return values: the action in the top 16 bits, its data below. */
#define SECCOMP_RET_KILL_PROCESS 0x80000000U
#define SECCOMP_RET_KILL_THREAD  0x00000000U
#define SECCOMP_RET_TRAP         0x00030000U
#define SECCOMP_RET_ERRNO        0x00050000U
#define SECCOMP_RET_USER_NOTIF   0x7fc00000U
#define SECCOMP_RET_TRACE        0x7ff00000U
#define SECCOMP_RET_LOG          0x7ffc0000U
#define SECCOMP_RET_ALLOW        0x7fff0000U
#define SECCOMP_RET_ACTION_FULL  0xffff0000U
#define SECCOMP_RET_DATA         0x0000ffffU

#define AUDIT_ARCH_I386          0x40000003U
#define SYS_SECCOMP              1          /* si_code of a SECCOMP_RET_TRAP SIGSYS */
#define SIGSYS                   31

/* Limits (Linux BPF_MAXINSNS, MAX_INSNS_PER_PATH). */
#define SECCOMP_MAX_INSNS        4096
#define SECCOMP_MAX_PATH_INSNS   32768

struct sock_filter {        /* struct sock_filter (uapi/linux/filter.h) */
    uint16_t code;
    uint8_t  jt;
    uint8_t  jf;
    uint32_t k;
};

struct seccomp_filter {
    int                    refcount;
    struct seccomp_filter *prev;     /* the filter that was in force before */
    uint16_t               len;
    uint8_t                log;      /* SECCOMP_FILTER_FLAG_LOG */
    struct sock_filter     insns[];
};

struct proc;
struct registers;

/* Run current_proc's filters for the syscall in regs (number `nr`, arguments
 * in ebx..ebp).  Returns 0 when the syscall may proceed; otherwise it must be
 * skipped, regs->eax already holds what the caller sees (an errno, or the
 * number itself for a SIGSYS trap whose handler supplies the result), and any
 * signal it raised is pending.  KILL actions do not return. */
int  seccomp_syscall_enter(struct registers *regs, uint32_t nr);

int  sys_seccomp(uint32_t op, uint32_t flags, uint32_t uargs);
int  seccomp_prctl_set(uint32_t mode, uint32_t uprog);

/* fork/clone: the child holds the parent's chain, mode and no_new_privs. */
void seccomp_fork(struct proc *child, struct proc *parent);
/* The thread is exiting (or a half-built child is discarded). */
void seccomp_release(struct proc *p);

/* Exposed for the self-check below and for /proc/<pid>/status. */
int  seccomp_filter_count(const struct proc *p);
