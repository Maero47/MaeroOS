#pragma once
#include <kernel/multiboot.h>
#include <stdint.h>

#define FBIOGET_VSCREENINFO 0x4600
#define FBIOGET_FSCREENINFO 0x4602

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

void framebuffer_init(const multiboot_info_t *mbi);
int framebuffer_available(void);
uint32_t framebuffer_size(void);
uint32_t framebuffer_phys(void);   /* physical base (0 if unavailable) */
uint32_t framebuffer_read(uint32_t off, uint32_t len, uint8_t *buf);
uint32_t framebuffer_write(uint32_t off, uint32_t len, const uint8_t *buf);
int framebuffer_ioctl(uint32_t req, void *arg);
