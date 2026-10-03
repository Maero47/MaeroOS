/*
 * schedlat: scheduler latency under CPU load.
 *
 *   schedlat pipe  HOGS N   thread A writes a pipe, thread B blocked in read()
 *   schedlat futex HOGS N   thread A FUTEX_WAKEs, thread B blocked in FUTEX_WAIT
 *   schedlat nice            two hogs, one at nice 10, for 2 s: their CPU
 *                           shares (1 CPU: ~9:1 under Linux's weights)
 *   schedlat handoff         SIGKILL a thread spinning in user mode (it dies on
 *                           an IRQ's way out), then check that futex wakes
 *                           still hand the CPU to the woken thread: the
 *                           "handoffs" count in /proc/cputime must keep rising
 *                           (boot with one CPU: an idle CPU takes a wakee
 *                           without a hand-off)
 *   schedlat time WORKLOAD   runs a busybox sh workload, prints its wall time:
 *                           shloop (arithmetic loop), forks (200 fork+exec),
 *                           targz (tar|gzip of /lib /bin /usr), par4 (4 shloops),
 *                           forks4 / targz4 (four forks / targz at once)
 *   schedlat scale KIND P MS P processes in a syscall loop (getpid, pipe,
 *                           stat, mmap) for MS ms: total ops/s
 *   schedlat bkl dump|reset  BKL statistics of a BKLSTAT=1 kernel
 *   schedlat audio HOGS N   a producer sends a buffer every 10 ms (absolute
 *                           clock_nanosleep) through a pipe; the consumer counts
 *                           late deliveries
 *
 * HOGS busy-looping processes run the whole time (fork; killed at the end).
 * Wake latency is rdtsc at the waker's write/wake to rdtsc after the sleeper
 * returns, converted with a TSC rate calibrated against CLOCK_MONOTONIC.
 * Prints median, p90, p99 and max in microseconds.
 *
 * Static musl, so the same binary runs on a Linux host for reference.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <linux/futex.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAXN 4096

static double tsc_per_us;
static int fds[2];
static volatile int futex_word;
static volatile uint64_t t_wake;
static uint64_t lat[MAXN];
static int iters;

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static uint64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void calibrate(void) {
    uint64_t n0 = mono_ns(), c0 = rdtsc();
    struct timespec d = { 0, 200 * 1000 * 1000 };
    nanosleep(&d, NULL);
    uint64_t n1 = mono_ns(), c1 = rdtsc();
    tsc_per_us = (double)(c1 - c0) / ((double)(n1 - n0) / 1000.0);
}

static void sleep_us(long us) {
    struct timespec d = { us / 1000000, (us % 1000000) * 1000 };
    nanosleep(&d, NULL);
}

static int futex(volatile int *uaddr, int op, int val) {
    return (int)syscall(SYS_futex, uaddr, op, val, NULL, NULL, 0);
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static void report(const char *what, uint64_t *v, int n, double scale) {
    qsort(v, n, sizeof(v[0]), cmp_u64);
    double us[4] = { v[n / 2] / scale, v[n * 9 / 10] / scale,
                     v[n * 99 / 100] / scale, v[n - 1] / scale };
    printf("schedlat %s n=%d median=%.1fus p90=%.1fus p99=%.1fus max=%.1fus\n",
           what, n, us[0], us[1], us[2], us[3]);
}

/* ── pipe / futex wake latency ─────────────────────────────────────────── */
static int use_futex;

static void *sleeper(void *arg) {
    (void)arg;
    for (int i = 0; i < iters; i++) {
        if (use_futex) {
            while (futex_word == 0)
                futex(&futex_word, FUTEX_WAIT_PRIVATE, 0);
            futex_word = 0;
        } else {
            char c;
            if (read(fds[0], &c, 1) != 1) break;
        }
        lat[i] = rdtsc() - t_wake;
    }
    return NULL;
}

static void run_wake(void) {
    pthread_t th;
    pthread_create(&th, NULL, sleeper, NULL);
    for (int i = 0; i < iters; i++) {
        /* vary the gap so wakes do not phase-lock with the tick */
        sleep_us(1000 + (i * 7919) % 3000);
        t_wake = rdtsc();
        if (use_futex) {
            futex_word = 1;
            futex(&futex_word, FUTEX_WAKE_PRIVATE, 1);
        } else {
            if (write(fds[1], "x", 1) != 1) break;
        }
    }
    pthread_join(th, NULL);
    report(use_futex ? "futex" : "pipe", lat, iters, tsc_per_us);
}

/* ── audio-like producer/consumer at a 10 ms cadence ───────────────────── */
static uint64_t timer_late[MAXN];

static void *consumer(void *arg) {
    (void)arg;
    for (int i = 0; i < iters; i++) {
        uint64_t sent;
        if (read(fds[0], &sent, sizeof(sent)) != sizeof(sent)) break;
        lat[i] = rdtsc() - sent;
    }
    return NULL;
}

static void run_audio(void) {
    pthread_t th;
    pthread_create(&th, NULL, consumer, NULL);
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    for (int i = 0; i < iters; i++) {
        next.tv_nsec += 10 * 1000 * 1000;
        if (next.tv_nsec >= 1000000000) { next.tv_nsec -= 1000000000; next.tv_sec++; }
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL) == EINTR) {}
        uint64_t now = mono_ns();
        uint64_t due = (uint64_t)next.tv_sec * 1000000000ULL + (uint64_t)next.tv_nsec;
        timer_late[i] = now > due ? now - due : 0;
        uint64_t t = rdtsc();
        if (write(fds[1], &t, sizeof(t)) != sizeof(t)) break;
    }
    pthread_join(th, NULL);
    int late2 = 0, late5 = 0, late10 = 0;
    for (int i = 0; i < iters; i++) {
        double us = lat[i] / tsc_per_us;
        late2 += us > 2000; late5 += us > 5000; late10 += us > 10000;
    }
    int tlate = 0;
    for (int i = 0; i < iters; i++) tlate += timer_late[i] > 10000000ULL;
    printf("schedlat audio deliveries=%d late>2ms=%d late>5ms=%d late>10ms=%d "
           "timer-missed-period=%d\n", iters, late2, late5, late10, tlate);
    report("audio-handoff", lat, iters, tsc_per_us);
    report("audio-timer", timer_late, iters, 1000.0);
}

#define SHLOOP "i=0; while [ $i -lt 100000 ]; do i=$((i+1)); done"
static const char *const workloads[][2] = {
    { "shloop", SHLOOP },
    { "forks",  "i=0; while [ $i -lt 200 ]; do /true; i=$((i+1)); done" },
    { "targz",  "busybox tar cf - /lib /bin /usr | busybox gzip > /tmp/schedlat.gz; busybox rm -f /tmp/schedlat.gz" },
    { "par4",   "for j in 1 2 3 4; do (" SHLOOP ") & done; wait" },
    { "forks4", "for j in 1 2 3 4; do (i=0; while [ $i -lt 100 ]; do /true; i=$((i+1)); done) & done; wait" },
    { "targz4", "for j in 1 2 3 4; do (busybox tar cf - /lib /bin /usr | busybox gzip > /tmp/schedlat$j.gz) & done; wait; busybox rm -f /tmp/schedlat?.gz" },
};

/*
 * schedlat scale KIND PROCS MS: PROCS processes each run one syscall-bound
 * loop for MS milliseconds and the total operations per second is printed.
 * With one Big Kernel Lock the total stops growing with PROCS as soon as the
 * kernel half of the loop saturates the lock (docs/smp-plan.md).
 *   getpid  the cheapest syscall: trap + lock + return
 *   pipe    write 64 bytes into a private pipe and read them back
 *   stat    stat("/bin/busybox"): path walk in the VFS
 *   mmap    mmap + touch + munmap one anonymous page (PMM, page tables, TLB)
 */
static long scale_loop(const char *kind, uint64_t end_ns) {
    long n = 0;
    int p[2] = { -1, -1 };
    char buf[64] = { 0 };
    struct stat st;
    if (!strcmp(kind, "pipe") && pipe(p) < 0) return -1;
    while (mono_ns() < end_ns) {
        for (int k = 0; k < 64; k++) {
            if (!strcmp(kind, "getpid")) syscall(SYS_getpid);
            else if (p[0] >= 0) {
                if (write(p[1], buf, sizeof buf) != sizeof buf ||
                    read(p[0], buf, sizeof buf) != sizeof buf) return -1;
            } else if (!strcmp(kind, "stat")) {
                if (stat("/bin/busybox", &st) < 0) return -1;
            } else {
                char *m = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                if (m == MAP_FAILED) return -1;
                m[0] = 1;
                munmap(m, 4096);
            }
        }
        n += 64;
    }
    return n;
}

static int run_scale(const char *kind, int procs, int ms) {
    int rp[2];
    if (procs < 1) procs = 1;
    if (procs > 32) procs = 32;
    if (pipe(rp) < 0) { perror("pipe"); return 1; }
    uint64_t start = mono_ns() + 50000000ULL;     /* everyone forked by then */
    uint64_t end = start + (uint64_t)ms * 1000000ULL;
    for (int i = 0; i < procs; i++) {
        if (fork() == 0) {
            while (mono_ns() < start) ;
            long n = scale_loop(kind, end);
            write(rp[1], &n, sizeof n);
            _exit(0);
        }
    }
    long total = 0, bad = 0;
    for (int i = 0; i < procs; i++) {
        long n = 0;
        if (read(rp[0], &n, sizeof n) != sizeof n || n < 0) bad++;
        else total += n;
    }
    while (wait(NULL) > 0) ;
    printf("schedlat scale %s procs=%d ops=%ld ops_per_s=%ld%s\n", kind, procs, total,
           (long)(total * 1000.0 / ms), bad ? " (errors)" : "");
    return bad ? 1 : 0;
}

static int run_time(const char *name) {
    const char *script = NULL;
    for (unsigned i = 0; i < sizeof(workloads) / sizeof(workloads[0]); i++)
        if (!strcmp(workloads[i][0], name)) script = workloads[i][1];
    if (!script) { fprintf(stderr, "schedlat: unknown workload %s\n", name); return 2; }
    char *argv[] = { "/bin/busybox", "sh", "-c", (char *)script, NULL };
    uint64_t t0 = mono_ns();
    pid_t pid = fork();
    if (pid == 0) {
        execv(argv[0], argv);
        perror("execv");
        _exit(127);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    printf("schedlat time %s: %.1f ms (status %d)\n", name,
           (mono_ns() - t0) / 1e6, st);
    return 0;
}

static int run_nice(void) {
    volatile uint64_t *cnt = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                                  MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (cnt == MAP_FAILED) { perror("mmap"); return 1; }
    cnt[0] = cnt[1] = 0;
    pid_t pid[2];
    for (int i = 0; i < 2; i++) {
        pid[i] = fork();
        if (pid[i] == 0) {
            if (i == 1 && setpriority(PRIO_PROCESS, 0, 10) < 0) perror("setpriority");
            for (;;) cnt[i]++;
        }
    }
    sleep_us(2000000);
    uint64_t a = cnt[0], b = cnt[1];
    int prio = getpriority(PRIO_PROCESS, pid[1]);
    for (int i = 0; i < 2; i++) kill(pid[i], SIGKILL);
    for (int i = 0; i < 2; i++) waitpid(pid[i], NULL, 0);
    printf("schedlat nice: nice0=%llu nice%d=%llu ratio=%.2f\n",
           (unsigned long long)a, prio, (unsigned long long)b, b ? (double)a / b : 0.0);
    return 0;
}

static unsigned long read_handoffs(void) {
    char buf[256];
    FILE *f = fopen("/proc/cputime", "r");
    unsigned long v = 0;
    if (!f) return 0;
    while (fgets(buf, sizeof(buf), f))
        if (!strncmp(buf, "handoffs ", 9)) v = strtoul(buf + 9, NULL, 10);
    fclose(f);
    return v;
}

static volatile int ack_word;

static void *handoff_waiter(void *arg) {
    (void)arg;
    for (int i = 0; i < iters; i++) {
        while (futex_word == 0) futex(&futex_word, FUTEX_WAIT_PRIVATE, 0);
        futex_word = 0;
        ack_word = 1;
        futex(&ack_word, FUTEX_WAKE_PRIVATE, 1);
    }
    return NULL;
}

static unsigned long handoff_round(void) {
    unsigned long h0 = read_handoffs();
    pthread_t th;
    pthread_create(&th, NULL, handoff_waiter, NULL);
    for (int i = 0; i < iters; i++) {
        sleep_us(500);                    /* let the waiter block again */
        futex_word = 1;
        futex(&futex_word, FUTEX_WAKE_PRIVATE, 1);
        while (ack_word == 0) futex(&ack_word, FUTEX_WAIT_PRIVATE, 0);
        ack_word = 0;
    }
    pthread_join(th, NULL);
    return read_handoffs() - h0;
}

static int run_handoff(void) {
    iters = 50;
    unsigned long before = handoff_round();
    pid_t pid = fork();
    if (pid == 0) { volatile uint32_t x = 0; for (;;) x++; }
    sleep_us(100000);                     /* the spinner is mid-slice in user mode */
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    unsigned long after = handoff_round();
    int ok = before >= (unsigned long)iters / 2 && after >= (unsigned long)iters / 2;
    printf("schedlat handoff: before-kill=%lu after-kill=%lu (of %d wakes) %s\n",
           before, after, iters, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "handoff")) return run_handoff();
    if (argc > 2 && !strcmp(argv[1], "time")) return run_time(argv[2]);
    if (argc > 1 && !strcmp(argv[1], "nice")) return run_nice();
    if (argc > 4 && !strcmp(argv[1], "scale"))
        return run_scale(argv[2], atoi(argv[3]), atoi(argv[4]));
    /* BKL statistics of a `make BKLSTAT=1` kernel: syscall 507 dumps them to
     * the console, 508 resets them (ENOSYS in a normal build). */
    if (argc > 2 && !strcmp(argv[1], "bkl")) {
        if (syscall(!strcmp(argv[2], "reset") ? 508 : 507) < 0) {
            perror("schedlat bkl");
            return 1;
        }
        return 0;
    }
    if (argc < 2) {
        fprintf(stderr, "usage: schedlat pipe|futex|audio [hogs] [iters]\n");
        return 2;
    }
    int hogs = argc > 2 ? atoi(argv[2]) : 1;
    iters = argc > 3 ? atoi(argv[3]) : 500;
    if (iters < 10) iters = 10;
    if (iters > MAXN) iters = MAXN;
    if (pipe(fds) < 0) { perror("pipe"); return 1; }
    calibrate();

    pid_t hog[64];
    if (hogs > 64) hogs = 64;
    for (int i = 0; i < hogs; i++) {
        hog[i] = fork();
        if (hog[i] == 0) {
            volatile uint32_t x = 0;
            for (;;) x++;
        }
    }
    sleep_us(100000);   /* let the hogs get going */

    if (!strcmp(argv[1], "pipe")) run_wake();
    else if (!strcmp(argv[1], "futex")) { use_futex = 1; run_wake(); }
    else if (!strcmp(argv[1], "audio")) run_audio();
    else fprintf(stderr, "schedlat: unknown mode %s\n", argv[1]);

    for (int i = 0; i < hogs; i++) kill(hog[i], SIGKILL);
    for (int i = 0; i < hogs; i++) waitpid(hog[i], NULL, 0);
    printf("schedlat done (hogs=%d)\n", hogs);
    return 0;
}
