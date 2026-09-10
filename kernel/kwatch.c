#include <kernel/kwatch.h>
#include <kernel/kprof.h>
#include <kernel/config.h>
#include "printk.h"
#include "../proc/process.h"
#include "../proc/scheduler.h"
#include "../proc/usocket.h"
#include "../proc/pipe.h"
#include "../fs/vfs.h"
#include "../arch/i686/cpu/pit.h"
#include "../arch/i686/cpu/percpu.h"
#include <registers.h>

/* See include/kernel/kwatch.h for what this is for. */

#define TICKS_PER_SEC   100u
#define STALL_TICKS     (KWATCH_STALL_SEC * TICKS_PER_SEC)

#define SYS_SCHED_YIELD_A  158
#define SYS_SCHED_YIELD_B  159

/* Syscalls that are not progress.  Both respawnprobe services run
 * `while (1) sched_yield()`, which keeps the raw syscall counter climbing at
 * roughly 800k/s no matter how dead everything else is; counting those as
 * progress would make this watchdog unable to fire on the one workload it
 * exists for. */
static uint32_t progress_count(void) {
    return (uint32_t)kprof_ev[KPE_SYSCALL]
         - kprof_syscall_count(SYS_SCHED_YIELD_A)
         - kprof_syscall_count(SYS_SCHED_YIELD_B);
}

static uint32_t g_last_sysc;      /* syscall counter at the last movement  */
static uint32_t g_last_ctxsw;
static uint32_t g_last_move;      /* pit tick when it last moved           */
static uint32_t g_stalls;         /* dumps emitted so far                  */
static int      g_armed;          /* only watch once userspace is running  */
static const char *volatile g_pending;  /* tag of a stall the tick detected */

static const char *state_name(proc_state_t s) {
    switch (s) {
    case PROC_UNUSED:   return "unused";
    case PROC_EMBRYO:   return "embryo";
    case PROC_RUNNABLE: return "RUNNABLE";
    case PROC_RUNNING:  return "RUNNING";
    case PROC_SLEEPING: return "sleep";
    case PROC_STOPPED:  return "stopped";
    case PROC_ZOMBIE:   return "zombie";
    }
    return "?";
}

/* The blocking syscalls, by number — everything else prints as a bare number.
 * A wedge is always somebody sitting in one of these. */
static const char *sys_name(int nr) {
    switch (nr) {
    case 1:   return "exit";
    case 3:   return "read";
    case 4:   return "write";
    case 7:   return "waitpid";
    case 11:  return "exec";
    case 29:  return "pause";
    case 42:  return "pipe";
    case 114: return "wait4";
    case 142: return "select";
    case 145: return "readv";
    case 146: return "writev";
    case 162: return "nanosleep";
    case 168: return "poll";
    case 179: return "sigsuspend";
    case 240: return "futex";
    case 256: return "epoll_wait";
    case 308: return "pselect6";
    case 309: return "ppoll";
    case 319: return "epoll_pwait";
    case 359: return "socket";
    case 362: return "connect";
    case 364: return "accept4";
    case 369: return "sendto";
    case 370: return "sendmsg";
    case 371: return "recvfrom";
    case 372: return "recvmsg";
    case 407: return "clock_nanosleep64";
    case 422: return "futex_time64";
    case 414: return "ppoll_time64";
    default:  return (const char *)0;
    }
}

/*
 * Decode a sleep channel into something a human can act on.  Every wait
 * channel in this kernel is either the global poll channel, a user address (a
 * futex), or the address of a kernel object that some process holds an fd on —
 * so a scan of every live fd table names it.  Prints nothing when the channel
 * is anonymous; the raw pointer is always printed by the caller.
 */
static void describe_chan(void *chan, struct proc *waiter) {
    if (!chan) return;
    if (chan == (void *)&io_activity) { printk(" poll/io"); return; }
    if (waiter->futex_wait) {
        printk(" futex%s", waiter->futex_shared ? "(shared)" : "");
        return;
    }
    for (int i = 0; i < MAX_PROCS; i++) {
        struct proc *o = &ptable[i];
        if (o->state == PROC_UNUSED || !o->ofile) continue;
        for (int fd = 0; fd < MAX_FD; fd++) {
            proc_file_t *f = &o->ofile[fd];
            switch (f->type) {
            case FD_PIPE_R:
            case FD_PIPE_W:
                if ((void *)f->pipe == chan) {
                    printk(" pipe(fd %d of pid %d, %u/%u bytes, r=%d w=%d)",
                           fd, o->pid, (unsigned)f->pipe->count,
                           (unsigned)PIPE_BUF_SIZE, f->pipe->nreaders,
                           f->pipe->nwriters);
                    return;
                }
                break;
            case FD_USOCKET: {
                if (!f->usock) break;
                void *rx = usocket_rx_id(f->usock), *tx = usocket_tx_id(f->usock);
                if (chan != (void *)f->usock && chan != rx && chan != tx) break;
                uint32_t rxn = 0, txn = 0, cap = 0; int gone = 0;
                usocket_stat(f->usock, &rxn, &txn, &cap, &gone);
                printk(" unix(fd %d of pid %d%s%s, %s, rx=%u tx=%u cap=%u%s)",
                       fd, o->pid,
                       usocket_path(f->usock)[0] ? " " : "",
                       usocket_path(f->usock),
                       chan == rx ? "waiting on rx" :
                       chan == tx ? "waiting on tx" : "listen",
                       (unsigned)rxn, (unsigned)txn, (unsigned)cap,
                       gone ? " PEER-GONE" : "");
                return;
            }
            case FD_FILE:
                /* Either the node itself or the object behind it — a PTY pair
                 * is the wait channel for both ends of a terminal, and the
                 * desktop's shell lives on one. */
                if ((void *)f->node == chan ||
                    (f->node && f->node->private == chan)) {
                    printk(" file(fd %d of pid %d %s)", fd, o->pid, f->path);
                    return;
                }
                break;
            default:
                break;
            }
        }
    }
}

void kwatch_dump(const char *tag) {
    unsigned u = 0, idl = 0, kern = 0;
    kprof_window_ms(&u, &idl, &kern);

    printk("[kwatch] --- %s #%u t=%u.%02us  sysc=%u (yield=%u) ctxsw=%u wake=%u\n",
           tag, (unsigned)++g_stalls,
           (unsigned)(pit_ticks() / 100u), (unsigned)(pit_ticks() % 100u),
           (unsigned)kprof_ev[KPE_SYSCALL],
           (unsigned)(kprof_syscall_count(SYS_SCHED_YIELD_A) +
                      kprof_syscall_count(SYS_SCHED_YIELD_B)),
           (unsigned)kprof_ev[KPE_CTXSW], (unsigned)kprof_ev[KPE_WAKE]);
    {
        int locked = 0, depth = 0;
        bkl_state(&locked, &depth);
        printk("[kwatch] bkl: locked=%d depth=%d%s\n", locked, depth,
               (locked && depth == 0)
                   ? "  <- held with nothing holding it: self-deadlock" : "");
    }
    printk("[kwatch] window: user=%ums idle=%ums kernel=%ums -> %s\n",
           u, idl, kern,
           idl > u + kern ? "ASLEEP (nothing runnable: a wake-up was missed)"
                          : "BUSY (something is looping)");

    uint32_t now = pit_ticks();
    for (int i = 0; i < MAX_PROCS; i++) {
        struct proc *p = &ptable[i];
        if (p->state == PROC_UNUSED) continue;
        const char *sn = sys_name(p->last_syscall);
        printk("[kwatch]  pid=%d tgid=%d %-12s %-8s sys=", p->pid, p->tgid,
               p->name[0] ? p->name : "?", state_name(p->state));
        if (sn) printk("%s", sn); else printk("%d", p->last_syscall);
        if (p->state == PROC_SLEEPING) {
            printk(" chan=%x for %u.%02us", (unsigned)(uintptr_t)p->sleep_chan,
                   (unsigned)((now - p->sleep_tick) / 100u),
                   (unsigned)((now - p->sleep_tick) % 100u));
            if (p->wake_tick)
                printk(" deadline=+%dt", (int)(p->wake_tick - now));
            describe_chan(p->sleep_chan, p);
        }
        if (p->tf)
            printk(" eip=%x esp=%x", (unsigned)p->tf->eip, (unsigned)p->tf->useresp);
        if (p->no_preempt) printk(" nopreempt=%d", p->no_preempt);
        if (p->pending_sigs)
            printk(" sig=%x/blk=%x", (unsigned)p->pending_sigs,
                   (unsigned)p->blocked_sigs);
        printk(" run=%u sched=%u\n", (unsigned)p->utime_ticks,
               (unsigned)p->sched_count);
    }
    printk("[kwatch] --- end #%u\n", (unsigned)g_stalls);
}

/*
 * NMI (vector 2) — the way into a guest that has interrupts off.
 *
 * int 0x80 is an interrupt gate, so a syscall runs with IF=0 from entry to
 * exit: a kernel path that spins instead of sleeping takes the timer tick away
 * with it, and no watchdog driven by that tick can ever fire.  An NMI is
 * delivered regardless of IF, so `nmi` from the QEMU monitor gets the same
 * state dump out of a hung guest, plus where the CPU actually was.  Registered
 * Reached through its own stub (nmi_isr in isr.asm), which deliberately does
 * not take the Big Kernel Lock: the hang this exists to photograph is one where
 * that lock is held and cannot be let go, so taking it here would deadlock
 * against the very state being looked at.
 */
void nmi_handler(registers_t *regs) {
    printk("[kwatch] NMI: cs=%x eip=%x eflags=%x (IF=%d)\n",
           (unsigned)regs->cs, (unsigned)regs->eip, (unsigned)regs->eflags,
           (regs->eflags & 0x200) ? 1 : 0);
    kwatch_dump("NMI");
}

void kwatch_tick(void) {
    uint32_t now  = pit_ticks();
    uint32_t sysc = progress_count();
    uint32_t ctx  = (uint32_t)kprof_ev[KPE_CTXSW];

    /* Arm only once userspace has started issuing syscalls: before that the
     * counter is legitimately flat while the kernel boots. */
    if (!g_armed) {
        if (!sysc) return;
        g_armed = 1;
        g_last_sysc = sysc; g_last_ctxsw = ctx; g_last_move = now;
        return;
    }
    if (sysc != g_last_sysc) {
        g_last_sysc = sysc; g_last_ctxsw = ctx; g_last_move = now;
        return;
    }
    if ((uint32_t)(now - g_last_move) < STALL_TICKS) return;

    /* No syscall has been entered for the whole window.  Whether anything ran
     * at all in it separates a total wedge from one spinning thread. */
    g_pending    = (ctx != g_last_ctxsw) ? "STALL (spinning)"
                                         : "STALL (nothing ran)";
    g_last_ctxsw = ctx;
    g_last_move  = now;
}

/*
 * Emit a stall the tick detected.  Called from the scheduler loop rather than
 * from the tick itself so the dump's printk runs with interrupts enabled and
 * on the scheduler's own stack — the same reason kprof_tick() lives in the
 * syscall path.  The scheduler loop is reached on every quantum and on every
 * pass through the idle halt, so a wedge of either kind still gets here.
 */
void kwatch_poll(void) {
    const char *tag = g_pending;
    if (!tag) return;
    g_pending = (const char *)0;
    kwatch_dump(tag);
}
