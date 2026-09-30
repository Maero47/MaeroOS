#pragma once
#include <sys/types.h>

struct sched_param { int sched_priority; };

#define SCHED_OTHER 0
#define SCHED_FIFO  1
#define SCHED_RR    2
#define SCHED_BATCH 3
#define SCHED_IDLE  5

/* One 32-bit word covers every CPU the kernel runs (SMP up to 32). */
typedef struct { unsigned long __bits[1]; } cpu_set_t;
#define CPU_SETSIZE 32
#define CPU_ZERO(s)     ((s)->__bits[0] = 0)
#define CPU_SET(c, s)   ((s)->__bits[0] |= 1UL << (c))
#define CPU_CLR(c, s)   ((s)->__bits[0] &= ~(1UL << (c)))
#define CPU_ISSET(c, s) (!!((s)->__bits[0] & (1UL << (c))))
#define CPU_COUNT(s)    __builtin_popcountl((s)->__bits[0])

int sched_yield(void);
int sched_getaffinity(pid_t pid, size_t size, cpu_set_t *set);
int sched_setaffinity(pid_t pid, size_t size, const cpu_set_t *set);
int sched_get_priority_max(int policy);
int sched_get_priority_min(int policy);
