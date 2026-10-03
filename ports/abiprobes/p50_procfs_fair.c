/*
 * P50 one user cannot deny /proc to the others.
 *
 * An unprivileged user (the hammer) keeps looking up ~6000 distinct
 * per-process /proc nodes in tight loops: fd/N, fdinfo/N and their
 * task/<tid>/ twins for 500 descriptors in each of three processes, from two
 * of them at once.  Meanwhile root and a second unprivileged user do what ps
 * does for a few seconds: list /proc and read /proc/<pid>/stat and
 * /proc/<pid>/status of every process, and /proc/self/stat and status (root
 * also runs ps itself when there is one).  None of their lookups may fail for
 * a process that still exists.  Where the kernel counts its cached /proc
 * nodes (/proc/sys/kernel/procfs_nodes, MaeroOS) the count must stay within
 * /proc/sys/kernel/procfs_nodes_max throughout, and fall back near where it
 * started within a few seconds of the hammer stopping.
 *
 * Linux: proc inodes and dentries are reclaimable cache, so the lookups just
 * work and there is no count to check.  Needs root (skipped otherwise).
 *
 * MaeroOS before: past 4096 cached nodes a lookup could only evict nodes idle
 * for 3 s, so the hammer kept the whole cache busy and every new lookup in
 * /proc, /proc/<pid> itself included, failed with ENOENT for everyone.
 */
#define PROBE_NAME "p50_procfs_fair"
#include "probe.h"
#include <dirent.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define HAMMER_UID 65528
#define OTHER_UID  65527
#define NFDS       500
#define RUN_SECS   6

static long read_long(const char *path)
{
    FILE *f = fopen(path, "r");
    long v = -1;
    if (f) { if (fscanf(f, "%ld", &v) != 1) v = -1; fclose(f); }
    return v;
}

struct shared {
    volatile int pids[3];
    volatile long rounds[2];     /* hammer passes completed */
    volatile long misses[2];     /* the hammer's own failed lookups */
    volatile long other_bad;     /* the second user's failures */
    volatile char other_what[128];
};
static struct shared *sh;

static int nfds;
static int fdlist[NFDS];

static void hammer_loop(int me)
{
    static const char *const fmt[4] = {
        "/proc/%d/fd/%d", "/proc/%d/fdinfo/%d",
        "/proc/%d/task/%d/fd/%d", "/proc/%d/task/%d/fdinfo/%d",
    };
    char path[64];
    struct stat st;
    for (;;) {
        for (int p = 0; p < 3; p++) {
            int pid = sh->pids[(p + me) % 3];
            for (int k = 0; k < 4; k++)
                for (int i = 0; i < nfds; i++) {
                    if (k < 2) snprintf(path, sizeof path, fmt[k], pid, fdlist[i]);
                    else snprintf(path, sizeof path, fmt[k], pid, pid, fdlist[i]);
                    if (lstat(path, &st) != 0) sh->misses[me]++;
                }
        }
        sh->rounds[me]++;
    }
}

/* The hammer: three processes of one user with NFDS descriptors each, two of
 * them looking up all of their /proc nodes over and over. */
static pid_t start_hammer(void)
{
    pid_t h = fork();
    if (h != 0) return h;
    if (setgid(HAMMER_UID) != 0 || setuid(HAMMER_UID) != 0) _exit(99);
    /* setuid made it non-dumpable: its processes could not see each
     * other's fd/ then. */
    prctl(PR_SET_DUMPABLE, 1);
    while (nfds + 2 <= NFDS) {
        int p[2];
        if (pipe(p) != 0) break;
        fdlist[nfds++] = p[0];
        fdlist[nfds++] = p[1];
    }
    sh->pids[0] = getpid();
    pid_t k1 = fork();
    if (k1 == 0) {
        while (!sh->pids[2]) usleep(1000);
        hammer_loop(1);
    }
    pid_t k2 = fork();
    if (k2 == 0) for (;;) sleep(60);
    sh->pids[1] = k1;
    sh->pids[2] = k2;
    hammer_loop(0);
    return 0;
}

static int exists(int pid)
{
    return kill(pid, 0) == 0 || errno == EPERM;
}

static int read_file(const char *path)
{
    char buf[1024];
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    int n = (int)read(fd, buf, sizeof buf);
    close(fd);
    return n > 0 ? 0 : -1;
}

/* What ps does, once.  Returns the number of failed lookups of live
 * processes; the first is described in what[]. */
static int ps_pass(char *what, size_t wl, long *peak)
{
    int bad = 0;
    const char *self[2] = { "/proc/self/stat", "/proc/self/status" };
    for (int i = 0; i < 2; i++)
        if (read_file(self[i]) != 0 && !bad++)
            snprintf(what, wl, "%s: %s", self[i], strerror(errno));
    DIR *d = opendir("/proc");
    if (!d) {
        if (!bad++) snprintf(what, wl, "opendir /proc: %s", strerror(errno));
        return bad;
    }
    struct dirent *e;
    char path[64];
    while ((e = readdir(d))) {
        int pid = atoi(e->d_name);
        if (pid <= 0) continue;
        static const char *const f[2] = { "stat", "status" };
        for (int i = 0; i < 2; i++) {
            snprintf(path, sizeof path, "/proc/%d/%s", pid, f[i]);
            if (read_file(path) != 0) {
                int err = errno;
                if (exists(pid) && !bad++)
                    snprintf(what, wl, "%s: %s", path, strerror(err));
            }
        }
        if (peak) {
            long n = read_long("/proc/sys/kernel/procfs_nodes");
            if (n > *peak) *peak = n;
        }
    }
    closedir(d);
    return bad;
}

/* Run ps (output discarded); its exit status. */
static int run_ps(const char *ps)
{
    pid_t c = fork();
    if (c == 0) {
        int fd = open("/dev/null", O_WRONLY);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); }
        execl(ps, "ps", (char *)NULL);
        _exit(127);
    }
    int st;
    if (c < 0 || waitpid(c, &st, 0) != c) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(void)
{
    probe_watchdog(150);
    if (geteuid() != 0) probe_skip("needs root (two other users)");
    sh = mmap(NULL, sizeof *sh, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (sh == MAP_FAILED) probe_fail("mmap");
    memset((void *)sh, 0, sizeof *sh);

    long max = read_long("/proc/sys/kernel/procfs_nodes_max");
    long base = read_long("/proc/sys/kernel/procfs_nodes");
    /* ps itself: procps/busybox on Linux, busybox at the initrd root here. */
    const char *ps = access("/bin/ps", X_OK) == 0 ? "/bin/ps"
                   : access("/busybox", X_OK) == 0 ? "/busybox" : NULL;
    int has_ps = ps != NULL;

    pid_t h = start_hammer();
    if (h < 0) probe_fail("fork");
    /* Let the hammer go round twice so it holds as much as it can. */
    double t0 = now_s();
    while (sh->rounds[0] < 2 || sh->rounds[1] < 2) {
        if (now_s() - t0 > 60) {
            kill(h, SIGKILL);
            probe_fail("hammer made no progress (rounds %ld/%ld)", sh->rounds[0], sh->rounds[1]);
        }
        usleep(20000);
    }
    long peak = read_long("/proc/sys/kernel/procfs_nodes");

    /* The second user. */
    pid_t o = fork();
    if (o == 0) {
        if (setgid(OTHER_UID) != 0 || setuid(OTHER_UID) != 0) _exit(99);
        char what[128] = "";
        double end = now_s() + RUN_SECS;
        while (now_s() < end) {
            int b = ps_pass(what, sizeof what, NULL);
            if (b && !sh->other_bad) memcpy((void *)sh->other_what, what, sizeof what);
            sh->other_bad += b;
        }
        _exit(0);
    }

    /* Root. */
    char what[128] = "";
    int bad = 0, passes = 0, ps_bad = 0;
    double end = now_s() + RUN_SECS;
    while (now_s() < end) {
        char w[128] = "";
        int b = ps_pass(w, sizeof w, max > 0 ? &peak : NULL);
        if (b && !bad) memcpy(what, w, sizeof what);
        bad += b;
        passes++;
        if (has_ps && passes % 4 == 1) {
            if (run_ps(ps) != 0) ps_bad++;
        }
    }
    int ost;
    waitpid(o, &ost, 0);
    long hr0 = sh->rounds[0], hr1 = sh->rounds[1];
    long hm = sh->misses[0] + sh->misses[1];
    kill(sh->pids[1], SIGKILL);
    kill(sh->pids[2], SIGKILL);
    kill(h, SIGKILL);
    waitpid(h, NULL, 0);

    long after = base >= 0 ? read_long("/proc/sys/kernel/procfs_nodes") : -1;
    struct stat st;
    for (int s = 0; s < 10 && base >= 0 && after > base + 64; s++) {
        sleep(1);
        stat("/proc/self/status", &st);       /* a lookup runs the sweep */
        after = read_long("/proc/sys/kernel/procfs_nodes");
    }

    probe_info("hammer: %ld+%ld passes over ~%d nodes, %ld of its own lookups refused",
               hr0, hr1, 3 * 4 * NFDS, hm);
    probe_info("root: %d ps passes, %d failed lookups, %d failed ps runs%s; other user: %ld failed",
               passes, bad, ps_bad, has_ps ? "" : " (no ps)", sh->other_bad);
    if (base >= 0)
        probe_info("/proc nodes: %ld before, peak %ld (max %ld), %ld after the hammer stopped",
                   base, peak, max, after);
    if (bad) probe_fail("root's lookups failed %d times, first %s", bad, what);
    if (sh->other_bad)
        probe_fail("the other user's lookups failed %ld times, first %s",
                   sh->other_bad, (const char *)sh->other_what);
    if (!WIFEXITED(ost) || WEXITSTATUS(ost) != 0) probe_fail("other user: status %#x", ost);
    if (ps_bad) probe_fail("ps failed %d times", ps_bad);
    if (max > 0 && peak > max) probe_fail("%ld /proc nodes cached, over the max %ld", peak, max);
    if (base >= 0 && after > base + 64)
        probe_fail("%ld nodes still cached %s after the hammer stopped", after - base,
                   "10 s");
    probe_pass();
}
