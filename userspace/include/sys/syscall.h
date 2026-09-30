#pragma once
#include <syscall.h>

/* Linux i386 system call numbers, for syscall(2). */
#define __NR_exit              1
#define __NR_fork              2
#define __NR_read              3
#define __NR_write             4
#define __NR_open              5
#define __NR_close             6
#define __NR_getpid            20
#define __NR_kill              37
#define __NR_ioctl             54
#define __NR_setpriority       97
#define __NR_getpriority       96
#define __NR_syslog            103
#define __NR_sysinfo           116
#define __NR_sched_getparam    155
#define __NR_sched_setscheduler 156
#define __NR_sched_getscheduler 157
#define __NR_sched_yield       158
#define __NR_sched_get_priority_max 159
#define __NR_sched_get_priority_min 160
#define __NR_sched_rr_get_interval 161
#define __NR_gettid            224
#define __NR_sched_setaffinity 241
#define __NR_sched_getaffinity 242
#define __NR_ioprio_set        289
#define __NR_ioprio_get        290
#define __NR_getrandom         355
#define __NR_renameat2         353

#define SYS_renameat2          __NR_renameat2
#define SYS_gettid             __NR_gettid
#define SYS_sched_getaffinity  __NR_sched_getaffinity
#define SYS_sched_setaffinity  __NR_sched_setaffinity
#define SYS_sched_getparam     __NR_sched_getparam
#define SYS_sched_setscheduler __NR_sched_setscheduler
#define SYS_sched_getscheduler __NR_sched_getscheduler
#define SYS_sched_get_priority_max __NR_sched_get_priority_max
#define SYS_sched_get_priority_min __NR_sched_get_priority_min
#define SYS_sched_rr_get_interval  __NR_sched_rr_get_interval
#define SYS_ioprio_get         __NR_ioprio_get
#define SYS_ioprio_set         __NR_ioprio_set
