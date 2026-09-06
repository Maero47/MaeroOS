#include "vmm.h"
#include "pmm.h"
#include "../kernel/printk.h"

void vmm_alloc_page(uint32_t virt, uint32_t flags) {
    uint32_t phys = pmm_alloc_frame();
    if (!phys) {
        printk("[VMM]  FATAL: OOM in vmm_alloc_page(0x%08x)\n", (unsigned)virt);
        for (;;) __asm__ volatile("hlt");
    }
    vmm_map_page(virt, phys, flags | PAGE_PRESENT);
}
