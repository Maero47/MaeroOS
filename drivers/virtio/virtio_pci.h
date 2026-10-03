#pragma once
/*
 * virtio over PCI: the transport every virtio device driver here sits on
 * (Virtual I/O Device (VIRTIO) Version 1.2, section 4.1 "Virtio Over PCI
 * Bus").  Generic: it knows nothing of what the device is; a driver finds
 * its device by virtio type, negotiates features, sets up its queues and
 * reads its own device-specific configuration through this API.
 *
 * Interfaces: the modern (virtio 1.x) one, found through the vendor PCI
 * capabilities: common, notify, ISR and device configuration structures in
 * memory BARs, mapped uncached with mm/mmio.c.  Transitional devices
 * (QEMU's default, 1af4:1000-103f) and modern-only ones (1af4:1040+type)
 * both use it.  A transitional device without those capabilities (QEMU's
 * "disable-modern=on", older hypervisors) falls back to the legacy 0.9.5
 * interface in its I/O BAR 0: 32 feature bits, no FEATURES_OK, queues of
 * the device's fixed size in one physically contiguous block from
 * pmm_alloc_contig (inside the 256 MiB direct map).  vp->legacy says which;
 * drivers see the same API either way, except that a legacy device never
 * has VIRTIO_F_VERSION_1 (virtio-net's header is then 10 bytes, not 12).
 * A memory BAR above 4 GiB is not usable (mmio_map is 32-bit).
 *
 * Interrupts: MSI-X when the function has it, with one vector (from the
 * MSI pool, arch/i686/cpu/irq.c, 0xE0-0xE7) shared by configuration changes
 * and every queue, delivered to the BSP's Local APIC; else the INTx line
 * through the PIC (shared lines are fine: the ISR status register tells
 * whose interrupt it is).  "virtio=intx" / "virtio=poll" on the kernel
 * command line force INTx or no interrupts at all.
 *
 * Memory: queues of up to VIRTQ_MAX_SIZE (256) entries.  Modern: each ring
 * area is at most one page, from the kernel heap, so needs no physically
 * contiguous memory.  Heap frames are all below 4 GiB, PAE or not
 * (pmm_alloc_frame), as is the direct map, so every DMA address is 32-bit.
 *
 * Driver sequence (3.1.1 "Driver Requirements: Device Initialization"):
 *
 *   const pci_device_t *d = virtio_pci_find(VIRTIO_ID_NET, 0);
 *   static struct virtio_pci vp;
 *   if (virtio_pci_probe(&vp, d, "VNET") < 0) return;  // reset, ACK, DRIVER
 *   if (virtio_pci_set_features(&vp, wanted | VIRTIO_F_VERSION_1) < 0)
 *       return;                                 // offered & wanted is kept
 *   virtio_pci_irq_setup(&vp, my_irq_cb, ctx);  // before the queues
 *   virtio_pci_queue_setup(&vp, &rxq, 0, 128);  // per queue
 *   ... read device config, post buffers ...
 *   virtio_pci_driver_ok(&vp);                  // device goes live
 *
 * then virtq_add/virtq_publish (virtqueue.h) and virtio_pci_kick() to give
 * the device work, virtq_get_used() to take it back.  On any failure after
 * probe call virtio_pci_fail(): status FAILED, bus mastering off.
 *
 * The interrupt callback runs in IRQ context with `isr` = VIRTIO_ISR_*
 * bits (with MSI-X, both: one vector cannot tell them apart); it should
 * only note what happened and wake a thread (io_wake()).
 */
#include <stdint.h>
#include "virtqueue.h"
#include "../pci.h"

#define VIRTIO_PCI_VENDOR       0x1AF4

/* Device types (5 "Device Types"). */
#define VIRTIO_ID_NET           1
#define VIRTIO_ID_BLOCK         2
#define VIRTIO_ID_CONSOLE       3
#define VIRTIO_ID_RNG           4
#define VIRTIO_ID_GPU           16
#define VIRTIO_ID_INPUT         18

/* Device status (2.1). */
#define VIRTIO_STATUS_ACKNOWLEDGE  1
#define VIRTIO_STATUS_DRIVER       2
#define VIRTIO_STATUS_DRIVER_OK    4
#define VIRTIO_STATUS_FEATURES_OK  8
#define VIRTIO_STATUS_NEEDS_RESET  64
#define VIRTIO_STATUS_FAILED       128

/* Device-independent feature bits (6 "Reserved Feature Bits"). */
#define VIRTIO_F_INDIRECT_DESC     (1ULL << 28)
#define VIRTIO_F_EVENT_IDX         (1ULL << 29)
#define VIRTIO_F_VERSION_1         (1ULL << 32)
#define VIRTIO_F_ACCESS_PLATFORM   (1ULL << 33)

/* ISR status bits (4.1.4.5). */
#define VIRTIO_ISR_QUEUE           1
#define VIRTIO_ISR_CONFIG          2

/* How the device interrupts us. */
#define VIRTIO_IRQ_POLL            0
#define VIRTIO_IRQ_INTX            1
#define VIRTIO_IRQ_MSIX            2

struct virtio_pci;
typedef void (*virtio_irq_fn)(struct virtio_pci *vp, uint8_t isr, void *ctx);

struct virtio_pci {
    const pci_device_t *pci;
    const char *tag;                 /* "[VNET]"-style log prefix, no brackets */
    uint16_t type;                   /* VIRTIO_ID_* */
    volatile uint8_t *common;        /* struct virtio_pci_common_cfg */
    volatile uint8_t *notify;        /* notify area base */
    uint32_t notify_mul;             /* notify_off_multiplier */
    uint32_t notify_len;
    volatile uint8_t *isr;
    volatile uint8_t *device;        /* device-specific configuration */
    uint32_t device_len;
    uint64_t device_features;        /* offered */
    uint64_t features;               /* negotiated */
    uint16_t num_queues;             /* modern only (legacy: 0xFFFF) */
    int legacy;                      /* 1: legacy I/O-port interface */
    uint16_t io;                     /* legacy: I/O BAR 0 base */
    uint32_t io_size;                /* legacy: its size */

    int irq_mode;                    /* VIRTIO_IRQ_* */
    int vector;                      /* MSI-X: IDT vector, else -1 */
    uint8_t irq_line;                /* INTx: PIC line */
    volatile uint8_t *msix_table;
    virtio_irq_fn irq_fn;
    void *irq_ctx;
    volatile uint32_t irqs;          /* interrupts taken for this device */
};

/* The `nth` (0-based) virtio PCI function of type `type`: modern ID
 * 1af4:(1040+type), or transitional 1af4:1000-103f whose subsystem ID is the
 * type.  NULL when there is none. */
const pci_device_t *virtio_pci_find(uint16_t type, int nth);

/* Map the modern capabilities (or find the legacy I/O BAR), enable decode
 * and bus mastering, reset the device and set ACKNOWLEDGE|DRIVER.  `tag` prefixes log lines.
 * 0, or -1 (message printed; the device is left reset). */
int virtio_pci_probe(struct virtio_pci *vp, const pci_device_t *d, const char *tag);

uint64_t virtio_pci_device_features(struct virtio_pci *vp);

/* Accept `features` & what the device offers (vp->features afterwards).
 * Modern: must include VIRTIO_F_VERSION_1; sets FEATURES_OK and checks the
 * device kept it.  Legacy: just written.  0, or -1 (FAILED set). */
int virtio_pci_set_features(struct virtio_pci *vp, uint64_t features);

/* MSI-X if possible, else INTx, else polling (see the top of the file);
 * returns the VIRTIO_IRQ_* mode chosen.  `want` caps it (VIRTIO_IRQ_MSIX:
 * best available); the "virtio=" command-line option caps it further.
 * Call after set_features and before queue_setup (queues take the vector). */
int virtio_pci_irq_setup(struct virtio_pci *vp, int want, virtio_irq_fn fn, void *ctx);

/* Allocate and register queue `index` with min(device max, `max_size`)
 * entries, rounded down to a power of two (legacy: the device's own size,
 * at most VIRTQ_MAX_SIZE), and enable it.  0, or -1 (the queue does not
 * exist, no memory, or its MSI-X vector was refused). */
int virtio_pci_queue_setup(struct virtio_pci *vp, struct virtqueue *vq,
                           uint16_t index, uint16_t max_size);

/* Status |= DRIVER_OK: the device may now use the queues. */
void virtio_pci_driver_ok(struct virtio_pci *vp);

/* Something went wrong after probe: status FAILED. */
void virtio_pci_fail(struct virtio_pci *vp);

uint8_t virtio_pci_status(struct virtio_pci *vp);

/* Tell the device queue `vq` has new available buffers. */
void virtio_pci_kick(struct virtio_pci *vp, struct virtqueue *vq);

/* Publish what was added to `vq` and kick if the device wants it. */
void virtio_pci_publish(struct virtio_pci *vp, struct virtqueue *vq);

/* Device-specific configuration, read consistently (retried while the
 * config generation changes, 4.1.4.3.1).  Out-of-range reads give zeros. */
void virtio_pci_config_read(struct virtio_pci *vp, uint32_t off, void *buf, uint32_t len);
uint8_t  virtio_pci_config8(struct virtio_pci *vp, uint32_t off);
uint16_t virtio_pci_config16(struct virtio_pci *vp, uint32_t off);
uint32_t virtio_pci_config32(struct virtio_pci *vp, uint32_t off);
void virtio_pci_config_write8(struct virtio_pci *vp, uint32_t off, uint8_t v);
void virtio_pci_config_write32(struct virtio_pci *vp, uint32_t off, uint32_t v);

/* " irq=msix/0xe4" or " irq=intx/11" etc. for /proc; bytes written. */
int virtio_pci_describe_irq(struct virtio_pci *vp, char *buf, uint32_t cap);

/* Page-aligned, zeroed DMA memory from the kernel heap (never freed), and
 * the physical address of a byte in it.  A buffer that must be physically
 * contiguous must not cross a page boundary. */
void *virtio_dma_alloc(uint32_t bytes);
uint32_t virtio_virt_to_phys(const void *p);
