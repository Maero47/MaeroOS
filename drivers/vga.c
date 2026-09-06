#include "vga.h"
#include <io.h>
#include <stdint.h>

/* Physical 0xB8000 mapped in boot.asm at PTE index 184 of boot_page_table1.
 * Its virtual address after the higher-half switch is 0xC00B8000. */
#define VGA_BUFFER ((volatile uint16_t *)0xC00B8000)

#define VGA_COLS 80
#define VGA_ROWS 25

/* CRTC ports for hardware cursor */
#define CRTC_ADDR 0x3D4
#define CRTC_DATA 0x3D5

static int     vga_col   = 0;
static int     vga_row   = 0;
static uint8_t vga_attr  = 0;   /* Current color attribute */

static void vga_update_cursor(void) {
    uint16_t pos = (uint16_t)(vga_row * VGA_COLS + vga_col);
    outw(CRTC_ADDR, (uint16_t)((14 << 8) | ((pos >> 8) & 0xFF)));
    outw(CRTC_ADDR, (uint16_t)((15 << 8) | (pos & 0xFF)));
}

static void vga_scroll(void) {
    /* Move rows 1..24 up by one row */
    volatile uint16_t *buf = VGA_BUFFER;
    for (int i = 0; i < (VGA_ROWS - 1) * VGA_COLS; i++)
        buf[i] = buf[i + VGA_COLS];
    /* Clear last row */
    uint16_t blank = VGA_ENTRY(' ', vga_attr);
    for (int i = (VGA_ROWS - 1) * VGA_COLS; i < VGA_ROWS * VGA_COLS; i++)
        buf[i] = blank;
}

void vga_init(void) {
    vga_attr = VGA_COLOR(VGA_LIGHT_GREY, VGA_BLACK);
    vga_clear();
}

void vga_clear(void) {
    uint16_t blank = VGA_ENTRY(' ', vga_attr);
    for (int i = 0; i < VGA_ROWS * VGA_COLS; i++)
        VGA_BUFFER[i] = blank;
    vga_row = 0;
    vga_col = 0;
    vga_update_cursor();
}

void vga_set_color(vga_color_t fg, vga_color_t bg) {
    vga_attr = VGA_COLOR(fg, bg);
}

void vga_set_color_attr(uint8_t attr) {
    vga_attr = attr;
}

void vga_putchar(char c) {
    if (c == '\n') {
        vga_col = 0;
        vga_row++;
    } else if (c == '\r') {
        vga_col = 0;
    } else if (c == '\t') {
        /* Align to next 8-column tab stop */
        vga_col = (vga_col + 8) & ~7;
        if (vga_col >= VGA_COLS) {
            vga_col = 0;
            vga_row++;
        }
    } else if (c == '\b') {
        if (vga_col > 0) {
            vga_col--;
            VGA_BUFFER[vga_row * VGA_COLS + vga_col] = VGA_ENTRY(' ', vga_attr);
        }
    } else {
        VGA_BUFFER[vga_row * VGA_COLS + vga_col] = VGA_ENTRY((uint8_t)c, vga_attr);
        vga_col++;
        if (vga_col >= VGA_COLS) {
            vga_col = 0;
            vga_row++;
        }
    }

    if (vga_row >= VGA_ROWS) {
        vga_scroll();
        vga_row = VGA_ROWS - 1;
    }
    vga_update_cursor();
}

void vga_puts(const char *s) {
    while (*s)
        vga_putchar(*s++);
}
