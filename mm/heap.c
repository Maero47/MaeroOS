#include "heap.h"
#include "vmm.h"
#include "pmm.h"
#include "../kernel/printk.h"
#include "../arch/i686/mm/paging.h"
#include <kernel/config.h>
#include <kernel/kprof.h>
#include <stdint.h>
#include <stddef.h>

/*
 * Kernel heap — segregated free lists over boundary-tagged blocks (TLSF-style).
 *
 * The mapped heap [HEAP_START, heap_end) is tiled by blocks, each
 *     [block_header_t | payload (size bytes) ]
 * so the block physically after `b` is simply b + HDR + b->size, and every
 * header carries a pointer to the block physically before it.  That is all
 * kfree() needs to coalesce with both neighbours in O(1); no list of ALL
 * blocks is kept any more.
 *
 * Free blocks alone are threaded onto one of FL_COUNT x SL_COUNT lists, the
 * links living in the first 8 payload bytes.  Sizes below SMALL_MAX get exact
 * 8-byte classes; above that each power of two is split into SL_COUNT linear
 * sub-classes.  Two bitmaps say which lists are non-empty, so a lookup is a
 * couple of bit scans: kmalloc rounds the request up to the next class
 * boundary, and any block in that class or a larger one fits (good fit, O(1)).
 * Only when that finds nothing is the request's own class searched block by
 * block — the one class that can hold both fitting and non-fitting blocks —
 * so the heap still grows only when no free block anywhere could serve the
 * request, exactly as with the first-fit walk this replaces.
 *
 * The first-fit walk visited every block (free or not) on every call: ~1 000
 * steps per kmalloc and ~22 M before Firefox's first paint.
 *
 * Invariants (heap_check() verifies all of them):
 *   - blocks tile the mapped window exactly; heap_last is the highest block;
 *   - no two physically adjacent blocks are both free (kfree coalesces);
 *   - a block is on a free list iff its state is BLK_FREE, and on the list
 *     its size maps to.
 * A header with the wrong magic, or a state that is neither BLK_FREE nor
 * BLK_USED, is corruption → panic.
 *
 * Pages mapped into the heap are never returned to the physical allocator
 * (they were not before either); see kmalloc_try in heap.h.
 */

#define HEAP_MAGIC  0xDEADBEEFU
#define BLK_FREE    0xF4EEB10CU
#define BLK_USED    0x05EDB10CU
#define ALIGN8(n)   (((n) + 7U) & ~7U)

typedef struct block_header {
    uint32_t             magic;
    size_t               size;       /* usable bytes, not including header */
    struct block_header *prev_phys;  /* block just below this one, or NULL */
    uint32_t             state;      /* BLK_FREE / BLK_USED                */
} block_header_t;                    /* 16 bytes: payloads are 8-aligned   */

/* Free-list links, stored in a free block's payload. */
typedef struct free_links {
    block_header_t *next;
    block_header_t *prev;
} free_links_t;

#define HDR         sizeof(block_header_t)
#define MIN_PAYLOAD sizeof(free_links_t)          /* 8 */
/* Largest request whose ALIGN8 + header arithmetic cannot wrap a size_t.
 * Anything bigger could never fit the heap window anyway; without the bound
 * ALIGN8 wraps to 0 and a lookup of 0 bytes hands back a live 0-byte block. */
#define HEAP_MAX_REQ ((size_t)-1 - 7U - HDR - PAGE_SIZE)
#define MIN_SPLIT   (HDR + MIN_PAYLOAD)

_Static_assert(sizeof(block_header_t) == 16, "heap header must stay 16 bytes");
_Static_assert(sizeof(free_links_t) <= 8, "free links must fit the minimum payload");

/* Size classes. */
#define SL_LOG      4
#define SL_COUNT    (1U << SL_LOG)                    /* 16 sub-classes      */
#define SMALL_SHIFT (SL_LOG + 3)                      /* 7                   */
#define SMALL_MAX   (1U << SMALL_SHIFT)               /* 128: exact classes  */
#define FL_COUNT    24          /* fl 0 = small; fl 1..22 = 2^7 .. 2^28 */
#define HEAP_WINDOW ((size_t)(HEAP_MAX - HEAP_START))

_Static_assert(HEAP_MAX - HEAP_START <= (1UL << 28),
               "size classes cover a heap window of at most 256 MiB");

/*
 * The heap lists are shared by all threads (shared address space) and mutated
 * by kmalloc/kfree; an unlocked list corrupts under preemption.  Single CPU
 * (SMP runs the kernel under the BKL) → saved-IF cli/sti.  saved-IF nests
 * correctly (heap_expand→paging→pmm also guard themselves), so the recursive
 * kmalloc path from the page-table code is safe.
 */
static inline uint32_t heap_irq_save(void) {
    uint32_t f;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(f) :: "memory");
    return f;
}
static inline void heap_irq_restore(uint32_t f) {
    if (f & 0x200) __asm__ volatile("sti" ::: "memory");
}

static block_header_t *free_head[FL_COUNT][SL_COUNT];
static uint32_t        fl_bitmap;
static uint32_t        sl_bitmap[FL_COUNT];

static block_header_t *heap_last = NULL;        /* highest-addressed block    */
static uint32_t        heap_end  = HEAP_START;  /* next unmapped virtual page */

/* Accounting, for heap_stats() and the self-test's leak check. */
static size_t   g_free_bytes;      /* payload bytes on the free lists */
static uint32_t g_free_blocks;
static uint32_t g_used_blocks;

extern void panic(const char *msg, void *regs) __attribute__((noreturn));

/*
 * Debug poisoning (`make KHEAP_TEST=1` turns it on along with the boot-time
 * self-test).  Free payload past the links is filled with POISON_FREE and
 * checked when the block is handed out again, so a write through a stale
 * pointer is caught at the next allocation of that memory.
 */
#define KHEAP_POISON KHEAP_TEST
#define POISON_FREE  0xDBU

/* ── Corruption checks ───────────────────────────────────────────────────── */

static void __attribute__((noreturn)) heap_corrupt(const char *what, const void *at) {
    printk("[HEAP] PANIC: %s at 0x%08x\n", what, (unsigned)(uintptr_t)at);
    panic("heap corruption", NULL);
}

static inline int in_heap(const void *p) {
    uintptr_t a = (uintptr_t)p;
    return a >= HEAP_START && a < heap_end;
}

static inline void check_block(const block_header_t *b) {
    if (!in_heap(b) || ((uintptr_t)b & 7U))
        heap_corrupt("block pointer outside the heap", b);
    if (b->magic != HEAP_MAGIC)
        heap_corrupt("corrupt block", b);
    if (b->state != BLK_FREE && b->state != BLK_USED)
        heap_corrupt("corrupt block state", b);
}

static inline free_links_t *links(block_header_t *b) {
    return (free_links_t *)((uint8_t *)b + HDR);
}
static inline void *payload(block_header_t *b) { return (uint8_t *)b + HDR; }

/* Block physically after b, or NULL if b is the last one. */
static inline block_header_t *next_phys(block_header_t *b) {
    uintptr_t n = (uintptr_t)b + HDR + b->size;
    return n < heap_end ? (block_header_t *)n : NULL;
}

#if KHEAP_POISON
static void poison(block_header_t *b) {
    uint8_t *p = (uint8_t *)payload(b);
    for (size_t i = MIN_PAYLOAD; i < b->size; i++) p[i] = POISON_FREE;
}
static void poison_verify(block_header_t *b) {
    const uint8_t *p = (const uint8_t *)payload(b);
    for (size_t i = MIN_PAYLOAD; i < b->size; i++)
        if (p[i] != POISON_FREE) {
            printk("[HEAP] free block 0x%08x (size %u) written at +%u after free\n",
                   (unsigned)(uintptr_t)b, (unsigned)b->size, (unsigned)i);
            heap_corrupt("use after free", p + i);
        }
}
#else
static inline void poison(block_header_t *b) { (void)b; }
static inline void poison_verify(block_header_t *b) { (void)b; }
#endif

/* ── Size classes ────────────────────────────────────────────────────────── */

static inline unsigned fls32(uint32_t x) { return 31U - (unsigned)__builtin_clz(x); }

/* The class a block of `size` bytes lives in. */
static void mapping_insert(size_t size, unsigned *fl, unsigned *sl) {
    if (size < SMALL_MAX) {
        *fl = 0;
        *sl = (unsigned)(size >> 3);
    } else {
        unsigned f = fls32((uint32_t)size);
        *sl = (unsigned)(size >> (f - SL_LOG)) & (SL_COUNT - 1);
        *fl = f - SMALL_SHIFT + 1;
    }
}

/* The smallest class whose every block is >= size.  size <= HEAP_WINDOW. */
static void mapping_search(size_t size, unsigned *fl, unsigned *sl) {
    if (size >= SMALL_MAX)
        size += (1U << (fls32((uint32_t)size) - SL_LOG)) - 1;
    mapping_insert(size, fl, sl);
}

static void list_insert(block_header_t *b) {
    unsigned fl, sl;
    mapping_insert(b->size, &fl, &sl);
    free_links_t *l = links(b);
    l->prev = NULL;
    l->next = free_head[fl][sl];
    if (l->next) links(l->next)->prev = b;
    free_head[fl][sl] = b;
    fl_bitmap     |= 1U << fl;
    sl_bitmap[fl] |= 1U << sl;
    b->state = BLK_FREE;
    g_free_bytes += b->size;
    g_free_blocks++;
}

static void list_remove(block_header_t *b) {
    unsigned fl, sl;
    mapping_insert(b->size, &fl, &sl);
    free_links_t *l = links(b);
    if (l->next && (!in_heap(l->next) || l->next->state != BLK_FREE))
        heap_corrupt("free-list link", l->next);
    if (l->prev && (!in_heap(l->prev) || l->prev->state != BLK_FREE))
        heap_corrupt("free-list link", l->prev);
    if (l->next) links(l->next)->prev = l->prev;
    if (l->prev) links(l->prev)->next = l->next;
    else {
        if (free_head[fl][sl] != b) heap_corrupt("free-list head", b);
        free_head[fl][sl] = l->next;
        if (!l->next) {
            sl_bitmap[fl] &= ~(1U << sl);
            if (!sl_bitmap[fl]) fl_bitmap &= ~(1U << fl);
        }
    }
    b->state = BLK_USED;
    g_free_bytes -= b->size;
    g_free_blocks--;
}

/*
 * A free block of at least `size` bytes, or NULL.  Allocates nothing, so the
 * caller can also ask whether a request can be met from what is mapped.
 * kprof: every call is one heap_walk step for the bitmap lookup, plus one per
 * block examined in the fallback scan.
 */
static block_header_t *find_free(size_t size) {
    unsigned fl, sl;
    kprof_count(KPE_HEAP_WALK);

    if (size <= HEAP_WINDOW) {
        mapping_search(size, &fl, &sl);
        if (fl < FL_COUNT) {
            uint32_t sm = sl_bitmap[fl] & (~0U << sl);
            if (!sm) {
                uint32_t fm = fl + 1 < 32 ? fl_bitmap & (~0U << (fl + 1)) : 0;
                if (fm) {
                    fl = (unsigned)__builtin_ctz(fm);
                    sm = sl_bitmap[fl];
                }
            }
            if (sm) {
                block_header_t *b = free_head[fl][__builtin_ctz(sm)];
                check_block(b);
                return b;
            }
        }
    }

    /* Nothing in a class that guarantees a fit.  The request's own class may
     * still hold a block that happens to be big enough. */
    mapping_insert(size < HEAP_WINDOW ? size : HEAP_WINDOW, &fl, &sl);
    if (fl >= FL_COUNT) return NULL;
    for (block_header_t *b = free_head[fl][sl]; b; b = links(b)->next) {
        kprof_count(KPE_HEAP_WALK);
        check_block(b);
        if (b->size >= size) return b;
    }
    return NULL;
}

/* ── Growth ──────────────────────────────────────────────────────────────── */

/* Pages heap_expand() would map to satisfy `size` (which must be ALIGN8'd). */
static size_t expand_pages_for(size_t size) {
    size_t needed = size + HDR;
    size_t pages  = (needed + PAGE_SIZE - 1) / PAGE_SIZE;
    return pages < 1 ? 1 : pages;
}

/*
 * Map `min_bytes` more of the heap window.  Returns 1 if the whole request was
 * mapped, 0 if it ran out of heap address space (HEAP_MAX) or of physical
 * frames first.  Partial progress is kept rather than unwound: heap_end tracks
 * what is actually mapped, and the caller folds whatever was gained into the
 * free lists before failing, so the pages are not lost.
 */
static int heap_expand(size_t min_bytes) {
    size_t pages = expand_pages_for(min_bytes);

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

/* Fold the pages heap_expand() just mapped, [old_end, heap_end), into the free
 * lists — extending the last block if it is free, otherwise as a new block.
 * Safe to call when nothing was gained. */
static void heap_absorb(uint32_t old_end) {
    if (heap_end == old_end) return;
    block_header_t *tail = heap_last;
    size_t gained = heap_end - old_end;

    if (tail && tail->state == BLK_FREE) {
        list_remove(tail);
        tail->size += gained;
        poison(tail);
        list_insert(tail);
        return;
    }
    block_header_t *nb = (block_header_t *)old_end;
    nb->magic     = HEAP_MAGIC;
    nb->size      = gained - HDR;
    nb->prev_phys = tail;
    poison(nb);
    list_insert(nb);
    heap_last = nb;
}

/* ── Allocation ──────────────────────────────────────────────────────────── */

/* Take the free block `b` off its list and carve `size` out of it, returning
 * the remainder to the lists if it is big enough to be a block of its own. */
static void *heap_carve(block_header_t *b, size_t size) {
    list_remove(b);
    poison_verify(b);
    if (b->size >= size + MIN_SPLIT) {
        block_header_t *nb = (block_header_t *)((uint8_t *)b + HDR + size);
        nb->magic     = HEAP_MAGIC;
        nb->size      = b->size - size - HDR;
        nb->prev_phys = b;
        b->size = size;
        block_header_t *after = next_phys(nb);
        if (after) after->prev_phys = nb;
        else       heap_last = nb;
        list_insert(nb);            /* its neighbour `after` is in use */
    }
    g_used_blocks++;
    return payload(b);
}

/* Can growing the heap by enough for `size` possibly succeed?  Refuses a
 * request the heap window could never hold before any page is mapped for it
 * (the old code mapped the whole remaining window, then failed). */
static int window_can_hold(size_t size) {
    size_t avail = HEAP_MAX - (unsigned long)heap_end;
    if (heap_last && heap_last->state == BLK_FREE) avail += heap_last->size + HDR;
    return size + HDR <= avail;
}

static void *kmalloc_nolock(size_t size) {
    block_header_t *b = find_free(size);
    if (b) return heap_carve(b, size);

    if (!window_can_hold(size)) {
        kmem_oom_report("kernel heap address space", (unsigned)size);
        return NULL;
    }
    /* No fitting block — grow the heap.  A partial expansion is absorbed too,
     * so the pages that were mapped stay usable. */
    uint32_t old_end = heap_end;
    size_t want = size;
    if (heap_last && heap_last->state == BLK_FREE)
        want = size > heap_last->size + HDR ? size - heap_last->size - HDR : 1;
    int full = heap_expand(want);
    heap_absorb(old_end);
    if (!full) return NULL;

    b = find_free(size);
    return b ? heap_carve(b, size) : NULL;
}

void *kmalloc(size_t size) {
    if (!size || size > HEAP_MAX_REQ) return NULL;
    size = ALIGN8(size);
    uint32_t irq = heap_irq_save();
    kprof_count(KPE_KMALLOC);
    void *r = kmalloc_nolock(size);
    heap_irq_restore(irq);
    return r;
}

void *kmalloc_try(size_t size) {
    if (!size || size > HEAP_MAX_REQ) return NULL;
    size_t asz = ALIGN8(size);
    uint32_t irq = heap_irq_save();
    kprof_count(KPE_KMALLOC);
    void *r = NULL;

    block_header_t *b = find_free(asz);
    if (b) {
        r = heap_carve(b, asz);             /* fits already: cannot expand */
    } else {
        /* Growing is the only way.  Refuse unless BOTH the heap window and
         * physical memory can supply the pages.  kmalloc() fails cleanly
         * too, so this is not about avoiding a halt — it is a politer
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

/* ── Free ────────────────────────────────────────────────────────────────── */

void kfree(void *ptr) {
    if (!ptr) return;
    uint32_t irq = heap_irq_save();
    kprof_count(KPE_KFREE);

    block_header_t *b = (block_header_t *)((uint8_t *)ptr - HDR);
    if (!in_heap(b) || ((uintptr_t)ptr & 7U)) {
        heap_irq_restore(irq);
        printk("[HEAP] PANIC: kfree of non-heap pointer 0x%08x\n", (unsigned)(uintptr_t)ptr);
        panic("kfree of a pointer the heap never handed out", NULL);
    }
    if (b->magic != HEAP_MAGIC ||
        (b->state != BLK_FREE && b->state != BLK_USED)) {
        heap_irq_restore(irq);
        printk("[HEAP] PANIC: kfree corrupt block at 0x%08x\n", (unsigned)(uintptr_t)b);
        panic("heap corruption in kfree", NULL);
    }
    if (b->state == BLK_FREE) {
        heap_irq_restore(irq);
        printk("[HEAP] WARN: double-free at 0x%08x\n", (unsigned)(uintptr_t)ptr);
        return;
    }
    g_used_blocks--;

    /* Coalesce with the next block */
    block_header_t *n = next_phys(b);
    if (n) {
        check_block(n);
        if (n->prev_phys != b) heap_corrupt("broken prev link", n);
        if (n->state == BLK_FREE) {
            list_remove(n);
            b->size += HDR + n->size;
            if (n == heap_last) heap_last = b;
        }
    }
    /* Coalesce with the previous block */
    block_header_t *p = b->prev_phys;
    if (p) {
        check_block(p);
        if (p->state == BLK_FREE) {
            list_remove(p);
            p->size += HDR + b->size;
            if (b == heap_last) heap_last = p;
            b = p;
        }
    }
    n = next_phys(b);
    if (n) n->prev_phys = b;
    poison(b);
    list_insert(b);
    heap_irq_restore(irq);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void heap_init(void) {
    /* Map the first heap page.  FATAL by design (audit category (c)): this runs
     * from kmain before there is a process, a scheduler or a caller to return
     * an error to, and a kernel with no heap at all cannot make progress. */
    if (vmm_alloc_page(HEAP_START, PAGE_WRITABLE) != 0)
        panic("heap_init: no memory for the first heap page", NULL);
    heap_end = HEAP_START + PAGE_SIZE;

    block_header_t *b = (block_header_t *)HEAP_START;
    b->magic     = HEAP_MAGIC;
    b->size      = PAGE_SIZE - HDR;
    b->prev_phys = NULL;
    poison(b);
    list_insert(b);
    heap_last = b;

    printk("[HEAP] Initialized at 0x%08x, initial block %u bytes\n",
           (unsigned)HEAP_START, (unsigned)b->size);
}

size_t heap_headroom(void) {
    uint32_t irq = heap_irq_save();
    size_t r = (size_t)(HEAP_MAX - (unsigned long)heap_end);
    heap_irq_restore(irq);
    return r;
}

void heap_stats(struct heap_stats *s) {
    uint32_t irq = heap_irq_save();
    s->mapped      = heap_end - HEAP_START;
    s->free_bytes  = g_free_bytes;
    s->free_blocks = g_free_blocks;
    s->used_blocks = g_used_blocks;
    /* Everything that is neither a header nor free payload is live data. */
    s->used_bytes  = s->mapped - g_free_bytes - (g_free_blocks + g_used_blocks) * HDR;
    heap_irq_restore(irq);
}

int heap_check(void) {
    uint32_t irq = heap_irq_save();
    uint32_t nfree = 0, nused = 0;
    size_t   fbytes = 0;
    block_header_t *prev = NULL, *b = (block_header_t *)HEAP_START;

    for (;;) {
        check_block(b);
        if (b->prev_phys != prev) heap_corrupt("broken prev link", b);
        if (b->size < MIN_PAYLOAD || (b->size & 7U)) heap_corrupt("bad block size", b);
        if ((uintptr_t)b + HDR + b->size > heap_end) heap_corrupt("block runs past heap end", b);
        if (b->state == BLK_FREE) {
            if (prev && prev->state == BLK_FREE) heap_corrupt("uncoalesced free blocks", b);
            nfree++;
            fbytes += b->size;
        } else {
            nused++;
        }
        block_header_t *n = next_phys(b);
        if (!n) break;
        prev = b;
        b = n;
    }
    if (b != heap_last) heap_corrupt("heap_last is not the last block", heap_last);

    /* Every list holds exactly the free blocks of its class. */
    uint32_t listed = 0;
    for (unsigned fl = 0; fl < FL_COUNT; fl++)
        for (unsigned sl = 0; sl < SL_COUNT; sl++) {
            int bit = (sl_bitmap[fl] >> sl) & 1;
            if (bit != (free_head[fl][sl] != NULL)) heap_corrupt("stale class bitmap", free_head[fl][sl]);
            block_header_t *pv = NULL;
            for (block_header_t *f = free_head[fl][sl]; f; f = links(f)->next) {
                check_block(f);
                unsigned cf, cs;
                mapping_insert(f->size, &cf, &cs);
                if (f->state != BLK_FREE || cf != fl || cs != sl || links(f)->prev != pv)
                    heap_corrupt("free list entry", f);
                pv = f;
                if (++listed > nfree) heap_corrupt("free list cycle", f);
            }
        }
    for (unsigned fl = 0; fl < FL_COUNT; fl++)
        if (((fl_bitmap >> fl) & 1) != (sl_bitmap[fl] != 0)) heap_corrupt("stale first-level bitmap", NULL);
    if (listed != nfree || nfree != g_free_blocks || nused != g_used_blocks || fbytes != g_free_bytes)
        heap_corrupt("heap accounting", NULL);
    heap_irq_restore(irq);
    return (int)(nfree + nused);
}

void *kcalloc(size_t count, size_t size) {
    size_t total;
    if (__builtin_mul_overflow(count, size, &total)) return NULL;
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

    block_header_t *b = (block_header_t *)((uint8_t *)ptr - HDR);
    if (!in_heap(b) || b->magic != HEAP_MAGIC || b->state != BLK_USED)
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

/* ── Self-test (`make KHEAP_TEST=1`) ─────────────────────────────────────── */

#if KHEAP_TEST
#define ST_SLOTS 512

static uint32_t st_rng = 0x12345678U;
static uint32_t st_rand(void) {
    st_rng ^= st_rng << 13; st_rng ^= st_rng >> 17; st_rng ^= st_rng << 5;
    return st_rng;
}

static size_t st_size(void) {
    uint32_t r = st_rand(), k = r & 1023U;
    if (k < 700) return 1 + (st_rand() % 128U);        /* small objects     */
    if (k < 950) return 1 + (st_rand() % 4096U);       /* up to a page      */
    if (k < 1015) return 4096 + (st_rand() % 65536U);  /* buffers           */
    return 65536 + (st_rand() % (1024U * 1024U));      /* rare large blocks */
}

static void st_fill(uint8_t *p, size_t n, uint32_t tag) {
    for (size_t i = 0; i < n; i++) p[i] = (uint8_t)(tag + i * 7U);
}
static int st_ok(const uint8_t *p, size_t n, uint32_t tag) {
    for (size_t i = 0; i < n; i++) if (p[i] != (uint8_t)(tag + i * 7U)) return 0;
    return 1;
}

static void st_fail(const char *what) {
    printk("[HEAP-TEST] FAIL: %s\n", what);
    panic("kernel heap self-test failed", NULL);
}

void heap_selftest(void) {
    static uint8_t *ptr[ST_SLOTS];
    static size_t   len[ST_SLOTS];
    static uint32_t tag[ST_SLOTS];
    struct heap_stats s0, s1;

    heap_check();
    heap_stats(&s0);
    uint32_t walk0 = (uint32_t)kprof_ev[KPE_HEAP_WALK];
    uint32_t km0   = (uint32_t)kprof_ev[KPE_KMALLOC];

    /* Bounds: the overflow guards must hold. */
    if (kmalloc(0) || kmalloc((size_t)-1) || kmalloc((size_t)-8) ||
        kmalloc(HEAP_MAX_REQ + 1) || kmalloc_try((size_t)-1))
        st_fail("oversized request was not rejected");
    if (kcalloc(0x10000U, 0x10001U) || kcalloc((size_t)-1, 2))
        st_fail("kcalloc overflow was not rejected");
    if (kmalloc(HEAP_WINDOW))
        st_fail("request larger than the heap window succeeded");

    for (unsigned round = 0; round < 40000; round++) {
        unsigned i = st_rand() % ST_SLOTS;
        if (ptr[i]) {
            if (!st_ok(ptr[i], len[i], tag[i])) st_fail("block contents changed");
            if ((st_rand() & 7U) == 0) {             /* grow via krealloc */
                size_t nl = len[i] + 1 + (st_rand() % 2048U);
                uint8_t *np = (uint8_t *)krealloc(ptr[i], nl);
                if (!np) st_fail("krealloc failed");
                if (!st_ok(np, len[i], tag[i])) st_fail("krealloc lost data");
                ptr[i] = np; len[i] = nl;
                st_fill(np, nl, tag[i]);
            } else {
                kfree(ptr[i]);
                ptr[i] = NULL;
            }
        } else {
            size_t n = st_size();
            int zero = (st_rand() & 3U) == 0;
            uint8_t *p = zero ? (uint8_t *)kcalloc(1, n) : (uint8_t *)kmalloc(n);
            if (!p) st_fail("allocation failed");
            if ((uintptr_t)p & 7U) st_fail("allocation not 8-byte aligned");
            if (zero) for (size_t k = 0; k < n; k++) if (p[k]) st_fail("kcalloc not zeroed");
            ptr[i] = p; len[i] = n; tag[i] = st_rand();
            st_fill(p, n, tag[i]);
        }
        if ((round & 1023U) == 0) heap_check();
    }
    for (unsigned i = 0; i < ST_SLOTS; i++) {
        if (!ptr[i]) continue;
        if (!st_ok(ptr[i], len[i], tag[i])) st_fail("block contents changed");
        kfree(ptr[i]);
        ptr[i] = NULL;
    }
    heap_check();
    heap_stats(&s1);

    uint32_t km = (uint32_t)kprof_ev[KPE_KMALLOC] - km0;
    uint32_t wk = (uint32_t)kprof_ev[KPE_HEAP_WALK] - walk0;
    printk("[HEAP-TEST] used %u -> %u bytes, blocks %u -> %u, free blocks %u -> %u, "
           "mapped %u -> %u KiB, %u allocs, %u lookup steps\n",
           (unsigned)s0.used_bytes, (unsigned)s1.used_bytes,
           (unsigned)s0.used_blocks, (unsigned)s1.used_blocks,
           (unsigned)s0.free_blocks, (unsigned)s1.free_blocks,
           (unsigned)(s0.mapped / 1024U), (unsigned)(s1.mapped / 1024U), (unsigned)km, (unsigned)wk);
    if (s1.used_bytes != s0.used_bytes || s1.used_blocks != s0.used_blocks)
        st_fail("memory not returned to baseline (leak)");
    if (s1.free_blocks != s0.free_blocks)
        st_fail("free blocks did not coalesce back to baseline");
    printk("[HEAP-TEST] PASS\n");

#if KHEAP_TEST == 2
    /* Negative check: a store through a freed pointer must be caught when the
     * memory is next handed out ("use after free" panic), not go unnoticed. */
    uint8_t *uaf = (uint8_t *)kmalloc(64);
    kfree(uaf);
    uaf[20] = 0x42;
    printk("[HEAP-TEST] wrote through a freed pointer; the next kmalloc must panic\n");
    (void)kmalloc(64);
    st_fail("use after free went undetected");
#endif
}
#endif
