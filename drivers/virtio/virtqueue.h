#pragma once
/*
 * Split virtqueues (Virtual I/O Device (VIRTIO) Version 1.2, section 2.7):
 * the descriptor table, the driver ("available") ring and the device
 * ("used") ring, shared with the device by DMA.
 *
 * The ring logic here knows nothing of PCI or of where the memory comes
 * from: the transport (virtio_pci.c) allocates the three areas, tells the
 * device where they are and does the notify; tools/test_virtqueue.c runs
 * this file on the host against a simulated device.
 *
 * Usage, for a driver:
 *   virtq_add(vq, bufs, n_out, n_in, cookie)  chain n_out device-readable
 *                                             then n_in device-writable
 *                                             buffers, put the head on the
 *                                             available ring (not yet visible)
 *   virtq_publish(vq)                         make the added chains visible
 *                                             (avail->idx); returns non-zero
 *                                             when the device wants a notify
 *   virtq_get_used(vq, &len)                  next completed chain's cookie
 *                                             (its descriptors are free
 *                                             again), NULL when none
 * No locking: the caller serialises (the drivers here run in process context
 * under preempt_disable, as lwIP does).
 */
#include <stdint.h>

#define VIRTQ_DESC_F_NEXT      1
#define VIRTQ_DESC_F_WRITE     2
#define VIRTQ_DESC_F_INDIRECT  4

#define VIRTQ_AVAIL_F_NO_INTERRUPT 1
#define VIRTQ_USED_F_NO_NOTIFY     1

/* The largest queue this code sets up: 256 descriptors are exactly one
 * 4 KiB page, so each of the three areas fits one page and needs no
 * physically contiguous run of pages. */
#define VIRTQ_MAX_SIZE 256

struct virtq_desc {
    uint64_t addr;            /* guest-physical */
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct virtq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];          /* [size], then used_event */
} __attribute__((packed));

struct virtq_used_elem {
    uint32_t id;              /* head of the completed chain */
    uint32_t len;             /* bytes the device wrote */
} __attribute__((packed));

struct virtq_used {
    uint16_t flags;
    uint16_t idx;
    struct virtq_used_elem ring[];   /* [size], then avail_event */
} __attribute__((packed));

/* Bytes of each area for a queue of `size` entries (2.7.10's table). */
#define VIRTQ_DESC_BYTES(size)  (16U * (uint32_t)(size))
#define VIRTQ_AVAIL_BYTES(size) (6U + 2U * (uint32_t)(size))
#define VIRTQ_USED_BYTES(size)  (6U + 8U * (uint32_t)(size))

struct virtq_buf {
    uint32_t phys;            /* below 4 GiB: every kernel page is */
    uint32_t len;
};

struct virtqueue {
    uint16_t index;           /* queue number on the device */
    uint16_t size;            /* entries, a power of two <= VIRTQ_MAX_SIZE */
    volatile struct virtq_desc *desc;
    volatile struct virtq_avail *avail;
    volatile struct virtq_used *used;
    uint32_t desc_phys, avail_phys, used_phys;

    uint16_t free_head;       /* first free descriptor (chained by .next) */
    uint16_t num_free;
    uint16_t avail_idx;       /* our copy of avail->idx, ahead until publish */
    uint16_t published;       /* avail->idx as last written */
    uint16_t last_used;       /* next used->ring slot to look at */
    void *cookie[VIRTQ_MAX_SIZE];   /* per head descriptor */
    uint16_t chain_len[VIRTQ_MAX_SIZE];

    /* Set by the transport: where to notify, and the value to write. */
    volatile uint16_t *notify;
    uint32_t kicks;           /* notifies written, for /proc */
};

/* Full memory barrier: the device sees the ring entries before the index
 * that publishes them (and we see its used entry before reading the data).
 * x86 keeps stores in order, but the compiler must not reorder either, and
 * a locked op also orders loads after stores. */
static inline void virtq_mb(void) {
    __asm__ volatile("lock; addl $0, (%%esp)" ::: "memory", "cc");
}

/* Lay out an empty queue on areas the caller allocated and zeroed (sizes
 * above, alignments 16/2/4).  `size` must be a power of two. */
void virtq_init(struct virtqueue *vq, uint16_t index, uint16_t size,
                void *desc, uint32_t desc_phys,
                void *avail, uint32_t avail_phys,
                void *used, uint32_t used_phys);

/* Chain `n_out` device-readable buffers then `n_in` device-writable ones
 * and queue the chain (visible to the device after virtq_publish).
 * `cookie` must not be NULL (virtq_get_used returns it).  Returns the head
 * descriptor, or -1 when fewer than n_out + n_in descriptors are free (or
 * the counts are 0). */
int virtq_add(struct virtqueue *vq, const struct virtq_buf *bufs,
              unsigned n_out, unsigned n_in, void *cookie);

/* Publish what virtq_add queued.  Returns 1 when the device asked to be
 * notified (VIRTQ_USED_F_NO_NOTIFY clear) and something new was published. */
int virtq_publish(struct virtqueue *vq);

/* 1 when the device has completed a chain we have not taken yet. */
int virtq_has_used(const struct virtqueue *vq);

/* Take the next completed chain: frees its descriptors and returns the
 * cookie given to virtq_add (len = bytes the device wrote), or NULL. */
void *virtq_get_used(struct virtqueue *vq, uint32_t *len);

/* Ask the device not to interrupt for this queue (a hint), or to again.
 * After enabling, check virtq_has_used(): a completion may have slipped in. */
void virtq_disable_cb(struct virtqueue *vq);
void virtq_enable_cb(struct virtqueue *vq);
