/*
 * P9 poll-eintr and SA_RESTART - tests E1, S5.
 *
 * Linux:
 *  - poll() returns -1/EINTR as soon as a signal with a handler arrives
 *    (fs/select.c do_sys_poll returns -ERESTARTNOHAND, which becomes EINTR
 *    whenever a handler runs; man 7 signal lists poll among the calls that
 *    are never restarted, so SA_RESTART makes no difference for poll);
 *  - read() on a pipe with SA_RESTART set is transparently restarted with
 *    its original syscall number (arch/x86/kernel/signal.c, orig_ax, and
 *    kernel/signal.c -ERESTARTSYS handling) and later returns the data;
 *    without SA_RESTART it fails with EINTR.
 *
 * MaeroOS (audit): poll/epoll_wait are interrupted only by SIGKILL
 * (proc/syscall.c:4143-4146, E1); SA_RESTART re-executes int 0x80 with
 * eax == -EINTR, i.e. syscall 0xFFFFFFFC, which returns ENOSYS
 * (proc/signal.c:181-189, S5).
 *
 * Note: the audit's P9 text expects SA_RESTART to restart poll(); Linux does
 * not, so this probe asserts EINTR for poll in both cases and uses read()
 * for the restart check.
 */
#define PROBE_NAME "p09_poll_eintr_restart"
#include "probe.h"
#include <poll.h>

static volatile sig_atomic_t alarms;
static int pfd[2];
static pthread_t main_thread;

static void on_alrm(int s) { (void)s; alarms++; }

struct kick { long sig_ms; long write_ms; };

static void *kicker(void *arg)
{
    struct kick *k = arg;
    sleep_ms(k->sig_ms);
    probe_kill_thread(main_thread, SIGALRM, "kicker");
    if (k->write_ms > 0) {
        sleep_ms(k->write_ms - k->sig_ms);
        if (write(pfd[1], "x", 1) != 1)
            probe_fail("write: %s", strerror(errno));
    }
    return NULL;
}

static void set_handler(int flags)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_alrm;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = flags;
    if (sigaction(SIGALRM, &sa, NULL) != 0)
        probe_fail("sigaction: %s", strerror(errno));
}

static void drain(void)
{
    char c;
    struct pollfd p = { pfd[0], POLLIN, 0 };
    while (poll(&p, 1, 0) == 1)
        if (read(pfd[0], &c, 1) != 1)
            break;
}

int main(void)
{
    probe_watchdog(60);
    main_thread = pthread_self();
    if (pipe(pfd) != 0)
        probe_fail("pipe: %s", strerror(errno));
    pthread_t t;
    struct kick k;
    struct pollfd p;
    double t0, dt;
    int r, e;

    /* E1: poll(-1) interrupted by a handled signal, no SA_RESTART. */
    set_handler(0);
    alarms = 0;
    k.sig_ms = 200; k.write_ms = 0;
    pthread_create(&t, NULL, kicker, &k);
    p.fd = pfd[0]; p.events = POLLIN; p.revents = 0;
    t0 = now_ms();
    r = poll(&p, 1, -1);
    e = errno;
    dt = now_ms() - t0;
    pthread_join(t, NULL);
    probe_info("poll(-1) + SIGALRM at 200 ms: r=%d errno=%s after %.0f ms, handler ran %d",
               r, r < 0 ? strerror(e) : "-", dt, (int)alarms);
    if (r != -1 || e != EINTR)
        probe_fail("poll was not interrupted by a handled signal (r=%d, %s)", r,
                   r < 0 ? strerror(e) : "no error");
    if (dt < 150 || dt > 1500)
        probe_fail("poll returned EINTR after %.0f ms, expected ~200 ms", dt);
    if (alarms != 1)
        probe_fail("SIGALRM handler ran %d times, expected 1", (int)alarms);

    /* poll with SA_RESTART: still EINTR on Linux. */
    set_handler(SA_RESTART);
    alarms = 0;
    k.sig_ms = 200; k.write_ms = 600;
    pthread_create(&t, NULL, kicker, &k);
    p.revents = 0;
    t0 = now_ms();
    r = poll(&p, 1, -1);
    e = errno;
    dt = now_ms() - t0;
    pthread_join(t, NULL);
    probe_info("poll(-1) + SA_RESTART SIGALRM at 200 ms, byte at 600 ms: r=%d errno=%s after %.0f ms",
               r, r < 0 ? strerror(e) : "-", dt);
    if (r != -1 || e != EINTR)
        probe_fail("poll with SA_RESTART returned %d (%s) after %.0f ms, Linux returns EINTR",
                   r, r < 0 ? strerror(e) : "no error", dt);
    drain();

    /* S5: read() with SA_RESTART is restarted and returns the later byte. */
    set_handler(SA_RESTART);
    alarms = 0;
    k.sig_ms = 200; k.write_ms = 600;
    pthread_create(&t, NULL, kicker, &k);
    char c = 0;
    t0 = now_ms();
    r = (int)read(pfd[0], &c, 1);
    e = errno;
    dt = now_ms() - t0;
    pthread_join(t, NULL);
    probe_info("read(pipe) + SA_RESTART SIGALRM at 200 ms, byte at 600 ms: r=%d errno=%s after %.0f ms, handler ran %d",
               r, r < 0 ? strerror(e) : "-", dt, (int)alarms);
    if (r != 1)
        probe_fail("restarted read returned %d (%s) after %.0f ms, expected 1 byte at ~600 ms",
                   r, r < 0 ? strerror(e) : "-", dt);
    if (alarms != 1)
        probe_fail("handler ran %d times during the restarted read", (int)alarms);
    if (dt < 500)
        probe_fail("restarted read returned after %.0f ms, before the byte was written", dt);

    /* read() without SA_RESTART: EINTR. */
    set_handler(0);
    alarms = 0;
    k.sig_ms = 200; k.write_ms = 600;
    pthread_create(&t, NULL, kicker, &k);
    t0 = now_ms();
    r = (int)read(pfd[0], &c, 1);
    e = errno;
    dt = now_ms() - t0;
    pthread_join(t, NULL);
    probe_info("read(pipe) + plain SIGALRM at 200 ms: r=%d errno=%s after %.0f ms",
               r, r < 0 ? strerror(e) : "-", dt);
    if (r != -1 || e != EINTR)
        probe_fail("read without SA_RESTART returned %d (%s), expected EINTR", r,
                   r < 0 ? strerror(e) : "no error");
    drain();

    probe_pass();
}
