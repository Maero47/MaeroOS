/*
 * P21 exec de_thread - tests C3.
 *
 * Linux: execve() from a multithreaded process runs de_thread() (fs/exec.c):
 * every OTHER thread of the group is killed and waited for before the new
 * image starts, so the program that gains control always sees a single
 * thread.  When the caller is not the group leader it takes over the leader's
 * identity (exchange_tids() + release_task(leader)), so the process keeps the
 * pid its parent knows and getpid() == gettid() in the new image.
 *
 * MaeroOS (audit C3): sys_exec replaced the image without touching sibling
 * threads — they kept running against the freed address space — and set
 * tgid = pid, so an exec from a worker thread silently changed the process's
 * pid.
 *
 * The probe forks a child, gives it a sibling thread that increments a
 * counter in a shared memfd page, and has the child execve() this same binary
 * in --helper mode.  The helper re-maps the memfd and reports its pid, its
 * tid and the counter before and after a pause: the counter must be nonzero
 * (the sibling really ran) and must not advance (the sibling is gone), and
 * the pid must still be the pid fork() returned.  Both cases are covered: the
 * exec issued by the group leader, and the exec issued by a worker thread.
 */
#define PROBE_NAME "p21_exec_dethread"
#include "probe.h"
#include <sys/mman.h>
#include <sys/wait.h>

extern char **environ;

static volatile uint32_t *g_counter;
static char  g_self[4096];
static char  g_fdarg[16];
static char *g_av[4];

static void *spinner(void *arg)
{
    (void)arg;
    for (;;) {
        (*g_counter)++;
        sleep_ms(2);
    }
    return NULL;
}

static void *execer(void *arg)
{
    (void)arg;
    while (*g_counter == 0)          /* let the leader prove it is running */
        sleep_ms(2);
    execve(g_self, g_av, environ);
    _exit(9);
    return NULL;
}

/* Child side: become a two-thread process and execve the helper, either from
 * the group leader (exec_from_thread = 0) or from a worker (= 1). */
static void child_run(int fd, int exec_from_thread)
{
    pthread_t t;
    void *m = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED)
        _exit(11);
    g_counter = (volatile uint32_t *)m;
    snprintf(g_fdarg, sizeof g_fdarg, "%d", fd);
    g_av[0] = g_self;
    g_av[1] = (char *)"--helper";
    g_av[2] = g_fdarg;
    g_av[3] = NULL;

    if (exec_from_thread) {
        if (pthread_create(&t, NULL, execer, NULL) != 0)
            _exit(12);
        spinner(NULL);               /* the leader keeps the counter moving */
        _exit(13);
    }
    if (pthread_create(&t, NULL, spinner, NULL) != 0)
        _exit(12);
    while (*g_counter == 0)          /* the sibling is really running */
        sleep_ms(2);
    execve(g_self, g_av, environ);
    _exit(13);
}

static void run_case(int exec_from_thread, const char *what)
{
    /* A fresh memfd per case: the counter must start at zero so "it moved"
     * proves the sibling of THIS process ran. */
    int fd = memfd_create("p21", 0);
    if (fd < 0)
        probe_fail("memfd_create: %s", strerror(errno));
    if (ftruncate(fd, 4096) != 0)
        probe_fail("ftruncate(memfd): %s", strerror(errno));

    int pfd[2];
    if (pipe(pfd) != 0)
        probe_fail("pipe: %s", strerror(errno));
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0)
        probe_fail("fork: %s", strerror(errno));
    if (pid == 0) {
        dup2(pfd[1], 1);
        close(pfd[0]);
        close(pfd[1]);
        child_run(fd, exec_from_thread);
        _exit(14);
    }
    close(pfd[1]);

    char out[256];
    size_t n = 0;
    for (;;) {
        ssize_t r = read(pfd[0], out + n, sizeof out - 1 - n);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0 || n + (size_t)r >= sizeof out - 1)
            break;
        n += (size_t)r;
    }
    out[n] = 0;
    close(pfd[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    for (char *c = out; *c; c++)
        if (*c == '\n')
            *c = ' ';
    probe_info("%s: child %d said '%s' status 0x%x", what, (int)pid, out, st);

    if (WIFSIGNALED(st))
        probe_fail("%s: helper died with signal %d", what, WTERMSIG(st));
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
        probe_fail("%s: helper exit status %d (%s)", what,
                   WIFEXITED(st) ? WEXITSTATUS(st) : -1, out);

    int hpid = -1, htid = -1;
    unsigned first = 0, second = 0;
    if (sscanf(out, "pid=%d tid=%d first=%u second=%u",
               &hpid, &htid, &first, &second) != 4)
        probe_fail("%s: unparsable helper output '%s'", what, out);
    if (first == 0)
        probe_fail("%s: the sibling thread never ran, nothing was tested", what);
    if (second != first)
        probe_fail("%s: counter advanced %u -> %u after execve — a sibling "
                   "thread survived the exec", what, first, second);
    if (hpid != (int)pid)
        probe_fail("%s: new image reports pid %d, expected the process's pid %d",
                   what, hpid, (int)pid);
    if (htid != hpid)
        probe_fail("%s: new image has tid %d != pid %d — it is not the thread-"
                   "group leader", what, htid, hpid);
    close(fd);
}

int main(int argc, char **argv)
{
    if (argc > 2 && strcmp(argv[1], "--helper") == 0) {
        int fd = atoi(argv[2]);
        void *m = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (m == MAP_FAILED) {
            printf("helper-mmap-failed errno=%d\n", errno);
            fflush(stdout);
            return 1;
        }
        volatile uint32_t *c = (volatile uint32_t *)m;
        unsigned first = *c;
        sleep_ms(300);
        unsigned second = *c;
        printf("pid=%d tid=%d first=%u second=%u\n", (int)getpid(),
               (int)raw_gettid(), first, second);
        fflush(stdout);
        return 0;
    }

    if (!probe_self_path(argv[0], g_self, sizeof g_self))
        probe_skip("cannot find own executable path (argv[0]=%s)", argv[0]);
    probe_watchdog(90);

    run_case(0, "execve from the group leader");
    run_case(1, "execve from a worker thread");

    probe_pass();
}
