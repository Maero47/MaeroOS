#pragma once
#include <registers.h>
#include <stdint.h>
#include <stddef.h>

/* Validate that [ptr, ptr+len) is user-space and currently mapped. */
int access_ok(const void *ptr, size_t len);

/* access_ok() plus: every page will take a kernel STORE (writable, COW, or a
 * writable VMA not yet populated).  For user buffers handed to code that
 * writes them with plain stores instead of copy_to_user(). */
int access_ok_write(void *ptr, size_t len);

/* Copy helpers for syscall arguments and return buffers. */
int copy_from_user(void *dst, const void *src, size_t len);
int copy_to_user(void *dst, const void *src, size_t len);

/* Called from isr_handler when int_no == 128 */
void syscall_dispatch(registers_t *regs);
