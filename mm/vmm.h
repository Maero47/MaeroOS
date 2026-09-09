#pragma once
#include <stdint.h>
#include "../arch/i686/mm/paging.h"

/* Architecture-independent virtual memory interface */

/* Map a virtual page to a physical frame with the given flags.
 * Returns 0, or -1 if the page table the mapping needs could not be
 * allocated (see paging_map). */
static inline int vmm_map_page(uint32_t virt, uint32_t phys, uint32_t flags) {
    return paging_map(virt, phys, flags);
}

/* Unmap a virtual page */
static inline void vmm_unmap_page(uint32_t virt) {
    paging_unmap(virt);
}

/* Allocate a physical frame and map it at virt.
 * Returns 0 on success, -1 if physical memory or a page table is exhausted.
 * MUST be checked: this used to halt the machine instead of failing. */
int vmm_alloc_page(uint32_t virt, uint32_t flags) __attribute__((warn_unused_result));
