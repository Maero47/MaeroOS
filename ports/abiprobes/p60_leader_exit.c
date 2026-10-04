/*
 * P60 the thread-group leader exits alone; the process lives on.
 *
 * Linux: a main thread that calls SYS_exit (not exit_group) while other
 * threads run leaves the address space to them (the mm goes when its last
 * user does, mmput).  The leader stays a zombie that waitpid() reports only
 * once the whole group is gone, with the group's exit status.
 *
 * MaeroOS before: proc_exit freed the address space's VMAs with the leader,
 * so a surviving thread that touched a not-yet-faulted page of an earlier
 * mmap took SIGSEGV.
 *
 * The probe forks a child that maps 64 untouched anonymous pages, starts a
 * worker and exits its main thread with SYS_exit.  The worker then fills and
 * reads back every page, maps and uses a fresh region, and tells the parent;
 * the parent checks waitpid(WNOHANG) does not report the child yet, releases
 * the worker, which calls exit_group(7), and requires exit status 7.
 */
#define PROBE_NAME "p60_leader_exit"
#include "probe.h"
#include <sys/mman.h>
#include <sys/wait.h>

#define NPAGES 64

static unsigned char *g_region;
static int g_up[2], g_down[2];

static void *worker(void *arg)
{
    (void)arg;
    sleep_ms(300);                         /* the leader is gone by now */
    long pg = sysconf(_SC_PAGESIZE);
    for (int i = 0; i < NPAGES; i++)
        memset(g_region + i * pg, 0x5a ^ i, (size_t)pg);
    for (int i = 0; i < NPAGES; i++)
        if (g_region[i * pg + pg - 1] != (unsigned char)(0x5a ^ i))
            _exit(3);
    unsigned char *fresh = mmap(NULL, 16 * pg, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (fresh == MAP_FAILED) _exit(4);
    memset(fresh, 1, 16 * pg);
    if (munmap(fresh, 16 * pg) != 0) _exit(5);
    char c = 'L';
    if (write(g_up[1], &c, 1) != 1) _exit(6);
    if (read(g_down[0], &c, 1) != 1) _exit(8);
    syscall(SYS_exit_group, 7);
    return NULL;
}

int main(void)
{
    probe_watchdog(60);
    if (pipe(g_up) || pipe(g_down)) probe_fail("pipe: %s", strerror(errno));
    pid_t pid = fork();
    if (pid < 0) probe_fail("fork: %s", strerror(errno));
    if (pid == 0) {
        long pg = sysconf(_SC_PAGESIZE);
        g_region = mmap(NULL, NPAGES * pg, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (g_region == MAP_FAILED) _exit(10);
        pthread_t t;
        if (pthread_create(&t, NULL, worker, NULL) != 0) _exit(11);
        syscall(SYS_exit, 0);              /* this thread only */
        _exit(12);
    }
    char c;
    ssize_t n = read(g_up[0], &c, 1);
    int st = 0;
    if (n != 1) {
        waitpid(pid, &st, 0);
        if (WIFSIGNALED(st))
            probe_fail("the worker died of signal %d after the leader exited "
                       "(its address space went with the leader)", WTERMSIG(st));
        probe_fail("the worker never reported (status 0x%x)", st);
    }
    pid_t w = waitpid(pid, &st, WNOHANG);
    if (w != 0)
        probe_fail("waitpid(WNOHANG) = %d (status 0x%x) while a thread of the "
                   "child still runs", (int)w, st);
    c = 'G';
    if (write(g_down[1], &c, 1) != 1) probe_fail("write: %s", strerror(errno));
    if (waitpid(pid, &st, 0) != pid) probe_fail("waitpid: %s", strerror(errno));
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 7)
        probe_fail("status 0x%x, want exit 7 (the group's exit_group code)", st);
    probe_pass();
}
