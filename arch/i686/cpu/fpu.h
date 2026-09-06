#pragma once
#include <stdint.h>

/* x87/SSE context switching (eager fxsave/fxrstor). Areas must be 512B,
 * 16-byte aligned. */
void fpu_init(void);                       /* boot: CR0/CR4 + capture init state */
void fpu_state_init(uint8_t *area);        /* copy the clean state into area */
void fpu_save(uint8_t *area);
void fpu_restore(const uint8_t *area);
