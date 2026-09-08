#include "framebuffer.h"
#include "../arch/i686/mm/paging.h"
#include "../kernel/printk.h"
#include "../lib/string.h"
#include <kernel/config.h>
#include <stdint.h>
#include <stddef.h>
#include <kernel/kprof.h>

#define FB_VIRT_BASE 0xE0000000U

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
} framebuffer_state_t;

static framebuffer_state_t fb;

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

    fb.phys = (uint32_t)mbi->framebuffer_addr;
    fb.virt = FB_VIRT_BASE;
    fb.pitch = mbi->framebuffer_pitch;
    fb.width = mbi->framebuffer_width;
    fb.height = mbi->framebuffer_height;
    fb.bpp = mbi->framebuffer_bpp;
    fb.size = fb.pitch * fb.height;

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

    for (uint32_t off = 0; off < map_len; off += PAGE_SIZE) {
        paging_map(FB_VIRT_BASE + off, phys_page + off,
                   PAGE_PRESENT | PAGE_WRITABLE | PAGE_NOCACHE);
    }
    fb.virt = FB_VIRT_BASE + page_off;
    fb.ready = 1;

    printk("[FB]   %ux%u@%u pitch=%u phys=0x%08x virt=0x%08x\n",
           (unsigned)fb.width, (unsigned)fb.height, (unsigned)fb.bpp,
           (unsigned)fb.pitch, (unsigned)fb.phys, (unsigned)fb.virt);
}

uint32_t framebuffer_phys(void) {
    return fb.phys;
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
    return len;
}

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

    /* FBIOPUT_VSCREENINFO: accept (we only have one mode) */
    if (req == 0x4601)
        return 0;

    return -25;
}
