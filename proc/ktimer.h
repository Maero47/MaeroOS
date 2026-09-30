#pragma once
#include <registers.h>

/* Signal-delivering interval timers (proc/ktimer.c): alarm, setitimer,
 * getitimer and the POSIX timer_* calls, all per thread group. */

/* From scheduler_tick on every CPU: expire due timers (wall-clock ones on the
 * BSP only) and charge the tick to the running process's CPU-time itimers.
 * IRQ context: only queues signals. */
void ktimer_tick(int user_mode);

/* The last thread of process tgid is gone: drop all its timers. */
void ktimer_group_exit(int tgid);

/* execve: POSIX timers are deleted, the itimers survive (Linux). */
void ktimer_exec(int tgid);

int sys_alarm(registers_t *regs);            /* 27  */
int sys_setitimer(registers_t *regs);        /* 104 */
int sys_getitimer(registers_t *regs);        /* 105 */
int sys_timer_create(registers_t *regs);     /* 259 */
int sys_timer_settime(registers_t *regs);    /* 260 */
int sys_timer_gettime(registers_t *regs);    /* 261 */
int sys_timer_getoverrun(registers_t *regs); /* 262 */
int sys_timer_delete(registers_t *regs);     /* 263 */
int sys_timer_gettime64(registers_t *regs);  /* 408 */
int sys_timer_settime64(registers_t *regs);  /* 409 */
