#include "pmm.h"
#include "../kernel/printk.h"
#include <kernel/config.h>
#include <stdint.h>

/*
 * Physical Memory Manager — bitmap allocator.
 *
 * One bit per 4 KiB physical frame: 1 = used, 0 = free.
 * For 128 MiB RAM: 32,768 frames → 4 KiB bitmap.
 *
 * The bitmap itself is placed immediately after the kernel image in physical
 * memory (starting at _kernel_phys_end, rounded up to PAGE_SIZE).
 */

/* Bitmap macros */
#define BIT_SET(a,b)   ((a)[(b)/32] |=  (1U << ((b)%32)))
#define BIT_CLEAR(a,b) ((a)[(b)/32] &= ~(1U << ((b)%32)))
#define BIT_TEST(a,b)  ((a)[(b)/32] &   (1U << ((b)%32)))

/*
 * The frame bitmap and refcounts are shared by all threads of a process (which
 * share an address space) and mutated from both syscalls and the page-fault
 * handler.  The check-then-set in pmm_alloc_frame is NOT atomic: if a thread is
 * preempted between BIT_TEST and BIT_SET, another thread can hand out the SAME
 * frame, and the two map it at different virtual addresses → memory corruption.
 * Single CPU, so a saved-IF cli/sti around each mutator is sufficient.
 */
static inline uint32_t pmm_irq_save(void) {
    uint32_t f;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(f) :: "memory");
    return f;
}
static inline void pmm_irq_restore(uint32_t f) {
    if (f & 0x200) __asm__ volatile("sti" ::: "memory");
}

/* Linker-provided kernel physical boundaries */
extern char _kernel_phys_start[];
extern char _kernel_phys_end[];

static uint32_t *pmm_bitmap;
static uint32_t  pmm_total;       /* Total frames */
static uint32_t  pmm_used;        /* Currently used frames */
static uint32_t  pmm_last_alloc;  /* Last successfully allocated frame index */

/* Physical address of the bitmap storage area */
static uint32_t pmm_bitmap_phys;
static uint32_t pmm_bitmap_size;  /* Bytes */

/* Per-frame reference counts (used by COW fork/free and shared memory) */
static uint16_t frame_refcount[PMM_MAX_FRAMES];

/* Allocator bitmap storage: 1 bit per frame, statically sized to the 4 GiB
 * maximum (PMM_MAX_FRAMES/32 words = 128 KiB).  Kept in .bss so it is always
 * within the kernel's early page mapping and never collides with the initrd. */
static uint32_t pmm_bitmap_storage[PMM_MAX_FRAMES / 32];

static void pmm_mark_used(uint32_t frame_idx) {
    if (!BIT_TEST(pmm_bitmap, frame_idx)) {
        BIT_SET(pmm_bitmap, frame_idx);
        pmm_used++;
    }
}

static void pmm_mark_free(uint32_t frame_idx) {
    if (BIT_TEST(pmm_bitmap, frame_idx)) {
        BIT_CLEAR(pmm_bitmap, frame_idx);
        pmm_used--;
    }
}

/* Mark a physical range [addr, addr+len) as free */
static void pmm_free_region(uint64_t addr, uint64_t len) {
    uint32_t start = (uint32_t)((addr + PAGE_SIZE - 1) / PAGE_SIZE); /* round up */
    uint32_t end   = (uint32_t)((addr + len) / PAGE_SIZE);           /* round down */
    for (uint32_t i = start; i < end && i < pmm_total; i++)
        pmm_mark_free(i);
}

/* Mark a physical range [addr, addr+len) as used */
static void pmm_used_region(uint32_t addr, uint32_t len) {
    uint32_t start = addr / PAGE_SIZE;
    uint32_t end   = (addr + len + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint32_t i = start; i < end && i < pmm_total; i++)
        pmm_mark_used(i);
}

void pmm_init(multiboot_info_t *mbi) {
    /* Determine total physical memory.  Use the mmap if available,
     * otherwise fall back to mem_upper. */
    uint32_t total_mem_kb = 1024;  /* At least 1 MiB (lower memory) */
    if (mbi->flags & MULTIBOOT_FLAG_MEM)
        total_mem_kb += mbi->mem_upper;

    pmm_total = total_mem_kb / 4;   /* Frames = KiB / 4 (each frame = 4 KiB) */
    /* Clamp to the statically-sized structures (bitmap + refcount = 4 GiB).
     * On non-PAE i686, usable RAM never exceeds this, but stay defensive. */
    if (pmm_total > PMM_MAX_FRAMES) pmm_total = PMM_MAX_FRAMES;

    /* The bitmap is a static array in the kernel image (.bss).  It used to be
     * placed dynamically at _kernel_phys_end, but GRUB loads the initrd module
     * right after the kernel too — so a large bitmap (more RAM = bigger bitmap)
     * would overwrite the initrd before it was parsed.  A static array sidesteps
     * this entirely: it is always inside the kernel's early page mapping and is
     * already reserved as part of the kernel region.  PMM_MAX_FRAMES (4 GiB)
     * bounds it to 128 KiB. */
    pmm_bitmap = pmm_bitmap_storage;
    pmm_bitmap_phys = (uint32_t)(uintptr_t)pmm_bitmap_storage - KERNEL_VMA;
    pmm_bitmap_size = (pmm_total + 7) / 8;
    pmm_bitmap_size = (pmm_bitmap_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    /* Mark everything as used initially */
    for (uint32_t i = 0; i < pmm_total / 32 + 1; i++)
        pmm_bitmap[i] = 0xFFFFFFFF;
    pmm_used = pmm_total;

    /* Walk the multiboot memory map: mark available regions free */
    if (mbi->flags & MULTIBOOT_FLAG_MMAP) {
        uintptr_t mmap_virt = (uintptr_t)mbi->mmap_addr + KERNEL_VMA;
        uintptr_t mmap_end  = mmap_virt + mbi->mmap_length;

        while (mmap_virt < mmap_end) {
            multiboot_mmap_entry_t *entry = (multiboot_mmap_entry_t *)mmap_virt;
            if (entry->type == MULTIBOOT_MEMORY_AVAILABLE)
                pmm_free_region(entry->addr, entry->len);
            /* Advance: entry->size does NOT include itself, add 4 for the size field */
            mmap_virt += entry->size + sizeof(entry->size);
        }
    } else if (mbi->flags & MULTIBOOT_FLAG_MEM) {
        /* No mmap — use mem_lower / mem_upper approximation */
        pmm_free_region(0x1000, (uint64_t)mbi->mem_lower * 1024 - 0x1000);
        pmm_free_region(0x100000, (uint64_t)mbi->mem_upper * 1024);
    }

    /* Re-mark as used: the first 1 MiB (BIOS data, VGA, etc.) */
    pmm_used_region(0, 0x100000);

    /* Re-mark as used: the kernel itself */
    pmm_used_region((uint32_t)(uintptr_t)_kernel_phys_start,
                    (uint32_t)((uintptr_t)_kernel_phys_end
                               - (uintptr_t)_kernel_phys_start));

    /* Re-mark as used: the bitmap storage */
    pmm_used_region(pmm_bitmap_phys, pmm_bitmap_size);

    /* Re-mark as used: multiboot info struct itself */
    pmm_used_region((uint32_t)(mbi->mmap_addr), mbi->mmap_length);

    printk("[PMM] %u MiB total (%u frames), %u free (%u MiB)\n",
           (unsigned)(pmm_total / 256),
           (unsigned)pmm_total,
           (unsigned)(pmm_total - pmm_used),
           (unsigned)((pmm_total - pmm_used) / 256));
}

/* UAF detector: a frame returned by the allocator must have refcount 0 (it was
 * either never refcounted or fully released).  If it is still non-zero, the
 * frame was freed (bitmap cleared) while a mapping still referenced it — a
 * use-after-free that lets two owners alias the same physical page (the GTK
 * heap-corruption bug).  Log it loudly with the stale count. */
static void pmm_alloc_uaf_check(uint32_t idx) {
    if (idx < PMM_MAX_FRAMES && frame_refcount[idx] != 0) {
        printk("[pmm-UAF] alloc frame idx=%u phys=%08x with stale refcount=%u!\n",
               (unsigned)idx, (unsigned)(idx * PAGE_SIZE),
               (unsigned)frame_refcount[idx]);
        frame_refcount[idx] = 0;   /* reset so the new owner starts clean */
    }
}

uint32_t pmm_alloc_frame(void) {
    uint32_t irq = pmm_irq_save();
    /* Fast scan: skip fully-used 32-frame words */
    for (uint32_t i = pmm_last_alloc / 32; i < (pmm_total + 31) / 32; i++) {
        if (pmm_bitmap[i] == 0xFFFFFFFF) continue;
        for (uint32_t bit = 0; bit < 32; bit++) {
            uint32_t idx = i * 32 + bit;
            if (idx >= pmm_total) break;
            if (!BIT_TEST(pmm_bitmap, idx)) {
                BIT_SET(pmm_bitmap, idx);
                pmm_used++;
                pmm_last_alloc = idx;
                pmm_alloc_uaf_check(idx);
                pmm_irq_restore(irq);
                return idx * PAGE_SIZE;
            }
        }
    }
    /* Wrap around from the beginning */
    for (uint32_t i = 0; i < pmm_last_alloc / 32; i++) {
        if (pmm_bitmap[i] == 0xFFFFFFFF) continue;
        for (uint32_t bit = 0; bit < 32; bit++) {
            uint32_t idx = i * 32 + bit;
            if (!BIT_TEST(pmm_bitmap, idx)) {
                BIT_SET(pmm_bitmap, idx);
                pmm_used++;
                pmm_last_alloc = idx;
                pmm_alloc_uaf_check(idx);
                pmm_irq_restore(irq);
                return idx * PAGE_SIZE;
            }
        }
    }
    pmm_irq_restore(irq);
    return 0;   /* Out of physical memory */
}

/* SMP use-after-free guard — deferred frame reuse (quarantine).
 *
 * A freed frame must not be handed straight back to the allocator: a sibling
 * thread on another CPU may still hold a stale TLB entry for it (a TLB shootdown
 * the target didn't confirm in time — we observed ~11 shootdown timeouts per
 * Firefox run on -smp 2).  Reusing the frame then lets that CPU read/write the
 * reclaimed page = memory corruption (the residual -smp 2 wild-pointer / SIGILL
 * crashes in the library region).  Instead hold each freed frame in a FIFO ring
 * and return the OLDEST to the bitmap only once the ring fills — by then
 * (hundreds of frees + many context switches / CR3 reloads later) any stale TLB
 * entry has certainly been flushed.  Deferred freeing, like Linux's RCU page
 * reclaim: a small bounded grace period that defeats the whole class of stale-
 * TLB use-after-free regardless of which shootdown was missed.  Cost: up to
 * PMM_QUARANTINE frames (2 MiB) held at any instant. */
#define PMM_QUARANTINE 512
static uint32_t pmm_quar[PMM_QUARANTINE];
static int      pmm_quar_n;     /* fill level (0..PMM_QUARANTINE)          */
static int      pmm_quar_pos;   /* oldest entry once full (FIFO eviction)  */

void pmm_free_frame(uint32_t phys) {
    uint32_t idx = phys / PAGE_SIZE;
    if (idx >= pmm_total) return;
    uint32_t irq = pmm_irq_save();
    if (pmm_quar_n < PMM_QUARANTINE) {
        pmm_quar[pmm_quar_n++] = idx;             /* still filling — just hold it */
    } else {
        uint32_t evict = pmm_quar[pmm_quar_pos];  /* oldest → now safe to reuse  */
        pmm_quar[pmm_quar_pos] = idx;
        pmm_quar_pos = (pmm_quar_pos + 1) % PMM_QUARANTINE;
        pmm_mark_free(evict);
    }
    pmm_irq_restore(irq);
}

uint32_t pmm_free_frames(void)  { return pmm_total - pmm_used; }
uint32_t pmm_total_frames(void) { return pmm_total; }

void pmm_reserve_region(uint32_t phys, uint32_t size) {
    pmm_used_region(phys, size);
}

void pmm_frame_incref(uint32_t phys) {
    uint32_t idx = phys / PAGE_SIZE;
    uint32_t irq = pmm_irq_save();
    if (idx < PMM_MAX_FRAMES && frame_refcount[idx] < 65535)
        frame_refcount[idx]++;
    pmm_irq_restore(irq);
}

/* Current reference count of a frame (0 if out of range).  Used by the shared-
 * mmap registry to detect frames that only IT still references (count==1) so
 * the entry can be reclaimed once no process maps it anymore. */
uint16_t pmm_frame_refcount(uint32_t phys) {
    uint32_t idx = phys / PAGE_SIZE;
    if (idx >= PMM_MAX_FRAMES) return 0;
    return frame_refcount[idx];
}

void pmm_frame_decref(uint32_t phys) {
    uint32_t idx = phys / PAGE_SIZE;
    if (idx >= PMM_MAX_FRAMES) return;
    uint32_t irq = pmm_irq_save();
    int do_free = 0;
    if (frame_refcount[idx] > 0) {
        frame_refcount[idx]--;
        if (frame_refcount[idx] == 0) do_free = 1;
    }
    pmm_irq_restore(irq);
    if (do_free) pmm_free_frame(phys);   /* takes its own guard */
}
