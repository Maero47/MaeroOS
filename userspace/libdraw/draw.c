#include <draw.h>
#include <font8x16.h>
#include <string.h>

uint32_t draw_color(const char *name) {
    if (!name) return 0;
    if (name[0] == '#') {
        uint32_t v = 0;
        int n = 0;
        for (const char *p = name + 1; *p && n < 6; p++, n++) {
            char c = *p;
            int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else break;
            v = (v << 4) | (uint32_t)d;
        }
        if (n == 6) return v & 0xFFFFFFu;
        return 0;
    }
    if (!strcmp(name, "black"))  return draw_rgb(20, 24, 26);
    if (!strcmp(name, "white"))  return draw_rgb(238, 240, 235);
    if (!strcmp(name, "gray"))   return draw_rgb(102, 110, 112);
    if (!strcmp(name, "red"))    return draw_rgb(205, 83, 73);
    if (!strcmp(name, "green"))  return draw_rgb(62, 156, 108);
    if (!strcmp(name, "blue"))   return draw_rgb(36, 127, 174);
    if (!strcmp(name, "yellow")) return draw_rgb(255, 214, 88);
    if (!strcmp(name, "cyan"))   return draw_rgb(85, 190, 205);
    return 0;
}

uint32_t draw_palette(int idx) {
    switch (idx) {
    case 1:  return draw_rgb(20, 24, 26);
    case 2:  return draw_rgb(238, 240, 235);
    case 3:  return draw_rgb(102, 110, 112);
    case 4:  return draw_rgb(205, 83, 73);
    case 5:  return draw_rgb(62, 156, 108);
    case 6:  return draw_rgb(36, 127, 174);
    case 7:  return draw_rgb(255, 214, 88);
    case 8:  return draw_rgb(85, 190, 205);
    case 9:  return draw_rgb(94, 129, 172);
    case 10: return draw_rgb(40, 44, 52);
    case 11: return draw_rgb(222, 144, 70);
    case 12: return draw_rgb(166, 120, 195);
    case 13: return draw_rgb(140, 100, 60);
    case 14: return draw_rgb(200, 204, 210);
    case 15: return draw_rgb(60, 64, 72);
    }
    return 0;
}

void draw_fill(draw_surface_t *s, uint32_t color) {
    if (!s || !s->px) return;
    int n = s->w * s->h;
    for (int i = 0; i < n; i++)
        s->px[i] = color;
}

void draw_rect(draw_surface_t *s, int x, int y, int w, int h, uint32_t color) {
    if (!s || !s->px) return;
    int x2 = x + w;
    int y2 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x2 > s->w) x2 = s->w;
    if (y2 > s->h) y2 = s->h;
    for (int yy = y; yy < y2; yy++) {
        uint32_t *row = s->px + yy * s->w;
        for (int xx = x; xx < x2; xx++)
            row[xx] = color;
    }
}

void draw_frame(draw_surface_t *s, int x, int y, int w, int h, uint32_t color) {
    draw_rect(s, x, y, w, 1, color);
    draw_rect(s, x, y + h - 1, w, 1, color);
    draw_rect(s, x, y, 1, h, color);
    draw_rect(s, x + w - 1, y, 1, h, color);
}

void draw_text(draw_surface_t *s, int x, int y, const char *str,
               uint32_t color) {
    if (!s || !s->px || !str) return;
    for (; *str; str++, x += DRAW_GLYPH_W) {
        unsigned char ch = (unsigned char)*str;
        if (ch >= 128) ch = 0;
        for (int gy = 0; gy < DRAW_GLYPH_H; gy++) {
            int yy = y + gy;
            if (yy < 0 || yy >= s->h) continue;
            uint8_t bits = font8x16[ch][gy];
            if (!bits) continue;
            uint32_t *row = s->px + yy * s->w;
            for (int gx = 0; gx < DRAW_GLYPH_W; gx++) {
                if (!(bits & (0x80u >> gx))) continue;
                int xx = x + gx;
                if (xx >= 0 && xx < s->w)
                    row[xx] = color;
            }
        }
    }
}

void draw_icon(draw_surface_t *s, int x, int y, int w, int h,
               const char *const *hexrows) {
    if (!s || !s->px || !hexrows) return;
    for (int gy = 0; gy < h; gy++) {
        int yy = y + gy;
        const char *rowstr = hexrows[gy];
        if (yy < 0 || yy >= s->h || !rowstr) continue;
        uint32_t *row = s->px + yy * s->w;
        for (int gx = 0; gx < w && rowstr[gx]; gx++) {
            char c = rowstr[gx];
            int v = 0;
            if (c >= '0' && c <= '9') v = c - '0';
            else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
            if (!v) continue;
            int xx = x + gx;
            if (xx >= 0 && xx < s->w)
                row[xx] = draw_palette(v);
        }
    }
}
