/*
 * maeroX — a minimal X11 server for MaeroOS (Firefox road, Phase 33).
 *
 * A native libgui application: owns a desktop window + composited surface,
 * listens on AF_UNIX /tmp/.X11-unix/X0, and speaks the X11 wire protocol to
 * clients (raw probes now; libX11/GTK/Firefox later).
 *
 *   33a — connection-setup handshake (done).
 *   33b — request loop: CreateWindow/GC, MapWindow, PolyFillRectangle, PutImage
 *         + the startup queries Xlib emits; mapped windows composite into the
 *         desktop surface.
 */
#include <draw.h>
#include <fcntl.h>
#include <gui.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

#define X_SOCKET_PATH "/tmp/.X11-unix/X0"
#define MAX_XCLIENTS  8
#define MAX_RES       128
/* Big enough for the LARGEST request the setup reply allows: we advertise
 * maximum-request-length = 65535 (units of 4 bytes), so Xlib chunks a big
 * PutImage into pieces of up to 65535*4 = 262140 bytes and expects the server
 * to take them.  With a 64 KiB buffer any request over 64 KiB wedged the
 * connection: process_client() would call read() with zero space left, read()
 * returns 0, and 0 is our "client closed" signal — so the first full-window
 * PutImage (818*531*4 = 1.7 MB, chunked to 256 KiB pieces) disconnected
 * Firefox instead of painting. */
#define INBUF_SIZE    270336   /* 264 KiB >= 65535 * 4 */

#define ROOT_WINDOW   0x00000001u
#define ROOT_COLORMAP 0x00000020u
#define ROOT_VISUAL   0x00000021u

/* X11 resource kinds. */
enum { R_NONE = 0, R_WINDOW, R_GC, R_PIXMAP, R_PICTURE, R_GLYPHSET };

typedef struct {
    uint32_t xid;
    int      kind;
    /* window / pixmap */
    int      x, y, w, h;
    uint32_t *px;          /* backing pixels (w*h), NULL for GC */
    int      mapped;
    int      maximized;    /* kiosk WM: main toplevel resized to fill the screen */
    /* gc */
    uint32_t fg, bg;
    /* picture (XRender): wraps a drawable + format, or a solid colour source */
    uint32_t pic_drawable; /* the window/pixmap this picture renders to/from */
    uint32_t pic_format;   /* PICTFORMAT id */
    int      pic_solid;    /* 1 = 1x1 solid colour source (colour in fg) */
    /* glyphset (XRender text): A8 coverage bitmaps keyed by glyph id */
    void    *gset;         /* glyphset_t* for R_GLYPHSET */
} xres_t;

/* XRender glyph storage: each glyph is an A8 coverage bitmap + metrics. */
typedef struct {
    uint32_t id;
    int      w, h, x, y, xoff, yoff;
    uint8_t *bits;         /* w*h A8 coverage (we store unpadded) */
} xglyph_t;
typedef struct { xglyph_t *g; int n, cap; } glyphset_t;

/* XRender extension: we advertise it under this major opcode (clients use the
 * value we return from QueryExtension, so any value > 127 works). */
#define RENDER_MAJOR  139
#define RENDER_ERROR_BASE 142
/* PICTFORMAT ids we advertise. */
#define PICTFMT_RGB24  0x30
#define PICTFMT_ARGB32 0x31
#define PICTFMT_A8     0x33
#define PICTFMT_A1     0x34

typedef struct {
    int      used;
    int      fd;
    int      setup_done;
    uint8_t  inbuf[INBUF_SIZE];
    int      inlen;
    uint16_t seq;
    xres_t   res[MAX_RES];
    int      nres;
} xclient_t;

static gui_window_t gui;
static int       headless;
static int       xdbg;
static int       dumpmode;       /* -D: composite off-screen + dump painted frame
                                  * over /dev/tty so the headless (-kernel, no
                                  * framebuffer) path is visually capturable. */
static int       dumps_done;
static int       listen_fd = -1;
static xclient_t clients[MAX_XCLIENTS];
static int       dirty = 1;

/* ── A0 diagnostic trace ─────────────────────────────────────────────────────
 * Firefox's X requests are the key to why nothing paints, but maeroX's stdout
 * goes to the GUI console (not the host serial).  Route a compact trace to
 * /dev/tty, which the kernel forwards to COM1 → the host serial log, so the
 * request profile is readable from the host.  Enabled with -T. */
static int      trace_fd = -1;
static unsigned op_hist[256];
static unsigned putimage_n, copyarea_n, render_n;
static uint8_t  op_ring[32];     /* last opcodes in order (stall diag) */
static unsigned op_ring_n;
static void xt(const char *fmt, ...) {
    if (trace_fd < 0) return;
    char buf[256];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) write(trace_fd, buf, n > (int)sizeof(buf) ? (int)sizeof(buf) : n);
}
static void xt_dump_hist(void) {
    if (trace_fd < 0) return;
    xt("XT hist: ");
    for (int i = 0; i < 256; i++)
        if (op_hist[i]) xt("op%d=%u ", i, op_hist[i]);
    xt("| putimg=%u copy=%u render=%u\n", putimage_n, copyarea_n, render_n);
    xt("XT lastops: ");
    int n = op_ring_n < 32 ? (int)op_ring_n : 32;
    int base = op_ring_n < 32 ? 0 : (int)(op_ring_n & 31);
    for (int i = 0; i < n; i++) xt("%d ", op_ring[(base + i) & 31]);
    xt("\n");
}

/* ── Atom registry (server-global) ───────────────────────────────────────────
 * X11 atoms are server-global names with stable integer ids.  GDK interns the
 * same name repeatedly and REQUIRES the same id back each time, and round-trips
 * via GetAtomName — the old "return a fresh counter every call" stub gave a new
 * id per InternAtom, corrupting GDK's atom cache.  Dynamic ids start at 100 to
 * stay clear of the predefined atom range (1..68) that Xlib uses by value. */
#define MAX_ATOMS 256
static struct { char name[64]; } atoms[MAX_ATOMS];
static int next_atom = 100;

/* Intern `name` (len chars): return its id, allocating one if new.  If
 * only_if_exists and the name is unknown, return 0 (None). */
static uint32_t atom_intern(const char *name, int len, int only_if_exists) {
    if (len <= 0 || len > 63) return 0;
    for (int i = 1; i < next_atom && i < MAX_ATOMS; i++)
        if (atoms[i].name[0] &&
            (int)strlen(atoms[i].name) == len &&
            memcmp(atoms[i].name, name, (size_t)len) == 0)
            return (uint32_t)i;
    if (only_if_exists || next_atom >= MAX_ATOMS) return 0;
    int id = next_atom++;
    memcpy(atoms[id].name, name, (size_t)len);
    atoms[id].name[len] = '\0';
    return (uint32_t)id;
}
static const char *atom_name(uint32_t id, int *len) {
    if (id >= 1 && id < (uint32_t)next_atom && id < MAX_ATOMS && atoms[id].name[0]) {
        *len = (int)strlen(atoms[id].name);
        return atoms[id].name;
    }
    *len = 0;
    return "";
}
static char      status[96] = "maeroX :0 - listening, 0 clients";

static void sleep_ms(long ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, 0);
}

/* ── little-endian readers (clients are LSBFirst on x86) ─────────────────── */
static uint32_t r16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static int      rs16(const uint8_t *p) { return (int)(int16_t)(p[0] | (p[1] << 8)); }
static uint32_t r32(const uint8_t *p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ── little-endian byte appender (setup reply) ───────────────────────────── */
typedef struct { uint8_t *p; int n, cap; } buf_t;
static void b8(buf_t *b, uint32_t v)  { if (b->n < b->cap) b->p[b->n] = (uint8_t)v; b->n++; }
static void b16(buf_t *b, uint32_t v) { b8(b, v); b8(b, v >> 8); }
static void b32(buf_t *b, uint32_t v) { b8(b, v); b8(b, v >> 8); b8(b, v >> 16); b8(b, v >> 24); }
static void bpad(buf_t *b, int n)     { while (n-- > 0) b8(b, 0); }

static void write_all(int fd, const uint8_t *p, int n) {
    int off = 0;
    while (off < n) {
        int w = write(fd, p + off, n - off);
        if (w > 0) off += w; else sleep_ms(1);
    }
}

/* ── resources ───────────────────────────────────────────────────────────── */
static xres_t *res_find(xclient_t *c, uint32_t xid) {
    if (xid == ROOT_WINDOW) return NULL;        /* root is implicit */
    for (int i = 0; i < c->nres; i++)
        if (c->res[i].kind && c->res[i].xid == xid) return &c->res[i];
    return NULL;
}

static xres_t *res_new(xclient_t *c, uint32_t xid, int kind) {
    xres_t *r = res_find(c, xid);
    if (!r) {
        for (int i = 0; i < MAX_RES; i++)
            if (!c->res[i].kind) { r = &c->res[i]; if (i >= c->nres) c->nres = i + 1; break; }
    }
    if (!r) return NULL;
    memset(r, 0, sizeof(*r));
    r->xid = xid;
    r->kind = kind;
    return r;
}

/* ── the X11 connection-setup success reply ──────────────────────────────── */
static int build_setup_reply(uint8_t *out, int cap, int scr_w, int scr_h) {
    buf_t b = { out, 0, cap };
    const char *vendor = "MaeroX";
    int vlen = (int)strlen(vendor);
    int vpad = (4 - (vlen & 3)) & 3;
    int extra = 32 + (vlen + vpad) + 2 * 8 + (40 + 8 + 24);

    b8(&b, 1); b8(&b, 0); b16(&b, 11); b16(&b, 0); b16(&b, extra / 4);

    b32(&b, 1);                /* release */
    b32(&b, 0x00200000);       /* resource-id-base */
    b32(&b, 0x001FFFFF);       /* resource-id-mask */
    b32(&b, 0);                /* motion-buffer-size */
    b16(&b, vlen);             /* vendor length */
    b16(&b, 65535);            /* maximum-request-length */
    b8(&b, 1);                 /* screens */
    b8(&b, 2);                 /* pixmap formats */
    b8(&b, 0); b8(&b, 0);      /* image-byte-order LSB, bit-order LSB */
    b8(&b, 32); b8(&b, 32);    /* scanline unit / pad */
    b8(&b, 8); b8(&b, 255);    /* min/max keycode */
    bpad(&b, 4);

    for (int i = 0; i < vlen; i++) b8(&b, (uint8_t)vendor[i]);
    bpad(&b, vpad);

    b8(&b, 1);  b8(&b, 1);  b8(&b, 32); bpad(&b, 5);    /* FORMAT depth 1 */
    b8(&b, 24); b8(&b, 32); b8(&b, 32); bpad(&b, 5);    /* FORMAT depth 24 */

    b32(&b, ROOT_WINDOW); b32(&b, ROOT_COLORMAP);
    b32(&b, 0x00FFFFFF); b32(&b, 0x00000000);           /* white / black */
    b32(&b, 0);                                         /* input-masks */
    b16(&b, scr_w); b16(&b, scr_h);
    b16(&b, scr_w * 264 / 1000); b16(&b, scr_h * 264 / 1000);
    b16(&b, 1); b16(&b, 1);
    b32(&b, ROOT_VISUAL);
    b8(&b, 0); b8(&b, 0); b8(&b, 24); b8(&b, 1);        /* backing/saveunder/depth/ndepths */

    b8(&b, 24); b8(&b, 0); b16(&b, 1); bpad(&b, 4);     /* DEPTH 24, 1 visual */
    b32(&b, ROOT_VISUAL); b8(&b, 4); b8(&b, 8); b16(&b, 256);
    b32(&b, 0x00FF0000); b32(&b, 0x0000FF00); b32(&b, 0x000000FF); bpad(&b, 4);

    return b.n;
}

/* Build + send a 32-byte reply with `data` (24 bytes after the 8-byte head). */
static void put32(uint8_t *p, uint32_t v) {
    p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}
static void put16(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; }

static void send_reply(xclient_t *c, uint8_t b1, const uint8_t data24[24]) {
    uint8_t r[32];
    memset(r, 0, sizeof(r));
    r[0] = 1;                  /* reply */
    r[1] = b1;
    r[2] = c->seq & 0xFF;
    r[3] = (c->seq >> 8) & 0xFF;
    /* r[4..7] reply-length = 0 (no extra) */
    if (data24) memcpy(r + 8, data24, 24);
    write_all(c->fd, r, 32);
    if (xdbg) printf("maerox: reply seq=%d b1=%d sent\n", c->seq, b1);
}

/* Variable reply: 32-byte head (with reply-length = extra_words) + extra data. */
static void send_reply_var(xclient_t *c, uint8_t b1, const uint8_t data24[24],
                           const uint8_t *extra, int extra_len) {
    uint8_t r[32];
    memset(r, 0, sizeof(r));
    r[0] = 1; r[1] = b1;
    r[2] = c->seq & 0xFF; r[3] = (c->seq >> 8) & 0xFF;
    int words = (extra_len + 3) / 4;
    put32(r + 4, (uint32_t)words);
    if (data24) memcpy(r + 8, data24, 24);
    write_all(c->fd, r, 32);
    if (extra_len > 0) {
        write_all(c->fd, extra, extra_len);
        int pad = words * 4 - extra_len;
        if (pad > 0) { uint8_t z[4] = {0,0,0,0}; write_all(c->fd, z, pad); }
    }
}

/* Core X requests that generate a reply — the client blocks until it arrives,
 * so maeroX must answer every one of these even if only with empty data. */
static int req_expects_reply(int op) {
    switch (op) {
    case 3: case 14: case 15: case 16: case 17: case 20: case 21:
    case 23: case 26: case 31: case 38: case 39: case 40: case 43:
    case 44: case 47: case 48: case 49: case 50: case 52: case 73:
    case 83: case 84: case 85: case 91: case 92: case 97: case 98:
    case 99: case 101: case 103: case 106: case 108: case 110:
    case 116: case 117: case 119:
        return 1;
    default: return 0;
    }
}

/* ── X11 events (32 bytes; byte 0 = type, byte 1 = detail) ───────────────── */

/* Expose: tell the client (a region of) its window needs repainting. */
static void send_expose(xclient_t *c, xres_t *w) {
    uint8_t e[32];
    memset(e, 0, sizeof(e));
    e[0] = 12;                          /* Expose */
    put16(e + 2, c->seq);
    put32(e + 4, w->xid);               /* window */
    put16(e + 8, 0); put16(e + 10, 0);  /* x, y */
    put16(e + 12, w->w); put16(e + 14, w->h);
    put16(e + 16, 0);                   /* count */
    write_all(c->fd, e, 32);
}

/* MapNotify: tell the client its window is now mapped/viewable.  GDK keeps the
 * window in an unmapped state — and never paints — until it sees this. */
static void send_map_notify(xclient_t *c, xres_t *w) {
    uint8_t e[32];
    memset(e, 0, sizeof(e));
    e[0] = 19;                          /* MapNotify */
    put16(e + 2, c->seq);
    put32(e + 4, w->xid);               /* event window */
    put32(e + 8, w->xid);               /* window */
    e[12] = 0;                          /* override-redirect = False */
    write_all(c->fd, e, 32);
}

/* ConfigureNotify: report the window's geometry after mapping. */
static void send_configure(xclient_t *c, xres_t *w) {
    uint8_t e[32];
    memset(e, 0, sizeof(e));
    e[0] = 22;                          /* ConfigureNotify */
    put16(e + 2, c->seq);
    put32(e + 4, w->xid);               /* event */
    put32(e + 8, w->xid);               /* window */
    put32(e + 12, 0);                   /* above-sibling = None */
    put16(e + 16, w->x); put16(e + 18, w->y);
    put16(e + 20, w->w); put16(e + 22, w->h);
    write_all(c->fd, e, 32);
}

/* X TIMESTAMPs are milliseconds since server start.  Zero is reserved
 * (CurrentTime), and GDK compares the value it gets back from
 * gdk_x11_get_server_time() against its own monotonic clock, so hand out a real
 * monotonically increasing millisecond count. */
static uint32_t x_time(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 1;
    uint32_t ms = (uint32_t)ts.tv_sec * 1000u + (uint32_t)(ts.tv_nsec / 1000000);
    return ms ? ms : 1;
}

/* PropertyNotify: a property on a window changed (state 0) or was deleted
 * (state 1).
 *
 * This event is what unblocks gdk_x11_get_server_time() (GTK 3.24,
 * gdk/x11/gdkwindow-x11.c:5624-5648): it writes a one-byte GDK_TIMESTAMP_PROP
 * property and then sits in XIfEvent() until a PropertyNotify for that window
 * and atom arrives — "The window must have GDK_PROPERTY_CHANGE_MASK in its
 * events mask or a hang will result", says its own doc comment.  Without this
 * event the Firefox main thread parked in that XIfEvent forever, which is why
 * the browser window was never shown.
 *
 * Like every other event maeroX sends (Expose, MapNotify, ConfigureNotify) this
 * ignores the window's event mask, which maeroX does not track. */
static void send_property_notify(xclient_t *c, uint32_t window, uint32_t atom,
                                 int deleted) {
    uint8_t e[32];
    memset(e, 0, sizeof(e));
    e[0] = 28;                          /* PropertyNotify */
    put16(e + 2, c->seq);
    put32(e + 4, window);
    put32(e + 8, atom);
    put32(e + 12, x_time());            /* time */
    e[16] = (uint8_t)(deleted ? 1 : 0); /* state: 0 NewValue, 1 Deleted */
    write_all(c->fd, e, 32);
}

/* A pointer event (ButtonPress/Release/Motion) relative to a window. */
static void send_pointer(xclient_t *c, xres_t *w, int type, int detail,
                         int ex, int ey) {
    uint8_t e[32];
    memset(e, 0, sizeof(e));
    e[0] = type;
    e[1] = detail;
    put16(e + 2, c->seq);
    put32(e + 4, x_time());             /* time */
    put32(e + 8, 0x00000001);           /* root window */
    put32(e + 12, w->xid);              /* event window */
    put32(e + 16, 0);                   /* child = None */
    put16(e + 20, w->x + ex); put16(e + 22, w->y + ey);  /* root-x/y */
    put16(e + 24, ex); put16(e + 26, ey);                /* event-x/y */
    put16(e + 28, 0);                  /* state */
    e[30] = 1;                          /* same-screen */
    write_all(c->fd, e, 32);
}

/* ── drawing into a drawable's backing buffer ────────────────────────────── */
static void fill_rect(xres_t *d, int x, int y, int w, int h, uint32_t color) {
    if (!d || !d->px) return;
    for (int yy = y; yy < y + h; yy++) {
        if (yy < 0 || yy >= d->h) continue;
        for (int xx = x; xx < x + w; xx++) {
            if (xx < 0 || xx >= d->w) continue;
            d->px[(size_t)yy * d->w + xx] = color;
        }
    }
}

/* ── XRender ─────────────────────────────────────────────────────────────────
 * Modern GTK3/cairo render EVERYTHING through XRender: a window's cairo surface
 * is an XRender Picture, and drawing is RenderComposite / RenderFillRectangles /
 * glyph compositing.  Without it cairo can't create a renderable surface and the
 * window never paints.  We implement the subset cairo actually issues. */

/* Resolve the drawable (window/pixmap) a picture renders into. */
static xres_t *pic_target(xclient_t *c, uint32_t picid) {
    xres_t *p = res_find(c, picid);
    if (!p || p->kind != R_PICTURE || p->pic_solid) return NULL;
    return res_find(c, p->pic_drawable);
}

/* Premultiplied "Over": dst = src + dst*(1-alpha).  src is ARGB premultiplied. */
static uint32_t blend_over(uint32_t s, uint32_t d) {
    uint32_t a = (s >> 24) & 0xff;
    if (a == 0xff) return s & 0x00FFFFFF;
    if (a == 0)    return d & 0x00FFFFFF;
    uint32_t ia = 255 - a;
    uint32_t sr = (s >> 16) & 0xff, sg = (s >> 8) & 0xff, sb = s & 0xff;
    uint32_t dr = (d >> 16) & 0xff, dg = (d >> 8) & 0xff, db = d & 0xff;
    uint32_t rr = sr + dr * ia / 255; if (rr > 255) rr = 255;
    uint32_t rg = sg + dg * ia / 255; if (rg > 255) rg = 255;
    uint32_t rb = sb + db * ia / 255; if (rb > 255) rb = 255;
    return (rr << 16) | (rg << 8) | rb;
}

/* Append a PICTFORMINFO (28 bytes) to a buffer for QueryPictFormats. */
static int put_pictform(uint8_t *b, uint32_t id, int depth,
                        int rs, int rm, int gs, int gm,
                        int bs, int bm, int as, int am) {
    memset(b, 0, 28);
    put32(b + 0, id); b[4] = 1 /*Direct*/; b[5] = (uint8_t)depth;
    put16(b + 8, rs);  put16(b + 10, rm);
    put16(b + 12, gs); put16(b + 14, gm);
    put16(b + 16, bs); put16(b + 18, bm);
    put16(b + 20, as); put16(b + 22, am);
    return 28;
}

/* ── XRender glyph storage (text) ───────────────────────────────────────── */
static glyphset_t *gset_of(xres_t *r) {
    if (!r || r->kind != R_GLYPHSET) return NULL;
    if (!r->gset) r->gset = calloc(1, sizeof(glyphset_t));
    return (glyphset_t *)r->gset;
}
static xglyph_t *glyph_find(glyphset_t *gs, uint32_t id) {
    if (!gs) return NULL;
    for (int i = 0; i < gs->n; i++) if (gs->g[i].id == id) return &gs->g[i];
    return NULL;
}
/* Blit one A8 glyph (coverage) in colour `col` onto drawable dd at pen (px,py). */
static void glyph_blit(xres_t *dd, xglyph_t *g, int px, int py, uint32_t col) {
    if (!g || !g->bits || !dd || !dd->px) return;
    int ox = px - g->x, oy = py - g->y;
    uint32_t cr = (col >> 16) & 0xff, cg = (col >> 8) & 0xff, cb = col & 0xff;
    for (int yy = 0; yy < g->h; yy++) {
        int ty = oy + yy; if (ty < 0 || ty >= dd->h) continue;
        for (int xx = 0; xx < g->w; xx++) {
            int tx = ox + xx; if (tx < 0 || tx >= dd->w) continue;
            int cov = g->bits[yy * g->w + xx];
            if (!cov) continue;
            /* premultiplied src for blend_over */
            uint32_t s = ((uint32_t)cov << 24) | ((cr * cov / 255) << 16) |
                         ((cg * cov / 255) << 8) | (cb * cov / 255);
            uint32_t *dp = &dd->px[(size_t)ty * dd->w + tx];
            *dp = blend_over(s, *dp);
        }
    }
}

static void dispatch_render(xclient_t *c, const uint8_t *q, int qlen) {
    int minor = q[1];
    switch (minor) {
    case 0: {        /* QueryVersion */
        uint8_t data[24]; memset(data, 0, sizeof(data));
        put32(data + 0, 0);   /* major */
        put32(data + 4, 11);  /* minor */
        send_reply(c, 0, data);
        break;
    }
    case 1: {        /* QueryPictFormats — cairo maps visuals→formats from this */
        uint8_t extra[256]; int n = 0;
        n += put_pictform(extra + n, PICTFMT_RGB24, 24, 16,0xff, 8,0xff, 0,0xff, 0,0x00);
        n += put_pictform(extra + n, PICTFMT_ARGB32,32, 16,0xff, 8,0xff, 0,0xff, 24,0xff);
        n += put_pictform(extra + n, PICTFMT_A8,     8,  0,0,    0,0,    0,0,    0,0xff);
        n += put_pictform(extra + n, PICTFMT_A1,     1,  0,0,    0,0,    0,0,    0,0x01);
        /* one screen */
        put32(extra + n, 1); n += 4;               /* numDepths in this screen */
        put32(extra + n, PICTFMT_RGB24); n += 4;   /* fallback format */
        extra[n] = 24; extra[n+1] = 0; put16(extra + n + 2, 1);    /* PICTDEPTH d=24 */
        put32(extra + n + 4, 0); n += 8;
        put32(extra + n, ROOT_VISUAL);   n += 4;   /* PICTVISUAL: visual→format */
        put32(extra + n, PICTFMT_RGB24); n += 4;
        put32(extra + n, 0); n += 4;               /* 1 subpixel order = Unknown */
        uint8_t data[24]; memset(data, 0, sizeof(data));
        put32(data + 0, 4);   /* numFormats */
        put32(data + 4, 1);   /* numScreens */
        put32(data + 8, 1);   /* numDepths  */
        put32(data + 12, 1);  /* numVisuals */
        put32(data + 16, 1);  /* numSubpixels */
        send_reply_var(c, 0, data, extra, n);
        break;
    }
    case 4: {        /* CreatePicture(pid, drawable, format, mask, values...) */
        xres_t *p = res_new(c, r32(q + 4), R_PICTURE);
        if (p) { p->pic_drawable = r32(q + 8); p->pic_format = r32(q + 12);
                 p->pic_solid = 0; }
        break;
    }
    case 5: break;   /* ChangePicture — accept (we ignore most attributes) */
    case 6: break;   /* SetPictureClipRectangles — accept (no clip tracking) */
    case 7: {        /* FreePicture */
        xres_t *p = res_find(c, r32(q + 4));
        if (p && p->kind == R_PICTURE) p->kind = R_NONE;
        break;
    }
    case 8: {        /* Composite(op, src, mask, dst, sx,sy, mx,my, dx,dy, w,h) */
        int op = q[4];
        xres_t *srcp = res_find(c, r32(q + 8));
        xres_t *dstp = res_find(c, r32(q + 16));
        int sx = rs16(q + 20), sy = rs16(q + 22);
        int dx = rs16(q + 28), dy = rs16(q + 30);
        int w  = (int)r16(q + 32), h = (int)r16(q + 34);
        if (!dstp || dstp->kind != R_PICTURE) break;
        xres_t *dd = res_find(c, dstp->pic_drawable);
        if (!dd || !dd->px) break;
        int    solid = (srcp && srcp->kind == R_PICTURE && srcp->pic_solid);
        uint32_t sc  = solid ? srcp->fg : 0;
        xres_t *sd   = (!solid && srcp && srcp->kind == R_PICTURE)
                     ? res_find(c, srcp->pic_drawable) : NULL;
        int has_alpha = solid || (srcp && srcp->pic_format == PICTFMT_ARGB32);
        for (int yy = 0; yy < h; yy++) {
            int ty = dy + yy; if (ty < 0 || ty >= dd->h) continue;
            for (int xx = 0; xx < w; xx++) {
                int tx = dx + xx; if (tx < 0 || tx >= dd->w) continue;
                uint32_t sp;
                if (solid) sp = sc;
                else if (sd && sd->px) {
                    int ux = sx + xx, uy = sy + yy;
                    if (ux < 0 || ux >= sd->w || uy < 0 || uy >= sd->h) continue;
                    sp = sd->px[(size_t)uy * sd->w + ux];
                } else continue;
                uint32_t *dp = &dd->px[(size_t)ty * dd->w + tx];
                if (op == 1 || !has_alpha) *dp = sp & 0x00FFFFFF;   /* Src */
                else                        *dp = blend_over(sp, *dp); /* Over */
            }
        }
        render_n++;
        if (render_n <= 8) xt("XT Composite op=%d dst-win=0x%x %dx%d @%d,%d\n",
                              op, (unsigned)dstp->pic_drawable, w, h, dx, dy);
        if (dd->kind == R_WINDOW) dirty = 1;
        break;
    }
    case 26: {       /* FillRectangles(op, dst, color[4xCARD16], rects...) */
        int op = q[4];
        xres_t *dstp = res_find(c, r32(q + 8));
        uint32_t cr = r16(q + 12) >> 8, cg = r16(q + 14) >> 8,
                 cb = r16(q + 16) >> 8, ca = r16(q + 18) >> 8;
        uint32_t color = (ca << 24) | (cr << 16) | (cg << 8) | cb;
        if (!dstp || dstp->kind != R_PICTURE) break;
        xres_t *dd = res_find(c, dstp->pic_drawable);
        if (!dd || !dd->px) break;
        int nr = (qlen - 20) / 8;
        for (int i = 0; i < nr; i++) {
            const uint8_t *rr = q + 20 + i * 8;
            int x = rs16(rr), y = rs16(rr + 2);
            int rw = (int)r16(rr + 4), rh = (int)r16(rr + 6);
            for (int yy = 0; yy < rh; yy++) {
                int ty = y + yy; if (ty < 0 || ty >= dd->h) continue;
                for (int xx = 0; xx < rw; xx++) {
                    int tx = x + xx; if (tx < 0 || tx >= dd->w) continue;
                    uint32_t *dp = &dd->px[(size_t)ty * dd->w + tx];
                    if (op == 1 || ca == 255) *dp = color & 0x00FFFFFF;
                    else if (ca > 0)          *dp = blend_over(color, *dp);
                }
            }
        }
        if (dd->kind == R_WINDOW) dirty = 1;
        break;
    }
    case 33: {       /* CreateSolidFill(pid, color[4xCARD16]) */
        xres_t *p = res_new(c, r32(q + 4), R_PICTURE);
        if (p) {
            uint32_t cr = r16(q + 8) >> 8, cg = r16(q + 10) >> 8,
                     cb = r16(q + 12) >> 8, ca = r16(q + 14) >> 8;
            p->pic_solid = 1; p->pic_format = PICTFMT_ARGB32;
            p->fg = (ca << 24) | (cr << 16) | (cg << 8) | cb;
        }
        break;
    }
    case 17: {       /* CreateGlyphSet — track as a resource so FreeGlyphSet works */
        xres_t *g = res_new(c, r32(q + 4), R_GLYPHSET); (void)g;
        break;
    }
    case 18: case 19: break;   /* Reference/FreeGlyphSet variants — accept */
    case 20: {       /* AddGlyphs(glyphset, nglyphs, ids[], infos[], A8 images) */
        xres_t *gr = res_find(c, r32(q + 4));
        glyphset_t *gs = gset_of(gr);
        if (!gs) break;
        uint32_t ng = r32(q + 8);
        if (ng > 8192) break;
        const uint8_t *ids   = q + 12;
        const uint8_t *infos = ids + (size_t)ng * 4;
        const uint8_t *img   = infos + (size_t)ng * 12;
        size_t imgoff = 0;
        for (uint32_t i = 0; i < ng; i++) {
            const uint8_t *gi = infos + (size_t)i * 12;
            int gw = (int)r16(gi),     gh = (int)r16(gi + 2);
            int gx = rs16(gi + 4),     gy = rs16(gi + 6);
            int xo = rs16(gi + 8),     yo = rs16(gi + 10);
            if (gw < 0 || gh < 0 || gw > 1024 || gh > 1024) break;
            int stride = (gw + 3) & ~3;                 /* A8 scanline pad 4 */
            if ((size_t)((img - q) + imgoff + (size_t)stride * gh) > (size_t)qlen) break;
            uint8_t *bits = (uint8_t *)malloc((size_t)(gw ? gw : 1) * (gh ? gh : 1));
            if (bits)
                for (int yy = 0; yy < gh; yy++)
                    for (int xx = 0; xx < gw; xx++)
                        bits[yy * gw + xx] = img[imgoff + (size_t)yy * stride + xx];
            if (gs->n >= gs->cap) {
                gs->cap = gs->cap ? gs->cap * 2 : 128;
                gs->g = (xglyph_t *)realloc(gs->g, (size_t)gs->cap * sizeof(xglyph_t));
            }
            if (gs->g) {
                xglyph_t *gg = &gs->g[gs->n++];
                gg->id = r32(ids + (size_t)i * 4);
                gg->w = gw; gg->h = gh; gg->x = gx; gg->y = gy;
                gg->xoff = xo; gg->yoff = yo; gg->bits = bits;
            }
            imgoff += (size_t)stride * gh;
        }
        break;
    }
    case 21: case 22: break;   /* FreeGlyphs — accept (we don't reclaim) */
    case 23: case 24: case 25: {   /* CompositeGlyphs 8/16/32 — render text */
        int idsz = (minor == 23) ? 1 : (minor == 24) ? 2 : 4;
        xres_t *srcp = res_find(c, r32(q + 8));
        xres_t *dstp = res_find(c, r32(q + 12));
        xres_t *gr   = res_find(c, r32(q + 20));
        glyphset_t *gs = gset_of(gr);
        if (!dstp || dstp->kind != R_PICTURE) break;
        xres_t *dd = res_find(c, dstp->pic_drawable);
        if (!dd || !dd->px) break;
        uint32_t col = (srcp && srcp->kind == R_PICTURE && srcp->pic_solid)
                     ? srcp->fg : 0xFF000000;          /* default opaque black */
        int penx = 0, peny = 0, off = 28;              /* glyph-element list */
        int drew = 0;
        while (off + 8 <= qlen) {
            int count = q[off];
            if (count == 255) {                        /* glyphset switch */
                gr = res_find(c, r32(q + off + 4)); gs = gset_of(gr);
                off += 8; continue;
            }
            penx += rs16(q + off + 4);                 /* deltax (1st elt = origin) */
            peny += rs16(q + off + 6);
            off += 8;
            for (int i = 0; i < count; i++) {
                if (off + idsz > qlen) break;
                uint32_t gid = idsz == 1 ? q[off] :
                               idsz == 2 ? r16(q + off) : r32(q + off);
                off += idsz;
                xglyph_t *g = glyph_find(gs, gid);
                if (g) { glyph_blit(dd, g, penx, peny, col); penx += g->xoff; peny += g->yoff; drew++; }
            }
            off = (off + 3) & ~3;                       /* pad to 4 */
        }
        if (drew && dd->kind == R_WINDOW) dirty = 1;
        render_n++;
        if (render_n <= 8) xt("XT CompositeGlyphs dst-win=0x%x drew=%d\n",
                              (unsigned)dstp->pic_drawable, drew);
        break;
    }
    case 10: case 11: break;   /* Trapezoids/Triangles — accept (no AA shapes yet) */
    default:
        /* QueryPictIndexValues(2) and a few others expect replies; give empty. */
        if (minor == 2) { uint8_t d[24]; memset(d,0,sizeof(d)); send_reply(c,0,d); }
        break;
    }
}

/* ── per-request dispatch ────────────────────────────────────────────────── */
static void dispatch(xclient_t *c, const uint8_t *q, int qlen) {
    int op = q[0];
    c->seq++;
    op_hist[op & 0xFF]++;
    op_ring[op_ring_n++ & 31] = (uint8_t)op;
    if (xdbg) printf("maerox: req op=%d seq=%d len=%d\n", op, c->seq, qlen);

    switch (op) {
    case RENDER_MAJOR:           /* XRender extension requests */
        dispatch_render(c, q, qlen);
        break;
    case 1: {  /* CreateWindow */
        uint32_t wid = r32(q + 4);
        xres_t *w = res_new(c, wid, R_WINDOW);
        if (!w) return;
        w->x = rs16(q + 12); w->y = rs16(q + 14);
        w->w = (int)r16(q + 16); w->h = (int)r16(q + 18);
        if (w->w < 1) w->w = 1; if (w->h < 1) w->h = 1;
        if (w->w > 4096) w->w = 4096; if (w->h > 4096) w->h = 4096;
        w->px = (uint32_t *)malloc((size_t)w->w * w->h * 4);
        if (w->px) for (int i = 0; i < w->w * w->h; i++) w->px[i] = 0x00202830;
        printf("maerox: CreateWindow xid=0x%x %dx%d @%d,%d\n",
               (unsigned)wid, w->w, w->h, w->x, w->y);
        break;
    }
    case 2: break;   /* ChangeWindowAttributes — accept */
    case 18: {       /* ChangeProperty — log 8-bit string props (WM_NAME /
                      * WM_CLASS / _NET_WM_NAME) so we can identify what window
                      * a client is naming (e.g. which modal dialog opens). */
        uint32_t wid  = r32(q + 4);
        uint32_t prop = r32(q + 8);
        uint8_t  fmt  = q[16];
        uint32_t dlen = r32(q + 20);
        if (fmt == 8 && dlen > 0 && dlen < 128 && 24 + dlen <= (uint32_t)qlen) {
            char s[130];
            memcpy(s, q + 24, dlen); s[dlen] = '\0';
            for (uint32_t i = 0; i < dlen; i++) if (s[i] == '\0') s[i] = '|';
            int anlen = 0;
            const char *an = atom_name(prop, &anlen);
            char nm[64];
            if (anlen > 63) anlen = 63;
            memcpy(nm, an, (size_t)anlen); nm[anlen] = '\0';
            printf("maerox: ChangeProperty xid=0x%x atom=%u(%s) str='%s'\n",
                   (unsigned)wid, (unsigned)prop, nm, s);
        }
        /* Every property change generates a PropertyNotify; GTK's
         * gdk_x11_get_server_time() blocks in XIfEvent until it sees one. */
        send_property_notify(c, wid, prop, 0);
        break;
    }
    case 19: {       /* DeleteProperty */
        send_property_notify(c, r32(q + 4), r32(q + 8), 1);
        break;
    }
    case 12: {       /* ConfigureWindow — GDK resizes/moves the window */
        xres_t *w = res_find(c, r32(q + 4));
        if (w && w->kind == R_WINDOW) {
            uint32_t mask = r16(q + 8);
            int off = 12;               /* value list follows the 12-byte header */
            int nx = w->x, ny = w->y, nw = w->w, nh = w->h;
            if (mask & 0x01) { nx = rs16(q + off); off += 4; }   /* x */
            if (mask & 0x02) { ny = rs16(q + off); off += 4; }   /* y */
            if (mask & 0x04) { nw = (int)r16(q + off); off += 4; }/* width */
            if (mask & 0x08) { nh = (int)r16(q + off); off += 4; }/* height */
            if (nw < 1) nw = 1; if (nh < 1) nh = 1;
            if (nw > 4096) nw = 4096; if (nh > 4096) nh = 4096;
            w->x = nx; w->y = ny;
            if (nw != w->w || nh != w->h) {   /* reallocate the backing buffer */
                uint32_t *np = (uint32_t *)malloc((size_t)nw * nh * 4);
                if (np) {
                    for (int i = 0; i < nw * nh; i++) np[i] = 0x00202830;
                    /* Carry the overlapping region across.  X leaves a resized
                     * window's contents undefined and we do send an Expose, but
                     * Firefox composites damage rather than redrawing on a bare
                     * Expose, so throwing the pixels away left the browser
                     * permanently blank whenever a ConfigureWindow arrived after
                     * the last PutImage — a run could report a real paint
                     * (putimg=25) and still show an empty window. */
                    if (w->px) {
                        int cw = w->w < nw ? w->w : nw;
                        int ch = w->h < nh ? w->h : nh;
                        for (int y = 0; y < ch; y++)
                            memcpy(np + (size_t)y * nw, w->px + (size_t)y * w->w,
                                   (size_t)cw * 4);
                        free(w->px);
                    }
                    w->px = np; w->w = nw; w->h = nh;
                }
            }
            dirty = 1;
            if (w->mapped) { send_configure(c, w); send_expose(c, w); }
        }
        break;
    }
    case 8: {        /* MapWindow */
        xres_t *w = res_find(c, r32(q + 4));
        if (w && w->kind == R_WINDOW) {
            /* Kiosk WM: GTK leaves the main browser window tiny (it expects a
             * sizing window-manager).  When a real top-level (not a 1x1/10x10
             * helper or a small popup) is first mapped, resize it to fill the
             * screen and report that geometry — gives Firefox a full content
             * area to render the page into (without this it configures to 1x1
             * and only the chrome paints). */
            int sw = (!headless && gui.surf.w > 0) ? gui.surf.w : 1280;
            int sh = (!headless && gui.surf.h > 0) ? gui.surf.h : 800;
            if (!w->maximized && w->w >= 400 && (w->w < sw || w->h < sh)) {
                uint32_t *np = (uint32_t *)malloc((size_t)sw * sh * 4);
                if (np) {
                    for (int i = 0; i < sw * sh; i++) np[i] = 0x00FFFFFF;
                    if (w->px) free(w->px);
                    w->px = np; w->w = sw; w->h = sh; w->x = 0; w->y = 0;
                }
                w->maximized = 1;
            }
            w->mapped = 1;
            dirty = 1;
            printf("maerox: MapWindow xid=0x%x %dx%d\n",
                   (unsigned)w->xid, w->w, w->h);
            xt("XT MapWindow xid=0x%x %dx%d @%d,%d\n",
               (unsigned)w->xid, w->w, w->h, w->x, w->y);
            send_map_notify(c, w);      /* mark viewable → GDK will paint */
            send_configure(c, w);       /* report geometry */
            send_expose(c, w);          /* ask the client to paint */
        }
        break;
    }
    case 10: {       /* UnmapWindow */
        xres_t *w = res_find(c, r32(q + 4));
        if (w && w->kind == R_WINDOW) { w->mapped = 0; dirty = 1; }
        break;
    }
    case 53: {       /* CreatePixmap */
        uint32_t pid = r32(q + 4);
        xres_t *p = res_new(c, pid, R_PIXMAP);
        if (!p) return;
        p->w = (int)r16(q + 12); p->h = (int)r16(q + 14);
        if (p->w < 1) p->w = 1; if (p->h < 1) p->h = 1;
        if (p->w > 4096) p->w = 4096; if (p->h > 4096) p->h = 4096;
        p->px = (uint32_t *)malloc((size_t)p->w * p->h * 4);
        if (p->px) memset(p->px, 0, (size_t)p->w * p->h * 4);
        break;
    }
    case 54: {       /* FreePixmap */
        xres_t *p = res_find(c, r32(q + 4));
        if (p) { if (p->px) free(p->px); p->kind = R_NONE; }
        break;
    }
    case 62: {       /* CopyArea (pixmap/window → window) — how GTK/Cairo paint */
        xres_t *src = res_find(c, r32(q + 4));
        xres_t *dst = res_find(c, r32(q + 8));
        copyarea_n++;
        if (copyarea_n <= 8) xt("XT CopyArea src=0x%x dst=0x%x\n",
                                (unsigned)r32(q + 4), (unsigned)r32(q + 8));
        if (src && src->px && dst && dst->px) {
            int sx = rs16(q + 16), sy = rs16(q + 18);
            int dx = rs16(q + 20), dy = rs16(q + 22);
            int w  = (int)r16(q + 24), h = (int)r16(q + 26);
            for (int yy = 0; yy < h; yy++) {
                int syy = sy + yy, dyy = dy + yy;
                if (syy < 0 || syy >= src->h || dyy < 0 || dyy >= dst->h) continue;
                for (int xx = 0; xx < w; xx++) {
                    int sxx = sx + xx, dxx = dx + xx;
                    if (sxx < 0 || sxx >= src->w || dxx < 0 || dxx >= dst->w) continue;
                    dst->px[(size_t)dyy * dst->w + dxx] =
                        src->px[(size_t)syy * src->w + sxx];
                }
            }
            dirty = 1;
        }
        break;
    }
    case 55:         /* CreateGC */
    case 56: {       /* ChangeGC */
        uint32_t gid  = r32(q + 4);
        int      voff = (op == 55) ? 16 : 12;
        uint32_t mask = r32(q + (op == 55 ? 12 : 8));
        xres_t *g = (op == 55) ? res_new(c, gid, R_GC) : res_find(c, gid);
        if (!g) break;
        g->kind = R_GC;
        for (int bit = 0; bit < 23; bit++) {
            if (!(mask & (1u << bit))) continue;
            uint32_t val = r32(q + voff);
            voff += 4;
            if (bit == 2) g->fg = val;       /* GCForeground */
            else if (bit == 3) g->bg = val;  /* GCBackground */
        }
        break;
    }
    case 60: {       /* FreeGC */
        xres_t *g = res_find(c, r32(q + 4));
        if (g) g->kind = R_NONE;
        break;
    }
    case 70: {       /* PolyFillRectangle */
        xres_t *d = res_find(c, r32(q + 4));
        xres_t *g = res_find(c, r32(q + 8));
        uint32_t fg = (g && g->kind == R_GC) ? g->fg : 0x00FFFFFF;
        int nrects = (qlen - 12) / 8;
        for (int i = 0; i < nrects; i++) {
            const uint8_t *rr = q + 12 + i * 8;
            fill_rect(d, rs16(rr), rs16(rr + 2), (int)r16(rr + 4), (int)r16(rr + 6), fg);
        }
        dirty = 1;
        break;
    }
    case 67: {       /* PolyRectangle (outline) */
        xres_t *d = res_find(c, r32(q + 4));
        xres_t *g = res_find(c, r32(q + 8));
        uint32_t fg = (g && g->kind == R_GC) ? g->fg : 0x00FFFFFF;
        int nrects = (qlen - 12) / 8;
        for (int i = 0; i < nrects; i++) {
            const uint8_t *rr = q + 12 + i * 8;
            int x = rs16(rr), y = rs16(rr + 2), w = (int)r16(rr + 4), h = (int)r16(rr + 6);
            fill_rect(d, x, y, w, 1, fg); fill_rect(d, x, y + h, w + 1, 1, fg);
            fill_rect(d, x, y, 1, h, fg); fill_rect(d, x + w, y, 1, h, fg);
        }
        dirty = 1;
        break;
    }
    case 72: {       /* PutImage (ZPixmap, depth 24/32) */
        xres_t *d = res_find(c, r32(q + 4));
        int iw = (int)r16(q + 12), ih = (int)r16(q + 14);
        int dx = rs16(q + 16), dy = rs16(q + 18);
        int depth = q[21];
        putimage_n++;
        if (putimage_n <= 8) xt("XT PutImage win=0x%x %dx%d @%d,%d depth=%d\n",
                                (unsigned)r32(q + 4), iw, ih, dx, dy, depth);
        /* Drop a "Firefox painted" marker the ff watchdog polls.  Re-created
         * after each launch (the launcher deletes it first), so check-then-create
         * makes it reappear on the first PutImage of a launch that reaches paint. */
        if (access("/tmp/ff_painted", F_OK) != 0) {
            int mfd = open("/tmp/ff_painted", O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (mfd >= 0) close(mfd);
        }
        if (d && d->px && (depth == 24 || depth == 32)) {
            int stride = ((iw * 4 + 3) & ~3);          /* scanline pad 32 */
            const uint8_t *img = q + 24;
            for (int yy = 0; yy < ih; yy++) {
                int ty = dy + yy;
                if (ty < 0 || ty >= d->h) continue;
                const uint8_t *row = img + (size_t)yy * stride;
                for (int xx = 0; xx < iw; xx++) {
                    int tx = dx + xx;
                    if (tx < 0 || tx >= d->w) continue;
                    d->px[(size_t)ty * d->w + tx] = r32(row + xx * 4) & 0x00FFFFFF;
                }
            }
            dirty = 1;
        }
        break;
    }
    case 61: {       /* ClearArea */
        xres_t *d = res_find(c, r32(q + 4));
        fill_rect(d, rs16(q + 8), rs16(q + 10), (int)r16(q + 12), (int)r16(q + 14), 0x00202830);
        dirty = 1;
        break;
    }
    case 73: {       /* GetImage → return the drawable's pixels (ZPixmap) */
        xres_t *d = res_find(c, r32(q + 4));
        int ix = rs16(q + 8), iy = rs16(q + 10);
        int iw = (int)r16(q + 12), ih = (int)r16(q + 14);
        uint8_t data[24]; memset(data, 0, sizeof(data));
        put32(data + 0, ROOT_VISUAL);          /* visual */
        if (d && d->px && iw > 0 && ih > 0 && iw * ih <= 1 << 22) {
            int n = iw * ih * 4;
            uint8_t *buf = (uint8_t *)malloc((size_t)n);
            if (buf) {
                for (int yy = 0; yy < ih; yy++) {
                    for (int xx = 0; xx < iw; xx++) {
                        uint32_t px = 0;
                        int sx = ix + xx, sy = iy + yy;
                        if (sx >= 0 && sx < d->w && sy >= 0 && sy < d->h)
                            px = d->px[(size_t)sy * d->w + sx];
                        put32(buf + ((size_t)yy * iw + xx) * 4, px);
                    }
                }
                send_reply_var(c, 24 /* depth */, data, buf, n);
                free(buf);
                break;
            }
        }
        send_reply_var(c, 24, data, NULL, 0);
        break;
    }
    case 14: {       /* GetGeometry → reply */
        xres_t *d = res_find(c, r32(q + 4));
        uint8_t data[24];
        memset(data, 0, sizeof(data));
        /* root */ data[0] = ROOT_WINDOW & 0xFF;
        if (d) {
            data[4] = d->x & 0xFF; data[5] = (d->x >> 8) & 0xFF;
            data[6] = d->y & 0xFF; data[7] = (d->y >> 8) & 0xFF;
            data[8] = d->w & 0xFF; data[9] = (d->w >> 8) & 0xFF;
            data[10] = d->h & 0xFF; data[11] = (d->h >> 8) & 0xFF;
        }
        send_reply(c, 24 /* depth */, data);
        break;
    }
    case 16: {       /* InternAtom → stable id per name (GDK round-trips these) */
        int only_if_exists = q[1];
        int nlen = (int)r16(q + 4);
        const char *name = (const char *)(q + 8);
        if (nlen < 0 || nlen > 63 || 8 + nlen > qlen) nlen = 0;
        uint32_t a = atom_intern(name, nlen, only_if_exists);
        uint8_t data[24]; memset(data, 0, sizeof(data));
        put32(data + 0, a);
        send_reply(c, 0, data);
        break;
    }
    case 17: {       /* GetAtomName → return the interned name */
        uint32_t a = r32(q + 4);
        int nlen = 0;
        const char *nm = atom_name(a, &nlen);
        uint8_t data[24]; memset(data, 0, sizeof(data));
        put16(data + 0, (uint32_t)nlen);            /* name length */
        send_reply_var(c, 0, data, (const uint8_t *)nm, nlen);
        break;
    }
    case 15: {       /* QueryTree → root + parent + children */
        uint32_t wid = r32(q + 4);
        uint8_t data[24]; memset(data, 0, sizeof(data));
        put32(data + 0, ROOT_WINDOW);               /* root */
        /* Top-level client windows are children of root; root has no parent. */
        put32(data + 4, (wid == ROOT_WINDOW) ? 0 : ROOT_WINDOW);  /* parent */
        put16(data + 8, 0);                         /* number of children */
        send_reply(c, 0, data);
        break;
    }
    case 20: {       /* GetProperty → empty */
        uint8_t data[24];
        memset(data, 0, sizeof(data));   /* type=None, bytes-after=0, len=0 */
        send_reply(c, 0, data);
        break;
    }
    case 43: {       /* GetInputFocus → reply (sync) */
        uint8_t data[24];
        memset(data, 0, sizeof(data));
        data[0] = ROOT_WINDOW & 0xFF;    /* focus = root */
        send_reply(c, 0, data);
        break;
    }
    case 98: {       /* QueryExtension */
        int nlen = (int)r16(q + 4);
        char nm[40]; nm[0] = '\0';
        if (nlen > 0 && nlen < 40 && 8 + nlen <= qlen) {
            memcpy(nm, q + 8, (size_t)nlen); nm[nlen] = '\0';
        }
        uint8_t data[24];
        memset(data, 0, sizeof(data));
        if (strcmp(nm, "RENDER") == 0) {   /* advertise XRender so GTK/cairo draw */
            data[0] = 1;                   /* present */
            data[1] = RENDER_MAJOR;        /* major-opcode */
            data[2] = 0;                   /* first-event */
            data[3] = RENDER_ERROR_BASE;   /* first-error */
            printf("maerox: QueryExtension 'RENDER' -> present (major %d)\n",
                   RENDER_MAJOR);
            xt("XT QueryExtension 'RENDER' -> PRESENT\n");
        } else {
            printf("maerox: QueryExtension '%s' -> not present\n", nm);
            xt("XT QueryExtension '%s' -> not present\n", nm);
        }
        send_reply(c, 0, data);
        break;
    }
    case 3: {        /* GetWindowAttributes → reply (44 bytes: 3 extra words) */
        uint32_t wid = r32(q + 4);
        xres_t *w = res_find(c, wid);            /* NULL for root or unknown */
        uint8_t data[24]; memset(data, 0, sizeof(data));
        uint8_t extra[12]; memset(extra, 0, sizeof(extra));
        put32(data + 0, ROOT_VISUAL);            /* the single TrueColor visual */
        data[4] = 1; data[5] = 0;                /* class = InputOutput */
        /* map-state: 0=Unmapped, 2=Viewable.  Report the window's real state so
         * GDK's view of mapped-ness matches the server. */
        int viewable = (wid == ROOT_WINDOW) || (w && w->mapped);
        data[16] = (uint8_t)(viewable ? 2 : 0);  /* map-state */
        /* extra: all-event-masks, your-event-mask, do-not-propagate, pad */
        send_reply_var(c, 0, data, extra, 12);
        break;
    }
    case 23: {       /* GetSelectionOwner → owner = None */
        uint8_t data[24]; memset(data, 0, sizeof(data));
        send_reply(c, 0, data);            /* owner window = 0 (None) */
        break;
    }
    case 38: {       /* QueryPointer → reply */
        uint8_t data[24]; memset(data, 0, sizeof(data));
        put32(data + 0, ROOT_WINDOW);      /* root */
        data[20] = 1;                      /* same-screen = True */
        /* b1 (sameScreen) handled as detail; report pointer at 0,0 */
        send_reply(c, 1, data);
        break;
    }
    case 101: {      /* GetKeyboardMapping → minimal 1 keysym/keycode */
        int first = q[4], count = q[5];
        if (count < 1) count = 1;
        int per = 1;                       /* keysyms-per-keycode */
        int n = count * per;
        uint8_t *ks = (uint8_t *)malloc((size_t)n * 4);
        if (ks) {
            for (int i = 0; i < count; i++)
                put32(ks + i * 4, (uint32_t)(first + i));  /* keycode as keysym */
            send_reply_var(c, (uint8_t)per, NULL, ks, n * 4);
            free(ks);
        } else {
            uint8_t d[24]; memset(d,0,sizeof(d)); send_reply(c, 1, d);
        }
        break;
    }
    case 119: {      /* GetModifierMapping → 8 modifiers, 0 keycodes each */
        uint8_t extra[8*2]; memset(extra, 0, sizeof(extra));
        send_reply_var(c, 2 /* keycodes-per-modifier */, NULL, extra, sizeof(extra));
        break;
    }
    case 78: break;  /* CreateColormap — accept */
    case 127: break; /* NoOperation */
    default:
        /* Any other reply-expecting request gets a generic empty reply so the
         * client doesn't block forever; non-reply requests are ignored. */
        if (req_expects_reply(op)) {
            uint8_t data[24]; memset(data, 0, sizeof(data));
            send_reply(c, 0, data);
            if (op_hist[op & 0xFF] <= 2) xt("XT generic-reply op=%d len=%d\n", op, qlen);
            if (xdbg) printf("maerox: generic-reply op=%d seq=%d\n", op, c->seq);
        } else {
            if (op_hist[op & 0xFF] <= 2) xt("XT ignored op=%d len=%d\n", op, qlen);
            if (xdbg) printf("maerox: ignored op=%d seq=%d len=%d\n", op, c->seq, qlen);
        }
        break;
    }
}

/* Drain a client's buffered bytes: first the setup request, then requests. */
/* Free everything a disconnecting client owns.  Firefox attempts are killed by
 * the ff watchdog many times per session; without this every dead client leaked
 * its window/pixmap pixel buffers (MBs per attempt) and glyphsets, degrading
 * later attempts monotonically. */
static void client_free_resources(xclient_t *c) {
    for (int i = 0; i < c->nres; i++) {
        xres_t *r = &c->res[i];
        if (r->kind == R_NONE) continue;
        if (r->px) { free(r->px); r->px = NULL; }
        if (r->kind == R_GLYPHSET && r->gset) {
            glyphset_t *gs = (glyphset_t *)r->gset;
            for (int k = 0; k < gs->n; k++)
                if (gs->g[k].bits) free(gs->g[k].bits);
            if (gs->g) free(gs->g);
            free(gs);
            r->gset = NULL;
        }
        r->kind = R_NONE;
    }
    c->nres = 0;
}

static void process_client(xclient_t *c) {
    /* Only read when there is room: read(fd, p, 0) returns 0, which is also how
     * a closed connection reports itself, so a full buffer would look like a
     * disconnect. */
    if (c->inlen < INBUF_SIZE) {
        int r = read(c->fd, c->inbuf + c->inlen, INBUF_SIZE - c->inlen);
        if (r == 0) { xt("XT client disconnected (last seq=%d)\n", c->seq);
                      close(c->fd); client_free_resources(c);
                      c->used = 0; dirty = 1; return; }   /* closed */
        if (r > 0) c->inlen += r;
    }

    if (!c->setup_done) {
        if (c->inlen < 12) return;
        int nauth = (int)r16(c->inbuf + 6), dauth = (int)r16(c->inbuf + 8);
        int need = 12 + ((nauth + 3) & ~3) + ((dauth + 3) & ~3);
        if (c->inlen < need) return;
        uint8_t reply[512];
        int sw = (!headless && gui.surf.w > 0) ? gui.surf.w : 1280;
        int sh = (!headless && gui.surf.h > 0) ? gui.surf.h : 800;
        int len = build_setup_reply(reply, sizeof(reply), sw, sh);
        write_all(c->fd, reply, len);
        c->setup_done = 1;
        memmove(c->inbuf, c->inbuf + need, c->inlen - need);
        c->inlen -= need;
        printf("maerox: client handshake complete (screen %dx%d headless=%d surf=%dx%d)\n",
               sw, sh, headless, gui.surf.w, gui.surf.h);
    }

    /* Process complete requests. */
    while (c->setup_done && c->inlen >= 4) {
        int qlen = (int)r16(c->inbuf + 2) * 4;
        if (qlen < 4) { c->inlen = 0; break; }       /* malformed; drop */
        if (qlen > INBUF_SIZE) {                     /* cannot ever complete */
            xt("XT oversize request op=%d len=%d > inbuf %d, dropping\n",
               c->inbuf[0], qlen, INBUF_SIZE);
            c->inlen = 0; break;
        }
        if (c->inlen < qlen) break;                  /* wait for the rest */
        dispatch(c, c->inbuf, qlen);
        memmove(c->inbuf, c->inbuf + qlen, c->inlen - qlen);
        c->inlen -= qlen;
    }
}

/* Base64-encode `len` bytes to fd, wrapping at 76 chars/line. */
static void b64_write(int fd, const uint8_t *in, int len) {
    static const char *T =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char line[80]; int col = 0;
    for (int i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < len) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < len) v |= in[i + 2];
        line[col++] = T[(v >> 18) & 63];
        line[col++] = T[(v >> 12) & 63];
        line[col++] = (i + 1 < len) ? T[(v >> 6) & 63] : '=';
        line[col++] = (i + 2 < len) ? T[v & 63] : '=';
        if (col >= 76) { line[col++] = '\n'; write(fd, line, col); col = 0; }
    }
    if (col) { line[col++] = '\n'; write(fd, line, col); }
}

/* Composite every mapped window into an off-screen RGB surface and stream it,
 * half-resolution, as base64 to /dev/tty (→ COM1 → host serial), bracketed by
 * FFDUMP/FFDUMPEND markers a host script decodes into a PNG.  This makes the
 * proven-but-invisible headless paint (putimg>0, no framebuffer) actually
 * viewable.  Half-res keeps the serial transfer to ~0.5MB. */
static void composite_windows(draw_surface_t *s);   /* fwd */
static void frame_dump(void) {
    if (trace_fd < 0) return;
    int W = 1280, H = 800;
    draw_surface_t s;
    s.w = W; s.h = H;
    s.px = (uint32_t *)malloc((size_t)W * H * 4);
    if (!s.px) return;
    for (int i = 0; i < W * H; i++) s.px[i] = 0x00202830;   /* desktop bg */
    composite_windows(&s);
    /* Downsample 2x into a packed RGB buffer. */
    int dw = W / 2, dh = H / 2;
    uint8_t *rgb = (uint8_t *)malloc((size_t)dw * dh * 3);
    if (rgb) {
        for (int y = 0; y < dh; y++)
            for (int x = 0; x < dw; x++) {
                uint32_t p = s.px[(size_t)(y * 2) * W + (x * 2)];
                uint8_t *o = rgb + ((size_t)y * dw + x) * 3;
                o[0] = (p >> 16) & 0xFF; o[1] = (p >> 8) & 0xFF; o[2] = p & 0xFF;
            }
        char hdr[48];
        int n = snprintf(hdr, sizeof(hdr), "\nFFDUMP %d %d\n", dw, dh);
        write(trace_fd, hdr, n);
        b64_write(trace_fd, rgb, dw * dh * 3);
        write(trace_fd, "FFDUMPEND\n", 10);
        free(rgb);
    }
    free(s.px);
}

/* ── compositing ─────────────────────────────────────────────────────────── */
static void composite_windows(draw_surface_t *s) {
    for (int ci = 0; ci < MAX_XCLIENTS; ci++) {
        if (!clients[ci].used) continue;
        xclient_t *c = &clients[ci];
        for (int i = 0; i < c->nres; i++) {
            xres_t *w = &c->res[i];
            if (w->kind != R_WINDOW || !w->mapped || !w->px) continue;
            for (int yy = 0; yy < w->h; yy++) {
                int ty = w->y + yy;
                if (ty < 0 || ty >= s->h) continue;
                for (int xx = 0; xx < w->w; xx++) {
                    int tx = w->x + xx;
                    if (tx < 0 || tx >= s->w) continue;
                    s->px[(size_t)ty * s->w + tx] = w->px[(size_t)yy * w->w + xx];
                }
            }
        }
    }
}

static int count_clients(void) {
    int n = 0;
    for (int i = 0; i < MAX_XCLIENTS; i++) if (clients[i].used) n++;
    return n;
}

/* Forward a libgui click (surface coords) to the topmost X window under it as a
 * ButtonPress + ButtonRelease pair. */
static void on_x_click(gui_window_t *g, int x, int y) {
    (void)g;
    xclient_t *hit_c = NULL; xres_t *hit_w = NULL;
    for (int ci = 0; ci < MAX_XCLIENTS; ci++) {
        if (!clients[ci].used) continue;
        xclient_t *c = &clients[ci];
        for (int i = 0; i < c->nres; i++) {
            xres_t *w = &c->res[i];
            if (w->kind != R_WINDOW || !w->mapped) continue;
            if (x >= w->x && x < w->x + w->w && y >= w->y && y < w->y + w->h) {
                hit_c = c; hit_w = w;       /* keep the last = topmost */
            }
        }
    }
    if (hit_c && hit_w) {
        int ex = x - hit_w->x, ey = y - hit_w->y;
        send_pointer(hit_c, hit_w, 4, 1, ex, ey);   /* ButtonPress, button 1 */
        send_pointer(hit_c, hit_w, 5, 1, ex, ey);   /* ButtonRelease */
    }
}

static void render(void) {
    draw_surface_t *s = &gui.surf;
    if (!s->px) return;
    draw_fill(s, draw_rgb(24, 28, 36));
    snprintf(status, sizeof(status), "maeroX :0 - %d client%s, DISPLAY=:0",
             count_clients(), count_clients() == 1 ? "" : "s");
    draw_text_aa(s, 12, 14, status, draw_rgb(150, 200, 255), &draw_font_ui);
    composite_windows(s);
    wm_commit(&gui.wm, gui.slot);
    dirty = 0;
}

/* ── connection setup ────────────────────────────────────────────────────── */
static int start_listener(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM | 0x800 /* NONBLOCK */, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, X_SOCKET_PATH);
    socklen_t alen = (socklen_t)(sizeof(addr.sun_family) + strlen(X_SOCKET_PATH));
    if (bind(fd, (struct sockaddr *)&addr, alen) != 0) { close(fd); return -2; }
    if (listen(fd, 8) != 0) { close(fd); return -3; }
    return fd;
}

static void accept_clients(void) {
    int cfd = accept(listen_fd, 0, 0);
    if (cfd < 0) return;
    fcntl(cfd, F_SETFL, O_RDWR | O_NONBLOCK);
    for (int i = 0; i < MAX_XCLIENTS; i++)
        if (!clients[i].used) {
            memset(&clients[i], 0, sizeof(clients[i]));
            clients[i].used = 1;
            clients[i].fd = cfd;
            dirty = 1;
            return;
        }
    close(cfd);   /* table full */
}

int main(int argc, char *argv[]) {
    int slot = 1;
    int daemon = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-H") || !strcmp(argv[i], "--headless")) headless = 1;
        else if (!strcmp(argv[i], "-v")) xdbg = 1;
        else if (!strcmp(argv[i], "-T")) trace_fd = -2;   /* request /dev/tty trace */
        else if (!strcmp(argv[i], "-D")) { dumpmode = 1; trace_fd = -2; }
        else if (!strcmp(argv[i], "-d") || !strcmp(argv[i], "--daemon")) daemon = 1;
        else if (argv[i][0] >= '0' && argv[i][0] <= '9') slot = atoi(argv[i]);
    }
    if (trace_fd == -2) {
        trace_fd = open("/dev/tty", O_WRONLY);   /* → COM1 → host serial log */
        xt("XT maeroX trace armed\n");
    }
    if (slot < 1 || slot > WM_MAX_SLOTS) slot = 1;

    listen_fd = start_listener();
    if (listen_fd < 0) { printf("maerox: listen failed (%d)\n", listen_fd); return 1; }

    if (daemon) {
        printf("maerox: listening on " X_SOCKET_PATH " (daemonized)\n");
        if (fork() > 0) return 0;          /* parent returns to the shell */
    }

    if (!headless &&
        gui_open(&gui, slot, "maeroX :0", 100 + slot * 12, 70 + slot * 10,
                 820, 560) < 0)
        headless = 1;
    if (headless) printf("maerox: running headless\n");
    if (!daemon)  printf("maerox: listening on " X_SOCKET_PATH "\n");
    if (!headless) {
        gui_set_click_handler(&gui, on_x_click);   /* forward clicks to X clients */
        render();
    }

    unsigned tick = 0;
    unsigned last_dump_tick = 0;
    while (headless || !gui.closed) {
        int events = headless ? 0 : gui_poll(&gui);
        accept_clients();
        for (int i = 0; i < MAX_XCLIENTS; i++)
            if (clients[i].used) process_client(&clients[i]);
        if (!headless && (dirty || events > 0)) render();
        if (trace_fd >= 0 && ++tick % 167 == 0) xt_dump_hist();   /* ~every 2s */
        /* Frame-dump mode: once Firefox has painted (putimage_n>0) and a full-
         * size toplevel is mapped, stream the composited frame every ~2s, a few
         * times (first paint is often partial; later ones are settled). */
        if (dumpmode && dumps_done < 4 && putimage_n > 0 &&
            (tick - last_dump_tick) >= 167) {
            int have_top = 0;
            for (int ci = 0; ci < MAX_XCLIENTS && !have_top; ci++) {
                if (!clients[ci].used) continue;
                for (int i = 0; i < clients[ci].nres; i++) {
                    xres_t *w = &clients[ci].res[i];
                    if (w->kind == R_WINDOW && w->mapped && w->px && w->w >= 400)
                        { have_top = 1; break; }
                }
            }
            if (have_top) { frame_dump(); dumps_done++; last_dump_tick = tick; }
        }
        sleep_ms(12);
    }

    close(listen_fd);
    if (!headless) gui_close(&gui);
    return 0;
}
