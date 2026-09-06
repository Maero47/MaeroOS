#pragma once
#include <stdint.h>
#include <stddef.h>

void  heap_init(void);
void *kmalloc(size_t size);
void *kcalloc(size_t count, size_t size);
void *krealloc(void *ptr, size_t size);
void  kfree(void *ptr);
