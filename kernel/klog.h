#pragma once
#include <stdint.h>
#include <stddef.h>

/*
 * Kernel log ring buffer.  printk() feeds every formatted message in here so
 * the boot/runtime log survives and can be read back from userspace through
 * /proc/kmsg (see dmesg).
 */

/* Append `len` bytes of log text to the ring buffer (overwrites oldest). */
void klog_write(const char *data, size_t len);

/*
 * Linearize the current ring buffer contents (oldest → newest) into `dst`.
 * Writes at most `dst_cap` bytes and returns the number written.
 */
uint32_t klog_snapshot(char *dst, uint32_t dst_cap);
