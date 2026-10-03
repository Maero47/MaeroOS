/*
 * seccomp — the Linux system-call filter (Documentation/userspace-api/
 * seccomp_filter.rst, seccomp(2), prctl(2)).
 *
 * Firefox's content, media and socket processes lock themselves down with it
 * (security/sandbox/linux in the Firefox source): prctl(PR_SET_NO_NEW_PRIVS),
 * then seccomp(SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_TSYNC, prog) with a
 * classic-BPF program compiled by Chromium's bpf_dsl.  The program sees a
 * struct seccomp_data for every system call and answers ALLOW, ERRNO(n),
 * TRAP(n) (SIGSYS, whose handler emulates the call — that is how file access
 * is forwarded to the parent's SandboxBroker), or KILL.
 *
 * Security properties, all enforced here:
 *   - a program is validated before it is accepted: 1..4096 instructions,
 *     only the opcodes Linux allows for seccomp, loads only of aligned 32-bit
 *     words inside seccomp_data, scratch cells written before they are read,
 *     no division by a zero constant, no shift by 32 or more, every jump
 *     forward and inside the program, and a RET as the last instruction;
 *   - so a run takes at most `len` steps: the program counter only grows;
 *   - a filter can only be added, never removed or replaced, and it is kept
 *     across fork, clone and execve;
 *   - installing one needs no_new_privs (or root), and no_new_privs makes
 *     execve ignore set-user-ID and set-group-ID bits, so a filtered process
 *     cannot exec a privileged program that would run under its filter;
 *   - TSYNC moves every other thread of the process to the caller's chain only
 *     if each one's chain is an ancestor of it, so it can never weaken one.
 *
 * Written from the documentation and the uapi definitions; no Linux code.
 */
#include "seccomp.h"
#include "process.h"
#include "signal.h"
#include "syscall.h"
#include "scheduler.h"
#include "../mm/heap.h"
#include "../kernel/printk.h"
#include <registers.h>
#include <stddef.h>

/* Classic BPF encoding (uapi/linux/bpf_common.h, filter.h). */
#define BPF_CLASS(c) ((c) & 0x07)
#define BPF_LD    0x00
#define BPF_LDX   0x01
#define BPF_ST    0x02
#define BPF_STX   0x03
#define BPF_ALU   0x04
#define BPF_JMP   0x05
#define BPF_RET   0x06
#define BPF_MISC  0x07
#define BPF_W     0x00
#define BPF_IMM   0x00
#define BPF_ABS   0x20
#define BPF_MEM   0x60
#define BPF_LEN   0x80
#define BPF_ADD   0x00
#define BPF_SUB   0x10
#define BPF_MUL   0x20
#define BPF_DIV   0x30
#define BPF_OR    0x40
#define BPF_AND   0x50
#define BPF_LSH   0x60
#define BPF_RSH   0x70
#define BPF_NEG   0x80
#define BPF_XOR   0xa0
#define BPF_JA    0x00
#define BPF_JEQ   0x10
#define BPF_JGT   0x20
#define BPF_JGE   0x30
#define BPF_JSET  0x40
#define BPF_K     0x00
#define BPF_X     0x08
#define BPF_A     0x10
#define BPF_TAX   0x00
#define BPF_TXA   0x80
#define BPF_MEMWORDS 16

/* struct seccomp_data on i386: nr, arch, instruction_pointer (u64),
 * args[6] (u64, the 32-bit registers zero-extended). */
#define SECCOMP_DATA_SIZE 64

#define EFAULT   14
#define EINVAL   22
#define EACCES   13
#define ENOMEM   12
#define ENOSYS   38
#define ESRCH    3
#define EOPNOTSUPP 95
#define MAX_ERRNO 4095

/* ── validation ──────────────────────────────────────────────────────────── */

/* The instructions a seccomp program may use (the same set Linux accepts):
 * no packet loads other than whole aligned words of seccomp_data, no MOD, no
 * indirect or byte/halfword loads, no RET X. */
static int insn_allowed(const struct sock_filter *f) {
    switch (f->code) {
    case BPF_LD | BPF_W | BPF_ABS:
        return f->k < SECCOMP_DATA_SIZE && (f->k & 3) == 0;
    case BPF_LD | BPF_W | BPF_LEN:
    case BPF_LDX | BPF_W | BPF_LEN:
    case BPF_RET | BPF_K:
    case BPF_RET | BPF_A:
    case BPF_ALU | BPF_NEG:
    case BPF_LD | BPF_IMM:
    case BPF_LDX | BPF_IMM:
    case BPF_MISC | BPF_TAX:
    case BPF_MISC | BPF_TXA:
    case BPF_JMP | BPF_JA:
        return 1;
    case BPF_LD | BPF_MEM:
    case BPF_LDX | BPF_MEM:
    case BPF_ST:
    case BPF_STX:
        return f->k < BPF_MEMWORDS;
    case BPF_ALU | BPF_DIV | BPF_K:
        return f->k != 0;
    case BPF_ALU | BPF_LSH | BPF_K:
    case BPF_ALU | BPF_RSH | BPF_K:
        return f->k < 32;
    case BPF_ALU | BPF_ADD | BPF_K: case BPF_ALU | BPF_ADD | BPF_X:
    case BPF_ALU | BPF_SUB | BPF_K: case BPF_ALU | BPF_SUB | BPF_X:
    case BPF_ALU | BPF_MUL | BPF_K: case BPF_ALU | BPF_MUL | BPF_X:
    case BPF_ALU | BPF_DIV | BPF_X:
    case BPF_ALU | BPF_AND | BPF_K: case BPF_ALU | BPF_AND | BPF_X:
    case BPF_ALU | BPF_OR  | BPF_K: case BPF_ALU | BPF_OR  | BPF_X:
    case BPF_ALU | BPF_XOR | BPF_K: case BPF_ALU | BPF_XOR | BPF_X:
    case BPF_ALU | BPF_LSH | BPF_X:
    case BPF_ALU | BPF_RSH | BPF_X:
    case BPF_JMP | BPF_JEQ | BPF_K:  case BPF_JMP | BPF_JEQ | BPF_X:
    case BPF_JMP | BPF_JGT | BPF_K:  case BPF_JMP | BPF_JGT | BPF_X:
    case BPF_JMP | BPF_JGE | BPF_K:  case BPF_JMP | BPF_JGE | BPF_X:
    case BPF_JMP | BPF_JSET | BPF_K: case BPF_JMP | BPF_JSET | BPF_X:
        return 1;
    default:
        return 0;
    }
}

static int is_cond_jump(uint16_t code) {
    return BPF_CLASS(code) == BPF_JMP && (code & 0xf0) != BPF_JA;
}

/* 0 when prog[0..len) is an acceptable seccomp program, -EINVAL otherwise. */
static int seccomp_validate(const struct sock_filter *prog, uint32_t len) {
    if (len == 0 || len > SECCOMP_MAX_INSNS) return -EINVAL;
    for (uint32_t pc = 0; pc < len; pc++) {
        const struct sock_filter *f = &prog[pc];
        if (!insn_allowed(f)) return -EINVAL;
        uint32_t left = len - pc - 1;         /* instructions after this one */
        if (f->code == (BPF_JMP | BPF_JA)) {
            if (f->k >= left) return -EINVAL;
        } else if (is_cond_jump(f->code)) {
            if (f->jt >= left || f->jf >= left) return -EINVAL;
        }
    }
    if (BPF_CLASS(prog[len - 1].code) != BPF_RET) return -EINVAL;

    /* Every read of a scratch cell must follow a write to it on every path
     * that reaches it.  Jumps only go forward, so one pass in program order
     * sees all predecessors of an instruction before the instruction itself:
     * known[pc] is the set of cells written on EVERY path into pc. */
    uint16_t *known = kmalloc(len * sizeof(uint16_t));
    if (!known) return -ENOMEM;
    for (uint32_t i = 0; i < len; i++) known[i] = 0xffff;
    known[0] = 0;
    int rc = 0;
    for (uint32_t pc = 0; pc < len; pc++) {
        const struct sock_filter *f = &prog[pc];
        uint16_t have = known[pc];
        switch (f->code) {
        case BPF_ST: case BPF_STX:
            have |= (uint16_t)(1u << f->k);
            break;
        case BPF_LD | BPF_MEM: case BPF_LDX | BPF_MEM:
            if (!(have & (1u << f->k))) { rc = -EINVAL; goto out; }
            break;
        }
        if (BPF_CLASS(f->code) == BPF_RET) continue;     /* no successor */
        if (f->code == (BPF_JMP | BPF_JA)) {
            known[pc + 1 + f->k] &= have;
        } else if (is_cond_jump(f->code)) {
            known[pc + 1 + f->jt] &= have;
            known[pc + 1 + f->jf] &= have;
        } else {
            known[pc + 1] &= have;           /* pc < len - 1: last is a RET */
        }
    }
out:
    kfree(known);
    return rc;
}

/* ── evaluation ──────────────────────────────────────────────────────────── */

/* One run of a validated program.  Every step moves pc forward, so this ends
 * within f->len steps. */
static uint32_t seccomp_run(const struct seccomp_filter *f, const uint32_t *data) {
    uint32_t A = 0, X = 0, M[BPF_MEMWORDS];
    for (int i = 0; i < BPF_MEMWORDS; i++) M[i] = 0;
    uint32_t pc = 0;
    while (pc < f->len) {
        const struct sock_filter *in = &f->insns[pc++];
        uint32_t k = in->k;
        switch (in->code) {
        case BPF_LD | BPF_W | BPF_ABS:  A = data[k >> 2];        break;
        case BPF_LD | BPF_W | BPF_LEN:  A = SECCOMP_DATA_SIZE;   break;
        case BPF_LDX | BPF_W | BPF_LEN: X = SECCOMP_DATA_SIZE;   break;
        case BPF_LD | BPF_IMM:          A = k;                   break;
        case BPF_LDX | BPF_IMM:         X = k;                   break;
        case BPF_LD | BPF_MEM:          A = M[k];                break;
        case BPF_LDX | BPF_MEM:         X = M[k];                break;
        case BPF_ST:                    M[k] = A;                break;
        case BPF_STX:                   M[k] = X;                break;
        case BPF_MISC | BPF_TAX:        X = A;                   break;
        case BPF_MISC | BPF_TXA:        A = X;                   break;
        case BPF_ALU | BPF_ADD | BPF_K: A += k;                  break;
        case BPF_ALU | BPF_ADD | BPF_X: A += X;                  break;
        case BPF_ALU | BPF_SUB | BPF_K: A -= k;                  break;
        case BPF_ALU | BPF_SUB | BPF_X: A -= X;                  break;
        case BPF_ALU | BPF_MUL | BPF_K: A *= k;                  break;
        case BPF_ALU | BPF_MUL | BPF_X: A *= X;                  break;
        case BPF_ALU | BPF_DIV | BPF_K: A /= k;                  break;  /* k != 0 */
        case BPF_ALU | BPF_DIV | BPF_X:
            if (X == 0) return SECCOMP_RET_KILL_THREAD;   /* classic BPF: return 0 */
            A /= X;
            break;
        case BPF_ALU | BPF_AND | BPF_K: A &= k;                  break;
        case BPF_ALU | BPF_AND | BPF_X: A &= X;                  break;
        case BPF_ALU | BPF_OR  | BPF_K: A |= k;                  break;
        case BPF_ALU | BPF_OR  | BPF_X: A |= X;                  break;
        case BPF_ALU | BPF_XOR | BPF_K: A ^= k;                  break;
        case BPF_ALU | BPF_XOR | BPF_X: A ^= X;                  break;
        case BPF_ALU | BPF_LSH | BPF_K: A <<= k;                 break;  /* k < 32 */
        case BPF_ALU | BPF_LSH | BPF_X: A <<= (X & 31);          break;
        case BPF_ALU | BPF_RSH | BPF_K: A >>= k;                 break;
        case BPF_ALU | BPF_RSH | BPF_X: A >>= (X & 31);          break;
        case BPF_ALU | BPF_NEG:         A = (uint32_t)-(int32_t)A; break;
        case BPF_JMP | BPF_JA:          pc += k;                 break;
        case BPF_JMP | BPF_JEQ | BPF_K:  pc += (A == k)       ? in->jt : in->jf; break;
        case BPF_JMP | BPF_JEQ | BPF_X:  pc += (A == X)       ? in->jt : in->jf; break;
        case BPF_JMP | BPF_JGT | BPF_K:  pc += (A >  k)       ? in->jt : in->jf; break;
        case BPF_JMP | BPF_JGT | BPF_X:  pc += (A >  X)       ? in->jt : in->jf; break;
        case BPF_JMP | BPF_JGE | BPF_K:  pc += (A >= k)       ? in->jt : in->jf; break;
        case BPF_JMP | BPF_JGE | BPF_X:  pc += (A >= X)       ? in->jt : in->jf; break;
        case BPF_JMP | BPF_JSET | BPF_K: pc += (A & k)        ? in->jt : in->jf; break;
        case BPF_JMP | BPF_JSET | BPF_X: pc += (A & X)        ? in->jt : in->jf; break;
        case BPF_RET | BPF_K:           return k;
        case BPF_RET | BPF_A:           return A;
        default:                        return SECCOMP_RET_KILL_THREAD;  /* unreachable */
        }
    }
    return SECCOMP_RET_KILL_THREAD;                       /* unreachable */
}

/* Precedence of a return value: the action part read as a signed number, so
 * KILL_PROCESS (0x80000000) beats everything and ALLOW comes last. */
static int32_t action_rank(uint32_t ret) {
    return (int32_t)(ret & SECCOMP_RET_ACTION_FULL);
}

/* ── filter chains ───────────────────────────────────────────────────────── */

static void filter_put(struct seccomp_filter *f) {
    while (f && --f->refcount == 0) {
        struct seccomp_filter *prev = f->prev;
        kfree(f);
        f = prev;
    }
}

int seccomp_filter_count(const struct proc *p) {
    int n = 0;
    for (const struct seccomp_filter *f = p->seccomp_filter; f; f = f->prev) n++;
    return n;
}

void seccomp_fork(struct proc *child, struct proc *parent) {
    child->no_new_privs   = parent->no_new_privs;
    child->seccomp_mode   = parent->seccomp_mode;
    child->seccomp_filter = parent->seccomp_filter;
    if (child->seccomp_filter) child->seccomp_filter->refcount++;
}

void seccomp_release(struct proc *p) {
    struct seccomp_filter *f = p->seccomp_filter;
    p->seccomp_filter = NULL;
    filter_put(f);
}

/* Is `anc` (possibly NULL) on the chain that starts at `f`? */
static int is_ancestor(const struct seccomp_filter *anc,
                       const struct seccomp_filter *f) {
    if (!anc) return 1;
    for (; f; f = f->prev)
        if (f == anc) return 1;
    return 0;
}

static int live_threads(const struct proc *me) {
    int n = 0;
    for (int i = 0; i < MAX_PROCS; i++) {
        const struct proc *q = &ptable[i];
        if (q->state == PROC_UNUSED || q->state == PROC_ZOMBIE) continue;
        if (q->tgid == me->tgid) n++;
    }
    return n;
}

/* ── installing ──────────────────────────────────────────────────────────── */

static int set_mode_strict(void) {
    struct proc *p = current_proc;
    if (p->seccomp_mode != SECCOMP_MODE_DISABLED &&
        p->seccomp_mode != SECCOMP_MODE_STRICT)
        return -EINVAL;
    p->seccomp_mode = SECCOMP_MODE_STRICT;
    return 0;
}

static int set_mode_filter(uint32_t flags, uint32_t uprog) {
    struct proc *p = current_proc;
    const uint32_t known = SECCOMP_FILTER_FLAG_TSYNC | SECCOMP_FILTER_FLAG_LOG |
                           SECCOMP_FILTER_FLAG_SPEC_ALLOW |
                           SECCOMP_FILTER_FLAG_TSYNC_ESRCH;
    /* No user-space notifier (SECCOMP_FILTER_FLAG_NEW_LISTENER): refused like
     * every other flag this kernel does not know. */
    if (flags & ~known) return -EINVAL;

    struct { uint16_t len; uint16_t pad; uint32_t filter; } fprog;  /* sock_fprog */
    if (!uprog || copy_from_user(&fprog, (void *)(uintptr_t)uprog, sizeof(fprog)) < 0)
        return -EFAULT;
    if (fprog.len == 0 || fprog.len > SECCOMP_MAX_INSNS) return -EINVAL;
    /* Without no_new_privs a filter could make a set-uid program misbehave
     * (fail a setuid() it relies on, say); only root may skip it. */
    if (!p->no_new_privs && p->euid != 0) return -EACCES;

    uint32_t bytes = (uint32_t)fprog.len * sizeof(struct sock_filter);
    struct seccomp_filter *f = kmalloc(sizeof(*f) + bytes);
    if (!f) return -ENOMEM;
    if (copy_from_user(f->insns, (void *)(uintptr_t)fprog.filter, bytes) < 0) {
        kfree(f);
        return -EFAULT;
    }
    int rc = seccomp_validate(f->insns, fprog.len);
    if (rc < 0) { kfree(f); return rc; }
    f->refcount = 1;
    f->len      = fprog.len;
    f->log      = (flags & SECCOMP_FILTER_FLAG_LOG) ? 1 : 0;
    f->prev     = NULL;

    if (p->seccomp_mode != SECCOMP_MODE_DISABLED &&
        p->seccomp_mode != SECCOMP_MODE_FILTER) {
        kfree(f);
        return -EINVAL;
    }

    /* Linux charges each filter on the path its length plus 4. */
    uint32_t total = f->len;
    for (struct seccomp_filter *w = p->seccomp_filter; w; w = w->prev)
        total += w->len + 4;
    if (total > SECCOMP_MAX_PATH_INSNS) { kfree(f); return -ENOMEM; }

    /* TSYNC: every other thread must be unfiltered or hold an ancestor of
     * our chain, so that moving it onto our chain only ever adds filters. */
    if (flags & SECCOMP_FILTER_FLAG_TSYNC) {
        for (int i = 0; i < MAX_PROCS; i++) {
            struct proc *q = &ptable[i];
            if (q == p || q->tgid != p->tgid) continue;
            if (q->state == PROC_UNUSED || q->state == PROC_ZOMBIE) continue;
            if (q->seccomp_mode == SECCOMP_MODE_DISABLED) continue;
            if (q->seccomp_mode == SECCOMP_MODE_FILTER &&
                is_ancestor(q->seccomp_filter, p->seccomp_filter)) continue;
            kfree(f);
            return (flags & SECCOMP_FILTER_FLAG_TSYNC_ESRCH) ? -ESRCH : q->pid;
        }
    }

    f->prev = p->seccomp_filter;          /* our reference moves to the new head */
    p->seccomp_filter = f;
    p->seccomp_mode   = SECCOMP_MODE_FILTER;

    if (flags & SECCOMP_FILTER_FLAG_TSYNC) {
        for (int i = 0; i < MAX_PROCS; i++) {
            struct proc *q = &ptable[i];
            if (q == p || q->tgid != p->tgid) continue;
            if (q->state == PROC_UNUSED || q->state == PROC_ZOMBIE) continue;
            struct seccomp_filter *old = q->seccomp_filter;
            f->refcount++;
            q->seccomp_filter = f;
            filter_put(old);
            if (p->no_new_privs) q->no_new_privs = 1;
            q->seccomp_mode = SECCOMP_MODE_FILTER;
        }
    }
    return 0;
}

int sys_seccomp(uint32_t op, uint32_t flags, uint32_t uargs) {
    switch (op) {
    case SECCOMP_SET_MODE_STRICT:
        if (flags != 0 || uargs != 0) return -EINVAL;
        return set_mode_strict();
    case SECCOMP_SET_MODE_FILTER:
        return set_mode_filter(flags, uargs);
    case SECCOMP_GET_ACTION_AVAIL: {
        uint32_t action;
        if (flags != 0) return -EINVAL;
        if (!uargs || copy_from_user(&action, (void *)(uintptr_t)uargs, 4) < 0)
            return -EFAULT;
        switch (action) {
        case SECCOMP_RET_KILL_PROCESS: case SECCOMP_RET_KILL_THREAD:
        case SECCOMP_RET_TRAP:         case SECCOMP_RET_ERRNO:
        case SECCOMP_RET_TRACE:        case SECCOMP_RET_LOG:
        case SECCOMP_RET_ALLOW:
            return 0;
        default:
            return -EOPNOTSUPP;
        }
    }
    default:
        return -EINVAL;
    }
}

int seccomp_prctl_set(uint32_t mode, uint32_t uprog) {
    switch (mode) {
    case SECCOMP_MODE_STRICT: return set_mode_strict();
    case SECCOMP_MODE_FILTER: return set_mode_filter(0, uprog);
    default:                  return -EINVAL;
    }
}

/* ── the syscall hook ────────────────────────────────────────────────────── */

static const char *action_name(uint32_t action) {
    switch (action) {
    case SECCOMP_RET_KILL_PROCESS: return "kill_process";
    case SECCOMP_RET_KILL_THREAD:  return "kill_thread";
    case SECCOMP_RET_TRAP:         return "trap";
    case SECCOMP_RET_ERRNO:        return "errno";
    case SECCOMP_RET_USER_NOTIF:   return "user_notif";
    case SECCOMP_RET_TRACE:        return "trace";
    case SECCOMP_RET_LOG:          return "log";
    case SECCOMP_RET_ALLOW:        return "allow";
    default:                       return "unknown";
    }
}

static void seccomp_log(uint32_t nr, uint32_t action, uint32_t eip) {
    static int budget = 64;            /* a LOG filter may fire on every call */
    if (action == SECCOMP_RET_LOG && budget <= 0) return;
    if (action == SECCOMP_RET_LOG) budget--;
    printk("[SECCOMP] pid=%d comm=%s syscall=%u ip=%08x action=%s\n",
           current_proc->pid, current_proc->name, (unsigned)nr,
           (unsigned)eip, action_name(action));
}

/* The thread dies of `sig`; when it is the last live thread the whole process
 * does, with that signal as its wait status. */
static void __attribute__((noreturn)) kill_thread(int sig) {
    if (live_threads(current_proc) <= 1) proc_group_exit(sig);
    proc_exit(sig);
    for (;;) ;
}

int seccomp_syscall_enter(registers_t *regs, uint32_t nr) {
    struct proc *p = current_proc;

    if (p->seccomp_mode == SECCOMP_MODE_STRICT) {
        /* read, write, exit, sigreturn — the i386 numbers. */
        if (nr == 3 || nr == 4 || nr == 1 || nr == 119) return 0;
        seccomp_log(nr, SECCOMP_RET_KILL_THREAD, regs->eip);
        kill_thread(SIGKILL);
    }

    uint32_t data[SECCOMP_DATA_SIZE / 4];
    data[0] = nr;
    data[1] = AUDIT_ARCH_I386;
    data[2] = regs->eip;                  /* the instruction after int 0x80 */
    data[3] = 0;
    const uint32_t args[6] = { regs->ebx, regs->ecx, regs->edx,
                               regs->esi, regs->edi, regs->ebp };
    for (int i = 0; i < 6; i++) {
        data[4 + 2 * i] = args[i];
        data[5 + 2 * i] = 0;
    }

    /* Every filter on the chain runs; the most restrictive answer wins, and
     * between equals the newest filter's (it ran first). */
    uint32_t ret = SECCOMP_RET_ALLOW;
    const struct seccomp_filter *match = NULL;
    for (const struct seccomp_filter *f = p->seccomp_filter; f; f = f->prev) {
        uint32_t r = seccomp_run(f, data);
        if (action_rank(r) < action_rank(ret)) { ret = r; match = f; }
    }
    uint32_t action = ret & SECCOMP_RET_ACTION_FULL;
    uint32_t rdata  = ret & SECCOMP_RET_DATA;

    switch (action) {
    case SECCOMP_RET_ALLOW:
        return 0;
    case SECCOMP_RET_LOG:
        seccomp_log(nr, action, regs->eip);
        return 0;
    case SECCOMP_RET_ERRNO:
        if (match && match->log) seccomp_log(nr, action, regs->eip);
        regs->eax = (uint32_t)-(int32_t)(rdata > MAX_ERRNO ? MAX_ERRNO : rdata);
        return 1;
    case SECCOMP_RET_TRAP:
        if (match && match->log) seccomp_log(nr, action, regs->eip);
        /* The registers stay as the call left them (eax = the number), which
         * is what the handler finds in its ucontext; it writes the result
         * there and sigreturn hands it back. */
        regs->eax = nr;
        signal_force_sigsys(p, (int)nr, regs->eip, (int)rdata);
        return 1;
    case SECCOMP_RET_TRACE:
    case SECCOMP_RET_USER_NOTIF:
        /* No tracer, no listener: the call fails as Linux fails it then. */
        regs->eax = (uint32_t)-ENOSYS;
        return 1;
    case SECCOMP_RET_KILL_THREAD:
        seccomp_log(nr, action, regs->eip);
        kill_thread(SIGSYS);
    case SECCOMP_RET_KILL_PROCESS:
    default:
        seccomp_log(nr, SECCOMP_RET_KILL_PROCESS, regs->eip);
        proc_group_exit(SIGSYS);
    }
}
