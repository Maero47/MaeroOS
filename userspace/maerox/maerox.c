/*
 * maeroX — the X11 server of MaeroOS.
 *
 * A libgui application: it owns one desktop window, whose body is the X
 * screen (the root window), listens on AF_UNIX /tmp/.X11-unix/X0 and serves
 * the X11 protocol to clients — Firefox, GTK 3 applications, Xt/Xaw programs
 * such as xterm, xeyes and xclock — from MaeroOS or from the Alpine chroot.
 *
 * This file holds the connections and the request loop, the window manager
 * (placement, focus, frames with a title bar and a close button, dragging),
 * compositing the window tree onto the desktop surface, and the input the
 * desktop forwards (pointer stream, uncooked keys, wheel).  The protocol
 * itself lives in xcore.c (core), xdraw.c (drawing), xfont.c (fonts) and
 * xrender.c (RENDER); see xs.h.
 *
 * Options:
 *   <slot>   desktop window slot (the desktop passes it)
 *   -H       headless: no desktop window, a 1280x800 screen (tests)
 *   -k       kiosk: a large toplevel fills the screen, no frames (Firefox);
 *            implied by -H
 *   -g WxH   initial window body size
 *   -K       headless only: the test injection FIFO /tmp/.maerox-keys
 *   -T       trace to /dev/tty;  -D also dumps frames there (base64)
 *   -x       request trace in the log: every request, reply, event and error
 *            per client (also on when /tmp/.maerox-xtrace exists as a client
 *            connects)
 *   -L file  log (QueryExtension answers, unimplemented requests...);
 *            default /tmp/maerox.log
 *   -d       daemonize after binding the socket
 */
#include <draw.h>
#include <errno.h>
#include <fcntl.h>
#include <gui.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include "xs.h"
#include "opnames.h"

#define X_SOCKET_DIR  "/tmp/.X11-unix"
#define X_SOCKET_PATH "/tmp/.X11-unix/X0"
#define FRAME_MS      16

client_t clients[MAX_XCLIENTS];
int      scr_w = 1280, scr_h = 800;
int      kiosk;
int      render_major = 139;

static gui_window_t gui;
static int       headless;
static int       listen_fd = -1;
static int       abstract_fd = -1;   /* @/tmp/.X11-unix/X0: reaches chroots */
static int       dirty = 1;
static int       pd_x0, pd_y0, pd_x1, pd_y1;
static unsigned  last_render_ms;
static int       trace_fd = -1;
int              xtrace;             /* request trace into the log (-x, or /tmp/.maerox-xtrace) */
static int       log_fd = -1;
static int       dumpmode, dumps_done;
static uint32_t  pending_focus;      /* toplevel to focus after its map */

void core_resize_root(int w, int h);

/* ── logging ─────────────────────────────────────────────────────────────── */
void xlog(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
    if (log_fd >= 0) write(log_fd, buf, (size_t)n);
    if (trace_fd >= 0) { write(trace_fd, "XT ", 3); write(trace_fd, buf, (size_t)n); }
}

static unsigned now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned)ts.tv_sec * 1000u + (unsigned)(ts.tv_nsec / 1000000L);
}

uint32_t x_time(void) {
    uint32_t t = now_ms();
    return t ? t : 1;
}

/* ── output ──────────────────────────────────────────────────────────────── */
static int out_send(client_t *c, const uint8_t *p, size_t n) {
    size_t off = 0;
    while (off < n) {
        int w = send(c->fd, p + off, n - off, MSG_NOSIGNAL);
        if (w > 0) { off += (size_t)w; continue; }
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && errno == EAGAIN) break;
        c->dead = 1;
        break;
    }
    return (int)off;
}

static void out_flush(client_t *c) {
    if (c->dead || c->outlen == 0) return;
    size_t w = (size_t)out_send(c, c->out + c->outoff, c->outlen);
    c->outoff += w;
    c->outlen -= w;
    if (c->outlen == 0) c->outoff = 0;
}

/* Never blocks: what the socket does not take now is queued, and a client
 * that lets more than OUTBUF_MAX pile up is dropped. */
void out_write(client_t *c, const void *data, size_t n) {
    const uint8_t *p = (const uint8_t *)data;
    if (c->dead || n == 0) return;
    if (c->outlen == 0) {
        size_t w = (size_t)out_send(c, p, n);
        if (c->dead) return;
        p += w; n -= w;
        if (n == 0) return;
    }
    if (c->outlen + n > OUTBUF_MAX) {
        xlog("client %d output queue over %u bytes, disconnecting\n", c->index, OUTBUF_MAX);
        c->dead = 1;
        return;
    }
    if (c->outoff + c->outlen + n > c->outcap && c->outoff) {
        memmove(c->out, c->out + c->outoff, c->outlen);
        c->outoff = 0;
    }
    if (c->outlen + n > c->outcap) {
        size_t cap = c->outcap ? c->outcap : 4096;
        while (cap < c->outlen + n) cap *= 2;
        if (cap > OUTBUF_MAX) cap = OUTBUF_MAX;
        uint8_t *np = realloc(c->out, cap);
        if (!np) { c->dead = 1; return; }
        c->out = np;
        c->outcap = cap;
    }
    memcpy(c->out + c->outoff + c->outlen, p, n);
    c->outlen += n;
}

void x_error(client_t *c, int code, uint32_t bad) {
    uint8_t e[32];
    memset(e, 0, sizeof(e));
    e[1] = (uint8_t)code;
    put16(e + 2, c->seq);
    put32(e + 4, bad);
    put16(e + 8, c->cur_minor);
    e[10] = c->cur_major;
    out_write(c, e, 32);
    if (trace_fd >= 0 || xtrace) xlog("c%d #%u error %d op=%d.%d bad=0x%x\n", c->index, c->seq, code, c->cur_major,
                            c->cur_minor, (unsigned)bad);
}

void send_reply(client_t *c, uint8_t b1, const uint8_t data24[24]) {
    uint8_t r[32];
    memset(r, 0, sizeof(r));
    r[0] = 1; r[1] = b1;
    put16(r + 2, c->seq);
    if (data24) memcpy(r + 8, data24, 24);
    out_write(c, r, 32);
    if (xtrace) xlog("c%d #%u reply\n", c->index, c->seq);
}

void send_reply_var(client_t *c, uint8_t b1, const uint8_t data24[24],
                    const uint8_t *extra, int extra_len) {
    uint8_t r[32];
    memset(r, 0, sizeof(r));
    r[0] = 1; r[1] = b1;
    put16(r + 2, c->seq);
    int words = (extra_len + 3) / 4;
    put32(r + 4, (uint32_t)words);
    if (data24) memcpy(r + 8, data24, 24);
    out_write(c, r, 32);
    if (xtrace) xlog("c%d #%u reply +%d bytes\n", c->index, c->seq, extra_len);
    if (extra_len > 0) {
        out_write(c, extra, (size_t)extra_len);
        int pad = words * 4 - extra_len;
        if (pad > 0) { uint8_t z[4] = { 0, 0, 0, 0 }; out_write(c, z, (size_t)pad); }
    }
}

void send_event(client_t *c, uint8_t ev[32]) {
    if (!c->used || c->dead || !c->setup_done) return;
    if ((ev[0] & 0x7F) != KeymapNotify) put16(ev + 2, c->seq);
    out_write(c, ev, 32);
    if (xtrace) xlog("c%d event %d%s win=0x%x\n", c->index, ev[0] & 0x7F, ev[0] & 0x80 ? " (sent)" : "",
                     (unsigned)(ev[4] | ev[5] << 8 | ev[6] << 16 | (uint32_t)ev[7] << 24));
}

/* ── damage ──────────────────────────────────────────────────────────────── */
static void damage_surface(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (pd_x0 >= pd_x1) { pd_x0 = x; pd_y0 = y; pd_x1 = x + w; pd_y1 = y + h; return; }
    if (x < pd_x0) pd_x0 = x;
    if (y < pd_y0) pd_y0 = y;
    if (x + w > pd_x1) pd_x1 = x + w;
    if (y + h > pd_y1) pd_y1 = y + h;
}

void damage_all(void) { dirty = 1; }

void damage_window(window_t *w, int x, int y, int ww, int hh) {
    if (dirty || !window_viewable(w)) return;
    int ax, ay;
    window_abs(w, &ax, &ay);
    damage_surface(ax + x, ay + y, ww, hh);
}

/* ── window manager ──────────────────────────────────────────────────────── */
#define TITLE_H 22
#define FRAME_B 1
static int cascade_n;

static int wm_framed(window_t *w) {
    return !kiosk && w->parent == root && !w->override_redirect && w->cls != 2 && w->wm_placed;
}

static void wm_frame_rect(window_t *w, int *x, int *y, int *fw, int *fh) {
    *x = w->x - FRAME_B;
    *y = w->y - TITLE_H - FRAME_B;
    *fw = w->d.w + 2 * w->bw + 2 * FRAME_B;
    *fh = w->d.h + 2 * w->bw + TITLE_H + 2 * FRAME_B;
}

static uint32_t A(const char *n) { return atom_intern(n, (int)strlen(n), 0); }

/* The first toplevel window of client c other than w, for dialogs without
 * WM_TRANSIENT_FOR. */
static window_t *transient_parent(window_t *w) {
    prop_t *p = prop_find(w, 68 /* WM_TRANSIENT_FOR */);
    if (p && p->format == 32 && p->n >= 1) {
        window_t *t = lookup_window(r32(p->data));
        if (t) return window_toplevel(t);
    }
    return NULL;
}

static void title_of(window_t *w, char *out, int cap);

void wm_map_toplevel(client_t *c, window_t *w) {
    (void)c;
    if (w->wm_placed) {
        raise_window(w);
        pending_focus = w->d.o.id;
        return;
    }
    w->wm_placed = 1;
    int x = w->x, y = w->y, ww = w->d.w, hh = w->d.h;
    if (kiosk) {
        if (ww >= 400) { x = 0; y = 0; ww = scr_w; hh = scr_h; w->wm_max = 1; }
    } else {
        int avail_w = scr_w - 2 * FRAME_B, avail_h = scr_h - TITLE_H - 2 * FRAME_B;
        if (ww > avail_w) ww = avail_w;
        if (hh > avail_h) hh = avail_h;
        /* WM_NORMAL_HINTS with USPosition/PPosition: honour the position. */
        prop_t *nh = prop_find(w, 40 /* WM_NORMAL_HINTS */);
        int has_pos = nh && nh->format == 32 && nh->n >= 1 && (r32(nh->data) & 3) && (x || y);
        window_t *tp = transient_parent(w);
        if (tp && tp != w) {
            x = tp->x + (tp->d.w - ww) / 2;
            y = tp->y + (tp->d.h - hh) / 2;
        } else if (!has_pos) {
            x = 24 + 28 * (cascade_n % 8);
            y = TITLE_H + 16 + 28 * (cascade_n % 8);
            cascade_n++;
        }
        if (x + ww + FRAME_B > scr_w) x = scr_w - ww - FRAME_B;
        if (y + hh + FRAME_B > scr_h) y = scr_h - hh - FRAME_B;
        if (x < FRAME_B) x = FRAME_B;
        if (y < TITLE_H + FRAME_B) y = TITLE_H + FRAME_B;
    }
    if (x != w->x || y != w->y || ww != w->d.w || hh != w->d.h)
        configure_window(w, x, y, ww, hh, w->bw, 1);
    raise_window(w);
    pending_focus = w->d.o.id;
    damage_all();
    char title[64];
    title_of(w, title, sizeof(title));
    xlog("map toplevel 0x%x client=%d %dx%d @%d,%d title='%s'\n", (unsigned)w->d.o.id,
         w->d.o.owner, w->d.w, w->d.h, w->x, w->y, title);
}

void wm_window_gone(window_t *w);
static uint32_t drag_win;
static int drag_dx, drag_dy;

void wm_window_gone(window_t *w) {
    if (drag_win == w->d.o.id) drag_win = 0;
    if (pending_focus == w->d.o.id) pending_focus = 0;
    damage_all();
}

void wm_restacked(void) { damage_all(); }

void wm_property_changed(window_t *w, uint32_t atom) {
    if (w->parent == root && (atom == 39 /* WM_NAME */ || atom == A("_NET_WM_NAME")))
        damage_all();
}

static void wm_activate(window_t *w) {
    if (!w || !w->mapped) return;
    raise_window(w);
    if (window_has_protocol(w, "WM_TAKE_FOCUS"))
        send_client_message(w, A("WM_PROTOCOLS"), A("WM_TAKE_FOCUS"), x_time());
    set_focus(w, 2);
}

static void wm_close_window(window_t *w) {
    if (window_has_protocol(w, "WM_DELETE_WINDOW"))
        send_client_message(w, A("WM_PROTOCOLS"), A("WM_DELETE_WINDOW"), x_time());
    else if (w->d.o.owner >= 0)
        kill_client_windows(w->d.o.owner);
}

static void wm_maximize(window_t *w, int on) {
    if (on && !w->wm_max) {
        w->wm_max = 1;
        configure_window(w, FRAME_B, TITLE_H + FRAME_B, scr_w - 2 * FRAME_B,
                         scr_h - TITLE_H - 2 * FRAME_B, w->bw, 1);
    } else if (!on && w->wm_max) {
        w->wm_max = 0;
        configure_window(w, 60, TITLE_H + 40, scr_w * 2 / 3, scr_h * 2 / 3, w->bw, 1);
    }
    damage_all();
}

/* EWMH/ICCCM messages a client sends the window manager through the root. */
int wm_client_message_to_root(client_t *c, const uint8_t *ev) {
    (void)c;
    window_t *w = lookup_window(r32(ev + 4));
    uint32_t type = r32(ev + 8);
    if (!w) return 0;
    if (type == A("_NET_ACTIVE_WINDOW")) {
        wm_activate(window_toplevel(w));
    } else if (type == A("_NET_CLOSE_WINDOW")) {
        wm_close_window(w);
    } else if (type == A("_NET_WM_STATE")) {
        uint32_t action = r32(ev + 12), a1 = r32(ev + 16), a2 = r32(ev + 20);
        uint32_t mh = A("_NET_WM_STATE_MAXIMIZED_HORZ"), mv = A("_NET_WM_STATE_MAXIMIZED_VERT");
        uint32_t fs = A("_NET_WM_STATE_FULLSCREEN");
        if (a1 == mh || a1 == mv || a2 == mh || a2 == mv || a1 == fs)
            wm_maximize(w, action == 2 ? !w->wm_max : action == 1);
    } else if (type == A("WM_CHANGE_STATE")) {
        /* IconicState: no icons here; leave the window as it is. */
    }
    return 1;
}

static void title_of(window_t *w, char *out, int cap) {
    prop_t *p = prop_find(w, A("_NET_WM_NAME"));
    if (!p || p->format != 8 || !p->n) p = prop_find(w, 39);
    out[0] = 0;
    if (!p || p->format != 8) return;
    int n = (int)p->n < cap - 1 ? (int)p->n : cap - 1;
    memcpy(out, p->data, (size_t)n);
    out[n] = 0;
}

/* Is (x, y) on the frame of a framed toplevel?  Returns it, *part 1 title,
 * 2 close button. */
static window_t *wm_frame_hit(int x, int y, int *part) {
    for (window_t *t = root->top; t; t = t->below) {
        if (!t->mapped) continue;
        int fx, fy, fw, fh;
        if (wm_framed(t)) {
            wm_frame_rect(t, &fx, &fy, &fw, &fh);
            if (x >= fx && y >= fy && x < fx + fw && y < fy + TITLE_H + FRAME_B) {
                *part = (x >= fx + fw - TITLE_H) ? 2 : 1;
                return t;
            }
        }
        if (x >= t->x && y >= t->y && x < t->x + t->d.w + 2 * t->bw &&
            y < t->y + t->d.h + 2 * t->bw)
            return NULL;                     /* inside a window above */
    }
    return NULL;
}

/* ── compositing ─────────────────────────────────────────────────────────── */
static void blit_rect(draw_surface_t *s, const uint32_t *src, int sw, int sh,
                      int ax, int ay, int cx0, int cy0, int cx1, int cy1) {
    int x0 = imax(ax, cx0), y0 = imax(ay, cy0), x1 = imin(ax + sw, cx1), y1 = imin(ay + sh, cy1);
    /* A source wholly left or right of the clip: x1 < x0 would be a
     * negative length (GIMP's many frames crashed maeroX here). */
    if (x0 >= x1) return;
    for (int y = y0; y < y1; y++)
        memcpy(s->px + (size_t)y * s->w + x0, src + (size_t)(y - ay) * sw + (x0 - ax),
               (size_t)(x1 - x0) * 4);
}

static void fill_clip(draw_surface_t *s, int x, int y, int w, int h, uint32_t col,
                      int cx0, int cy0, int cx1, int cy1) {
    int x0 = imax(x, cx0), y0 = imax(y, cy0), x1 = imin(x + w, cx1), y1 = imin(y + h, cy1);
    if (x0 >= x1) return;
    for (int yy = y0; yy < y1; yy++) fill32(s->px + (size_t)yy * s->w + x0, col, x1 - x0);
}

static void paint_frame(draw_surface_t *s, window_t *w, int cx0, int cy0, int cx1, int cy1) {
    int fx, fy, fw, fh;
    wm_frame_rect(w, &fx, &fy, &fw, &fh);
    int focused = focus_window() && window_toplevel(focus_window()) == w;
    uint32_t edge = focused ? draw_rgb(58, 121, 200) : draw_rgb(70, 74, 84);
    uint32_t bar = focused ? draw_rgb(44, 62, 92) : draw_rgb(48, 52, 60);
    fill_clip(s, fx, fy, fw, FRAME_B, edge, cx0, cy0, cx1, cy1);
    fill_clip(s, fx, fy + fh - FRAME_B, fw, FRAME_B, edge, cx0, cy0, cx1, cy1);
    fill_clip(s, fx, fy, FRAME_B, fh, edge, cx0, cy0, cx1, cy1);
    fill_clip(s, fx + fw - FRAME_B, fy, FRAME_B, fh, edge, cx0, cy0, cx1, cy1);
    fill_clip(s, fx + FRAME_B, fy + FRAME_B, fw - 2 * FRAME_B, TITLE_H, bar, cx0, cy0, cx1, cy1);
    /* close box */
    int bx = fx + fw - TITLE_H, by = fy + FRAME_B;
    fill_clip(s, bx + 3, by + 3, TITLE_H - 6, TITLE_H - 6, draw_rgb(176, 64, 64), cx0, cy0, cx1, cy1);
    if (fy + TITLE_H < cy0 || fy > cy1) return;
    /* Title text: drawn into a strip, then clipped onto the surface. */
    char title[128];
    title_of(w, title, sizeof(title));
    int tw = fw - TITLE_H - 12;
    if (tw <= 0 || !title[0]) return;
    uint32_t *strip = malloc((size_t)tw * TITLE_H * 4);
    if (!strip) return;
    draw_surface_t t = { strip, tw, TITLE_H };
    draw_fill(&t, bar);
    draw_text_aa(&t, 0, 4, title, draw_rgb(225, 230, 240), &draw_font_ui);
    blit_rect(s, strip, tw, TITLE_H, fx + 8, fy + FRAME_B, cx0, cy0, cx1, cy1);
    free(strip);
}

static void draw_window(draw_surface_t *s, window_t *w, int px, int py,
                        int cx0, int cy0, int cx1, int cy1) {
    if (!w->mapped) return;
    int ox = px + w->x, oy = py + w->y;          /* outer corner */
    int ax = ox + w->bw, ay = oy + w->bw;        /* inside */
    if (w->parent == root && wm_framed(w)) paint_frame(s, w, cx0, cy0, cx1, cy1);
    if (w->bw > 0 && w->cls != 2) {
        int ow = w->d.w + 2 * w->bw, oh = w->d.h + 2 * w->bw;
        uint32_t bc = w->border_pixel & 0xFFFFFF;
        fill_clip(s, ox, oy, ow, w->bw, bc, cx0, cy0, cx1, cy1);
        fill_clip(s, ox, oy + oh - w->bw, ow, w->bw, bc, cx0, cy0, cx1, cy1);
        fill_clip(s, ox, oy, w->bw, oh, bc, cx0, cy0, cx1, cy1);
        fill_clip(s, ox + ow - w->bw, oy, w->bw, oh, bc, cx0, cy0, cx1, cy1);
    }
    int ix0 = imax(ax, cx0), iy0 = imax(ay, cy0);
    int ix1 = imin(ax + w->d.w, cx1), iy1 = imin(ay + w->d.h, cy1);
    if (ix0 >= ix1 || iy0 >= iy1) return;
    if (w->cls != 2 && w->d.px && (w->painted || w->bg_mode != BG_NONE))
        blit_rect(s, w->d.px, w->d.w, w->d.h, ax, ay, ix0, iy0, ix1, iy1);
    for (window_t *ch = w->bottom; ch; ch = ch->above)
        draw_window(s, ch, ax, ay, ix0, iy0, ix1, iy1);
}

static void composite(draw_surface_t *s, int x0, int y0, int x1, int y1) {
    if (x1 > s->w) x1 = s->w;
    if (y1 > s->h) y1 = s->h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x0 >= x1 || y0 >= y1) return;
    blit_rect(s, root->d.px, root->d.w, root->d.h, 0, 0, x0, y0, x1, y1);
    for (window_t *w = root->bottom; w; w = w->above) draw_window(s, w, 0, 0, x0, y0, x1, y1);
}

static int count_clients(void) {
    int n = 0;
    for (int i = 0; i < MAX_XCLIENTS; i++) if (clients[i].used) n++;
    return n;
}

/* The root's background with a status line in its bottom-left corner. */
static int status_clients = -1;
static void paint_root_status(void) {
    int n = count_clients();
    if (n == status_clients) return;
    status_clients = n;
    fill_background(root, 0, root->d.h - 24, root->d.w, 24);
    char line[96];
    snprintf(line, sizeof(line), "maeroX  DISPLAY=:0  %d client%s", n, n == 1 ? "" : "s");
    draw_surface_t t = { root->d.px, root->d.w, root->d.h };
    draw_text_aa(&t, 10, root->d.h - 20, line, draw_rgb(120, 140, 170), &draw_font_ui);
    damage_window(root, 0, root->d.h - 24, root->d.w, 24);
}

static void render(void) {
    draw_surface_t *s = &gui.surf;
    if (!s->px) return;
    paint_root_status();
    int x0 = 0, y0 = 0, x1 = s->w, y1 = s->h;
    if (!dirty) {
        if (pd_x0 >= pd_x1) return;
        x0 = pd_x0; y0 = pd_y0; x1 = pd_x1; y1 = pd_y1;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > s->w) x1 = s->w;
        if (y1 > s->h) y1 = s->h;
    }
    pd_x0 = pd_x1 = 0;
    if (x0 < x1 && y0 < y1) {
        composite(s, x0, y0, x1, y1);
        if (dirty) wm_commit(&gui.wm, gui.slot);
        else wm_commit_rect(&gui.wm, gui.slot, x0, y0, x1 - x0, y1 - y0);
    }
    dirty = 0;
    last_render_ms = now_ms();
}

static int render_due(void) {
    if (!dirty && pd_x0 >= pd_x1) return 0;
    return now_ms() - last_render_ms >= FRAME_MS;
}

/* ── frame dump (-D): the composited screen, half size, base64 on /dev/tty ── */
static void b64_write(int fd, const uint8_t *in, int len) {
    static const char *T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char line[80];
    int col = 0;
    for (int i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < len) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < len) v |= in[i + 2];
        line[col++] = T[(v >> 18) & 63];
        line[col++] = T[(v >> 12) & 63];
        line[col++] = (i + 1 < len) ? T[(v >> 6) & 63] : '=';
        line[col++] = (i + 2 < len) ? T[v & 63] : '=';
        if (col >= 76) { line[col++] = '\n'; write(fd, line, (size_t)col); col = 0; }
    }
    if (col) { line[col++] = '\n'; write(fd, line, (size_t)col); }
}

static void frame_dump(void) {
    if (trace_fd < 0) return;
    draw_surface_t s;
    s.w = scr_w; s.h = scr_h;
    s.px = malloc((size_t)scr_w * scr_h * 4);
    if (!s.px) return;
    composite(&s, 0, 0, s.w, s.h);
    int dw = scr_w / 2, dh = scr_h / 2;
    uint8_t *rgb = malloc((size_t)dw * dh * 3);
    if (rgb) {
        for (int y = 0; y < dh; y++)
            for (int x = 0; x < dw; x++) {
                uint32_t p = s.px[(size_t)(y * 2) * scr_w + x * 2];
                uint8_t *o = rgb + ((size_t)y * dw + x) * 3;
                o[0] = (uint8_t)(p >> 16); o[1] = (uint8_t)(p >> 8); o[2] = (uint8_t)p;
            }
        char hdr[48];
        int n = snprintf(hdr, sizeof(hdr), "\nFFDUMP %d %d\n", dw, dh);
        write(trace_fd, hdr, (size_t)n);
        b64_write(trace_fd, rgb, dw * dh * 3);
        write(trace_fd, "FFDUMPEND\n", 10);
        free(rgb);
    }
    free(s.px);
}

/* ── input from the desktop ──────────────────────────────────────────────── */
static unsigned desk_buttons;            /* desktop mask: 1 left, 2 right, 4 middle */

/* A press on the content of a toplevel brings it forward and gives it the
 * keyboard, unless the client manages the focus itself and already has it
 * inside that toplevel. */
static void click_to_focus(void) {
    window_t *sp = sprite_window();
    window_t *top = sp ? window_toplevel(sp) : NULL;
    if (!top || top->override_redirect) return;
    window_t *f = focus_window();
    if (f && window_toplevel(f) == top) {
        if (root->top != top) raise_window(top);
        return;
    }
    wm_activate(top);
}

static void on_pointer(int x, int y, unsigned buttons) {
    int part = 0;
    unsigned pressed = buttons & ~desk_buttons, released = desk_buttons & ~buttons;
    if (drag_win) {                          /* moving a toplevel by its title */
        window_t *w = lookup_window(drag_win);
        if (w && (x != ptr_x || y != ptr_y))
            configure_window(w, x - drag_dx, y - drag_dy, w->d.w, w->d.h, w->bw, 1);
        ptr_x = x; ptr_y = y;
        if (!(buttons & 1)) drag_win = 0;
        desk_buttons = buttons;
        return;
    }
    if ((pressed & 1) && !(ptr_buttons & 0x1F00)) {
        window_t *t = wm_frame_hit(x, y, &part);
        if (t) {
            wm_activate(t);
            if (part == 2) wm_close_window(t);
            else { drag_win = t->d.o.id; drag_dx = x - t->x; drag_dy = y - t->y; }
            desk_buttons = buttons;
            ptr_x = x; ptr_y = y;
            return;
        }
    }
    if (x != ptr_x || y != ptr_y) {
        ptr_x = x; ptr_y = y;
        pointer_moved();
    }
    static const int xbtn[3] = { 1, 3, 2 };   /* left, right, middle */
    for (int b = 0; b < 3; b++) {
        if (pressed & (1u << b)) {
            if (!(ptr_buttons & 0x1F00)) click_to_focus();
            pointer_button(xbtn[b], 1);
        }
        if (released & (1u << b)) pointer_button(xbtn[b], 0);
    }
    desk_buttons = buttons;
}

static void on_ptr(gui_window_t *g, int x, int y, int buttons, int inside) {
    (void)g; (void)inside;
    on_pointer(x, y, (unsigned)buttons & 7);
}

static void on_rawkey(gui_window_t *g, int code, int value, int mods) {
    (void)g;
    if (code <= 0 || code + 8 > 255) return;
    key_mods = (unsigned)mods & 0xFF;
    key_event(code + 8, value);
}

static void on_scroll(gui_window_t *g, int delta) {
    (void)g;
    scroll_event(delta);
}

/* ── test injection channel (-K, headless only) ──────────────────────────── */
#define KEYFIFO_PATH "/tmp/.maerox-keys"
static int  test_keys;
static int  keyfifo_fd = -1;
static char keyfifo_line[64];
static int  keyfifo_used;

/* Real input arrives from the desktop; a headless server has none, so the
 * smoke tests inject through a FIFO: "k <linux keycode> <1|0> <mods>" and
 * "c <x> <y>" (a left click).  It needs -K AND headless, so a desktop
 * session never has the channel. */
static void keyfifo_open(void) {
    if (!test_keys) return;
    if (!headless) {
        printf("maerox: -K refused: the key-injection channel is headless-only\n");
        return;
    }
    unlink(KEYFIFO_PATH);
    if (mkfifo(KEYFIFO_PATH, 0600) != 0) return;
    chmod(KEYFIFO_PATH, 0600);
    keyfifo_fd = open(KEYFIFO_PATH, O_RDONLY | O_NONBLOCK);
}

static void keyfifo_poll(void) {
    char ch;
    if (keyfifo_fd < 0) return;
    while (read(keyfifo_fd, &ch, 1) == 1) {
        if (ch == '\r') continue;
        if (ch != '\n') {
            if (keyfifo_used + 1 < (int)sizeof(keyfifo_line)) keyfifo_line[keyfifo_used++] = ch;
            continue;
        }
        keyfifo_line[keyfifo_used] = 0;
        keyfifo_used = 0;
        int a = 0, b = 0, m = 0;
        if (keyfifo_line[0] == 'k' && sscanf(keyfifo_line + 1, "%d %d %d", &a, &b, &m) == 3) {
            on_rawkey(NULL, a, b, m);
        } else if (keyfifo_line[0] == 'c' && sscanf(keyfifo_line + 1, "%d %d", &a, &b) == 2) {
            on_pointer(a, b, desk_buttons);
            on_pointer(a, b, desk_buttons | 1);
            on_pointer(a, b, desk_buttons & ~1u);
        } else if (keyfifo_line[0] == 'p' && sscanf(keyfifo_line + 1, "%d %d %d", &a, &b, &m) == 3) {
            on_pointer(a, b, (unsigned)m);
        }
    }
}

/* ── connections ─────────────────────────────────────────────────────────── */
static int build_setup_reply(uint8_t *out, int cap, int ci) {
    (void)cap;
    uint8_t *p = out;
    const char *vendor = "MaeroOS maeroX";
    int vlen = (int)strlen(vendor), vpad = (4 - (vlen & 3)) & 3;
    /* pixmap formats: depth 1, 4, 8, 24, 32 */
    static const uint8_t fmts[5][3] = { { 1, 1, 32 }, { 4, 8, 32 }, { 8, 8, 32 }, { 24, 32, 32 }, { 32, 32, 32 } };
    int nfmt = 5;
    /* screen: 40 bytes + depths: 24 (1 visual: 8 + 24) and 1, 4, 8, 32 (none, 8 each) */
    int screen_len = 40 + (8 + 24) + 4 * 8;
    int extra = 32 + vlen + vpad + 8 * nfmt + screen_len;
    memset(out, 0, (size_t)(8 + extra));
    p[0] = 1;
    put16(p + 2, 11); put16(p + 4, 0); put16(p + 6, (uint32_t)(extra / 4));
    p += 8;
    put32(p + 0, 12101004);                      /* release */
    put32(p + 4, (uint32_t)(ci + 1) << CLIENT_ID_SHIFT);
    put32(p + 8, CLIENT_ID_MASK);
    put32(p + 12, 256);                          /* motion buffer */
    put16(p + 16, (uint32_t)vlen);
    put16(p + 18, 65535);                        /* maximum request length */
    p[20] = 1;                                   /* screens */
    p[21] = (uint8_t)nfmt;
    p[22] = 0; p[23] = 0;                        /* LSBFirst image and bitmap */
    p[24] = 32; p[25] = 32;                      /* bitmap scanline unit/pad */
    p[26] = 8; p[27] = 255;                      /* keycodes */
    p += 32;
    memcpy(p, vendor, (size_t)vlen);
    p += vlen + vpad;
    for (int i = 0; i < nfmt; i++) { p[0] = fmts[i][0]; p[1] = fmts[i][1]; p[2] = fmts[i][2]; p += 8; }
    put32(p + 0, ROOT_WINDOW);
    put32(p + 4, ROOT_COLORMAP);
    put32(p + 8, 0xFFFFFF);                      /* white */
    put32(p + 12, 0);                            /* black */
    put32(p + 16, 0);                            /* current input masks */
    put16(p + 20, (uint32_t)scr_w); put16(p + 22, (uint32_t)scr_h);
    put16(p + 24, (uint32_t)(scr_w * 254 / 960)); put16(p + 26, (uint32_t)(scr_h * 254 / 960));
    put16(p + 28, 1); put16(p + 30, 1);          /* installed maps */
    put32(p + 32, ROOT_VISUAL);
    p[36] = 0; p[37] = 0; p[38] = 24; p[39] = 5; /* backing, save-unders, root depth, depths */
    p += 40;
    p[0] = 24; put16(p + 2, 1); p += 8;          /* DEPTH 24, one visual */
    put32(p, ROOT_VISUAL); p[4] = 4; p[5] = 8; put16(p + 6, 256);
    put32(p + 8, 0xFF0000); put32(p + 12, 0x00FF00); put32(p + 16, 0x0000FF);
    p += 24;
    static const uint8_t other[4] = { 1, 4, 8, 32 };
    for (int i = 0; i < 4; i++) { p[0] = other[i]; p += 8; }
    return (int)(p - out);
}

static void setup_refuse(client_t *c, int msb, const char *why) {
    uint8_t r[8 + 64];
    int n = (int)strlen(why), words = (n + 3) / 4;
    memset(r, 0, sizeof(r));
    r[1] = (uint8_t)n;
    if (msb) { r[3] = 11; r[7] = (uint8_t)words; }
    else { r[2] = 11; r[6] = (uint8_t)words; }
    memcpy(r + 8, why, (size_t)n);
    out_write(c, r, 8 + (size_t)words * 4);
    c->dead = 1;
}

static void client_close(client_t *c) {
    xlog("client %d disconnected (seq %d%s)\n", c->index, c->seq, c->dead ? ", dropped" : "");
    close(c->fd);
    core_client_gone(c);
    free(c->out);
    free(c->inbuf);
    memset(c, 0, sizeof(*c));
    damage_all();
}

static void dispatch(client_t *c, const uint8_t *q, int qlen) {
    c->seq++;
    c->cur_major = q[0];
    c->cur_minor = 0;
    if (xtrace) {
        if (q[0] >= 1 && q[0] <= 119) xlog("c%d #%u %s len=%d\n", c->index, c->seq, core_names[q[0]], qlen);
        else if (q[0] == render_major) xlog("c%d #%u RENDER.%d len=%d\n", c->index, c->seq, q[1], qlen);
        else xlog("c%d #%u op=%d.%d len=%d\n", c->index, c->seq, q[0], q[1], qlen);
    }
    core_dispatch(c, q, qlen);
}

static void process_client(client_t *c) {
    if (c->dead) return;
    if (c->inlen < INBUF_SIZE) {
        int r = read(c->fd, c->inbuf + c->inlen, (size_t)(INBUF_SIZE - c->inlen));
        if (r == 0) { c->dead = 1; return; }
        if (r < 0 && errno != EAGAIN && errno != EINTR) { c->dead = 1; return; }
        if (r > 0) c->inlen += r;
    }
    if (!c->setup_done) {
        if (c->inlen < 1) return;
        if (c->inbuf[0] != 'l') {
            setup_refuse(c, c->inbuf[0] == 'B', "maeroX serves LSBFirst clients only");
            return;
        }
        if (c->inlen < 12) return;
        int nauth = (int)r16(c->inbuf + 6), dauth = (int)r16(c->inbuf + 8);
        int need = 12 + ((nauth + 3) & ~3) + ((dauth + 3) & ~3);
        if (c->inlen < need) return;
        uint8_t reply[512];
        int len = build_setup_reply(reply, sizeof(reply), c->index);
        out_write(c, reply, (size_t)len);
        c->setup_done = 1;
        memmove(c->inbuf, c->inbuf + need, (size_t)(c->inlen - need));
        c->inlen -= need;
        if (access("/tmp/.maerox-xtrace", F_OK) == 0) xtrace = 1;
        xlog("client %d connected (screen %dx%d)\n", c->index, scr_w, scr_h);
    }
    int pos = 0;
    while (c->setup_done && !c->dead && c->inlen - pos >= 4) {
        const uint8_t *q = c->inbuf + pos;
        int qlen = (int)r16(q + 2) * 4;
        if (qlen < 4) {
            /* Length 0 would be BIG-REQUESTS, which is not offered: the
             * stream cannot be resynchronised, so report and drop. */
            c->seq++;
            c->cur_major = q[0]; c->cur_minor = 0;
            x_error(c, BadLength, 0);
            c->dead = 1;
            break;
        }
        if (c->inlen - pos < qlen) break;
        dispatch(c, q, qlen);
        pos += qlen;
        if (pending_focus) {
            window_t *w = lookup_window(pending_focus);
            pending_focus = 0;
            if (w && window_viewable(w) && !kiosk) set_focus(w, 2);
            else if (w && window_viewable(w) && !focus_window()) set_focus(w, 2);
        }
    }
    if (pos) {
        memmove(c->inbuf, c->inbuf + pos, (size_t)(c->inlen - pos));
        c->inlen -= pos;
    }
}

static void accept_on(int lfd);
static void accept_clients(void) {
    accept_on(listen_fd);
    if (abstract_fd >= 0) accept_on(abstract_fd);
}

static void accept_on(int lfd) {
    int cfd = accept(lfd, 0, 0);
    if (cfd < 0) return;
    fcntl(cfd, F_SETFL, O_RDWR | O_NONBLOCK);
    for (int i = 0; i < MAX_XCLIENTS; i++)
        if (!clients[i].used) {
            memset(&clients[i], 0, sizeof(clients[i]));
            clients[i].inbuf = malloc(INBUF_SIZE);
            if (!clients[i].inbuf) break;
            clients[i].used = 1;
            clients[i].fd = cfd;
            clients[i].index = i;
            return;
        }
    close(cfd);
}

static int start_listener(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM | 0x800 /* NONBLOCK */, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, X_SOCKET_PATH);
    socklen_t alen = (socklen_t)(sizeof(addr.sun_family) + strlen(X_SOCKET_PATH));
    if (mkdir(X_SOCKET_DIR, 01777) == 0) chmod(X_SOCKET_DIR, 01777);
    int probe = socket(AF_UNIX, SOCK_STREAM, 0);
    if (probe >= 0) {
        if (connect(probe, (struct sockaddr *)&addr, alen) != 0) unlink(X_SOCKET_PATH);
        close(probe);
    }
    if (bind(fd, (struct sockaddr *)&addr, alen) != 0) { close(fd); return -2; }
    chmod(X_SOCKET_PATH, 0777);
    if (listen(fd, 16) != 0) { close(fd); return -3; }
    return fd;
}

/* The same display in the abstract namespace, which libxcb tries first: it
 * is not part of the filesystem, so a client inside the Alpine chroot (whose
 * /tmp is the chroot's own) reaches the server through it. */
static int start_abstract_listener(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM | 0x800 /* NONBLOCK */, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path + 1, X_SOCKET_PATH);
    socklen_t alen = (socklen_t)(sizeof(addr.sun_family) + 1 + strlen(X_SOCKET_PATH));
    if (bind(fd, (struct sockaddr *)&addr, alen) != 0 || listen(fd, 16) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void wait_for_clients(int ms) {
    struct pollfd pfd[2 + MAX_XCLIENTS];
    int n = 0;
    if (listen_fd >= 0) { pfd[n].fd = listen_fd; pfd[n].events = POLLIN; pfd[n].revents = 0; n++; }
    if (abstract_fd >= 0) { pfd[n].fd = abstract_fd; pfd[n].events = POLLIN; pfd[n].revents = 0; n++; }
    for (int i = 0; i < MAX_XCLIENTS; i++) {
        client_t *c = &clients[i];
        if (!c->used || c->dead) continue;
        short ev = 0;
        if (c->inlen < INBUF_SIZE) ev |= POLLIN;
        if (c->outlen > 0) ev |= POLLOUT;
        if (!ev) continue;
        pfd[n].fd = c->fd; pfd[n].events = ev; pfd[n].revents = 0; n++;
    }
    poll(pfd, (unsigned long)n, ms);
}

/* The desktop resized our window: the root follows the body. */
static void poll_desktop(void) {
    uint32_t *px = gui.surf.px;
    int w = gui.surf.w, h = gui.surf.h;
    gui_poll(&gui);
    if (gui.surf.px != px || gui.surf.w != w || gui.surf.h != h) {
        if (gui.surf.w > 0 && gui.surf.h > 0 && (gui.surf.w != scr_w || gui.surf.h != scr_h)) {
            scr_w = gui.surf.w;
            scr_h = gui.surf.h;
            core_resize_root(scr_w, scr_h);
            status_clients = -1;
            for (window_t *t = root->bottom; t; t = t->above)
                if (t->wm_max && kiosk) configure_window(t, 0, 0, scr_w, scr_h, t->bw, 1);
        }
        dirty = 1;
    }
}

int main(int argc, char *argv[]) {
    int slot = 1, daemon = 0, gw = 1000, gh = 700;
    const char *logpath = "/tmp/maerox.log";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-H") || !strcmp(argv[i], "--headless")) headless = 1;
        else if (!strcmp(argv[i], "-k")) kiosk = 1;
        else if (!strcmp(argv[i], "-T")) trace_fd = -2;
        else if (!strcmp(argv[i], "-x")) xtrace = 1;
        else if (!strcmp(argv[i], "-D")) { dumpmode = 1; trace_fd = -2; }
        else if (!strcmp(argv[i], "-K") || !strcmp(argv[i], "--test-keys")) test_keys = 1;
        else if (!strcmp(argv[i], "-d") || !strcmp(argv[i], "--daemon")) daemon = 1;
        else if (!strcmp(argv[i], "-g") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &gw, &gh);
        else if (!strcmp(argv[i], "-L") && i + 1 < argc) logpath = argv[++i];
        else if (argv[i][0] >= '0' && argv[i][0] <= '9') slot = atoi(argv[i]);
    }
    if (headless) kiosk = 1;
    if (trace_fd == -2) trace_fd = open("/dev/tty", O_WRONLY);
    log_fd = open(logpath, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
    if (slot < 1 || slot > WM_MAX_SLOTS) slot = 1;
    signal(SIGPIPE, SIG_IGN);

    listen_fd = start_listener();
    if (listen_fd < 0) { printf("maerox: listen failed (%d)\n", listen_fd); return 1; }
    abstract_fd = start_abstract_listener();

    if (daemon) {
        printf("maerox: listening on " X_SOCKET_PATH " (daemonized)\n");
        if (fork() > 0) return 0;
    }
    if (!headless && gui_open(&gui, slot, "maeroX :0", 60 + slot * 12, 40 + slot * 10, gw, gh) < 0)
        headless = 1;
    if (!headless && gui.surf.w > 0) { scr_w = gui.surf.w; scr_h = gui.surf.h; }
    if (headless) { printf("maerox: running headless\n"); kiosk = 1; }
    printf("maerox: listening on " X_SOCKET_PATH " (%dx%d)\n", scr_w, scr_h);
    core_init();
    if (!headless) {
        gui_set_ptr_handler(&gui, on_ptr);
        gui_set_rawkey_handler(&gui, on_rawkey);
        gui_set_scroll_handler(&gui, on_scroll);
        render();
    }
    keyfifo_open();
    xlog("keychannel: %s (%s)\n", keyfifo_fd >= 0 ? "OPEN" : "absent",
         access(KEYFIFO_PATH, F_OK) == 0 ? "node present" : "no node");

    unsigned last_dump = now_ms();
    while (headless || !gui.closed) {
        if (!headless) poll_desktop();
        keyfifo_poll();
        /* The X apps xapp starts are our children: reap them as they exit. */
        while (waitpid(-1, NULL, 1 /* WNOHANG */) > 0) ;
        accept_clients();
        for (int i = 0; i < MAX_XCLIENTS; i++)
            if (clients[i].used) { out_flush(&clients[i]); process_client(&clients[i]); }
        for (int i = 0; i < MAX_XCLIENTS; i++)
            if (clients[i].used && clients[i].dead) client_close(&clients[i]);
        if (!headless && render_due()) render();
        unsigned now = now_ms();
        if (dumpmode && dumps_done < 4 && now - last_dump >= 2000) {
            for (window_t *t = root->bottom; t; t = t->above)
                if (t->mapped && t->d.w >= 400) { frame_dump(); dumps_done++; last_dump = now; break; }
        }
        int wait = 12;
        if (!headless && (dirty || pd_x0 < pd_x1)) {
            unsigned since = now_ms() - last_render_ms;
            int left = since >= FRAME_MS ? 0 : (int)(FRAME_MS - since);
            if (left < wait) wait = left;
        }
        wait_for_clients(wait);
    }
    close(listen_fd);
    if (abstract_fd >= 0) close(abstract_fd);
    unlink(X_SOCKET_PATH);
    if (keyfifo_fd >= 0) { close(keyfifo_fd); unlink(KEYFIFO_PATH); }
    if (!headless) gui_close(&gui);
    return 0;
}
