/*
 * maeroX internals shared by its source files.
 *
 *   maerox.c  connections, the request loop, the window manager, compositing
 *             onto the desktop surface, pointer and keyboard input
 *   xcore.c   core protocol requests: windows, properties, atoms, selections,
 *             events, grabs, focus, colours, keyboard, misc queries
 *   xdraw.c   GCs and core drawing (fills, lines, arcs, polygons, images,
 *             CopyArea/CopyPlane, core text)
 *   xfont.c   the server's core fonts (one built-in bitmap face under XLFD
 *             names) for OpenFont/QueryFont/ListFonts and the text requests
 *   xrender.c the RENDER extension
 *
 * Every window owns a backing buffer of its own (a composited server, as if
 * every window were redirected), so drawing never has to clip against
 * siblings or children and nothing is ever lost when windows overlap:
 * maerox.c composites the tree bottom-up.  Pixmaps and windows share the
 * drawable_t layout: one uint32_t per pixel whatever the depth (depth 1 and 8
 * keep the value in the low bits).
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

#define MAX_XCLIENTS   32
/* Client i owns the ids (i + 1) << 21 .. + 0x1FFFFF (setup reply's base and
 * mask), so every resource id is unique server-wide. */
#define CLIENT_ID_SHIFT 21
#define CLIENT_ID_MASK  0x001FFFFFu
#define ROOT_WINDOW     0x00000001u
#define ROOT_COLORMAP   0x00000020u
#define ROOT_VISUAL     0x00000021u
#define DEFAULT_CURSOR  0x00000022u

#define INBUF_SIZE    (65535 * 4 + 4096)
#define OUTBUF_MAX    (24u << 20)
#define PROP_MAX      (64u << 20)   /* bytes in one window property */

/* Resource types. */
enum { XT_NONE, XT_WINDOW, XT_PIXMAP, XT_GC, XT_FONT, XT_CURSOR, XT_COLORMAP,
       XT_PICTURE, XT_GLYPHSET };

/* Error codes (core protocol). */
enum { BadRequest = 1, BadValue, BadWindow, BadPixmap, BadAtom, BadCursor,
       BadFont, BadMatch, BadDrawable, BadAccess, BadAlloc, BadColor, BadGC,
       BadIDChoice, BadName, BadLength, BadImplementation };

/* Event masks. */
#define KeyPressMask             (1u << 0)
#define KeyReleaseMask           (1u << 1)
#define ButtonPressMask          (1u << 2)
#define ButtonReleaseMask        (1u << 3)
#define EnterWindowMask          (1u << 4)
#define LeaveWindowMask          (1u << 5)
#define PointerMotionMask        (1u << 6)
#define PointerMotionHintMask    (1u << 7)
#define Button1MotionMask        (1u << 8)
#define ButtonMotionMask         (1u << 13)
#define KeymapStateMask          (1u << 14)
#define ExposureMask             (1u << 15)
#define VisibilityChangeMask     (1u << 16)
#define StructureNotifyMask      (1u << 17)
#define ResizeRedirectMask       (1u << 18)
#define SubstructureNotifyMask   (1u << 19)
#define SubstructureRedirectMask (1u << 20)
#define FocusChangeMask          (1u << 21)
#define PropertyChangeMask       (1u << 22)
#define ColormapChangeMask       (1u << 23)
#define OwnerGrabButtonMask      (1u << 24)

/* Event codes. */
enum { KeyPress = 2, KeyRelease, ButtonPress, ButtonRelease, MotionNotify,
       EnterNotify, LeaveNotify, FocusIn, FocusOut, KeymapNotify, Expose,
       GraphicsExpose, NoExpose, VisibilityNotify, CreateNotify, DestroyNotify,
       UnmapNotify, MapNotify, MapRequest, ReparentNotify, ConfigureNotify,
       ConfigureRequest, GravityNotify, ResizeRequest, CirculateNotify,
       CirculateRequest, PropertyNotify, SelectionClear, SelectionRequest,
       SelectionNotify, ColormapNotify, ClientMessage, MappingNotify };

typedef struct { int x, y, w, h; } xrect_t;

typedef struct xobj {
    uint32_t id;
    uint8_t  type;
    int8_t   owner;          /* client index, -1 = the server */
    struct xobj *hnext;      /* resource hash chain */
} xobj_t;

typedef struct drawable {
    xobj_t    o;
    int       w, h, depth;
    uint32_t *px;            /* w*h pixels */
    int       refs;          /* pictures/GCs/windows holding it (+1 while it has an id) */
    int       dead;          /* its id is gone (window destroyed / pixmap freed) */
} drawable_t;

typedef drawable_t pixmap_t;

typedef struct evsel {
    int8_t   client;
    uint32_t mask;
    struct evsel *next;
} evsel_t;

typedef struct prop {
    uint32_t name, type;
    int      format;         /* 8, 16, 32 */
    uint32_t n;              /* items */
    uint8_t *data;
    struct prop *next;
} prop_t;

enum { BG_NONE, BG_PIXEL, BG_PIXMAP, BG_PARENT };

typedef struct window {
    drawable_t d;            /* inner size, backing buffer */
    struct window *parent;
    struct window *bottom, *top;     /* children, stacking order */
    struct window *below, *above;    /* siblings */
    int      x, y, bw;       /* outer corner relative to the parent's inside */
    int      cls;            /* 1 InputOutput, 2 InputOnly */
    uint32_t visual, colormap, cursor;
    int      mapped;
    int      override_redirect;
    int      bg_mode;
    uint32_t bg_pixel;
    pixmap_t *bg_pixmap;
    uint32_t border_pixel;
    uint32_t dont_propagate;
    int      bit_gravity, win_gravity, backing_store, save_under;
    evsel_t *sel;
    prop_t  *props;
    int      painted;        /* drawn into, or has a background */
    unsigned create_seq;
    /* window manager (toplevels) */
    int      wm_placed;      /* placed once on its first map */
    int      wm_max;         /* maximised by the kiosk policy */
} window_t;

typedef struct gc {
    xobj_t   o;
    int      depth;
    int      function;
    uint32_t plane_mask, fg, bg;
    int      line_width, line_style, cap_style, join_style;
    int      fill_style, fill_rule, arc_mode;
    pixmap_t *tile, *stipple;
    int      ts_x, ts_y;
    uint32_t font;
    int      subwindow_mode, graphics_exposures;
    int      clip_x, clip_y;
    pixmap_t *clip_mask;
    int      nclip;          /* -1 = no clip rectangles */
    xrect_t *clip;
    int      dash_offset, ndash;
    uint8_t  dashes[16];
} gc_t;

typedef struct client {
    int      used, fd, setup_done, dead;
    uint8_t *inbuf;
    int      inlen;
    uint16_t seq;
    uint8_t *out;
    size_t   outoff, outlen, outcap;
    uint8_t  cur_major, cur_minor;
    int      index;
    int      close_mode;
    int      big_requests;
} client_t;

/* ── globals (maerox.c) ── */
extern client_t  clients[MAX_XCLIENTS];
extern window_t *root;
extern int       scr_w, scr_h;
extern int       ptr_x, ptr_y;             /* pointer, root coordinates */
extern unsigned  ptr_buttons;              /* X button state bits (Button1Mask...) */
extern unsigned  key_mods;                 /* modifier state */
extern int       kiosk;
extern unsigned  win_seq;
extern int       render_major;

/* ── maerox.c ── */
void     out_write(client_t *c, const void *data, size_t n);
void     x_error(client_t *c, int code, uint32_t bad);
void     send_reply(client_t *c, uint8_t b1, const uint8_t data24[24]);
void     send_reply_var(client_t *c, uint8_t b1, const uint8_t data24[24],
                        const uint8_t *extra, int extra_len);
void     send_event(client_t *c, uint8_t ev[32]);
uint32_t x_time(void);
void     xlog(const char *fmt, ...);
void     damage_window(window_t *w, int x, int y, int ww, int hh);
void     damage_all(void);
void     wm_map_toplevel(client_t *c, window_t *w);
void     wm_window_gone(window_t *w);
void     wm_restacked(void);
int      wm_client_message_to_root(client_t *c, const uint8_t *ev);
void     wm_property_changed(window_t *w, uint32_t atom);
window_t *sprite_window(void);
void     input_window_changed(void);

/* ── xcore.c ── */
void      res_init(void);
xobj_t   *res_lookup(uint32_t id);
void      res_add(xobj_t *o);
void      res_remove(xobj_t *o);
int       res_check_new(client_t *c, uint32_t id);
window_t *lookup_window(uint32_t id);
drawable_t *lookup_drawable(uint32_t id);
gc_t     *lookup_gc(uint32_t id);
void      drawable_unref(drawable_t *d);
int       window_viewable(window_t *w);
void      window_abs(window_t *w, int *ax, int *ay);   /* inside origin */
int       window_is_ancestor(window_t *a, window_t *w);
window_t *window_toplevel(window_t *w);
void      core_dispatch(client_t *c, const uint8_t *q, int qlen);
void      core_client_gone(client_t *c);
void      core_init(void);
uint32_t  atom_intern(const char *name, int len, int only_if_exists);
const char *atom_name(uint32_t id, int *len);
prop_t   *prop_find(window_t *w, uint32_t atom);
void      prop_set(window_t *w, uint32_t name, uint32_t type, int format,
                   const void *data, uint32_t n, int notify);
uint32_t  event_mask_of(window_t *w);
uint32_t  client_mask(window_t *w, int client);
void      deliver_to_window(window_t *w, uint32_t mask, uint8_t ev[32], int set_window_off);
void      deliver_structure(window_t *w, uint8_t ev[32]);
void      send_expose(window_t *w, int x, int y, int ww, int hh);
void      configure_window(window_t *w, int x, int y, int ww, int hh, int bw,
                           int synthetic_too);
void      map_window(client_t *c, window_t *w);
void      unmap_window(window_t *w);
void      destroy_window(window_t *w);
void      raise_window(window_t *w);
void      set_focus(window_t *w, int revert);
window_t *focus_window(void);
void      pointer_moved(void);
void      pointer_button(int button, int press);
void      key_event(int keycode, int press);
void      scroll_event(int delta);
void      kill_client_windows(int ci);
void      send_client_message(window_t *w, uint32_t type, uint32_t d0, uint32_t d1);
int       window_has_protocol(window_t *w, const char *name);
void      fill_background(window_t *w, int x, int y, int ww, int hh);

/* ── xdraw.c ── */
gc_t     *gc_new(uint32_t id, int owner, int depth);
void      gc_free(gc_t *g);
int       gc_change(client_t *c, gc_t *g, uint32_t mask, const uint8_t *v, int nvals);
void      gc_copy(gc_t *dst, gc_t *src, uint32_t mask);
void      draw_dispatch(client_t *c, const uint8_t *q, int qlen);
int       clip_span(int *d, int *s, int *n, int dl, int sl, int *skip);
void      fill32(uint32_t *p, uint32_t v, int n);
void      damage_drawable(drawable_t *d, int x, int y, int w, int h);
void      mark_painted(drawable_t *d);
uint32_t  depth_mask(int depth);

/* ── xfont.c ── */
#define FONT_W       8
#define FONT_H      16
#define FONT_ASCENT 14
#define FONT_DESCENT 2
int       font_open(client_t *c, uint32_t fid, const char *name, int len);
int       font_exists(uint32_t fid);
void      font_query(client_t *c, uint32_t fid);
void      font_text_extents(client_t *c, uint32_t fid, const uint8_t *s, int n16);
void      font_list(client_t *c, const char *pat, int len, int max, int with_info);
const uint8_t *font_glyph(uint32_t fid, unsigned ch, int *bold);
void      font_close(uint32_t fid);
void      font_get_path(client_t *c);

/* ── xrender.c ── */
#define RENDER_ERROR_BASE 142
#define RENDER_EVENT_BASE 0
void      render_dispatch(client_t *c, const uint8_t *q, int qlen);
void      render_free(xobj_t *o);

/* ── byte helpers ── */
static inline uint32_t r16(const uint8_t *p) { return (uint32_t)(p[0] | (p[1] << 8)); }
static inline int      rs16(const uint8_t *p) { return (int)(int16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t r32(const uint8_t *p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
/* a*b*c for a buffer sized from request fields.  size_t is 32 bits here, so
 * 16-bit widths and heights times a pixel size can wrap: returns 0 when the
 * product does not fit, and the caller fails the request instead of
 * allocating the wrapped size. */
static inline int size_mul3(size_t a, size_t b, size_t c, size_t *out) {
    size_t ab;
    return !__builtin_mul_overflow(a, b, &ab) && !__builtin_mul_overflow(ab, c, out);
}
static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }
