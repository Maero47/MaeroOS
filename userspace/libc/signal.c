/* Signal APIs on top of the kernel's rt_sig* calls.
 *
 * The user sigset_t is one word (signal n at bit n-1, signals 1..32); the
 * kernel takes an 8-byte set, so every call goes through a local two-word
 * buffer and passes sigsetsize = 8.  Keeping the kernel's view in our own
 * buffers also means nothing the caller passed is written past what the
 * caller's type holds, however many bytes the kernel copies.
 *
 * No SA_RESTORER: the kernel writes its own sigreturn trampoline into every
 * frame (proc/signal.c) and ignores sa_restorer, so there is nothing for
 * libc to supply. */
#include "../include/syscall.h"
#include "../include/errno.h"
#include "../include/signal.h"
#include <stdint.h>

#define SYS_KILL           37
#define SYS_RT_SIGACTION   174
#define SYS_RT_SIGPROCMASK 175
#define SYS_RT_SIGPENDING  176
#define SYS_RT_SIGSUSPEND  179
#define SYS_SIGALTSTACK    186
#define SYS_GETTID         224
#define SYS_TKILL          238

#define KSIGSET_SIZE 8

/* i386 kernel_sigaction: handler, flags, restorer, 8-byte mask = 20 bytes.
 * Padded to 32: the kernel has copied a full 32 bytes both ways in the past,
 * and the pad keeps that from reaching anything but this buffer. */
struct k_sigaction {
    void    *handler;
    uint32_t flags;
    void    *restorer;
    uint32_t mask[2];
    uint32_t pad[3];
};

static int sys_ret(int r) {
    if (r < 0) { errno = -r; return -1; }
    return 0;
}

static int sig_valid(int sig) { return sig >= 1 && sig <= NSIG; }

/* ── sigset_t ────────────────────────────────────────────────────────────── */

int sigemptyset(sigset_t *set) { *set = 0; return 0; }
int sigfillset(sigset_t *set)  { *set = ~0UL; return 0; }

int sigaddset(sigset_t *set, int sig) {
    if (!sig_valid(sig)) { errno = EINVAL; return -1; }
    *set |= 1UL << (sig - 1);
    return 0;
}

int sigdelset(sigset_t *set, int sig) {
    if (!sig_valid(sig)) { errno = EINVAL; return -1; }
    *set &= ~(1UL << (sig - 1));
    return 0;
}

int sigismember(const sigset_t *set, int sig) {
    if (!sig_valid(sig)) { errno = EINVAL; return -1; }
    return (*set >> (sig - 1)) & 1;
}

/* ── SA_RESETHAND ────────────────────────────────────────────────────────── */

/* The kernel does not implement SA_RESETHAND, so a handler installed with it
 * is entered through resethand_entry(), which puts the disposition back to
 * SIG_DFL (Linux handle_signal: sa_handler = SIG_DFL, flags and mask kept)
 * before calling it.  The signal is still blocked across the handler unless
 * SA_NODEFER, as the kernel adds it to the mask on entry, so a second one
 * arriving meanwhile stays pending and then takes the default action. */
struct resethand {
    void    *handler;
    int      flags;
    sigset_t mask;
};
static struct resethand resethand[NSIG + 1];

static void resethand_entry(int sig, siginfo_t *si, void *uc) {
    struct resethand r = resethand[sig];
    struct k_sigaction k = { 0 };
    k.handler = SIG_DFL;
    k.flags   = (uint32_t)r.flags & ~(uint32_t)SA_RESETHAND;
    k.mask[0] = r.mask;
    syscall4(SYS_RT_SIGACTION, sig, (int)&k, 0, KSIGSET_SIZE);
    if (r.flags & SA_SIGINFO)
        ((void (*)(int, siginfo_t *, void *))r.handler)(sig, si, uc);
    else
        ((sighandler_t)r.handler)(sig);
}

/* ── sigaction / signal ──────────────────────────────────────────────────── */

int sigaction(int sig, const struct sigaction *act, struct sigaction *oldact) {
    struct k_sigaction kact = { 0 }, koact = { 0 };
    struct resethand prev = { 0 };
    int emulate = 0;

    if (sig >= 1 && sig <= NSIG) prev = resethand[sig];
    if (act) {
        kact.handler = (void *)act->sa_handler;
        kact.flags   = (uint32_t)act->sa_flags;
        kact.mask[0] = act->sa_mask;
        if ((act->sa_flags & SA_RESETHAND) && sig >= 1 && sig <= NSIG &&
            act->sa_handler != SIG_DFL && act->sa_handler != SIG_IGN) {
            resethand[sig].handler = (void *)act->sa_handler;
            resethand[sig].flags   = act->sa_flags;
            resethand[sig].mask    = act->sa_mask;
            kact.handler = (void *)resethand_entry;
            kact.flags  &= ~(uint32_t)SA_RESETHAND;
            emulate = 1;
        }
    }
    int r = syscall4(SYS_RT_SIGACTION, sig, act ? (int)&kact : 0,
                     oldact ? (int)&koact : 0, KSIGSET_SIZE);
    if (r < 0) {
        if (emulate) resethand[sig] = prev;
        return sys_ret(r);
    }
    if (oldact) {
        if (koact.handler == (void *)resethand_entry) {
            oldact->sa_handler = (sighandler_t)prev.handler;
            oldact->sa_flags   = prev.flags;
            oldact->sa_mask    = prev.mask;
        } else {
            oldact->sa_handler = (sighandler_t)koact.handler;
            oldact->sa_flags   = (int)koact.flags;
            oldact->sa_mask    = koact.mask[0];
        }
        oldact->sa_restorer = 0;
    }
    return 0;
}

/* BSD semantics, like musl and glibc: the handler stays installed and
 * interrupted system calls are restarted. */
sighandler_t signal(int sig, sighandler_t handler) {
    struct sigaction sa = { 0 }, old;
    sa.sa_handler = handler;
    sa.sa_flags   = SA_RESTART;
    if (sigaction(sig, &sa, &old) < 0) return SIG_ERR;
    return old.sa_handler;
}

/* ── Masks ───────────────────────────────────────────────────────────────── */

int sigprocmask(int how, const sigset_t *set, sigset_t *oldset) {
    uint32_t kset[2] = { set ? *set : 0, 0 }, kold[2] = { 0, 0 };
    if (set && how != SIG_BLOCK && how != SIG_UNBLOCK && how != SIG_SETMASK) {
        errno = EINVAL;
        return -1;
    }
    int r = syscall4(SYS_RT_SIGPROCMASK, how, set ? (int)kset : 0,
                     oldset ? (int)kold : 0, KSIGSET_SIZE);
    if (r < 0) return sys_ret(r);
    if (oldset) *oldset = kold[0];
    return 0;
}

int pthread_sigmask(int how, const sigset_t *set, sigset_t *oldset) {
    return sigprocmask(how, set, oldset) < 0 ? errno : 0;
}

int sigpending(sigset_t *set) {
    uint32_t k[2] = { 0, 0 };
    int r = syscall2(SYS_RT_SIGPENDING, (int)k, KSIGSET_SIZE);
    if (r < 0) return sys_ret(r);
    *set = k[0];
    return 0;
}

/* Always -1/EINTR: it returns only once a handler has run. */
int sigsuspend(const sigset_t *mask) {
    uint32_t k[2] = { *mask, 0 };
    return sys_ret(syscall2(SYS_RT_SIGSUSPEND, (int)k, KSIGSET_SIZE));
}

int sigaltstack(const stack_t *ss, stack_t *old_ss) {
    return sys_ret(syscall2(SYS_SIGALTSTACK, (int)ss, (int)old_ss));
}

/* ── Sending ─────────────────────────────────────────────────────────────── */

int kill(int pid, int sig) {
    return sys_ret(syscall2(SYS_KILL, pid, sig));
}

/* To the calling thread, not the process (POSIX: raise == pthread_kill of
 * self).  An unblocked signal is delivered on the way out of tkill, i.e.
 * before raise() returns. */
int raise(int sig) {
    return sys_ret(syscall2(SYS_TKILL, syscall0(SYS_GETTID), sig));
}
