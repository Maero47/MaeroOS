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
 *
 * S7 and S8 cover the review of the S6 fix:
 *  S7 sigreturn must take the general-purpose registers, EIP and ESP from the
 *     frame and NOTHING else.  The frame lives on the user stack, so a handler
 *     that writes IOPL=3 and IF=0 into the saved EFLAGS, or a bogus selector
 *     into a saved segment register, must not gain unrestricted port I/O,
 *     must not come back with interrupts disabled, and must not be able to
 *     fault the kernel on its way out (Linux restore_sigcontext keeps IOPL and
 *     IF out of FIX_EFLAGS and forces CS/SS to ring 3);
 *  S8 a signal frame built on the sigaltstack() stack must stay inside it.
 *     Nested SA_ONSTACK deliveries do not switch again — sp is already on the
 *     alternate stack — so each frame lands below the last; the one that would
 *     leave the stack must kill the process (Linux get_sigframe returns -1L →
 *     force_sigsegv) rather than writing below it.
 */
#define PROBE_NAME "p19_siginfo"
#include "probe.h"
#include <setjmp.h>
#include <ucontext.h>
#include <sys/mman.h>
#include <sys/wait.h>

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

/* ── S7: sigreturn must not take privilege state from the frame ─────────── */

static sigjmp_buf iojb;
static volatile int io_faulted;
static void on_io_fault(int sig) { (void)sig; io_faulted = 1; siglongjmp(iojb, 1); }

/* Read one I/O port.  Unprivileged code has IOPL 0 and no port bitmap, so this
 * must raise #GP -> SIGSEGV.  Each use is its own call site (its own faulting
 * EIP), which keeps MaeroOS's same-EIP fault-loop breaker out of the way. */
#define PORT_MUST_FAULT(what)                                                 \
    do {                                                                      \
        io_faulted = 0;                                                       \
        if (sigsetjmp(iojb, 1) == 0) {                                        \
            __asm__ volatile("inb $0x80, %%al" ::: "al");                     \
            probe_fail("S7: %s — the process can read an I/O port, so the "   \
                       "saved EFLAGS raised IOPL", what);                     \
        }                                                                     \
    } while (0)

static uint32_t read_eflags(void)
{
    uint32_t f;
    __asm__ volatile("pushfl; popl %0" : "=r"(f));
    return f;
}

/* Part A, through the documented interface: an SA_SIGINFO handler edits the
 * saved EFLAGS in its own ucontext.  Linux keeps IOPL and IF out of FIX_EFLAGS
 * (arch/x86/kernel/signal.c restore_sigcontext), so this must change nothing. */
static void on_efl(int sig, siginfo_t *si, void *ucv)
{
    (void)sig; (void)si;
    ucontext_t *uc = ucv;
    uc->uc_mcontext.gregs[REG_EFL] |= 0x3000;        /* IOPL = 3 */
    uc->uc_mcontext.gregs[REG_EFL] &= ~(greg_t)0x200; /* IF off */
}

/* Part B, the frame itself: a handler installed WITHOUT SA_SIGINFO gets no
 * ucontext, but on a kernel that puts its sigreturn trampoline on the signal
 * frame (MaeroOS does; Linux points the return at a libc restorer in text) the
 * saved register image sits just below that trampoline and is writable.  Blast
 * every EFLAGS-looking word there with IOPL=3 + IF clear, and every word that
 * looks like a segment selector with one that names the TSS — which would #GP
 * inside the kernel's own `pop %ds` / `iret` if it were ever loaded. */
static volatile int frame_efl_hits, frame_seg_hits;
static void on_plain(int sig)
{
    (void)sig;
    char here;
    uintptr_t ret = (uintptr_t)__builtin_return_address(0);
    uintptr_t me  = (uintptr_t)&here;
    if (ret <= me || ret - me > 4096)
        return;                       /* the return is not on this stack */
    uint32_t *p = (uint32_t *)(ret - 8);
    for (int i = 0; i < 20; i++, p--) {
        uint32_t v = *p;
        if ((v & ~0xFFFu) == 0 && (v & 0x2) && (v & 0x200)) {
            *p = (v | 0x3000u) & ~0x200u;
            frame_efl_hits++;
        } else if (v == 0x23 || v == 0x1B || v == 0x33) {
            *p = 0x2B;                /* the TSS descriptor, RPL 3 */
            frame_seg_hits++;
        }
    }
}

/* ── S8: a frame on the alternate stack must stay on it ─────────────────── */

#define S8_GUARD  8192               /* memory the frames must never reach */
#define S8_ALT    4096               /* deliberately small alternate stack */
#define S8_SLACK  512                /* the deepest handler's own locals may
                                      * legitimately sit just below the stack */
static volatile int nest_depth;
static void on_nest(int sig, siginfo_t *si, void *uc)
{
    (void)si; (void)uc;
    if (++nest_depth < 32)
        syscall(SYS_tgkill, getpid(), raw_gettid(), sig);
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

    /* S7: privilege state must come from the kernel, never from the frame. */
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_io_fault;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    PORT_MUST_FAULT("before any signal");
    if (!io_faulted)
        probe_fail("S7: the baseline port read did not fault");

    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_efl;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);
    raise(SIGUSR1);
    uint32_t fl_a = read_eflags();
    if (!(fl_a & 0x200))
        probe_fail("S7: returned from the handler with interrupts disabled (eflags %08x)",
                   fl_a);
    if (fl_a & 0x3000)
        probe_fail("S7: the handler raised IOPL through uc_mcontext (eflags %08x)", fl_a);
    PORT_MUST_FAULT("after a handler edited uc_mcontext EFLAGS");

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_plain;
    sa.sa_flags = 0;                 /* no SA_SIGINFO: the plain frame */
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);
    raise(SIGUSR1);
    uint32_t fl_b = read_eflags();
    probe_info("S7: frame words rewritten: %d eflags, %d selector%s", frame_efl_hits,
               frame_seg_hits,
               (frame_efl_hits || frame_seg_hits) ? "" : " (frame not reachable here)");
    if (!(fl_b & 0x200))
        probe_fail("S7: returned from the plain handler with interrupts disabled "
                   "(eflags %08x)", fl_b);
    if (fl_b & 0x3000)
        probe_fail("S7: the plain frame's EFLAGS raised IOPL (eflags %08x)", fl_b);
    PORT_MUST_FAULT("after a handler rewrote its saved frame");
    probe_info("sigreturn keeps IOPL, IF and the segment registers out of the frame");

    /* S8: nested SA_ONSTACK frames must not walk off the alternate stack. */
    int mfd = memfd_create("p19", 0);
    if (mfd < 0)
        probe_fail("S8: memfd_create: %s", strerror(errno));
    int rc = posix_fallocate(mfd, 0, S8_GUARD + S8_ALT);
    if (rc != 0)
        probe_fail("S8: posix_fallocate: %s", strerror(rc));
    unsigned char *shared = mmap(NULL, S8_GUARD + S8_ALT, PROT_READ | PROT_WRITE,
                                 MAP_SHARED, mfd, 0);
    if (shared == MAP_FAILED)
        probe_fail("S8: mmap(memfd, MAP_SHARED): %s", strerror(errno));
    memset(shared, 0xAB, S8_GUARD + S8_ALT);
    fflush(stdout);
    pid_t kid = fork();
    if (kid < 0)
        probe_fail("S8: fork: %s", strerror(errno));
    if (kid == 0) {
        shared[0] = 0x5A;                   /* proves the mapping is shared */
        /* A wild fault in here must kill the child outright: with the parent's
         * SIGSEGV handler still installed it would siglongjmp back into the
         * parent's flow and print a verdict line of its own. */
        signal(SIGSEGV, SIG_DFL);
        signal(SIGBUS, SIG_DFL);
        stack_t s8;
        s8.ss_sp = shared + S8_GUARD;
        s8.ss_size = S8_ALT;
        s8.ss_flags = 0;
        if (sigaltstack(&s8, NULL) != 0)
            _exit(11);
        struct sigaction n;
        memset(&n, 0, sizeof n);
        n.sa_sigaction = on_nest;
        n.sa_flags = SA_ONSTACK | SA_SIGINFO | SA_NODEFER;
        sigemptyset(&n.sa_mask);
        if (sigaction(SIGUSR1, &n, NULL) != 0)
            _exit(12);
        raise(SIGUSR1);
        _exit(13);                          /* 32 frames fitted: nothing to judge */
    }
    int st = 0;
    if (waitpid(kid, &st, 0) != kid)
        probe_fail("S8: waitpid: %s", strerror(errno));
    if (shared[0] != 0x5A)
        probe_fail("S8: the MAP_SHARED memfd is not shared with the child, so the "
                   "guard region cannot be judged");
    if (!WIFSIGNALED(st) || WTERMSIG(st) != SIGSEGV)
        probe_fail("S8: the child %s%d instead of dying of SIGSEGV when its nested "
                   "frames ran out of alternate stack",
                   WIFSIGNALED(st) ? "was killed by signal " : "exited with status ",
                   WIFSIGNALED(st) ? WTERMSIG(st) : WEXITSTATUS(st));
    for (int i = 1; i < S8_GUARD - S8_SLACK; i++)
        if (shared[i] != 0xAB)
            probe_fail("S8: a signal frame was built %d bytes below the alternate "
                       "stack, over memory the process owns", S8_GUARD - i);
    probe_info("nested SA_ONSTACK frames stop at the alternate stack instead of "
               "running past it");

    probe_pass();
}
