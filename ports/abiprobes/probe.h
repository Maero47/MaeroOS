/*
 * probe.h - shared helpers for the Linux-ABI probes (docs/audit/
 * firefox-first-paint.md, section 8).
 *
 * Contract: every probe prints exactly one final line
 *     PASS <name>            the observed behaviour matches Linux
 *     FAIL <name>: <detail>  it does not (detail says what was seen)
 *     SKIP <name>: <why>     the check cannot run in this environment
 * Any number of "info <name>: ..." lines may precede it.  The same static
 * binary runs on the Linux host (reference) and on MaeroOS under QEMU.
 *
 * PROBE_NAME must be defined before including this header.
 */
#ifndef ABIPROBE_H
#define ABIPROBE_H

#define _GNU_SOURCE 1
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#ifndef PROBE_NAME
#error "define PROBE_NAME before including probe.h"
#endif

static void probe_pass(void) __attribute__((noreturn, unused));
static void probe_fail(const char *fmt, ...)
    __attribute__((noreturn, unused, format(printf, 1, 2)));
static void probe_skip(const char *fmt, ...)
    __attribute__((noreturn, unused, format(printf, 1, 2)));
static void probe_info(const char *fmt, ...)
    __attribute__((unused, format(printf, 1, 2)));

static void probe_pass(void)
{
    printf("PASS %s\n", PROBE_NAME);
    fflush(stdout);
    _exit(0);
}

static void probe_fail(const char *fmt, ...)
{
    va_list ap;
    printf("FAIL %s: ", PROBE_NAME);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
    _exit(1);
}

static void probe_skip(const char *fmt, ...)
{
    va_list ap;
    printf("SKIP %s: ", PROBE_NAME);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
    _exit(0);
}

static void probe_info(const char *fmt, ...)
{
    va_list ap;
    printf("info %s: ", PROBE_NAME);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

/* Milliseconds on CLOCK_MONOTONIC. */
static double now_ms(void) __attribute__((unused));
static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* Sleep at least ms milliseconds, re-checking the clock: on MaeroOS a
 * nanosleep can return early on any wake-up (audit T4). */
static void sleep_ms(long ms) __attribute__((unused));
static void sleep_ms(long ms)
{
    double deadline = now_ms() + (double)ms;
    for (;;) {
        double left = deadline - now_ms();
        if (left <= 0)
            return;
        struct timespec ts;
        ts.tv_sec = (time_t)(left / 1000.0);
        ts.tv_nsec = (long)((left - ts.tv_sec * 1000.0) * 1e6);
        if (ts.tv_nsec < 0)
            ts.tv_nsec = 0;
        nanosleep(&ts, NULL);
    }
}

/* Watchdog: a helper thread that turns a hung probe into a FAIL line so the
 * QEMU driver always gets its prompt back. */
static long probe_watchdog_secs;

static void *probe_watchdog_thread(void *arg)
{
    (void)arg;
    sleep_ms(probe_watchdog_secs * 1000);
    printf("FAIL %s: watchdog fired after %ld s\n", PROBE_NAME,
           probe_watchdog_secs);
    fflush(stdout);
    _exit(2);
    return NULL;
}

static void probe_watchdog(long secs) __attribute__((unused));
static void probe_watchdog(long secs)
{
    pthread_t t;
    pthread_attr_t attr;
    sigset_t all, old;
    probe_watchdog_secs = secs;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    /* The watchdog blocks every signal so it never becomes the thread that
     * Linux picks for a process-directed signal (the probes reason about
     * which thread receives what). */
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, &old);
    if (pthread_create(&t, &attr, probe_watchdog_thread, NULL) != 0)
        probe_info("watchdog thread not started: %s", strerror(errno));
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    pthread_attr_destroy(&attr);
}

/* Timespec in the layout the raw SYS_futex (240) syscall expects: 32-bit
 * seconds.  musl 1.2 made time_t 64-bit on i386, so its struct timespec is
 * 16 bytes and must not be handed to the old syscall directly (the kernel
 * would read the high half of tv_sec as tv_nsec). */
struct kernel_old_timespec {
    long tv_sec;
    long tv_nsec;
};

/* Absolute path of this executable, for probes that re-exec themselves.
 * argv[0] is used when it carries a directory (the smoke driver and the
 * host runner always invoke probes by path); /proc/self/exe otherwise. */
static const char *probe_self_path(const char *, char *, size_t) __attribute__((unused));
static const char *probe_self_path(const char *argv0, char *buf, size_t len)
{
    if (argv0 && strchr(argv0, '/')) {
        snprintf(buf, len, "%s", argv0);
        return buf;
    }
    ssize_t n = readlink("/proc/self/exe", buf, len - 1);
    if (n > 0) {
        buf[n] = 0;
        return buf;
    }
    return NULL;
}

static inline long raw_gettid(void) __attribute__((unused));
static inline long raw_gettid(void)
{
    return syscall(SYS_gettid);
}

#endif /* ABIPROBE_H */
