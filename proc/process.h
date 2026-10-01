#pragma once
#include <stdint.h>
#include <registers.h>
#include <kernel/config.h>
#include "signal.h"

/* Forward declarations */
struct vfs_node;
struct pipe_buf;
struct net_socket;

/* ── File descriptor table ──────────────────────────────────────────────── */

typedef enum {
    FD_NONE   = 0,  /* slot unused */
    FD_FILE   = 1,  /* regular VFS file */
    FD_PIPE_R = 2,  /* read end of a pipe */
    FD_PIPE_W = 3,  /* write end of a pipe */
    FD_SOCKET = 4,  /* network socket (AF_INET / lwIP) */
    FD_USOCKET = 5, /* local socket (AF_UNIX) */
    FD_EPOLL  = 6,  /* epoll instance (epoll_create1) */
    FD_EVENTFD = 7, /* eventfd counter object (eventfd/eventfd2) */
} fd_type_t;

/* Open flags stored in proc_file_t (match Linux O_ values) */
#define O_RDONLY   0
#define O_WRONLY   1
#define O_RDWR     2
#define O_ACCMODE  3
#define O_CREAT    0x040
#define O_EXCL     0x080
#define O_TRUNC    0x200
#define O_APPEND   0x400
#define O_NONBLOCK 0x800
#define O_CLOEXEC  0x80000

/* FD flags (separate from open flags — set via fcntl F_SETFD) */
#define FD_CLOEXEC 1

typedef struct {
    fd_type_t        type;
    struct vfs_node *node;    /* FD_FILE: vfs node */
    uint32_t         offset;  /* FD_FILE: current read/write position */
    struct pipe_buf *pipe;    /* FD_PIPE_R/FD_PIPE_W: the pipe buffer */
    struct net_socket *socket;/* FD_SOCKET: network socket object */
    struct usocket   *usock;  /* FD_USOCKET: AF_UNIX socket object */
    struct epoll     *epoll;  /* FD_EPOLL: epoll instance */
    struct eventfd_obj *efd;  /* FD_EVENTFD: counter object */
    int              flags;   /* O_RDONLY / O_WRONLY / O_RDWR */
    uint8_t          cloexec; /* FD_CLOEXEC: close on exec */
    uint32_t         seals;   /* memfd F_ADD_SEALS bitmask (accepted, not enforced) */
    uint32_t         fid;     /* open-file identity for epoll, given on the
                               * first copy (fd_copy) or EPOLL_CTL_ADD; every
                               * copy shares it, close clears it; 0 = none */
    char             path[256]; /* FD_FILE: canonical opened path */
    /* The mount(2) mount the file was opened on (NULL: the boot tree) and
     * its number, for EROFS on descriptor-based changes (fchmod, ...). */
    struct vfs_mnt  *mnt;
    uint32_t         mnt_seq;
} proc_file_t;

/* Shared, reference-counted file-descriptor table.  Threads of a process
 * (clone with CLONE_FILES) share ONE fdtable, so an fd opened by any thread is
 * immediately visible to every thread of the group (Linux semantics).  fork()
 * gives the child its own copy.  Released when the last sharer exits. */
struct fdtable {
    int         refcount;
    proc_file_t f[MAX_FD];
};
struct proc;
struct fdtable *fdtable_alloc(void);            /* fresh table, refcount=1 */
void            fdtable_attach(struct proc *p, struct fdtable *t);  /* set fdt+ofile */
void            fdtable_put(struct proc *p);    /* decref; release fds + free at 0 */

/* fd-table reference counting (defined in syscall.c) — exposed so SCM_RIGHTS
 * fd-passing in usocket.c can retain/release the underlying open-file objects. */
void fd_retain(proc_file_t *f);
void fd_copy(proc_file_t *dst, proc_file_t *src);   /* dup: fid + copy + retain */
void fd_release(proc_file_t *f);
uint32_t fd_new_fid(void);                          /* a fresh open-file identity */

/* SCM_RIGHTS fd-passing over AF_UNIX sockets (defined in usocket.c). */
#define SCM_MAX_FDS 16
struct usocket;
int usocket_send_fds(struct usocket *s, proc_file_t *files, int n, uint32_t *id_out);
int usocket_cancel_fds(struct usocket *s, uint32_t id);
int usocket_recv_fds(struct usocket *s, proc_file_t *out, int max);

/* Diagnostic: print a one-line state snapshot of every live process. */

/* ── Context switch ─────────────────────────────────────────────────────── */

/*
 * Saved callee-saved registers for a kernel context switch.
 * Layout must exactly match the push/pop order in swtch.asm.
 */
struct context {
    uint32_t edi;
    uint32_t esi;
    uint32_t ebx;
    uint32_t ebp;
    uint32_t eip;   /* implicit — popped by RET in swtch */
};

/* ── Process states ─────────────────────────────────────────────────────── */

typedef enum {
    PROC_UNUSED   = 0,
    PROC_EMBRYO,
    PROC_RUNNABLE,
    PROC_RUNNING,
    PROC_SLEEPING,
    PROC_STOPPED,   /* stopped by SIGSTOP/SIGTSTP; resumed by SIGCONT */
    PROC_ZOMBIE,
} proc_state_t;

/* Supplementary groups a process may hold.  Linux allows NGROUPS_MAX (65536);
 * setgroups() with more than this is -EINVAL here. */
#define PROC_NGROUPS_MAX 32

/* ── Process control block ──────────────────────────────────────────────── */

struct proc {
    proc_state_t     state;
    int              pid;
    uint32_t         pgdir_phys;  /* physical address of page directory */
    uint8_t         *kstack;      /* base of kernel stack (kmalloc'd) */
    struct context  *context;     /* saved kernel SP (points into kstack) */
    registers_t     *tf;          /* trapframe pointer (for user processes) */
    struct proc     *parent;      /* parent process */
    int              exit_status;
    int              stop_sig;      /* signal that last stopped this process */
    int              stop_reported; /* waitpid(WUNTRACED) already saw this stop */
    /* Group stop (Linux signal->group_stop_count + JOBCTL_STOP_PENDING).  On
     * the LEADER, group_stop is the stop signal in force for the whole thread
     * group (0: none) and group_continued records a SIGCONT that ended one, for
     * waitpid(WCONTINUED).  On every thread, jobctl_stop asks it to stop on its
     * next return to user mode. */
    int              group_stop;
    int              group_continued;
    int              jobctl_stop;
    int              time_slice;
    int              last_syscall;            /* diagnostic: last syscall number */
    int              kprof_bucket;            /* kprof: bucket in effect when this
                                               * thread last switched away */
    char             name[16];

    /* File descriptor table — shared (refcounted) among threads of the group,
     * private per process.  `ofile` aliases `fdt->f` so ->ofile[i] still works. */
    struct fdtable  *fdt;
    proc_file_t     *ofile;

    /* Virtual memory */
    uint32_t         heap_end;    /* user heap break (managed by sys_brk) */

    /* Signals.  pending/blocked are per thread (Linux task->pending, ->blocked);
     * the handler table is shared by the thread group (see struct sighand), and
     * so are process-directed pending signals (see struct sigshared). */
    uint32_t         pending_sigs;          /* bitmask of pending signals */
    uint32_t         blocked_sigs;          /* bitmask of blocked signals */
    struct sighand  *sighand;               /* shared under CLONE_SIGHAND */
    struct sigshared *sigshared;            /* process-wide pending set, shared
                                             * under CLONE_THREAD; NULL for
                                             * kernel threads */
    uint32_t         sigframe_addr;         /* user addr of saved trapframe for sigreturn */

    /* sigsuspend's temporary mask (Linux saved_sigmask + TIF_RESTORE_SIGMASK).
     * sigsuspend installs its mask and returns WITHOUT putting the caller's
     * mask back, so the signal it is waiting for is still deliverable when the
     * return-to-user path looks for one; that path puts `saved_sigmask` back
     * once it has decided what to deliver.  Restoring the mask in the syscall
     * itself makes the awaited signal blocked again, so nothing is deliverable,
     * the -ERESTARTNOHAND return restarts the call, and the pair spins. */
    uint32_t         saved_sigmask;
    int              restore_sigmask;

    /* Detail of a synchronous fault waiting to be delivered (Linux
     * force_sig_fault's siginfo): which signal it belongs to, its si_code and
     * the faulting address.  Consumed by the delivery of that signal, so an
     * unrelated signal delivered first never picks it up. */
    int              fault_sig;
    int              fault_code;
    uint32_t         fault_addr;

    /* sigaltstack (Linux task->sas_ss_sp / sas_ss_size): the stack SA_ONSTACK
     * handlers run on.  Size 0 means none is installed.  Inherited by fork,
     * cleared by exec. */
    uint32_t         sas_sp, sas_size;

    /* Group exit (Linux signal_struct SIGNAL_GROUP_EXIT + group_exit_code),
     * kept on the thread-group leader: set when exit_group() or a fatal signal
     * ends the whole process, so the status waitpid() reports is the group's,
     * not whatever SIGKILL later delivered to the leader itself. */
    int              group_exit;

    /* Sleep channel (non-NULL when state == PROC_SLEEPING) */
    void            *sleep_chan;
    uint32_t         wake_tick;   /* PIT tick deadline for timed sleeps; consumed
                                   * (zeroed) by whichever path ends the sleep so a
                                   * deadline never leaks into a later sleep */
    uint8_t          sleep_timed_out; /* set by scheduler_tick when it ended the
                                   * sleep by deadline; sleep_on() returns it */
    uint32_t         sleep_tick;  /* PIT tick when this sleep began */
    uint32_t         sleep_seq;   /* monotonic enqueue order — FIFO wakeup (Linux futex plist) */
    uint8_t          futex_wait;  /* 1: sleeping in a futex wait (futex_wake_n only
                                   * matches these, so a wake_up() on a channel that
                                   * happens to equal a user address cannot hit them);
                                   * 2: that wait was ended by a FUTEX_WAKE, which
                                   * sys_futex reports as 0 ahead of any signal */
    uint8_t          futex_shared; /* futex wait is process-SHARED (FUTEX_PRIVATE
                                    * bit clear).  Private futexes are keyed by
                                    * (tgid, uaddr); shared by physical page.  This
                                    * mirrors Linux get_futex_key and stops a
                                    * private condvar signal in one process from
                                    * being mis-delivered to a same-vaddr waiter in
                                    * ANOTHER process (ASLR is off, so all Firefox
                                    * processes share virtual addresses). */
    uint32_t         futex_phys;   /* physical addr of the futex word (shared only),
                                    * resolved at WAIT time for cross-process match. */

    /* Current working directory (absolute path, always starts with '/') */
    char             cwd[256];

    /* chroot(2): this process's root directory, or NULL when it is not
     * chrooted.  The node is pinned (vfs_retain) when chroot() resolves it,
     * so renaming or replacing the path it was reached by later changes
     * nothing; fork and clone take their own reference, exit and fork_abort
     * drop it.  cwd, fd paths and every path the process passes in are
     * relative to it; vfs_lookup() walks from it (fs/vfs.c). */
    struct vfs_node *root_node;

    /* Next available address for anonymous mmap allocations */
    uint32_t         mmap_next;

    /* Last file-backed page this thread faulted on, for the profiler's
     * sequentiality counter (see KPE_PF_FILE_SEQ). */
    uint32_t         last_file_fault;

    /* Demand-paged anonymous memory regions (mmap MAP_ANONYMOUS).  Owned by the
     * thread-group leader (threads share the address space).  Pages in a VMA are
     * allocated lazily on first fault instead of eagerly at mmap time — this
     * makes 8 MiB thread stacks cost only what they touch. */
    struct vma      *vmas;

    /* CPU accounting */
    uint32_t         utime_ticks;   /* PIT ticks spent running */
    uint32_t         sched_count;   /* times scheduled */

    /* Non-zero: the PIT tick must not preempt this process (it is inside a
     * non-reentrant critical section, e.g. the lwIP stack).  Nests. */
    int              no_preempt;

    /* Threads: thread-group id (== leader pid; getpid returns this) and
     * the user TLS segment base loaded into the GDT on context switch.
     * `parent` above always points at a thread-group LEADER (Linux real_parent
     * is current->group_leader): children belong to the process, so any thread
     * of the parent can wait for them, and CLONE_THREAD siblings inherit the
     * leader's parent rather than being "children" of the creating thread. */
    int              tgid;
    uint32_t         tls_base;
    /* CLONE_CHILD_CLEARTID address: on thread exit the kernel writes 0 here
     * and futex-wakes it — this is how pthread_join() learns a thread ended. */
    uint32_t         clear_child_tid;
    /* CLONE_CHILD_SETTID for a fork-style clone: the child's tid must be
     * written into the CHILD's copy of the address space, so it is done by
     * the child itself on first dispatch (forkret), like Linux schedule_tail. */
    uint32_t         set_child_tid;
    /* CLONE_VM without CLONE_THREAD (vfork, posix_spawn, Breakpad's dumper):
     * the child is its own thread group but runs in the creator's address
     * space, whose VMA list and mmap cursor live on that group's leader.
     * mmap_owner() follows this pointer; cleared by exec (own pgdir). */
    struct proc     *vm_owner;

    /* set_robust_list head: user pointer to this thread's list of held robust
     * mutexes.  On exit the kernel walks it, marks each owned futex
     * FUTEX_OWNER_DIED and wakes a waiter, so a lock held by a crashed/exited
     * thread is recoverable instead of deadlocking every other thread. */
    uint32_t         robust_list_head;

    /* CLONE_VFORK: the child shares the address space and the parent must BLOCK
     * until the child execs or exits (posix_spawn / vfork rely on this).  The
     * child carries a pointer to the blocked parent and the parent spins on its
     * own flag; the child clears+wakes it on exec/exit. */
    struct proc     *vfork_parent;   /* on child: parent to wake on exec/exit */
    volatile int     vfork_waiting;  /* on parent: 1 while blocked in vfork */

    /* x87/SSE state (fxsave format).  fxsave needs 16-byte alignment;
     * fpu_area() rounds the raw buffer up. */
    uint8_t          fpu_raw[512 + 16];

    /* File creation mask (default 022) */
    uint32_t         umask;

    /* User/group credentials (0 = root).  euid/egid drive permission
     * checks; uid/gid are the real ids; suid/sgid are the saved set-IDs (Linux
     * cred->suid/sgid): exec copies the new euid/egid into them, and an
     * unprivileged setuid()/setresuid() may only move among real, effective
     * and saved.  Inherited across fork/clone. */
    uint32_t         uid, gid, euid, egid, suid, sgid;
    /* Supplementary group list (Linux cred->group_info): setgroups() sets it,
     * fork/clone copy it, exec keeps it.  A file whose group is any of these
     * is accessed with its group permission bits. */
    uint32_t         ngroups;
    uint32_t         groups[PROC_NGROUPS_MAX];
    /* Set by execve, clear in a fresh fork (Linux !PF_FORKNOEXEC): a parent
     * may no longer setpgid() a child that has exec'd (-EACCES). */
    int              did_exec;

    /* Process group and session IDs */
    int              pgrp;   /* process group ID */
    int              sid;    /* session ID */
    struct vfs_node *ctty;   /* controlling terminal, if any */

    /* Path of the executable (set by sys_exec) */
    char             exe[256];

    /* Process-image layout, captured at exec, for a real /proc/self/maps:
     * image_start/end span the loaded ELF, brk_base is the heap segment start
     * (heap_end grows up from here via sys_brk). */
    uint32_t         image_start, image_end;
    uint32_t         brk_base;

    /* Captured argv/envp/auxv (at exec) so /proc/self/{cmdline,environ,auxv}
     * can expose them for the process lifetime. */
    char             cmdline[1024];  uint32_t cmdline_len;
    char             environ[2048];  uint32_t environ_len;
    uint8_t          auxv_data[2 * 24 * 4];  uint32_t auxv_bytes;

    /* Fault-loop breaker: if the same faulting EIP repeats with no forward
     * progress (a SIGSEGV handler that returns straight to the bad
     * instruction), force-kill rather than re-delivering forever.  For a
     * page fault the address must repeat too: one store instruction faulting
     * on a different page each time is a caller probing or write-barriering
     * memory, not a loop. */
    uint32_t         last_fault_eip;
    uint32_t         last_fault_addr;
    uint32_t         fault_repeat;

    /* Execute permission policy of the image, set at exec from its
     * PT_GNU_STACK (Linux fs/binfmt_elf.c), inherited across fork and clone:
     *   read_implies_exec — no PT_GNU_STACK at all: a legacy i386 binary that
     *       may run code from anything readable (READ_IMPLIES_EXEC), so every
     *       PROT_READ mapping, the heap and the stack are executable;
     *   exec_stack — PT_GNU_STACK asks for PF_X: the stack is executable.
     * Both 0 is the normal case: only PF_X segments and PROT_EXEC mappings
     * run.  Without NX (legacy paging) the hardware ignores all of it. */
    uint8_t          read_implies_exec;
    uint8_t          exec_stack;
};

/* Is the main/grown stack of p executable? */
static inline int proc_stack_exec(const struct proc *p) {
    return p && (p->exec_stack || p->read_implies_exec);
}
/* mmap/mprotect prot as the image's policy widens it (PROT_READ → +EXEC). */
static inline uint32_t proc_prot_adjust(const struct proc *p, uint32_t prot) {
    return (p && p->read_implies_exec && (prot & 0x1U)) ? (prot | 0x4U) : prot;
}

/* Process table and current process */
extern struct proc ptable[];

/* SMP: `current_proc` is PER-CPU — the process running on the calling CPU.  It
 * expands to that CPU's slot in cpus[] (indexed by logical CPU id), so reads and
 * assignments (`current_proc = p`, `current_proc->field`, `&current_proc->field`)
 * all act on the running CPU's process with no global to race.  Before the LAPIC
 * is up this_cpu_id() is 0, matching the old single-`current_proc` behaviour. */
#include "../arch/i686/cpu/percpu.h"
#define current_proc (cpus[this_cpu_id()].proc)

/* Allocate an EMBRYO slot and set up its kernel stack */
struct proc *allocproc(void);

/* The thread-group leader of p (the proc whose pid == p->tgid); p itself if
 * the leader slot is gone. */
struct proc *proc_group_leader(struct proc *p);

/* Free a ZOMBIE's kernel stack and page directory and return its slot (Linux
 * release_task).  Called by waitpid for a reaped process, by the scheduler for
 * an exited non-leader thread, and for orphans adopted by init. */
void proc_release(struct proc *p);

/* True if no thread of leader's group other than the leader itself is still
 * alive (Linux thread_group_empty); exited threads awaiting release count as
 * gone. */
int proc_group_empty(struct proc *leader);

/* Initialize the process subsystem */
void proc_init(void);

/* forkret — first function a new kernel task runs after swtch() returns */
void forkret(void);

/* Create a kernel thread that runs fn() */
struct proc *proc_create_kthread(void (*fn)(void), const char *name);

/* 16-byte-aligned view of a process's fxsave area */
static inline uint8_t *fpu_area(struct proc *p) {
    return (uint8_t *)(((uintptr_t)p->fpu_raw + 15) & ~(uintptr_t)15);
}

/* Create a user-mode process from raw bytecode */
struct proc *proc_create_userproc(const uint8_t *code, uint32_t code_len,
                                   const char *name);

/* Create a user-mode process by loading an ELF32 executable from a VFS node */
struct proc *proc_create_from_elf(struct vfs_node *node, const char *name);

/* Iterate a process's anonymous VMAs for the maps procfs file (the struct vma
 * layout is private to proc/syscall.c).  Returns 0 and fills start/end/prot
 * for the idx-th VMA, or -1 when idx is past the end. */
int proc_vma_iter(struct proc *p, int idx,
                  uint32_t *start, uint32_t *end, uint32_t *prot);
/* Same, plus whether the mapping is MAP_SHARED and the backing file's name
 * ("" for anonymous). */
int proc_vma_iter_ex(struct proc *p, int idx, uint32_t *start, uint32_t *end,
                     uint32_t *prot, int *shared, const char **name);
