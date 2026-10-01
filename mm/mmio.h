#pragma once
#include <stdint.h>

/*
 * Kernel MMIO window allocator.
 *
 * Hands out uncached kernel virtual ranges for device registers from one
 * window, MMIO_WINDOW_START..MMIO_WINDOW_END (include/kernel/config.h, which
 * also maps out every other kernel window), instead of every driver picking
 * its own fixed address.
 *
 * Boot-time only: call before proc_init().  pgdir_create() snapshots the
 * kernel PDEs, so a page table created for this window after the first
 * process exists would be invisible to that process.  Never freed.
 */

/* Map [phys, phys+size) uncached; returns the virtual address of `phys`
 * (sub-page offset kept), or 0 when the window is full or out of memory. */
void *mmio_map(uint32_t phys, uint32_t size);
