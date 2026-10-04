#pragma once
#include <stdint.h>
#include <kernel/multiboot.h>

/* A physical address.  Under PAE, RAM above 4 GiB is real memory too; only
 * user pages live there (pmm_alloc_user_frame), so everything the kernel
 * allocates for itself stays a uint32_t. */
typedef uint64_t phys_t;

/* Maximum supported physical frames: 4M frames × 4 KiB = 16 GiB under PAE
 * (legacy paging stops at 4 GiB, PMM_LOW_FRAMES).  The allocator bitmap is
 * sized for this (512 KiB of .bss); the refcounts are sized from detected
 * RAM, on the heap. */
#define PMM_MAX_FRAMES 4194304U
#define PMM_LOW_FRAMES 1048576U    /* frames below 4 GiB */

void     pmm_init(multiboot_info_t *mbi);
/* A frame below 4 GiB (page tables, the PDPT, the heap, DMA, page cache, any
 * frame the kernel keeps a 32-bit address of).  Physical address, 0 on OOM. */
uint32_t pmm_alloc_frame(void);
/* A frame for a private user page: above 4 GiB when there is RAM there (and
 * PAE to map it), else as pmm_alloc_frame.  0 on OOM. */
phys_t   pmm_alloc_user_frame(void);
/* `n` contiguous frames below frame index `limit` (pass 65536 for the
 * kernel's 256 MiB direct map, so phys + KERNEL_VMA addresses them).
 * Physical address of the first, 0 when there is no such run. */
uint32_t pmm_alloc_contig(uint32_t n, uint32_t limit);
void     pmm_free_frame(phys_t phys);
uint32_t pmm_free_frames(void);    /* Number of free frames */
uint32_t pmm_total_frames(void);   /* frame index span, holes included */
uint32_t pmm_ram_frames(void);     /* frames of RAM (what MemTotal reports) */

/* Per-frame reference counting (for COW fork).  pmm_refcount_init allocates
 * the counts once the kernel heap is up (kernel_main, after heap_init). */
void    pmm_refcount_init(void);
void    pmm_frame_incref(phys_t phys);
void    pmm_frame_decref(phys_t phys); /* frees frame when count reaches 0 */
uint16_t pmm_frame_refcount(phys_t phys); /* current refcount (0 if invalid) */
/* A frame whose count reaches PMM_REF_SATURATED is pinned for good (see
 * pmm_frame_incref); user-driven increfs stop at PMM_REF_LIMIT (-ENOMEM). */
#define PMM_REF_SATURATED 65535U
#define PMM_REF_LIMIT     60000U
/* Frames above 4 GiB: total, and how many are free. */
uint32_t pmm_high_frames(void);
uint32_t pmm_high_free_frames(void);

/* Mark a physical range as used (e.g. to reserve multiboot module memory) */
void    pmm_reserve_region(uint32_t phys, uint32_t size);
