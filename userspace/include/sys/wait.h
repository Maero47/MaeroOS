#pragma once

#define WNOHANG 1

#define WIFEXITED(status)   (((status) & 0x7f) == 0)
#define WEXITSTATUS(status) (((status) >> 8) & 0xff)
#define WIFSIGNALED(status) (((status) & 0x7f) != 0)
#define WTERMSIG(status)    ((status) & 0x7f)

int waitpid(int pid, int *status, int options);
