/*
 * maeroX core drawing: GCs and every core graphics request.
 *
 * Everything that paints with a GC goes through put_span(): one horizontal
 * run of a drawable row, clipped to the drawable and to the GC's clip
 * (rectangles or a bitmap), painted with the GC's fill style (solid, tiled,
 * stippled, opaque-stippled), raster function and plane mask.  Rectangles,
 * polygons, arcs, wide lines and text are all turned into spans; thin lines
 * into one-pixel spans.  Images and CopyArea copy rows directly and share the
 * same clipping.
 */
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "xs.h"

uint32_t depth_mask(int depth) {
    return depth >= 32 ? 0xFFFFFFFFu : depth == 24 ? 0x00FFFFFFu : ((1u << depth) - 1);
}

void fill32(uint32_t *p, uint32_t v, int n) {
    if (n <= 0) return;
    __asm__ volatile("rep stosl" : "+D"(p), "+c"(n) : "a"(v) : "memory");
}

/* Clip one axis of a copy: the run of *n pixels starting at *d in a
 * destination of length dl and (unless s is NULL) at *s in a source of
 * length sl.  Returns 0 when nothing is left. */
int clip_span(int *d, int *s, int *n, int dl, int sl, int *skip) {
    long lo = 0, hi = *n;
    if (*d < 0 && -(long)*d > lo) lo = -(long)*d;
    if ((long)dl - *d < hi) hi = (long)dl - *d;
    if (s) {
        if (*s < 0 && -(long)*s > lo) lo = -(long)*s;
        if ((long)sl - *s < hi) hi = (long)sl - *s;
    }
    if (hi <= lo) return 0;
    *d += (int)lo;
    if (s) *s += (int)lo;
    *n = (int)(hi - lo);
    if (skip) *skip = (int)lo;
    return 1;
}

void damage_drawable(drawable_t *d, int x, int y, int w, int h) {
    if (d && d->o.type == XT_WINDOW) damage_window((window_t *)d, x, y, w, h);
}

void mark_painted(drawable_t *d) {
    if (d->o.type != XT_WINDOW) return;
    window_t *w = (window_t *)d;
    if (!w->painted) {
        w->painted = 1;
        damage_window(w, 0, 0, d->w, d->h);
    }
}

/* ── GCs ─────────────────────────────────────────────────────────────────── */
gc_t *gc_new(uint32_t id, int owner, int depth) {
    gc_t *g = calloc(1, sizeof(*g));
    if (!g) return NULL;
    g->o.id = id; g->o.type = XT_GC; g->o.owner = (int8_t)owner;
    g->depth = depth;
    g->function = 3;
    g->plane_mask = 0xFFFFFFFFu;
    g->fg = 0; g->bg = 1;
    g->cap_style = 1;
    g->arc_mode = 1;
    g->graphics_exposures = 1;
    g->nclip = -1;
    g->ndash = 2; g->dashes[0] = g->dashes[1] = 4;
    return g;
}

static void gc_set_pixmap(pixmap_t **slot, pixmap_t *p) {
    if (*slot) drawable_unref(*slot);
    *slot = p;
    if (p) p->refs++;
}

void gc_free(gc_t *g) {
    gc_set_pixmap(&g->tile, NULL);
    gc_set_pixmap(&g->stipple, NULL);
    gc_set_pixmap(&g->clip_mask, NULL);
    free(g->clip);
    free(g);
}

static int mask_bits(uint32_t m) {
    int n = 0;
    for (; m; m &= m - 1) n++;
    return n;
}

int gc_change(client_t *c, gc_t *g, uint32_t mask, const uint8_t *v, int nvals) {
    if (mask_bits(mask & 0x7FFFFF) > nvals) { x_error(c, BadLength, 0); return -1; }
    for (int bit = 0; bit < 23; bit++) {
        if (!(mask & (1u << bit))) continue;
        uint32_t val = r32(v);
        v += 4;
        switch (bit) {
        case 0: g->function = (int)(val & 15); break;
        case 1: g->plane_mask = val; break;
        case 2: g->fg = val; break;
        case 3: g->bg = val; break;
        case 4: g->line_width = (int)(val & 0xFFFF); break;
        case 5: g->line_style = (int)val; break;
        case 6: g->cap_style = (int)val; break;
        case 7: g->join_style = (int)val; break;
        case 8: g->fill_style = (int)val; break;
        case 9: g->fill_rule = (int)val; break;
        case 10: case 11: {
            xobj_t *o = res_lookup(val);
            if (!o || o->type != XT_PIXMAP) { x_error(c, BadPixmap, val); return -1; }
            gc_set_pixmap(bit == 10 ? &g->tile : &g->stipple, (pixmap_t *)o);
            break;
        }
        case 12: g->ts_x = (int)(int16_t)val; break;
        case 13: g->ts_y = (int)(int16_t)val; break;
        case 14: g->font = val; break;
        case 15: g->subwindow_mode = (int)val; break;
        case 16: g->graphics_exposures = (int)(val & 1); break;
        case 17: g->clip_x = (int)(int16_t)val; break;
        case 18: g->clip_y = (int)(int16_t)val; break;
        case 19:
            free(g->clip); g->clip = NULL; g->nclip = -1;
            if (val == 0) gc_set_pixmap(&g->clip_mask, NULL);
            else {
                xobj_t *o = res_lookup(val);
                if (!o || o->type != XT_PIXMAP) { x_error(c, BadPixmap, val); return -1; }
                gc_set_pixmap(&g->clip_mask, (pixmap_t *)o);
            }
            break;
        case 20: g->dash_offset = (int)val; break;
        case 21: g->ndash = 2; g->dashes[0] = g->dashes[1] = (uint8_t)val; break;
        case 22: g->arc_mode = (int)val; break;
        }
    }
    return 0;
}

void gc_copy(gc_t *d, gc_t *s, uint32_t mask) {
    if (mask & (1u << 0)) d->function = s->function;
    if (mask & (1u << 1)) d->plane_mask = s->plane_mask;
    if (mask & (1u << 2)) d->fg = s->fg;
    if (mask & (1u << 3)) d->bg = s->bg;
    if (mask & (1u << 4)) d->line_width = s->line_width;
    if (mask & (1u << 5)) d->line_style = s->line_style;
    if (mask & (1u << 6)) d->cap_style = s->cap_style;
    if (mask & (1u << 7)) d->join_style = s->join_style;
    if (mask & (1u << 8)) d->fill_style = s->fill_style;
    if (mask & (1u << 9)) d->fill_rule = s->fill_rule;
    if (mask & (1u << 10)) gc_set_pixmap(&d->tile, s->tile);
    if (mask & (1u << 11)) gc_set_pixmap(&d->stipple, s->stipple);
    if (mask & (1u << 12)) d->ts_x = s->ts_x;
    if (mask & (1u << 13)) d->ts_y = s->ts_y;
    if (mask & (1u << 14)) d->font = s->font;
    if (mask & (1u << 15)) d->subwindow_mode = s->subwindow_mode;
    if (mask & (1u << 16)) d->graphics_exposures = s->graphics_exposures;
    if (mask & (1u << 17)) d->clip_x = s->clip_x;
    if (mask & (1u << 18)) d->clip_y = s->clip_y;
    if (mask & (1u << 19)) {
        gc_set_pixmap(&d->clip_mask, s->clip_mask);
        free(d->clip); d->clip = NULL; d->nclip = s->nclip;
        if (s->nclip > 0) {
            d->clip = malloc((size_t)s->nclip * sizeof(xrect_t));
            if (d->clip) memcpy(d->clip, s->clip, (size_t)s->nclip * sizeof(xrect_t));
            else d->nclip = -1;
        }
    }
    if (mask & (1u << 20)) d->dash_offset = s->dash_offset;
    if (mask & (1u << 21)) { d->ndash = s->ndash; memcpy(d->dashes, s->dashes, sizeof(d->dashes)); }
    if (mask & (1u << 22)) d->arc_mode = s->arc_mode;
}

/* ── spans ───────────────────────────────────────────────────────────────── */
static inline uint32_t rop(int f, uint32_t s, uint32_t d) {
    switch (f) {
    case 0: return 0;
    case 1: return s & d;
    case 2: return s & ~d;
    case 3: return s;
    case 4: return ~s & d;
    case 5: return d;
    case 6: return s ^ d;
    case 7: return s | d;
    case 8: return ~(s | d);
    case 9: return ~(s ^ d);
    case 10: return ~d;
    case 11: return s | ~d;
    case 12: return ~s;
    case 13: return ~s | d;
    case 14: return ~(s & d);
    default: return 0xFFFFFFFFu;
    }
}

static inline int tile_mod(int v, int n) { int r = v % n; return r < 0 ? r + n : r; }

/* Paint pixels [a, b) of row y (already clipped) with fill colour `fgv` (or
 * the GC's fill style when `fill` is set). */
static void raw_span(drawable_t *d, gc_t *g, int y, int a, int b, int fill, uint32_t fgv) {
    if (a >= b) return;
    uint32_t dm = depth_mask(d->depth);
    uint32_t pm = g->plane_mask & dm;
    uint32_t *row = d->px + (size_t)y * d->w;
    int fs = fill ? g->fill_style : 0;
    if (fs == 1 && !g->tile) fs = 0;
    if ((fs == 2 || fs == 3) && !g->stipple) fs = 0;
    if (fs == 0 && g->function == 3 && pm == dm) {
        fill32(row + a, fgv & dm, b - a);
    } else {
        for (int x = a; x < b; x++) {
            uint32_t src;
            if (fs == 0) src = fgv;
            else if (fs == 1) {
                pixmap_t *t = g->tile;
                src = t->px[(size_t)tile_mod(y - g->ts_y, t->h) * t->w + tile_mod(x - g->ts_x, t->w)];
            } else {
                pixmap_t *t = g->stipple;
                int on = t->px[(size_t)tile_mod(y - g->ts_y, t->h) * t->w + tile_mod(x - g->ts_x, t->w)] & 1;
                if (!on && fs == 2) continue;
                src = on ? g->fg : g->bg;
            }
            uint32_t r = rop(g->function, src, row[x]);
            row[x] = ((row[x] & ~pm) | (r & pm)) & dm;
        }
    }
    damage_drawable(d, a, y, b - a, 1);
}

/* Clip [x0, x1) of row y to the drawable and the GC clip, then paint. */
static void put_span_c(drawable_t *d, gc_t *g, int y, int x0, int x1, int fill, uint32_t fgv) {
    if (y < 0 || y >= d->h || !d->px) return;
    if (x0 < 0) x0 = 0;
    if (x1 > d->w) x1 = d->w;
    if (x0 >= x1) return;
    if (g->nclip >= 0) {
        for (int i = 0; i < g->nclip; i++) {
            xrect_t *r = &g->clip[i];
            int ry = r->y + g->clip_y, rx = r->x + g->clip_x;
            if (y < ry || y >= ry + r->h) continue;
            int a = imax(x0, rx), b = imin(x1, rx + r->w);
            if (a < b) raw_span(d, g, y, a, b, fill, fgv);
        }
        return;
    }
    if (g->clip_mask && g->clip_mask->px) {
        pixmap_t *m = g->clip_mask;
        int my = y - g->clip_y;
        if (my < 0 || my >= m->h) return;
        const uint32_t *mr = m->px + (size_t)my * m->w;
        int x = x0;
        while (x < x1) {
            int mx = x - g->clip_x;
            if (mx < 0 || mx >= m->w || !mr[mx]) { x++; continue; }
            int s = x;
            while (x < x1 && (x - g->clip_x) < m->w && mr[x - g->clip_x]) x++;
            raw_span(d, g, y, s, x, fill, fgv);
        }
        return;
    }
    raw_span(d, g, y, x0, x1, fill, fgv);
}

static void put_span(drawable_t *d, gc_t *g, int y, int x0, int x1) {
    put_span_c(d, g, y, x0, x1, 1, g->fg);
}

static void fill_rect_gc(drawable_t *d, gc_t *g, int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    int x0 = x, y0 = y;
    if (!clip_span(&x0, NULL, &w, d->w, 0, NULL) || !clip_span(&y0, NULL, &h, d->h, 0, NULL)) return;
    for (int yy = y0; yy < y0 + h; yy++) put_span(d, g, yy, x0, x0 + w);
}

/* ── polygons (scanline, pixel centres) ──────────────────────────────────── */
typedef struct { double x, y; } dpt_t;

static int cmp_double(const void *a, const void *b) {
    double x = ((const double *)a)[0], y = ((const double *)b)[0];
    return x < y ? -1 : x > y;
}

static void fill_polygon(drawable_t *d, gc_t *g, const dpt_t *p, int n, int winding) {
    if (n < 3) return;
    double ymin = p[0].y, ymax = p[0].y;
    for (int i = 1; i < n; i++) { if (p[i].y < ymin) ymin = p[i].y; if (p[i].y > ymax) ymax = p[i].y; }
    int y0 = (int)ceil(ymin - 0.5), y1 = (int)ceil(ymax - 0.5);
    if (y0 < 0) y0 = 0;
    if (y1 > d->h) y1 = d->h;
    double (*xs)[2] = malloc((size_t)n * sizeof(*xs));
    if (!xs) return;
    for (int y = y0; y < y1; y++) {
        double yc = y + 0.5;
        int k = 0;
        for (int i = 0; i < n; i++) {
            const dpt_t *a = &p[i], *b = &p[(i + 1) % n];
            if (a->y == b->y) continue;
            double lo = a->y < b->y ? a->y : b->y, hi = a->y < b->y ? b->y : a->y;
            if (yc < lo || yc >= hi) continue;
            xs[k][0] = a->x + (yc - a->y) * (b->x - a->x) / (b->y - a->y);
            xs[k][1] = a->y < b->y ? 1 : -1;
            k++;
        }
        qsort(xs, (size_t)k, sizeof(*xs), cmp_double);
        if (!winding) {
            for (int i = 0; i + 1 < k; i += 2) {
                int xa = (int)ceil(xs[i][0] - 0.5), xb = (int)ceil(xs[i + 1][0] - 0.5);
                put_span(d, g, y, xa, xb);
            }
        } else {
            int wn = 0;
            for (int i = 0; i + 1 < k; i++) {
                wn += (int)xs[i][1];
                if (wn) {
                    int xa = (int)ceil(xs[i][0] - 0.5), xb = (int)ceil(xs[i + 1][0] - 0.5);
                    put_span(d, g, y, xa, xb);
                }
            }
        }
    }
    free(xs);
}

/* ── lines ───────────────────────────────────────────────────────────────── */
static void thin_line(drawable_t *d, gc_t *g, int x0, int y0, int x1, int y1, int last) {
    int dx = abs(x1 - x0), dy = -abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    /* Clip the walk to the drawable: a 65535-long line costs nothing extra. */
    for (int steps = 0; steps < 140000; steps++) {
        int at_end = (x0 == x1 && y0 == y1);
        if (!at_end || last) put_span(d, g, y0, x0, x0 + 1);
        if (at_end) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void wide_line(drawable_t *d, gc_t *g, double x0, double y0, double x1, double y1) {
    double lw = g->line_width;
    double dx = x1 - x0, dy = y1 - y0, len = sqrt(dx * dx + dy * dy);
    if (len == 0) {
        if (g->cap_style >= 2) fill_rect_gc(d, g, (int)(x0 - lw / 2), (int)(y0 - lw / 2), (int)lw, (int)lw);
        return;
    }
    double ux = dx / len, uy = dy / len, nx = -uy * lw / 2, ny = ux * lw / 2;
    if (g->cap_style >= 2) {                     /* Projecting / Round (approx.) */
        x0 -= ux * lw / 2; y0 -= uy * lw / 2;
        x1 += ux * lw / 2; y1 += uy * lw / 2;
    }
    dpt_t q[4] = { { x0 + nx + 0.5, y0 + ny + 0.5 }, { x1 + nx + 0.5, y1 + ny + 0.5 },
                   { x1 - nx + 0.5, y1 - ny + 0.5 }, { x0 - nx + 0.5, y0 - ny + 0.5 } };
    fill_polygon(d, g, q, 4, 1);
}

static void join_square(drawable_t *d, gc_t *g, int x, int y) {
    int lw = g->line_width;
    if (lw > 1) fill_rect_gc(d, g, x - lw / 2, y - lw / 2, lw, lw);
}

static void draw_polyline(drawable_t *d, gc_t *g, const int *xy, int n, int closed) {
    for (int i = 0; i + 1 < n; i++) {
        int x0 = xy[2 * i], y0 = xy[2 * i + 1], x1 = xy[2 * i + 2], y1 = xy[2 * i + 3];
        if (g->line_width <= 1)
            thin_line(d, g, x0, y0, x1, y1, i == n - 2 && !closed && g->cap_style != 0);
        else {
            wide_line(d, g, x0, y0, x1, y1);
            if (i > 0) join_square(d, g, x0, y0);
        }
    }
}

/* ── arcs ────────────────────────────────────────────────────────────────── */
static int arc_points(int x, int y, int w, int h, int a1, int a2, dpt_t **out) {
    double cx = x + w / 2.0, cy = y + h / 2.0, rx = w / 2.0, ry = h / 2.0;
    if (a2 > 360 * 64) a2 = 360 * 64;
    if (a2 < -360 * 64) a2 = -360 * 64;
    double t0 = a1 / 64.0 * M_PI / 180.0, ext = a2 / 64.0 * M_PI / 180.0;
    int n = (int)(fabs(ext) / (2 * M_PI) * (rx + ry) * 1.5) + 4;
    if (n > 720) n = 720;
    dpt_t *p = malloc((size_t)(n + 2) * sizeof(dpt_t));
    if (!p) return 0;
    for (int i = 0; i <= n; i++) {
        double t = t0 + ext * i / n;
        p[i].x = cx + rx * cos(t);
        p[i].y = cy - ry * sin(t);
    }
    *out = p;
    return n + 1;
}

static void draw_arc(drawable_t *d, gc_t *g, int x, int y, int w, int h, int a1, int a2, int filled) {
    dpt_t *p = NULL;
    int n = arc_points(x, y, w, h, a1, a2, &p);
    if (!n) return;
    if (filled) {
        int full = abs(a2) >= 360 * 64;
        if (!full && g->arc_mode == 1) { p[n].x = x + w / 2.0; p[n].y = y + h / 2.0; n++; }
        for (int i = 0; i < n; i++) { p[i].x += 0.5; p[i].y += 0.5; }   /* centre the outline */
        fill_polygon(d, g, p, n, 1);
    } else {
        int *xy = malloc((size_t)n * 2 * sizeof(int));
        if (xy) {
            for (int i = 0; i < n; i++) { xy[2 * i] = (int)floor(p[i].x + 0.5); xy[2 * i + 1] = (int)floor(p[i].y + 0.5); }
            draw_polyline(d, g, xy, n, 0);
            free(xy);
        }
    }
    free(p);
}

/* ── text ────────────────────────────────────────────────────────────────── */
static int draw_glyph(drawable_t *d, gc_t *g, uint32_t fid, unsigned ch, int x, int y) {
    int bold = 0;
    const uint8_t *gl = font_glyph(fid, ch, &bold);
    if (!gl) return FONT_W;
    int top = y - FONT_ASCENT;
    for (int r = 0; r < FONT_H; r++) {
        uint32_t bits = gl[r];
        if (bold) bits |= bits >> 1;
        int c = 0;
        while (c < 8) {
            if (!(bits & (0x80u >> c))) { c++; continue; }
            int s = c;
            while (c < 8 && (bits & (0x80u >> c))) c++;
            put_span(d, g, top + r, x + s, x + c);
        }
    }
    return FONT_W;
}

static void poly_text(client_t *c, drawable_t *d, gc_t *g, const uint8_t *q, int qlen, int wide) {
    int x = rs16(q + 12), y = rs16(q + 14);
    int off = 16;
    uint32_t fid = g->font;
    while (off + 1 < qlen) {
        int n = q[off];
        if (n == 0) break;
        if (n == 255) {                          /* font shift, MSB first */
            if (off + 5 > qlen) break;
            fid = ((uint32_t)q[off + 1] << 24) | ((uint32_t)q[off + 2] << 16) |
                  ((uint32_t)q[off + 3] << 8) | q[off + 4];
            g->font = fid;
            off += 5;
            continue;
        }
        x += (int8_t)q[off + 1];
        off += 2;
        for (int i = 0; i < n && off + (wide ? 2 : 1) <= qlen; i++) {
            unsigned ch = wide ? ((unsigned)q[off] << 8 | q[off + 1]) : q[off];
            off += wide ? 2 : 1;
            x += draw_glyph(d, g, fid, ch, x, y);
        }
    }
    (void)c;
}

static void image_text(drawable_t *d, gc_t *g, const uint8_t *q, int qlen, int wide) {
    int n = q[1];
    int x = rs16(q + 12), y = rs16(q + 14);
    if (16 + n * (wide ? 2 : 1) > qlen) n = (qlen - 16) / (wide ? 2 : 1);
    /* The background box is drawn with the background colour, solid, GXcopy
     * (the protocol ignores function and fill-style here). */
    int fn = g->function, fs = g->fill_style;
    g->function = 3; g->fill_style = 0;
    for (int yy = y - FONT_ASCENT; yy < y + FONT_DESCENT; yy++)
        put_span_c(d, g, yy, x, x + n * FONT_W, 0, g->bg);
    for (int i = 0; i < n; i++) {
        unsigned ch = wide ? ((unsigned)q[16 + 2 * i] << 8 | q[17 + 2 * i]) : q[16 + i];
        draw_glyph(d, g, g->font, ch, x + i * FONT_W, y);
    }
    g->function = fn; g->fill_style = fs;
}

/* ── images ──────────────────────────────────────────────────────────────── */
static int depth_ok(int depth) {
    return depth == 1 || depth == 4 || depth == 8 || depth == 24 || depth == 32;
}

static int bits_stride(int w) { return ((w + 31) / 32) * 4; }
static int z_stride(int depth, int w) {
    if (depth == 1) return bits_stride(w);
    if (depth == 4 || depth == 8) return (w + 3) & ~3;
    return w * 4;
}

/* Write one row of source pixels into the drawable through the GC (clip,
 * function, plane mask). */
static void put_row(drawable_t *d, gc_t *g, int y, int x, const uint32_t *src, int n) {
    if (y < 0 || y >= d->h) return;
    int sx = 0;
    if (!clip_span(&x, &sx, &n, d->w, n, NULL)) return;
    uint32_t dm = depth_mask(d->depth), pm = g->plane_mask & dm;
    uint32_t *row = d->px + (size_t)y * d->w;
    int simple = g->function == 3 && pm == dm && g->nclip < 0 && !g->clip_mask;
    if (simple) {
        for (int i = 0; i < n; i++) row[x + i] = src[sx + i] & dm;
        damage_drawable(d, x, y, n, 1);
        return;
    }
    /* With a clip, paint each visible run through the span clipper using a
     * temporary per-pixel source. */
    for (int i = 0; i < n; i++) {
        int px = x + i;
        int vis = 1;
        if (g->nclip >= 0) {
            vis = 0;
            for (int k = 0; k < g->nclip && !vis; k++) {
                xrect_t *r = &g->clip[k];
                vis = px >= r->x + g->clip_x && px < r->x + g->clip_x + r->w &&
                      y >= r->y + g->clip_y && y < r->y + g->clip_y + r->h;
            }
        } else if (g->clip_mask && g->clip_mask->px) {
            int mx = px - g->clip_x, my = y - g->clip_y;
            vis = mx >= 0 && my >= 0 && mx < g->clip_mask->w && my < g->clip_mask->h &&
                  g->clip_mask->px[(size_t)my * g->clip_mask->w + mx];
        }
        if (!vis) continue;
        uint32_t r = rop(g->function, src[sx + i], row[px]);
        row[px] = ((row[px] & ~pm) | (r & pm)) & dm;
    }
    damage_drawable(d, x, y, n, 1);
}

static void put_image(client_t *c, drawable_t *d, gc_t *g, const uint8_t *q, int qlen) {
    int format = q[1];
    int w = (int)r16(q + 12), h = (int)r16(q + 14);
    int dx = rs16(q + 16), dy = rs16(q + 18);
    int lpad = q[20], depth = q[21];
    const uint8_t *img = q + 24;
    uint64_t stride;
    if (format == 2) stride = (uint64_t)z_stride(depth, w);
    else stride = (uint64_t)bits_stride(w + lpad);
    uint64_t need = stride * (uint64_t)h * (format == 1 && depth > 1 ? (uint64_t)depth : 1);
    if (24 + need > (uint64_t)qlen) { x_error(c, BadLength, 0); return; }
    if (format == 2 && lpad) { x_error(c, BadMatch, 0); return; }
    if (!d->px || w == 0 || h == 0) return;
    mark_painted(d);
    uint32_t *row = malloc((size_t)w * 4);
    if (!row) { x_error(c, BadAlloc, 0); return; }
    for (int y = 0; y < h; y++) {
        int yy = dy + y;
        if (yy < 0 || yy >= d->h) continue;
        const uint8_t *s = img + (size_t)stride * y;
        if (format == 0 || depth == 1) {           /* bitmap: LSB first */
            for (int x = 0; x < w; x++) {
                int b = x + (format == 2 ? 0 : lpad);
                int on = (s[b >> 3] >> (b & 7)) & 1;
                row[x] = format == 0 ? (on ? g->fg : g->bg) : (uint32_t)on;
            }
        } else if (format == 2 && (depth == 24 || depth == 32)) {
            const uint32_t *p = (const uint32_t *)(const void *)s;
            for (int x = 0; x < w; x++) row[x] = p[x];
        } else if (format == 2 && (depth == 8 || depth == 4)) {
            for (int x = 0; x < w; x++) row[x] = s[x];
        } else {
            x_error(c, BadMatch, 0);
            break;
        }
        put_row(d, g, yy, dx, row, w);
    }
    free(row);
    /* The Firefox launcher (userspace/ff) waits for this marker. */
    static unsigned last;
    unsigned t = x_time();
    if (t - last >= 1000) {
        last = t;
        if (access("/tmp/ff_painted", F_OK) != 0) {
            int fd = open("/tmp/ff_painted", O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0) close(fd);
        }
    }
}

/* A window's pixels as the screen shows them: its own contents with its
 * viewable children composited on top. */
static void composite_children(window_t *w, uint32_t *out, int ox, int oy, int ow, int oh) {
    for (window_t *ch = w->bottom; ch; ch = ch->above) {
        if (!ch->mapped || ch->cls == 2) continue;
        int cx = ch->x + ch->bw, cy = ch->y + ch->bw;
        if (ch->d.px && (ch->painted || ch->bg_mode != BG_NONE)) {
            for (int y = 0; y < ch->d.h; y++) {
                int ty = cy + y - oy;
                if (ty < 0 || ty >= oh) continue;
                int tx = cx - ox, sx = 0, n = ch->d.w;
                if (!clip_span(&tx, &sx, &n, ow, ch->d.w, NULL)) continue;
                memcpy(out + (size_t)ty * ow + tx, ch->d.px + (size_t)y * ch->d.w + sx, (size_t)n * 4);
            }
        }
        composite_children(ch, out, ox - cx, oy - cy, ow, oh);
    }
}

static void get_image(client_t *c, const uint8_t *q) {
    int format = q[1];
    drawable_t *d = lookup_drawable(r32(q + 4));
    if (r32(q + 4) == ROOT_WINDOW) d = &root->d;
    int x = rs16(q + 8), y = rs16(q + 10);
    int w = (int)r16(q + 12), h = (int)r16(q + 14);
    uint32_t pmask = r32(q + 16);
    if ((uint64_t)w * h > (1u << 22)) { x_error(c, BadAlloc, 0); return; }
    if (!d) { x_error(c, BadDrawable, r32(q + 4)); return; }
    if (format != 2 && !(format == 1 && d->depth == 1)) { x_error(c, BadMatch, 0); return; }
    uint32_t *pix = calloc((size_t)(w ? w : 1) * (h ? h : 1), 4);
    if (!pix) { x_error(c, BadAlloc, 0); return; }
    if (d->px)
        for (int yy = 0; yy < h; yy++) {
            int sy = y + yy, sx = x, n = w, skip = 0;
            if (sy < 0 || sy >= d->h) continue;
            if (!clip_span(&sx, NULL, &n, d->w, 0, &skip)) continue;
            memcpy(pix + (size_t)yy * w + skip, d->px + (size_t)sy * d->w + sx, (size_t)n * 4);
        }
    if (d->o.type == XT_WINDOW && ((window_t *)d)->bottom)
        composite_children((window_t *)d, pix, x, y, w, h);
    int stride = z_stride(d->depth, w);
    size_t n = (size_t)stride * h;
    uint8_t *data = calloc(n ? n : 1, 1);
    if (!data) { free(pix); x_error(c, BadAlloc, 0); return; }
    for (int yy = 0; yy < h; yy++) {
        uint8_t *o = data + (size_t)stride * yy;
        const uint32_t *s = pix + (size_t)yy * w;
        for (int xx = 0; xx < w; xx++) {
            uint32_t v = s[xx] & pmask;
            if (d->depth == 1) { if (v & 1) o[xx >> 3] |= (uint8_t)(1u << (xx & 7)); }
            else if (d->depth <= 8) o[xx] = (uint8_t)v;
            else put32(o + xx * 4, d->depth == 32 ? v : (v & 0xFFFFFF));
        }
    }
    uint8_t r[24];
    memset(r, 0, sizeof(r));
    put32(r, d->o.type == XT_WINDOW ? ROOT_VISUAL : 0);
    send_reply_var(c, (uint8_t)d->depth, r, data, (int)n);
    free(data);
    free(pix);
}

static void no_expose(client_t *c, drawable_t *d, int major) {
    uint8_t e[32];
    memset(e, 0, sizeof(e));
    e[0] = NoExpose;
    put32(e + 4, d->o.id);
    put16(e + 8, 0);
    e[10] = (uint8_t)major;
    send_event(c, e);
}

static void copy_area(client_t *c, const uint8_t *q, int plane) {
    drawable_t *s = lookup_drawable(r32(q + 4)), *d = lookup_drawable(r32(q + 8));
    if (r32(q + 4) == ROOT_WINDOW) s = &root->d;
    if (r32(q + 8) == ROOT_WINDOW) d = &root->d;
    gc_t *g = lookup_gc(r32(q + 12));
    if (!s) { x_error(c, BadDrawable, r32(q + 4)); return; }
    if (!d) { x_error(c, BadDrawable, r32(q + 8)); return; }
    if (!g) { x_error(c, BadGC, r32(q + 12)); return; }
    int sx = rs16(q + 16), sy = rs16(q + 18), dx = rs16(q + 20), dy = rs16(q + 22);
    int w = (int)r16(q + 24), h = (int)r16(q + 26);
    uint32_t bit = plane ? r32(q + 28) : 0;
    if (s->px && d->px && clip_span(&dx, &sx, &w, d->w, s->w, NULL) &&
        clip_span(&dy, &sy, &h, d->h, s->h, NULL)) {
        mark_painted(d);
        int down = (s == d && dy > sy);
        uint32_t *tmp = malloc((size_t)w * 4);
        if (tmp) {
            for (int i = 0; i < h; i++) {
                int yy = down ? h - 1 - i : i;
                const uint32_t *srow = s->px + (size_t)(sy + yy) * s->w + sx;
                if (plane) {
                    for (int x = 0; x < w; x++) tmp[x] = (srow[x] & bit) ? g->fg : g->bg;
                } else {
                    memcpy(tmp, srow, (size_t)w * 4);
                }
                put_row(d, g, dy + yy, dx, tmp, w);
            }
            free(tmp);
        }
    }
    if (g->graphics_exposures) no_expose(c, d, plane ? 63 : 62);
}

/* ── the requests ────────────────────────────────────────────────────────── */
static int req_draw(client_t *c, const uint8_t *q, drawable_t **d, gc_t **g) {
    uint32_t did = r32(q + 4);
    *d = did == ROOT_WINDOW ? &root->d : lookup_drawable(did);
    *g = lookup_gc(r32(q + 8));
    if (!*d) { x_error(c, BadDrawable, did); return 0; }
    if (!*g) { x_error(c, BadGC, r32(q + 8)); return 0; }
    if (!(*d)->px) return 0;                /* InputOnly */
    mark_painted(*d);
    return 1;
}

void draw_dispatch(client_t *c, const uint8_t *q, int qlen) {
    int op = q[0];
    drawable_t *d;
    gc_t *g;
    switch (op) {
    case 53: {                                                 /* CreatePixmap */
        uint32_t pid = r32(q + 4);
        int depth = q[1], w = (int)r16(q + 12), h = (int)r16(q + 14);
        if (!res_check_new(c, pid)) return;
        if (!lookup_drawable(r32(q + 8)) && r32(q + 8) != ROOT_WINDOW) { x_error(c, BadDrawable, r32(q + 8)); return; }
        if (!depth_ok(depth)) { x_error(c, BadValue, (uint32_t)depth); return; }
        if (w == 0 || h == 0) { x_error(c, BadValue, 0); return; }
        pixmap_t *p = calloc(1, sizeof(*p));
        if (p) p->px = calloc((size_t)w * h, 4);
        if (!p || !p->px) { free(p); x_error(c, BadAlloc, pid); return; }
        p->o.id = pid; p->o.type = XT_PIXMAP; p->o.owner = (int8_t)c->index;
        p->w = w; p->h = h; p->depth = depth; p->refs = 1;
        res_add(&p->o);
        break;
    }
    case 54: {                                                 /* FreePixmap */
        xobj_t *o = res_lookup(r32(q + 4));
        if (!o || o->type != XT_PIXMAP) { x_error(c, BadPixmap, r32(q + 4)); return; }
        res_remove(o);
        ((drawable_t *)o)->dead = 1;
        drawable_unref((drawable_t *)o);
        break;
    }
    case 55: {                                                 /* CreateGC */
        uint32_t gid = r32(q + 4);
        if (!res_check_new(c, gid)) return;
        drawable_t *dd = r32(q + 8) == ROOT_WINDOW ? &root->d : lookup_drawable(r32(q + 8));
        if (!dd) { x_error(c, BadDrawable, r32(q + 8)); return; }
        gc_t *ng = gc_new(gid, c->index, dd->depth);
        if (!ng) { x_error(c, BadAlloc, gid); return; }
        if (gc_change(c, ng, r32(q + 12), q + 16, (qlen - 16) / 4) < 0) { gc_free(ng); return; }
        res_add(&ng->o);
        break;
    }
    case 56: {                                                 /* ChangeGC */
        gc_t *gg = lookup_gc(r32(q + 4));
        if (!gg) { x_error(c, BadGC, r32(q + 4)); return; }
        gc_change(c, gg, r32(q + 8), q + 12, (qlen - 12) / 4);
        break;
    }
    case 57: {                                                 /* CopyGC */
        gc_t *s = lookup_gc(r32(q + 4)), *t = lookup_gc(r32(q + 8));
        if (!s || !t) { x_error(c, BadGC, s ? r32(q + 8) : r32(q + 4)); return; }
        gc_copy(t, s, r32(q + 12));
        break;
    }
    case 58: {                                                 /* SetDashes */
        gc_t *gg = lookup_gc(r32(q + 4));
        if (!gg) { x_error(c, BadGC, r32(q + 4)); return; }
        int n = (int)r16(q + 10);
        gg->dash_offset = (int)r16(q + 8);
        gg->ndash = n > 16 ? 16 : n;
        if (12 + gg->ndash <= qlen) memcpy(gg->dashes, q + 12, (size_t)gg->ndash);
        break;
    }
    case 59: {                                                 /* SetClipRectangles */
        gc_t *gg = lookup_gc(r32(q + 4));
        if (!gg) { x_error(c, BadGC, r32(q + 4)); return; }
        int n = (qlen - 12) / 8;
        xrect_t *r = malloc((size_t)(n ? n : 1) * sizeof(xrect_t));
        if (!r) { x_error(c, BadAlloc, 0); return; }
        for (int i = 0; i < n; i++) {
            const uint8_t *p = q + 12 + 8 * i;
            r[i].x = rs16(p); r[i].y = rs16(p + 2); r[i].w = (int)r16(p + 4); r[i].h = (int)r16(p + 6);
        }
        free(gg->clip);
        gg->clip = r; gg->nclip = n;
        gg->clip_x = rs16(q + 8); gg->clip_y = rs16(q + 10);
        if (gg->clip_mask) { drawable_unref(gg->clip_mask); gg->clip_mask = NULL; }
        break;
    }
    case 60: {                                                 /* FreeGC */
        gc_t *gg = lookup_gc(r32(q + 4));
        if (!gg) { x_error(c, BadGC, r32(q + 4)); return; }
        res_remove(&gg->o);
        gc_free(gg);
        break;
    }
    case 62: copy_area(c, q, 0); break;
    case 63: copy_area(c, q, 1); break;
    case 64: {                                                 /* PolyPoint */
        if (!req_draw(c, q, &d, &g)) return;
        int n = (qlen - 12) / 4, x = 0, y = 0;
        for (int i = 0; i < n; i++) {
            int px = rs16(q + 12 + 4 * i), py = rs16(q + 14 + 4 * i);
            if (q[1] && i) { x += px; y += py; } else { x = px; y = py; }
            put_span(d, g, y, x, x + 1);
        }
        break;
    }
    case 65: {                                                 /* PolyLine */
        if (!req_draw(c, q, &d, &g)) return;
        int n = (qlen - 12) / 4;
        int *xy = malloc((size_t)(n ? n : 1) * 2 * sizeof(int));
        if (!xy) return;
        for (int i = 0; i < n; i++) {
            int px = rs16(q + 12 + 4 * i), py = rs16(q + 14 + 4 * i);
            if (q[1] && i) { px += xy[2 * i - 2]; py += xy[2 * i - 1]; }
            xy[2 * i] = px; xy[2 * i + 1] = py;
        }
        int closed = n > 2 && xy[0] == xy[2 * n - 2] && xy[1] == xy[2 * n - 1];
        if (n == 1) put_span(d, g, xy[1], xy[0], xy[0] + 1);
        draw_polyline(d, g, xy, n, closed);
        free(xy);
        break;
    }
    case 66: {                                                 /* PolySegment */
        if (!req_draw(c, q, &d, &g)) return;
        int n = (qlen - 12) / 8;
        for (int i = 0; i < n; i++) {
            const uint8_t *p = q + 12 + 8 * i;
            int xy[4] = { rs16(p), rs16(p + 2), rs16(p + 4), rs16(p + 6) };
            draw_polyline(d, g, xy, 2, 0);
        }
        break;
    }
    case 67: {                                                 /* PolyRectangle */
        if (!req_draw(c, q, &d, &g)) return;
        int n = (qlen - 12) / 8;
        for (int i = 0; i < n; i++) {
            const uint8_t *p = q + 12 + 8 * i;
            int x = rs16(p), y = rs16(p + 2), w = (int)r16(p + 4), h = (int)r16(p + 6);
            if (g->line_width <= 1) {
                fill_rect_gc(d, g, x, y, w + 1, 1);
                fill_rect_gc(d, g, x, y + h, w + 1, 1);
                fill_rect_gc(d, g, x, y + 1, 1, h - 1);
                fill_rect_gc(d, g, x + w, y + 1, 1, h - 1);
            } else {
                int lw = g->line_width, a = lw / 2;
                fill_rect_gc(d, g, x - a, y - a, w + lw, lw);
                fill_rect_gc(d, g, x - a, y + h - a, w + lw, lw);
                fill_rect_gc(d, g, x - a, y - a + lw, lw, h - lw);
                fill_rect_gc(d, g, x + w - a, y - a + lw, lw, h - lw);
            }
        }
        break;
    }
    case 68: case 71: {                                        /* PolyArc / PolyFillArc */
        if (!req_draw(c, q, &d, &g)) return;
        int n = (qlen - 12) / 12;
        for (int i = 0; i < n; i++) {
            const uint8_t *p = q + 12 + 12 * i;
            draw_arc(d, g, rs16(p), rs16(p + 2), (int)r16(p + 4), (int)r16(p + 6),
                     rs16(p + 8), rs16(p + 10), op == 71);
        }
        break;
    }
    case 69: {                                                 /* FillPoly */
        if (!req_draw(c, q, &d, &g)) return;
        int rel = q[13];
        int n = (qlen - 16) / 4;
        dpt_t *p = malloc((size_t)(n ? n : 1) * sizeof(dpt_t));
        if (!p) return;
        for (int i = 0; i < n; i++) {
            double px = rs16(q + 16 + 4 * i), py = rs16(q + 18 + 4 * i);
            if (rel && i) { px += p[i - 1].x; py += p[i - 1].y; }
            p[i].x = px; p[i].y = py;
        }
        fill_polygon(d, g, p, n, g->fill_rule == 1);
        free(p);
        break;
    }
    case 70: {                                                 /* PolyFillRectangle */
        if (!req_draw(c, q, &d, &g)) return;
        int n = (qlen - 12) / 8;
        for (int i = 0; i < n; i++) {
            const uint8_t *p = q + 12 + 8 * i;
            fill_rect_gc(d, g, rs16(p), rs16(p + 2), (int)r16(p + 4), (int)r16(p + 6));
        }
        break;
    }
    case 72: {                                                 /* PutImage */
        uint32_t did = r32(q + 4);
        d = did == ROOT_WINDOW ? &root->d : lookup_drawable(did);
        g = lookup_gc(r32(q + 8));
        if (!d) { x_error(c, BadDrawable, did); return; }
        if (!g) { x_error(c, BadGC, r32(q + 8)); return; }
        put_image(c, d, g, q, qlen);
        break;
    }
    case 73: get_image(c, q); break;
    case 74: case 75: if (req_draw(c, q, &d, &g)) poly_text(c, d, g, q, qlen, op == 75); break;
    case 76: case 77: if (req_draw(c, q, &d, &g)) image_text(d, g, q, qlen, op == 77); break;
    }
}
