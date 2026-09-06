/*
 * image.c — full-color RGBA raster icons for libdraw.
 *
 * Loads MaeroOS .mic icons (full-color, anti-aliased, rasterized from the
 * Reversal-blue SVG theme by tools/mkicons.py) and alpha-blends them onto a
 * draw surface.  Replaces the old 16-color hex-art for crisp themed icons.
 *
 * .mic format (little-endian): u32 'MIC1', u16 w, u16 h, w*h*4 RGBA.
 */
#include <draw.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#define MIC_MAGIC 0x3143494DU   /* 'MIC1' */

draw_image_t *draw_load_mic(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;

    unsigned char hdr[8];
    if (read(fd, hdr, 8) != 8) { close(fd); return 0; }
    uint32_t magic = (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8) |
                     ((uint32_t)hdr[2] << 16) | ((uint32_t)hdr[3] << 24);
    if (magic != MIC_MAGIC) { close(fd); return 0; }
    int w = hdr[4] | (hdr[5] << 8);
    int h = hdr[6] | (hdr[7] << 8);
    if (w <= 0 || h <= 0 || w > 512 || h > 512) { close(fd); return 0; }

    draw_image_t *img = (draw_image_t *)malloc(sizeof(*img));
    if (!img) { close(fd); return 0; }
    img->w = w;
    img->h = h;
    img->px = (uint32_t *)malloc((size_t)w * h * 4);
    if (!img->px) { free(img); close(fd); return 0; }

    /* Read RGBA bytes and pack into 0xAARRGGBB. */
    size_t need = (size_t)w * h * 4;
    unsigned char *buf = (unsigned char *)img->px;  /* temp reuse */
    size_t got = 0;
    while (got < need) {
        int n = read(fd, buf + got, (int)(need - got));
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(fd);
    if (got != need) { free(img->px); free(img); return 0; }

    /* In-place RGBA(bytes) -> 0xAARRGGBB(uint32), back to front. */
    for (int i = w * h - 1; i >= 0; i--) {
        unsigned char r = buf[i * 4 + 0];
        unsigned char g = buf[i * 4 + 1];
        unsigned char b = buf[i * 4 + 2];
        unsigned char a = buf[i * 4 + 3];
        img->px[i] = ((uint32_t)a << 24) | ((uint32_t)r << 16) |
                     ((uint32_t)g << 8) | b;
    }
    return img;
}

void draw_image_free(draw_image_t *img) {
    if (!img) return;
    free(img->px);
    free(img);
}

void draw_image_blit_scaled(draw_surface_t *s, int x, int y, int dw, int dh,
                            const draw_image_t *img) {
    if (!s || !s->px || !img || !img->px || dw <= 0 || dh <= 0) return;
    for (int dy = 0; dy < dh; dy++) {
        int py = y + dy;
        if (py < 0 || py >= s->h) continue;
        int sy = dy * img->h / dh;
        uint32_t *srow = img->px + (size_t)sy * img->w;
        uint32_t *drow = s->px + (size_t)py * s->w;
        for (int dx = 0; dx < dw; dx++) {
            int px = x + dx;
            if (px < 0 || px >= s->w) continue;
            uint32_t src = srow[dx * img->w / dw];
            unsigned a = src >> 24;
            if (!a) continue;
            if (a >= 255)
                drow[px] = src & 0x00FFFFFF;
            else
                drow[px] = draw_blend(drow[px], src & 0x00FFFFFF, a);
        }
    }
}

void draw_image_blit(draw_surface_t *s, int x, int y, const draw_image_t *img) {
    if (!img) return;
    draw_image_blit_scaled(s, x, y, img->w, img->h, img);
}
