/*
 * Split virtqueue ring logic (virtio 1.2, 2.7); see virtqueue.h.  Written
 * from the specification; no code was copied.  Freestanding, with no kernel
 * includes, so tools/test_virtqueue.c can build it on the host.
 */
#include "virtqueue.h"

void virtq_init(struct virtqueue *vq, uint16_t index, uint16_t size,
                void *desc, uint32_t desc_phys,
                void *avail, uint32_t avail_phys,
                void *used, uint32_t used_phys) {
    vq->index = index;
    vq->size = size;
    vq->desc = desc;
    vq->avail = avail;
    vq->used = used;
    vq->desc_phys = desc_phys;
    vq->avail_phys = avail_phys;
    vq->used_phys = used_phys;
    for (uint16_t i = 0; i < size; i++) {
        vq->desc[i].addr = 0;
        vq->desc[i].len = 0;
        vq->desc[i].flags = 0;
        vq->desc[i].next = (uint16_t)(i + 1);
        vq->cookie[i] = 0;
        vq->chain_len[i] = 0;
    }
    vq->avail->flags = 0;
    vq->avail->idx = 0;
    vq->used->flags = 0;
    vq->used->idx = 0;
    vq->free_head = 0;
    vq->num_free = size;
    vq->avail_idx = 0;
    vq->published = 0;
    vq->last_used = 0;
    vq->notify = 0;
    vq->kicks = 0;
}

int virtq_add(struct virtqueue *vq, const struct virtq_buf *bufs,
              unsigned n_out, unsigned n_in, void *cookie) {
    unsigned n = n_out + n_in;
    if (n == 0 || n > vq->num_free)
        return -1;
    uint16_t head = vq->free_head, d = head;
    for (unsigned i = 0; i < n; i++) {
        vq->desc[d].addr = bufs[i].phys;
        vq->desc[d].len = bufs[i].len;
        vq->desc[d].flags = (uint16_t)((i >= n_out ? VIRTQ_DESC_F_WRITE : 0) |
                                       (i + 1 < n ? VIRTQ_DESC_F_NEXT : 0));
        d = vq->desc[d].next;
    }
    vq->free_head = d;
    vq->num_free = (uint16_t)(vq->num_free - n);
    vq->cookie[head] = cookie;
    vq->chain_len[head] = (uint16_t)n;
    vq->avail->ring[vq->avail_idx & (vq->size - 1)] = head;
    vq->avail_idx++;
    return head;
}

int virtq_publish(struct virtqueue *vq) {
    if (vq->avail_idx == vq->published)
        return 0;
    virtq_mb();                         /* ring entries before the index */
    vq->avail->idx = vq->avail_idx;
    vq->published = vq->avail_idx;
    virtq_mb();                         /* the index before reading flags */
    return !(vq->used->flags & VIRTQ_USED_F_NO_NOTIFY);
}

int virtq_has_used(const struct virtqueue *vq) {
    return vq->used->idx != vq->last_used;
}

void *virtq_get_used(struct virtqueue *vq, uint32_t *len) {
    if (vq->used->idx == vq->last_used)
        return 0;
    virtq_mb();                         /* the index before the entry */
    volatile struct virtq_used_elem *e = &vq->used->ring[vq->last_used & (vq->size - 1)];
    uint32_t id = e->id;
    uint32_t written = e->len;
    vq->last_used++;
    if (id >= vq->size || vq->chain_len[id] == 0)
        return 0;                       /* a device bug; drop the entry */

    /* Back on the free list: walk to the chain's tail, link it to the old
     * head of the free list. */
    uint16_t n = vq->chain_len[id], tail = (uint16_t)id;
    for (uint16_t i = 1; i < n; i++)
        tail = vq->desc[tail].next;
    vq->desc[tail].next = vq->free_head;
    vq->free_head = (uint16_t)id;
    vq->num_free = (uint16_t)(vq->num_free + n);
    vq->chain_len[id] = 0;
    void *cookie = vq->cookie[id];
    vq->cookie[id] = 0;
    if (len)
        *len = written;
    return cookie;
}

void virtq_disable_cb(struct virtqueue *vq) {
    vq->avail->flags = VIRTQ_AVAIL_F_NO_INTERRUPT;
}

void virtq_enable_cb(struct virtqueue *vq) {
    vq->avail->flags = 0;
    virtq_mb();
}
