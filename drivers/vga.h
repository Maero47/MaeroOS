#pragma once
#include <stdint.h>

/* Standard 16-color VGA palette */
typedef enum {
    VGA_BLACK = 0, VGA_BLUE, VGA_GREEN, VGA_CYAN,
    VGA_RED, VGA_MAGENTA, VGA_BROWN, VGA_LIGHT_GREY,
    VGA_DARK_GREY, VGA_LIGHT_BLUE, VGA_LIGHT_GREEN, VGA_LIGHT_CYAN,
    VGA_LIGHT_RED, VGA_LIGHT_MAGENTA, VGA_LIGHT_BROWN, VGA_WHITE,
} vga_color_t;

#define VGA_COLOR(fg, bg) ((uint8_t)((bg) << 4) | (uint8_t)(fg))
#define VGA_ENTRY(c, attr) ((uint16_t)(c) | ((uint16_t)(attr) << 8))

void    vga_init(void);
void    vga_clear(void);
void    vga_putchar(char c);
void    vga_puts(const char *s);
void    vga_set_color(vga_color_t fg, vga_color_t bg);
void    vga_set_color_attr(uint8_t attr);
