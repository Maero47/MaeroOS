#pragma once
#include <stdint.h>

/*
 * libdraw — client-side 2D rendering into a pixel buffer (0x00RRGGBB).
 * Used by libgui v2 (and apps) to draw window content into shared-memory
 * surfaces that the desktop compositor blits.
 */

typedef struct {
    uint32_t *px;
    int w;
    int h;
} draw_surface_t;

#define DRAW_GLYPH_W 8
#define DRAW_GLYPH_H 16

static inline uint32_t draw_rgb(unsigned r, unsigned g, unsigned b) {
    return (r << 16) | (g << 8) | b;
}

/* Parse "#RRGGBB" or the WM's named colors; falls back to black. */
uint32_t draw_color(const char *name);

/* 16-color icon palette (same indexes the WM uses); 0 = transparent. */
uint32_t draw_palette(int idx);

void draw_fill(draw_surface_t *s, uint32_t color);
void draw_rect(draw_surface_t *s, int x, int y, int w, int h, uint32_t color);
/* 1px outline */
void draw_frame(draw_surface_t *s, int x, int y, int w, int h, uint32_t color);
void draw_text(draw_surface_t *s, int x, int y, const char *str,
               uint32_t color);
/* Hex-row icon (one palette digit per pixel, 0 transparent) */
void draw_icon(draw_surface_t *s, int x, int y, int w, int h,
               const char *const *hexrows);

/* ── Alpha blending + anti-aliased proportional text ──────────────────── */

/* Blend color over dst at opacity a (0..255). */
static inline uint32_t draw_blend(uint32_t dst, uint32_t color, unsigned a) {
    unsigned ia = 255 - a;
    unsigned r = ((color >> 16 & 0xff) * a + (dst >> 16 & 0xff) * ia) >> 8;
    unsigned g = ((color >> 8 & 0xff) * a + (dst >> 8 & 0xff) * ia) >> 8;
    unsigned b = ((color & 0xff) * a + (dst & 0xff) * ia) >> 8;
    return (r << 16) | (g << 8) | b;
}

/* Filled rect blended over existing pixels at opacity alpha (0..255). */
void draw_rect_alpha(draw_surface_t *s, int x, int y, int w, int h,
                     uint32_t color, unsigned alpha);

/* Filled rounded rect (radius r); corners are anti-aliased against the
 * existing backdrop, so call after the background under it is painted. */
void draw_round_rect(draw_surface_t *s, int x, int y, int w, int h,
                     int r, uint32_t color);
/* 1px rounded outline matching draw_round_rect's silhouette. */
void draw_round_frame(draw_surface_t *s, int x, int y, int w, int h,
                      int r, uint32_t color);

/* Anti-aliased proportional fonts (atlases from tools/mkfont.py). */
typedef struct {
    int line_h;
    const unsigned char *widths;    /* per-glyph advance, ASCII 32..126 */
    const unsigned int *offsets;    /* into alpha[], [95] = total size */
    const unsigned char *alpha;     /* w x line_h coverage per glyph */
} draw_font_t;

extern const draw_font_t draw_font_ui;       /* ~15px UI text */
extern const draw_font_t draw_font_ui_big;   /* ~20px headings */
extern const draw_font_t draw_font_mono;     /* AA monospace (Menlo 16px) */
#define DRAW_MONO_CW   10                    /* monospace cell width (px) */
#define DRAW_MONO_LH   19                    /* monospace line height (px) */

/* Returns the pen advance in pixels. */
int draw_text_aa(draw_surface_t *s, int x, int y, const char *str,
                 uint32_t color, const draw_font_t *font);
int draw_text_width(const char *str, const draw_font_t *font);

/* ── Full-color RGBA raster icons (.mic theme icons from tools/mkicons.py) ── */
typedef struct draw_image {
    int w, h;
    uint32_t *px;   /* straight alpha: 0xAARRGGBB */
} draw_image_t;

/* Load a .mic icon file into a malloc'd image (free with draw_image_free).
 * Returns NULL on error. */
draw_image_t *draw_load_mic(const char *path);
void draw_image_free(draw_image_t *img);

/* Alpha-blend an RGBA image's source rect onto the surface at (x,y).
 * If dw/dh differ from the image size the image is nearest-neighbor scaled. */
void draw_image_blit(draw_surface_t *s, int x, int y, const draw_image_t *img);
void draw_image_blit_scaled(draw_surface_t *s, int x, int y, int dw, int dh,
                            const draw_image_t *img);
