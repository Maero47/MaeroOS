#pragma once
#include <stdint.h>

/*
 * Kernel stacks with guard pages.
 *
 * Every stack gets a KSTACK_SLOT_SIZE slot of the KSTACK_REGION window
 * (kernel/config.h): KSTACKSIZE of freshly allocated frames at the top, and
 * unmapped address space below.  Running off the bottom of a stack therefore
 * faults instead of silently overwriting a neighbouring allocation.  A push
 * into the guard cannot be delivered on the same stack, so it becomes a double
 * fault, which runs on its own task (arch/i686/cpu/tss.c) and reports it.
 *
 * The page tables covering the window are created at boot (paging_init), before
 * any process page directory exists, so every address space shares them and a
 * stack mapped here is visible in all of them.
 */

/* Allocate a KSTACKSIZE stack.  Returns its lowest usable address (the stack
 * top is that + KSTACKSIZE), or NULL when the window or physical memory is
 * exhausted.  Must be called with the Big Kernel Lock held once SMP is up. */
void *kstack_alloc(void);

/* Release a stack from kstack_alloc (NULL is ignored).  The stack must not be
 * the one the caller is running on. */
void  kstack_free(void *base);

/* If `addr` lies in the guard area of a slot, return that slot's stack base
 * (the address just above the guard) whether or not the slot is currently in
 * use; otherwise 0.  Safe to call from any context: it only does arithmetic. */
uint32_t kstack_guard_stack(uint32_t addr);

/* Number of stacks currently allocated (diagnostics). */
unsigned kstack_in_use(void);
