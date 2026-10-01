#pragma once
#include <stdint.h>
#include <kernel/multiboot.h>

/* Maximum supported physical frames: 1048576 frames × 4 KiB = 4 GiB (the
 * full 32-bit physical range).  The allocator bitmap is sized for this (128
 * KiB of .bss); the refcounts are sized from detected RAM, on the heap. */
#define PMM_MAX_FRAMES 1048576

void     pmm_init(multiboot_info_t *mbi);
uint32_t pmm_alloc_frame(void);    /* Returns physical address, 0 on OOM */
void     pmm_free_frame(uint32_t phys);
uint32_t pmm_free_frames(void);    /* Number of free frames */
uint32_t pmm_total_frames(void);

/* Per-frame reference counting (for COW fork).  pmm_refcount_init allocates
 * the counts once the kernel heap is up (kernel_main, after heap_init). */
void    pmm_refcount_init(void);
void    pmm_frame_incref(uint32_t phys);
void    pmm_frame_decref(uint32_t phys); /* frees frame when count reaches 0 */
uint16_t pmm_frame_refcount(uint32_t phys); /* current refcount (0 if invalid) */

/* Mark a physical range as used (e.g. to reserve multiboot module memory) */
void    pmm_reserve_region(uint32_t phys, uint32_t size);
