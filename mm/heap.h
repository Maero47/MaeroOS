#pragma once
#include <stdint.h>
#include <stddef.h>

void  heap_init(void);
void *kmalloc(size_t size);

/*
 * kmalloc that can fail instead of halting the machine.
 *
 * kmalloc() cannot fail for a request the free list does not already hold: it
 * grows the heap, and both heap_expand() (out of heap address space) and
 * vmm_alloc_page() (out of physical frames) print and enter a hlt loop rather
 * than return.  That is a workable contract for the small allocations the
 * kernel makes constantly, and a useless one for a caller that is sizing a
 * cache and has a smaller size it would happily take instead.  kmalloc_try()
 * checks both resources before it can reach either halt, so such a caller can
 * offer to be smaller.  Returns NULL if the request would have to grow the
 * heap past what either resource can supply.
 */
void *kmalloc_try(size_t size);

/* Bytes of the heap window (HEAP_START..HEAP_MAX) not yet mapped. */
size_t heap_headroom(void);
void *kcalloc(size_t count, size_t size);
void *krealloc(void *ptr, size_t size);
void  kfree(void *ptr);
