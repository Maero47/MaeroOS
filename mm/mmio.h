#pragma once
#include <stdint.h>

/*
 * Kernel MMIO window allocator.
 *
 * Hands out uncached kernel virtual ranges for device registers from one
 * window, MMIO_WINDOW_START..MMIO_WINDOW_END, instead of every driver picking
 * its own fixed address.  Fixed windows already in use elsewhere:
 *   0xE0000000-0xF0000000 framebuffer     0xF0000000-0xF2000000 kernel stacks
 *   0xF6000000 xHCI   0xF7000000 AHCI    0xFD000000-0xFE000000 ACPI
 *   0xFEE00000 LAPIC  0xFF400000 e1000   0xFFC00000- recursive page tables
 *
 * Boot-time only: call before proc_init().  pgdir_create() snapshots the
 * kernel PDEs, so a page table created for this window after the first
 * process exists would be invisible to that process.  Never freed.
 */
#define MMIO_WINDOW_START 0xF8000000U
#define MMIO_WINDOW_END   0xFC000000U

/* Map [phys, phys+size) uncached; returns the virtual address of `phys`
 * (sub-page offset kept), or 0 when the window is full or out of memory. */
void *mmio_map(uint32_t phys, uint32_t size);
