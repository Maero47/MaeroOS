#pragma once
#include <stdint.h>
#include "../arch/i686/mm/paging.h"

/* Architecture-independent virtual memory interface */

/* Map a virtual page to a physical frame with the given flags */
static inline void vmm_map_page(uint32_t virt, uint32_t phys, uint32_t flags) {
    paging_map(virt, phys, flags);
}

/* Unmap a virtual page */
static inline void vmm_unmap_page(uint32_t virt) {
    paging_unmap(virt);
}

/* Allocate a physical frame and map it at virt */
void vmm_alloc_page(uint32_t virt, uint32_t flags);
