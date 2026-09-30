/*
 * sigexecprobe — signal state across fork, clone and execve must follow Linux
 * (execve(2), fork(2), signal(7)).
 *
 * Set up: SIGINT = SIG_IGN, SIGUSR1 = a handler with SA_ONSTACK and a sa_mask,
 * an alternate signal stack, SIGUSR2 blocked and then raised (so pending).
 *
 *   fork   child inherits both dispositions, the blocked mask and the
 *          alternate stack; its pending set starts empty
 *   clone  CLONE_VM|CLONE_SIGHAND|CLONE_VFORK child (the handler table is
 *          SHARED) execs: the image it runs keeps SIG_IGN, sees SIGUSR1 at
 *          SIG_DFL, and the parent's table is left untouched
 *   exec   the process execs itself: SIGINT still SIG_IGN, SIGUSR1 back to
 *          SIG_DFL with sa_flags and sa_mask cleared, SIGUSR2 still blocked
 *          and still pending, no alternate stack
 *
 * Prints "sigexecprobe: <case> ok" per case, "... FAIL ..." on a failure and
 * "sigexecprobe ok" when every case passed (tools/smoke_toybox.py).
 */
#include "../include/stdio.h"
#include "../include/stdlib.h"
#include "../include/string.h"
#include "../include/stdint.h"
#include "../include/syscall.h"
#include "../include/unistd.h"
#include "../include/signal.h"
#include "../include/sys/wait.h"

#define NR_EXIT          1
#define NR_EXECVE        11
#define NR_SIGPENDING    73
#define NR_CLONE         120
#define NR_RT_SIGACTION  174
#define NR_RT_SIGPROCMASK 175
#define NR_RT_SIGPENDING 176
#define NR_SIGALTSTACK   186

#define K_SA_ONSTACK     0x08000000u
#define K_SS_DISABLE     2
#define K_CLONE_VM       0x00000100u
#define K_CLONE_SIGHAND  0x00000800u
#define K_CLONE_VFORK    0x00004000u

#define BIT(sig)         (1u << ((sig) - 1))    /* user sigset_t numbering */

#define SELF             "/sigexecprobe"

static int failures;

static void check(const char *what, int ok) {
    if (!ok) {
        printf("sigexecprobe: %s FAIL\n", what);
        failures++;
    }
}

/* Linux i386 struct kernel_sigaction: handler, flags, restorer, mask[2]. */
struct kact { uint32_t handler, flags, restorer, mask[2]; };

static int get_act(int sig, struct kact *out) {
    memset(out, 0, sizeof(*out));
    return syscall4(NR_RT_SIGACTION, sig, 0, (int)out, 8);
}

static int set_act(int sig, uint32_t handler, uint32_t flags, uint32_t mask) {
    struct kact a;
    memset(&a, 0, sizeof(a));
    a.handler = handler;
    a.flags   = flags;
    a.mask[0] = mask;
    return syscall4(NR_RT_SIGACTION, sig, (int)&a, 0, 8);
}

static uint32_t blocked(void) {
    uint32_t old = 0;
    syscall4(NR_RT_SIGPROCMASK, SIG_BLOCK, 0, (int)&old, 8);
    return old;
}

static uint32_t pending(void) {
    uint32_t set = 0xdeadbeef;
    if (syscall2(NR_RT_SIGPENDING, (int)&set, 8) < 0) return 0xdeadbeef;
    return set;
}

static int altstack_disabled(void) {
    uint32_t oss[3] = { 0, 0, 0 };
    if (syscall2(NR_SIGALTSTACK, 0, (int)oss) < 0) return 0;
    return (oss[1] & K_SS_DISABLE) && oss[2] == 0;
}

static void on_usr1(int sig) { (void)sig; }

/* ── the image exec'd by the process itself ─────────────────────────────── */
static int after_exec(void) {
    struct kact a;

    check("exec: SIGINT kept SIG_IGN",
          get_act(SIGINT, &a) == 0 && a.handler == (uint32_t)SIG_IGN);
    check("exec: SIGUSR1 reset to SIG_DFL",
          get_act(SIGUSR1, &a) == 0 && a.handler == (uint32_t)SIG_DFL);
    check("exec: SIGUSR1 sa_flags cleared", a.flags == 0);
    check("exec: SIGUSR1 sa_mask cleared", a.mask[0] == 0);
    check("exec: SIGUSR2 still blocked", (blocked() & BIT(SIGUSR2)) != 0);
    check("exec: SIGUSR2 still pending", pending() == BIT(SIGUSR2));
    uint32_t old = 0;
    check("exec: sigpending agrees",
          syscall1(NR_SIGPENDING, (int)&old) == 0 && old == BIT(SIGUSR2));
    check("exec: sigaltstack disabled", altstack_disabled());
    if (!failures) printf("sigexecprobe: exec ok\n");

    if (failures) {
        printf("sigexecprobe FAILED (%d)\n", failures);
        return 1;
    }
    printf("sigexecprobe ok\n");
    return 0;
}

/* ── the image exec'd by the CLONE_SIGHAND child ────────────────────────── */
static int after_clone_exec(void) {
    struct kact a;
    int ok = get_act(SIGINT, &a) == 0 && a.handler == (uint32_t)SIG_IGN;
    ok = ok && get_act(SIGUSR1, &a) == 0 && a.handler == (uint32_t)SIG_DFL;
    ok = ok && (blocked() & BIT(SIGUSR2)) && pending() == 0;
    return ok ? 0 : 1;
}

/* ── fork ───────────────────────────────────────────────────────────────── */
static void fork_case(void) {
    int pid = fork();
    if (pid < 0) { check("fork: fork()", 0); return; }
    if (pid == 0) {
        struct kact a;
        int ok = get_act(SIGINT, &a) == 0 && a.handler == (uint32_t)SIG_IGN;
        ok = ok && get_act(SIGUSR1, &a) == 0 &&
             a.handler == (uint32_t)on_usr1 && (a.flags & K_SA_ONSTACK) &&
             a.mask[0] == BIT(SIGTERM);
        ok = ok && (blocked() & BIT(SIGUSR2)) && pending() == 0;
        ok = ok && !altstack_disabled();
        _exit(ok ? 0 : 1);
    }
    int st = -1;
    check("fork: child inherits dispositions/mask, pending empty",
          waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0);
    check("fork: parent's SIGUSR2 still pending", pending() == BIT(SIGUSR2));
}

/* ── clone(CLONE_VM|CLONE_SIGHAND|CLONE_VFORK) + execve ─────────────────── */
static const char *g_path;
static char      **g_argv;
static char      **g_envp;
static char        clone_stack[8192] __attribute__((aligned(16)));

static void clone_case(char **envp) {
    static char *cargv[] = { "sigexecprobe", "clone-child", 0 };
    g_path = SELF;
    g_argv = cargv;
    g_envp = envp;

    /* The child shares our memory and runs on clone_stack; it never returns
     * into C — it execs straight away (or exits 127). */
    int pid;
    __asm__ volatile(
        "int $0x80\n\t"
        "test %%eax, %%eax\n\t"
        "jnz 1f\n\t"
        "movl %[p], %%ebx\n\t"
        "movl %[a], %%ecx\n\t"
        "movl %[e], %%edx\n\t"
        "movl $11, %%eax\n\t"
        "int $0x80\n\t"
        "movl $127, %%ebx\n\t"
        "movl $1, %%eax\n\t"
        "int $0x80\n"
        "1:"
        : "=a"(pid)
        : "0"(NR_CLONE),
          "b"(K_CLONE_VM | K_CLONE_SIGHAND | K_CLONE_VFORK | SIGCHLD),
          "c"(clone_stack + sizeof(clone_stack)), "d"(0), "S"(0), "D"(0),
          [p]"m"(g_path), [a]"m"(g_argv), [e]"m"(g_envp)
        : "memory");
    if (pid < 0) { check("clone: clone()", 0); return; }

    int st = -1;
    check("clone: exec'd image keeps SIG_IGN, resets handler",
          waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0);
    struct kact a;
    check("clone: parent's SIGUSR1 handler untouched by child's exec",
          get_act(SIGUSR1, &a) == 0 && a.handler == (uint32_t)on_usr1 &&
          (a.flags & K_SA_ONSTACK));
    check("clone: parent's SIGINT still SIG_IGN",
          get_act(SIGINT, &a) == 0 && a.handler == (uint32_t)SIG_IGN);
}

static char altstack[16384] __attribute__((aligned(16)));

int main(int argc, char **argv, char **envp) {
    if (argc > 1 && strcmp(argv[1], "exec-child") == 0) return after_exec();
    if (argc > 1 && strcmp(argv[1], "clone-child") == 0) return after_clone_exec();

    check("setup: SIGINT=SIG_IGN", set_act(SIGINT, (uint32_t)SIG_IGN, 0, 0) == 0);
    check("setup: SIGUSR1=handler",
          set_act(SIGUSR1, (uint32_t)on_usr1, K_SA_ONSTACK, BIT(SIGTERM)) == 0);
    uint32_t ss[3] = { (uint32_t)altstack, 0, sizeof(altstack) };
    check("setup: sigaltstack", syscall2(NR_SIGALTSTACK, (int)ss, 0) == 0);
    uint32_t set = BIT(SIGUSR2);
    check("setup: block SIGUSR2",
          syscall4(NR_RT_SIGPROCMASK, SIG_BLOCK, (int)&set, 0, 8) == 0);
    check("setup: raise SIGUSR2", kill(getpid(), SIGUSR2) == 0);
    check("setup: SIGUSR2 pending", pending() == BIT(SIGUSR2));
    if (failures) {
        printf("sigexecprobe FAILED (%d)\n", failures);
        return 1;
    }

    int before = failures;
    fork_case();
    if (failures == before) printf("sigexecprobe: fork ok\n");
    before = failures;
    clone_case(envp);
    if (failures == before) printf("sigexecprobe: clone ok\n");
    if (failures) {
        printf("sigexecprobe FAILED (%d)\n", failures);
        return 1;
    }

    fflush(stdout);
    char *xargv[] = { "sigexecprobe", "exec-child", 0 };
    syscall3(NR_EXECVE, (int)SELF, (int)xargv, (int)envp);
    printf("sigexecprobe: exec FAIL (execve returned)\n");
    return 1;
}
