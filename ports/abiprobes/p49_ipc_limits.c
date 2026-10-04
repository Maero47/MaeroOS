/*
 * P49 an unprivileged user cannot pin unbounded kernel memory through
 * inotify or System V IPC, and the system stays usable when one tries.
 *
 * Run as root it does the work as nobody (65534); as another user, as that
 * user.  Eight inotify instances watch one tmpfs directory and are never read
 * while files with 200-byte names are created and removed: more events than
 * max_queued_events.  Every queue must then end in exactly one IN_Q_OVERFLOW
 * (wd -1) after which nothing more is queued, and meanwhile a forked child can
 * still allocate and touch 8 MiB, open files and create an inotify instance.
 * A message queue filled with IPC_NOWAIT must refuse with EAGAIN.  Where
 * kernel.shmall is below 1 GiB (MaeroOS), shmget of 1 MiB segments must stop
 * with ENOSPC before reaching it; on Linux's default (unlimited) shmall that
 * part is skipped.
 *
 * MaeroOS before: inotify queues had no memory bound beyond 16384 events per
 * instance (128 instances of 16384 long-named events exhaust the 256 MiB
 * kernel heap).
 */
#define PROBE_NAME "p49_ipc_limits"
#include "probe.h"
#include <sys/inotify.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define NINST 8

static long read_long(const char *path)
{
    FILE *f = fopen(path, "r");
    long v = -1;
    if (f) { if (fscanf(f, "%ld", &v) != 1) v = -1; fclose(f); }
    return v;
}

/* The system still works: fork, allocate and touch memory, open, inotify. */
static int usable(void)
{
    pid_t c = fork();
    if (c < 0) return 0;
    if (c == 0) {
        char *m = malloc(8 << 20);
        if (!m) _exit(1);
        for (int i = 0; i < (8 << 20); i += 4096) m[i] = (char)i;
        int fd = open("/proc/self/status", O_RDONLY);
        if (fd < 0) _exit(2);
        close(fd);
        int in = inotify_init1(IN_NONBLOCK);
        if (in < 0) _exit(3);
        close(in);
        _exit(0);
    }
    int st;
    if (waitpid(c, &st, 0) != c) return 0;
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

static char buf[1 << 16] __attribute__((aligned(8)));

static int run(void)
{
    long maxq = read_long("/proc/sys/fs/inotify/max_queued_events");
    if (maxq <= 0 || maxq > 100000) maxq = 16384;
    char dir[64], path[300];
    snprintf(dir, sizeof dir, "/tmp/p49.%d", (int)getpid());
    if (mkdir(dir, 0700) != 0) return 10;
    int in[NINST];
    for (int i = 0; i < NINST; i++) {
        in[i] = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (in[i] < 0 || inotify_add_watch(in[i], dir, IN_CREATE) < 0) return 11;
    }
    char name[201];
    memset(name, 'n', 200);
    name[200] = 0;
    for (long k = 0; k < maxq + 64; k++) {
        snprintf(path, sizeof path, "%s/%06ld%s", dir, k, name + 6);
        int fd = open(path, O_CREAT | O_WRONLY, 0600);
        if (fd < 0) return 12;
        close(fd);
        unlink(path);
        if (k == maxq / 2 && !usable()) return 13;   /* while filling up */
    }
    if (!usable()) return 14;                         /* with every queue full */
    for (int i = 0; i < NINST; i++) {
        long events = 0, overflow = 0, after = 0;
        int n;
        while ((n = (int)read(in[i], buf, sizeof buf)) > 0) {
            for (int off = 0; off < n;) {
                struct inotify_event *e = (struct inotify_event *)(buf + off);
                if (e->mask & IN_Q_OVERFLOW) {
                    if (e->wd != -1) return 15;
                    overflow++;
                } else if (overflow) {
                    after++;
                } else {
                    events++;
                }
                off += (int)sizeof(*e) + (int)e->len;
            }
        }
        if (overflow != 1 || after) {
            probe_info("instance %d: %ld events, %ld IN_Q_OVERFLOW, %ld after it",
                       i, events, overflow, after);
            return 16;
        }
        if (i == 0) probe_info("instance 0 held %ld events before IN_Q_OVERFLOW", events);
        close(in[i]);
    }
    rmdir(dir);

    /* A full message queue refuses (EAGAIN), it does not grow. */
    int q = msgget(IPC_PRIVATE, IPC_CREAT | 0600);
    if (q < 0) return 20;
    struct { long type; char text[64]; } m = { 1, "" };
    long sent = 0;
    for (; sent < 1000000; sent++)
        if (msgsnd(q, &m, 0, IPC_NOWAIT) != 0) break;
    int e = errno;
    msgctl(q, IPC_RMID, NULL);
    if (sent >= 1000000 || e != EAGAIN) return 21;
    probe_info("a queue took %ld empty messages", sent);

    /* shm: bounded where shmall is (MaeroOS); Linux's default is not. */
    long shmall = read_long("/proc/sys/kernel/shmall");
    if (shmall > 0 && shmall < (1L << 30) / 4096) {
        int ids[2048], n = 0;
        while (n < 2048) {
            int id = shmget(IPC_PRIVATE, 1 << 20, IPC_CREAT | 0600);
            if (id < 0) break;
            ids[n++] = id;
        }
        int se = errno;
        int ok = usable();
        for (int i = 0; i < n; i++) shmctl(ids[i], IPC_RMID, NULL);
        probe_info("shm: %d MiB of segments before %s", n, strerror(se));
        if (n >= 2048 || (long)n * 256 > shmall || se != ENOSPC) return 22;
        if (!ok) return 23;
    } else {
        probe_info("kernel.shmall is unbounded here: shm cap not checked");
    }
    return 0;
}

int main(void)
{
    probe_watchdog(240);
    if (geteuid() != 0) {
        int r = run();
        if (r) probe_fail("step %d failed", r);
        probe_pass();
    }
    pid_t c = fork();
    if (c == 0) {
        if (setgid(65534) != 0 || setuid(65534) != 0) _exit(99);
        _exit(run());
    }
    int st;
    waitpid(c, &st, 0);
    if (!WIFEXITED(st)) probe_fail("the unprivileged child died (status %#x)", st);
    if (WEXITSTATUS(st) == 99) probe_fail("setuid(65534) failed");
    if (WEXITSTATUS(st)) probe_fail("as nobody, step %d failed", WEXITSTATUS(st));
    if (!usable()) probe_fail("root cannot fork/allocate afterwards");
    probe_pass();
}
