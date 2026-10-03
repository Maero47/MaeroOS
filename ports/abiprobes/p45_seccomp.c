/*
 * P45 seccomp(2) filters and prctl(PR_SET_NO_NEW_PRIVS): what Firefox's
 * content-process sandbox (security/sandbox/linux) relies on.
 *
 * Linux: a classic-BPF program over struct seccomp_data runs before every
 * system call of a thread that installed one (or inherited it through fork,
 * clone or execve).  Its answer is ALLOW, ERRNO(n) (the call fails with n),
 * TRAP(n) (SIGSYS with si_code SYS_SECCOMP, si_errno n, si_call_addr,
 * si_syscall and si_arch; the handler's ucontext holds the call's registers
 * and whatever it leaves in eax is the result), KILL_THREAD or KILL_PROCESS
 * (death by SIGSYS), or LOG (allowed and logged).  The strictest answer of all
 * the stacked filters wins.  SECCOMP_FILTER_FLAG_TSYNC puts the filter on
 * every thread, or returns the tid of one that holds an unrelated filter.
 * Installing a filter needs no_new_privs (or CAP_SYS_ADMIN), and no_new_privs
 * makes execve ignore the set-user-ID bit.  Programs are validated: at most
 * 4096 instructions, aligned word loads inside seccomp_data only, forward
 * in-range jumps, no division by a constant zero, scratch cells written
 * before read, a RET at the end.
 *
 * MaeroOS: syscall 354 did not exist and prctl() accepted every option and
 * did nothing, so Firefox ran its children with the sandbox switched off.
 *
 * The set-uid part needs root (to give a copy of this binary another owner);
 * on a non-root Linux host it is reported as an info line and skipped.
 */
#define PROBE_NAME "p45_seccomp"
#include "probe.h"
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <linux/audit.h>
#include <stddef.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <ucontext.h>

#ifndef SECCOMP_RET_KILL_PROCESS
#define SECCOMP_RET_KILL_PROCESS 0x80000000U
#endif
#ifndef SECCOMP_RET_LOG
#define SECCOMP_RET_LOG 0x7ffc0000U
#endif
#ifndef SECCOMP_FILTER_FLAG_TSYNC
#define SECCOMP_FILTER_FLAG_TSYNC 1
#endif

#define NR_OFF   offsetof(struct seccomp_data, nr)
#define ARCH_OFF offsetof(struct seccomp_data, arch)
#define ARG0_OFF offsetof(struct seccomp_data, args[0])

static long sys_seccomp(unsigned op, unsigned flags, const void *args)
{
    return syscall(SYS_seccomp, op, flags, args);
}

static int install(const struct sock_filter *insns, unsigned short len, unsigned flags)
{
    struct sock_fprog prog = { len, (struct sock_filter *)insns };
    return (int)sys_seccomp(SECCOMP_SET_MODE_FILTER, flags, &prog);
}

/* A filter that answers `ret` for syscall `nr` and allows everything else. */
static int install_one(int nr, unsigned ret, unsigned flags)
{
    struct sock_filter f[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, ARCH_OFF),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_I386, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, NR_OFF),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (unsigned)nr, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, ret),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    return install(f, sizeof f / sizeof f[0], flags);
}

static void nnp(void)
{
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
        probe_fail("PR_SET_NO_NEW_PRIVS: %s", strerror(errno));
}

/* Run fn in a child; return its wait status. */
static int in_child(void (*fn)(void))
{
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) probe_fail("fork: %s", strerror(errno));
    if (pid == 0) { fn(); _exit(0); }
    int st;
    if (waitpid(pid, &st, 0) != pid) probe_fail("waitpid: %s", strerror(errno));
    return st;
}

static void expect_exit0(const char *what, int st)
{
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
        probe_fail("%s: child status %#x, want exit 0", what, st);
}

static void expect_sig(const char *what, int st, int sig)
{
    if (!WIFSIGNALED(st) || WTERMSIG(st) != sig)
        probe_fail("%s: child status %#x, want killed by signal %d", what, st, sig);
}

/* Child-side failure: report on stdout and exit 1 (the parent then fails). */
static void cfail(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
static void cfail(const char *fmt, ...)
{
    va_list ap;
    printf("info %s: child: ", PROBE_NAME);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
    _exit(1);
}

/* ── validation ─────────────────────────────────────────────────────────── */

static void check_rejected(const char *what, const struct sock_filter *f, unsigned short n)
{
    errno = 0;
    if (install(f, n, 0) != -1 || errno != EINVAL)
        cfail("%s: install = %s, want EINVAL", what, strerror(errno));
}

static void c_validation(void)
{
    nnp();
    struct sock_filter backward[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, NR_OFF),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        BPF_JUMP(BPF_JMP | BPF_JA, 0xfffffffeU, 0, 0),   /* back to 1 */
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    check_rejected("backward jump", backward, 4);
    struct sock_filter past_end[] = {
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 5, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    check_rejected("jump past the end", past_end, 2);
    struct sock_filter ja_past[] = {
        BPF_JUMP(BPF_JMP | BPF_JA, 1, 0, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    check_rejected("ja past the end", ja_past, 2);
    struct sock_filter misaligned[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 2),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    check_rejected("misaligned load", misaligned, 2);
    struct sock_filter beyond[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 64),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    check_rejected("load past seccomp_data", beyond, 2);
    struct sock_filter byte_load[] = {
        BPF_STMT(BPF_LD | BPF_B | BPF_ABS, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    check_rejected("byte load", byte_load, 2);
    struct sock_filter div0[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, NR_OFF),
        BPF_STMT(BPF_ALU | BPF_DIV | BPF_K, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    check_rejected("divide by constant 0", div0, 3);
    struct sock_filter no_ret[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, NR_OFF),
    };
    check_rejected("no final RET", no_ret, 1);
    struct sock_filter uninit[] = {
        BPF_STMT(BPF_LD | BPF_MEM, 3),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    check_rejected("read of an unwritten scratch cell", uninit, 2);
    struct sock_filter half_init[] = {         /* written on one path only */
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, NR_OFF),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 0, 1),
        BPF_STMT(BPF_ST, 2),
        BPF_STMT(BPF_LDX | BPF_MEM, 2),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    check_rejected("scratch cell written on one path", half_init, 5);
    struct sock_filter mem_ok[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, NR_OFF),
        BPF_STMT(BPF_ST, 15),
        BPF_STMT(BPF_LDX | BPF_MEM, 15),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    if (install(mem_ok, 4, 0) != 0)
        cfail("a valid scratch-cell program was refused: %s", strerror(errno));
    struct sock_filter one = BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
    errno = 0;
    if (install(&one, 0, 0) != -1 || errno != EINVAL)
        cfail("len 0: %s, want EINVAL", strerror(errno));
    static struct sock_filter big[4097];
    for (int i = 0; i < 4097; i++) big[i] = one;
    errno = 0;
    if (install(big, 4097, 0) != -1 || errno != EINVAL)
        cfail("len 4097: %s, want EINVAL", strerror(errno));
    if (install(big, 4096, 0) != 0)
        cfail("len 4096 refused: %s", strerror(errno));
    errno = 0;
    if (sys_seccomp(SECCOMP_SET_MODE_FILTER, 0x80000000U, NULL) != -1 || errno != EINVAL)
        cfail("unknown flag: %s, want EINVAL", strerror(errno));
}

/* ── ALLOW / ERRNO, stacking, irremovability ─────────────────────────────── */

static void c_errno(void)
{
    nnp();
    if (prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) != 1)
        cfail("PR_GET_NO_NEW_PRIVS != 1");
    errno = 0;
    if (prctl(PR_SET_NO_NEW_PRIVS, 0, 0, 0, 0) != -1 || errno != EINVAL)
        cfail("PR_SET_NO_NEW_PRIVS(0) = %s, want EINVAL", strerror(errno));
    if (prctl(PR_GET_SECCOMP, 0, 0, 0, 0) != 0) cfail("PR_GET_SECCOMP before != 0");
    if (install_one(SYS_getppid, SECCOMP_RET_ERRNO | 77, 0) != 0)
        cfail("install: %s", strerror(errno));
    if (prctl(PR_GET_SECCOMP, 0, 0, 0, 0) != 2) cfail("PR_GET_SECCOMP after != 2");
    errno = 0;
    if (syscall(SYS_getppid) != -1 || errno != 77)
        cfail("getppid under ERRNO(77): %s", strerror(errno));
    if (syscall(SYS_getpid) != getpid() || getpid() <= 0) cfail("getpid not allowed");
    /* An ALLOW-everything filter on top cannot lift the ERRNO below it. */
    struct sock_filter allow = BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
    if (install(&allow, 1, 0) != 0) cfail("allow filter: %s", strerror(errno));
    errno = 0;
    if (syscall(SYS_getppid) != -1 || errno != 77)
        cfail("a later ALLOW filter weakened ERRNO: %s", strerror(errno));
    /* ERRNO data above 4095 is clamped. */
    if (install_one(SYS_getuid32, SECCOMP_RET_ERRNO | 0xffff, 0) != 0)
        cfail("install 2: %s", strerror(errno));
    errno = 0;
    if (syscall(SYS_getuid32) != -1 || errno != 4095)
        cfail("ERRNO(0xffff) gave errno %d, want 4095", errno);
    /* Strict mode cannot replace filter mode. */
    errno = 0;
    if (sys_seccomp(SECCOMP_SET_MODE_STRICT, 0, NULL) != -1 || errno != EINVAL)
        cfail("STRICT over FILTER: %s, want EINVAL", strerror(errno));
    /* LOG lets the call through. */
    if (install_one(SYS_getpid, SECCOMP_RET_LOG, 0) != 0)
        cfail("LOG filter: %s", strerror(errno));
    if (syscall(SYS_getpid) <= 0) cfail("getpid under LOG failed");
}

/* ── TRAP ────────────────────────────────────────────────────────────────── */

static volatile int trap_ok;
static char trap_why[160];

static void sigsys_handler(int sig, siginfo_t *si, void *vctx)
{
    ucontext_t *uc = vctx;
    greg_t *g = uc->uc_mcontext.gregs;
    trap_ok = 0;
    if (sig != SIGSYS || si->si_code != 1 /* SYS_SECCOMP */) {
        snprintf(trap_why, sizeof trap_why, "sig %d code %d", sig, si->si_code);
        return;
    }
    if (si->si_errno != 0x42 || si->si_syscall != SYS_dup ||
        si->si_arch != AUDIT_ARCH_I386) {
        snprintf(trap_why, sizeof trap_why, "si_errno %#x si_syscall %d si_arch %#x",
                 si->si_errno, si->si_syscall, si->si_arch);
        return;
    }
    if ((unsigned long)si->si_call_addr != (unsigned long)g[REG_EIP]) {
        snprintf(trap_why, sizeof trap_why, "si_call_addr %p != eip %#lx",
                 si->si_call_addr, (unsigned long)g[REG_EIP]);
        return;
    }
    if (g[REG_EAX] != SYS_dup || g[REG_EBX] != 4321) {
        snprintf(trap_why, sizeof trap_why, "ucontext eax %ld ebx %ld",
                 (long)g[REG_EAX], (long)g[REG_EBX]);
        return;
    }
    trap_ok = 1;
    g[REG_EAX] = 9876;                   /* the emulated result */
}

static void c_trap(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = sigsys_handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGSYS, &sa, NULL);
    nnp();
    if (install_one(SYS_dup, SECCOMP_RET_TRAP | 0x42, 0) != 0)
        cfail("install: %s", strerror(errno));
    long r = syscall(SYS_dup, 4321);
    if (!trap_ok) cfail("SIGSYS handler: %s", trap_why[0] ? trap_why : "never ran");
    if (r != 9876) cfail("trapped dup returned %ld, want the handler's 9876", r);
    /* TRAP beats ERRNO whatever the order of the filters. */
    if (install_one(SYS_dup, SECCOMP_RET_ERRNO | 5, 0) != 0)
        cfail("install 2: %s", strerror(errno));
    trap_ok = 0;
    r = syscall(SYS_dup, 4321);
    if (!trap_ok || r != 9876) cfail("ERRNO filter on top of TRAP won (r=%ld)", r);
}

/* A TRAP with SIGSYS blocked or ignored is forced: the process dies. */
static void c_trap_blocked(void)
{
    sigset_t s;
    sigemptyset(&s);
    sigaddset(&s, SIGSYS);
    sigprocmask(SIG_BLOCK, &s, NULL);
    nnp();
    if (install_one(SYS_dup, SECCOMP_RET_TRAP | 1, 0) != 0)
        cfail("install: %s", strerror(errno));
    syscall(SYS_dup, 0);
    cfail("survived a TRAP with SIGSYS blocked");
}

/* ── KILL ────────────────────────────────────────────────────────────────── */

static void c_kill_process(void)
{
    nnp();
    if (install_one(SYS_getppid, SECCOMP_RET_KILL_PROCESS, 0) != 0)
        cfail("install: %s", strerror(errno));
    syscall(SYS_getppid);
    cfail("survived KILL_PROCESS");
}

static void *kp_thread(void *arg)
{
    (void)arg;
    syscall(SYS_getppid);
    return NULL;
}

/* KILL_PROCESS from a second thread takes the whole process down. */
static void c_kill_process_thread(void)
{
    nnp();
    if (install_one(SYS_getppid, SECCOMP_RET_KILL_PROCESS, 0) != 0)
        cfail("install: %s", strerror(errno));
    pthread_t t;
    pthread_create(&t, NULL, kp_thread, NULL);
    sleep_ms(3000);
    cfail("main thread survived KILL_PROCESS in another thread");
}

static void c_kill_thread_single(void)
{
    nnp();
    if (install_one(SYS_getppid, SECCOMP_RET_KILL_THREAD, 0) != 0)
        cfail("install: %s", strerror(errno));
    syscall(SYS_getppid);
    cfail("survived KILL_THREAD");
}

static volatile int kt_after;
static volatile long kt_tid;

static void *kt_thread(void *arg)
{
    (void)arg;
    kt_tid = raw_gettid();
    syscall(SYS_getppid);
    kt_after = 1;                        /* must never run */
    return NULL;
}

/* KILL_THREAD in one of two threads ends only that thread.  (Not joined:
 * Linux leaves the CLONE_CHILD_CLEARTID word of a thread it killed this way
 * as it was, so pthread_join would wait for ever.  The tid going away is the
 * evidence instead.) */
static void c_kill_thread_multi(void)
{
    nnp();
    if (install_one(SYS_getppid, SECCOMP_RET_KILL_THREAD, 0) != 0)
        cfail("install: %s", strerror(errno));
    pthread_t t;
    if (pthread_create(&t, NULL, kt_thread, NULL) != 0) cfail("pthread_create");
    for (int i = 0; i < 500; i++) {
        long tid = kt_tid;
        if (tid > 0 && syscall(SYS_tgkill, getpid(), tid, 0) == -1 && errno == ESRCH)
            break;
        sleep_ms(10);
    }
    if (kt_tid <= 0 || syscall(SYS_tgkill, getpid(), kt_tid, 0) != -1)
        cfail("the thread that made the killed call is still there");
    if (kt_after) cfail("the killed thread went on running");
}

static void c_strict(void)
{
    if (sys_seccomp(SECCOMP_SET_MODE_STRICT, 0, NULL) != 0)
        cfail("STRICT: %s", strerror(errno));
    static const char msg[] = "";
    if (syscall(SYS_write, 1, msg, 0) != 0) syscall(SYS_exit, 3);
    syscall(SYS_getpid);
    syscall(SYS_exit, 4);
}

/* ── TSYNC ───────────────────────────────────────────────────────────────── */

static int ts_pipe[2];
static volatile long ts_result, ts_errno;

static void *ts_thread(void *arg)
{
    (void)arg;
    char c;
    if (read(ts_pipe[0], &c, 1) != 1) return NULL;
    errno = 0;
    ts_result = syscall(SYS_getppid);
    ts_errno = errno;
    return NULL;
}

static void c_tsync(void)
{
    if (pipe(ts_pipe) != 0) cfail("pipe");
    pthread_t t;
    if (pthread_create(&t, NULL, ts_thread, NULL) != 0) cfail("pthread_create");
    sleep_ms(100);
    nnp();
    long r = install_one(SYS_getppid, SECCOMP_RET_ERRNO | 33, SECCOMP_FILTER_FLAG_TSYNC);
    if (r != 0) cfail("TSYNC install returned %ld (%s)", r, strerror(errno));
    if (write(ts_pipe[1], "x", 1) != 1) cfail("write");
    pthread_join(t, NULL);
    if (ts_result != -1 || ts_errno != 33)
        cfail("other thread after TSYNC: getppid = %ld errno %ld, want -1/33",
              ts_result, ts_errno);
}

static volatile long tsf_tid;
static volatile int tsf_stage;

static void *tsf_thread(void *arg)
{
    (void)arg;
    tsf_tid = raw_gettid();
    if (install_one(SYS_getuid32, SECCOMP_RET_ERRNO | 1, 0) != 0) tsf_tid = -1;
    tsf_stage = 1;
    while (tsf_stage != 2) sleep_ms(10);
    return NULL;
}

/* A thread with a filter the caller does not have makes TSYNC fail with its
 * tid, and nothing is installed. */
static void c_tsync_fail(void)
{
    nnp();
    pthread_t t;
    if (pthread_create(&t, NULL, tsf_thread, NULL) != 0) cfail("pthread_create");
    while (tsf_stage != 1) sleep_ms(10);
    if (tsf_tid <= 0) cfail("thread could not install its filter");
    long r = install_one(SYS_getppid, SECCOMP_RET_ERRNO | 9, SECCOMP_FILTER_FLAG_TSYNC);
    tsf_stage = 2;
    pthread_join(t, NULL);
    if (r != tsf_tid) cfail("TSYNC returned %ld, want the other thread's tid %ld", r, tsf_tid);
    if (syscall(SYS_getppid) <= 0) cfail("the failed TSYNC filter was installed");
}

/* ── inheritance ─────────────────────────────────────────────────────────── */

static const char *self_path;

static void c_inherit(void)
{
    nnp();
    if (install_one(SYS_getppid, SECCOMP_RET_ERRNO | 61, 0) != 0)
        cfail("install: %s", strerror(errno));
    /* fork */
    pid_t pid = fork();
    if (pid == 0) {
        errno = 0;
        _exit(syscall(SYS_getppid) == -1 && errno == 61 ? 0 : 1);
    }
    int st;
    waitpid(pid, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) cfail("forked child not filtered");
    /* execve */
    fflush(stdout);
    pid = fork();
    if (pid == 0) {
        execl(self_path, self_path, "--exec-check", (char *)NULL);
        _exit(98);
    }
    waitpid(pid, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
        cfail("exec'd image not filtered (status %#x)", st);
}

static int exec_check(void)
{
    errno = 0;
    if (syscall(SYS_getppid) != -1 || errno != 61) return 1;
    if (prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) != 1) return 2;
    if (prctl(PR_GET_SECCOMP, 0, 0, 0, 0) != 2) return 3;
    /* /proc/self/status says so too. */
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return 4;
    char line[128];
    int seen = 0;
    while (fgets(line, sizeof line, f)) {
        if (!strcmp(line, "NoNewPrivs:\t1\n")) seen |= 1;
        if (!strcmp(line, "Seccomp:\t2\n")) seen |= 2;
    }
    fclose(f);
    return seen == 3 ? 0 : 5;
}

/* ── no_new_privs and set-uid ────────────────────────────────────────────── */

#define SUID_COPY "/p45suid"
static char suid_path[256];

static int report_euid(void)
{
    printf("euid=%u\n", (unsigned)geteuid());
    fflush(stdout);
    return (int)(geteuid() & 0xff);
}

/* Exec the set-uid copy as uid 1000, with or without no_new_privs; return
 * the euid it saw (low byte, via the exit status). */
static int run_suid(int with_nnp)
{
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        if (setresgid(1000, 1000, 1000) || setresuid(1000, 1000, 1000)) _exit(250);
        if (with_nnp) nnp();
        execl(suid_path, suid_path, "--euid", (char *)NULL);
        _exit(251);
    }
    int st;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 252;
}

static void check_setuid(void)
{
    if (geteuid() != 0) {
        probe_info("set-uid check skipped: needs root to create a set-uid file");
        return;
    }
    const char *dirs[] = { "/tmp", ".", NULL };
    for (int i = 0; dirs[i]; i++) {
        snprintf(suid_path, sizeof suid_path, "%s%s", dirs[i], SUID_COPY);
        unlink(suid_path);
        int in = open(self_path, O_RDONLY), out = open(suid_path, O_CREAT | O_WRONLY | O_TRUNC, 0755);
        if (in < 0 || out < 0) probe_fail("copy self: %s", strerror(errno));
        char buf[8192];
        ssize_t n;
        while ((n = read(in, buf, sizeof buf)) > 0)
            if (write(out, buf, n) != n) probe_fail("copy self: %s", strerror(errno));
        close(in);
        close(out);
        if (chown(suid_path, 77, 77) != 0 || chmod(suid_path, 04755) != 0)
            probe_fail("chown/chmod: %s", strerror(errno));
        int plain = run_suid(0);
        if (plain != 77) {
            probe_info("%s: set-uid bit not honoured here (euid %d), next dir", dirs[i], plain);
            unlink(suid_path);
            continue;
        }
        int guarded = run_suid(1);
        unlink(suid_path);
        if (guarded != (1000 & 0xff))
            probe_fail("with no_new_privs the set-uid exec ran with euid&0xff %d, want %d",
                       guarded, 1000 & 0xff);
        probe_info("set-uid: euid 77 without no_new_privs, 1000 with it");
        return;
    }
    probe_info("set-uid check skipped: no directory honours set-uid");
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--exec-check")) return exec_check();
    if (argc > 1 && !strcmp(argv[1], "--euid")) return report_euid();

    probe_watchdog(60);
    static char pbuf[256];
    self_path = probe_self_path(argv[0], pbuf, sizeof pbuf);
    if (!self_path) probe_fail("cannot find own path");

    /* Feature probes exactly as Firefox's SandboxInfo makes them. */
    errno = 0;
    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, NULL) != -1 || errno != EFAULT)
        probe_fail("prctl(PR_SET_SECCOMP, FILTER, NULL) = %s, want EFAULT", strerror(errno));
    errno = 0;
    if (sys_seccomp(SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_TSYNC, NULL) != -1 ||
        errno != EFAULT)
        probe_fail("seccomp(FILTER, TSYNC, NULL) = %s, want EFAULT", strerror(errno));
    unsigned act = SECCOMP_RET_TRAP;
    if (sys_seccomp(SECCOMP_GET_ACTION_AVAIL, 0, &act) != 0)
        probe_fail("GET_ACTION_AVAIL(TRAP): %s", strerror(errno));
    errno = 0;
    if (prctl(0x7fff1234, 0, 0, 0, 0) != -1 || errno != EINVAL)
        probe_fail("prctl(unknown option) = %s, want EINVAL", strerror(errno));

    /* Without no_new_privs only root may install a filter. */
    if (geteuid() != 0) {
        errno = 0;
        if (install_one(SYS_getppid, SECCOMP_RET_ALLOW, 0) != -1 || errno != EACCES)
            probe_fail("filter without no_new_privs: %s, want EACCES", strerror(errno));
    }

    expect_exit0("validation", in_child(c_validation));
    expect_exit0("ALLOW/ERRNO/LOG", in_child(c_errno));
    expect_exit0("TRAP", in_child(c_trap));
    expect_sig("TRAP with SIGSYS blocked", in_child(c_trap_blocked), SIGSYS);
    expect_sig("KILL_PROCESS", in_child(c_kill_process), SIGSYS);
    expect_sig("KILL_PROCESS from a thread", in_child(c_kill_process_thread), SIGSYS);
    expect_sig("KILL_THREAD, one thread", in_child(c_kill_thread_single), SIGSYS);
    expect_exit0("KILL_THREAD, two threads", in_child(c_kill_thread_multi));
    expect_sig("STRICT", in_child(c_strict), SIGKILL);
    expect_exit0("TSYNC", in_child(c_tsync));
    expect_exit0("TSYNC failure", in_child(c_tsync_fail));
    expect_exit0("fork/exec inheritance", in_child(c_inherit));
    check_setuid();
    probe_pass();
}
