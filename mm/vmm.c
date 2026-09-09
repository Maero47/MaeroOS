#include "vmm.h"
#include "pmm.h"

/*
 * Allocate a frame and map it at `virt`.
 *
 * Returns 0, or -1 if either resource is exhausted: no free physical frame, or
 * no frame for the page table the mapping needs.  It used to print and hlt
 * forever, which turned every allocation a user process could drive into a
 * machine halt.  Callers must check; the frame is released again on the
 * page-table failure path so a partial failure leaks nothing.
 */
int vmm_alloc_page(uint32_t virt, uint32_t flags) {
    uint32_t phys = pmm_alloc_frame();
    if (!phys) return -1;
    if (vmm_map_page(virt, phys, flags | PAGE_PRESENT) != 0) {
        pmm_free_frame(phys);
        return -1;
    }
    return 0;
}
