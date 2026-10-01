#include "kstack.h"
#include "pmm.h"
#include "vmm.h"
#include "../kernel/printk.h"
#include "../arch/i686/mm/paging.h"
#include "../arch/i686/mm/tlb.h"
#include "../arch/i686/cpu/percpu.h"
#include <kernel/config.h>
#include <stdint.h>
#include <stddef.h>

/*
 * Kernel stack allocator — see kstack.h.
 *
 * Slot i covers [KSTACK_REGION_START + i*KSTACK_SLOT_SIZE, +KSTACK_SLOT_SIZE).
 * The stack is the top KSTACKSIZE of it; the rest is the guard and is never
 * mapped.  Frames are taken from the PMM on alloc and given back on free, so
 * unlike the heap (which never returns pages) a burst of threads does not pin
 * its stacks' memory forever.
 *
 * TLB: freeing unmaps the pages and invalidates them on this CPU only.  Another
 * CPU may still hold a translation for the old frames (it touched the stack,
 * e.g. to set up a signal frame for that thread), and if the slot were handed
 * out again with new frames that CPU could write through the stale entry into
 * frames the PMM has since given to someone else.  So a freed slot is marked
 * stale and is not reused until every CPU has flushed.  Allocation prefers
 * clean slots and only when none is left does one shootdown clean them all;
 * since each CPU also reloads CR3 (a full flush — PGE is off) every time it
 * passes through the scheduler, that is a rare, cheap event.
 */

_Static_assert(KSTACKSIZE % PAGE_SIZE == 0, "KSTACKSIZE must be whole pages");
_Static_assert(KSTACK_SLOT_SIZE >= KSTACKSIZE + PAGE_SIZE,
               "a kernel stack slot needs at least one guard page");
_Static_assert(KSTACK_SLOTS >= MAX_PROCS + MAX_CPUS,
               "every process and every AP boot stack needs a kernel stack slot");
_Static_assert(KSTACK_REGION_START >= 0xF0000000UL &&
               KSTACK_REGION_END <= 0xFEE00000UL,
               "kernel stack window overlaps the framebuffer or the LAPIC");

#define KSTACK_GUARD (KSTACK_SLOT_SIZE - KSTACKSIZE)

enum { SLOT_FREE = 0, SLOT_USED, SLOT_STALE, SLOT_FLUSHING };
static uint8_t  slot_state[KSTACK_SLOTS];
static unsigned slots_used;
static unsigned slots_stale;

static inline uint32_t irq_save(void) {
    uint32_t f;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(f) :: "memory");
    return f;
}
static inline void irq_restore(uint32_t f) {
    if (f & 0x200) __asm__ volatile("sti" ::: "memory");
}

static inline uint32_t slot_base(unsigned i) {
    return (uint32_t)(KSTACK_REGION_START + i * KSTACK_SLOT_SIZE + KSTACK_GUARD);
}

/* Unmap `pages` pages from `base` and return their frames to the PMM. */
static void unmap_stack(uint32_t base, unsigned pages) {
    for (unsigned k = 0; k < pages; k++) {
        uint32_t va = base + k * PAGE_SIZE;
        uint32_t phys = paging_get_physical(va);
        paging_unmap(va);                    /* also invlpg on this CPU */
        if (phys) pmm_free_frame(phys & ~0xFFFU);
    }
}

void *kstack_alloc(void) {
    for (;;) {
        uint32_t f = irq_save();
        int pick = -1;
        for (unsigned i = 0; i < KSTACK_SLOTS; i++)
            if (slot_state[i] == SLOT_FREE) { pick = (int)i; break; }
        if (pick < 0 && slots_stale) {
            /* Only the slots stale NOW are covered by the flush below; one
             * freed while it runs stays stale for the next round. */
            for (unsigned i = 0; i < KSTACK_SLOTS; i++)
                if (slot_state[i] == SLOT_STALE) slot_state[i] = SLOT_FLUSHING;
            slots_stale = 0;
            irq_restore(f);
            /* Outside the cli section: the shootdown waits for the other
             * CPUs' acknowledgements.  Under the BKL, as every caller is. */
            ktrace("[KSTACK] reusing freed slots: TLB shootdown (%u in use)\n",
                   slots_used);
            tlb_flush_all();
            tlb_shootdown();
            f = irq_save();
            for (unsigned i = 0; i < KSTACK_SLOTS; i++)
                if (slot_state[i] == SLOT_FLUSHING) slot_state[i] = SLOT_FREE;
            irq_restore(f);
            continue;
        }
        if (pick < 0) {
            irq_restore(f);
            kmem_oom_report("kernel stack slots", slots_used);
            return NULL;
        }
        slot_state[pick] = SLOT_USED;
        slots_used++;
        irq_restore(f);

        uint32_t base = slot_base((unsigned)pick);
        for (unsigned k = 0; k < KSTACKSIZE / PAGE_SIZE; k++) {
            if (vmm_alloc_page(base + k * PAGE_SIZE,
                               PAGE_PRESENT | PAGE_WRITABLE | PAGE_NX) != 0) {
                kmem_oom_report("physical memory for a kernel stack", base);
                unmap_stack(base, k);
                f = irq_save();
                slot_state[pick] = SLOT_STALE;
                slots_used--;
                slots_stale++;
                irq_restore(f);
                return NULL;
            }
        }
        return (void *)(uintptr_t)base;
    }
}

void kstack_free(void *p) {
    if (!p) return;
    uint32_t base = (uint32_t)(uintptr_t)p;
    uint32_t off  = base - (uint32_t)KSTACK_REGION_START;
    unsigned i    = off / KSTACK_SLOT_SIZE;
    if (base < KSTACK_REGION_START || base >= KSTACK_REGION_END ||
        base != slot_base(i) || slot_state[i] != SLOT_USED) {
        printk("[KSTACK] bad free of 0x%08x ignored\n", (unsigned)base);
        return;
    }
    unmap_stack(base, KSTACKSIZE / PAGE_SIZE);
    uint32_t f = irq_save();
    slot_state[i] = SLOT_STALE;
    slots_used--;
    slots_stale++;
    irq_restore(f);
}

uint32_t kstack_guard_stack(uint32_t addr) {
    if (addr < KSTACK_REGION_START || addr >= KSTACK_REGION_END) return 0;
    unsigned i = (addr - (uint32_t)KSTACK_REGION_START) / KSTACK_SLOT_SIZE;
    uint32_t base = slot_base(i);
    return addr < base ? base : 0;
}

unsigned kstack_in_use(void) {
    return slots_used;
}
