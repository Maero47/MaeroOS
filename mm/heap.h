#pragma once
#include <stdint.h>
#include <stddef.h>

void  heap_init(void);
/* Returns NULL when the kernel heap cannot be grown to satisfy `size` (out of
 * heap address space, or out of physical frames).  EVERY caller must check. */
void *kmalloc(size_t size);

/*
 * kmalloc that declines to grow the heap unless it comfortably can.
 *
 * kmalloc() returns NULL on exhaustion, so any caller may simply check it.
 * kmalloc_try() is for the caller that has a SMALLER size it would happily
 * take instead (the ext2 block cache sizes itself this way): it refuses the
 * request unless both the heap window and the free-frame count can supply the
 * pages, rather than growing the heap by whatever it can get and then failing
 * anyway.  Heap growth is one-way — pages folded into the kernel heap are
 * never handed back to the physical allocator — so a speculative large request
 * that fails would permanently cost the frames it did manage to take.
 */
void *kmalloc_try(size_t size);

/* Bytes of the heap window (HEAP_START..HEAP_MAX) not yet mapped. */
size_t heap_headroom(void);
void *kcalloc(size_t count, size_t size);
void *krealloc(void *ptr, size_t size);
void  kfree(void *ptr);
