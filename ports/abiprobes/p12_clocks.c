/*
 * P12 clocks - tests T1, T2, T4.
 *
 * Linux (kernel/time/posix-timers.c, timekeeping.c, hrtimer.c):
 *  T1 clock_getres(CLOCK_MONOTONIC) is 1 ns and clock_gettime advances
 *     continuously (thousands of distinct values in a 30 ms busy loop);
 *  T2 the ids are distinct clocks: CLOCK_MONOTONIC_COARSE and CLOCK_BOOTTIME
 *     track CLOCK_MONOTONIC (uptime based, decades away from the realtime
 *     epoch), CLOCK_REALTIME_COARSE tracks CLOCK_REALTIME, and the CPU-time
 *     clocks report the small CPU time of this fresh process/thread;
 *  T4 clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, now + 100 ms) returns
 *     0 after ~100 ms, an expired absolute deadline returns at once, and a
 *     nanosleep interrupted by a handled signal returns EINTR with the
 *     remaining time filled in.
 *
 * MaeroOS (audit): all clocks derive from the 100 Hz tick with 10 ms
 * resolution (proc/syscall.c:3340-3375, :4824-4839); every id except 1 and 4
 * returns wall-clock time (:3346-3352); clock_nanosleep ignores clockid and
 * TIMER_ABSTIME (:4723-4728); nanosleep returns 0 early with rem zero
 * (:2276-2302).
 */
#define PROBE_NAME "p12_clocks"
#include "probe.h"

static volatile sig_atomic_t got;
static void on_usr1(int s) { (void)s; got = 1; }
static pthread_t main_thread;

static void *kicker(void *arg)
{
    (void)arg;
    sleep_ms(100);
    pthread_kill(main_thread, SIGUSR1);
    return NULL;
}

static double ts_s(const struct timespec *t) { return t->tv_sec + t->tv_nsec / 1e9; }

int main(void)
{
    probe_watchdog(60);
    main_thread = pthread_self();
    static const char *names[8] = {
        "REALTIME", "MONOTONIC", "PROCESS_CPUTIME_ID", "THREAD_CPUTIME_ID",
        "MONOTONIC_RAW", "REALTIME_COARSE", "MONOTONIC_COARSE", "BOOTTIME" };
    struct timespec res[8], now[8];
    for (int id = 0; id < 8; id++) {
        if (clock_getres(id, &res[id]) != 0)
            probe_fail("clock_getres(%s): %s", names[id], strerror(errno));
        if (clock_gettime(id, &now[id]) != 0)
            probe_fail("clock_gettime(%s): %s", names[id], strerror(errno));
        probe_info("clock %d %-19s res %ld.%09ld now %ld.%09ld", id, names[id],
                   (long)res[id].tv_sec, res[id].tv_nsec, (long)now[id].tv_sec, now[id].tv_nsec);
    }

    /* T1 */
    if (res[CLOCK_MONOTONIC].tv_sec != 0 || res[CLOCK_MONOTONIC].tv_nsec > 1000000L)
        probe_fail("CLOCK_MONOTONIC resolution is %ld.%09ld s, expected <= 1 ms",
                   (long)res[CLOCK_MONOTONIC].tv_sec, res[CLOCK_MONOTONIC].tv_nsec);
    struct timespec prev, cur;
    clock_gettime(CLOCK_MONOTONIC, &prev);
    int distinct = 0, backwards = 0;
    double t0 = now_ms();
    while (now_ms() - t0 < 30) {
        clock_gettime(CLOCK_MONOTONIC, &cur);
        if (cur.tv_sec != prev.tv_sec || cur.tv_nsec != prev.tv_nsec) {
            distinct++;
            if (ts_s(&cur) < ts_s(&prev))
                backwards++;
            prev = cur;
        }
    }
    probe_info("CLOCK_MONOTONIC: %d distinct values in 30 ms, %d backwards steps", distinct, backwards);
    if (backwards)
        probe_fail("CLOCK_MONOTONIC went backwards %d times", backwards);
    if (distinct < 30)
        probe_fail("CLOCK_MONOTONIC produced only %d distinct values in 30 ms (10 ms tick?)", distinct);

    /* T2 */
    double rt = ts_s(&now[CLOCK_REALTIME]), mono = ts_s(&now[CLOCK_MONOTONIC]);
    double coarse = ts_s(&now[CLOCK_MONOTONIC_COARSE]), boot = ts_s(&now[CLOCK_BOOTTIME]);
    double rtc = ts_s(&now[CLOCK_REALTIME_COARSE]);
    const double year = 365.0 * 86400;
    if (rt - mono < year)
        probe_fail("CLOCK_MONOTONIC (%.0f) is not uptime based: within a year of CLOCK_REALTIME (%.0f)", mono, rt);
    if (coarse - mono > 0.1 || mono - coarse > 0.1)
        probe_fail("CLOCK_MONOTONIC_COARSE (%.3f) is %.3f s away from CLOCK_MONOTONIC (%.3f)",
                   coarse, coarse - mono, mono);
    if (rt - boot < year)
        probe_fail("CLOCK_BOOTTIME (%.0f) tracks CLOCK_REALTIME instead of uptime", boot);
    if (boot + 0.1 < mono)
        probe_fail("CLOCK_BOOTTIME (%.3f) is behind CLOCK_MONOTONIC (%.3f)", boot, mono);
    if (rtc - rt > 0.1 || rt - rtc > 0.1)
        probe_fail("CLOCK_REALTIME_COARSE is %.3f s away from CLOCK_REALTIME", rtc - rt);
    if (ts_s(&now[CLOCK_PROCESS_CPUTIME_ID]) > 60 || ts_s(&now[CLOCK_THREAD_CPUTIME_ID]) > 60)
        probe_fail("CPU-time clocks report %.0f / %.0f s for a fresh process (wall clock?)",
                   ts_s(&now[CLOCK_PROCESS_CPUTIME_ID]), ts_s(&now[CLOCK_THREAD_CPUTIME_ID]));
    probe_info("MONOTONIC_COARSE and BOOTTIME track MONOTONIC; REALTIME_COARSE tracks REALTIME");

    /* T4 absolute sleep */
    struct timespec dl;
    clock_gettime(CLOCK_MONOTONIC, &dl);
    dl.tv_nsec += 100000000L;
    if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
    t0 = now_ms();
    int r = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &dl, NULL);
    double dt = now_ms() - t0;
    probe_info("clock_nanosleep(MONOTONIC, ABSTIME, now+100ms) = %d after %.1f ms", r, dt);
    if (r != 0)
        probe_fail("clock_nanosleep(TIMER_ABSTIME) returned %d (%s)", r, strerror(r));
    if (dt < 95 || dt > 1000)
        probe_fail("clock_nanosleep(TIMER_ABSTIME, now+100 ms) took %.1f ms", dt);
    clock_gettime(CLOCK_MONOTONIC, &dl);
    dl.tv_sec -= 1;                         /* already expired */
    t0 = now_ms();
    r = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &dl, NULL);
    dt = now_ms() - t0;
    if (r != 0 || dt > 50)
        probe_fail("clock_nanosleep with an expired deadline returned %d after %.1f ms", r, dt);

    /* T4 interrupted nanosleep */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_usr1;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);
    pthread_t t;
    pthread_create(&t, NULL, kicker, NULL);
    struct timespec req = { 2, 0 }, rem = { 0, 0 };
    t0 = now_ms();
    r = nanosleep(&req, &rem);
    int e = errno;
    dt = now_ms() - t0;
    pthread_join(t, NULL);
    probe_info("nanosleep(2 s) + SIGUSR1 at 100 ms: r=%d errno=%s after %.0f ms, rem %.3f s",
               r, r < 0 ? strerror(e) : "-", dt, ts_s(&rem));
    if (r != -1 || e != EINTR)
        probe_fail("interrupted nanosleep returned %d (%s) after %.0f ms, expected EINTR",
                   r, r < 0 ? strerror(e) : "-", dt);
    if (ts_s(&rem) < 1.0 || ts_s(&rem) > 1.95)
        probe_fail("remaining time after interrupt is %.3f s, expected ~1.9 s", ts_s(&rem));

    /* T4, SA_RESTART variant.  Linux never restarts a sleep once a handler has
     * run: hrtimer_nanosleep returns ERESTART_RESTARTBLOCK, which handle_signal
     * turns into EINTR whatever SA_RESTART says (a restart would resume the
     * REMAINING time through restart_block, never re-sleep the original
     * request).  A kernel that maps its sleep's interrupted return onto the
     * ordinary ERESTARTSYS code re-issues the call with the original duration
     * under any SA_RESTART handler, so the sleep takes ~2 s + the delay
     * instead of ~0.1 s and the remainder just written is thrown away.  An
     * SA_RESTART SIGCHLD or SIGALRM handler is completely ordinary, so this is
     * a routine over-sleep, not a corner case. */
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_usr1;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGUSR1, &sa, NULL);
    got = 0;
    pthread_create(&t, NULL, kicker, NULL);
    req.tv_sec = 2; req.tv_nsec = 0;
    rem.tv_sec = 0; rem.tv_nsec = 0;
    t0 = now_ms();
    r = nanosleep(&req, &rem);
    e = errno;
    dt = now_ms() - t0;
    pthread_join(t, NULL);
    probe_info("nanosleep(2 s) + SA_RESTART SIGUSR1 at 100 ms: r=%d errno=%s after "
               "%.0f ms, rem %.3f s", r, r < 0 ? strerror(e) : "-", dt, ts_s(&rem));
    if (r != -1 || e != EINTR)
        probe_fail("SA_RESTART: interrupted nanosleep returned %d (%s) after %.0f ms, "
                   "expected EINTR — the call was restarted", r,
                   r < 0 ? strerror(e) : "-", dt);
    if (dt > 1500)
        probe_fail("SA_RESTART: nanosleep(2 s) interrupted at 100 ms took %.0f ms — "
                   "it slept the original duration again", dt);
    if (ts_s(&rem) < 1.0 || ts_s(&rem) > 1.95)
        probe_fail("SA_RESTART: remaining time is %.3f s, expected ~1.9 s", ts_s(&rem));
    if (!got)
        probe_fail("SA_RESTART: the handler never ran");

    /* The same for clock_nanosleep(267/407), which shares the implementation. */
    got = 0;
    pthread_create(&t, NULL, kicker, NULL);
    req.tv_sec = 2; req.tv_nsec = 0;
    rem.tv_sec = 0; rem.tv_nsec = 0;
    t0 = now_ms();
    r = clock_nanosleep(CLOCK_MONOTONIC, 0, &req, &rem);
    dt = now_ms() - t0;
    pthread_join(t, NULL);
    probe_info("clock_nanosleep(2 s, relative) + SA_RESTART SIGUSR1 at 100 ms: r=%d "
               "after %.0f ms, rem %.3f s", r, dt, ts_s(&rem));
    if (r != EINTR)
        probe_fail("SA_RESTART: clock_nanosleep returned %d (%s), expected EINTR",
                   r, strerror(r));
    if (dt > 1500)
        probe_fail("SA_RESTART: clock_nanosleep(2 s) interrupted at 100 ms took "
                   "%.0f ms — it slept the original duration again", dt);
    if (ts_s(&rem) < 1.0 || ts_s(&rem) > 1.95)
        probe_fail("SA_RESTART: clock_nanosleep remainder is %.3f s, expected ~1.9 s",
                   ts_s(&rem));

    probe_pass();
}
