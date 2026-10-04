/*
 * P49 kernel bookkeeping does not drift or grow with use.
 *
 * Part 1, /proc nodes: looking up /proc/self/fd/N and /proc/self/fdinfo/N for
 * 400 pipe descriptors, then closing them, must leave the kernel's count of
 * cached /proc nodes (/proc/sys/kernel/procfs_nodes, MaeroOS) back near where
 * it started within a few seconds.  Linux has no such count: there the part
 * only checks that the lookups work.
 *
 * Part 2, inotify accounting (root, MaeroOS's /proc/sys/fs/inotify/acct_slots
 * test knob): with two accounting slots, two users take them and a third is
 * charged to the shared spill bucket; after the first two leave, the third
 * user's new instance gets a slot, and reading the old instance's events must
 * bring the spill bucket and the total back to 0, after which the third user
 * can still queue events without IN_Q_OVERFLOW.  Skipped where the knob does
 * not exist (Linux).
 *
 * MaeroOS before: fd/fdinfo nodes stayed cached until the process exited
 * (one user could pin ~100 MiB of kernel heap), and a spilled user who later
 * got a slot uncharged the spill bucket's bytes from that slot, leaving the
 * spill inflated for good.
 */
#define PROBE_NAME "p49_accounting"
#include "probe.h"
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/wait.h>

static long read_long(const char *path)
{
    FILE *f = fopen(path, "r");
    long v = -1;
    if (f) { if (fscanf(f, "%ld", &v) != 1) v = -1; fclose(f); }
    return v;
}

static int write_long(const char *path, long v)
{
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    int r = fprintf(f, "%ld\n", v) > 0 ? 0 : -1;
    if (fclose(f) != 0) r = -1;
    return r;
}

static void part_procfs(void)
{
    long base = read_long("/proc/sys/kernel/procfs_nodes");
    int fds[400], n = 0;
    while (n < 400) {
        int p[2];
        if (pipe(p) != 0) break;
        fds[n++] = p[0];
        fds[n++] = p[1];
    }
    struct stat st;
    char path[64];
    for (int i = 0; i < n; i++) {
        snprintf(path, sizeof path, "/proc/self/fdinfo/%d", fds[i]);
        if (stat(path, &st) != 0) probe_fail("stat %s: %s", path, strerror(errno));
        snprintf(path, sizeof path, "/proc/self/fd/%d", fds[i]);
        if (lstat(path, &st) != 0) probe_fail("lstat %s: %s", path, strerror(errno));
    }
    if (base < 0) {
        for (int i = 0; i < n; i++) close(fds[i]);
        probe_info("no procfs_nodes count here: only the lookups were checked");
        return;
    }
    long peak = read_long("/proc/sys/kernel/procfs_nodes");
    for (int i = 0; i < n; i++) close(fds[i]);
    long now = peak;
    for (int s = 0; s < 6 && now > base + 16; s++) {
        sleep(1);
        stat("/proc/self/status", &st);   /* a lookup runs the sweep */
        now = read_long("/proc/sys/kernel/procfs_nodes");
    }
    probe_info("/proc nodes: %ld before, %ld with %d fds looked up, %ld after closing",
               base, peak, n, now);
    if (peak < base + n) probe_fail("the lookups made only %ld nodes", peak - base);
    if (now > base + 16) probe_fail("%ld nodes still cached after the fds closed", now - base);
}

#define SLOTS  "/proc/sys/fs/inotify/acct_slots"
#define SPILL  "/proc/sys/fs/inotify/spill_bytes"
#define QUEUED "/proc/sys/fs/inotify/queued_bytes"

static char dir[64];

static void make_events(int count)
{
    char path[128];
    for (int i = 0; i < count; i++) {
        snprintf(path, sizeof path, "%s/e%d", dir, i);
        int fd = open(path, O_CREAT | O_WRONLY, 0600);
        if (fd >= 0) close(fd);
        unlink(path);
    }
}

/* A user that holds one instance with queued events until told to go. */
static pid_t holder(int uid, int go[2], int ready[2])
{
    pid_t c = fork();
    if (c == 0) {
        if (setgid(uid) != 0 || setuid(uid) != 0) _exit(99);
        int in = inotify_init1(IN_NONBLOCK);
        if (in < 0 || inotify_add_watch(in, dir, IN_CREATE) < 0) _exit(98);
        char x = 1;
        if (write(ready[1], &x, 1) != 1) _exit(97);
        if (read(go[0], &x, 1) < 0) _exit(96);
        _exit(0);
    }
    return c;
}

static int drain(int in)
{
    static char buf[65536] __attribute__((aligned(8)));
    int events = 0, n;
    while ((n = (int)read(in, buf, sizeof buf)) > 0)
        for (int off = 0; off < n;) {
            struct inotify_event *e = (struct inotify_event *)(buf + off);
            if (e->mask & IN_Q_OVERFLOW) return -1;
            events++;
            off += (int)sizeof(*e) + (int)e->len;
        }
    return events;
}

static void part_inotify(void)
{
    long slots = read_long(SLOTS);
    if (slots < 0) { probe_info("no acct_slots knob here: accounting not checked"); return; }
    if (geteuid() != 0) { probe_info("not root: accounting not checked"); return; }
    snprintf(dir, sizeof dir, "/tmp/p49.%d", (int)getpid());
    if (mkdir(dir, 0777) != 0 || chmod(dir, 0777) != 0) probe_fail("mkdir %s", dir);
    if (write_long(SLOTS, 2) != 0) probe_fail("cannot set %s", SLOTS);

    int go[2], ready[2];
    if (pipe(go) != 0 || pipe(ready) != 0) probe_fail("pipe");
    pid_t a = holder(65531, go, ready), b = holder(65532, go, ready);
    char x;
    if (read(ready[0], &x, 1) != 1 || read(ready[0], &x, 1) != 1) probe_fail("holders");

    /* The third user: spilled, then given a slot once the others leave. */
    int cpipe[2], cgo[2];
    if (pipe(cpipe) != 0 || pipe(cgo) != 0) probe_fail("pipe");
    pid_t c = fork();
    if (c == 0) {
        if (setgid(65533) != 0 || setuid(65533) != 0) _exit(99);
        int old = inotify_init1(IN_NONBLOCK);
        if (old < 0 || inotify_add_watch(old, dir, IN_CREATE) < 0) _exit(98);
        char y = 1;
        if (write(cpipe[1], &y, 1) != 1) _exit(97);          /* armed */
        if (read(cgo[0], &y, 1) != 1) _exit(96);             /* events made, others gone */
        int nw = inotify_init1(IN_NONBLOCK);
        if (nw < 0 || inotify_add_watch(nw, dir, IN_CREATE) < 0) _exit(95);
        if (drain(old) <= 0) _exit(94);
        close(old);
        if (write(cpipe[1], &y, 1) != 1) _exit(93);          /* old one gone */
        if (read(cgo[0], &y, 1) != 1) _exit(92);             /* more events made */
        int got = drain(nw);
        _exit(got == 200 ? 0 : (got < 0 ? 91 : 90));
    }
    if (read(cpipe[0], &x, 1) != 1) probe_fail("third user did not start");
    make_events(100);
    long spill = read_long(SPILL);
    if (spill <= 0) probe_fail("the third user was not charged to the spill bucket (%ld)", spill);
    x = 1;
    if (write(go[1], &x, 1) != 1 || write(go[1], &x, 1) != 1) probe_fail("release");
    waitpid(a, NULL, 0);
    waitpid(b, NULL, 0);
    if (write(cgo[1], &x, 1) != 1 || read(cpipe[0], &x, 1) != 1) probe_fail("third user");
    long spill_after = read_long(SPILL);
    make_events(200);
    if (write(cgo[1], &x, 1) != 1) probe_fail("third user (2)");
    int st;
    waitpid(c, &st, 0);
    long queued = read_long(QUEUED);
    write_long(SLOTS, slots);
    rmdir(dir);
    probe_info("spill bucket %ld while spilled, %ld after reading, total %ld at the end",
               spill, spill_after, queued);
    if (spill_after != 0) probe_fail("spill bucket left at %ld bytes", spill_after);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
        probe_fail("third user: status %#x (91: IN_Q_OVERFLOW, 90: events lost)", st);
    if (queued != 0) probe_fail("%ld queued bytes left with every instance closed", queued);
}

int main(void)
{
    probe_watchdog(120);
    part_procfs();
    part_inotify();
    probe_pass();
}
