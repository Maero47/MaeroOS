/*
 * P47 /proc: the system files and /proc/<pid>, and who may read what.
 *
 * Linux: /proc/self is a link to the caller's pid and /proc/thread-self to
 * "<pid>/task/<tid>"; /proc/<pid>/stat, status, cmdline, comm, statm, task/
 * describe the process (prctl(PR_SET_NAME) shows in comm, a new thread in
 * Threads: and task/); /proc/stat, /proc/loadavg and /proc/uptime have their
 * documented shapes.  Another user's environ, maps, io, fd/ and the
 * exe/cwd/root links need ptrace read access (EACCES), while its stat,
 * status and cmdline stay readable.
 *
 * MaeroOS: /proc/self was a directory, /proc/<pid> held only stat and status,
 * and /proc/stat and /proc/loadavg did not exist.
 */
#define PROBE_NAME "p47_proc_pid"
#include "probe.h"
#include <dirent.h>
#include <sys/prctl.h>
#include <sys/wait.h>

static char text[16384];

static int slurp(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -errno;
    int n = 0, k;
    while (n < (int)sizeof text - 1 && (k = (int)read(fd, text + n, sizeof text - 1 - n)) > 0)
        n += k;
    int e = k < 0 ? errno : 0;
    close(fd);
    text[n] = 0;
    return k < 0 ? -e : n;
}

static int count_dir(const char *path)
{
    DIR *d = opendir(path);
    if (!d) return -errno;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)))
        if (e->d_name[0] != '.') n++;
    closedir(d);
    return n;
}

static volatile int stop;
static char tself[64];

static void *thread_fn(void *arg)
{
    (void)arg;
    ssize_t n = readlink("/proc/thread-self", tself, sizeof tself - 1);
    if (n > 0) tself[n] = 0;
    while (!stop) usleep(10000);
    return NULL;
}

int main(int argc, char **argv)
{
    probe_watchdog(60);
    char want[64], link[256];
    (void)argc;
    snprintf(want, sizeof want, "%d", (int)getpid());
    ssize_t n = readlink("/proc/self", link, sizeof link - 1);
    if (n < 0) probe_fail("readlink /proc/self: %s", strerror(errno));
    link[n] = 0;
    if (strcmp(link, want)) probe_fail("/proc/self -> \"%s\", want \"%s\"", link, want);

    if (slurp("/proc/self/stat") <= 0) probe_fail("/proc/self/stat unreadable");
    int pid, ppid;
    char comm[64], state;
    if (sscanf(text, "%d (%63[^)]) %c %d", &pid, comm, &state, &ppid) != 4 ||
        pid != getpid() || ppid != getppid())
        probe_fail("/proc/self/stat: \"%.60s\"", text);
    /* 52 fields since Linux 3.5. */
    int fields = 0;
    for (char *p = strrchr(text, ')') + 1; *p; p++)
        if (*p == ' ') fields++;
    if (fields < 50) probe_fail("/proc/self/stat has %d fields after comm", fields + 1);

    if (prctl(PR_SET_NAME, "p47renamed") != 0) probe_fail("PR_SET_NAME: %s", strerror(errno));
    if (slurp("/proc/self/comm") <= 0 || strcmp(text, "p47renamed\n"))
        probe_fail("/proc/self/comm after PR_SET_NAME: \"%s\"", text);

    pthread_t t;
    pthread_create(&t, NULL, thread_fn, NULL);
    usleep(100000);
    if (slurp("/proc/self/status") <= 0) probe_fail("/proc/self/status unreadable");
    char uidline[64];
    snprintf(uidline, sizeof uidline, "\nUid:\t%u\t", (unsigned)getuid());
    if (!strstr(text, uidline)) probe_fail("status has no \"%s\"", uidline + 1);
    /* main, the watchdog (probe.h) and thread_fn */
    if (!strstr(text, "\nThreads:\t3\n")) probe_fail("status: Threads is not 3");
    if (!strstr(text, "\nVmRSS:")) probe_fail("status has no VmRSS");
    if (count_dir("/proc/self/task") != 3) probe_fail("task/ has %d entries", count_dir("/proc/self/task"));
    stop = 1;
    pthread_join(t, NULL);
    char wantt[64];
    snprintf(wantt, sizeof wantt, "%d/task/", (int)getpid());
    if (strncmp(tself, wantt, strlen(wantt)) || !strcmp(tself + strlen(wantt), want))
        probe_fail("/proc/thread-self in a thread -> \"%s\"", tself);

    if (slurp("/proc/self/cmdline") <= 0 || strcmp(text, argv[0]))
        probe_fail("cmdline: \"%s\"", text);
    if (slurp("/proc/self/statm") <= 0) probe_fail("statm unreadable");
    unsigned long sz, res;
    if (sscanf(text, "%lu %lu", &sz, &res) != 2 || !sz || !res || res > sz)
        probe_fail("statm: \"%s\"", text);
    if (count_dir("/proc/self/fd") < 3) probe_fail("fd/ lists %d", count_dir("/proc/self/fd"));
    if (slurp("/proc/self/environ") < 0) probe_fail("own environ unreadable");

    if (slurp("/proc/stat") <= 0 || strncmp(text, "cpu ", 4) || !strstr(text, "\ncpu0 ") ||
        !strstr(text, "\nbtime ") || !strstr(text, "\nprocesses ") || !strstr(text, "\nctxt "))
        probe_fail("/proc/stat: \"%.80s\"", text);
    double l1, l5, l15;
    int run, tot;
    if (slurp("/proc/loadavg") <= 0 ||
        sscanf(text, "%lf %lf %lf %d/%d", &l1, &l5, &l15, &run, &tot) != 5 || run < 1 || tot < run)
        probe_fail("/proc/loadavg: \"%s\"", text);
    double up, idle;
    if (slurp("/proc/uptime") <= 0 || sscanf(text, "%lf %lf", &up, &idle) != 2 || up <= 0)
        probe_fail("/proc/uptime: \"%s\"", text);

    /* Another user's process: pid 1 is root's.  As root, look as nobody. */
    pid_t c = 0;
    int in_child = 0;
    if (geteuid() == 0) {
        c = fork();
        if (c > 0) {
            int st;
            waitpid(c, &st, 0);
            if (!WIFEXITED(st)) probe_fail("child died");
            if (WEXITSTATUS(st) == 0) probe_pass();
            if (WEXITSTATUS(st) == 99) probe_fail("setuid(65534) failed");
            probe_fail("as another user, check %d failed (see the list in the source)",
                       WEXITSTATUS(st));
        }
        if (setgid(65534) != 0 || setuid(65534) != 0) _exit(99);
        in_child = 1;
    }
    int step = 0;
#define CHECK(cond) do {                                          \
        step++;                                                   \
        if (!(cond)) {                                            \
            if (in_child) _exit(step);                            \
            probe_fail("check %d: %s", step, #cond);              \
        }                                                         \
    } while (0)
    CHECK(slurp("/proc/1/environ") == -EACCES);                         /* 1 */
    CHECK(readlink("/proc/1/exe", link, sizeof link) < 0 && errno == EACCES);
    CHECK(readlink("/proc/1/cwd", link, sizeof link) < 0 && errno == EACCES);
    CHECK(readlink("/proc/1/root", link, sizeof link) < 0 && errno == EACCES);
    CHECK(count_dir("/proc/1/fd") == -EACCES);                          /* 5 */
    CHECK(slurp("/proc/1/maps") == -EACCES);
    CHECK(slurp("/proc/1/io") == -EACCES);
    CHECK(slurp("/proc/1/stat") > 0);
    CHECK(slurp("/proc/1/status") > 0 && strstr(text, "\nUid:\t0\t"));
    CHECK(slurp("/proc/1/cmdline") >= 0);                               /* 10 */
    CHECK(slurp("/proc/self/environ") >= 0);
    CHECK(readlink("/proc/self/exe", link, sizeof link) > 0);
    if (in_child) _exit(0);
    probe_pass();
}
