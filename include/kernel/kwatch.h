#pragma once
#include <stdint.h>

/*
 * kwatch — a progress watchdog that turns a silent wedge into evidence.
 *
 * A hung boot prints nothing, so the usual "read the last line" approach has
 * nothing to read.  kwatch runs off the timer tick, watches one number that
 * always moves on a live system (the syscall counter), and when that number
 * has not moved for KWATCH_STALL_SEC seconds it dumps, once per window:
 *
 *   - how the CPU spent the stalled window (user / idle / kernel milliseconds),
 *     which splits "everything is asleep, someone missed a wake-up" from
 *     "something is spinning" in one line;
 *   - every live process: state, wait channel (decoded to the pipe, AF_UNIX
 *     ring, poll or futex it is actually waiting on), how long it has waited,
 *     its last syscall, and its user EIP/ESP.
 *
 * The per-tick cost is one compare against a counter, so it is left on.
 */

#define KWATCH_STALL_SEC   20     /* silence this long is a stall */

void kwatch_init(void);           /* register the NMI state dump           */
void kwatch_tick(void);           /* every timer tick, from scheduler_tick */
void kwatch_poll(void);           /* emit a detected stall; scheduler loop  */
void kwatch_dump(const char *tag);/* dump every live process's state now */
