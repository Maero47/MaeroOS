#pragma once
#include <kernel/multiboot.h>
#include <stdint.h>

#define FBIOGET_VSCREENINFO 0x4600
#define FBIOPUT_VSCREENINFO 0x4601
#define FBIOGET_FSCREENINFO 0x4602

/* fb_var_screeninfo.activate: FB_ACTIVATE_TEST only checks the mode. */
#define FB_ACTIVATE_TEST    2

/*
 * MaeroOS extensions in the fbdev ioctl range ('F').  Linux lists modes in
 * sysfs (/sys/class/graphics/fb0/modes) and has no flush ioctl (DRM's
 * DIRTYFB is the closest); these are the two things a mode-setting desktop on
 * a virtio-gpu needs that fbdev does not have.
 *
 * FBIO_MAEROS_MODES  out: fb_modelist_t, the modes FBIOPUT_VSCREENINFO
 *                    accepts (32 bpp each) and which one is current.
 * FBIO_MAEROS_FLUSH  in:  fb_flush_t, rectangles of the framebuffer that
 *                    changed since the last flush.  Needed when modes.flags
 *                    has FB_MODES_FLUSH (virtio-gpu: the scanout is a host
 *                    resource and only flushed rectangles reach it); a no-op
 *                    otherwise.
 */
#define FBIO_MAEROS_MODES   0x46E0
#define FBIO_MAEROS_FLUSH   0x46E1

#define FB_MAX_MODES        32
#define FB_FLUSH_MAX        16

#define FB_MODES_SETTABLE   1   /* a driver can change the mode */
#define FB_MODES_FLUSH      2   /* writes reach the screen only when flushed */

typedef struct { uint16_t w, h; } fb_mode_t;

typedef struct {
    uint32_t  count;
    uint32_t  current;          /* index into modes; count if none matches */
    uint32_t  flags;            /* FB_MODES_* */
    uint32_t  generation;       /* changes when the list does (hotplug) */
    char      driver[16];       /* "boot", "bochs", "virtio-gpu" */
    uint16_t  preferred_w, preferred_h;  /* the display's own, 0 = unknown */
    fb_mode_t modes[FB_MAX_MODES];
} __attribute__((packed)) fb_modelist_t;

typedef struct { uint32_t x, y, w, h; } fb_rect_t;

typedef struct {
    uint32_t  count;            /* rects used, at most FB_FLUSH_MAX */
    fb_rect_t rects[FB_FLUSH_MAX];
} __attribute__((packed)) fb_flush_t;

/* EXACT Linux UAPI layouts (real Linux binaries read these by offset). */
typedef struct {
    char     id[16];
    uint32_t smem_start;
    uint32_t smem_len;
    uint32_t type;
    uint32_t type_aux;
    uint32_t visual;
    uint16_t xpanstep, ypanstep, ywrapstep;
    uint16_t pad1;
    uint32_t line_length;
    uint32_t mmio_start;
    uint32_t mmio_len;
    uint32_t accel;
    uint16_t capabilities;
    uint16_t reserved[2];
} __attribute__((packed)) fb_fix_screeninfo_t;

typedef struct { uint32_t offset, length, msb_right; } fb_bitfield_t;

typedef struct {
    uint32_t xres, yres;
    uint32_t xres_virtual, yres_virtual;
    uint32_t xoffset, yoffset;
    uint32_t bits_per_pixel;
    uint32_t grayscale;
    fb_bitfield_t red, green, blue, transp;
    uint32_t nonstd;
    uint32_t activate;
    uint32_t height, width;
    uint32_t accel_flags;
    uint32_t pixclock;
    uint32_t left_margin, right_margin, upper_margin, lower_margin;
    uint32_t hsync_len, vsync_len;
    uint32_t sync, vmode, rotate, colorspace;
    uint32_t reserved[4];
} __attribute__((packed)) fb_var_screeninfo_t;

/*
 * A display driver that can set modes (drivers/bochs_vga.c,
 * drivers/virtio_gpu.c).  It takes over from the boot loader's framebuffer
 * with framebuffer_attach() during boot, before devfs and the first process.
 */
typedef struct fb_driver {
    const char *name;
    /* Program a w x h, 32 bpp mode; *pitch gets the bytes per scanline.  The
     * pixels live in the memory given to framebuffer_attach.  0 or -errno. */
    int  (*set_mode)(uint32_t w, uint32_t h, uint32_t *pitch);
    /* Push a changed rectangle to the screen; NULL when the scanout reads
     * the framebuffer memory itself. */
    void (*flush)(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
    /* Check for display-configuration changes (may refill the mode list);
     * NULL when there are none.  Called from the flush thread. */
    void (*poll)(void);
} fb_driver_t;

void framebuffer_init(const multiboot_info_t *mbi);
int framebuffer_available(void);
uint32_t framebuffer_size(void);
uint32_t framebuffer_phys(void);   /* physical base (0 if unavailable) */
/* Physical address of the framebuffer page at byte offset `off` (the memory
 * of a virtio-gpu framebuffer is not contiguous); 0 past its end. */
uint32_t framebuffer_page_phys(uint32_t off);
/* 1: the framebuffer is RAM, map it cached (virtio-gpu); 0: device memory. */
int framebuffer_cached(void);
/* A user mapping of /dev/fb0 now exists: on a flush driver, the flush thread
 * pushes the whole screen until the next explicit flush. */
void framebuffer_note_mmap(void);
uint32_t framebuffer_read(uint32_t off, uint32_t len, uint8_t *buf);
uint32_t framebuffer_write(uint32_t off, uint32_t len, const uint8_t *buf);
int framebuffer_ioctl(uint32_t req, void *arg);

/* Driver side.  `phys` is a contiguous framebuffer (pages == NULL), else
 * pages[] lists `map_bytes / 4096` frames; either way it is mapped at the
 * framebuffer window (include/kernel/config.h) and must be at least as large
 * as the largest mode added.  Boot-time only.  Returns 0 or -1. */
int  framebuffer_attach(const fb_driver_t *drv, uint32_t phys,
                        const uint32_t *pages, uint32_t map_bytes, int cached);
void framebuffer_modes_clear(void);
void framebuffer_mode_add(uint32_t w, uint32_t h);
void framebuffer_set_preferred(uint32_t w, uint32_t h);
/* Program a mode through the attached driver (also used by the driver at
 * attach time to start from a known mode).  0 or -errno. */
int  framebuffer_set_mode(uint32_t w, uint32_t h);
/* The flush thread; started by kernel_main when the driver has flush/poll. */
void framebuffer_start_thread(void);
/* Common resolutions up to (max_w, max_h) and max_bytes at 32 bpp. */
void framebuffer_add_standard_modes(uint32_t max_w, uint32_t max_h,
                                    uint32_t max_bytes);
