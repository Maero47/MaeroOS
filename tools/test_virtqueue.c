/*
 * Host test for the split virtqueue ring logic (drivers/virtio/virtqueue.c),
 * against a simulated device that reads the available ring, walks each
 * descriptor chain, fills its device-writable buffers and completes chains
 * in or out of order.  Run by tools/test_virtqueue.py.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "virtio/virtqueue.c"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

/* "Physical" addresses are offsets into this arena. */
static unsigned char arena[1 << 16];

struct sim {
    struct virtqueue *vq;
    uint16_t last_avail;
    uint16_t pending[VIRTQ_MAX_SIZE];   /* heads taken, not completed */
    uint32_t pending_len[VIRTQ_MAX_SIZE];
    int npending;
    uint32_t seen_out_bytes;
};

/* Take every newly available chain: check its shape, sum the readable
 * bytes, fill the writable ones with the head index. */
static void sim_take(struct sim *s) {
    struct virtqueue *vq = s->vq;
    while (s->last_avail != vq->avail->idx) {
        uint16_t head = vq->avail->ring[s->last_avail % vq->size];
        s->last_avail++;
        uint16_t d = head;
        uint32_t written = 0;
        int n = 0, seen_write = 0;
        for (;;) {
            CHECK(d < vq->size);
            if (d >= vq->size)
                break;
            volatile struct virtq_desc *e = &vq->desc[d];
            CHECK(e->addr + e->len <= sizeof(arena));
            if (e->flags & VIRTQ_DESC_F_WRITE) {
                seen_write = 1;
                memset(arena + e->addr, head & 0xFF, e->len);
                written += e->len;
            } else {
                CHECK(!seen_write);         /* readable ones come first */
                s->seen_out_bytes += e->len;
            }
            n++;
            CHECK(n <= vq->size);
            if (!(e->flags & VIRTQ_DESC_F_NEXT) || n > vq->size)
                break;
            d = e->next;
        }
        s->pending[s->npending] = head;
        s->pending_len[s->npending] = written;
        s->npending++;
    }
}

/* Complete pending chain number `i` (in take order). */
static void sim_complete(struct sim *s, int i) {
    struct virtqueue *vq = s->vq;
    volatile struct virtq_used_elem *e = &vq->used->ring[vq->used->idx % vq->size];
    e->id = s->pending[i];
    e->len = s->pending_len[i];
    vq->used->idx++;
    for (int j = i; j + 1 < s->npending; j++) {
        s->pending[j] = s->pending[j + 1];
        s->pending_len[j] = s->pending_len[j + 1];
    }
    s->npending--;
}

static struct virtqueue vq;
static struct virtq_desc desc[VIRTQ_MAX_SIZE];
static unsigned char avail_mem[6 + 2 * VIRTQ_MAX_SIZE];
static unsigned char used_mem[6 + 8 * VIRTQ_MAX_SIZE];

static void setup(uint16_t size) {
    memset(desc, 0xAA, sizeof(desc));
    memset(avail_mem, 0xAA, sizeof(avail_mem));
    memset(used_mem, 0xAA, sizeof(used_mem));
    virtq_init(&vq, 3, size, desc, 0x1000, avail_mem, 0x2000, used_mem, 0x3000);
}

static void *cookie(int i) { return (void *)(uintptr_t)(i + 1); }

int main(void) {
    /* Basic: init state. */
    setup(8);
    CHECK(vq.num_free == 8 && vq.avail->idx == 0 && vq.used->idx == 0);
    CHECK(vq.avail->flags == 0 && !virtq_has_used(&vq));
    CHECK(virtq_get_used(&vq, 0) == 0);
    CHECK(virtq_publish(&vq) == 0);         /* nothing to publish */

    /* One chain of 2 out + 1 in; nothing visible until publish. */
    struct sim s = { .vq = &vq };
    struct virtq_buf b[3] = { { 0, 10 }, { 100, 20 }, { 200, 64 } };
    int h = virtq_add(&vq, b, 2, 1, cookie(0));
    CHECK(h == 0 && vq.num_free == 5);
    CHECK(vq.desc[0].flags == VIRTQ_DESC_F_NEXT && vq.desc[1].flags == VIRTQ_DESC_F_NEXT &&
          vq.desc[2].flags == VIRTQ_DESC_F_WRITE);
    sim_take(&s);
    CHECK(s.npending == 0);
    CHECK(virtq_publish(&vq) == 1);
    sim_take(&s);
    CHECK(s.npending == 1 && s.seen_out_bytes == 30);
    sim_complete(&s, 0);
    uint32_t len = 0;
    CHECK(virtq_get_used(&vq, &len) == cookie(0) && len == 64);
    CHECK(arena[200] == 0 && arena[263] == 0);   /* head 0 fills with 0 */
    CHECK(vq.num_free == 8);

    /* NO_NOTIFY: publish reports that no kick is wanted. */
    vq.used->flags = VIRTQ_USED_F_NO_NOTIFY;
    struct virtq_buf one = { 300, 8 };
    CHECK(virtq_add(&vq, &one, 0, 1, cookie(1)) >= 0);
    CHECK(virtq_publish(&vq) == 0);
    vq.used->flags = 0;
    sim_take(&s);
    sim_complete(&s, 0);
    CHECK(virtq_get_used(&vq, 0) == cookie(1));

    /* Full ring: exactly size single-descriptor chains fit. */
    setup(16);
    s = (struct sim){ .vq = &vq };
    for (int i = 0; i < 16; i++) {
        struct virtq_buf x = { (uint32_t)(i * 64), 64 };
        CHECK(virtq_add(&vq, &x, 0, 1, cookie(i)) >= 0);
    }
    struct virtq_buf x = { 0, 64 };
    CHECK(virtq_add(&vq, &x, 0, 1, cookie(99)) == -1);
    CHECK(virtq_add(&vq, b, 0, 0, cookie(99)) == -1);
    virtq_publish(&vq);
    sim_take(&s);
    CHECK(s.npending == 16);

    /* Out-of-order completion: every cookie comes back exactly once and
     * the free list is whole again. */
    int got[16] = { 0 };
    while (s.npending) {
        sim_complete(&s, s.npending > 2 ? 2 : s.npending - 1);
        void *c = virtq_get_used(&vq, &len);
        CHECK(c != 0);
        if (c) {
            int i = (int)((uintptr_t)c - 1);
            CHECK(i >= 0 && i < 16 && !got[i]);
            if (i >= 0 && i < 16) got[i]++;
            CHECK(len == 64);
        }
    }
    for (int i = 0; i < 16; i++)
        CHECK(got[i] == 1);
    CHECK(vq.num_free == 16);

    /* Mixed chain lengths, random completion order, past the 16-bit index
     * wrap (70000 chains through a 32-entry ring). */
    setup(32);
    s = (struct sim){ .vq = &vq };
    srand(7);
    unsigned long added = 0, done = 0;
    int inflight[VIRTQ_MAX_SIZE] = { 0 };
    while (done < 70000) {
        for (int k = rand() % 6; k-- > 0;) {
            unsigned nout = (unsigned)(rand() % 3), nin = (unsigned)(rand() % 3);
            if (nout + nin == 0) nin = 1;
            struct virtq_buf c[4];
            for (unsigned j = 0; j < nout + nin; j++)
                c[j] = (struct virtq_buf){ (uint32_t)(rand() % 60000), 16 };
            int hh = virtq_add(&vq, c, nout, nin, cookie((int)(added % 1000)));
            if (hh < 0) {
                CHECK(vq.num_free < nout + nin);
                break;
            }
            inflight[added % 1000 % VIRTQ_MAX_SIZE]++;
            added++;
        }
        virtq_publish(&vq);
        sim_take(&s);
        for (int k = rand() % 5; k-- > 0 && s.npending;) {
            sim_complete(&s, rand() % s.npending);
            void *c = virtq_get_used(&vq, 0);
            CHECK(c != 0);
            if (c) {
                inflight[((uintptr_t)c - 1) % VIRTQ_MAX_SIZE]--;
                done++;
            }
        }
    }
    while (s.npending) {
        sim_complete(&s, 0);
        void *c = virtq_get_used(&vq, 0);
        CHECK(c != 0);
        if (c) inflight[((uintptr_t)c - 1) % VIRTQ_MAX_SIZE]--;
    }
    for (int i = 0; i < VIRTQ_MAX_SIZE; i++)
        CHECK(inflight[i] == 0);
    CHECK(vq.num_free == 32 && !virtq_has_used(&vq));
    CHECK(vq.avail_idx == (uint16_t)added);

    /* A used entry naming a descriptor that is no chain head is dropped
     * without corrupting the free list. */
    vq.used->ring[vq.used->idx % vq.size].id = 1000;
    vq.used->idx++;
    CHECK(virtq_get_used(&vq, 0) == 0);
    CHECK(vq.num_free == 32);

    /* Interrupt suppression flag. */
    virtq_disable_cb(&vq);
    CHECK(vq.avail->flags == VIRTQ_AVAIL_F_NO_INTERRUPT);
    virtq_enable_cb(&vq);
    CHECK(vq.avail->flags == 0);

    if (failures) {
        printf("test_virtqueue: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_virtqueue ok (%lu chains through a 32-entry ring)\n", added);
    return 0;
}
