#include "heap.h"
#include "vmm.h"
#include "pmm.h"
#include "../kernel/printk.h"
#include "../arch/i686/mm/paging.h"
#include <kernel/config.h>
#include <stdint.h>
#include <stddef.h>

/*
 * Kernel heap — doubly-linked free-list allocator.
 *
 * Layout: [block_header_t | user data | ...]
 * Header magic must be HEAP_MAGIC on every access — corrupt magic → panic.
 * Adjacent free blocks are coalesced immediately on kfree().
 * Heap expands by mapping new pages when no fitting block is found.
 */

#define HEAP_MAGIC  0xDEADBEEFU
#define ALIGN8(n)   (((n) + 7U) & ~7U)
#define MIN_SPLIT   (sizeof(block_header_t) + 8)

/*
 * The heap free-list is shared by all threads of a process (shared address
 * space) and is walked + mutated by kmalloc/kfree.  Concurrent thread mmaps
 * read files through ext2, which kmallocs block + indirect-cache buffers, so
 * the heap is hammered from multiple threads; an unlocked free-list corrupts
 * under preemption → arbitrary memory corruption.  Single CPU → saved-IF
 * cli/sti.  saved-IF nests correctly (kmalloc's heap_expand→paging→pmm also
 * guard themselves), so the recursive kmalloc path is safe.
 */
static inline uint32_t heap_irq_save(void) {
    uint32_t f;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(f) :: "memory");
    return f;
}
static inline void heap_irq_restore(uint32_t f) {
    if (f & 0x200) __asm__ volatile("sti" ::: "memory");
}

typedef struct block_header {
    uint32_t            magic;
    size_t              size;       /* usable bytes, not including header */
    int                 is_free;
    struct block_header *next;
    struct block_header *prev;
} block_header_t;

static block_header_t *heap_head = NULL;
static uint32_t        heap_end  = HEAP_START; /* next unmapped virtual page */

extern void panic(const char *msg, void *regs) __attribute__((noreturn));

/* ── Internal helpers ─────────────────────────────────────────────────────── */

/*
 * Map `min_bytes` more of the heap window.  Returns 1 if the whole request was
 * mapped, 0 if it ran out of heap address space (HEAP_MAX) or of physical
 * frames first.
 *
 * It used to print "heap exhausted" and hlt forever, which is why kmalloc()
 * could not fail and every "if (!p) return -ENOMEM" in the kernel was dead
 * code.  Partial progress is kept rather than unwound: heap_end tracks what is
 * actually mapped, and kmalloc_nolock folds whatever was gained into the free
 * list before failing, so the pages are not lost.
 */
static int heap_expand(size_t min_bytes) {
    /* Round up to whole pages, at least one */
    size_t needed = min_bytes + sizeof(block_header_t);
    size_t pages  = (needed + PAGE_SIZE - 1) / PAGE_SIZE;
    if (pages < 1) pages = 1;

    for (size_t i = 0; i < pages; i++) {
        if (heap_end >= HEAP_MAX) {
            kmem_oom_report("kernel heap address space", (unsigned)heap_end);
            return 0;
        }
        if (vmm_alloc_page(heap_end, PAGE_WRITABLE) != 0) {
            kmem_oom_report("physical memory for the kernel heap",
                            (unsigned)heap_end);
            return 0;
        }
        heap_end += PAGE_SIZE;
    }
    return 1;
}

static block_header_t *get_end_block(void) {
    block_header_t *b = heap_head;
    while (b && b->next) b = b->next;
    return b;
}

/* ── Public API ───────────────────────────────────────────────────────────── */

void heap_init(void) {
    /* Map the first heap page.  FATAL by design (audit category (c)): this runs
     * from kmain before there is a process, a scheduler or a caller to return
     * an error to, and a kernel with no heap at all cannot make progress. */
    if (vmm_alloc_page(HEAP_START, PAGE_WRITABLE) != 0)
        panic("heap_init: no memory for the first heap page", NULL);
    heap_end = HEAP_START + PAGE_SIZE;

    heap_head        = (block_header_t *)HEAP_START;
    heap_head->magic   = HEAP_MAGIC;
    heap_head->size    = PAGE_SIZE - sizeof(block_header_t);
    heap_head->is_free = 1;
    heap_head->next    = NULL;
    heap_head->prev    = NULL;

    printk("[HEAP] Initialized at 0x%08x, initial block %u bytes\n",
           (unsigned)HEAP_START,
           (unsigned)heap_head->size);
}

static void *kmalloc_nolock(size_t size);

/* First-fit scan that allocates nothing, so a caller can ask whether a request
 * can be met out of what is already mapped. */
static block_header_t *first_fit(size_t size) {
    for (block_header_t *b = heap_head; b; b = b->next) {
        if (b->magic != HEAP_MAGIC) {
            printk("[HEAP] PANIC: corrupt block at 0x%08x\n", (unsigned)(uintptr_t)b);
            panic("heap corruption", NULL);
        }
        if (b->is_free && b->size >= size) return b;
    }
    return NULL;
}

size_t heap_headroom(void) {
    uint32_t irq = heap_irq_save();
    size_t r = (size_t)(HEAP_MAX - (unsigned long)heap_end);
    heap_irq_restore(irq);
    return r;
}

/* Pages heap_expand() would map to satisfy `size` (which must be ALIGN8'd). */
static size_t expand_pages_for(size_t size) {
    size_t needed = size + sizeof(block_header_t);
    size_t pages  = (needed + PAGE_SIZE - 1) / PAGE_SIZE;
    return pages < 1 ? 1 : pages;
}

void *kmalloc_try(size_t size) {
    if (!size) return NULL;
    uint32_t irq = heap_irq_save();
    size_t asz = ALIGN8(size);
    void *r = NULL;

    if (first_fit(asz)) {
        r = kmalloc_nolock(asz);            /* fits already: cannot expand */
    } else {
        /* Growing is the only way.  Refuse unless BOTH the heap window and
         * physical memory can supply the pages.  kmalloc() now fails cleanly
         * too, so this is no longer about avoiding a halt — it is a politer
         * policy: a caller with a smaller acceptable size (the ext2 block
         * cache) should not consume the last frames into the kernel heap,
         * where only the kernel heap can ever use them again, merely to
         * discover it cannot have the big size. */
        size_t pages = expand_pages_for(asz);
        if (pages <= (size_t)((HEAP_MAX - (unsigned long)heap_end) / PAGE_SIZE) &&
            pages <= (size_t)pmm_free_frames())
            r = kmalloc_nolock(asz);
    }
    heap_irq_restore(irq);
    return r;
}

/* Carve `size` out of the free block `b`, splitting off the remainder if it is
 * big enough to hold a header plus something useful. */
static void *heap_carve(block_header_t *b, size_t size) {
    if (b->size >= size + MIN_SPLIT) {
        block_header_t *nb = (block_header_t *)((uint8_t *)b
                              + sizeof(block_header_t) + size);
        nb->magic   = HEAP_MAGIC;
        nb->size    = b->size - size - sizeof(block_header_t);
        nb->is_free = 1;
        nb->next    = b->next;
        nb->prev    = b;
        if (nb->next) nb->next->prev = nb;
        b->next = nb;
        b->size = size;
    }
    b->is_free = 0;
    return (void *)((uint8_t *)b + sizeof(block_header_t));
}

/* Fold the pages heap_expand() just mapped, [old_end, heap_end), into the free
 * list — extending the tail block if it is free, otherwise as a new block.
 * Safe to call when nothing was gained. */
static void heap_absorb(block_header_t *tail, uint32_t old_end) {
    if (heap_end == old_end) return;

    if (tail && tail->is_free) {
        tail->size += heap_end - old_end;
        return;
    }
    block_header_t *nb = (block_header_t *)old_end;
    nb->magic   = HEAP_MAGIC;
    nb->size    = heap_end - old_end - sizeof(block_header_t);
    nb->is_free = 1;
    nb->next    = NULL;
    nb->prev    = tail;
    if (tail) tail->next = nb;
    if (!heap_head) heap_head = nb;
}

static void *kmalloc_nolock(size_t size) {
    if (!size) return NULL;
    size = ALIGN8(size);

    block_header_t *b = first_fit(size);
    if (b) return heap_carve(b, size);

    /* No fitting block — grow the heap.  A partial expansion is absorbed too,
     * so the pages that were mapped stay usable; the retry is a single
     * first_fit rather than the recursion this used to do, because with a
     * failing heap_expand() that recursion could no longer be assumed to
     * terminate on the first retry. */
    block_header_t *tail = get_end_block();
    uint32_t old_end = heap_end;
    int full = heap_expand(size);
    heap_absorb(tail, old_end);
    if (!full) return NULL;

    b = first_fit(size);
    return b ? heap_carve(b, size) : NULL;
}

void *kmalloc(size_t size) {
    uint32_t irq = heap_irq_save();
    void *r = kmalloc_nolock(size);
    heap_irq_restore(irq);
    return r;
}

void kfree(void *ptr) {
    if (!ptr) return;
    uint32_t irq = heap_irq_save();

    block_header_t *b = (block_header_t *)((uint8_t *)ptr - sizeof(block_header_t));
    if (b->magic != HEAP_MAGIC) {
        heap_irq_restore(irq);
        printk("[HEAP] PANIC: kfree corrupt block at 0x%08x\n", (unsigned)(uintptr_t)b);
        panic("heap corruption in kfree", NULL);
    }
    if (b->is_free) {
        heap_irq_restore(irq);
        printk("[HEAP] WARN: double-free at 0x%08x\n", (unsigned)(uintptr_t)ptr);
        return;
    }
    b->is_free = 1;

    /* Coalesce with next block */
    if (b->next && b->next->is_free) {
        b->size += sizeof(block_header_t) + b->next->size;
        b->next  = b->next->next;
        if (b->next) b->next->prev = b;
    }
    /* Coalesce with previous block */
    if (b->prev && b->prev->is_free) {
        b->prev->size += sizeof(block_header_t) + b->size;
        b->prev->next  = b->next;
        if (b->next) b->next->prev = b->prev;
    }
    heap_irq_restore(irq);
}

void *kcalloc(size_t count, size_t size) {
    size_t total = count * size;
    void *ptr = kmalloc(total);
    if (ptr) {
        uint8_t *p = (uint8_t *)ptr;
        for (size_t i = 0; i < total; i++) p[i] = 0;
    }
    return ptr;
}

void *krealloc(void *ptr, size_t size) {
    if (!ptr) return kmalloc(size);
    if (!size) { kfree(ptr); return NULL; }

    block_header_t *b = (block_header_t *)((uint8_t *)ptr - sizeof(block_header_t));
    if (b->magic != HEAP_MAGIC)
        panic("heap corruption in krealloc", NULL);

    if (b->size >= size) return ptr;  /* already fits */

    void *np = kmalloc(size);
    if (!np) return NULL;
    uint8_t *src = (uint8_t *)ptr;
    uint8_t *dst = (uint8_t *)np;
    size_t  copy = b->size < size ? b->size : size;
    for (size_t i = 0; i < copy; i++) dst[i] = src[i];
    kfree(ptr);
    return np;
}
