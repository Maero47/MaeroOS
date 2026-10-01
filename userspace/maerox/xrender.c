/*
 * maeroX RENDER extension (version 0.11).
 *
 * Pictures wrap a drawable (formats a8r8g8b8, x8r8g8b8, a8, a1) or are pure
 * sources (solid fill, linear/radial/conical gradients).  Composite runs the
 * general pipeline row by row: fetch the source (transform with nearest or
 * bilinear filtering, repeat none/normal/pad/reflect), fetch the mask's alpha
 * (or its four channels with component alpha), combine with the Porter-Duff
 * operator in premultiplied 8-bit ARGB, store in the destination format,
 * clipped to the destination's clip rectangles or clip mask.
 *
 * Trapezoids, triangles, strips, fans and AddTraps are rasterised into an A8
 * coverage mask: each pixel row is sampled at 8 sub-rows and every sub-row's
 * span is accumulated with exact horizontal coverage.  Glyphs are kept per
 * glyph set as A8 (A1 sets are widened) or ARGB32 images.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xs.h"

#define PICTFMT_RGB24  0x30
#define PICTFMT_ARGB32 0x31
#define PICTFMT_A8     0x33
#define PICTFMT_A1     0x34

enum { PK_DRAWABLE, PK_SOLID, PK_LINEAR, PK_RADIAL, PK_CONICAL };

typedef struct picture {
    xobj_t      o;
    drawable_t *d;
    int         kind;
    uint32_t    format;
    int         repeat;            /* 0 None, 1 Normal, 2 Pad, 3 Reflect */
    int         component_alpha;
    uint32_t    solid;             /* premultiplied ARGB */
    double      x1, y1, x2, y2, r1, r2, angle;
    int         nstops;
    double     *soff;
    uint32_t   *scol;              /* non-premultiplied ARGB */
    int         has_xform;
    double      xf[9];
    int         filter;            /* 0 nearest, 1 bilinear */
    int         clip_x, clip_y;
    int         nclip;             /* -1 = none */
    xrect_t    *clip;
    drawable_t *clip_mask;
} picture_t;

typedef struct {
    uint32_t id;
    int      w, h, x, y, xoff, yoff;
    uint8_t *a8;                   /* w*h coverage, or NULL */
    uint32_t *argb;                /* w*h premultiplied, or NULL */
    int      used;
} glyph_t;

typedef struct {
    int      refs;
    uint32_t format;
    glyph_t *g;                    /* open-addressing table */
    int      cap, n;
} glyphset_t;

typedef struct {
    xobj_t      o;
    glyphset_t *gs;
} gsref_t;

/* ── pixel helpers ───────────────────────────────────────────────────────── */
static inline uint32_t mul8(uint32_t a, uint32_t b) {   /* a*b/255 rounded */
    uint32_t t = a * b + 128;
    return (t + (t >> 8)) >> 8;
}

static inline uint32_t premul(uint32_t argb) {
    uint32_t a = argb >> 24;
    if (a == 255) return argb;
    return (a << 24) | (mul8((argb >> 16) & 255, a) << 16) |
           (mul8((argb >> 8) & 255, a) << 8) | mul8(argb & 255, a);
}

static int format_depth(uint32_t f) {
    switch (f) {
    case PICTFMT_ARGB32: return 32;
    case PICTFMT_RGB24: return 24;
    case PICTFMT_A8: return 8;
    case PICTFMT_A1: return 1;
    default: return 0;
    }
}

static uint32_t format_for_depth(int depth) {
    switch (depth) {
    case 32: return PICTFMT_ARGB32;
    case 24: return PICTFMT_RGB24;
    case 8: return PICTFMT_A8;
    case 1: return PICTFMT_A1;
    default: return PICTFMT_RGB24;
    }
}

static inline uint32_t load_px(uint32_t format, uint32_t v) {
    switch (format) {
    case PICTFMT_ARGB32: return v;
    case PICTFMT_A8: return (v & 0xFF) << 24;
    case PICTFMT_A1: return (v & 1) ? 0xFF000000u : 0;
    default: return 0xFF000000u | (v & 0xFFFFFF);
    }
}

static inline uint32_t store_px(uint32_t format, uint32_t v) {
    switch (format) {
    case PICTFMT_ARGB32: return v;
    case PICTFMT_A8: return v >> 24;
    case PICTFMT_A1: return (v >> 24) >= 0x80 ? 1 : 0;
    default: return v & 0xFFFFFF;
    }
}

static int repeat_coord(int v, int n, int mode) {
    if (n <= 0) return -1;
    if (v >= 0 && v < n) return v;
    switch (mode) {
    case 1: { int r = v % n; return r < 0 ? r + n : r; }
    case 2: return v < 0 ? 0 : n - 1;
    case 3: {
        int p = 2 * n, r = v % p;
        if (r < 0) r += p;
        return r < n ? r : p - 1 - r;
    }
    default: return -1;
    }
}

static inline uint32_t sample(picture_t *p, int x, int y) {
    drawable_t *d = p->d;
    int xx = repeat_coord(x, d->w, p->repeat), yy = repeat_coord(y, d->h, p->repeat);
    if (xx < 0 || yy < 0) return 0;
    return load_px(p->format, d->px[(size_t)yy * d->w + xx]);
}

static uint32_t lerp_px(uint32_t a, uint32_t b, uint32_t t) {   /* t 0..256 */
    uint32_t r = 0;
    for (int s = 0; s < 32; s += 8) {
        uint32_t ca = (a >> s) & 255, cb = (b >> s) & 255;
        r |= (((ca * (256 - t) + cb * t) >> 8) & 255) << s;
    }
    return r;
}

static uint32_t stop_color(picture_t *p, double t) {
    if (p->nstops == 0) return 0;
    if (p->repeat == 0 && (t < 0 || t > 1)) return 0;
    if (p->repeat == 1) t -= floor(t);
    else if (p->repeat == 3) { t = fmod(fabs(t), 2.0); if (t > 1) t = 2 - t; }
    if (t <= p->soff[0]) return premul(p->scol[0]);
    for (int i = 1; i < p->nstops; i++) {
        if (t <= p->soff[i]) {
            double span = p->soff[i] - p->soff[i - 1];
            uint32_t f = span > 0 ? (uint32_t)((t - p->soff[i - 1]) / span * 256) : 256;
            if (f > 256) f = 256;
            return premul(lerp_px(p->scol[i - 1], p->scol[i], f));
        }
    }
    return premul(p->scol[p->nstops - 1]);
}

static void xform_pt(picture_t *p, double x, double y, double *u, double *v) {
    if (!p->has_xform) { *u = x; *v = y; return; }
    double w = p->xf[6] * x + p->xf[7] * y + p->xf[8];
    if (w == 0) w = 1e-9;
    *u = (p->xf[0] * x + p->xf[1] * y + p->xf[2]) / w;
    *v = (p->xf[3] * x + p->xf[4] * y + p->xf[5]) / w;
}

static uint32_t gradient_at(picture_t *p, double px, double py) {
    double u, v;
    xform_pt(p, px, py, &u, &v);
    if (p->kind == PK_LINEAR) {
        double dx = p->x2 - p->x1, dy = p->y2 - p->y1, l = dx * dx + dy * dy;
        if (l == 0) return 0;
        return stop_color(p, ((u - p->x1) * dx + (v - p->y1) * dy) / l);
    }
    if (p->kind == PK_RADIAL) {
        double cdx = p->x2 - p->x1, cdy = p->y2 - p->y1, dr = p->r2 - p->r1;
        double a = cdx * cdx + cdy * cdy - dr * dr;
        double pdx = u - p->x1, pdy = v - p->y1;
        double b = pdx * cdx + pdy * cdy + p->r1 * dr;
        double c = pdx * pdx + pdy * pdy - p->r1 * p->r1;
        double t;
        if (fabs(a) < 1e-9) {
            if (fabs(b) < 1e-9) return 0;
            t = c / (2 * b);
            if (p->r1 + t * dr < 0) return 0;
        } else {
            double disc = b * b - a * c;
            if (disc < 0) return 0;
            double sq = sqrt(disc), t1 = (b + sq) / a, t2 = (b - sq) / a;
            if (t1 < t2) { double s = t1; t1 = t2; t2 = s; }
            if (p->r1 + t1 * dr >= 0) t = t1;
            else if (p->r1 + t2 * dr >= 0) t = t2;
            else return 0;
        }
        return stop_color(p, t);
    }
    /* conical: the angle around the centre, from p->angle, as 0..1 */
    double ang = atan2(v - p->y1, u - p->x1) - p->angle * M_PI / 180.0;
    double t = ang / (2 * M_PI);
    t -= floor(t);
    return stop_color(p, t);
}

/* Source pixels for n destination pixels starting at picture coordinate
 * (x, y). */
static void fetch_row(picture_t *p, int x, int y, int n, uint32_t *out) {
    if (p->kind == PK_SOLID) {
        for (int i = 0; i < n; i++) out[i] = p->solid;
        return;
    }
    if (p->kind != PK_DRAWABLE) {
        for (int i = 0; i < n; i++) out[i] = gradient_at(p, x + i + 0.5, y + 0.5);
        return;
    }
    drawable_t *d = p->d;
    if (!d || !d->px) { memset(out, 0, (size_t)n * 4); return; }
    if (!p->has_xform) {
        int yy = repeat_coord(y, d->h, p->repeat);
        if (yy < 0) { memset(out, 0, (size_t)n * 4); return; }
        const uint32_t *row = d->px + (size_t)yy * d->w;
        if (p->repeat == 0 && x >= 0 && x + n <= d->w) {
            switch (p->format) {
            case PICTFMT_ARGB32: memcpy(out, row + x, (size_t)n * 4); return;
            case PICTFMT_RGB24: for (int i = 0; i < n; i++) out[i] = 0xFF000000u | row[x + i]; return;
            default: break;
            }
        }
        for (int i = 0; i < n; i++) {
            int xx = repeat_coord(x + i, d->w, p->repeat);
            out[i] = xx < 0 ? 0 : load_px(p->format, row[xx]);
        }
        return;
    }
    for (int i = 0; i < n; i++) {
        double u, v;
        xform_pt(p, x + i + 0.5, y + 0.5, &u, &v);
        if (p->filter == 0) {
            out[i] = sample(p, (int)floor(u), (int)floor(v));
        } else {
            u -= 0.5; v -= 0.5;
            int x0 = (int)floor(u), y0 = (int)floor(v);
            uint32_t fx = (uint32_t)((u - x0) * 256), fy = (uint32_t)((v - y0) * 256);
            uint32_t a = sample(p, x0, y0), b = sample(p, x0 + 1, y0);
            uint32_t c = sample(p, x0, y0 + 1), e = sample(p, x0 + 1, y0 + 1);
            out[i] = lerp_px(lerp_px(a, b, fx), lerp_px(c, e, fx), fy);
        }
    }
}

/* Porter-Duff factors for operator op given source and destination alpha. */
static inline void pd_factors(int op, uint32_t as, uint32_t ad, uint32_t *fa, uint32_t *fb) {
    switch (op) {
    case 0: *fa = 0; *fb = 0; break;
    case 1: *fa = 255; *fb = 0; break;
    case 2: *fa = 0; *fb = 255; break;
    case 3: *fa = 255; *fb = 255 - as; break;
    case 4: *fa = 255 - ad; *fb = 255; break;
    case 5: *fa = ad; *fb = 0; break;
    case 6: *fa = 0; *fb = as; break;
    case 7: *fa = 255 - ad; *fb = 0; break;
    case 8: *fa = 0; *fb = 255 - as; break;
    case 9: *fa = ad; *fb = 255 - as; break;
    case 10: *fa = 255 - ad; *fb = as; break;
    case 11: *fa = 255 - ad; *fb = 255 - as; break;
    case 12: *fa = 255; *fb = 255; break;
    case 13: *fa = as ? (255 - ad >= as ? 255 : (255 - ad) * 255 / as) : 255; *fb = 255; break;
    default: *fa = 255; *fb = 255 - as; break;
    }
}

static inline uint32_t combine(int op, uint32_t s, uint32_t d) {
    if (op == 3) {                                          /* Over, the common case */
        uint32_t as = s >> 24;
        if (as == 255) return s;
        if (as == 0) return d;
    }
    uint32_t fa, fb;
    pd_factors(op, s >> 24, d >> 24, &fa, &fb);
    uint32_t r = 0;
    for (int sh = 0; sh < 32; sh += 8) {
        uint32_t v = mul8((s >> sh) & 255, fa) + mul8((d >> sh) & 255, fb);
        if (v > 255) v = 255;
        r |= v << sh;
    }
    return r;
}

static inline uint32_t in_mask(uint32_t s, uint32_t m, int ca) {
    if (!ca) {
        uint32_t a = m >> 24;
        if (a == 255) return s;
        if (a == 0) return 0;
        return (mul8(s >> 24, a) << 24) | (mul8((s >> 16) & 255, a) << 16) |
               (mul8((s >> 8) & 255, a) << 8) | mul8(s & 255, a);
    }
    return (mul8(s >> 24, m >> 24) << 24) | (mul8((s >> 16) & 255, (m >> 16) & 255) << 16) |
           (mul8((s >> 8) & 255, (m >> 8) & 255) << 8) | mul8(s & 255, m & 255);
}

/* ── the composite pipeline ──────────────────────────────────────────────── */
typedef struct {
    int op;
    picture_t *src, *mask, *dst;
    int sx, sy, mx, my;            /* src/mask coordinate of dst (dx, dy) */
    int dx, dy;
    const uint8_t *a8mask;         /* or an A8 buffer (traps, glyphs) */
    const uint32_t *argbmask;
    int amx, amy, amw, amh;        /* that buffer's placement in dst coords */
} comp_t;

static void comp_span(comp_t *k, int y, int a, int b, uint32_t *sbuf, uint32_t *mbuf) {
    drawable_t *d = k->dst->d;
    int n = b - a;
    fetch_row(k->src, k->sx + (a - k->dx), k->sy + (y - k->dy), n, sbuf);
    int have_mask = 0, ca = 0;
    if (k->mask) {
        fetch_row(k->mask, k->mx + (a - k->dx), k->my + (y - k->dy), n, mbuf);
        have_mask = 1;
        ca = k->mask->component_alpha;
    } else if (k->a8mask) {
        int my = y - k->amy;
        for (int i = 0; i < n; i++) {
            int mx = a + i - k->amx;
            mbuf[i] = (my >= 0 && my < k->amh && mx >= 0 && mx < k->amw)
                    ? (uint32_t)k->a8mask[(size_t)my * k->amw + mx] << 24 : 0;
        }
        have_mask = 1;
    } else if (k->argbmask) {
        int my = y - k->amy;
        for (int i = 0; i < n; i++) {
            int mx = a + i - k->amx;
            mbuf[i] = (my >= 0 && my < k->amh && mx >= 0 && mx < k->amw)
                    ? k->argbmask[(size_t)my * k->amw + mx] : 0;
        }
        have_mask = 1;
        ca = 1;
    }
    uint32_t *row = d->px + (size_t)y * d->w;
    uint32_t f = k->dst->format;
    for (int i = 0; i < n; i++) {
        uint32_t s = sbuf[i];
        if (have_mask) s = in_mask(s, mbuf[i], ca);
        uint32_t dv = load_px(f, row[a + i]);
        uint32_t r;
        if (ca && have_mask && k->op == 3) {
            /* component alpha Over: per channel dst*(1-src.a*mask.c) */
            uint32_t sa = k->src->kind == PK_SOLID ? k->src->solid >> 24 : sbuf[i] >> 24;
            uint32_t m = mbuf[i];
            r = 0;
            for (int sh = 0; sh < 32; sh += 8) {
                uint32_t mc = sh == 24 ? (m >> 24) : (m >> sh) & 255;
                uint32_t v = ((s >> sh) & 255) + mul8((dv >> sh) & 255, 255 - mul8(sa, mc));
                r |= (v > 255 ? 255 : v) << sh;
            }
        } else {
            r = combine(k->op, s, dv);
        }
        row[a + i] = store_px(f, r);
    }
    damage_drawable(d, a, y, n, 1);
}

/* Composite the dst rectangle (x, y, w, h), clipped to the dst drawable and
 * the dst picture's clip. */
static void composite_rect(comp_t *k, int x, int y, int w, int h) {
    picture_t *dp = k->dst;
    drawable_t *d = dp->d;
    if (!d || !d->px || w <= 0 || h <= 0) return;
    if (!clip_span(&x, NULL, &w, d->w, 0, NULL) || !clip_span(&y, NULL, &h, d->h, 0, NULL)) return;
    mark_painted(d);
    uint32_t *sbuf = malloc((size_t)w * 8);
    if (!sbuf) return;
    uint32_t *mbuf = sbuf + w;
    for (int yy = y; yy < y + h; yy++) {
        if (dp->nclip >= 0) {
            for (int i = 0; i < dp->nclip; i++) {
                xrect_t *r = &dp->clip[i];
                int ry = r->y + dp->clip_y, rx = r->x + dp->clip_x;
                if (yy < ry || yy >= ry + r->h) continue;
                int a = imax(x, rx), b = imin(x + w, rx + r->w);
                if (a < b) comp_span(k, yy, a, b, sbuf, mbuf);
            }
        } else if (dp->clip_mask && dp->clip_mask->px) {
            drawable_t *m = dp->clip_mask;
            int my = yy - dp->clip_y;
            if (my < 0 || my >= m->h) continue;
            int xx = x;
            while (xx < x + w) {
                int mx = xx - dp->clip_x;
                if (mx < 0 || mx >= m->w || !m->px[(size_t)my * m->w + mx]) { xx++; continue; }
                int s = xx;
                while (xx < x + w && xx - dp->clip_x < m->w && m->px[(size_t)my * m->w + xx - dp->clip_x]) xx++;
                comp_span(k, yy, s, xx, sbuf, mbuf);
            }
        } else {
            comp_span(k, yy, x, x + w, sbuf, mbuf);
        }
    }
    free(sbuf);
}

/* ── coverage rasteriser (traps, triangles) ──────────────────────────────── */
#define SUBROWS 8
typedef struct { int x, y, w, h; uint16_t *acc; } covbuf_t;

static double fx2d(uint32_t v) { return (int32_t)v / 65536.0; }

/* Add the coverage of [xl, xr) on one sub-row (weight 256/SUBROWS) to row. */
static void cov_span(covbuf_t *cb, int row, double xl, double xr) {
    if (xr <= xl || row < 0 || row >= cb->h) return;
    xl -= cb->x; xr -= cb->x;
    if (xl < 0) xl = 0;
    if (xr > cb->w) xr = cb->w;
    if (xr <= xl) return;
    uint16_t *r = cb->acc + (size_t)row * cb->w;
    const int W = 256 / SUBROWS;
    int il = (int)floor(xl), ir = (int)floor(xr);
    if (il == ir) { r[il] += (uint16_t)((xr - xl) * W); return; }
    r[il] += (uint16_t)((il + 1 - xl) * W);
    for (int i = il + 1; i < ir; i++) r[i] += (uint16_t)W;
    if (ir < cb->w) r[ir] += (uint16_t)((xr - ir) * W);
}

typedef struct { double x, y; } fpt_t;

/* A convex polygon (trapezoid or triangle) between ytop and ybot. */
/* Rows outside the coverage buffer contribute nothing: clamp the walk to it
 * (a trapezoid 65535 rows tall cost every row of every sub-row before). */
static void cov_rows(const covbuf_t *cb, double top, double bot, int *y0, int *y1) {
    double a = floor(top), b = ceil(bot);
    if (a < cb->y) a = cb->y;
    if (b > cb->y + cb->h) b = cb->y + cb->h;
    *y0 = (int)a; *y1 = b > a ? (int)b : (int)a;
}

static void cov_convex(covbuf_t *cb, const fpt_t *p, int n, double ytop, double ybot) {
    int y0, y1;
    if (!(ybot > ytop)) return;
    cov_rows(cb, ytop, ybot, &y0, &y1);
    for (int y = y0; y < y1; y++) {
        for (int s = 0; s < SUBROWS; s++) {
            double sy = y + (s + 0.5) / SUBROWS;
            if (sy < ytop || sy >= ybot) continue;
            double xl = 1e30, xr = -1e30;
            int hits = 0;
            for (int i = 0; i < n; i++) {
                const fpt_t *a = &p[i], *b = &p[(i + 1) % n];
                double lo = a->y < b->y ? a->y : b->y, hi = a->y < b->y ? b->y : a->y;
                if (sy < lo || sy > hi || a->y == b->y) continue;
                double x = a->x + (sy - a->y) * (b->x - a->x) / (b->y - a->y);
                if (x < xl) xl = x;
                if (x > xr) xr = x;
                hits++;
            }
            if (hits >= 2) cov_span(cb, y - cb->y, xl, xr);
        }
    }
}

/* A Render trapezoid: horizontal top/bottom, left and right edges as lines
 * (which may extend past top/bottom). */
static void cov_trap(covbuf_t *cb, const uint8_t *t) {
    double top = fx2d(r32(t)), bot = fx2d(r32(t + 4));
    double l1x = fx2d(r32(t + 8)), l1y = fx2d(r32(t + 12)), l2x = fx2d(r32(t + 16)), l2y = fx2d(r32(t + 20));
    double r1x = fx2d(r32(t + 24)), r1y = fx2d(r32(t + 28)), r2x = fx2d(r32(t + 32)), r2y = fx2d(r32(t + 36));
    if (bot <= top || l1y == l2y || r1y == r2y) return;
    int y0, y1;
    cov_rows(cb, top, bot, &y0, &y1);
    for (int y = y0; y < y1; y++)
        for (int s = 0; s < SUBROWS; s++) {
            double sy = y + (s + 0.5) / SUBROWS;
            if (sy < top || sy >= bot) continue;
            double xl = l1x + (sy - l1y) * (l2x - l1x) / (l2y - l1y);
            double xr = r1x + (sy - r1y) * (r2x - r1x) / (r2y - r1y);
            cov_span(cb, y - cb->y, xl, xr);
        }
}

static int cov_alloc(covbuf_t *cb, drawable_t *d, double x0, double y0, double x1, double y1) {
    int ix0 = (int)floor(x0), iy0 = (int)floor(y0), ix1 = (int)ceil(x1), iy1 = (int)ceil(y1);
    if (ix0 < 0) ix0 = 0;
    if (iy0 < 0) iy0 = 0;
    if (ix1 > d->w) ix1 = d->w;
    if (iy1 > d->h) iy1 = d->h;
    if (ix1 <= ix0 || iy1 <= iy0) return 0;
    cb->x = ix0; cb->y = iy0; cb->w = ix1 - ix0; cb->h = iy1 - iy0;
    cb->acc = calloc((size_t)cb->w * cb->h, sizeof(uint16_t));
    return cb->acc != NULL;
}

static uint8_t *cov_to_a8(covbuf_t *cb) {
    size_t n = (size_t)cb->w * cb->h;
    uint8_t *m = malloc(n ? n : 1);
    if (!m) return NULL;
    for (size_t i = 0; i < n; i++) m[i] = cb->acc[i] >= 255 ? 255 : (uint8_t)cb->acc[i];
    return m;
}

/* ── glyph sets ──────────────────────────────────────────────────────────── */
static glyph_t *glyph_slot(glyphset_t *gs, uint32_t id, int create) {
    if (create && (gs->n + 1) * 2 > gs->cap) {
        int ncap = gs->cap ? gs->cap * 2 : 256;
        glyph_t *ng = calloc((size_t)ncap, sizeof(glyph_t));
        if (!ng) return NULL;
        for (int i = 0; i < gs->cap; i++) {
            if (!gs->g[i].used) continue;
            unsigned h = (gs->g[i].id * 2654435761u) & (unsigned)(ncap - 1);
            while (ng[h].used) h = (h + 1) & (unsigned)(ncap - 1);
            ng[h] = gs->g[i];
        }
        free(gs->g);
        gs->g = ng;
        gs->cap = ncap;
    }
    if (!gs->cap) return NULL;
    unsigned h = (id * 2654435761u) & (unsigned)(gs->cap - 1);
    while (gs->g[h].used) {
        if (gs->g[h].id == id) return &gs->g[h];
        h = (h + 1) & (unsigned)(gs->cap - 1);
    }
    if (!create) return NULL;
    gs->g[h].used = 1;
    gs->g[h].id = id;
    gs->n++;
    return &gs->g[h];
}

static void glyph_remove(glyphset_t *gs, uint32_t id) {
    glyph_t *g = glyph_slot(gs, id, 0);
    if (!g) return;
    free(g->a8); free(g->argb);
    /* Re-insert the rest of the probe chain (open addressing delete). */
    unsigned h = (unsigned)(g - gs->g);
    g->used = 0; g->a8 = NULL; g->argb = NULL;
    gs->n--;
    for (unsigned j = (h + 1) & (unsigned)(gs->cap - 1); gs->g[j].used;
         j = (j + 1) & (unsigned)(gs->cap - 1)) {
        glyph_t tmp = gs->g[j];
        gs->g[j].used = 0;
        gs->n--;
        glyph_t *ng = glyph_slot(gs, tmp.id, 1);
        if (ng) { *ng = tmp; }
    }
}

static void glyphset_unref(glyphset_t *gs) {
    if (--gs->refs > 0) return;
    for (int i = 0; i < gs->cap; i++)
        if (gs->g[i].used) { free(gs->g[i].a8); free(gs->g[i].argb); }
    free(gs->g);
    free(gs);
}

/* ── resources ───────────────────────────────────────────────────────────── */
static picture_t *lookup_picture(uint32_t id) {
    xobj_t *o = res_lookup(id);
    return (o && o->type == XT_PICTURE) ? (picture_t *)o : NULL;
}

static glyphset_t *lookup_glyphset(uint32_t id) {
    xobj_t *o = res_lookup(id);
    return (o && o->type == XT_GLYPHSET) ? ((gsref_t *)o)->gs : NULL;
}

void render_free(xobj_t *o) {
    if (o->type == XT_PICTURE) {
        picture_t *p = (picture_t *)o;
        if (p->d) drawable_unref(p->d);
        if (p->clip_mask) drawable_unref(p->clip_mask);
        free(p->clip); free(p->soff); free(p->scol);
        free(p);
    } else if (o->type == XT_GLYPHSET) {
        glyphset_unref(((gsref_t *)o)->gs);
        free(o);
    }
}

static picture_t *picture_new(client_t *c, uint32_t id, int kind) {
    if (!res_check_new(c, id)) return NULL;
    picture_t *p = calloc(1, sizeof(*p));
    if (!p) { x_error(c, BadAlloc, id); return NULL; }
    p->o.id = id; p->o.type = XT_PICTURE; p->o.owner = (int8_t)c->index;
    p->kind = kind;
    p->nclip = -1;
    p->filter = 0;
    return p;
}

static int mask_bits(uint32_t m) {
    int n = 0;
    for (; m; m &= m - 1) n++;
    return n;
}

static int picture_attrs(client_t *c, picture_t *p, uint32_t mask, const uint8_t *v, int nvals) {
    if (mask_bits(mask & 0x1FFF) > nvals) { x_error(c, BadLength, 0); return -1; }
    for (int bit = 0; bit < 13; bit++) {
        if (!(mask & (1u << bit))) continue;
        uint32_t val = r32(v);
        v += 4;
        switch (bit) {
        case 0: p->repeat = (int)val; break;
        case 4: p->clip_x = (int)(int32_t)val; break;
        case 5: p->clip_y = (int)(int32_t)val; break;
        case 6:
            free(p->clip); p->clip = NULL; p->nclip = -1;
            if (p->clip_mask) { drawable_unref(p->clip_mask); p->clip_mask = NULL; }
            if (val) {
                xobj_t *o = res_lookup(val);
                if (!o || o->type != XT_PIXMAP) { x_error(c, BadPixmap, val); return -1; }
                p->clip_mask = (drawable_t *)o;
                p->clip_mask->refs++;
            }
            break;
        case 12: p->component_alpha = (int)(val & 1); break;
        default: break;
        }
    }
    return 0;
}

static int read_stops(picture_t *p, const uint8_t *q, int qlen, int off, int n) {
    if (n < 0 || n > 4096 || off + n * 12 > qlen) return -1;
    p->nstops = n;
    p->soff = malloc((size_t)(n ? n : 1) * sizeof(double));
    p->scol = malloc((size_t)(n ? n : 1) * sizeof(uint32_t));
    if (!p->soff || !p->scol) return -1;
    for (int i = 0; i < n; i++) p->soff[i] = fx2d(r32(q + off + 4 * i));
    const uint8_t *cs = q + off + 4 * n;
    for (int i = 0; i < n; i++) {
        uint32_t r = r16(cs + 8 * i) >> 8, g = r16(cs + 8 * i + 2) >> 8;
        uint32_t b = r16(cs + 8 * i + 4) >> 8, a = r16(cs + 8 * i + 6) >> 8;
        p->scol[i] = (a << 24) | (r << 16) | (g << 8) | b;
    }
    return 0;
}

/* ── requests ────────────────────────────────────────────────────────────── */
static int render_min_len(int minor) {
    switch (minor) {
    case 0: return 12;  case 2: return 8;   case 4: return 20;  case 5: return 12;
    case 6: return 12;  case 7: return 8;   case 8: return 36;  case 10: case 11:
    case 12: case 13: return 24;            case 17: return 12; case 18: return 12;
    case 19: return 8;  case 20: return 12; case 22: return 8;  case 23: case 24:
    case 25: return 28; case 26: return 20; case 27: return 16; case 28: return 44;
    case 29: return 8;  case 30: return 12; case 31: return 8;  case 32: return 12;
    case 33: return 16; case 34: return 28; case 35: return 36; case 36: return 24;
    default: return 4;
    }
}

static void query_pict_formats(client_t *c) {
    uint8_t ex[512];
    int n = 0;
    struct { uint32_t id; int depth, rs, rm, gs, gm, bs, bm, as, am; } f[] = {
        { PICTFMT_RGB24, 24, 16, 0xff, 8, 0xff, 0, 0xff, 0, 0 },
        { PICTFMT_ARGB32, 32, 16, 0xff, 8, 0xff, 0, 0xff, 24, 0xff },
        { PICTFMT_A8, 8, 0, 0, 0, 0, 0, 0, 0, 0xff },
        { PICTFMT_A1, 1, 0, 0, 0, 0, 0, 0, 0, 1 },
    };
    for (int i = 0; i < 4; i++) {
        uint8_t *b = ex + n;
        memset(b, 0, 28);
        put32(b, f[i].id); b[4] = 1; b[5] = (uint8_t)f[i].depth;
        put16(b + 8, (uint32_t)f[i].rs); put16(b + 10, (uint32_t)f[i].rm);
        put16(b + 12, (uint32_t)f[i].gs); put16(b + 14, (uint32_t)f[i].gm);
        put16(b + 16, (uint32_t)f[i].bs); put16(b + 18, (uint32_t)f[i].bm);
        put16(b + 20, (uint32_t)f[i].as); put16(b + 22, (uint32_t)f[i].am);
        n += 28;
    }
    /* One screen: depth 24 with the root visual, depths 32/8/1 without
     * visuals. */
    put32(ex + n, 4); put32(ex + n + 4, PICTFMT_RGB24); n += 8;
    ex[n] = 24; ex[n + 1] = 0; put16(ex + n + 2, 1); put32(ex + n + 4, 0); n += 8;
    put32(ex + n, ROOT_VISUAL); put32(ex + n + 4, PICTFMT_RGB24); n += 8;
    int dd[3] = { 32, 8, 1 };
    for (int i = 0; i < 3; i++) {
        ex[n] = (uint8_t)dd[i]; ex[n + 1] = 0; put16(ex + n + 2, 0); put32(ex + n + 4, 0); n += 8;
    }
    put32(ex + n, 0); n += 4;                 /* subpixel: Unknown */
    uint8_t d[24];
    memset(d, 0, sizeof(d));
    put32(d + 0, 4);    /* formats */
    put32(d + 4, 1);    /* screens */
    put32(d + 8, 4);    /* depths */
    put32(d + 12, 1);   /* visuals */
    put32(d + 16, 1);   /* subpixels */
    send_reply_var(c, 0, d, ex, n);
}

static void query_filters(client_t *c) {
    static const char *names[] = { "nearest", "bilinear", "fast", "good", "best" };
    uint8_t buf[128];
    int n = 0, nf = 5;
    for (int i = 0; i < nf; i++) put16(buf + 2 * i, 0xFFFF);   /* no aliases */
    n = 2 * nf;
    for (int i = 0; i < nf; i++) {
        int l = (int)strlen(names[i]);
        buf[n++] = (uint8_t)l;
        memcpy(buf + n, names[i], (size_t)l);
        n += l;
    }
    uint8_t d[24];
    memset(d, 0, sizeof(d));
    put32(d, (uint32_t)nf);
    put32(d + 4, (uint32_t)nf);
    send_reply_var(c, 0, d, buf, n);
}

/* Render a coverage buffer: src IN coverage OP dst, src aligned so that the
 * dst point (xdst0, ydst0) takes the src point (xsrc, ysrc). */
static void composite_coverage(int op, picture_t *src, picture_t *dst, covbuf_t *cb,
                               int xsrc, int ysrc, int xdst0, int ydst0) {
    uint8_t *m = cov_to_a8(cb);
    if (!m) return;
    comp_t k;
    memset(&k, 0, sizeof(k));
    k.op = op; k.src = src; k.dst = dst;
    k.dx = xdst0; k.dy = ydst0; k.sx = xsrc; k.sy = ysrc;
    k.a8mask = m; k.amx = cb->x; k.amy = cb->y; k.amw = cb->w; k.amh = cb->h;
    composite_rect(&k, cb->x, cb->y, cb->w, cb->h);
    free(m);
}

static void do_triangles(client_t *c, const uint8_t *q, int qlen, int minor) {
    int op = q[4];
    picture_t *src = lookup_picture(r32(q + 8)), *dst = lookup_picture(r32(q + 12));
    if (!src || !dst) { x_error(c, RENDER_ERROR_BASE + 1, 0); return; }
    if (!dst->d || !dst->d->px) return;
    int xsrc = rs16(q + 20), ysrc = rs16(q + 22);
    int np = (qlen - 24) / 8;
    if (np < 3) return;
    fpt_t *pt = malloc((size_t)np * sizeof(fpt_t));
    if (!pt) return;
    for (int i = 0; i < np; i++) { pt[i].x = fx2d(r32(q + 24 + 8 * i)); pt[i].y = fx2d(r32(q + 28 + 8 * i)); }
    int ntri = minor == 11 ? np / 3 : np - 2;
    double x0 = 1e30, y0 = 1e30, x1 = -1e30, y1 = -1e30;
    for (int i = 0; i < np; i++) {
        if (pt[i].x < x0) x0 = pt[i].x;
        if (pt[i].x > x1) x1 = pt[i].x;
        if (pt[i].y < y0) y0 = pt[i].y;
        if (pt[i].y > y1) y1 = pt[i].y;
    }
    covbuf_t cb;
    if (cov_alloc(&cb, dst->d, x0, y0, x1, y1)) {
        for (int t = 0; t < ntri; t++) {
            fpt_t tri[3];
            if (minor == 11) { tri[0] = pt[3 * t]; tri[1] = pt[3 * t + 1]; tri[2] = pt[3 * t + 2]; }
            else if (minor == 12) { tri[0] = pt[t]; tri[1] = pt[t + 1]; tri[2] = pt[t + 2]; }
            else { tri[0] = pt[0]; tri[1] = pt[t + 1]; tri[2] = pt[t + 2]; }
            double ty0 = tri[0].y, ty1 = tri[0].y;
            for (int i = 1; i < 3; i++) { if (tri[i].y < ty0) ty0 = tri[i].y; if (tri[i].y > ty1) ty1 = tri[i].y; }
            cov_convex(&cb, tri, 3, ty0, ty1);
        }
        composite_coverage(op, src, dst, &cb, xsrc, ysrc, (int)floor(pt[0].x), (int)floor(pt[0].y));
        free(cb.acc);
    }
    free(pt);
}

static void do_trapezoids(client_t *c, const uint8_t *q, int qlen) {
    int op = q[4];
    picture_t *src = lookup_picture(r32(q + 8)), *dst = lookup_picture(r32(q + 12));
    if (!src || !dst) { x_error(c, RENDER_ERROR_BASE + 1, 0); return; }
    if (!dst->d || !dst->d->px) return;
    int xsrc = rs16(q + 20), ysrc = rs16(q + 22);
    int nt = (qlen - 24) / 40;
    if (nt <= 0) return;
    double x0 = 1e30, y0 = 1e30, x1 = -1e30, y1 = -1e30;
    for (int i = 0; i < nt; i++) {
        const uint8_t *t = q + 24 + 40 * i;
        double top = fx2d(r32(t)), bot = fx2d(r32(t + 4));
        if (top < y0) y0 = top;
        if (bot > y1) y1 = bot;
        for (int k = 0; k < 4; k++) {
            double x = fx2d(r32(t + 8 + 8 * k));
            if (x < x0) x0 = x;
            if (x > x1) x1 = x;
        }
    }
    /* Edges may lean outward past their end points between top and bottom;
     * widen by the slope's reach so the box holds the whole shape. */
    x0 -= 2; x1 += 2;
    if (x0 < -1e6) x0 = -1e6;
    if (x1 > 1e6) x1 = 1e6;
    covbuf_t cb;
    if (!cov_alloc(&cb, dst->d, x0, y0, x1, y1)) return;
    for (int i = 0; i < nt; i++) cov_trap(&cb, q + 24 + 40 * i);
    const uint8_t *t0 = q + 24;
    composite_coverage(op, src, dst, &cb, xsrc, ysrc,
                       (int)floor(fx2d(r32(t0 + 8))), (int)floor(fx2d(r32(t0 + 12))));
    free(cb.acc);
}

static void do_add_traps(client_t *c, const uint8_t *q, int qlen) {
    picture_t *p = lookup_picture(r32(q + 4));
    if (!p) { x_error(c, RENDER_ERROR_BASE + 1, r32(q + 4)); return; }
    if (!p->d || !p->d->px) return;
    int xo = rs16(q + 8), yo = rs16(q + 10);
    int nt = (qlen - 12) / 24;
    for (int i = 0; i < nt; i++) {
        const uint8_t *t = q + 12 + 24 * i;
        double tl = fx2d(r32(t)) + xo, tr = fx2d(r32(t + 4)) + xo, ty = fx2d(r32(t + 8)) + yo;
        double bl = fx2d(r32(t + 12)) + xo, br = fx2d(r32(t + 16)) + xo, by = fx2d(r32(t + 20)) + yo;
        fpt_t poly[4] = { { tl, ty }, { tr, ty }, { br, by }, { bl, by } };
        covbuf_t cb;
        double x0 = fmin(tl, bl), x1 = fmax(tr, br);
        if (!cov_alloc(&cb, p->d, x0, ty, x1, by)) continue;
        cov_convex(&cb, poly, 4, ty, by);
        for (int y = 0; y < cb.h; y++)
            for (int x = 0; x < cb.w; x++) {
                uint32_t a = cb.acc[(size_t)y * cb.w + x];
                if (!a) continue;
                uint32_t *dp = &p->d->px[(size_t)(cb.y + y) * p->d->w + cb.x + x];
                uint32_t cur = load_px(p->format, *dp) >> 24;
                uint32_t v = cur + (a > 255 ? 255 : a);
                *dp = store_px(p->format, (v > 255 ? 255 : v) << 24);
            }
        damage_drawable(p->d, cb.x, cb.y, cb.w, cb.h);
        free(cb.acc);
    }
}

static void do_add_glyphs(client_t *c, const uint8_t *q, int qlen) {
    xobj_t *o = res_lookup(r32(q + 4));
    if (!o || o->type != XT_GLYPHSET) { x_error(c, RENDER_ERROR_BASE + 3, r32(q + 4)); return; }
    glyphset_t *gs = ((gsref_t *)o)->gs;
    uint32_t ng = r32(q + 8);
    if (ng > 65536 || 12 + (uint64_t)ng * 16 > (uint64_t)qlen) { x_error(c, BadLength, 0); return; }
    const uint8_t *ids = q + 12, *infos = ids + ng * 4, *img = infos + ng * 12;
    size_t off = 0, avail = (size_t)qlen - (size_t)(img - q);
    int fmt = (int)gs->format;
    for (uint32_t i = 0; i < ng; i++) {
        const uint8_t *gi = infos + 12 * i;
        int w = (int)r16(gi), h = (int)r16(gi + 2);
        int stride = fmt == PICTFMT_A1 ? ((w + 31) / 32) * 4 : fmt == PICTFMT_ARGB32 ? w * 4 : (w + 3) & ~3;
        size_t sz = (size_t)stride * h;
        if (off + sz > avail) { x_error(c, BadLength, 0); return; }
        uint32_t gid = r32(ids + 4 * i);
        glyph_t *g = glyph_slot(gs, gid, 1);
        if (!g) { off += sz; continue; }
        free(g->a8); free(g->argb);
        g->a8 = NULL; g->argb = NULL;
        g->w = w; g->h = h; g->x = rs16(gi + 4); g->y = rs16(gi + 6);
        g->xoff = rs16(gi + 8); g->yoff = rs16(gi + 10);
        const uint8_t *src = img + off;
        size_t n = (size_t)w * h;
        if (fmt == PICTFMT_ARGB32) {
            g->argb = malloc(n ? n * 4 : 4);
            if (g->argb)
                for (int y = 0; y < h; y++) memcpy(g->argb + (size_t)y * w, src + (size_t)y * stride, (size_t)w * 4);
        } else {
            g->a8 = malloc(n ? n : 1);
            if (g->a8)
                for (int y = 0; y < h; y++)
                    for (int x = 0; x < w; x++)
                        g->a8[(size_t)y * w + x] = fmt == PICTFMT_A1
                            ? (((src[(size_t)y * stride + (x >> 3)] >> (x & 7)) & 1) ? 255 : 0)
                            : src[(size_t)y * stride + x];
        }
        off += sz;
    }
}

static void do_composite_glyphs(client_t *c, const uint8_t *q, int qlen, int minor) {
    int idsz = minor == 23 ? 1 : minor == 24 ? 2 : 4;
    int op = q[4];
    picture_t *src = lookup_picture(r32(q + 8)), *dst = lookup_picture(r32(q + 12));
    glyphset_t *gs = lookup_glyphset(r32(q + 20));
    if (!src || !dst) { x_error(c, RENDER_ERROR_BASE + 1, 0); return; }
    if (!dst->d || !dst->d->px) return;
    int xsrc = rs16(q + 24), ysrc = rs16(q + 26);
    int penx = 0, peny = 0, first = 1, x0 = 0, y0 = 0;
    int off = 28;
    while (off + 8 <= qlen) {
        int count = q[off];
        if (count == 255) {
            if (off + 12 > qlen) break;
            gs = lookup_glyphset(r32(q + off + 8));
            off += 12;
            continue;
        }
        penx += rs16(q + off + 4);
        peny += rs16(q + off + 6);
        off += 8;
        if (first) { x0 = penx; y0 = peny; first = 0; }
        for (int i = 0; i < count; i++) {
            if (off + idsz > qlen) break;
            uint32_t gid = idsz == 1 ? q[off] : idsz == 2 ? r16(q + off) : r32(q + off);
            off += idsz;
            glyph_t *g = gs ? glyph_slot(gs, gid, 0) : NULL;
            if (!g) continue;
            if (g->w && g->h && (g->a8 || g->argb)) {
                comp_t k;
                memset(&k, 0, sizeof(k));
                k.op = op; k.src = src; k.dst = dst;
                k.dx = x0; k.dy = y0; k.sx = xsrc; k.sy = ysrc;
                k.amx = penx - g->x; k.amy = peny - g->y; k.amw = g->w; k.amh = g->h;
                k.a8mask = g->a8; k.argbmask = g->argb;
                composite_rect(&k, k.amx, k.amy, g->w, g->h);
            }
            penx += g->xoff;
            peny += g->yoff;
        }
        off = (off + 3) & ~3;
    }
}

void render_dispatch(client_t *c, const uint8_t *q, int qlen) {
    int minor = q[1];
    c->cur_minor = (uint8_t)minor;
    if (qlen < render_min_len(minor)) { x_error(c, BadLength, 0); return; }
    switch (minor) {
    case 0: {                                                   /* QueryVersion */
        uint8_t d[24];
        memset(d, 0, sizeof(d));
        put32(d, 0); put32(d + 4, 11);
        send_reply(c, 0, d);
        break;
    }
    case 1: query_pict_formats(c); break;
    case 2: {                                                   /* QueryPictIndexValues */
        uint8_t d[24];
        memset(d, 0, sizeof(d));
        send_reply(c, 0, d);
        break;
    }
    case 4: {                                                   /* CreatePicture */
        uint32_t did = r32(q + 8);
        drawable_t *d = did == ROOT_WINDOW ? &root->d : lookup_drawable(did);
        if (!d) { x_error(c, BadDrawable, did); return; }
        picture_t *p = picture_new(c, r32(q + 4), PK_DRAWABLE);
        if (!p) return;
        p->d = d; d->refs++;
        p->format = r32(q + 12);
        if (!format_depth(p->format)) p->format = format_for_depth(d->depth);
        if (picture_attrs(c, p, r32(q + 16), q + 20, (qlen - 20) / 4) < 0) { render_free(&p->o); return; }
        res_add(&p->o);
        break;
    }
    case 5: {                                                   /* ChangePicture */
        picture_t *p = lookup_picture(r32(q + 4));
        if (!p) { x_error(c, RENDER_ERROR_BASE + 1, r32(q + 4)); return; }
        picture_attrs(c, p, r32(q + 8), q + 12, (qlen - 12) / 4);
        break;
    }
    case 6: {                                                   /* SetPictureClipRectangles */
        picture_t *p = lookup_picture(r32(q + 4));
        if (!p) { x_error(c, RENDER_ERROR_BASE + 1, r32(q + 4)); return; }
        int n = (qlen - 12) / 8;
        xrect_t *r = malloc((size_t)(n ? n : 1) * sizeof(xrect_t));
        if (!r) return;
        for (int i = 0; i < n; i++) {
            const uint8_t *b = q + 12 + 8 * i;
            r[i].x = rs16(b); r[i].y = rs16(b + 2); r[i].w = (int)r16(b + 4); r[i].h = (int)r16(b + 6);
        }
        free(p->clip);
        p->clip = r; p->nclip = n;
        p->clip_x = rs16(q + 8); p->clip_y = rs16(q + 10);
        if (p->clip_mask) { drawable_unref(p->clip_mask); p->clip_mask = NULL; }
        break;
    }
    case 7: {                                                   /* FreePicture */
        picture_t *p = lookup_picture(r32(q + 4));
        if (!p) { x_error(c, RENDER_ERROR_BASE + 1, r32(q + 4)); return; }
        res_remove(&p->o);
        render_free(&p->o);
        break;
    }
    case 8: {                                                   /* Composite */
        comp_t k;
        memset(&k, 0, sizeof(k));
        k.op = q[4];
        k.src = lookup_picture(r32(q + 8));
        k.mask = r32(q + 12) ? lookup_picture(r32(q + 12)) : NULL;
        k.dst = lookup_picture(r32(q + 16));
        if (!k.src || !k.dst || (r32(q + 12) && !k.mask)) { x_error(c, RENDER_ERROR_BASE + 1, 0); return; }
        k.sx = rs16(q + 20); k.sy = rs16(q + 22);
        k.mx = rs16(q + 24); k.my = rs16(q + 26);
        k.dx = rs16(q + 28); k.dy = rs16(q + 30);
        composite_rect(&k, k.dx, k.dy, (int)r16(q + 32), (int)r16(q + 34));
        break;
    }
    case 10: do_trapezoids(c, q, qlen); break;
    case 11: case 12: case 13: do_triangles(c, q, qlen, minor); break;
    case 17: {                                                  /* CreateGlyphSet */
        if (!res_check_new(c, r32(q + 4))) return;
        gsref_t *r = calloc(1, sizeof(*r));
        glyphset_t *gs = calloc(1, sizeof(*gs));
        if (!r || !gs) { free(r); free(gs); x_error(c, BadAlloc, 0); return; }
        gs->refs = 1;
        gs->format = r32(q + 8);
        r->o.id = r32(q + 4); r->o.type = XT_GLYPHSET; r->o.owner = (int8_t)c->index;
        r->gs = gs;
        res_add(&r->o);
        break;
    }
    case 18: {                                                  /* ReferenceGlyphSet */
        glyphset_t *gs = lookup_glyphset(r32(q + 8));
        if (!gs) { x_error(c, RENDER_ERROR_BASE + 3, r32(q + 8)); return; }
        if (!res_check_new(c, r32(q + 4))) return;
        gsref_t *r = calloc(1, sizeof(*r));
        if (!r) return;
        r->o.id = r32(q + 4); r->o.type = XT_GLYPHSET; r->o.owner = (int8_t)c->index;
        r->gs = gs; gs->refs++;
        res_add(&r->o);
        break;
    }
    case 19: {                                                  /* FreeGlyphSet */
        xobj_t *o = res_lookup(r32(q + 4));
        if (!o || o->type != XT_GLYPHSET) { x_error(c, RENDER_ERROR_BASE + 3, r32(q + 4)); return; }
        res_remove(o);
        render_free(o);
        break;
    }
    case 20: do_add_glyphs(c, q, qlen); break;
    case 21: break;                                             /* AddGlyphsFromPicture */
    case 22: {                                                  /* FreeGlyphs */
        glyphset_t *gs = lookup_glyphset(r32(q + 4));
        if (!gs) { x_error(c, RENDER_ERROR_BASE + 3, r32(q + 4)); return; }
        for (int off = 8; off + 4 <= qlen; off += 4) glyph_remove(gs, r32(q + off));
        break;
    }
    case 23: case 24: case 25: do_composite_glyphs(c, q, qlen, minor); break;
    case 26: {                                                  /* FillRectangles */
        picture_t *dp = lookup_picture(r32(q + 8));
        if (!dp) { x_error(c, RENDER_ERROR_BASE + 1, r32(q + 8)); return; }
        picture_t solid;
        memset(&solid, 0, sizeof(solid));
        solid.kind = PK_SOLID;
        solid.solid = ((r16(q + 18) >> 8) << 24) | ((r16(q + 12) >> 8) << 16) |
                      ((r16(q + 14) >> 8) << 8) | (r16(q + 16) >> 8);
        comp_t k;
        memset(&k, 0, sizeof(k));
        k.op = q[4]; k.src = &solid; k.dst = dp;
        int n = (qlen - 20) / 8;
        for (int i = 0; i < n; i++) {
            const uint8_t *r = q + 20 + 8 * i;
            composite_rect(&k, rs16(r), rs16(r + 2), (int)r16(r + 4), (int)r16(r + 6));
        }
        break;
    }
    case 27: case 31: {                                         /* CreateCursor / CreateAnimCursor */
        if (!res_check_new(c, r32(q + 4))) return;
        xobj_t *o = calloc(1, sizeof(*o));
        if (o) { o->id = r32(q + 4); o->type = XT_CURSOR; o->owner = (int8_t)c->index; res_add(o); }
        break;
    }
    case 28: {                                                  /* SetPictureTransform */
        picture_t *p = lookup_picture(r32(q + 4));
        if (!p) { x_error(c, RENDER_ERROR_BASE + 1, r32(q + 4)); return; }
        for (int i = 0; i < 9; i++) p->xf[i] = fx2d(r32(q + 8 + 4 * i));
        p->has_xform = !(p->xf[0] == 1 && p->xf[1] == 0 && p->xf[2] == 0 && p->xf[3] == 0 &&
                         p->xf[4] == 1 && p->xf[5] == 0 && p->xf[6] == 0 && p->xf[7] == 0 &&
                         p->xf[8] == 1);
        break;
    }
    case 29: query_filters(c); break;
    case 30: {                                                  /* SetPictureFilter */
        picture_t *p = lookup_picture(r32(q + 4));
        if (!p) { x_error(c, RENDER_ERROR_BASE + 1, r32(q + 4)); return; }
        int n = (int)r16(q + 8);
        if (12 + n > qlen) { x_error(c, BadLength, 0); return; }
        p->filter = !((n == 7 && !memcmp(q + 12, "nearest", 7)) || (n == 4 && !memcmp(q + 12, "fast", 4)));
        break;
    }
    case 32: do_add_traps(c, q, qlen); break;
    case 33: {                                                  /* CreateSolidFill */
        picture_t *p = picture_new(c, r32(q + 4), PK_SOLID);
        if (!p) return;
        p->format = PICTFMT_ARGB32;
        p->solid = ((r16(q + 14) >> 8) << 24) | ((r16(q + 8) >> 8) << 16) |
                   ((r16(q + 10) >> 8) << 8) | (r16(q + 12) >> 8);
        res_add(&p->o);
        break;
    }
    case 34: case 35: case 36: {                                /* gradients */
        picture_t *p = picture_new(c, r32(q + 4), minor == 34 ? PK_LINEAR : minor == 35 ? PK_RADIAL : PK_CONICAL);
        if (!p) return;
        p->format = PICTFMT_ARGB32;
        int off, n;
        if (minor == 34) {
            p->x1 = fx2d(r32(q + 8)); p->y1 = fx2d(r32(q + 12));
            p->x2 = fx2d(r32(q + 16)); p->y2 = fx2d(r32(q + 20));
            n = (int)r32(q + 24); off = 28;
        } else if (minor == 35) {
            p->x1 = fx2d(r32(q + 8)); p->y1 = fx2d(r32(q + 12));
            p->x2 = fx2d(r32(q + 16)); p->y2 = fx2d(r32(q + 20));
            p->r1 = fx2d(r32(q + 24)); p->r2 = fx2d(r32(q + 28));
            n = (int)r32(q + 32); off = 36;
        } else {
            p->x1 = fx2d(r32(q + 8)); p->y1 = fx2d(r32(q + 12));
            p->angle = fx2d(r32(q + 16));
            n = (int)r32(q + 20); off = 24;
        }
        if (read_stops(p, q, qlen, off, n) < 0) { render_free(&p->o); x_error(c, BadLength, 0); return; }
        res_add(&p->o);
        break;
    }
    default: {
        static unsigned seen[2];
        if (minor < 64 && !(seen[minor >> 5] & (1u << (minor & 31)))) {
            seen[minor >> 5] |= 1u << (minor & 31);
            xlog("unimplemented RENDER request minor=%d len=%d\n", minor, qlen);
        }
        x_error(c, BadRequest, 0);
        break;
    }
    }
}
