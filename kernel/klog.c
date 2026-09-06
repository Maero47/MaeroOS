#include "klog.h"

/*
 * Fixed-size kernel log ring buffer.  16 KiB holds the full boot log plus a
 * healthy amount of runtime output.  When the buffer fills, the oldest bytes
 * are overwritten.
 *
 * No locking: writes are single-byte stores driven from printk(), which can be
 * called from interrupt context.  The worst case is interleaved characters
 * between a thread and an IRQ — never a crash — which is acceptable for a log.
 */

#define KLOG_SIZE 65536

static char     klog_buf[KLOG_SIZE];
static uint32_t klog_head;   /* next write position (mod KLOG_SIZE) */
static uint32_t klog_count;  /* bytes stored so far, capped at KLOG_SIZE */

void klog_write(const char *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        klog_buf[klog_head] = data[i];
        klog_head = (klog_head + 1) % KLOG_SIZE;
        if (klog_count < KLOG_SIZE)
            klog_count++;
    }
}

uint32_t klog_snapshot(char *dst, uint32_t dst_cap) {
    uint32_t count = klog_count;
    uint32_t start;

    if (count >= KLOG_SIZE) {
        count = KLOG_SIZE;
        start = klog_head;        /* full: oldest byte sits at head */
    } else {
        start = 0;                /* not wrapped: data is [0, head) */
    }

    /* If the caller's buffer is smaller than the log, keep the newest bytes. */
    if (count > dst_cap) {
        start = (start + (count - dst_cap)) % KLOG_SIZE;
        count = dst_cap;
    }

    for (uint32_t i = 0; i < count; i++)
        dst[i] = klog_buf[(start + i) % KLOG_SIZE];
    return count;
}
