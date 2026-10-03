/*
 * virtio-gpu, 2D only (QEMU -device virtio-gpu-pci or virtio-vga; PCI
 * 1af4:1050).
 *
 * Transport: the generic virtio PCI transport and split virtqueues
 * (drivers/virtio/virtio_pci.h, virtqueue.h); virtio-gpu is a modern-only
 * device.  One virtqueue, the control queue, is driven synchronously and
 * polled: a command is a two-descriptor chain (request, response), the
 * driver publishes, kicks and spins on the used ring.  Interrupts are off
 * (VIRTIO_IRQ_POLL: no MSI vector from the shared pool, INTx disabled,
 * VIRTQ_AVAIL_F_NO_INTERRUPT); the cursor queue is not used (the desktop
 * draws its own pointer).
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
#include "virtio/virtio_pci.h"
#include "../arch/i686/cpu/spinlock.h"
#include "../kernel/printk.h"
#include "../lib/string.h"
#include "../mm/pmm.h"
#include <kernel/boot_info.h>
#include <kernel/config.h>
#include <stdint.h>

#define QSIZE 16                /* plenty for two in-flight commands */

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

/* Command memory: in the kernel image, so each page is physically
 * contiguous and below 4 GiB.  Two command slots, each a request page and a
 * response page. */
static uint8_t cmd_mem[2][4096] __attribute__((aligned(4096)));
static uint8_t resp_mem[2][4096] __attribute__((aligned(4096)));

static struct virtio_pci vp;
static struct virtqueue ctrlq;
static spinlock_t vg_lock;
static int vg_dead;                   /* a command timed out: stop talking */

static uint32_t fb_frames[VG_FB_PAGES];
static uint32_t cur_res, cur_w, cur_h;
static uint32_t next_res = 1;

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
        memset(resp_mem[i], 0, sizeof(struct gpu_hdr));
        struct virtq_buf b[2] = {
            { virtio_virt_to_phys(cmd_mem[i]), req_len[i] },
            { virtio_virt_to_phys(resp_mem[i]), resp_len[i] },
        };
        if (virtq_add(&ctrlq, b, 1, 1, cmd_mem[i]) < 0) {
            printk("[VGPU] control queue full; display driver stopped\n");
            vg_dead = 1;
            return -5;
        }
    }
    virtio_pci_publish(&vp, &ctrlq);
    /* QEMU answers within microseconds under KVM; TCG and a busy host
     * get a generous budget before the device is given up on. */
    for (int done = 0; done < n;) {
        for (uint32_t spin = 0; !virtq_has_used(&ctrlq); spin++) {
            if (spin > 400000000U) {
                printk("[VGPU] control queue timeout; display driver stopped\n");
                vg_dead = 1;
                return -5;
            }
            __asm__ volatile("pause" ::: "memory");
        }
        if (virtq_get_used(&ctrlq, 0)) done++;
    }
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
    if (vg_dead || !vp.device) return;
    if (!(virtio_pci_config32(&vp, 0) & VIRTIO_GPU_EVENT_DISPLAY)) return;   /* events_read */
    virtio_pci_config_write32(&vp, 4, VIRTIO_GPU_EVENT_DISPLAY);             /* events_clear */
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

int virtio_gpu_init(void) {
    const pci_device_t *d = virtio_pci_find(VIRTIO_ID_GPU, 0);
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

    /* 3.1.1 Driver Requirements: Device Initialization (no EDID, no virgl) */
    if (virtio_pci_probe(&vp, d, "VGPU") < 0) return 0;
    if (virtio_pci_set_features(&vp, VIRTIO_F_VERSION_1) < 0) return 0;
    virtio_pci_irq_setup(&vp, VIRTIO_IRQ_POLL, 0, 0);
    /* Polled: INTx off too, so a display event raises no interrupt. */
    uint32_t cmdreg = pci_read_config32(d->bus, d->slot, d->func, 0x04);
    pci_write_config32(d->bus, d->slot, d->func, 0x04, (cmdreg & 0xFFFF) | 0x400);

    /* control queue (0) */
    if (virtio_pci_queue_setup(&vp, &ctrlq, 0, QSIZE) < 0 || ctrlq.size < 4) {
        printk("[VGPU] no usable control queue\n");
        virtio_pci_fail(&vp);
        return 0;
    }
    virtq_disable_cb(&ctrlq);
    virtio_pci_driver_ok(&vp);
    spin_init(&vg_lock);

    /* The framebuffer: RAM frames, each holding the driver's reference so
     * a user mapping of /dev/fb0 (which takes and drops its own) never
     * frees one. */
    for (uint32_t i = 0; i < VG_FB_PAGES; i++) {
        fb_frames[i] = pmm_alloc_frame();
        if (!fb_frames[i]) {
            printk("[VGPU] out of memory for the framebuffer\n");
            for (uint32_t j = 0; j < i; j++) pmm_free_frame(fb_frames[j]);
            virtio_pci_fail(&vp);
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
           is_vga ? " (virtio-vga)" : "", (unsigned)ctrlq.size,
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
