#include "framebuffer.h"
#include "../arch/i686/mm/paging.h"
#include "../kernel/printk.h"
#include "../kernel/panic.h"
#include "../lib/string.h"
#include <kernel/config.h>
#include <stdint.h>
#include <stddef.h>
#include <kernel/kprof.h>
#include "../arch/i686/cpu/spinlock.h"
#include "../arch/i686/cpu/pit.h"
#include "../proc/process.h"
#include "../proc/scheduler.h"

/* The framebuffer window: from the end of the kernel heap (HEAP_MAX) up to
 * where the identity-mapped local APIC page (0xFEE00000) and the recursive
 * page tables (0xFFC00000) begin.  256 MiB holds any real mode many times
 * over; a framebuffer claiming more than that is not one we can map. */
#define FB_VIRT_BASE  ((uint32_t)FB_WINDOW_START)
#define FB_VIRT_LIMIT ((uint32_t)FB_WINDOW_END)

typedef struct {
    uint32_t phys;
    uint32_t virt;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint32_t bpp;
    uint32_t size;
    uint8_t red_pos;
    uint8_t red_size;
    uint8_t green_pos;
    uint8_t green_size;
    uint8_t blue_pos;
    uint8_t blue_size;
    int ready;
    /* Set by framebuffer_attach: a mode-setting driver, the memory behind
     * the window (contiguous at phys, or the frames in pages[]) and its
     * length. */
    const fb_driver_t *drv;
    const uint32_t *pages;
    uint32_t map_bytes;
    int cached;
} framebuffer_state_t;

static framebuffer_state_t fb;

/* Modes FBIOPUT_VSCREENINFO accepts, smallest first. */
static fb_mode_t modes[FB_MAX_MODES];
static uint32_t nmodes;
static uint32_t modes_gen;
static uint16_t pref_w, pref_h;

/* Framebuffer bytes written through write(2) and not flushed yet, as a
 * scanline range; the flush thread pushes them for clients that do not flush
 * themselves (fbtest).  fb_mmapped: a client draws through mmap, so the thread
 * pushes the whole screen (fbDOOM).  Both only matter with a flush driver. */
static spinlock_t fb_lock;
static uint32_t dirty_y0, dirty_y1;
static int fb_mmapped;

void framebuffer_init(const multiboot_info_t *mbi) {
    memset(&fb, 0, sizeof(fb));

    if (!mbi || !(mbi->flags & MULTIBOOT_FLAG_FB)) {
        printk("[FB]   no multiboot framebuffer; VGA text fallback only\n");
        return;
    }

    if (mbi->framebuffer_type != 1 || mbi->framebuffer_bpp == 0 ||
        mbi->framebuffer_pitch == 0 || mbi->framebuffer_width == 0 ||
        mbi->framebuffer_height == 0) {
        printk("[FB]   unsupported framebuffer type=%u bpp=%u\n",
               (unsigned)mbi->framebuffer_type,
               (unsigned)mbi->framebuffer_bpp);
        return;
    }

    if (mbi->framebuffer_addr > 0xFFFFFFFFULL) {
        printk("[FB]   framebuffer above 32-bit address space\n");
        return;
    }

    /* The geometry comes straight from the bootloader.  pitch * height is
     * what gets mapped at FB_VIRT_BASE, so check it in 64 bits against the
     * window before trusting it: a wrapped or oversized product would map
     * over the APIC page or the recursive page tables, or leave the tail
     * that the console writes to unmapped. */
    uint64_t size64 = (uint64_t)mbi->framebuffer_pitch * mbi->framebuffer_height;
    uint64_t row64 = ((uint64_t)mbi->framebuffer_width * mbi->framebuffer_bpp + 7) / 8;
    uint32_t page_off0 = (uint32_t)mbi->framebuffer_addr & 0xFFFU;
    if (mbi->framebuffer_bpp > 32 || row64 > mbi->framebuffer_pitch ||
        size64 + page_off0 > (uint64_t)(FB_VIRT_LIMIT - FB_VIRT_BASE) ||
        mbi->framebuffer_addr + size64 > 0x100000000ULL) {
        printk("[FB]   implausible framebuffer %ux%u@%u pitch=%u; VGA text fallback only\n",
               (unsigned)mbi->framebuffer_width, (unsigned)mbi->framebuffer_height,
               (unsigned)mbi->framebuffer_bpp, (unsigned)mbi->framebuffer_pitch);
        return;
    }

    fb.phys = (uint32_t)mbi->framebuffer_addr;
    fb.virt = FB_VIRT_BASE;
    fb.pitch = mbi->framebuffer_pitch;
    fb.width = mbi->framebuffer_width;
    fb.height = mbi->framebuffer_height;
    fb.bpp = mbi->framebuffer_bpp;
    fb.size = (uint32_t)size64;

    /*
     * Multiboot places RGB channel metadata immediately after
     * framebuffer_type for direct RGB framebuffers.
     */
    const uint8_t *rgb = (const uint8_t *)&mbi->framebuffer_type + 1;
    fb.red_pos = rgb[0];
    fb.red_size = rgb[1];
    fb.green_pos = rgb[2];
    fb.green_size = rgb[3];
    fb.blue_pos = rgb[4];
    fb.blue_size = rgb[5];

    uint32_t phys_page = fb.phys & ~0xFFFU;
    uint32_t page_off = fb.phys & 0xFFFU;
    uint32_t map_len = (fb.size + page_off + 0xFFFU) & ~0xFFFU;

    /* FATAL by design (audit category (c)): the framebuffer is mapped once
     * from framebuffer_init during boot, from the multiboot info, before any
     * process exists.  A half-mapped framebuffer would be written past its
     * mapped tail on the first console scroll. */
    for (uint32_t off = 0; off < map_len; off += PAGE_SIZE) {
        if (paging_map(FB_VIRT_BASE + off, phys_page + off,
                       PAGE_PRESENT | PAGE_WRITABLE | PAGE_NOCACHE | PAGE_NX) != 0)
            panic("framebuffer_init: cannot map the framebuffer", NULL);
    }
    fb.virt = FB_VIRT_BASE + page_off;
    fb.ready = 1;
    if (fb.bpp == 32) framebuffer_mode_add(fb.width, fb.height);

    printk("[FB]   %ux%u@%u pitch=%u phys=0x%08x virt=0x%08x\n",
           (unsigned)fb.width, (unsigned)fb.height, (unsigned)fb.bpp,
           (unsigned)fb.pitch, (unsigned)fb.phys, (unsigned)fb.virt);
}

uint32_t framebuffer_phys(void) {
    return fb.phys;
}

uint32_t framebuffer_page_phys(uint32_t off) {
    if (!fb.ready) return 0;
    if (fb.drv) {
        if (off >= fb.map_bytes) return 0;
        return fb.pages ? fb.pages[off / PAGE_SIZE]
                        : (fb.phys & ~0xFFFU) + (off & ~0xFFFU);
    }
    if (off >= ((fb.size + (fb.phys & 0xFFFU) + 0xFFFU) & ~0xFFFU)) return 0;
    return (fb.phys & ~0xFFFU) + (off & ~0xFFFU);
}

int framebuffer_cached(void) {
    return fb.cached;
}

void framebuffer_note_mmap(void) {
    if (fb.drv && fb.drv->flush) fb_mmapped = 1;
}

int framebuffer_available(void) {
    return fb.ready;
}

uint32_t framebuffer_size(void) {
    return fb.ready ? fb.size : 0;
}

uint32_t framebuffer_read(uint32_t off, uint32_t len, uint8_t *buf) {
    if (!fb.ready || !buf || off >= fb.size) return 0;
    if (len > fb.size - off) len = fb.size - off;
    memcpy(buf, (const void *)(uintptr_t)(fb.virt + off), len);
    return len;
}

uint32_t framebuffer_write(uint32_t off, uint32_t len, const uint8_t *buf) {
    if (!fb.ready || !buf || off >= fb.size) return 0;
    if (len > fb.size - off) len = fb.size - off;
    /* The framebuffer is device memory, so this copy runs at MMIO speed, not
     * RAM speed; kprof gives it its own bucket because at a full-screen blit
     * per frame it is one of the two largest costs of a Firefox startup. */
    kprof_add(KPE_FB_KB, len >> 10);
    int kp_old = kprof_switch(KPB_FB);
    memcpy((void *)(uintptr_t)(fb.virt + off), buf, len);
    kprof_switch(kp_old);
    if (fb.drv && fb.drv->flush && len) {
        uint32_t y0 = off / fb.pitch, y1 = (off + len - 1) / fb.pitch + 1;
        spin_lock(&fb_lock);
        if (dirty_y0 >= dirty_y1) { dirty_y0 = y0; dirty_y1 = y1; }
        else {
            if (y0 < dirty_y0) dirty_y0 = y0;
            if (y1 > dirty_y1) dirty_y1 = y1;
        }
        spin_unlock(&fb_lock);
    }
    return len;
}

/* ── modes ──────────────────────────────────────────────────────────────── */

void framebuffer_modes_clear(void) {
    nmodes = 0;
    modes_gen++;
}

void framebuffer_mode_add(uint32_t w, uint32_t h) {
    if (!w || !h || w > 0xFFFF || h > 0xFFFF) return;
    uint32_t i;
    for (i = 0; i < nmodes; i++)
        if (modes[i].w == w && modes[i].h == h) return;
    if (nmodes >= FB_MAX_MODES) return;
    /* keep the list ordered by area, then width */
    for (i = nmodes; i > 0; i--) {
        uint32_t a = (uint32_t)modes[i - 1].w * modes[i - 1].h;
        if (a < w * h || (a == w * h && modes[i - 1].w < w)) break;
        modes[i] = modes[i - 1];
    }
    modes[i].w = (uint16_t)w;
    modes[i].h = (uint16_t)h;
    nmodes++;
    modes_gen++;
}

void framebuffer_set_preferred(uint32_t w, uint32_t h) {
    if (pref_w != w || pref_h != h) modes_gen++;
    pref_w = (uint16_t)w;
    pref_h = (uint16_t)h;
}

/* The usual VESA/DMT and laptop panel sizes.  Widths are multiples of 8:
 * Bochs DISPI rejects anything else (so no 1366x768). */
static const fb_mode_t standard_modes[] = {
    { 640, 480 }, { 800, 600 }, { 1024, 768 }, { 1152, 864 },
    { 1280, 720 }, { 1280, 800 }, { 1280, 1024 }, { 1360, 768 },
    { 1440, 900 }, { 1600, 900 }, { 1600, 1200 }, { 1680, 1050 },
    { 1920, 1080 }, { 1920, 1200 }, { 2560, 1440 }, { 2560, 1600 },
};

void framebuffer_add_standard_modes(uint32_t max_w, uint32_t max_h,
                                    uint32_t max_bytes) {
    for (uint32_t i = 0; i < sizeof(standard_modes) / sizeof(standard_modes[0]); i++) {
        uint32_t w = standard_modes[i].w, h = standard_modes[i].h;
        if (w <= max_w && h <= max_h && (uint64_t)w * h * 4 <= max_bytes)
            framebuffer_mode_add(w, h);
    }
}

static int mode_known(uint32_t w, uint32_t h) {
    for (uint32_t i = 0; i < nmodes; i++)
        if (modes[i].w == w && modes[i].h == h) return 1;
    return 0;
}

int framebuffer_set_mode(uint32_t w, uint32_t h) {
    uint32_t pitch = 0;

    if (!fb.drv || !fb.drv->set_mode) return -22;
    if (!mode_known(w, h) || (uint64_t)w * h * 4 > fb.map_bytes) return -22;
    int rc = fb.drv->set_mode(w, h, &pitch);
    if (rc < 0) return rc;
    if (pitch < w * 4 || (uint64_t)pitch * h > fb.map_bytes) return -5;
    spin_lock(&fb_lock);
    fb.width = w;
    fb.height = h;
    fb.pitch = pitch;
    fb.bpp = 32;
    fb.size = pitch * h;
    fb.red_pos = 16;   fb.red_size = 8;
    fb.green_pos = 8;  fb.green_size = 8;
    fb.blue_pos = 0;   fb.blue_size = 8;
    dirty_y0 = dirty_y1 = 0;
    spin_unlock(&fb_lock);
    printk("[FB]   %s: mode %ux%u pitch=%u\n", fb.drv->name, (unsigned)w,
           (unsigned)h, (unsigned)pitch);
    return 0;
}

int framebuffer_attach(const fb_driver_t *drv, uint32_t phys,
                       const uint32_t *pages, uint32_t map_bytes, int cached) {
    uint32_t len = (map_bytes + (phys & 0xFFFU) + 0xFFFU) & ~0xFFFU;
    uint32_t old_len = fb.ready ?
        ((fb.size + (fb.phys & 0xFFFU) + 0xFFFU) & ~0xFFFU) : 0;
    pte_t flags = PAGE_PRESENT | PAGE_WRITABLE | PAGE_NX |
                  (cached ? 0 : PAGE_NOCACHE);

    if (!drv || !map_bytes || len > FB_VIRT_LIMIT - FB_VIRT_BASE) return -1;
    if (paging_reserve_range(FB_VIRT_BASE, FB_VIRT_BASE + len, 0) != 0) return -1;
    for (uint32_t off = 0; off < len; off += PAGE_SIZE) {
        uint32_t p = pages ? pages[off / PAGE_SIZE] : (phys & ~0xFFFU) + off;
        if (paging_map(FB_VIRT_BASE + off, p, flags) != 0) return -1;
    }
    /* drop what is left of the boot loader's mapping past the new one */
    for (uint32_t off = len; off < old_len; off += PAGE_SIZE)
        paging_unmap(FB_VIRT_BASE + off);
    spin_init(&fb_lock);
    fb.drv = drv;
    fb.pages = pages;
    fb.phys = pages ? pages[0] : phys;
    fb.virt = FB_VIRT_BASE + (pages ? 0 : (phys & 0xFFFU));
    fb.map_bytes = map_bytes;
    fb.cached = cached;
    fb.ready = 1;
    printk("[FB]   %s takes over: %u KiB at 0x%08x%s\n", drv->name,
           (unsigned)(map_bytes >> 10), (unsigned)fb.phys,
           pages ? " (scattered)" : "");
    return 0;
}

/* ── flushing (virtio-gpu) ──────────────────────────────────────────────── */

static void flush_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (x >= fb.width || y >= fb.height || !w || !h) return;
    if (w > fb.width - x) w = fb.width - x;
    if (h > fb.height - y) h = fb.height - y;
    fb.drv->flush(x, y, w, h);
}

static void fbflushd(void) {
    for (;;) {
        if (fb.drv->poll) fb.drv->poll();
        if (fb.drv->flush) {
            uint32_t y0, y1;
            spin_lock(&fb_lock);
            y0 = dirty_y0; y1 = dirty_y1;
            dirty_y0 = dirty_y1 = 0;
            spin_unlock(&fb_lock);
            if (fb_mmapped) flush_rect(0, 0, fb.width, fb.height);
            else if (y0 < y1) flush_rect(0, y0, fb.width, y1 - y0);
        }
        current_proc->wake_tick = pit_ticks() + 4;    /* 25 Hz */
        sleep_on(&fb_mmapped);
    }
}

void framebuffer_start_thread(void) {
    if (fb.drv && (fb.drv->flush || fb.drv->poll))
        proc_create_kthread(fbflushd, "fbflushd");
}

/* ── /dev/fb0 ioctls ─────────────────────────────────────────────────────── */

int framebuffer_ioctl(uint32_t req, void *arg) {
    if (!fb.ready) return -19;
    if (!arg) return -14;

    if (req == FBIOGET_FSCREENINFO) {
        fb_fix_screeninfo_t *fix = (fb_fix_screeninfo_t *)arg;
        memset(fix, 0, sizeof(*fix));
        memcpy(fix->id, "maeros-fb", 10);
        fix->smem_start = fb.phys;
        fix->smem_len = fb.size;
        fix->type = 0;             /* FB_TYPE_PACKED_PIXELS */
        fix->visual = 2;           /* FB_VISUAL_TRUECOLOR */
        fix->line_length = fb.pitch;
        return 0;
    }

    if (req == FBIOGET_VSCREENINFO) {
        fb_var_screeninfo_t *var = (fb_var_screeninfo_t *)arg;
        memset(var, 0, sizeof(*var));
        var->xres = fb.width;
        var->yres = fb.height;
        var->xres_virtual = fb.width;
        var->yres_virtual = fb.height;
        var->bits_per_pixel = fb.bpp;
        var->red.offset = fb.red_pos;
        var->red.length = fb.red_size;
        var->green.offset = fb.green_pos;
        var->green.length = fb.green_size;
        var->blue.offset = fb.blue_pos;
        var->blue.length = fb.blue_size;
        var->height = 0xFFFFFFFFu;   /* unknown physical size (-1) */
        var->width = 0xFFFFFFFFu;
        return 0;
    }

    /* FBIOPUT_VSCREENINFO: xres x yres at 32 bpp from the mode list (no
     * panning: the virtual size is the visible one).  The current mode is
     * always accepted, so a client that puts back what it got succeeds on
     * the boot framebuffer too.  Like Linux, the resulting var is written
     * back. */
    if (req == FBIOPUT_VSCREENINFO) {
        fb_var_screeninfo_t *var = (fb_var_screeninfo_t *)arg;
        uint32_t w = var->xres, h = var->yres;
        int same = w == fb.width && h == fb.height;
        if (var->bits_per_pixel && var->bits_per_pixel != fb.bpp &&
            !(fb.drv && var->bits_per_pixel == 32))
            return -22;
        if ((var->xres_virtual && var->xres_virtual != w) ||
            (var->yres_virtual && var->yres_virtual != h) ||
            var->xoffset || var->yoffset)
            return -22;
        if (!same && (!fb.drv || !mode_known(w, h))) return -22;
        if (!(var->activate & FB_ACTIVATE_TEST) && !same) {
            int rc = framebuffer_set_mode(w, h);
            if (rc < 0) return rc;
        }
        if (var->activate & FB_ACTIVATE_TEST) return 0;
        return framebuffer_ioctl(FBIOGET_VSCREENINFO, arg);
    }

    if (req == FBIO_MAEROS_MODES) {
        fb_modelist_t *ml = (fb_modelist_t *)arg;
        memset(ml, 0, sizeof(*ml));
        ml->count = nmodes;
        ml->current = nmodes;
        for (uint32_t i = 0; i < nmodes; i++) {
            ml->modes[i] = modes[i];
            if (modes[i].w == fb.width && modes[i].h == fb.height) ml->current = i;
        }
        ml->flags = (fb.drv ? FB_MODES_SETTABLE : 0) |
                    (fb.drv && fb.drv->flush ? FB_MODES_FLUSH : 0);
        ml->generation = modes_gen;
        strncpy(ml->driver, fb.drv ? fb.drv->name : "boot", sizeof(ml->driver) - 1);
        ml->preferred_w = pref_w;
        ml->preferred_h = pref_h;
        return 0;
    }

    if (req == FBIO_MAEROS_FLUSH) {
        const fb_flush_t *fl = (const fb_flush_t *)arg;
        if (fl->count > FB_FLUSH_MAX) return -22;
        if (!fb.drv || !fb.drv->flush) return 0;
        fb_mmapped = 0;        /* a compositor owns the screen again */
        for (uint32_t i = 0; i < fl->count; i++) {
            fb_rect_t r;
            memcpy(&r, (const uint8_t *)fl->rects + i * sizeof(r), sizeof(r));
            flush_rect(r.x, r.y, r.w, r.h);
        }
        /* write(2)s are now flushed by whoever wrote them */
        spin_lock(&fb_lock);
        dirty_y0 = dirty_y1 = 0;
        spin_unlock(&fb_lock);
        return 0;
    }

    return -25;
}
