#pragma once
#include <termios.h>

#define TIOCNOTTY 0x5422
#define FIONREAD  0x541B
#define FIONBIO   0x5421
#define BLKGETSIZE64 0x80081272

#define FBIOGET_VSCREENINFO 0x4600
#define FBIOPUT_VSCREENINFO 0x4601
#define FBIOGET_FSCREENINFO 0x4602
#define FB_ACTIVATE_TEST    2

/* MaeroOS fbdev extensions (drivers/framebuffer.h): the modes
 * FBIOPUT_VSCREENINFO accepts, and flushing changed rectangles to a display
 * that needs it (virtio-gpu: modes.flags & FB_MODES_FLUSH). */
#define FBIO_MAEROS_MODES   0x46E0
#define FBIO_MAEROS_FLUSH   0x46E1
#define FB_MAX_MODES        32
#define FB_FLUSH_MAX        16
#define FB_MODES_SETTABLE   1
#define FB_MODES_FLUSH      2

struct fb_mode { unsigned short w, h; };

struct fb_modelist {
    unsigned int count;
    unsigned int current;           /* index into modes; count if none */
    unsigned int flags;             /* FB_MODES_* */
    unsigned int generation;        /* changes with the list (hotplug) */
    char driver[16];                /* "boot", "bochs", "virtio-gpu" */
    unsigned short preferred_w, preferred_h;
    struct fb_mode modes[FB_MAX_MODES];
};

struct fb_rect { unsigned int x, y, w, h; };

struct fb_flush {
    unsigned int count;
    struct fb_rect rects[FB_FLUSH_MAX];
};
_Static_assert(sizeof(struct fb_modelist) == 164 && sizeof(struct fb_flush) == 260,
               "fb ioctl layouts must match drivers/framebuffer.h");

/* EXACT Linux UAPI layouts */
struct fb_fix_screeninfo {
    char id[16];
    unsigned int smem_start;
    unsigned int smem_len;
    unsigned int type;
    unsigned int type_aux;
    unsigned int visual;
    unsigned short xpanstep, ypanstep, ywrapstep, pad1;
    unsigned int line_length;
    unsigned int mmio_start;
    unsigned int mmio_len;
    unsigned int accel;
    unsigned short capabilities, reserved[2];
} __attribute__((packed));

struct fb_bitfield {
    unsigned int offset, length, msb_right;
};

struct fb_var_screeninfo {
    unsigned int xres, yres;
    unsigned int xres_virtual, yres_virtual;
    unsigned int xoffset, yoffset;
    unsigned int bits_per_pixel;
    unsigned int grayscale;
    struct fb_bitfield red, green, blue, transp;
    unsigned int nonstd;
    unsigned int activate;
    unsigned int height, width;
    unsigned int accel_flags;
    unsigned int pixclock;
    unsigned int left_margin, right_margin, upper_margin, lower_margin;
    unsigned int hsync_len, vsync_len;
    unsigned int sync, vmode, rotate, colorspace;
    unsigned int reserved[4];
} __attribute__((packed));
