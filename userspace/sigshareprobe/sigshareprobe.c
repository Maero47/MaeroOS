/*
 * sigshareprobe — SA_RESETHAND/SA_NODEFER and process-wide (shared) pending
 * signals must follow Linux (sigaction(2), signal(7), kernel/signal.c).
 *
 *   resethand  a SA_RESETHAND handler runs once and the disposition is back to
 *              SIG_DFL inside it; the second signal takes the default action
 *              (the child dies of SIGUSR1)
 *   nodefer    without SA_NODEFER the signal is blocked while its handler
 *              runs, with it it is not; sa_mask is blocked either way; the
 *              mask is put back when the handler returns
 *   threads    thread A blocks SIGUSR1, thread B does not: kill(pid) is
 *              handled by B.  With both blocking it stays pending for the
 *              process (sigpending shows it in either thread) until one of
 *              them unblocks it — B first, then A itself
 *   exit       both threads block SIGUSR1, kill(pid), then the LEADER exits
 *              as a thread: the signal is still pending for the surviving
 *              thread, which takes it on unblocking
 *   exec       both threads block SIGUSR1, kill(pid), then the NON-leader
 *              execve()s: the new image sees it pending and takes it on
 *              unblocking
 *   stop       job control: the leader sleeps in pthread_join, a worker
 *              spins; SIGTSTP to the process stops the WHOLE group —
 *              waitpid(WUNTRACED) reports it and the worker makes no progress
 *              — and SIGCONT resumes it (waitpid(WCONTINUED)); then it exits
 *              cleanly
 *
 * Every multi-threaded case runs in a forked child so a stray signal cannot
 * take the probe down.  Prints "sigshareprobe: <case> ok" per case,
 * "... FAIL ..." on a failure and "sigshareprobe ok" when every case passed
 * (tools/smoke_toybox.py).
 */
#include "../include/stdio.h"
#include "../include/stdlib.h"
#include "../include/string.h"
#include "../include/stdint.h"
#include "../include/syscall.h"
#include "../include/unistd.h"
#include "../include/signal.h"
#include "../include/pthread.h"
#include "../include/time.h"
#include "../include/sys/wait.h"
#include "../include/fcntl.h"

#define NR_EXIT           1
#define NR_EXECVE         11
#define NR_WAITPID        7
#define NR_KILL           37
#define NR_FCNTL          55
#define NR_RT_SIGACTION   174
#define NR_RT_SIGPROCMASK 175
#define NR_RT_SIGPENDING  176
#define NR_GETTID         224
#define NR_EXIT_GROUP     252

#define K_SA_NODEFER      0x40000000u
#define K_SA_RESETHAND    0x80000000u

#define BIT(sig)          (1u << ((sig) - 1))    /* user sigset_t numbering */

#define SELF              "/sigshareprobe"

static int failures;

static void check(const char *what, int ok) {
    if (!ok) {
        printf("sigshareprobe: %s FAIL\n", what);
        failures++;
    }
}

/* Linux i386 struct kernel_sigaction: handler, flags, restorer, mask[2]. */
struct kact { uint32_t handler, flags, restorer, mask[2]; };

static int get_act(int sig, struct kact *out) {
    memset(out, 0, sizeof(*out));
    return syscall4(NR_RT_SIGACTION, sig, 0, (int)out, 8);
}

static int set_act(int sig, void (*h)(int), uint32_t flags, uint32_t mask) {
    struct kact a;
    memset(&a, 0, sizeof(a));
    a.handler = (uint32_t)h;
    a.flags   = flags;
    a.mask[0] = mask;
    return syscall4(NR_RT_SIGACTION, sig, (int)&a, 0, 8);
}

static uint32_t blocked(void) {
    uint32_t old = 0;
    syscall4(NR_RT_SIGPROCMASK, SIG_BLOCK, 0, (int)&old, 8);
    return old;
}

static void mask(int how, uint32_t set) {
    syscall4(NR_RT_SIGPROCMASK, how, (int)&set, 0, 8);
}

static uint32_t pending(void) {
    uint32_t set = 0xdeadbeef;
    if (syscall2(NR_RT_SIGPENDING, (int)&set, 8) < 0) return 0xdeadbeef;
    return set;
}

static int gettid_(void) { return syscall0(NR_GETTID); }
static int kill_(int pid, int sig) { return syscall2(NR_KILL, pid, sig); }

static void msleep(int ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, 0);
}

/* Wait (bounded, ~2 s at the 100 Hz tick) until *flag reaches want, so a
 * signal that never arrives is a FAIL rather than a hang. */
static int wait_for(volatile int *flag, int want) {
    for (int i = 0; i < 200; i++) {
        if (*flag >= want) return 1;
        msleep(10);
    }
    return *flag >= want;
}

/* Run fn in a forked child; its exit status is its failure count. */
static int in_child(int (*fn)(void)) {
    fflush(stdout);
    int pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int f = fn();
        fflush(stdout);
        syscall1(NR_EXIT_GROUP, f);
    }
    int st = -1;
    if (waitpid(pid, &st, 0) != pid) return -1;
    if (!WIFEXITED(st)) {
        printf("sigshareprobe: child died (status %x)\n", st);
        return -1;
    }
    return WEXITSTATUS(st);
}

/* ── resethand ──────────────────────────────────────────────────────────── */
static volatile int g_once_hits;
static volatile uint32_t g_once_act = 0xdeadbeef, g_once_blocked;

static void on_once(int sig) {
    (void)sig;
    struct kact a;
    g_once_hits++;
    if (get_act(SIGUSR1, &a) == 0) g_once_act = a.handler;
    g_once_blocked = blocked();
}

static void resethand_case(void) {
    int fds[2];
    if (pipe(fds) < 0) { check("resethand: pipe", 0); return; }
    fflush(stdout);
    int pid = fork();
    if (pid < 0) { check("resethand: fork", 0); return; }
    if (pid == 0) {
        close(fds[0]);
        int ok = set_act(SIGUSR1, on_once, K_SA_RESETHAND, 0) == 0;
        ok = ok && kill_(getpid(), SIGUSR1) == 0;
        ok = ok && g_once_hits == 1;
        ok = ok && g_once_act == (uint32_t)SIG_DFL;        /* reset on entry */
        ok = ok && (g_once_blocked & BIT(SIGUSR1));        /* still deferred */
        struct kact a;
        ok = ok && get_act(SIGUSR1, &a) == 0 && a.handler == (uint32_t)SIG_DFL;
        char c = ok ? 'Y' : 'N';
        write(fds[1], &c, 1);
        kill_(getpid(), SIGUSR1);                          /* default: die */
        _exit(99);
    }
    close(fds[1]);
    char c = 0;
    check("resethand: handler ran once, disposition reset to SIG_DFL",
          read(fds[0], &c, 1) == 1 && c == 'Y');
    close(fds[0]);
    int st = -1;
    check("resethand: second signal takes the default action",
          waitpid(pid, &st, 0) == pid && WIFSIGNALED(st) && WTERMSIG(st) == SIGUSR1);
}

/* ── nodefer ────────────────────────────────────────────────────────────── */
static volatile uint32_t g_in_mask;

static void on_usr2(int sig) { (void)sig; g_in_mask = blocked(); }

static void nodefer_case(void) {
    uint32_t before = blocked();
    set_act(SIGUSR2, on_usr2, 0, BIT(SIGTERM));
    g_in_mask = 0;
    kill_(getpid(), SIGUSR2);
    check("nodefer: signal blocked in its handler by default",
          (g_in_mask & BIT(SIGUSR2)) && (g_in_mask & BIT(SIGTERM)));
    set_act(SIGUSR2, on_usr2, K_SA_NODEFER, BIT(SIGTERM));
    g_in_mask = 0;
    kill_(getpid(), SIGUSR2);
    check("nodefer: SA_NODEFER leaves it unblocked, sa_mask still applies",
          !(g_in_mask & BIT(SIGUSR2)) && (g_in_mask & BIT(SIGTERM)));
    check("nodefer: mask restored after the handler", blocked() == before);
    set_act(SIGUSR2, (void (*)(int))SIG_DFL, 0, 0);
}

/* ── shared state for the threaded cases ────────────────────────────────── */
static volatile int g_hits, g_hit_tid;
static volatile int b_tid, b_phase, a_phase;

static void on_usr1(int sig) {
    (void)sig;
    g_hit_tid = gettid_();
    g_hits++;
}

/* ── threads ────────────────────────────────────────────────────────────── */
static void *threads_b(void *arg) {
    (void)arg;
    b_tid = gettid_();
    mask(SIG_UNBLOCK, BIT(SIGUSR1));
    b_phase = 1;                                  /* ready, SIGUSR1 open */
    wait_for(&a_phase, 1);
    mask(SIG_BLOCK, BIT(SIGUSR1));
    b_phase = 2;                                  /* SIGUSR1 blocked */
    wait_for(&a_phase, 2);
    mask(SIG_UNBLOCK, BIT(SIGUSR1));              /* takes the pending one */
    b_phase = 3;
    wait_for(&a_phase, 3);
    mask(SIG_BLOCK, BIT(SIGUSR1));
    b_phase = 4;
    wait_for(&a_phase, 4);                        /* until A is done */
    return 0;
}

static int threads_child(void) {
    int me = gettid_();
    set_act(SIGUSR1, on_usr1, 0, 0);
    mask(SIG_BLOCK, BIT(SIGUSR1));
    pthread_t t;
    if (pthread_create(&t, 0, threads_b, 0) != 0) { check("threads: create", 0); return failures; }
    check("threads: B ready", wait_for(&b_phase, 1));

    /* 1: A blocks, B does not → B takes it. */
    kill_(getpid(), SIGUSR1);
    check("threads: kill(pid) handled by the unblocked thread",
          wait_for(&g_hits, 1) && g_hit_tid == b_tid);

    /* 2: both block → pending for the process, visible from A. */
    a_phase = 1;
    check("threads: B blocked", wait_for(&b_phase, 2));
    kill_(getpid(), SIGUSR1);
    msleep(50);
    check("threads: all-blocked signal not delivered", g_hits == 1);
    check("threads: sigpending shows it", pending() & BIT(SIGUSR1));
    /* B unblocks → B takes it. */
    a_phase = 2;
    check("threads: unblocking in B delivers it to B",
          wait_for(&g_hits, 2) && g_hit_tid == b_tid);
    check("threads: no longer pending", !(pending() & BIT(SIGUSR1)));

    /* 3: both block again; A unblocks itself → A takes it at once. */
    a_phase = 3;
    check("threads: B blocked again", wait_for(&b_phase, 4));
    kill_(getpid(), SIGUSR1);
    msleep(20);
    check("threads: pending again", (pending() & BIT(SIGUSR1)) && g_hits == 2);
    mask(SIG_UNBLOCK, BIT(SIGUSR1));
    check("threads: unblocking in A delivers it to A",
          g_hits == 3 && g_hit_tid == me);

    a_phase = 4;
    pthread_join(t, 0);
    return failures;
}

/* ── exit: the leader leaves, the signal stays ──────────────────────────── */
static void *exit_b(void *arg) {
    (void)arg;
    b_phase = 1;
    wait_for(&a_phase, 1);
    msleep(100);                                  /* the leader is gone by now */
    int f = 0;
    if (!(blocked() & BIT(SIGUSR1))) { printf("sigshareprobe: exit: B mask FAIL\n"); f++; }
    if (!(pending() & BIT(SIGUSR1))) {
        printf("sigshareprobe: exit: pending after the leader exited FAIL\n"); f++;
    }
    mask(SIG_UNBLOCK, BIT(SIGUSR1));
    if (g_hits != 1 || g_hit_tid != gettid_()) {
        printf("sigshareprobe: exit: survivor takes it on unblock FAIL\n"); f++;
    }
    fflush(stdout);
    syscall1(NR_EXIT_GROUP, f);
    return 0;
}

static int exit_child(void) {
    set_act(SIGUSR1, on_usr1, 0, 0);
    mask(SIG_BLOCK, BIT(SIGUSR1));
    pthread_t t;
    if (pthread_create(&t, 0, exit_b, 0) != 0) return 1;
    if (!wait_for(&b_phase, 1)) return 1;
    kill_(getpid(), SIGUSR1);
    if (g_hits != 0) return 1;
    a_phase = 1;
    syscall1(NR_EXIT, 0);                         /* this thread only */
    return 1;
}

/* ── exec: a non-leader execs, the signal carries over ──────────────────── */
static char **g_envp;

static void *exec_b(void *arg) {
    (void)arg;
    wait_for(&a_phase, 1);
    fflush(stdout);
    char *xargv[] = { "sigshareprobe", "exec-child", 0 };
    syscall3(NR_EXECVE, (int)SELF, (int)xargv, (int)g_envp);
    syscall1(NR_EXIT_GROUP, 100);
    return 0;
}

static int exec_child(void) {
    mask(SIG_BLOCK, BIT(SIGUSR1));
    pthread_t t;
    if (pthread_create(&t, 0, exec_b, 0) != 0) return 1;
    kill_(getpid(), SIGUSR1);
    a_phase = 1;
    for (;;) sched_yield();                       /* until de_thread kills us */
}

static int after_exec(void) {
    int f = 0;
    if (!(blocked() & BIT(SIGUSR1))) { printf("sigshareprobe: exec: mask kept FAIL\n"); f++; }
    if (pending() != BIT(SIGUSR1)) {
        printf("sigshareprobe: exec: process-wide pending kept FAIL (%x)\n",
               (unsigned)pending());
        f++;
    }
    set_act(SIGUSR1, on_usr1, 0, 0);
    mask(SIG_UNBLOCK, BIT(SIGUSR1));
    if (g_hits != 1) { printf("sigshareprobe: exec: delivered on unblock FAIL\n"); f++; }
    fflush(stdout);
    return f;
}

/* ── stop: group stop and continue ──────────────────────────────────────── */
static int g_pipe_w;
static volatile int g_quit;

static void on_quit(int sig) { (void)sig; g_quit = 1; }

static void *stop_worker(void *arg) {
    (void)arg;
    while (!g_quit) {
        for (volatile int i = 0; i < 200000; i++) { }
        char c = 'x';
        write(g_pipe_w, &c, 1);
    }
    return 0;
}

static int stop_child(void) {
    /* Whatever started us may ignore SIGTSTP, and SIG_IGN survives exec. */
    set_act(SIGTSTP, (void (*)(int))SIG_DFL, 0, 0);
    set_act(SIGUSR1, on_quit, 0, 0);
    pthread_t t;
    if (pthread_create(&t, 0, stop_worker, 0) != 0) return 1;
    pthread_join(t, 0);                           /* the leader sleeps here */
    return 0;
}

/* Bytes waiting in the non-blocking pipe fd (the worker's progress). */
static int drain(int fd) {
    char buf[256];
    int n = 0, r;
    while ((r = read(fd, buf, sizeof(buf))) > 0) n += r;
    return n;
}

static void stop_case(void) {
    int fds[2];
    if (pipe(fds) < 0) { check("stop: pipe", 0); return; }
    g_pipe_w = fds[1];
    fflush(stdout);
    int pid = fork();
    if (pid < 0) { check("stop: fork", 0); return; }
    if (pid == 0) {
        close(fds[0]);
        int f = stop_child();
        syscall1(NR_EXIT_GROUP, f);
    }
    close(fds[1]);
    char c;
    check("stop: worker running", read(fds[0], &c, 1) == 1);
    syscall3(NR_FCNTL, fds[0], F_SETFL, O_NONBLOCK);

    int st = -1;
    kill_(pid, SIGTSTP);
    int r = syscall3(NR_WAITPID, pid, (int)&st, 2 /* WUNTRACED */);
    check("stop: waitpid(WUNTRACED) reports the process stopped by SIGTSTP",
          r == pid && (st & 0xff) == 0x7f && ((st >> 8) & 0xff) == SIGTSTP);
    msleep(100);
    drain(fds[0]);                                /* anything written before */
    msleep(300);
    check("stop: the worker thread is stopped too", drain(fds[0]) == 0);

    kill_(pid, SIGCONT);
    st = -1;
    r = syscall3(NR_WAITPID, pid, (int)&st, 8 /* WCONTINUED */);
    check("stop: waitpid(WCONTINUED) reports it continued",
          r == pid && st == 0xffff);
    int got = 0;
    for (int i = 0; i < 200 && !got; i++) { got = drain(fds[0]); msleep(10); }
    check("stop: the worker runs again after SIGCONT", got > 0);

    kill_(pid, SIGUSR1);                          /* ask it to finish */
    st = -1;
    r = syscall3(NR_WAITPID, pid, (int)&st, 0);
    check("stop: clean exit after resuming",
          r == pid && (st & 0x7f) == 0 && ((st >> 8) & 0xff) == 0);
    close(fds[0]);
}

static void run(const char *name, int (*fn)(void)) {
    int f = in_child(fn);
    if (f) {
        printf("sigshareprobe: %s FAIL (%d)\n", name, f);
        failures++;
    } else {
        printf("sigshareprobe: %s ok\n", name);
    }
}

/* `sigshareprobe disp`: print the disposition this process was started with
 * for the signals an interactive shell ignores for itself (D = SIG_DFL,
 * I = SIG_IGN, H = a handler) — what the shell hands its commands. */
static int print_dispositions(void) {
    static const struct { int sig; const char *name; } s[] = {
        { SIGINT, "INT" }, { SIGQUIT, "QUIT" }, { SIGPIPE, "PIPE" },
        { SIGTSTP, "TSTP" }, { SIGTTIN, "TTIN" }, { SIGTTOU, "TTOU" },
    };
    printf("disp:");
    for (unsigned i = 0; i < sizeof(s) / sizeof(s[0]); i++) {
        struct kact a;
        get_act(s[i].sig, &a);
        printf(" %s=%c", s[i].name,
               a.handler == (uint32_t)SIG_DFL ? 'D' :
               a.handler == (uint32_t)SIG_IGN ? 'I' : 'H');
    }
    printf("\n");
    return 0;
}

int main(int argc, char **argv, char **envp) {
    if (argc > 1 && strcmp(argv[1], "exec-child") == 0) return after_exec();
    if (argc > 1 && strcmp(argv[1], "disp") == 0) return print_dispositions();
    g_envp = envp;

    int before = failures;
    resethand_case();
    if (failures == before) printf("sigshareprobe: resethand ok\n");
    before = failures;
    nodefer_case();
    if (failures == before) printf("sigshareprobe: nodefer ok\n");
    run("threads", threads_child);
    run("exit", exit_child);
    run("exec", exec_child);
    before = failures;
    stop_case();
    if (failures == before) printf("sigshareprobe: stop ok\n");

    if (failures) {
        printf("sigshareprobe FAILED (%d)\n", failures);
        return 1;
    }
    printf("sigshareprobe ok\n");
    return 0;
}
