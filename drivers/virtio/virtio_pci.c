/*
 * virtio PCI transport, modern (virtio 1.x) interface; see virtio_pci.h.
 * Written from the OASIS specification "Virtual I/O Device (VIRTIO)
 * Version 1.2" (sections 2, 3.1 and 4.1); no code was copied.
 */
#include "virtio_pci.h"
#include "../../arch/i686/cpu/apic.h"
#include "../../arch/i686/cpu/irq.h"
#include "../../arch/i686/cpu/pic.h"
#include "../../arch/i686/mm/paging.h"
#include "../../include/kernel/boot_info.h"
#include "../../include/kernel/config.h"
#include "../../kernel/printk.h"
#include "../../lib/printf.h"
#include "../../lib/string.h"
#include "../../mm/heap.h"
#include "../../mm/mmio.h"
#include <registers.h>

/* struct virtio_pci_cap (4.1.4) fields, as config-space offsets from the
 * capability. */
#define CAP_CFG_TYPE   3
#define CAP_BAR        4
#define CAP_OFFSET     8
#define CAP_LENGTH     12
#define CAP_NOTIFY_MUL 16

#define CFG_COMMON  1
#define CFG_NOTIFY  2
#define CFG_ISR     3
#define CFG_DEVICE  4

/* struct virtio_pci_common_cfg (4.1.4.3). */
#define COMMON_DFSELECT      0x00
#define COMMON_DF            0x04
#define COMMON_GFSELECT      0x08
#define COMMON_GF            0x0C
#define COMMON_MSIX          0x10
#define COMMON_NUMQ          0x12
#define COMMON_STATUS        0x14
#define COMMON_CFGGENERATION 0x15
#define COMMON_Q_SELECT      0x16
#define COMMON_Q_SIZE        0x18
#define COMMON_Q_MSIX        0x1A
#define COMMON_Q_ENABLE      0x1C
#define COMMON_Q_NOFF        0x1E
#define COMMON_Q_DESCLO      0x20
#define COMMON_Q_DESCHI      0x24
#define COMMON_Q_AVAILLO     0x28
#define COMMON_Q_AVAILHI     0x2C
#define COMMON_Q_USEDLO      0x30
#define COMMON_Q_USEDHI      0x34
#define COMMON_LEN           0x38

#define VIRTIO_MSI_NO_VECTOR 0xFFFF

/* Every probed device, for the shared interrupt handler. */
#define VIRTIO_MAX_DEVS 8
static struct virtio_pci *devs[VIRTIO_MAX_DEVS];
static int ndevs;

/* ── Register access ─────────────────────────────────────────────────────── */
static inline uint8_t rd8(volatile uint8_t *b, uint32_t o) { return *(volatile uint8_t *)(b + o); }
static inline uint16_t rd16(volatile uint8_t *b, uint32_t o) { return *(volatile uint16_t *)(b + o); }
static inline uint32_t rd32(volatile uint8_t *b, uint32_t o) { return *(volatile uint32_t *)(b + o); }
static inline void wr8(volatile uint8_t *b, uint32_t o, uint8_t v) { *(volatile uint8_t *)(b + o) = v; }
static inline void wr16(volatile uint8_t *b, uint32_t o, uint16_t v) { *(volatile uint16_t *)(b + o) = v; }
static inline void wr32(volatile uint8_t *b, uint32_t o, uint32_t v) { *(volatile uint32_t *)(b + o) = v; }

static uint8_t cfg8(const pci_device_t *d, uint8_t off) {
    return (uint8_t)(pci_read_config32(d->bus, d->slot, d->func, (uint8_t)(off & ~3U)) >> ((off & 3U) * 8));
}

static uint32_t cfg32(const pci_device_t *d, uint8_t off) {
    return pci_read_config32(d->bus, d->slot, d->func, off);
}

/* ── DMA memory ──────────────────────────────────────────────────────────── */
void *virtio_dma_alloc(uint32_t bytes) {
    uint8_t *raw = kmalloc(bytes + PAGE_SIZE - 1);
    if (!raw)
        return 0;
    uint8_t *p = (uint8_t *)(((uintptr_t)raw + PAGE_SIZE - 1) & ~(uintptr_t)(PAGE_SIZE - 1));
    memset(p, 0, bytes);
    return p;
}

uint32_t virtio_virt_to_phys(const void *p) {
    return paging_get_physical((uint32_t)(uintptr_t)p);
}

/* ── Discovery ───────────────────────────────────────────────────────────── */
const pci_device_t *virtio_pci_find(uint16_t type, int nth) {
    for (int i = 0; i < pci_device_count(); i++) {
        const pci_device_t *d = pci_get_device(i);
        if (d->vendor_id != VIRTIO_PCI_VENDOR)
            continue;
        int match;
        if (d->device_id >= 0x1040 && d->device_id <= 0x107F)
            match = d->device_id - 0x1040 == type;
        else if (d->device_id >= 0x1000 && d->device_id <= 0x103F)
            match = (cfg32(d, 0x2C) >> 16) == type;      /* subsystem ID */
        else
            match = 0;
        if (match && nth-- == 0)
            return d;
    }
    return 0;
}

/* Map `len` bytes at `off` into memory BAR `bar`.  NULL: I/O BAR, 64-bit
 * BAR above 4 GiB, unassigned, or the window is full. */
static volatile uint8_t *map_bar(struct virtio_pci *vp, uint8_t bar, uint32_t off, uint32_t len) {
    const pci_device_t *d = vp->pci;
    if (bar > 5 || len == 0)
        return 0;
    uint32_t b = d->bar[bar];
    if (b & 1U)
        return 0;
    if (((b >> 1) & 3U) == 2U && (bar == 5 || d->bar[bar + 1] != 0)) {
        printk("[%s] BAR%u is above 4 GiB; not usable\n", vp->tag, (unsigned)bar);
        return 0;
    }
    uint32_t base = b & ~0xFU;
    if (!base)
        return 0;
    return mmio_map(base + off, len);
}

/* Walk the vendor-specific capabilities (ID 0x09) and map the first of
 * each structure type, as 4.1.4 asks. */
static int map_caps(struct virtio_pci *vp) {
    const pci_device_t *d = vp->pci;
    if (!(cfg32(d, 0x04) & (1U << 20)))
        return -1;                                     /* no capability list */
    uint8_t off = cfg8(d, 0x34) & 0xFC;
    for (int guard = 0; off && guard < 48; guard++) {
        uint32_t hdr = cfg32(d, off);
        if ((hdr & 0xFF) == 0x09) {
            uint8_t type = cfg8(d, (uint8_t)(off + CAP_CFG_TYPE));
            uint8_t bar = cfg8(d, (uint8_t)(off + CAP_BAR));
            uint32_t boff = cfg32(d, (uint8_t)(off + CAP_OFFSET));
            uint32_t blen = cfg32(d, (uint8_t)(off + CAP_LENGTH));
            switch (type) {
            case CFG_COMMON:
                if (!vp->common && blen >= COMMON_LEN)
                    vp->common = map_bar(vp, bar, boff, blen);
                break;
            case CFG_NOTIFY:
                if (!vp->notify && blen >= 2) {
                    vp->notify = map_bar(vp, bar, boff, blen);
                    vp->notify_len = blen;
                    vp->notify_mul = cfg32(d, (uint8_t)(off + CAP_NOTIFY_MUL));
                }
                break;
            case CFG_ISR:
                if (!vp->isr && blen >= 1)
                    vp->isr = map_bar(vp, bar, boff, blen);
                break;
            case CFG_DEVICE:
                if (!vp->device && blen) {
                    vp->device = map_bar(vp, bar, boff, blen);
                    vp->device_len = vp->device ? blen : 0;
                }
                break;
            }
        }
        off = (uint8_t)((hdr >> 8) & 0xFC);
    }
    return (vp->common && vp->notify && vp->isr) ? 0 : -1;
}

/* ── Status and features ─────────────────────────────────────────────────── */
uint8_t virtio_pci_status(struct virtio_pci *vp) {
    return rd8(vp->common, COMMON_STATUS);
}

static void set_status(struct virtio_pci *vp, uint8_t bits) {
    wr8(vp->common, COMMON_STATUS, (uint8_t)(rd8(vp->common, COMMON_STATUS) | bits));
}

/* Write 0 and wait for the device to read back 0 (4.1.4.3.2). */
static int reset(struct virtio_pci *vp) {
    wr8(vp->common, COMMON_STATUS, 0);
    for (int i = 0; i < 1000000; i++) {
        if (rd8(vp->common, COMMON_STATUS) == 0)
            return 0;
        __asm__ volatile("pause");
    }
    return -1;
}

void virtio_pci_fail(struct virtio_pci *vp) {
    if (vp->common)
        set_status(vp, VIRTIO_STATUS_FAILED);
}

int virtio_pci_probe(struct virtio_pci *vp, const pci_device_t *d, const char *tag) {
    memset(vp, 0, sizeof(*vp));
    vp->pci = d;
    vp->tag = tag;
    vp->vector = -1;
    vp->type = (d->device_id >= 0x1040) ? (uint16_t)(d->device_id - 0x1040)
                                        : (uint16_t)(cfg32(d, 0x2C) >> 16);

    /* Memory decode and bus mastering on (the queues are DMA). */
    uint32_t cmd = cfg32(d, 0x04);
    pci_write_config32(d->bus, d->slot, d->func, 0x04, (cmd & 0xFFFFU) | 0x6U);

    if (map_caps(vp) < 0) {
        printk("[%s] %04x:%04x has no usable virtio 1.x capabilities (legacy-only device?); not used\n",
               tag, (unsigned)d->vendor_id, (unsigned)d->device_id);
        return -1;
    }
    if (reset(vp) < 0) {
        printk("[%s] device did not reset\n", tag);
        return -1;
    }
    set_status(vp, VIRTIO_STATUS_ACKNOWLEDGE);
    set_status(vp, VIRTIO_STATUS_DRIVER);
    vp->num_queues = rd16(vp->common, COMMON_NUMQ);
    vp->device_features = virtio_pci_device_features(vp);
    if (ndevs < VIRTIO_MAX_DEVS)
        devs[ndevs++] = vp;
    return 0;
}

uint64_t virtio_pci_device_features(struct virtio_pci *vp) {
    wr32(vp->common, COMMON_DFSELECT, 0);
    uint32_t lo = rd32(vp->common, COMMON_DF);
    wr32(vp->common, COMMON_DFSELECT, 1);
    uint32_t hi = rd32(vp->common, COMMON_DF);
    return ((uint64_t)hi << 32) | lo;
}

int virtio_pci_set_features(struct virtio_pci *vp, uint64_t features) {
    features &= vp->device_features;
    if (!(features & VIRTIO_F_VERSION_1)) {
        printk("[%s] device does not offer VIRTIO_F_VERSION_1\n", vp->tag);
        virtio_pci_fail(vp);
        return -1;
    }
    wr32(vp->common, COMMON_GFSELECT, 0);
    wr32(vp->common, COMMON_GF, (uint32_t)features);
    wr32(vp->common, COMMON_GFSELECT, 1);
    wr32(vp->common, COMMON_GF, (uint32_t)(features >> 32));
    set_status(vp, VIRTIO_STATUS_FEATURES_OK);
    if (!(virtio_pci_status(vp) & VIRTIO_STATUS_FEATURES_OK)) {
        printk("[%s] device refused features 0x%08x%08x\n", vp->tag,
               (unsigned)(features >> 32), (unsigned)features);
        virtio_pci_fail(vp);
        return -1;
    }
    vp->features = features;
    return 0;
}

void virtio_pci_driver_ok(struct virtio_pci *vp) {
    set_status(vp, VIRTIO_STATUS_DRIVER_OK);
}

/* ── Interrupts ──────────────────────────────────────────────────────────── */
static void virtio_pci_irq(registers_t *regs) {
    uint32_t vec = regs->int_no;
    for (int i = 0; i < ndevs; i++) {
        struct virtio_pci *vp = devs[i];
        if (vp->irq_mode == VIRTIO_IRQ_MSIX) {
            if ((uint32_t)vp->vector != vec)
                continue;
            vp->irqs++;
            if (vp->irq_fn)
                vp->irq_fn(vp, VIRTIO_ISR_QUEUE | VIRTIO_ISR_CONFIG, vp->irq_ctx);
        } else if (vp->irq_mode == VIRTIO_IRQ_INTX && vec == 32U + vp->irq_line) {
            /* Reading ISR status acknowledges it; 0 = not this device's. */
            uint8_t isr = rd8(vp->isr, 0);
            if (!isr)
                continue;
            vp->irqs++;
            if (vp->irq_fn)
                vp->irq_fn(vp, isr, vp->irq_ctx);
        }
    }
}

/* "virtio=intx" / "virtio=poll" on the kernel command line. */
static int cmdline_cap(void) {
    const char *cl = boot_info_cmdline();
    for (const char *c = cl; c && *c; c++)
        if ((c == cl || c[-1] == ' ') && strncmp(c, "virtio=", 7) == 0) {
            if (strncmp(c + 7, "poll", 4) == 0) return VIRTIO_IRQ_POLL;
            if (strncmp(c + 7, "intx", 4) == 0) return VIRTIO_IRQ_INTX;
        }
    return VIRTIO_IRQ_MSIX;
}

static int msix_setup(struct virtio_pci *vp) {
    const pci_device_t *d = vp->pci;
    uint8_t bir;
    uint32_t toff;
    uint8_t cap = pci_msix_table(d, &bir, &toff);
    if (!cap)
        return -1;
    uint32_t entries = ((cfg32(d, cap) >> 16) & 0x7FFU) + 1;
    vp->msix_table = map_bar(vp, bir, toff, entries * 16U);
    if (!vp->msix_table)
        return -1;
    int vec = msi_install_handler(virtio_pci_irq);
    if (vec < 0) {
        printk("[%s] no free MSI vector\n", vp->tag);
        return -1;
    }
    /* Entry 0: message address/data for the BSP, unmasked; the rest
     * stay masked (their reset state). */
    volatile uint8_t *e = vp->msix_table;
    wr32(e, 0, 0xFEE00000U | (apic_id() << 12));
    wr32(e, 4, 0);
    wr32(e, 8, (uint32_t)vec);
    wr32(e, 12, 0);
    vp->vector = vec;
    vp->irq_mode = VIRTIO_IRQ_MSIX;
    pci_msix_enable(d, cap);
    wr16(vp->common, COMMON_MSIX, 0);
    if (rd16(vp->common, COMMON_MSIX) != 0) {
        printk("[%s] device refused MSI-X vector 0 for config changes\n", vp->tag);
        /* Back to INTx: MSI-X off again (enable bit 15). */
        uint32_t hdr = cfg32(d, cap);
        pci_write_config32(d->bus, d->slot, d->func, cap, hdr & ~(1U << 31));
        vp->irq_mode = VIRTIO_IRQ_POLL;
        vp->vector = -1;
        return -1;      /* the MSI vector stays taken; there are few devices */
    }
    return 0;
}

int virtio_pci_irq_setup(struct virtio_pci *vp, int want, virtio_irq_fn fn, void *ctx) {
    vp->irq_fn = fn;
    vp->irq_ctx = ctx;
    vp->irq_mode = VIRTIO_IRQ_POLL;
    int cap = cmdline_cap();
    if (want > cap)
        want = cap;
    if (want >= VIRTIO_IRQ_MSIX && apic_available() && msix_setup(vp) == 0)
        return vp->irq_mode;
    const pci_device_t *d = vp->pci;
    if (want >= VIRTIO_IRQ_INTX && d->irq_pin && d->irq_line >= 1 && d->irq_line <= 15) {
        vp->irq_line = d->irq_line;
        vp->irq_mode = VIRTIO_IRQ_INTX;
        /* One handler per line serves every virtio device on it. */
        int shared = 0;
        for (int i = 0; i < ndevs; i++)
            if (devs[i] != vp && devs[i]->irq_mode == VIRTIO_IRQ_INTX &&
                devs[i]->irq_line == vp->irq_line)
                shared = 1;
        if (!shared)
            irq_install_handler(vp->irq_line, virtio_pci_irq);
        uint32_t cmd = cfg32(d, 0x04);
        pci_write_config32(d->bus, d->slot, d->func, 0x04, (cmd & 0xFFFFU) & ~(1U << 10));
        pic_unmask(vp->irq_line);
    }
    return vp->irq_mode;
}

int virtio_pci_describe_irq(struct virtio_pci *vp, char *buf, uint32_t cap) {
    switch (vp->irq_mode) {
    case VIRTIO_IRQ_MSIX:
        return snprintf(buf, cap, " irq=msix/0x%02x", (unsigned)vp->vector);
    case VIRTIO_IRQ_INTX:
        return snprintf(buf, cap, " irq=intx/%u", (unsigned)vp->irq_line);
    default:
        return snprintf(buf, cap, " irq=poll");
    }
}

/* ── Queues ──────────────────────────────────────────────────────────────── */
int virtio_pci_queue_setup(struct virtio_pci *vp, struct virtqueue *vq,
                           uint16_t index, uint16_t max_size) {
    volatile uint8_t *c = vp->common;
    if (index >= vp->num_queues)
        return -1;
    wr16(c, COMMON_Q_SELECT, index);
    uint16_t dev_max = rd16(c, COMMON_Q_SIZE);
    if (dev_max == 0 || rd16(c, COMMON_Q_ENABLE))
        return -1;
    uint16_t want = dev_max < max_size ? dev_max : max_size;
    if (want > VIRTQ_MAX_SIZE)
        want = VIRTQ_MAX_SIZE;
    uint16_t size = 1;
    while ((uint16_t)(size << 1) <= want && size < 0x8000)
        size = (uint16_t)(size << 1);

    /* One page per area: 16*256, 6+2*256 and 6+8*256 bytes all fit. */
    uint8_t *mem = virtio_dma_alloc(3 * PAGE_SIZE);
    if (!mem)
        return -1;
    uint8_t *desc = mem, *avail = mem + PAGE_SIZE, *used = mem + 2 * PAGE_SIZE;
    virtq_init(vq, index, size,
               desc, virtio_virt_to_phys(desc),
               avail, virtio_virt_to_phys(avail),
               used, virtio_virt_to_phys(used));

    wr16(c, COMMON_Q_SIZE, size);
    wr32(c, COMMON_Q_DESCLO, vq->desc_phys);
    wr32(c, COMMON_Q_DESCHI, 0);
    wr32(c, COMMON_Q_AVAILLO, vq->avail_phys);
    wr32(c, COMMON_Q_AVAILHI, 0);
    wr32(c, COMMON_Q_USEDLO, vq->used_phys);
    wr32(c, COMMON_Q_USEDHI, 0);
    if (vp->irq_mode == VIRTIO_IRQ_MSIX) {
        wr16(c, COMMON_Q_MSIX, 0);
        if (rd16(c, COMMON_Q_MSIX) != 0) {
            printk("[%s] queue %u: device refused MSI-X vector 0\n", vp->tag, (unsigned)index);
            return -1;
        }
    } else {
        wr16(c, COMMON_Q_MSIX, VIRTIO_MSI_NO_VECTOR);
    }
    uint32_t noff = (uint32_t)rd16(c, COMMON_Q_NOFF) * vp->notify_mul;
    if (noff + 2 > vp->notify_len) {
        printk("[%s] queue %u: notify offset 0x%x outside the notify area\n",
               vp->tag, (unsigned)index, (unsigned)noff);
        return -1;
    }
    vq->notify = (volatile uint16_t *)(vp->notify + noff);
    wr16(c, COMMON_Q_ENABLE, 1);
    return 0;
}

void virtio_pci_kick(struct virtio_pci *vp, struct virtqueue *vq) {
    (void)vp;
    *vq->notify = vq->index;
    vq->kicks++;
}

void virtio_pci_publish(struct virtio_pci *vp, struct virtqueue *vq) {
    if (virtq_publish(vq))
        virtio_pci_kick(vp, vq);
}

/* ── Device configuration ────────────────────────────────────────────────── */
void virtio_pci_config_read(struct virtio_pci *vp, uint32_t off, void *buf, uint32_t len) {
    uint8_t *out = buf;
    if (!vp->device || off > vp->device_len || len > vp->device_len - off) {
        memset(out, 0, len);
        return;
    }
    for (int tries = 0; tries < 100; tries++) {
        uint8_t gen = rd8(vp->common, COMMON_CFGGENERATION);
        for (uint32_t i = 0; i < len; i++)
            out[i] = rd8(vp->device, off + i);
        if (rd8(vp->common, COMMON_CFGGENERATION) == gen)
            return;
    }
}

uint8_t virtio_pci_config8(struct virtio_pci *vp, uint32_t off) {
    if (!vp->device || off >= vp->device_len)
        return 0;
    return rd8(vp->device, off);
}

uint16_t virtio_pci_config16(struct virtio_pci *vp, uint32_t off) {
    if (!vp->device || off + 2 > vp->device_len)
        return 0;
    return rd16(vp->device, off);
}

uint32_t virtio_pci_config32(struct virtio_pci *vp, uint32_t off) {
    if (!vp->device || off + 4 > vp->device_len)
        return 0;
    return rd32(vp->device, off);
}

void virtio_pci_config_write8(struct virtio_pci *vp, uint32_t off, uint8_t v) {
    if (vp->device && off < vp->device_len)
        wr8(vp->device, off, v);
}
