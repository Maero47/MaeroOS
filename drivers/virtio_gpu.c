/*
 * virtio-gpu, 2D only (QEMU -device virtio-gpu-pci or virtio-vga; PCI
 * 1af4:1050).
 *
 * Transport: virtio 1.x over PCI ("modern"): the common, notify and device
 * configuration structures are found through the vendor capabilities and
 * mapped with mmio_map().  One split virtqueue, the control queue, is driven
 * synchronously and polled: a command is a two-descriptor chain (request,
 * response), the driver notifies and spins on the used ring.  Interrupts are
 * off (INTx disabled, no MSI-X vectors, VIRTQ_AVAIL_F_NO_INTERRUPT); the
 * cursor queue is not used (the desktop draws its own pointer).
 *
 * Display: the framebuffer is guest RAM, enough frames for the largest mode,
 * allocated once and mapped cached at the framebuffer window.  A mode is a
 * host resource of that size (RESOURCE_CREATE_2D, B8G8R8X8 = the XRGB8888
 * the desktop draws) backed by the first w*h*4 bytes of those frames
 * (RESOURCE_ATTACH_BACKING) and shown on scanout 0 (SET_SCANOUT).  What the
 * clients draw reaches the screen when it is flushed: TRANSFER_TO_HOST_2D
 * copies a rectangle from guest memory into the resource and RESOURCE_FLUSH
 * puts it on the display.  The desktop flushes its damage rectangles through
 * FBIO_MAEROS_FLUSH; fbflushd covers clients that do not (framebuffer.c).
 *
 * Display configuration: GET_DISPLAY_INFO gives the host's preferred size
 * (the QEMU window), added to the mode list; VIRTIO_GPU_EVENT_DISPLAY in the
 * device configuration's events_read says it changed, and the poll hook
 * reads it again.
 *
 * References: the Virtual I/O Device (VIRTIO) Version 1.2 specification,
 * sections 2.7 (split virtqueues), 4.1 (virtio over PCI) and 5.7 (GPU
 * device); QEMU hw/display/virtio-gpu.c for what the host does with each
 * command.  The structure layouts are the spec's.
 */
#include "virtio_gpu.h"
#include "framebuffer.h"
#include "pci.h"
#include "../arch/i686/cpu/spinlock.h"
#include "../kernel/printk.h"
#include "../lib/string.h"
#include "../mm/mmio.h"
#include "../mm/pmm.h"
#include <kernel/boot_info.h>
#include <kernel/config.h>
#include <stdint.h>

/* ── virtio over PCI ─────────────────────────────────────────────────────── */

#define VIRTIO_PCI_CAP_COMMON_CFG  1
#define VIRTIO_PCI_CAP_NOTIFY_CFG  2
#define VIRTIO_PCI_CAP_DEVICE_CFG  4

/* struct virtio_pci_common_cfg */
#define VC_DEVICE_FEATURE_SELECT 0x00
#define VC_DEVICE_FEATURE        0x04
#define VC_DRIVER_FEATURE_SELECT 0x08
#define VC_DRIVER_FEATURE        0x0C
#define VC_MSIX_CONFIG           0x10
#define VC_NUM_QUEUES            0x12
#define VC_DEVICE_STATUS         0x14
#define VC_QUEUE_SELECT          0x16
#define VC_QUEUE_SIZE            0x18
#define VC_QUEUE_MSIX_VECTOR     0x1A
#define VC_QUEUE_ENABLE          0x1C
#define VC_QUEUE_NOTIFY_OFF      0x1E
#define VC_QUEUE_DESC            0x20
#define VC_QUEUE_DRIVER          0x28
#define VC_QUEUE_DEVICE          0x30

#define VS_ACKNOWLEDGE  1
#define VS_DRIVER       2
#define VS_DRIVER_OK    4
#define VS_FEATURES_OK  8
#define VS_FAILED       128

#define VIRTIO_F_VERSION_1_HI  (1U << 0)   /* feature bit 32 */
#define VIRTIO_MSI_NO_VECTOR   0xFFFF

#define VIRTQ_DESC_F_NEXT      1
#define VIRTQ_DESC_F_WRITE     2
#define VIRTQ_AVAIL_F_NO_INTERRUPT 1

#define QSIZE 16                /* plenty for two in-flight commands */

struct virtq_desc { uint64_t addr; uint32_t len; uint16_t flags; uint16_t next; };
struct virtq_avail { uint16_t flags; uint16_t idx; uint16_t ring[QSIZE]; uint16_t used_event; };
struct virtq_used_elem { uint32_t id; uint32_t len; };
struct virtq_used { uint16_t flags; uint16_t idx; struct virtq_used_elem ring[QSIZE]; uint16_t avail_event; };

/* ── virtio-gpu ──────────────────────────────────────────────────────────── */

#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO        0x0100
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D      0x0101
#define VIRTIO_GPU_CMD_RESOURCE_UNREF          0x0102
#define VIRTIO_GPU_CMD_SET_SCANOUT             0x0103
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH          0x0104
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D     0x0105
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING 0x0106
#define VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING 0x0107
#define VIRTIO_GPU_RESP_OK_NODATA              0x1100
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO        0x1101

#define VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM       2
#define VIRTIO_GPU_EVENT_DISPLAY               1
#define VIRTIO_GPU_MAX_SCANOUTS                16

struct gpu_hdr {
    uint32_t type, flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint8_t  ring_idx, pad[3];
};
struct gpu_rect { uint32_t x, y, width, height; };
struct gpu_display_info {
    struct gpu_hdr hdr;
    struct { struct gpu_rect r; uint32_t enabled, flags; } pmodes[VIRTIO_GPU_MAX_SCANOUTS];
};
struct gpu_create_2d { struct gpu_hdr hdr; uint32_t resource_id, format, width, height; };
struct gpu_set_scanout { struct gpu_hdr hdr; struct gpu_rect r; uint32_t scanout_id, resource_id; };
struct gpu_flush { struct gpu_hdr hdr; struct gpu_rect r; uint32_t resource_id, pad; };
struct gpu_transfer_2d { struct gpu_hdr hdr; struct gpu_rect r; uint64_t offset; uint32_t resource_id, pad; };
struct gpu_attach_backing { struct gpu_hdr hdr; uint32_t resource_id, nr_entries; };
struct gpu_mem_entry { uint64_t addr; uint32_t length, pad; };
struct gpu_resource { struct gpu_hdr hdr; uint32_t resource_id, pad; };

/* The largest mode: what the backing is sized for (9 MiB). */
#define VG_MAX_W 1920
#define VG_MAX_H 1200
#define VG_FB_BYTES  (VG_MAX_W * VG_MAX_H * 4)
#define VG_FB_PAGES  (VG_FB_BYTES / 4096)
/* Backing entries: the frames coalesced into runs; one page of the command
 * area holds the request header and up to this many. */
#define VG_MAX_ENTRIES ((4096 - sizeof(struct gpu_attach_backing)) / sizeof(struct gpu_mem_entry))

/* DMA memory: in the kernel image, so physically contiguous and below 4 GiB
 * (phys = virt - KERNEL_VMA).  The queue in one page; two command slots, each
 * a request page and a response page. */
static uint8_t vq_mem[4096] __attribute__((aligned(4096)));
static uint8_t cmd_mem[2][4096] __attribute__((aligned(4096)));
static uint8_t resp_mem[2][4096] __attribute__((aligned(4096)));

static volatile struct virtq_desc  *vq_desc;
static volatile struct virtq_avail *vq_avail;
static volatile struct virtq_used  *vq_used;
static uint16_t vq_used_seen;

static volatile uint8_t *common;
static volatile uint8_t *notify;      /* this queue's notify address */
static volatile uint8_t *devcfg;
static spinlock_t vg_lock;
static int vg_dead;                   /* a command timed out: stop talking */

static uint32_t fb_frames[VG_FB_PAGES];
static uint32_t cur_res, cur_w, cur_h;
static uint32_t next_res = 1;

static uint32_t kphys(const void *p) {
    return (uint32_t)(uintptr_t)p - (uint32_t)KERNEL_VMA;
}

static inline void mb(void) { __asm__ volatile("lock; addl $0,(%%esp)" ::: "memory"); }

static inline uint8_t  c8(uint32_t o)  { return *(volatile uint8_t *)(common + o); }
static inline uint16_t c16(uint32_t o) { return *(volatile uint16_t *)(common + o); }
static inline uint32_t c32(uint32_t o) { return *(volatile uint32_t *)(common + o); }
static inline void w8(uint32_t o, uint8_t v)   { *(volatile uint8_t *)(common + o) = v; }
static inline void w16(uint32_t o, uint16_t v) { *(volatile uint16_t *)(common + o) = v; }
static inline void w32(uint32_t o, uint32_t v) { *(volatile uint32_t *)(common + o) = v; }
static inline void w64(uint32_t o, uint64_t v) {
    w32(o, (uint32_t)v);
    w32(o + 4, (uint32_t)(v >> 32));
}

/*
 * Submit `n` commands (1 or 2: slot i's request of req_len[i] bytes and
 * response of resp_len[i]) and wait until the device has used them all.
 * Each response's type is checked against `want`.  0, or -5 on an error
 * response or a device that stopped answering.
 */
static int vg_submit(int n, const uint32_t *req_len, const uint32_t *resp_len,
                     const uint32_t *want) {
    if (vg_dead) return -5;
    for (int i = 0; i < n; i++) {
        volatile struct virtq_desc *d = &vq_desc[2 * i];
        d[0].addr = kphys(cmd_mem[i]);
        d[0].len = req_len[i];
        d[0].flags = VIRTQ_DESC_F_NEXT;
        d[0].next = (uint16_t)(2 * i + 1);
        d[1].addr = kphys(resp_mem[i]);
        d[1].len = resp_len[i];
        d[1].flags = VIRTQ_DESC_F_WRITE;
        d[1].next = 0;
        memset(resp_mem[i], 0, sizeof(struct gpu_hdr));
        vq_avail->ring[(vq_avail->idx + i) % QSIZE] = (uint16_t)(2 * i);
    }
    mb();
    vq_avail->idx = (uint16_t)(vq_avail->idx + n);
    mb();
    *(volatile uint16_t *)notify = 0;           /* queue 0 */
    uint16_t target = (uint16_t)(vq_used_seen + n);
    /* QEMU answers within microseconds under KVM; TCG and a busy host
     * get a generous budget before the device is given up on. */
    for (uint32_t spin = 0; (uint16_t)(vq_used->idx - target) > 0x8000; spin++) {
        if (spin > 400000000U) {
            printk("[VGPU] control queue timeout; display driver stopped\n");
            vg_dead = 1;
            return -5;
        }
        __asm__ volatile("pause" ::: "memory");
    }
    vq_used_seen = target;
    mb();
    int rc = 0;
    for (int i = 0; i < n; i++) {
        const struct gpu_hdr *r = (const struct gpu_hdr *)resp_mem[i];
        if (r->type != want[i]) {
            printk("[VGPU] command 0x%04x: response 0x%04x\n",
                   (unsigned)((const struct gpu_hdr *)cmd_mem[i])->type,
                   (unsigned)r->type);
            rc = -5;
        }
    }
    return rc;
}

static void *cmd(int slot, uint32_t type, uint32_t len) {
    memset(cmd_mem[slot], 0, len);
    ((struct gpu_hdr *)cmd_mem[slot])->type = type;
    return cmd_mem[slot];
}

static int vg_simple(uint32_t len) {
    uint32_t rl = sizeof(struct gpu_hdr), want = VIRTIO_GPU_RESP_OK_NODATA;
    return vg_submit(1, &len, &rl, &want);
}

static int vg_display_info(uint32_t *w, uint32_t *h) {
    uint32_t len = sizeof(struct gpu_hdr), rl = sizeof(struct gpu_display_info);
    uint32_t want = VIRTIO_GPU_RESP_OK_DISPLAY_INFO;
    cmd(0, VIRTIO_GPU_CMD_GET_DISPLAY_INFO, len);
    if (vg_submit(1, &len, &rl, &want) != 0) return -1;
    const struct gpu_display_info *di = (const struct gpu_display_info *)resp_mem[0];
    if (!di->pmodes[0].enabled || !di->pmodes[0].r.width) return -1;
    *w = di->pmodes[0].r.width;
    *h = di->pmodes[0].r.height;
    return 0;
}

/* Resource `res` (w x h) on the first w*h*4 bytes of the framebuffer frames,
 * adjacent frames merged into one entry. */
static int vg_attach_backing(uint32_t res, uint32_t w, uint32_t h) {
    uint32_t bytes = (w * h * 4 + 4095) & ~4095U, n = 0;
    struct gpu_attach_backing *ab = cmd(0, VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING, 4096);
    struct gpu_mem_entry *e = (struct gpu_mem_entry *)(ab + 1);
    for (uint32_t i = 0; i < bytes / 4096; i++) {
        if (n && e[n - 1].addr + e[n - 1].length == fb_frames[i]) {
            e[n - 1].length += 4096;
            continue;
        }
        if (n == VG_MAX_ENTRIES) return -12;
        e[n].addr = fb_frames[i];
        e[n].length = 4096;
        n++;
    }
    ab->resource_id = res;
    ab->nr_entries = n;
    return vg_simple(sizeof(*ab) + n * sizeof(*e));
}

static int vg_set_mode(uint32_t w, uint32_t h, uint32_t *pitch) {
    uint32_t res = next_res++, old = cur_res;
    int rc;

    spin_lock(&vg_lock);
    struct gpu_create_2d *c = cmd(0, VIRTIO_GPU_CMD_RESOURCE_CREATE_2D, sizeof(*c));
    c->resource_id = res;
    c->format = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM;
    c->width = w;
    c->height = h;
    rc = vg_simple(sizeof(*c));
    if (rc == 0) rc = vg_attach_backing(res, w, h);
    if (rc == 0) {
        struct gpu_set_scanout *s = cmd(0, VIRTIO_GPU_CMD_SET_SCANOUT, sizeof(*s));
        s->r.width = w;
        s->r.height = h;
        s->scanout_id = 0;
        s->resource_id = res;
        rc = vg_simple(sizeof(*s));
    }
    if (rc != 0) {                      /* leave the old mode up */
        struct gpu_resource *u = cmd(0, VIRTIO_GPU_CMD_RESOURCE_UNREF, sizeof(*u));
        u->resource_id = res;
        vg_simple(sizeof(*u));
        spin_unlock(&vg_lock);
        return -22;
    }
    if (old) {
        struct gpu_resource *d = cmd(0, VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING, sizeof(*d));
        d->resource_id = old;
        vg_simple(sizeof(*d));
        struct gpu_resource *u = cmd(0, VIRTIO_GPU_CMD_RESOURCE_UNREF, sizeof(*u));
        u->resource_id = old;
        vg_simple(sizeof(*u));
    }
    cur_res = res;
    cur_w = w;
    cur_h = h;
    spin_unlock(&vg_lock);
    *pitch = w * 4;
    return 0;
}

/* TRANSFER_TO_HOST_2D and RESOURCE_FLUSH of one rectangle, submitted
 * together (the device runs the control queue in order). */
static void vg_flush(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    spin_lock(&vg_lock);
    if (!cur_res || x + w > cur_w || y + h > cur_h) {
        spin_unlock(&vg_lock);
        return;
    }
    struct gpu_transfer_2d *t = cmd(0, VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D, sizeof(*t));
    t->r.x = x; t->r.y = y; t->r.width = w; t->r.height = h;
    t->offset = (uint64_t)y * cur_w * 4 + x * 4;
    t->resource_id = cur_res;
    struct gpu_flush *f = cmd(1, VIRTIO_GPU_CMD_RESOURCE_FLUSH, sizeof(*f));
    f->r.x = x; f->r.y = y; f->r.width = w; f->r.height = h;
    f->resource_id = cur_res;
    uint32_t len[2] = { sizeof(*t), sizeof(*f) };
    uint32_t rl[2] = { sizeof(struct gpu_hdr), sizeof(struct gpu_hdr) };
    uint32_t want[2] = { VIRTIO_GPU_RESP_OK_NODATA, VIRTIO_GPU_RESP_OK_NODATA };
    vg_submit(2, len, rl, want);
    spin_unlock(&vg_lock);
}

static void vg_refresh_preferred(void) {
    uint32_t w, h;
    if (vg_display_info(&w, &h) != 0) return;
    framebuffer_set_preferred(w, h);
    if (w <= VG_MAX_W && h <= VG_MAX_H && (w & 7) == 0)
        framebuffer_mode_add(w, h);
}

/* Display-configuration change (the QEMU window was resized): read the
 * preferred size again.  The desktop sees the new list (generation) and
 * decides; the scanout keeps its mode until then. */
static void vg_poll(void) {
    if (vg_dead || !devcfg) return;
    if (!(*(volatile uint32_t *)devcfg & VIRTIO_GPU_EVENT_DISPLAY)) return;
    *(volatile uint32_t *)(devcfg + 4) = VIRTIO_GPU_EVENT_DISPLAY;   /* events_clear */
    spin_lock(&vg_lock);
    vg_refresh_preferred();
    spin_unlock(&vg_lock);
    printk("[VGPU] display configuration changed\n");
}

static const fb_driver_t vg_driver = {
    .name = "virtio-gpu",
    .set_mode = vg_set_mode,
    .flush = vg_flush,
    .poll = vg_poll,
};

static volatile uint8_t *map_cap(const pci_device_t *d, uint8_t cap, uint32_t min_len) {
    uint8_t bar = pci_read8(d, (uint8_t)(cap + 4));
    uint32_t off = pci_read_config32(d->bus, d->slot, d->func, (uint8_t)(cap + 8));
    uint32_t len = pci_read_config32(d->bus, d->slot, d->func, (uint8_t)(cap + 12));
    if (bar > 5 || (d->bar[bar] & 1) || len < min_len) return 0;
    if ((d->bar[bar] & 0x6) == 0x4 && (bar == 5 || d->bar[bar + 1])) {
        printk("[VGPU] BAR%u is above 4 GiB\n", (unsigned)bar);
        return 0;
    }
    uint32_t base = d->bar[bar] & ~0xFU;
    if (!base) return 0;
    if (len > 0x10000) len = 0x10000;
    return (volatile uint8_t *)mmio_map(base + off, len);
}

int virtio_gpu_init(void) {
    const pci_device_t *d = pci_find_device(0x1AF4, 0x1050);
    if (!d) return 0;
    const char *c = boot_info_cmdline();
    for (const char *p = c; p && *p; p++)
        if ((p == c || p[-1] == ' ') && !strncmp(p, "nomodeset", 9) &&
            (p[9] == 0 || p[9] == ' ')) {
            printk("[VGPU] nomodeset: not taking over the display\n");
            return 0;
        }
    /* virtio-vga: the boot loader's framebuffer is the VGA side (BAR0),
     * which this driver replaces.  Another card's framebuffer stays. */
    int is_vga = d->class_code == 0x03 && d->subclass == 0x00;
    if (framebuffer_available()) {
        uint32_t bar0 = d->bar[0] & ~0xFU;
        uint32_t fbp = framebuffer_phys();
        if (!is_vga || (d->bar[0] & 1) || fbp < bar0 ||
            fbp - bar0 >= pci_bar_size(d, 0)) {
            printk("[VGPU] the boot framebuffer is on another display; leaving it\n");
            return 0;
        }
    }

    /* Find the common, notify and device configuration structures. */
    uint8_t cap_common = 0, cap_notify = 0, cap_dev = 0, id = 0;
    for (uint8_t off = pci_cap_next(d, 0, &id), guard = 0; off && guard < 48;
         off = pci_cap_next(d, off, &id), guard++) {
        if (id != 0x09) continue;
        uint8_t type = pci_read8(d, (uint8_t)(off + 3));
        if (type == VIRTIO_PCI_CAP_COMMON_CFG && !cap_common) cap_common = off;
        if (type == VIRTIO_PCI_CAP_NOTIFY_CFG && !cap_notify) cap_notify = off;
        if (type == VIRTIO_PCI_CAP_DEVICE_CFG && !cap_dev) cap_dev = off;
    }
    if (!cap_common || !cap_notify) {
        printk("[VGPU] no virtio 1.0 PCI capabilities\n");
        return 0;
    }
    /* memory decode and bus mastering on, INTx off (the queue is polled) */
    uint32_t cmdreg = pci_read_config32(d->bus, d->slot, d->func, 0x04);
    pci_write_config32(d->bus, d->slot, d->func, 0x04, (cmdreg & 0xFFFF) | 0x6 | 0x400);

    common = map_cap(d, cap_common, 0x38);
    volatile uint8_t *notify_base = map_cap(d, cap_notify, 2);
    devcfg = cap_dev ? map_cap(d, cap_dev, 16) : 0;
    uint32_t notify_mult = pci_read_config32(d->bus, d->slot, d->func,
                                             (uint8_t)(cap_notify + 16));
    if (!common || !notify_base) return 0;

    /* 3.1.1 Driver Requirements: Device Initialization */
    w8(VC_DEVICE_STATUS, 0);
    for (int i = 0; i < 1000000 && c8(VC_DEVICE_STATUS); i++)
        __asm__ volatile("pause");
    w8(VC_DEVICE_STATUS, VS_ACKNOWLEDGE);
    w8(VC_DEVICE_STATUS, VS_ACKNOWLEDGE | VS_DRIVER);
    w32(VC_DEVICE_FEATURE_SELECT, 1);
    uint32_t feat_hi = c32(VC_DEVICE_FEATURE);
    if (!(feat_hi & VIRTIO_F_VERSION_1_HI)) {
        printk("[VGPU] device lacks VIRTIO_F_VERSION_1\n");
        w8(VC_DEVICE_STATUS, VS_FAILED);
        return 0;
    }
    w32(VC_DRIVER_FEATURE_SELECT, 0);
    w32(VC_DRIVER_FEATURE, 0);              /* no EDID, no virgl */
    w32(VC_DRIVER_FEATURE_SELECT, 1);
    w32(VC_DRIVER_FEATURE, VIRTIO_F_VERSION_1_HI);
    w8(VC_DEVICE_STATUS, VS_ACKNOWLEDGE | VS_DRIVER | VS_FEATURES_OK);
    if (!(c8(VC_DEVICE_STATUS) & VS_FEATURES_OK)) {
        printk("[VGPU] features not accepted\n");
        w8(VC_DEVICE_STATUS, VS_FAILED);
        return 0;
    }
    w16(VC_MSIX_CONFIG, VIRTIO_MSI_NO_VECTOR);

    /* control queue (0) */
    w16(VC_QUEUE_SELECT, 0);
    uint16_t qmax = c16(VC_QUEUE_SIZE);
    if (qmax < 4) {
        w8(VC_DEVICE_STATUS, VS_FAILED);
        return 0;
    }
    memset(vq_mem, 0, sizeof(vq_mem));
    vq_desc  = (volatile struct virtq_desc *)vq_mem;
    vq_avail = (volatile struct virtq_avail *)(vq_mem + 16 * QSIZE);
    vq_used  = (volatile struct virtq_used *)(vq_mem + 2048);
    w16(VC_QUEUE_SIZE, qmax < QSIZE ? qmax : QSIZE);
    if (c16(VC_QUEUE_SIZE) != QSIZE) {
        printk("[VGPU] queue size %u refused\n", (unsigned)QSIZE);
        w8(VC_DEVICE_STATUS, VS_FAILED);
        return 0;
    }
    vq_avail->flags = VIRTQ_AVAIL_F_NO_INTERRUPT;
    w16(VC_QUEUE_MSIX_VECTOR, VIRTIO_MSI_NO_VECTOR);
    w64(VC_QUEUE_DESC, kphys((const void *)vq_desc));
    w64(VC_QUEUE_DRIVER, kphys((const void *)vq_avail));
    w64(VC_QUEUE_DEVICE, kphys((const void *)vq_used));
    notify = notify_base + (uint32_t)c16(VC_QUEUE_NOTIFY_OFF) * notify_mult;
    w16(VC_QUEUE_ENABLE, 1);
    w8(VC_DEVICE_STATUS, VS_ACKNOWLEDGE | VS_DRIVER | VS_FEATURES_OK | VS_DRIVER_OK);
    spin_init(&vg_lock);

    /* The framebuffer: RAM frames, each holding the driver's reference so
     * a user mapping of /dev/fb0 (which takes and drops its own) never
     * frees one. */
    for (uint32_t i = 0; i < VG_FB_PAGES; i++) {
        fb_frames[i] = pmm_alloc_frame();
        if (!fb_frames[i]) {
            printk("[VGPU] out of memory for the framebuffer\n");
            for (uint32_t j = 0; j < i; j++) pmm_free_frame(fb_frames[j]);
            w8(VC_DEVICE_STATUS, VS_FAILED);
            return 0;
        }
        pmm_frame_incref(fb_frames[i]);
    }

    fb_var_screeninfo_t boot;
    int have_boot = framebuffer_available() &&
                    framebuffer_ioctl(FBIOGET_VSCREENINFO, &boot) == 0;
    if (framebuffer_attach(&vg_driver, 0, fb_frames, VG_FB_BYTES, 1) != 0) {
        printk("[VGPU] cannot map the framebuffer\n");
        return 0;
    }
    /* clear it: the frames held whatever they held */
    memset((void *)(uintptr_t)FB_WINDOW_START, 0, VG_FB_BYTES);

    framebuffer_modes_clear();
    framebuffer_add_standard_modes(VG_MAX_W, VG_MAX_H, VG_FB_BYTES);
    spin_lock(&vg_lock);
    uint32_t pw = 0, ph = 0;
    int have_pref = vg_display_info(&pw, &ph) == 0;
    spin_unlock(&vg_lock);
    if (have_pref) {
        framebuffer_set_preferred(pw, ph);
        if (pw <= VG_MAX_W && ph <= VG_MAX_H && (pw & 7) == 0)
            framebuffer_mode_add(pw, ph);
    }
    if (have_boot && boot.xres <= VG_MAX_W && boot.yres <= VG_MAX_H)
        framebuffer_mode_add(boot.xres, boot.yres);
    printk("[VGPU] virtio-gpu%s: control queue %u, display %ux%u\n",
           is_vga ? " (virtio-vga)" : "", (unsigned)QSIZE,
           (unsigned)pw, (unsigned)ph);

    /* Start in the display's own size (QEMU: the -device's xres/yres,
     * 1280x800 by default, or the window's); the boot loader's mode was for
     * the VGA side (often 640x480: the virtio VGA BIOS has few VBE modes)
     * and is only the fallback, then 1024x768. */
    int rc = -1;
    if (have_pref) rc = framebuffer_set_mode(pw, ph);
    if (rc != 0 && have_boot) rc = framebuffer_set_mode(boot.xres, boot.yres);
    if (rc != 0) rc = framebuffer_set_mode(1024, 768);
    if (rc != 0) printk("[VGPU] cannot set a mode\n");
    else vg_flush(0, 0, cur_w, cur_h);
    return 1;
}
