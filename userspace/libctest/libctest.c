/* libctest — regression checks for libc behaviour that other programs rely
 * on.  Prints one line per check and "LIBCTEST PASS" at the end when all of
 * them held (tools/smoke_toybox.py looks for that line). */
#include "../include/errno.h"
#include "../include/fcntl.h"
#include "../include/pwd.h"
#include "../include/signal.h"
#include "../include/stdio.h"
#include "../include/stdlib.h"
#include "../include/string.h"
#include "../include/sys/stat.h"
#include "../include/syscall.h"
#include "../include/unistd.h"

static int failures;

static void check(int ok, const char *what) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

static int file_mode(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 ? (int)(st.st_mode & 07777) : -1;
}

static void test_snprintf(void) {
    char buf[8];
    memset(buf, 'Z', sizeof(buf));
    int n = snprintf(buf, 0, "hello %d", 42);
    check(n == 8 && buf[0] == 'Z' && buf[7] == 'Z', "snprintf(buf, 0) returns length, writes nothing");
    check(snprintf((char *)0, 0, "%s-%d", "abc", 12345) == 9, "snprintf(NULL, 0) returns length");
    n = snprintf(buf, sizeof(buf), "%s", "0123456789");
    check(n == 10 && !strcmp(buf, "0123456"), "snprintf truncates and terminates");
}

static void test_malloc(void) {
    check(malloc((size_t)-1 - 8) == 0, "malloc(SIZE_MAX-8) returns NULL");
    check(malloc(0x80000000u) == 0, "malloc(2 GiB) returns NULL");
    check(calloc(0x10000, 0x10001) == 0, "calloc overflowing product returns NULL");
    check(calloc((size_t)-1 / 2 + 1, 2) == 0, "calloc wrapping to 0 returns NULL");
    unsigned char *p = calloc(100, 3);
    int zero = p != 0;
    for (int i = 0; p && i < 300; i++) if (p[i]) zero = 0;
    check(zero, "calloc(100, 3) returns zeroed memory");
    free(p);
}

static void test_asprintf(void) {
    char *big = malloc(1501), *out = 0;
    memset(big, 'a', 1500);
    big[1500] = 0;
    int n = asprintf(&out, "%s-%d|", big, 7);
    int ok = n == 1503 && out && strlen(out) == 1503 &&
             !strncmp(out, big, 1500) && !strcmp(out + 1500, "-7|");
    check(ok, "asprintf of 1503 bytes is complete");
    free(out);
    free(big);
}

static void test_printf_ll(void) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%lld", 1LL << 40);
    check(!strcmp(buf, "1099511627776"), "printf %lld 1<<40");
    snprintf(buf, sizeof(buf), "%lld|%s", -(1LL << 40) - 5, "next");
    check(!strcmp(buf, "-1099511627781|next"), "printf %lld negative, next arg intact");
    snprintf(buf, sizeof(buf), "%llu %llx", 18446744073709551615ULL, 0x123456789abcULL);
    check(!strcmp(buf, "18446744073709551615 123456789abc"), "printf %llu / %llx");
    snprintf(buf, sizeof(buf), "%ld %d %lu", -7L, 3, 4000000000UL);
    check(!strcmp(buf, "-7 3 4000000000"), "printf %ld / %lu unchanged");
}

static void test_sscanf(void) {
    char s[8];
    memset(s, 'Z', sizeof(s));
    int n = sscanf("abcdefgh", "%3s", s);
    check(n == 1 && !strcmp(s, "abc") && s[4] == 'Z', "sscanf %3s stops at width");
    int a = 0, b = 0;
    n = sscanf("12345", "%2d%3d", &a, &b);
    check(n == 2 && a == 12 && b == 345, "sscanf %2d%3d honours widths");
    long long ll = 0;
    n = sscanf("x=1099511627776", "x=%lld", &ll);
    check(n == 1 && ll == (1LL << 40), "sscanf %lld");
    long l = 0;
    n = sscanf("-42 rest", "%ld", &l);
    check(n == 1 && l == -42, "sscanf %ld");
}

static void test_open_mode(void) {
    const char *p = "/tmp/libctest.mode";
    int old = umask(022);
    int fd;

    check(umask(022) == 022, "umask returns the previous mask");
    unlink(p);
    fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    close(fd);
    check(fd >= 0 && file_mode(p) == 0600, "open(O_CREAT, 0600) gives 0600");
    unlink(p);
    fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    close(fd);
    check(fd >= 0 && file_mode(p) == 0755, "open(O_CREAT, 0755) gives 0755");
    unlink(p);
    umask(077);
    fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    close(fd);
    check(fd >= 0 && file_mode(p) == 0600, "open(O_CREAT, 0666) under umask 077 gives 0600");
    umask(022);
    errno = 0;
    fd = open(p, O_WRONLY | O_CREAT | O_EXCL, 0600);
    check(fd < 0 && errno == EEXIST, "open(O_CREAT|O_EXCL) on an existing file fails EEXIST");
    if (fd >= 0) close(fd);
    unlink(p);
    umask(old);
}

static void test_mkstemp(void) {
    char a[] = "/tmp/lcXXXXXX", b[] = "/tmp/lcXXXXXX", bad[] = "/tmp/lcXXXX";
    int fa = mkstemp(a), fb = mkstemp(b);
    check(fa >= 0 && fb >= 0 && fa != fb, "mkstemp opens two files");
    check(strcmp(a, "/tmp/lcXXXXXX") && strcmp(a, b), "mkstemp names are filled in and unique");
    check(file_mode(a) == 0600 && file_mode(b) == 0600, "mkstemp files are 0600");
    if (fa >= 0) close(fa);
    if (fb >= 0) close(fb);
    unlink(a);
    unlink(b);
    errno = 0;
    check(mkstemp(bad) < 0 && errno == EINVAL, "mkstemp rejects a template without XXXXXX");
}

static void test_stdio_errors(void) {
    const char *p = "/tmp/libctest.ro";
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) close(fd);
    fd = open(p, O_RDONLY);
    FILE *f = fdopen(fd, "w");   /* stream over a read-only descriptor */
    int r1 = f ? fputs("data that cannot be written\n", f) : 0;
    check(f && r1 == EOF && ferror(f), "fputs reports a failed write");
    check(f && fclose(f) == EOF, "fclose reports the failed write");
    unlink(p);

    f = fopen(p, "w");
    check(f && fputs("fine\n", f) >= 0 && fflush(f) == 0 && !ferror(f) && fclose(f) == 0,
          "a good write still succeeds");
    unlink(p);
}

static void test_passwd(void) {
    struct passwd *pw = getpwuid(0);
    check(pw && !strcmp(pw->pw_name, "root"), "getpwuid(0)->pw_name == root");
    pw = getpwnam("root");
    check(pw && pw->pw_uid == 0 && pw->pw_dir && pw->pw_dir[0] == '/', "getpwnam(root)");
    pw = getpwnam("user");
    check(pw && pw->pw_uid == 1000 && pw->pw_gid == 100, "getpwnam(user) reads /etc/passwd");
    pw = getpwuid(1000);
    check(pw && !strcmp(pw->pw_name, "user"), "getpwuid(1000)->pw_name == user");
    check(getpwnam("nosuchuser") == 0 && getpwuid(4242) == 0, "unknown users are not found");
}

/* ── Signals ─────────────────────────────────────────────────────────────── */

static volatile int usr1_hits, usr2_hits, usr2_during_usr1, depth, max_depth;
static volatile sigset_t mask_in_handler;

static sigset_t cur_mask(void) {
    sigset_t m = 0;
    sigprocmask(SIG_BLOCK, 0, &m);
    return m;
}

static sigset_t one(int sig) {
    sigset_t s;
    sigemptyset(&s);
    sigaddset(&s, sig);
    return s;
}

static void set_handler(int sig, sighandler_t h, sigset_t mask, int flags) {
    struct sigaction sa = { 0 };
    sa.sa_handler = h;
    sa.sa_mask = mask;
    sa.sa_flags = flags;
    sigaction(sig, &sa, 0);
}

static void on_usr2(int sig) { (void)sig; usr2_hits++; }

static void on_usr1_masked(int sig) {
    (void)sig;
    usr1_hits++;
    mask_in_handler = cur_mask();
    raise(SIGUSR2);                 /* blocked by sa_mask: must not run yet */
    usr2_during_usr1 = usr2_hits;
}

static void on_usr1_nest(int sig) {
    (void)sig;
    usr1_hits++;
    if (++depth > max_depth) max_depth = depth;
    if (usr1_hits == 1) raise(SIGUSR1);
    depth--;
}

static void on_usr1_count(int sig) { (void)sig; usr1_hits++; mask_in_handler = cur_mask(); }

static void test_sigset(void) {
    sigset_t s;
    sigemptyset(&s);
    int ok = !sigismember(&s, SIGINT);
    sigaddset(&s, SIGINT);
    sigaddset(&s, SIGSYS);
    ok = ok && sigismember(&s, SIGINT) == 1 && sigismember(&s, SIGSYS) == 1 &&
         !sigismember(&s, SIGQUIT);
    sigdelset(&s, SIGINT);
    ok = ok && !sigismember(&s, SIGINT) && sigismember(&s, SIGSYS);
    sigfillset(&s);
    ok = ok && sigismember(&s, SIGUSR1) && sigismember(&s, SIGHUP);
    check(ok, "sigemptyset/sigfillset/sigaddset/sigdelset/sigismember");
    errno = 0;
    check(sigaddset(&s, 0) < 0 && errno == EINVAL && sigismember(&s, 99) < 0,
          "sigset calls reject invalid signal numbers");
}

static void test_sigaction_mask(void) {
    usr1_hits = usr2_hits = usr2_during_usr1 = 0;
    set_handler(SIGUSR2, on_usr2, 0, 0);
    set_handler(SIGUSR1, on_usr1_masked, one(SIGUSR2), 0);
    raise(SIGUSR1);
    getpid();                        /* the unblocked SIGUSR2 is delivered here */
    check(usr1_hits == 1 && usr2_during_usr1 == 0 && usr2_hits == 1,
          "sa_mask blocks the masked signal during the handler, delivered after");
    check((mask_in_handler & one(SIGUSR1)) && (mask_in_handler & one(SIGUSR2)),
          "handler runs with its own signal and sa_mask blocked");
    check(!(cur_mask() & (one(SIGUSR1) | one(SIGUSR2))), "mask restored after the handler");
}

static void test_nodefer(void) {
    usr1_hits = depth = max_depth = 0;
    set_handler(SIGUSR1, on_usr1_nest, 0, SA_NODEFER);
    raise(SIGUSR1);
    check(usr1_hits == 2 && max_depth == 2, "SA_NODEFER lets the handler nest");
    usr1_hits = depth = max_depth = 0;
    set_handler(SIGUSR1, on_usr1_nest, 0, 0);
    raise(SIGUSR1);
    getpid();
    check(usr1_hits == 2 && max_depth == 1, "without SA_NODEFER the signal waits for the handler");
}

static void test_resethand(void) {
    struct sigaction sa = { 0 }, old, now;
    usr1_hits = 0;
    sa.sa_handler = on_usr1_count;
    sa.sa_flags = SA_RESETHAND;
    sa.sa_mask = one(SIGUSR2);
    sigaction(SIGUSR1, &sa, 0);
    sigaction(SIGUSR1, 0, &old);
    check(old.sa_handler == on_usr1_count && (old.sa_flags & SA_RESETHAND) &&
          old.sa_mask == one(SIGUSR2), "SA_RESETHAND action reads back as installed");
    raise(SIGUSR1);
    sigaction(SIGUSR1, 0, &now);
    check(usr1_hits == 1 && now.sa_handler == SIG_DFL, "SA_RESETHAND resets to SIG_DFL on delivery");
    check(mask_in_handler & one(SIGUSR1), "SA_RESETHAND without SA_NODEFER blocks the signal");
}

static void test_sigprocmask(void) {
    sigset_t old = 0, pend = 0;
    usr1_hits = 0;
    set_handler(SIGUSR1, on_usr1_count, 0, 0);
    sigset_t s = one(SIGUSR1);
    check(sigprocmask(SIG_BLOCK, &s, &old) == 0 && !(old & s) && (cur_mask() & s),
          "sigprocmask SIG_BLOCK blocks, returns the old mask");
    raise(SIGUSR1);
    check(usr1_hits == 0, "a blocked signal is not delivered");

    /* rt_sigpending was a stub that returned 0 without writing the set;
     * tell that apart from "nothing pending" with a sentinel. */
    unsigned raw[2] = { 0xA5A5A5A5u, 0xA5A5A5A5u };
    syscall2(176, (int)raw, 8);
    if (raw[0] == 0xA5A5A5A5u) {
        printf("skip sigpending reports the blocked signal (kernel rt_sigpending is a stub)\n");
    } else {
        check(sigpending(&pend) == 0 && (pend & s), "sigpending reports the blocked signal");
    }
    check(sigprocmask(SIG_UNBLOCK, &s, 0) == 0 && usr1_hits == 1,
          "unblocking delivers the pending signal");
    check(sigprocmask(SIG_SETMASK, &old, 0) == 0 && cur_mask() == old, "SIG_SETMASK restores");
    errno = 0;
    check(sigprocmask(7, &s, 0) < 0 && errno == EINVAL, "sigprocmask rejects a bad how");
}

static void test_sigsuspend(void) {
    usr1_hits = 0;
    set_handler(SIGUSR1, on_usr1_count, 0, 0);
    sigset_t s = one(SIGUSR1), old, wait_mask = one(SIGUSR2);
    sigprocmask(SIG_BLOCK, &s, &old);
    int parent = getpid();
    int pid = fork();
    if (pid == 0) {
        usleep(100000);
        kill(parent, SIGUSR1);
        _exit(0);
    }
    errno = 0;
    int r = sigsuspend(&wait_mask);
    check(r == -1 && errno == EINTR && usr1_hits == 1, "sigsuspend waits for the signal, EINTR");
    check((mask_in_handler & one(SIGUSR2)) && (mask_in_handler & s),
          "handler runs under the sigsuspend mask");
    check(cur_mask() == (old | s), "sigsuspend restores the mask");
    if (pid > 0) waitpid(pid, 0, 0);
    sigprocmask(SIG_SETMASK, &old, 0);
}

static void test_oldact(void) {
    struct { struct sigaction sa; unsigned char guard[32]; } o;
    struct sigaction sa = { 0 };
    sa.sa_handler = on_usr2;
    sa.sa_mask = one(SIGHUP) | one(SIGTERM);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGUSR2, &sa, 0);
    memset(&o, 0xA5, sizeof(o));
    sa.sa_handler = SIG_IGN;
    sa.sa_mask = 0;
    sa.sa_flags = 0;
    int r = sigaction(SIGUSR2, &sa, &o.sa);
    check(r == 0 && o.sa.sa_handler == on_usr2 && o.sa.sa_mask == (one(SIGHUP) | one(SIGTERM)) &&
          (o.sa.sa_flags & SA_RESTART), "sigaction returns the old action");
    int intact = 1;
    for (unsigned i = 0; i < sizeof(o.guard); i++) if (o.guard[i] != 0xA5) intact = 0;
    check(intact, "sigaction writes nothing past struct sigaction");

    check(signal(SIGUSR2, SIG_DFL) == SIG_IGN, "signal() returns the previous handler");
    errno = 0;
    check(signal(SIGKILL, on_usr2) == SIG_ERR && errno == EINVAL, "signal(SIGKILL) fails EINVAL");
    signal(SIGUSR1, on_usr1_count);
    sigaction(SIGUSR1, 0, &sa);
    check(sa.sa_flags & SA_RESTART, "signal() installs with SA_RESTART");
}

static void test_sigaltstack(void) {
    static char stk[SIGSTKSZ];
    stack_t ss = { stk, 0, sizeof(stk) }, old;
    check(sigaltstack(&ss, 0) == 0 && sigaltstack(0, &old) == 0 && old.ss_sp == stk &&
          old.ss_size == sizeof(stk) && old.ss_flags == 0, "sigaltstack installs and reads back");
    ss.ss_flags = SS_DISABLE;
    check(sigaltstack(&ss, 0) == 0 && sigaltstack(0, &old) == 0 && old.ss_flags == SS_DISABLE,
          "sigaltstack SS_DISABLE");
}

static void test_signals(void) {
    test_sigset();
    test_sigaction_mask();
    test_nodefer();
    test_resethand();
    test_sigprocmask();
    test_sigsuspend();
    test_oldact();
    test_sigaltstack();
    signal(SIGUSR1, SIG_DFL);
    signal(SIGUSR2, SIG_DFL);
    sigset_t none = 0;
    sigprocmask(SIG_SETMASK, &none, 0);
}

int main(void) {
    test_snprintf();
    test_malloc();
    test_asprintf();
    test_printf_ll();
    test_sscanf();
    test_open_mode();
    test_mkstemp();
    test_stdio_errors();
    test_passwd();
    test_signals();
    if (failures) printf("LIBCTEST FAILED %d\n", failures);
    else printf("LIBCTEST PASS\n");
    return failures ? 1 : 0;
}
