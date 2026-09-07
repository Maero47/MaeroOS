/*
 * P19 siginfo - tests S6.
 *
 * Linux (arch/x86/kernel/signal.c, arch/x86/mm/fault.c, kernel/signal.c):
 *  - a SIGSEGV from a write to unmapped 0x1234 carries si_addr == 0x1234
 *    and si_code == SEGV_MAPERR;
 *  - sa_mask is added to the blocked set while the handler runs and the
 *    signal itself is blocked too (no SA_NODEFER); the ucontext's uc_sigmask
 *    is the mask from before the handler; the old mask is restored after;
 *  - with SA_ONSTACK the handler runs on the sigaltstack() stack.
 *
 * MaeroOS (audit): sa_mask ignored, signal not blocked in its own handler,
 * uc_sigmask zero, si_code 0 and no si_addr (proc/syscall.c:3477-3483,
 * proc/signal.c:82-290, :233-237); sigaltstack is a stub (:4526-4529).
 */
#define PROBE_NAME "p19_siginfo"
#include "probe.h"
#include <setjmp.h>
#include <ucontext.h>

static sigjmp_buf jb;
static volatile void *seen_addr;
static volatile int seen_code, seen_signo;

static void on_segv(int sig, siginfo_t *si, void *uc)
{
    (void)uc;
    seen_signo = sig;
    seen_addr = si->si_addr;
    seen_code = si->si_code;
    siglongjmp(jb, 1);
}

static volatile int in_mask_usr1, in_mask_usr2, uc_has_usr1, uc_has_usr2, uc_has_alrm;
static void on_usr1(int sig, siginfo_t *si, void *ucv)
{
    (void)sig; (void)si;
    sigset_t cur;
    sigprocmask(SIG_BLOCK, NULL, &cur);
    in_mask_usr1 = sigismember(&cur, SIGUSR1);
    in_mask_usr2 = sigismember(&cur, SIGUSR2);
    ucontext_t *uc = ucv;
    uc_has_usr1 = sigismember(&uc->uc_sigmask, SIGUSR1);
    uc_has_usr2 = sigismember(&uc->uc_sigmask, SIGUSR2);
    uc_has_alrm = sigismember(&uc->uc_sigmask, SIGALRM);
}

static char altstack[65536];
static volatile int on_alt = -1;
static void on_usr2(int sig)
{
    (void)sig;
    volatile char probe_local = 0;
    uintptr_t sp = (uintptr_t)&probe_local;
    on_alt = sp >= (uintptr_t)altstack && sp < (uintptr_t)altstack + sizeof altstack;
}

int main(void)
{
    probe_watchdog(60);
    struct sigaction sa;

    /* si_addr / si_code */
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    volatile int *bad = (volatile int *)(uintptr_t)0x1234;
    if (sigsetjmp(jb, 1) == 0) {
        *bad = 1;
        probe_fail("write to 0x1234 did not fault");
    }
    probe_info("SIGSEGV: signo %d si_addr %p si_code %d", seen_signo, (void *)seen_addr, seen_code);
    if (seen_signo != SIGSEGV)
        probe_fail("fault delivered signal %d, expected SIGSEGV", seen_signo);
    if (seen_addr != (void *)(uintptr_t)0x1234)
        probe_fail("si_addr is %p, expected 0x1234", (void *)seen_addr);
    if (seen_code != SEGV_MAPERR)
        probe_fail("si_code is %d, expected SEGV_MAPERR (%d)", seen_code, SEGV_MAPERR);

    /* sa_mask, self-blocking, uc_sigmask */
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_usr1;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaddset(&sa.sa_mask, SIGUSR2);
    sigaction(SIGUSR1, &sa, NULL);
    sigset_t pre, post;
    sigemptyset(&pre);
    sigaddset(&pre, SIGALRM);
    sigprocmask(SIG_SETMASK, &pre, NULL);
    raise(SIGUSR1);
    sigprocmask(SIG_BLOCK, NULL, &post);
    probe_info("in handler: blocked USR1=%d USR2=%d; uc_sigmask USR1=%d USR2=%d ALRM=%d",
               in_mask_usr1, in_mask_usr2, uc_has_usr1, uc_has_usr2, uc_has_alrm);
    if (!in_mask_usr1)
        probe_fail("the signal being handled (SIGUSR1) was not blocked inside its handler");
    if (!in_mask_usr2)
        probe_fail("sa_mask (SIGUSR2) was not applied inside the handler");
    if (uc_has_usr1 || uc_has_usr2 || !uc_has_alrm)
        probe_fail("uc_sigmask does not hold the pre-handler mask (USR1=%d USR2=%d ALRM=%d)",
                   uc_has_usr1, uc_has_usr2, uc_has_alrm);
    if (sigismember(&post, SIGUSR2) || sigismember(&post, SIGUSR1) || !sigismember(&post, SIGALRM))
        probe_fail("signal mask not restored after the handler returned");
    sigemptyset(&pre);
    sigprocmask(SIG_SETMASK, &pre, NULL);

    /* sigaltstack */
    stack_t ss;
    ss.ss_sp = altstack;
    ss.ss_size = sizeof altstack;
    ss.ss_flags = 0;
    if (sigaltstack(&ss, NULL) != 0)
        probe_fail("sigaltstack: %s", strerror(errno));
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_usr2;
    sa.sa_flags = SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR2, &sa, NULL);
    raise(SIGUSR2);
    if (on_alt != 1)
        probe_fail("SA_ONSTACK handler did not run on the sigaltstack (%d)", on_alt);
    probe_info("SA_ONSTACK handler ran on the alternate stack");

    probe_pass();
}
