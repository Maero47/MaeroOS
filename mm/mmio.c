#include "mmio.h"
#include "../arch/i686/mm/paging.h"
#include "../kernel/printk.h"
#include <kernel/config.h>

static uint32_t mmio_next = MMIO_WINDOW_START;

void *mmio_map(uint32_t phys, uint32_t size) {
    uint32_t off = phys & 0xFFFU;
    uint32_t len = (size + off + 0xFFFU) & ~0xFFFU;
    uint32_t base = mmio_next;

    if (!size || len < size || len > MMIO_WINDOW_END - base) {
        printk("[MMIO] window full: 0x%08x+0x%x\n", (unsigned)phys, (unsigned)size);
        return 0;
    }
    if (paging_reserve_range(base, base + len, 0) != 0)
        return 0;
    for (uint32_t p = 0; p < len; p += PAGE_SIZE) {
        /* cannot fail: the tables were reserved above */
        if (paging_map(base + p, (phys & ~0xFFFU) + p,
                       PAGE_PRESENT | PAGE_WRITABLE | PAGE_NOCACHE) != 0)
            return 0;
    }
    /* one unmapped guard page between consecutive mappings */
    mmio_next = base + len + PAGE_SIZE;
    return (void *)(uintptr_t)(base + off);
}
