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

/*
 * Scrolling without reading VGA memory (docs/smp-plan.md stage 1e).  Text-mode
 * memory is emulated MMIO under QEMU/KVM: every access is an exit to the
 * host, and a scroll used to read and rewrite all 2000 cells -- ~4000 exits,
 * several milliseconds per printed line, with the BKL held.  Instead the
 * visible screen is a window into the 32 KiB of text memory: a scroll moves
 * the CRTC start address down one row and clears only the new row.  When the
 * window reaches the end of text memory the screen is rewritten at the start
 * from a RAM copy (once every ~180 lines).  The cursor is set once per string.
 */
#define VGA_MEM_CELLS 16384             /* 32 KiB of text memory at 0xB8000 */
static uint16_t vga_shadow[VGA_ROWS * VGA_COLS];
static uint32_t vga_base;               /* cell offset of the visible row 0 */

static void crtc_write(uint8_t idx, uint8_t val) {
    outw(CRTC_ADDR, (uint16_t)(((uint16_t)val << 8) | idx));
}

static void vga_update_cursor(void) {
    uint16_t pos = (uint16_t)(vga_base + (uint32_t)(vga_row * VGA_COLS + vga_col));
    crtc_write(14, (uint8_t)(pos >> 8));
    crtc_write(15, (uint8_t)pos);
}

static inline void vga_cell(int row, int col, uint16_t v) {
    vga_shadow[row * VGA_COLS + col] = v;
    VGA_BUFFER[vga_base + (uint32_t)(row * VGA_COLS + col)] = v;
}

static void vga_scroll(void) {
    uint16_t blank = VGA_ENTRY(' ', vga_attr);
    for (int i = 0; i < (VGA_ROWS - 1) * VGA_COLS; i++)
        vga_shadow[i] = vga_shadow[i + VGA_COLS];
    for (int i = (VGA_ROWS - 1) * VGA_COLS; i < VGA_ROWS * VGA_COLS; i++)
        vga_shadow[i] = blank;
    if (vga_base + (VGA_ROWS + 1) * VGA_COLS <= VGA_MEM_CELLS) {
        vga_base += VGA_COLS;
        volatile uint16_t *row = VGA_BUFFER + vga_base + (VGA_ROWS - 1) * VGA_COLS;
        for (int i = 0; i < VGA_COLS; i++)
            row[i] = blank;
    } else {
        vga_base = 0;
        for (int i = 0; i < VGA_ROWS * VGA_COLS; i++)
            VGA_BUFFER[i] = vga_shadow[i];
    }
    crtc_write(0x0C, (uint8_t)(vga_base >> 8));
    crtc_write(0x0D, (uint8_t)vga_base);
}

void vga_init(void) {
    vga_attr = VGA_COLOR(VGA_LIGHT_GREY, VGA_BLACK);
    vga_clear();
}

void vga_clear(void) {
    uint16_t blank = VGA_ENTRY(' ', vga_attr);
    vga_base = 0;
    crtc_write(0x0C, 0);
    crtc_write(0x0D, 0);
    for (int i = 0; i < VGA_ROWS * VGA_COLS; i++) {
        vga_shadow[i] = blank;
        VGA_BUFFER[i] = blank;
    }
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

static void vga_put(char c) {
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
            vga_cell(vga_row, vga_col, VGA_ENTRY(' ', vga_attr));
        }
    } else {
        vga_cell(vga_row, vga_col, VGA_ENTRY((uint8_t)c, vga_attr));
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
}

void vga_putchar(char c) {
    vga_put(c);
    vga_update_cursor();
}

void vga_puts(const char *s) {
    while (*s)
        vga_put(*s++);
    vga_update_cursor();
}
