#pragma once
#include <termios.h>

#define TIOCNOTTY 0x5422
#define BLKGETSIZE64 0x80081272

#define FBIOGET_VSCREENINFO 0x4600
#define FBIOGET_FSCREENINFO 0x4602

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
