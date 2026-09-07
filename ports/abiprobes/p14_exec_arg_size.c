/*
 * P14 exec-arg-size - tests C4.
 *
 * Linux: execve() accepts up to MAX_ARG_STRLEN (128 KiB) per string and
 * ARG_MAX (at least 128 KiB, a quarter of the stack limit) in total
 * (fs/exec.c, copy_strings / bprm_stack_limits), returning E2BIG beyond
 * that.  An argv of 70 entries plus 100 environment variables (~6 KiB), or a
 * single 64 KiB argument, arrive intact in the new image.
 *
 * MaeroOS (audit): argv, envp and auxv are packed into the top 4 KiB stack
 * page with no bound check; EXEC_MAXARGS is 64 and EXEC_ARGBUF 8 KiB, extra
 * arguments are silently dropped (proc/syscall.c:1259-1298, :1500-1622).
 *
 * The probe re-executes itself with --helper-args; the helper prints argc,
 * the environment count and the longest env/arg lengths to a pipe.
 */
#define PROBE_NAME "p14_exec_arg_size"
#include "probe.h"
#include <sys/wait.h>

extern char **environ;

static void run_exec(const char *self, char **argv, char **envp, int want_argc, int want_nenv,
                     size_t want_maxenv, size_t want_maxarg, const char *what)
{
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
        execve(self, argv, envp);
        printf("execve-failed errno=%d\n", errno);
        fflush(stdout);
        _exit(9);
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
    probe_info("%s: helper said '%s' status 0x%x", what, out, st);
    if (WIFSIGNALED(st))
        probe_fail("%s: helper died with signal %d", what, WTERMSIG(st));
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
        probe_fail("%s: helper exit status %d (%s)", what,
                   WIFEXITED(st) ? WEXITSTATUS(st) : -1, out);
    int argc = -1, nenv = -1;
    unsigned long maxenv = 0, maxarg = 0;
    if (sscanf(out, "argc=%d nenv=%d maxenv=%lu maxarg=%lu", &argc, &nenv, &maxenv, &maxarg) != 4)
        probe_fail("%s: unparsable helper output '%s'", what, out);
    if (argc != want_argc)
        probe_fail("%s: helper saw argc %d, expected %d", what, argc, want_argc);
    if (nenv != want_nenv)
        probe_fail("%s: helper saw %d environment variables, expected %d", what, nenv, want_nenv);
    if (maxenv != want_maxenv)
        probe_fail("%s: longest env var %lu bytes, expected %lu", what, maxenv, (unsigned long)want_maxenv);
    if (maxarg != want_maxarg)
        probe_fail("%s: longest argument %lu bytes, expected %lu", what, maxarg, (unsigned long)want_maxarg);
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--helper-args") == 0) {
        size_t maxenv = 0, maxarg = 0;
        int nenv = 0;
        for (char **e = environ; *e; e++, nenv++)
            if (strlen(*e) > maxenv)
                maxenv = strlen(*e);
        for (int i = 0; i < argc; i++)
            if (strlen(argv[i]) > maxarg)
                maxarg = strlen(argv[i]);
        printf("argc=%d nenv=%d maxenv=%lu maxarg=%lu\n", argc, nenv,
               (unsigned long)maxenv, (unsigned long)maxarg);
        fflush(stdout);
        return 0;
    }

    char self[4096];
    if (!probe_self_path(argv[0], self, sizeof self))
        probe_skip("cannot find own executable path (argv[0]=%s)", argv[0]);
    probe_watchdog(60);

    /* Case 1: 70 argv entries, 100 env vars of 61 bytes (~6 KiB). */
    static char *av[72];
    static char *ev[101];
    static char evbuf[100][64];
    av[0] = self;
    av[1] = "--helper-args";
    for (int i = 2; i < 70; i++) {
        char *s = malloc(16);
        snprintf(s, 16, "arg%02d", i);
        av[i] = s;
    }
    av[70] = NULL;
    for (int i = 0; i < 100; i++) {
        snprintf(evbuf[i], sizeof evbuf[i], "P14_VAR_%03d=", i);
        size_t l = strlen(evbuf[i]);
        memset(evbuf[i] + l, 'x', 61 - l);
        evbuf[i][61] = 0;
        ev[i] = evbuf[i];
    }
    ev[100] = NULL;
    size_t selflen = strlen(self);
    run_exec(self, av, ev, 70, 100, 61, selflen > 13 ? selflen : 13, "70 args + 6 KiB env");

    /* Case 2: one 64 KiB argument and one 32 KiB variable. */
    char *bigarg = malloc(65536 + 1);
    memset(bigarg, 'y', 65536);
    bigarg[65536] = 0;
    char *bigenv = malloc(32768 + 1);
    memcpy(bigenv, "P14_BIG=", 8);
    memset(bigenv + 8, 'z', 32768 - 8);
    bigenv[32768] = 0;
    char *av2[] = { self, "--helper-args", bigarg, NULL };
    char *ev2[] = { bigenv, NULL };
    run_exec(self, av2, ev2, 3, 1, 32768, 65536, "64 KiB arg + 32 KiB env");

    probe_pass();
}
