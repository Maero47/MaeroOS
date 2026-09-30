/*
 * timerprobe — alarm(2), setitimer(2)/getitimer(2) and the POSIX timer_*(2)
 * calls must deliver their signals like Linux, and those signals must break a
 * blocking syscall the way any other signal does (signal(7) "Interruption of
 * system calls and library functions by signal handlers").
 *
 *   alarm      returns the seconds left on the previous alarm (rounded)
 *   eintr      alarm(1) interrupts a blocking pipe read: -1/EINTR when the
 *              handler lacks SA_RESTART
 *   restart    the same read is transparently restarted under SA_RESTART
 *   nanosleep  interrupted even under SA_RESTART, with the remainder in *rem
 *   poll       likewise (-ERESTARTNOHAND)
 *   periodic   a 50 ms ITIMER_REAL fires once per period (±1)
 *   getitimer  reports the time remaining, and 0 once disarmed
 *   virtual    ITIMER_VIRTUAL / ITIMER_PROF expire on CPU time
 *   fork       the child starts with no timers; the parent's is untouched
 *   exec       alarm survives execve, POSIX timers do not
 *   posix      timer_create/settime/gettime/getoverrun/delete, TIMER_ABSTIME,
 *              SIGEV_NONE
 *
 * "timerprobe connect" (needs a NIC, so not part of the default run): alarm(1)
 * interrupts a blocking TCP connect to a silent address with EINTR.
 *
 * Prints "timerprobe: <case> ok" per case, "... FAIL ..." on a failure and
 * "timerprobe ok" when every case passed (tools/smoke_toybox.py).
 */
#include "../include/stdio.h"
#include "../include/stdlib.h"
#include "../include/string.h"
#include "../include/errno.h"
#include "../include/unistd.h"
#include "../include/signal.h"
#include "../include/poll.h"
#include "../include/time.h"
#include "../include/sys/time.h"
#include "../include/sys/wait.h"
#include "../include/sys/socket.h"
#include "../include/netinet/in.h"

#define SELF "/timerprobe"

static int failures, case_failures;

static void check(const char *what, int ok) {
    if (!ok) {
        printf("timerprobe: %s FAIL\n", what);
        failures++;
        case_failures++;
    }
}

static void case_done(const char *name) {
    if (!case_failures) printf("timerprobe: %s ok\n", name);
    case_failures = 0;
}

/* Monotonic milliseconds. */
static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static volatile int g_hits;
static volatile int g_pipe_w = -1;

static void on_sig(int sig) { (void)sig; g_hits++; }
static void on_sig_write(int sig) {
    (void)sig;
    g_hits++;
    if (g_pipe_w >= 0) write(g_pipe_w, "x", 1);
}

static void handle(int sig, void (*fn)(int), int flags) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = fn;
    sa.sa_flags = flags;
    sigaction(sig, &sa, 0);
}

static void set_real(long value_ms, long interval_ms) {
    struct itimerval it;
    it.it_value.tv_sec = value_ms / 1000;
    it.it_value.tv_usec = (value_ms % 1000) * 1000;
    it.it_interval.tv_sec = interval_ms / 1000;
    it.it_interval.tv_usec = (interval_ms % 1000) * 1000;
    setitimer(ITIMER_REAL, &it, 0);
}

/* ── alarm return value ─────────────────────────────────────────────────── */
static void alarm_case(void) {
    handle(SIGALRM, on_sig, 0);
    alarm(0);
    check("alarm: no previous alarm -> 0", alarm(5) == 0);
    unsigned r = alarm(3);
    check("alarm: previous 5 s alarm -> 5", r == 5);
    r = alarm(0);
    check("alarm: previous 3 s alarm -> 3", r == 3);
    check("alarm: cancelled alarm -> 0", alarm(0) == 0);
    case_done("alarm");
}

/* ── alarm(1) vs a blocking pipe read ───────────────────────────────────── */
static void eintr_case(void) {
    int p[2];
    if (pipe(p) < 0) { check("eintr: pipe", 0); return; }
    char c;

    /* No SA_RESTART: the read fails with EINTR after about a second, even
     * though the handler has put a byte in the pipe by then. */
    g_hits = 0;
    g_pipe_w = p[1];
    handle(SIGALRM, on_sig_write, 0);
    long t0 = now_ms();
    alarm(1);
    errno = 0;
    int n = read(p[0], &c, 1);
    int err = errno;           /* this libc zeroes errno on every success */
    long dt = now_ms() - t0;
    check("eintr: read returns -1", n == -1);
    check("eintr: errno EINTR", err == EINTR);
    check("eintr: handler ran once", g_hits == 1);
    check("eintr: after ~1 s", dt >= 950 && dt < 1600);
    if (n < 0) read(p[0], &c, 1);          /* drain the handler's byte */
    case_done("eintr");

    /* SA_RESTART: the read is restarted and returns the handler's byte. */
    g_hits = 0;
    handle(SIGALRM, on_sig_write, SA_RESTART);
    t0 = now_ms();
    alarm(1);
    errno = 0;
    c = 0;
    n = read(p[0], &c, 1);
    dt = now_ms() - t0;
    check("restart: read restarted and returned the byte", n == 1 && c == 'x');
    check("restart: handler ran once", g_hits == 1);
    check("restart: after ~1 s", dt >= 950 && dt < 1600);
    case_done("restart");

    /* nanosleep is never restarted once a handler ran (ERESTARTNOHAND),
     * SA_RESTART or not, and reports the time left. */
    g_pipe_w = -1;
    g_hits = 0;
    handle(SIGALRM, on_sig, SA_RESTART);
    set_real(200, 0);
    struct timespec req = { 2, 0 }, rem = { 0, 0 };
    t0 = now_ms();
    errno = 0;
    n = nanosleep(&req, &rem);
    err = errno;
    dt = now_ms() - t0;
    check("nanosleep: -1/EINTR", n == -1 && err == EINTR);
    check("nanosleep: after ~200 ms", dt >= 190 && dt < 700);
    check("nanosleep: remainder ~1.8 s", rem.tv_sec == 1 && rem.tv_nsec > 300000000L);
    case_done("nanosleep");

    /* poll with no timeout on an empty pipe. */
    g_hits = 0;
    set_real(200, 0);
    struct pollfd pfd = { p[0], POLLIN, 0 };
    errno = 0;
    n = poll(&pfd, 1, -1);
    check("poll: -1/EINTR", n == -1 && errno == EINTR);
    check("poll: handler ran", g_hits == 1);
    case_done("poll");

    close(p[0]);
    close(p[1]);
}

/* ── periodic ITIMER_REAL, getitimer ────────────────────────────────────── */
static void periodic_case(void) {
    handle(SIGALRM, on_sig, SA_RESTART);
    g_hits = 0;
    long t0 = now_ms();
    set_real(50, 50);
    while (now_ms() - t0 < 600) { }
    set_real(0, 0);
    int hits = g_hits;
    long dt = now_ms() - t0;
    long expect = dt / 50;
    check("periodic: fires once per 50 ms period (+-1)",
          hits >= expect - 1 && hits <= expect + 1);
    if (hits < expect - 1 || hits > expect + 1)
        printf("timerprobe: periodic: %d expiries in %ld ms\n", hits, dt);
    case_done("periodic");

    struct itimerval cur;
    set_real(2000, 0);
    check("getitimer: call", getitimer(ITIMER_REAL, &cur) == 0);
    long left = cur.it_value.tv_sec * 1000L + cur.it_value.tv_usec / 1000;
    check("getitimer: just armed 2 s -> 1.9..2 s", left > 1900 && left <= 2000);
    check("getitimer: one-shot interval 0",
          cur.it_interval.tv_sec == 0 && cur.it_interval.tv_usec == 0);
    struct timespec pause_ts = { 0, 300000000L };
    nanosleep(&pause_ts, 0);
    getitimer(ITIMER_REAL, &cur);
    left = cur.it_value.tv_sec * 1000L + cur.it_value.tv_usec / 1000;
    check("getitimer: 300 ms later -> ~1.7 s", left > 1550 && left <= 1710);
    struct itimerval old;
    struct itimerval zero;
    memset(&zero, 0, sizeof(zero));
    setitimer(ITIMER_REAL, &zero, &old);
    check("setitimer: old value returned",
          old.it_value.tv_sec == 1 && old.it_value.tv_usec > 500000);
    getitimer(ITIMER_REAL, &cur);
    check("getitimer: disarmed -> 0",
          cur.it_value.tv_sec == 0 && cur.it_value.tv_usec == 0);
    errno = 0;
    check("setitimer: bad which -> EINVAL",
          setitimer(7, &zero, 0) == -1 && errno == EINVAL);
    case_done("getitimer");
}

/* ── ITIMER_VIRTUAL / ITIMER_PROF ───────────────────────────────────────── */
static void cpu_case(int which, int sig, const char *name) {
    handle(sig, on_sig, SA_RESTART);
    g_hits = 0;
    struct itimerval it;
    memset(&it, 0, sizeof(it));
    it.it_value.tv_usec = 100000;          /* 100 ms of CPU time */
    long t0 = now_ms();
    setitimer(which, &it, 0);
    struct itimerval cur;
    getitimer(which, &cur);
    check("cpu timer: armed value readable", cur.it_value.tv_usec > 0);
    while (!g_hits && now_ms() - t0 < 5000) { }
    long dt = now_ms() - t0;
    check("cpu timer: expired while spinning", g_hits == 1);
    check("cpu timer: not before 100 ms", dt >= 90);
    getitimer(which, &cur);
    check("cpu timer: one-shot disarmed after expiry",
          cur.it_value.tv_sec == 0 && cur.it_value.tv_usec == 0);
    case_done(name);
}

/* ── fork clears, exec keeps ────────────────────────────────────────────── */
static int after_exec(void) {
    /* The alarm armed before execve is still pending (handler reset to
     * SIG_DFL, so cancel it before it can kill us). */
    unsigned left = alarm(0);
    struct itimerspec its;
    errno = 0;
    int gone = timer_gettime(0, &its) == -1 && errno == EINVAL;
    if (left < 6 || left > 7) printf("timerprobe: exec: alarm left %u\n", left);
    return (left >= 6 && left <= 7 ? 0 : 1) | (gone ? 0 : 2);
}

static void fork_exec_case(void) {
    handle(SIGALRM, on_sig, 0);
    alarm(10);
    int pid = fork();
    if (pid == 0) {
        struct itimerval cur;
        getitimer(ITIMER_REAL, &cur);
        int ok = cur.it_value.tv_sec == 0 && cur.it_value.tv_usec == 0;
        ok = ok && alarm(0) == 0;
        _exit(ok ? 0 : 1);
    }
    int st = -1;
    check("fork: child starts with no alarm",
          pid > 0 && waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0);
    unsigned r = alarm(0);
    check("fork: parent's alarm untouched", r >= 9 && r <= 10);
    case_done("fork");

    pid = fork();
    if (pid == 0) {
        alarm(7);
        timer_t id;
        struct sigevent ev;
        memset(&ev, 0, sizeof(ev));
        ev.sigev_notify = SIGEV_NONE;
        if (timer_create(CLOCK_MONOTONIC, &ev, &id) < 0 || id != 0) _exit(4);
        char *argv[] = { "timerprobe", "exec-child", 0 };
        char *envp[] = { 0 };
        execve(SELF, argv, envp);
        _exit(8);
    }
    st = -1;
    int got = pid > 0 && waitpid(pid, &st, 0) == pid && WIFEXITED(st);
    int code = got ? WEXITSTATUS(st) : -1;
    check("exec: alarm kept across execve", got && !(code & ~2));
    check("exec: POSIX timer deleted by execve", got && !(code & ~1));
    if (code) printf("timerprobe: exec: child status %d\n", code);
    case_done("exec");
}

/* ── POSIX timers ───────────────────────────────────────────────────────── */
static void posix_case(void) {
    handle(SIGUSR1, on_sig, SA_RESTART);
    struct sigevent ev;
    memset(&ev, 0, sizeof(ev));
    ev.sigev_notify = SIGEV_SIGNAL;
    ev.sigev_signo = SIGUSR1;
    timer_t id = -1;
    check("posix: timer_create", timer_create(CLOCK_MONOTONIC, &ev, &id) == 0 && id >= 0);

    /* Periodic 100 ms. */
    struct itimerspec its, cur;
    memset(&its, 0, sizeof(its));
    its.it_value.tv_nsec = 100000000L;
    its.it_interval.tv_nsec = 100000000L;
    g_hits = 0;
    long t0 = now_ms();
    check("posix: timer_settime", timer_settime(id, 0, &its, 0) == 0);
    check("posix: timer_gettime", timer_gettime(id, &cur) == 0);
    check("posix: gettime interval 100 ms",
          cur.it_interval.tv_sec == 0 && cur.it_interval.tv_nsec == 100000000L);
    check("posix: gettime value <= 100 ms",
          cur.it_value.tv_sec == 0 && cur.it_value.tv_nsec > 0 &&
          cur.it_value.tv_nsec <= 100000000L);
    while (now_ms() - t0 < 550) { }
    memset(&its, 0, sizeof(its));
    timer_settime(id, 0, &its, 0);
    long dt = now_ms() - t0;
    int hits = g_hits;
    check("posix: periodic rate (+-1)", hits >= dt / 100 - 1 && hits <= dt / 100 + 1);
    if (hits < dt / 100 - 1 || hits > dt / 100 + 1)
        printf("timerprobe: posix: %d expiries in %ld ms\n", hits, dt);
    timer_gettime(id, &cur);
    check("posix: disarmed -> 0", cur.it_value.tv_sec == 0 && cur.it_value.tv_nsec == 0);

    /* Overruns: with the signal blocked, expiries after the first one are
     * counted, not queued; the count is handed over at delivery. */
    sigset_t set, old;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    sigprocmask(SIG_BLOCK, &set, &old);
    its.it_value.tv_nsec = 20000000L;
    its.it_interval.tv_nsec = 20000000L;
    g_hits = 0;
    timer_settime(id, 0, &its, 0);
    t0 = now_ms();
    while (now_ms() - t0 < 250) { }
    sigprocmask(SIG_SETMASK, &old, 0);      /* delivers the one queued signal */
    int ov = timer_getoverrun(id);
    check("posix: blocked periodic timer queues one signal", g_hits == 1);
    memset(&its, 0, sizeof(its));
    timer_settime(id, 0, &its, 0);
    check("posix: overruns counted", ov >= 8 && ov <= 13);
    if (ov < 8 || ov > 13) printf("timerprobe: posix: overrun %d\n", ov);
    check("posix: settime resets the overrun count", timer_getoverrun(id) == 0);

    /* TIMER_ABSTIME on CLOCK_REALTIME. */
    timer_t rid = -1;
    check("posix: second timer", timer_create(CLOCK_REALTIME, &ev, &rid) == 0 && rid != id);
    struct timespec rt;
    clock_gettime(CLOCK_REALTIME, &rt);
    rt.tv_nsec += 200000000L;
    if (rt.tv_nsec >= 1000000000L) { rt.tv_sec++; rt.tv_nsec -= 1000000000L; }
    memset(&its, 0, sizeof(its));
    its.it_value = rt;
    g_hits = 0;
    t0 = now_ms();
    check("posix: abstime settime", timer_settime(rid, TIMER_ABSTIME, &its, 0) == 0);
    while (!g_hits && now_ms() - t0 < 3000) { }
    dt = now_ms() - t0;
    check("posix: abstime expiry at ~200 ms", g_hits == 1 && dt >= 150 && dt < 600);

    /* SIGEV_NONE: counts down, never signals. */
    timer_t nid = -1;
    ev.sigev_notify = SIGEV_NONE;
    check("posix: SIGEV_NONE create", timer_create(CLOCK_MONOTONIC, &ev, &nid) == 0);
    memset(&its, 0, sizeof(its));
    its.it_value.tv_nsec = 50000000L;
    g_hits = 0;
    timer_settime(nid, 0, &its, 0);
    t0 = now_ms();
    while (now_ms() - t0 < 150) { }
    timer_gettime(nid, &cur);
    check("posix: SIGEV_NONE expired silently",
          g_hits == 0 && cur.it_value.tv_sec == 0 && cur.it_value.tv_nsec == 0);

    check("posix: timer_delete", timer_delete(id) == 0);
    errno = 0;
    check("posix: deleted id -> EINVAL", timer_gettime(id, &cur) == -1 && errno == EINVAL);
    errno = 0;
    check("posix: bad clock -> EINVAL",
          timer_create(12345, &ev, &id) == -1 && errno == EINVAL);
    timer_delete(rid);
    timer_delete(nid);
    case_done("posix");
}

/* ── connect (manual: make run-net) ─────────────────────────────────────── */
static int connect_case(void) {
    handle(SIGALRM, on_sig, 0);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(9);
    sa.sin_addr.s_addr = htonl(0x0A00024DU);   /* 10.0.2.77: nobody answers */
    g_hits = 0;
    long t0 = now_ms();
    alarm(1);
    int n = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
    int err = errno;
    long dt = now_ms() - t0;
    alarm(0);
    close(fd);
    printf("timerprobe: connect: n=%d errno=%d after %ld ms, handler %d\n", n, err, dt, g_hits);
    int ok = n == -1 && err == EINTR && g_hits == 1 && dt >= 950 && dt < 1600;
    printf(ok ? "timerprobe: connect ok\n" : "timerprobe: connect FAIL\n");
    return ok ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "exec-child") == 0) return after_exec();
    if (argc > 1 && strcmp(argv[1], "connect") == 0) return connect_case();

    alarm_case();
    eintr_case();
    periodic_case();
    cpu_case(ITIMER_VIRTUAL, SIGVTALRM, "virtual");
    cpu_case(ITIMER_PROF, SIGPROF, "prof");
    fork_exec_case();
    posix_case();
    alarm(0);

    if (failures) {
        printf("timerprobe FAILED (%d)\n", failures);
        return 1;
    }
    printf("timerprobe ok\n");
    return 0;
}
