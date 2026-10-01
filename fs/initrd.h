#pragma once
#include <stdint.h>

/*
 * initrd_init — parse a ustar tar image loaded at physical addresses
 * [mod_phys_start, mod_phys_end) and mount its files under vfs_root.
 *
 * The image must lie below 256 MiB physical: it is read in place through the
 * higher-half direct map, which paging_init builds only up to
 * HEAP_START - KERNEL_VMA, so mod_phys_start + KERNEL_VMA is valid only there.
 * initrd_init panics on an image that reaches past it.  Call it after
 * paging_init, and reserve the module range with pmm_reserve_region first so
 * the PMM does not reclaim those frames.
 */
void initrd_init(uint32_t mod_phys_start, uint32_t mod_phys_end);

/* The whole image as loaded (kernel virtual), and its size; NULL and 0 when
 * there is none.  devfs serves it read-only as /dev/initrd, which is how
 * maeros-install copies the running system's initrd to a disk. */
const uint8_t *initrd_image(uint32_t *size);
