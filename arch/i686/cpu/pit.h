#pragma once
#include <stdint.h>

/* Initialize the PIT channel 0 to fire at `hz` times per second */
void pit_init(uint32_t hz);

/* Returns monotonic tick count since boot */
uint32_t pit_ticks(void);
