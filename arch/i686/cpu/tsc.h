#pragma once
#include <stdint.h>

/*
 * Fine-grained monotonic clock: the 100 Hz PIT tick interpolated with the
 * CPU time-stamp counter (Linux: jiffies for scheduling, a clocksource for
 * clock_gettime).  The tick count stays the scheduler's time base (wake_tick,
 * futex/poll timeouts); the TSC only refines the position inside the current
 * 10 ms tick, so the two never disagree by more than one tick.
 */

#define TICK_HZ   100U
#define TICK_NS   (1000000000U / TICK_HZ)   /* 10 ms */

/* Called from the PIT interrupt handler, once per tick, BEFORE the tick
 * counter is published. */
void tsc_tick_sample(void);

/* Monotonic time since boot as (seconds, nanoseconds), nanoseconds < 1e9. */
void clock_mono(uint32_t *sec, uint32_t *nsec);

/* Monotonic time since boot in nanoseconds (64-bit). */
uint64_t clock_mono_ns(void);

/* Tick index at or after which clock_mono() >= the given monotonic instant.
 * Deadline in (sec, nsec) since boot; saturates on overflow. */
uint32_t clock_mono_to_tick(uint32_t sec, uint32_t nsec);

/* Non-zero once the TSC rate has been measured (a few ticks after boot);
 * before that clock_mono() has plain 10 ms resolution. */
int clock_tsc_calibrated(void);
