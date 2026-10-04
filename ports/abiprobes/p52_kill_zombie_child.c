/*
 * P52 SIGKILL to a process with an unreaped (zombie) child.
 *
 * A process forks a child that exits at once and is never waited for, then
 * sleeps.  kill(pid, SIGKILL) of the parent, and kill(-pgrp, SIGKILL) of a
 * second such parent in its own process group, must return and the parents
 * must die (the zombies are reparented and reaped as usual).
 *
 * MaeroOS before: kill(pid, SIGKILL) also kills the target's descendants,
 * and that walk counted the zombie child (which takes no signal) as progress
 * on every pass, so it spun forever with the big kernel lock held and the
 * whole machine stopped.  Any user could do it to their own processes.
 */
#define PROBE_NAME "p52_kill_zombie_child"
#include "probe.h"
#include <sys/wait.h>

/* A parent with one zombie child; returns the parent's pid once the child
 * has exited. */
static pid_t parent_with_zombie(int own_group)
{
    int p[2];
    if (pipe(p) != 0) probe_fail("pipe");
    pid_t a = fork();
    if (a < 0) probe_fail("fork");
    if (a == 0) {
        if (own_group) setpgid(0, 0);
        pid_t c = fork();
        if (c == 0) _exit(0);
        for (int i = 0; i < 500; i++) {         /* until it is a zombie */
            char path[64], st = 0;
            snprintf(path, sizeof path, "/proc/%d/stat", (int)c);
            FILE *f = fopen(path, "r");
            if (f) {
                int n = fscanf(f, "%*d (%*[^)]) %c", &st);
                fclose(f);
                if (n == 1 && st == 'Z') break;
            }
            usleep(10000);
        }
        char x = 1;
        if (write(p[1], &x, 1) != 1) _exit(2);
        for (;;) sleep(60);
    }
    close(p[1]);
    char x;
    if (read(p[0], &x, 1) != 1) probe_fail("parent %d did not start", (int)a);
    close(p[0]);
    if (own_group) setpgid(a, a);                /* either side may win */
    return a;
}

static void reap(pid_t a, const char *how)
{
    int st;
    if (waitpid(a, &st, 0) != a) probe_fail("%s: waitpid: %s", how, strerror(errno));
    if (!WIFSIGNALED(st) || WTERMSIG(st) != SIGKILL)
        probe_fail("%s: parent status %#x, not killed by SIGKILL", how, st);
}

int main(void)
{
    probe_watchdog(60);
    pid_t a = parent_with_zombie(0);
    if (kill(a, SIGKILL) != 0) probe_fail("kill(%d, SIGKILL): %s", (int)a, strerror(errno));
    reap(a, "kill(pid)");

    pid_t b = parent_with_zombie(1);
    if (kill(-b, SIGKILL) != 0) probe_fail("kill(-%d, SIGKILL): %s", (int)b, strerror(errno));
    reap(b, "kill(-pgrp)");
    probe_pass();
}
