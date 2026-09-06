#pragma once
#include <stdint.h>

/*
 * initrd_init — parse a ustar tar image loaded at physical addresses
 * [mod_phys_start, mod_phys_end) and mount its files under vfs_root.
 *
 * The image must lie within the first 4 MiB of physical RAM (boot_page_table1
 * maps that range at KERNEL_VMA, so mod_phys_start + KERNEL_VMA is valid).
 * Call pmm_reserve_region on the module range before paging_init so the PMM
 * does not reclaim those frames.
 */
void initrd_init(uint32_t mod_phys_start, uint32_t mod_phys_end);
