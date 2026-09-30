#pragma once
#include <sys/types.h>

#define WNOHANG    1
#define WUNTRACED  2
#define WSTOPPED   WUNTRACED
#define WEXITED    4
#define WCONTINUED 8
#define WNOWAIT    0x01000000

#define WEXITSTATUS(status) (((status) >> 8) & 0xff)
#define WTERMSIG(status)    ((status) & 0x7f)
#define WSTOPSIG(status)    WEXITSTATUS(status)
#define WIFEXITED(status)   (WTERMSIG(status) == 0)
#define WIFSTOPPED(status)  (((status) & 0xff) == 0x7f)
#define WIFSIGNALED(status) (((status) & 0xffff) - 1U < 0xffu)
#define WIFCONTINUED(status) ((status) == 0xffff)
#define WCOREDUMP(status)   ((status) & 0x80)

struct rusage;

int waitpid(int pid, int *status, int options);
pid_t wait(int *status);
pid_t wait3(int *status, int options, struct rusage *usage);
pid_t wait4(pid_t pid, int *status, int options, struct rusage *usage);
