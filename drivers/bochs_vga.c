/*
 * Bochs/QEMU "std" VGA (PCI 1234:1111) and VirtualBox VGA (80ee:beef):
 * runtime mode setting through the VBE DISPI interface.
 *
 * The boot loader already put the card in a linear 32 bpp mode (GRUB's
 * gfxpayload, Limine, or OVMF's QemuVideoDxe all go through the same DISPI
 * registers), with the framebuffer at BAR0.  This driver maps all of BAR0's
 * VRAM at the framebuffer window, lists the common resolutions that fit in it
 * and in the card's maximum (DISPI GETCAPS), and reprograms XRES/YRES/BPP on
 * FBIOPUT_VSCREENINFO.  The scanout reads VRAM itself, so there is nothing to
 * flush.
 *
 * Without a boot framebuffer at BAR0 (QEMU -kernel, a text-mode boot) the
 * driver stays out of the way: the machine keeps its VGA text console and
 * init does not start the desktop, as before.  "nomodeset" on the kernel
 * command line keeps it out too.
 *
 * Register interface: Bochs vbe.h / QEMU hw/display/vga.c (VBE_DISPI_*),
 * OSDev wiki "Bochs VBE Extensions".  The ports are the same on VirtualBox.
 */
#include "bochs_vga.h"
#include "framebuffer.h"
#include "pci.h"
#include "../arch/i686/include/io.h"
#include "../kernel/printk.h"
#include <kernel/boot_info.h>
#include <stdint.h>

#define VBE_DISPI_IOPORT_INDEX   0x01CE
#define VBE_DISPI_IOPORT_DATA    0x01CF

#define VBE_DISPI_INDEX_ID         0x0
#define VBE_DISPI_INDEX_XRES       0x1
#define VBE_DISPI_INDEX_YRES       0x2
#define VBE_DISPI_INDEX_BPP        0x3
#define VBE_DISPI_INDEX_ENABLE     0x4
#define VBE_DISPI_INDEX_BANK       0x5
#define VBE_DISPI_INDEX_VIRT_WIDTH 0x6
#define VBE_DISPI_INDEX_VIRT_HEIGHT 0x7
#define VBE_DISPI_INDEX_X_OFFSET   0x8
#define VBE_DISPI_INDEX_Y_OFFSET   0x9
#define VBE_DISPI_INDEX_VIDEO_MEMORY_64K 0xa   /* QEMU; VBox uses 0xa otherwise */

#define VBE_DISPI_ID0            0xB0C0
#define VBE_DISPI_ID5            0xB0C5

#define VBE_DISPI_ENABLED        0x01
#define VBE_DISPI_GETCAPS        0x02
#define VBE_DISPI_LFB_ENABLED    0x40
#define VBE_DISPI_NOCLEARMEM     0x80

static uint16_t dispi_id;

static uint16_t dispi_read(uint16_t idx) {
    outw(VBE_DISPI_IOPORT_INDEX, idx);
    return inw(VBE_DISPI_IOPORT_DATA);
}

static void dispi_write(uint16_t idx, uint16_t val) {
    outw(VBE_DISPI_IOPORT_INDEX, idx);
    outw(VBE_DISPI_IOPORT_DATA, val);
}

static int bochs_set_mode(uint32_t w, uint32_t h, uint32_t *pitch) {
    dispi_write(VBE_DISPI_INDEX_ENABLE, 0);
    dispi_write(VBE_DISPI_INDEX_BANK, 0);
    dispi_write(VBE_DISPI_INDEX_BPP, 32);
    dispi_write(VBE_DISPI_INDEX_XRES, (uint16_t)w);
    dispi_write(VBE_DISPI_INDEX_YRES, (uint16_t)h);
    dispi_write(VBE_DISPI_INDEX_VIRT_WIDTH, (uint16_t)w);
    dispi_write(VBE_DISPI_INDEX_VIRT_HEIGHT, (uint16_t)h);
    dispi_write(VBE_DISPI_INDEX_X_OFFSET, 0);
    dispi_write(VBE_DISPI_INDEX_Y_OFFSET, 0);
    dispi_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED);
    /* The card refuses what it cannot do (QEMU: a width that is not a
     * multiple of 8, more than its VRAM) by leaving the old value. */
    if (dispi_read(VBE_DISPI_INDEX_XRES) != w || dispi_read(VBE_DISPI_INDEX_YRES) != h ||
        dispi_read(VBE_DISPI_INDEX_BPP) != 32)
        return -22;
    uint32_t vw = dispi_read(VBE_DISPI_INDEX_VIRT_WIDTH);
    *pitch = (vw >= w ? vw : w) * 4;
    return 0;
}

static const fb_driver_t bochs_driver = {
    .name = "bochs",
    .set_mode = bochs_set_mode,
};

static int cmdline_has(const char *word) {
    const char *c = boot_info_cmdline();
    uint32_t n = 0;
    while (word[n]) n++;
    for (const char *p = c; p && *p; p++) {
        if (p != c && p[-1] != ' ') continue;
        uint32_t i = 0;
        while (i < n && p[i] == word[i]) i++;
        if (i == n && (p[n] == 0 || p[n] == ' ')) return 1;
    }
    return 0;
}

int bochs_vga_init(void) {
    const pci_device_t *d = pci_find_device(0x1234, 0x1111);
    const char *what = "std VGA";
    if (!d) { d = pci_find_device(0x80EE, 0xBEEF); what = "VirtualBox VGA"; }
    if (!d) return 0;
    if (cmdline_has("nomodeset")) {
        printk("[BOCHS] %s: nomodeset, keeping the boot framebuffer\n", what);
        return 0;
    }

    dispi_id = dispi_read(VBE_DISPI_INDEX_ID);
    if (dispi_id < VBE_DISPI_ID0 || dispi_id > 0xB0CF) {
        printk("[BOCHS] %s: no DISPI interface (id 0x%04x)\n", what, dispi_id);
        return 0;
    }
    uint32_t bar0 = d->bar[0];
    if (bar0 & 1) return 0;                          /* I/O BAR: not an LFB */
    if ((bar0 & 0x6) == 0x4 && d->bar[1]) {
        printk("[BOCHS] %s: BAR0 above 4 GiB\n", what);
        return 0;
    }
    bar0 &= ~0xFU;
    uint32_t vram = pci_bar_size(d, 0);
    if (dispi_id >= VBE_DISPI_ID5 && d->vendor_id == 0x1234) {
        uint32_t v64k = dispi_read(VBE_DISPI_INDEX_VIDEO_MEMORY_64K);
        if (v64k && v64k * 65536U < vram) vram = v64k * 65536U;
    }
    if (vram > 0x10000000U) vram = 0x10000000U;     /* the framebuffer window */
    if (vram < 0x100000U) return 0;

    /* Take over only the screen the boot loader set up on this card. */
    if (!framebuffer_available() || (framebuffer_phys() & ~0xFFFU) != bar0) {
        printk("[BOCHS] %s: boot framebuffer is not at BAR0 0x%08x; not taking over\n",
               what, (unsigned)bar0);
        return 0;
    }
    fb_var_screeninfo_t var;
    if (framebuffer_ioctl(FBIOGET_VSCREENINFO, &var) != 0) return 0;

    /* The card's limits: with GETCAPS set, XRES/YRES/BPP read as maxima. */
    uint16_t en = dispi_read(VBE_DISPI_INDEX_ENABLE);
    dispi_write(VBE_DISPI_INDEX_ENABLE, en | VBE_DISPI_GETCAPS);
    uint32_t max_w = dispi_read(VBE_DISPI_INDEX_XRES);
    uint32_t max_h = dispi_read(VBE_DISPI_INDEX_YRES);
    dispi_write(VBE_DISPI_INDEX_ENABLE, en);
    if (max_w < 640 || max_h < 480) { max_w = 1920; max_h = 1200; }

    if (framebuffer_attach(&bochs_driver, bar0, 0, vram, 0) != 0) {
        printk("[BOCHS] %s: cannot map %u KiB of VRAM\n", what, (unsigned)(vram >> 10));
        return 0;
    }
    framebuffer_modes_clear();
    framebuffer_add_standard_modes(max_w, max_h, vram);
    if (var.bits_per_pixel == 32 && (var.xres & 7) == 0 &&
        (uint64_t)var.xres * var.yres * 4 <= vram)
        framebuffer_mode_add(var.xres, var.yres);
    printk("[BOCHS] %s DISPI id 0x%04x, %u KiB VRAM at 0x%08x, max %ux%u\n",
           what, dispi_id, (unsigned)(vram >> 10), (unsigned)bar0,
           (unsigned)max_w, (unsigned)max_h);
    /* Reprogram the boot mode so the driver's idea of the pitch is the
     * card's; if the boot loader's mode is not one DISPI does, 1024x768. */
    if (framebuffer_set_mode(var.xres, var.yres) != 0 &&
        framebuffer_set_mode(1024, 768) != 0)
        printk("[BOCHS] cannot set a mode\n");
    return 1;
}
