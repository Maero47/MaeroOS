/*
 * maeroX core protocol: resources, atoms, properties, the window tree, event
 * delivery, grabs, focus, selections, colours, keyboard and pointer queries,
 * and the core request dispatcher.  Drawing requests go to xdraw.c, fonts to
 * xfont.c, RENDER to xrender.c.
 *
 * Semantics follow the X11 protocol specification (X Window System Protocol,
 * X Consortium Standard, version 11 release 7.7); where maeroX simplifies, the
 * comment says how.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "xs.h"
#include "rgbnames.h"

/* ── resources ───────────────────────────────────────────────────────────── */
#define RES_BUCKETS 4096
static xobj_t *res_hash[RES_BUCKETS];

static unsigned res_bucket(uint32_t id) { return (id ^ (id >> 12) ^ (id >> 21)) & (RES_BUCKETS - 1); }

void res_init(void) { memset(res_hash, 0, sizeof(res_hash)); }

xobj_t *res_lookup(uint32_t id) {
    for (xobj_t *o = res_hash[res_bucket(id)]; o; o = o->hnext)
        if (o->id == id) return o;
    return NULL;
}

void res_add(xobj_t *o) {
    unsigned b = res_bucket(o->id);
    o->hnext = res_hash[b];
    res_hash[b] = o;
}

void res_remove(xobj_t *o) {
    xobj_t **pp = &res_hash[res_bucket(o->id)];
    while (*pp) {
        if (*pp == o) { *pp = o->hnext; o->hnext = NULL; return; }
        pp = &(*pp)->hnext;
    }
}

/* A new id must lie in the client's range and be unused (BadIDChoice). */
int res_check_new(client_t *c, uint32_t id) {
    uint32_t base = (uint32_t)(c->index + 1) << CLIENT_ID_SHIFT;
    if ((id & ~CLIENT_ID_MASK) != base || res_lookup(id)) {
        x_error(c, BadIDChoice, id);
        return 0;
    }
    return 1;
}

window_t *lookup_window(uint32_t id) {
    xobj_t *o = res_lookup(id);
    return (o && o->type == XT_WINDOW) ? (window_t *)o : NULL;
}

drawable_t *lookup_drawable(uint32_t id) {
    xobj_t *o = res_lookup(id);
    return (o && (o->type == XT_WINDOW || o->type == XT_PIXMAP)) ? (drawable_t *)o : NULL;
}

gc_t *lookup_gc(uint32_t id) {
    xobj_t *o = res_lookup(id);
    return (o && o->type == XT_GC) ? (gc_t *)o : NULL;
}

void drawable_unref(drawable_t *d) {
    if (!d || --d->refs > 0) return;
    if (d->o.type == XT_WINDOW) {
        window_t *w = (window_t *)d;
        free(w->d.px);
        free(w);
        return;
    }
    free(d->px);
    free(d);
}

/* ── atoms ───────────────────────────────────────────────────────────────── */
static const char *const predefined_atoms[] = {
    NULL, "PRIMARY", "SECONDARY", "ARC", "ATOM", "BITMAP", "CARDINAL",
    "COLORMAP", "CURSOR", "CUT_BUFFER0", "CUT_BUFFER1", "CUT_BUFFER2",
    "CUT_BUFFER3", "CUT_BUFFER4", "CUT_BUFFER5", "CUT_BUFFER6", "CUT_BUFFER7",
    "DRAWABLE", "FONT", "INTEGER", "PIXMAP", "POINT", "RECTANGLE",
    "RESOURCE_MANAGER", "RGB_COLOR_MAP", "RGB_BEST_MAP", "RGB_BLUE_MAP",
    "RGB_DEFAULT_MAP", "RGB_GRAY_MAP", "RGB_GREEN_MAP", "RGB_RED_MAP", "STRING",
    "VISUALID", "WINDOW", "WM_COMMAND", "WM_HINTS", "WM_CLIENT_MACHINE",
    "WM_ICON_NAME", "WM_ICON_SIZE", "WM_NAME", "WM_NORMAL_HINTS",
    "WM_SIZE_HINTS", "WM_ZOOM_HINTS", "MIN_SPACE", "NORM_SPACE", "MAX_SPACE",
    "END_SPACE", "SUPERSCRIPT_X", "SUPERSCRIPT_Y", "SUBSCRIPT_X", "SUBSCRIPT_Y",
    "UNDERLINE_POSITION", "UNDERLINE_THICKNESS", "STRIKEOUT_ASCENT",
    "STRIKEOUT_DESCENT", "ITALIC_ANGLE", "X_HEIGHT", "QUAD_WIDTH", "WEIGHT",
    "POINT_SIZE", "RESOLUTION", "COPYRIGHT", "NOTICE", "FONT_NAME",
    "FAMILY_NAME", "FULL_NAME", "CAP_HEIGHT", "WM_CLASS", "WM_TRANSIENT_FOR",
};
#define N_PREDEFINED 69

static char   **atom_names;
static uint32_t natoms, atom_cap;       /* natoms = next id */
#define ATOM_BUCKETS 1024
static uint32_t *atom_chain;            /* next id in the same bucket */
static uint32_t  atom_bucket_head[ATOM_BUCKETS];

static unsigned atom_hash(const char *s, int len) {
    unsigned h = 5381;
    for (int i = 0; i < len; i++) h = h * 33 + (uint8_t)s[i];
    return h & (ATOM_BUCKETS - 1);
}

static uint32_t atom_add(const char *name, int len) {
    if (natoms >= atom_cap) {
        uint32_t cap = atom_cap ? atom_cap * 2 : 1024;
        char **nn = realloc(atom_names, cap * sizeof(char *));
        uint32_t *nc = realloc(atom_chain, cap * sizeof(uint32_t));
        if (!nn || !nc) return 0;
        atom_names = nn; atom_chain = nc; atom_cap = cap;
    }
    char *s = malloc((size_t)len + 1);
    if (!s) return 0;
    memcpy(s, name, (size_t)len);
    s[len] = 0;
    uint32_t id = natoms++;
    atom_names[id] = s;
    unsigned b = atom_hash(name, len);
    atom_chain[id] = atom_bucket_head[b];
    atom_bucket_head[b] = id;
    return id;
}

uint32_t atom_intern(const char *name, int len, int only_if_exists) {
    if (len <= 0) return 0;
    for (uint32_t id = atom_bucket_head[atom_hash(name, len)]; id; id = atom_chain[id])
        if ((int)strlen(atom_names[id]) == len && !memcmp(atom_names[id], name, (size_t)len))
            return id;
    if (only_if_exists) return 0;
    return atom_add(name, len);
}

const char *atom_name(uint32_t id, int *len) {
    if (id == 0 || id >= natoms || !atom_names[id]) { *len = 0; return NULL; }
    *len = (int)strlen(atom_names[id]);
    return atom_names[id];
}

static uint32_t A(const char *name) { return atom_intern(name, (int)strlen(name), 0); }

/* ── properties ──────────────────────────────────────────────────────────── */
prop_t *prop_find(window_t *w, uint32_t atom) {
    for (prop_t *p = w->props; p; p = p->next)
        if (p->name == atom) return p;
    return NULL;
}

static void property_notify(window_t *w, uint32_t atom, int deleted) {
    uint8_t e[32];
    memset(e, 0, sizeof(e));
    e[0] = PropertyNotify;
    put32(e + 4, w->d.o.id);
    put32(e + 8, atom);
    put32(e + 12, x_time());
    e[16] = (uint8_t)(deleted ? 1 : 0);
    deliver_to_window(w, PropertyChangeMask, e, -1);
}

void prop_set(window_t *w, uint32_t name, uint32_t type, int format,
              const void *data, uint32_t n, int notify) {
    prop_t *p = prop_find(w, name);
    size_t bytes = (size_t)n * (size_t)(format / 8);
    uint8_t *nd = malloc(bytes ? bytes : 1);
    if (!nd) return;
    if (bytes) memcpy(nd, data, bytes);
    if (!p) {
        p = calloc(1, sizeof(*p));
        if (!p) { free(nd); return; }
        p->name = name;
        p->next = w->props;
        w->props = p;
    } else {
        free(p->data);
    }
    p->type = type; p->format = format; p->n = n; p->data = nd;
    if (notify) property_notify(w, name, 0);
    wm_property_changed(w, name);
}

static void prop_delete(window_t *w, uint32_t name) {
    for (prop_t **pp = &w->props; *pp; pp = &(*pp)->next)
        if ((*pp)->name == name) {
            prop_t *p = *pp;
            *pp = p->next;
            free(p->data);
            free(p);
            property_notify(w, name, 1);
            wm_property_changed(w, name);
            return;
        }
}

/* ── the window tree ─────────────────────────────────────────────────────── */
window_t *root;
unsigned  win_seq;

int window_viewable(window_t *w) {
    for (; w; w = w->parent)
        if (!w->mapped) return 0;
    return 1;
}

void window_abs(window_t *w, int *ax, int *ay) {
    int x = 0, y = 0;
    for (; w && w != root; w = w->parent) { x += w->x + w->bw; y += w->y + w->bw; }
    *ax = x; *ay = y;
}

int window_is_ancestor(window_t *a, window_t *w) {     /* a is w or above w */
    for (; w; w = w->parent) if (w == a) return 1;
    return 0;
}

window_t *window_toplevel(window_t *w) {
    while (w && w->parent && w->parent != root) w = w->parent;
    return (w && w != root) ? w : NULL;
}

static void unlink_sibling(window_t *w) {
    window_t *p = w->parent;
    if (!p) return;
    if (w->below) w->below->above = w->above; else p->bottom = w->above;
    if (w->above) w->above->below = w->below; else p->top = w->below;
    w->above = w->below = NULL;
}

static void link_top(window_t *p, window_t *w) {
    w->parent = p;
    w->below = p->top; w->above = NULL;
    if (p->top) p->top->above = w; else p->bottom = w;
    p->top = w;
}

static void link_above(window_t *w, window_t *sib) {     /* w just above sib */
    window_t *p = sib->parent;
    w->parent = p;
    w->below = sib; w->above = sib->above;
    if (sib->above) sib->above->below = w; else p->top = w;
    sib->above = w;
}

static void link_below(window_t *w, window_t *sib) {
    window_t *p = sib->parent;
    w->parent = p;
    w->above = sib; w->below = sib->below;
    if (sib->below) sib->below->above = w; else p->bottom = w;
    sib->below = w;
}

/* Which client gets events selected with `mask` on w. */
uint32_t client_mask(window_t *w, int client) {
    for (evsel_t *s = w->sel; s; s = s->next) if (s->client == client) return s->mask;
    return 0;
}

uint32_t event_mask_of(window_t *w) {
    uint32_t m = 0;
    for (evsel_t *s = w->sel; s; s = s->next) m |= s->mask;
    return m;
}

static void select_input(window_t *w, int client, uint32_t mask) {
    evsel_t **pp = &w->sel;
    for (; *pp; pp = &(*pp)->next)
        if ((*pp)->client == client) {
            if (mask) { (*pp)->mask = mask; return; }
            evsel_t *s = *pp; *pp = s->next; free(s); return;
        }
    if (!mask) return;
    evsel_t *s = calloc(1, sizeof(*s));
    if (!s) return;
    s->client = (int8_t)client; s->mask = mask;
    *pp = s;
}

/* Send ev (a copy per client) to every client that selected any bit of mask
 * on w.  If win_off >= 0 the event window field at that offset is set to w. */
void deliver_to_window(window_t *w, uint32_t mask, uint8_t ev[32], int win_off) {
    for (evsel_t *s = w->sel; s; s = s->next) {
        if (!(s->mask & mask)) continue;
        client_t *c = &clients[(int)s->client];
        if (!c->used || c->dead) continue;
        uint8_t e[32];
        memcpy(e, ev, 32);
        if (win_off >= 0) put32(e + win_off, w->d.o.id);
        send_event(c, e);
    }
}

/* Structure events: StructureNotify on the window itself, SubstructureNotify
 * on its parent; offset 4 is the event window, 8 the window. */
void deliver_structure(window_t *w, uint8_t ev[32]) {
    put32(ev + 8, w->d.o.id);
    deliver_to_window(w, StructureNotifyMask, ev, 4);
    if (w->parent) deliver_to_window(w->parent, SubstructureNotifyMask, ev, 4);
}

void send_expose(window_t *w, int x, int y, int ww, int hh) {
    if (w->cls == 2) return;
    uint8_t e[32];
    memset(e, 0, sizeof(e));
    e[0] = Expose;
    put32(e + 4, w->d.o.id);
    put16(e + 8, (uint32_t)x); put16(e + 10, (uint32_t)y);
    put16(e + 12, (uint32_t)ww); put16(e + 14, (uint32_t)hh);
    deliver_to_window(w, ExposureMask, e, -1);
}

void fill_background(window_t *w, int x, int y, int ww, int hh) {
    if (!w->d.px) return;
    if (!clip_span(&x, NULL, &ww, w->d.w, 0, NULL) ||
        !clip_span(&y, NULL, &hh, w->d.h, 0, NULL)) return;
    if (w->bg_mode == BG_PIXEL) {
        for (int yy = y; yy < y + hh; yy++)
            fill32(w->d.px + (size_t)yy * w->d.w + x, w->bg_pixel & 0xFFFFFF, ww);
    } else if (w->bg_mode == BG_PIXMAP && w->bg_pixmap && w->bg_pixmap->px) {
        pixmap_t *t = w->bg_pixmap;
        for (int yy = y; yy < y + hh; yy++) {
            const uint32_t *tr = t->px + (size_t)(yy % t->h) * t->w;
            uint32_t *dr = w->d.px + (size_t)yy * w->d.w;
            for (int xx = x; xx < x + ww; xx++) dr[xx] = tr[xx % t->w] & 0xFFFFFF;
        }
    } else if (w->bg_mode == BG_PARENT && w->parent && w->parent->d.px) {
        /* ParentRelative: the parent's pixels behind the window. */
        window_t *p = w->parent;
        int ox = w->x + w->bw, oy = w->y + w->bw;
        for (int yy = y; yy < y + hh; yy++) {
            int py = yy + oy;
            if (py < 0 || py >= p->d.h) continue;
            for (int xx = x; xx < x + ww; xx++) {
                int px = xx + ox;
                if (px >= 0 && px < p->d.w)
                    w->d.px[(size_t)yy * w->d.w + xx] = p->d.px[(size_t)py * p->d.w + px];
            }
        }
    } else {
        return;
    }
    w->painted = 1;
    damage_window(w, x, y, ww, hh);
}

static window_t *window_new(uint32_t id, int owner, window_t *parent, int x, int y,
                            int ww, int hh, int bw, int cls) {
    window_t *w = calloc(1, sizeof(*w));
    if (!w) return NULL;
    w->d.o.id = id; w->d.o.type = XT_WINDOW; w->d.o.owner = (int8_t)owner;
    if (ww < 1) ww = 1;
    if (hh < 1) hh = 1;
    if (ww > 8192) ww = 8192;
    if (hh > 8192) hh = 8192;
    w->d.w = ww; w->d.h = hh; w->d.depth = 24; w->d.refs = 1;
    w->x = x; w->y = y; w->bw = bw; w->cls = cls;
    w->visual = ROOT_VISUAL; w->colormap = ROOT_COLORMAP;
    w->win_gravity = 1; w->bg_mode = BG_NONE;
    w->create_seq = ++win_seq;
    if (cls != 2) {
        w->d.px = malloc((size_t)ww * hh * 4);
        if (!w->d.px) { free(w); return NULL; }
        memset(w->d.px, 0, (size_t)ww * hh * 4);
    }
    if (parent) link_top(parent, w);
    return w;
}

static void notify_viewable(window_t *w) {
    /* w just became viewable: paint the backgrounds and ask for contents,
     * parents before children. */
    if (!w->mapped) return;
    if (w->bg_mode != BG_NONE) fill_background(w, 0, 0, w->d.w, w->d.h);
    send_expose(w, 0, 0, w->d.w, w->d.h);
    for (window_t *ch = w->bottom; ch; ch = ch->above) notify_viewable(ch);
}

static void damage_outer(window_t *w) {
    damage_window(w, -w->bw, -w->bw, w->d.w + 2 * w->bw, w->d.h + 2 * w->bw);
}

void map_window(client_t *c, window_t *w) {
    if (w == root || w->mapped) return;
    if (w->parent == root && !w->override_redirect) wm_map_toplevel(c, w);
    w->mapped = 1;
    uint8_t e[32];
    memset(e, 0, sizeof(e));
    e[0] = MapNotify;
    e[12] = (uint8_t)w->override_redirect;
    deliver_structure(w, e);
    if (window_viewable(w)) {
        notify_viewable(w);
        damage_outer(w);
        input_window_changed();
    }
}

static void focus_lost_check(window_t *w);

void unmap_window(window_t *w) {
    if (w == root || !w->mapped) return;
    int was_viewable = window_viewable(w);
    if (was_viewable) damage_outer(w);
    w->mapped = 0;
    uint8_t e[32];
    memset(e, 0, sizeof(e));
    e[0] = UnmapNotify;
    e[12] = 0;                           /* from-configure */
    deliver_structure(w, e);
    if (was_viewable) {
        focus_lost_check(w);
        input_window_changed();
    }
}

static void send_configure_notify(window_t *w) {
    uint8_t e[32];
    memset(e, 0, sizeof(e));
    e[0] = ConfigureNotify;
    put32(e + 12, w->below ? w->below->d.o.id : 0);
    put16(e + 16, (uint32_t)w->x); put16(e + 18, (uint32_t)w->y);
    put16(e + 20, (uint32_t)w->d.w); put16(e + 22, (uint32_t)w->d.h);
    put16(e + 24, (uint32_t)w->bw);
    e[26] = (uint8_t)w->override_redirect;
    deliver_structure(w, e);
}

/* The synthetic ConfigureNotify a window manager sends a toplevel after
 * moving it (ICCCM 4.1.5): root coordinates, send_event bit set, only to the
 * window's own StructureNotify selectors. */
static void send_synthetic_configure(window_t *w) {
    uint8_t e[32];
    int ax, ay;
    window_abs(w, &ax, &ay);
    memset(e, 0, sizeof(e));
    e[0] = ConfigureNotify | 0x80;
    put32(e + 4, w->d.o.id);
    put32(e + 8, w->d.o.id);
    put16(e + 16, (uint32_t)(ax - w->bw)); put16(e + 18, (uint32_t)(ay - w->bw));
    put16(e + 20, (uint32_t)w->d.w); put16(e + 22, (uint32_t)w->d.h);
    put16(e + 24, (uint32_t)w->bw);
    deliver_to_window(w, StructureNotifyMask, e, -1);
}

static int resize_backing(window_t *w, int nw, int nh) {
    if (nw < 1) nw = 1;
    if (nh < 1) nh = 1;
    if (nw > 8192) nw = 8192;
    if (nh > 8192) nh = 8192;
    if (nw == w->d.w && nh == w->d.h) return 0;
    if (w->cls != 2) {
        uint32_t *np = malloc((size_t)nw * nh * 4);
        if (!np) return 0;
        memset(np, 0, (size_t)nw * nh * 4);
        if (w->d.px) {
            /* Keep the old contents at the top left (NorthWest bit gravity,
             * what toolkits that redraw on Expose all accept). */
            int cw = imin(w->d.w, nw), ch = imin(w->d.h, nh);
            for (int y = 0; y < ch; y++)
                memcpy(np + (size_t)y * nw, w->d.px + (size_t)y * w->d.w, (size_t)cw * 4);
            free(w->d.px);
        }
        w->d.px = np;
    }
    int ow = w->d.w, oh = w->d.h;
    w->d.w = nw; w->d.h = nh;
    if (w->bg_mode != BG_NONE) {
        if (nw > ow) fill_background(w, ow, 0, nw - ow, nh);
        if (nh > oh) fill_background(w, 0, oh, imin(ow, nw), nh - oh);
    }
    return 1;
}

void configure_window(window_t *w, int x, int y, int ww, int hh, int bw, int synthetic) {
    int viewable = window_viewable(w);
    if (viewable) damage_outer(w);
    int moved = (x != w->x || y != w->y);
    w->x = x; w->y = y; w->bw = bw;
    int resized = resize_backing(w, ww, hh);
    send_configure_notify(w);
    if (synthetic && w->parent == root) send_synthetic_configure(w);
    if (viewable) {
        damage_outer(w);
        if (resized) send_expose(w, 0, 0, w->d.w, w->d.h);
        if (resized || moved) input_window_changed();
    }
}

/* Restack w relative to sibling sib (or among all siblings when sib is
 * NULL): 0 Above, 1 Below, 2 TopIf, 3 BottomIf, 4 Opposite.  TopIf/BottomIf/
 * Opposite are treated as Above/Below/toggle without the occlusion test. */
static void restack(window_t *w, window_t *sib, int mode) {
    if (!w->parent) return;
    if (sib && (sib == w || sib->parent != w->parent)) return;
    if (mode == 4) mode = (w->above == NULL) ? 1 : 0;
    if (mode == 2) mode = 0;
    if (mode == 3) mode = 1;
    unlink_sibling(w);
    if (mode == 0) {
        if (sib) link_above(w, sib); else link_top(w->parent, w);
    } else {
        if (sib) link_below(w, sib);
        else {
            window_t *p = w->parent;
            w->parent = p;
            w->above = p->bottom; w->below = NULL;
            if (p->bottom) p->bottom->below = w; else p->top = w;
            p->bottom = w;
        }
    }
    if (window_viewable(w)) { damage_outer(w); input_window_changed(); }
    if (w->parent == root) wm_restacked();
}

void raise_window(window_t *w) {
    if (w->parent && w->parent->top != w) {
        restack(w, NULL, 0);
        send_configure_notify(w);
    }
}

/* ── selections ──────────────────────────────────────────────────────────── */
#define MAX_SELECTIONS 64
static struct { uint32_t atom, window, time; int client; } sels[MAX_SELECTIONS];
static int nsels;

static void selection_clear_window(window_t *w) {
    for (int i = 0; i < nsels; i++)
        if (sels[i].window == w->d.o.id) { sels[i].window = 0; sels[i].client = -1; }
}

/* ── focus ───────────────────────────────────────────────────────────────── */
static int       focus_mode = 1;      /* 0 None, 1 PointerRoot, 2 window */
static window_t *focus_win;
static int       focus_revert = 1;

window_t *focus_window(void) { return focus_mode == 2 ? focus_win : NULL; }

static void focus_event(window_t *w, int type, int detail, int mode) {
    uint8_t e[32];
    memset(e, 0, sizeof(e));
    e[0] = (uint8_t)type;
    e[1] = (uint8_t)detail;
    put32(e + 4, w->d.o.id);
    e[8] = (uint8_t)mode;
    deliver_to_window(w, FocusChangeMask, e, -1);
}

/* FocusOut on the old window and FocusIn on the new, Nonlinear (3) — the
 * detail a toolkit sees when a window manager moves the focus between
 * unrelated toplevels.  The virtual events on the windows in between are
 * not generated. */
void set_focus(window_t *w, int revert) {
    window_t *old = focus_mode == 2 ? focus_win : NULL;
    focus_revert = revert;
    if (w == NULL) { focus_mode = 0; focus_win = NULL; }
    else if (w == (window_t *)1) { focus_mode = 1; focus_win = NULL; w = NULL; }
    else { focus_mode = 2; }
    if (old == w && w) return;
    if (old) focus_event(old, FocusOut, 3, 0);
    if (w) { focus_win = w; focus_event(w, FocusIn, 3, 0); }
    xlog("focus -> 0x%x\n", w ? (unsigned)w->d.o.id : (unsigned)focus_mode);
}

static window_t *wm_pick_focus(window_t *except) {
    for (window_t *t = root->top; t; t = t->below)
        if (t != except && t->mapped && !t->override_redirect && t->cls != 2 &&
            t->d.w > 1 && t->d.h > 1)
            return t;
    return NULL;
}

/* The focus window (or an ancestor of it) became unviewable: revert. */
static void focus_lost_check(window_t *gone) {
    if (focus_mode != 2 || !focus_win) return;
    if (!window_is_ancestor(gone, focus_win)) return;
    window_t *nf = NULL;
    if (focus_revert == 2) {                       /* RevertToParent */
        for (window_t *p = gone->parent; p && p != root; p = p->parent)
            if (window_viewable(p)) { nf = p; break; }
    }
    if (!nf && focus_revert != 0) nf = wm_pick_focus(window_toplevel(gone));
    focus_win = NULL;
    focus_mode = nf ? 2 : (focus_revert == 1 ? 1 : 0);
    if (nf) { focus_win = nf; focus_event(nf, FocusIn, 3, 0); }
}

/* ── pointer and keyboard ────────────────────────────────────────────────── */
int      ptr_x, ptr_y;
unsigned ptr_buttons, key_mods;
static window_t *sprite;               /* deepest viewable window under the pointer */

static struct {
    window_t *win;
    int       client;
    uint32_t  mask;
    int       owner_events;
    int       implicit;
} pgrab;

static struct {
    window_t *win;
    int       client;
    int       owner_events;
} kgrab;

static uint8_t keys_down[32];

static window_t *window_at(window_t *w, int x, int y) {
    /* x, y relative to w's inside */
    for (window_t *ch = w->top; ch; ch = ch->below) {
        if (!ch->mapped) continue;
        int cx = x - ch->x - ch->bw, cy = y - ch->y - ch->bw;
        if (cx >= -ch->bw && cy >= -ch->bw && cx < ch->d.w + ch->bw && cy < ch->d.h + ch->bw) {
            if (cx < 0 || cy < 0 || cx >= ch->d.w || cy >= ch->d.h) return ch;   /* border */
            return window_at(ch, cx, cy);
        }
    }
    return w;
}

window_t *sprite_window(void) {
    return window_at(root, ptr_x, ptr_y);
}

static uint32_t state_mask(void) { return key_mods | ptr_buttons; }

static void fill_device(uint8_t *e, int type, int detail, window_t *ew, window_t *child) {
    int ax, ay;
    window_abs(ew, &ax, &ay);
    memset(e, 0, 32);
    e[0] = (uint8_t)type;
    e[1] = (uint8_t)detail;
    put32(e + 4, x_time());
    put32(e + 8, ROOT_WINDOW);
    put32(e + 12, ew->d.o.id);
    put32(e + 16, child ? child->d.o.id : 0);
    put16(e + 20, (uint32_t)ptr_x); put16(e + 22, (uint32_t)ptr_y);
    put16(e + 24, (uint32_t)(ptr_x - ax)); put16(e + 26, (uint32_t)(ptr_y - ay));
    put16(e + 28, state_mask());
    e[30] = 1;
}

/* Deliver a device event from src upwards (stopping after `stop`, or at the
 * root) to the first window where some client selected `mask`; only clients
 * >= 0 restricts it to one client.  Returns the window it went to. */
static window_t *deliver_device(window_t *src, uint32_t mask, int type, int detail,
                                window_t *stop, int only, int *to_client) {
    window_t *child = NULL;
    for (window_t *w = src; w; child = w, w = w->parent) {
        int sent = 0;
        for (evsel_t *s = w->sel; s; s = s->next) {
            if (!(s->mask & mask)) continue;
            if (only >= 0 && s->client != only) continue;
            client_t *c = &clients[(int)s->client];
            if (!c->used || c->dead) continue;
            uint8_t e[32];
            fill_device(e, type, detail, w, child);
            if (type == MotionNotify && (s->mask & PointerMotionHintMask)) e[1] = 1;
            send_event(c, e);
            if (to_client) *to_client = s->client;
            sent = 1;
        }
        if (sent) return w;
        if (w->dont_propagate & mask) return NULL;
        if (w == stop) return NULL;
    }
    return NULL;
}

/* Send to one client on one window regardless of propagation (grabs). */
static void deliver_grabbed(window_t *w, int client, int type, int detail) {
    client_t *c = &clients[client];
    if (!c->used || c->dead || !w) return;
    window_t *child = NULL;
    if (sprite && sprite != w && window_is_ancestor(w, sprite))
        for (child = sprite; child->parent != w; child = child->parent) ;
    uint8_t e[32];
    fill_device(e, type, detail, w, child);
    send_event(c, e);
}

static uint32_t motion_mask(void) {
    uint32_t m = PointerMotionMask;
    if (ptr_buttons & 0x1F00) {
        m |= ButtonMotionMask;
        for (int b = 0; b < 5; b++)
            if (ptr_buttons & (0x100u << b)) m |= Button1MotionMask << b;
    }
    return m;
}

static void pointer_event(int type, int detail, uint32_t mask) {
    if (pgrab.win) {
        if (pgrab.owner_events && sprite &&
            deliver_device(sprite, mask, type, detail, NULL, pgrab.client, NULL))
            return;
        if (pgrab.mask & mask) deliver_grabbed(pgrab.win, pgrab.client, type, detail);
        return;
    }
    if (sprite) deliver_device(sprite, mask, type, detail, NULL, -1, NULL);
}

/* Crossing events, as in the protocol's "Pointer Window" section:
 * Ancestor/Inferior/Nonlinear on the end windows, the Virtual kinds on the
 * windows in between. */
static void crossing(window_t *w, int type, int detail, window_t *child) {
    uint32_t mask = type == EnterNotify ? EnterWindowMask : LeaveWindowMask;
    if (!(event_mask_of(w) & mask)) return;
    uint8_t e[32];
    fill_device(e, type, detail, w, child);
    e[30] = 0;                               /* mode Normal */
    e[31] = (uint8_t)(2 | ((focus_mode == 2 && focus_win == w) ? 1 : 0));
    deliver_to_window(w, mask, e, -1);
}

static void pointer_crossing(window_t *from, window_t *to) {
    if (from == to) return;
    if (!from) { crossing(to, EnterNotify, 3, NULL); return; }
    if (!to) { crossing(from, LeaveNotify, 3, NULL); return; }
    if (window_is_ancestor(to, from)) {              /* to is above from */
        crossing(from, LeaveNotify, 0, NULL);        /* Ancestor */
        window_t *ch = from;
        for (window_t *w = from->parent; w && w != to; ch = w, w = w->parent)
            crossing(w, LeaveNotify, 1, ch);        /* Virtual */
        for (ch = from; ch->parent != to; ch = ch->parent) ;
        crossing(to, EnterNotify, 2, ch);            /* Inferior */
    } else if (window_is_ancestor(from, to)) {       /* to is below from */
        window_t *ch;
        for (ch = to; ch->parent != from; ch = ch->parent) ;
        crossing(from, LeaveNotify, 2, ch);          /* Inferior */
        window_t *path[64]; int n = 0;
        for (window_t *w = to->parent; w && w != from && n < 64; w = w->parent) path[n++] = w;
        for (int i = n - 1; i >= 0; i--) crossing(path[i], EnterNotify, 1, NULL);
        crossing(to, EnterNotify, 0, NULL);
    } else {
        window_t *common = from->parent;
        while (common && !window_is_ancestor(common, to)) common = common->parent;
        crossing(from, LeaveNotify, 3, NULL);        /* Nonlinear */
        for (window_t *w = from->parent; w && w != common; w = w->parent)
            crossing(w, LeaveNotify, 4, NULL);       /* NonlinearVirtual */
        window_t *path[64]; int n = 0;
        for (window_t *w = to->parent; w && w != common && n < 64; w = w->parent) path[n++] = w;
        for (int i = n - 1; i >= 0; i--) crossing(path[i], EnterNotify, 4, NULL);
        crossing(to, EnterNotify, 3, NULL);
    }
}

/* The window under the pointer may have changed (map, unmap, move,
 * restack, destroy, or the pointer moved). */
void input_window_changed(void) {
    window_t *now = sprite_window();
    if (now != sprite) {
        window_t *old = sprite;
        sprite = now;
        pointer_crossing(old, now);
    }
}

void pointer_moved(void) {
    input_window_changed();
    pointer_event(MotionNotify, 0, motion_mask());
}

void pointer_button(int button, int press) {
    uint32_t bit = 0x80u << button;          /* Button1Mask = 1 << 8 */
    sprite = sprite_window();
    if (press) {
        if (!pgrab.win && sprite) {
            int to = -1;
            window_t *w = deliver_device(sprite, ButtonPressMask, ButtonPress, button,
                                         NULL, -1, &to);
            if (w && to >= 0) {
                /* Implicit grab: the rest of this press goes to w. */
                pgrab.win = w; pgrab.client = to;
                pgrab.mask = client_mask(w, to);
                pgrab.owner_events = (pgrab.mask & OwnerGrabButtonMask) != 0;
                pgrab.implicit = 1;
            }
        } else if (pgrab.win) {
            pointer_event(ButtonPress, button, ButtonPressMask);
        }
        ptr_buttons |= bit;
    } else {
        ptr_buttons &= ~bit;
        pointer_event(ButtonRelease, button, ButtonReleaseMask);
        if (pgrab.win && pgrab.implicit && !(ptr_buttons & 0x1F00)) {
            pgrab.win = NULL;
            input_window_changed();
        }
    }
}

/* Wheel notches are buttons 4 (up) and 5 (down), pressed and released. */
void scroll_event(int delta) {
    int b = delta > 0 ? 4 : 5;
    for (int i = 0; i < (delta > 0 ? delta : -delta) && i < 10; i++) {
        pointer_button(b, 1);
        pointer_button(b, 0);
    }
}

/* The pairing rule (the desktop applies the same one): a KeyRelease goes to
 * the window its KeyPress went to, even if the focus moved in between, and a
 * release whose press was not delivered is dropped — so no client is left
 * holding a key, or handed a release for a key it never saw. */
static uint32_t key_press_win[256];

void key_event(int keycode, int press) {
    if (press) keys_down[keycode >> 3] |= (uint8_t)(1u << (keycode & 7));
    else keys_down[keycode >> 3] &= (uint8_t)~(1u << (keycode & 7));
    sprite = sprite_window();
    if (!press) {
        window_t *w = key_press_win[keycode] ? lookup_window(key_press_win[keycode]) : NULL;
        key_press_win[keycode] = 0;
        if (w) deliver_device(w, KeyReleaseMask, KeyRelease, keycode, NULL, -1, NULL);
        return;
    }
    window_t *to = NULL;
    if (kgrab.win) {
        if (kgrab.owner_events && focus_mode == 2 && focus_win &&
            focus_win->d.o.owner == kgrab.client) {
            window_t *src = (sprite && window_is_ancestor(focus_win, sprite)) ? sprite : focus_win;
            to = deliver_device(src, KeyPressMask, KeyPress, keycode, focus_win, kgrab.client, NULL);
        }
        if (!to) {
            deliver_grabbed(kgrab.win, kgrab.client, KeyPress, keycode);
            to = kgrab.win;
        }
    } else if (focus_mode != 0) {
        window_t *src = NULL, *stop = NULL;
        if (focus_mode == 1) src = sprite;
        else {
            stop = focus_win;
            src = (sprite && window_is_ancestor(focus_win, sprite)) ? sprite : focus_win;
        }
        if (src) to = deliver_device(src, KeyPressMask, KeyPress, keycode, stop, -1, NULL);
    }
    key_press_win[keycode] = to ? to->d.o.id : 0;
}

/* ── window destruction ──────────────────────────────────────────────────── */
static void destroy_one(window_t *w) {
    /* children first (bottom-up), DestroyNotify on each */
    while (w->top) destroy_one(w->top);
    uint8_t e[32];
    memset(e, 0, sizeof(e));
    e[0] = DestroyNotify;
    deliver_structure(w, e);
    if (sprite == w) sprite = NULL;
    if (pgrab.win == w) pgrab.win = NULL;
    if (kgrab.win == w) kgrab.win = NULL;
    if (focus_mode == 2 && focus_win == w) { focus_win = NULL; focus_mode = 1; }
    selection_clear_window(w);
    wm_window_gone(w);
    unlink_sibling(w);
    w->parent = NULL;
    res_remove(&w->d.o);
    w->d.dead = 1;
    while (w->props) {
        prop_t *p = w->props; w->props = p->next;
        free(p->data); free(p);
    }
    while (w->sel) { evsel_t *s = w->sel; w->sel = s->next; free(s); }
    if (w->bg_pixmap) { drawable_unref(w->bg_pixmap); w->bg_pixmap = NULL; }
    free(w->d.px);
    w->d.px = NULL;
    drawable_unref(&w->d);
}

void destroy_window(window_t *w) {
    if (w == root) return;
    if (w->mapped) unmap_window(w);
    destroy_one(w);
    input_window_changed();
}

void kill_client_windows(int ci) {
    clients[ci].dead = 1;
}

/* ── client messages ─────────────────────────────────────────────────────── */
void send_client_message(window_t *w, uint32_t type, uint32_t d0, uint32_t d1) {
    if (w->d.o.owner < 0) return;
    client_t *c = &clients[(int)w->d.o.owner];
    if (!c->used || c->dead) return;
    uint8_t e[32];
    memset(e, 0, sizeof(e));
    e[0] = ClientMessage;
    e[1] = 32;
    put32(e + 4, w->d.o.id);
    put32(e + 8, type);
    put32(e + 12, d0);
    put32(e + 16, d1);
    send_event(c, e);
}

int window_has_protocol(window_t *w, const char *name) {
    prop_t *p = prop_find(w, A("WM_PROTOCOLS"));
    uint32_t a = atom_intern(name, (int)strlen(name), 1);
    if (!p || !a || p->format != 32) return 0;
    for (uint32_t i = 0; i < p->n; i++)
        if (r32(p->data + i * 4) == a) return 1;
    return 0;
}

/* ── colours ─────────────────────────────────────────────────────────────── */
static int hexval(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static int cmp_rgbname(const void *k, const void *e) {
    return strcmp((const char *)k, *(const char *const *)e);
}

/* A colour name (rgb.txt names, case and spaces ignored, "#rgb..." and
 * "rgb:r/g/b") to 0xRRGGBB, or -1. */
static long color_lookup(const char *s, int len) {
    char key[64];
    int n = 0;
    if (len <= 0 || len > 60) return -1;
    if (s[0] == '#') {
        int digits = len - 1, per = digits / 3;
        if (digits % 3 || per < 1 || per > 4) return -1;
        uint32_t rgb = 0;
        for (int c = 0; c < 3; c++) {
            int v = 0;
            for (int i = 0; i < per; i++) {
                int h = hexval(s[1 + c * per + i]);
                if (h < 0) return -1;
                v = v * 16 + h;
            }
            v = per == 1 ? v * 17 : per == 2 ? v : per == 3 ? v >> 4 : v >> 8;
            rgb = (rgb << 8) | (uint32_t)v;
        }
        return (long)rgb;
    }
    if (len > 4 && !strncasecmp(s, "rgb:", 4)) {
        uint32_t rgb = 0;
        int i = 4;
        for (int c = 0; c < 3; c++) {
            int v = 0, d = 0;
            while (i < len && s[i] != '/') {
                int h = hexval(s[i++]);
                if (h < 0) return -1;
                v = v * 16 + h; d++;
            }
            i++;
            if (d < 1 || d > 4) return -1;
            v = d == 1 ? v * 17 : d == 2 ? v : d == 3 ? v >> 4 : v >> 8;
            rgb = (rgb << 8) | (uint32_t)v;
        }
        return (long)rgb;
    }
    for (int i = 0; i < len; i++) {
        char ch = s[i];
        if (ch == ' ') continue;
        if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
        key[n++] = ch;
    }
    key[n] = 0;
    const void *hit = bsearch(key, rgb_names, sizeof(rgb_names) / sizeof(rgb_names[0]),
                              sizeof(rgb_names[0]), cmp_rgbname);
    if (!hit) return -1;
    return (long)((const __typeof__(rgb_names[0]) *)hit)->rgb;
}

/* ── keyboard tables ─────────────────────────────────────────────────────── */
/* Linux keycodes (drivers/keyboard.c) + 8 are the X keycodes; US layout,
 * two keysyms per keycode (unshifted, shifted).  The modifier rows pair with
 * wm.h's WM_MOD_* which are the X masks. */
#define X_KEYCODE_BASE 8
#define KEYMAP_MAX     128
static const uint32_t us_keysyms[KEYMAP_MAX][2] = {
    [1]  = { 0xff1b, 0xff1b },
    [2]  = { '1', '!' },  [3]  = { '2', '@' },  [4]  = { '3', '#' },
    [5]  = { '4', '$' },  [6]  = { '5', '%' },  [7]  = { '6', '^' },
    [8]  = { '7', '&' },  [9]  = { '8', '*' },  [10] = { '9', '(' },
    [11] = { '0', ')' },  [12] = { '-', '_' },  [13] = { '=', '+' },
    [14] = { 0xff08, 0xff08 },
    [15] = { 0xff09, 0xfe20 },
    [16] = { 'q', 'Q' },  [17] = { 'w', 'W' },  [18] = { 'e', 'E' },
    [19] = { 'r', 'R' },  [20] = { 't', 'T' },  [21] = { 'y', 'Y' },
    [22] = { 'u', 'U' },  [23] = { 'i', 'I' },  [24] = { 'o', 'O' },
    [25] = { 'p', 'P' },  [26] = { '[', '{' },  [27] = { ']', '}' },
    [28] = { 0xff0d, 0xff0d },
    [29] = { 0xffe3, 0xffe3 },
    [30] = { 'a', 'A' },  [31] = { 's', 'S' },  [32] = { 'd', 'D' },
    [33] = { 'f', 'F' },  [34] = { 'g', 'G' },  [35] = { 'h', 'H' },
    [36] = { 'j', 'J' },  [37] = { 'k', 'K' },  [38] = { 'l', 'L' },
    [39] = { ';', ':' },  [40] = { '\'', '"' }, [41] = { '`', '~' },
    [42] = { 0xffe1, 0xffe1 },
    [43] = { '\\', '|' },
    [44] = { 'z', 'Z' },  [45] = { 'x', 'X' },  [46] = { 'c', 'C' },
    [47] = { 'v', 'V' },  [48] = { 'b', 'B' },  [49] = { 'n', 'N' },
    [50] = { 'm', 'M' },  [51] = { ',', '<' },  [52] = { '.', '>' },
    [53] = { '/', '?' },
    [54] = { 0xffe2, 0xffe2 },
    [55] = { 0xffaa, 0xffaa },
    [56] = { 0xffe9, 0xffe9 },
    [57] = { ' ', ' ' },
    [58] = { 0xffe5, 0xffe5 },
    [59] = { 0xffbe, 0xffbe }, [60] = { 0xffbf, 0xffbf },
    [61] = { 0xffc0, 0xffc0 }, [62] = { 0xffc1, 0xffc1 },
    [63] = { 0xffc2, 0xffc2 }, [64] = { 0xffc3, 0xffc3 },
    [65] = { 0xffc4, 0xffc4 }, [66] = { 0xffc5, 0xffc5 },
    [67] = { 0xffc6, 0xffc6 }, [68] = { 0xffc7, 0xffc7 },
    [69] = { 0xff7f, 0xff7f },
    [70] = { 0xff14, 0xff14 },
    [71] = { 0xffb7, 0xffb7 }, [72] = { 0xffb8, 0xffb8 }, [73] = { 0xffb9, 0xffb9 },
    [74] = { 0xffad, 0xffad },
    [75] = { 0xffb4, 0xffb4 }, [76] = { 0xffb5, 0xffb5 }, [77] = { 0xffb6, 0xffb6 },
    [78] = { 0xffab, 0xffab },
    [79] = { 0xffb1, 0xffb1 }, [80] = { 0xffb2, 0xffb2 }, [81] = { 0xffb3, 0xffb3 },
    [82] = { 0xffb0, 0xffb0 },
    [83] = { 0xffae, 0xffae },
    [87] = { 0xffc8, 0xffc8 }, [88] = { 0xffc9, 0xffc9 },
    [96] = { 0xff8d, 0xff8d },
    [97] = { 0xffe4, 0xffe4 },
    [98] = { 0xffaf, 0xffaf },
    [100] = { 0xffea, 0xffea },
    [102] = { 0xff50, 0xff50 },
    [103] = { 0xff52, 0xff52 },
    [104] = { 0xff55, 0xff55 },
    [105] = { 0xff51, 0xff51 },
    [106] = { 0xff53, 0xff53 },
    [107] = { 0xff57, 0xff57 },
    [108] = { 0xff54, 0xff54 },
    [109] = { 0xff56, 0xff56 },
    [110] = { 0xff63, 0xff63 },
    [111] = { 0xffff, 0xffff },
    [125] = { 0xffeb, 0xffeb },
    [126] = { 0xffec, 0xffec },
    [127] = { 0xff67, 0xff67 },
};

#define KEYCODES_PER_MODIFIER 2
static const uint8_t modifier_linux_keys[8][KEYCODES_PER_MODIFIER] = {
    { 42, 54 },     /* Shift */
    { 58, 0 },      /* Lock */
    { 29, 97 },     /* Control */
    { 56, 100 },    /* Mod1 = Alt */
    { 69, 0 },      /* Mod2 = NumLock */
    { 0, 0 },
    { 125, 126 },   /* Mod4 = Super */
    { 0, 0 },
};

/* ── request helpers ─────────────────────────────────────────────────────── */
static int mask_bits(uint32_t m) {
    int n = 0;
    for (; m; m &= m - 1) n++;
    return n;
}

static window_t *req_window(client_t *c, uint32_t id) {
    window_t *w = id == ROOT_WINDOW ? root : lookup_window(id);
    if (!w) x_error(c, BadWindow, id);
    return w;
}

/* CreateWindow / ChangeWindowAttributes value list. */
static int set_window_attrs(client_t *c, window_t *w, uint32_t mask, const uint8_t *v,
                            int nvals) {
    if (mask_bits(mask & 0x7FFF) > nvals) { x_error(c, BadLength, 0); return -1; }
    int bg_changed = 0;
    for (int bit = 0; bit < 15; bit++) {
        if (!(mask & (1u << bit))) continue;
        uint32_t val = r32(v);
        v += 4;
        switch (bit) {
        case 0:                                    /* background-pixmap */
            if (w->bg_pixmap) { drawable_unref(w->bg_pixmap); w->bg_pixmap = NULL; }
            if (val == 0) w->bg_mode = BG_NONE;
            else if (val == 1) w->bg_mode = BG_PARENT;
            else {
                xobj_t *o = res_lookup(val);
                if (o && o->type == XT_PIXMAP) {
                    w->bg_mode = BG_PIXMAP;
                    w->bg_pixmap = (pixmap_t *)o;
                    w->bg_pixmap->refs++;
                } else {
                    x_error(c, BadPixmap, val);
                    return -1;
                }
            }
            bg_changed = 1;
            break;
        case 1: w->bg_mode = BG_PIXEL; w->bg_pixel = val;
                if (w->bg_pixmap) { drawable_unref(w->bg_pixmap); w->bg_pixmap = NULL; }
                bg_changed = 1; break;
        case 2: break;                             /* border-pixmap */
        case 3: w->border_pixel = val; break;
        case 4: w->bit_gravity = (int)val; break;
        case 5: w->win_gravity = (int)val; break;
        case 6: w->backing_store = (int)val; break;
        case 7: case 8: break;
        case 9: w->override_redirect = (int)(val & 1); break;
        case 10: w->save_under = (int)(val & 1); break;
        case 11: select_input(w, c->index, val); break;
        case 12: w->dont_propagate = val; break;
        case 13: w->colormap = val; break;
        case 14: w->cursor = val; break;
        }
    }
    (void)bg_changed;
    return 0;
}

static void reply_geometry(client_t *c, uint32_t id) {
    xobj_t *o = id == ROOT_WINDOW ? &root->d.o : res_lookup(id);
    uint8_t d[24];
    memset(d, 0, sizeof(d));
    if (!o || (o->type != XT_WINDOW && o->type != XT_PIXMAP)) {
        x_error(c, BadDrawable, id);
        return;
    }
    put32(d + 0, ROOT_WINDOW);
    drawable_t *dr = (drawable_t *)o;
    if (o->type == XT_WINDOW) {
        window_t *w = (window_t *)o;
        put16(d + 4, (uint32_t)w->x); put16(d + 6, (uint32_t)w->y);
        put16(d + 12, (uint32_t)w->bw);
    }
    put16(d + 8, (uint32_t)dr->w); put16(d + 10, (uint32_t)dr->h);
    send_reply(c, (uint8_t)(dr->depth ? dr->depth : 24), d);
}

static void reply_window_attributes(client_t *c, window_t *w) {
    uint8_t d[24], extra[12];
    memset(d, 0, sizeof(d));
    memset(extra, 0, sizeof(extra));
    put32(d + 0, w->cls == 2 ? 0 : ROOT_VISUAL);
    put16(d + 4, (uint32_t)(w->cls ? w->cls : 1));
    d[6] = (uint8_t)w->bit_gravity;
    d[7] = (uint8_t)w->win_gravity;
    put32(d + 8, 0xFFFFFFFF);                 /* backing-planes */
    put32(d + 12, 0);                         /* backing-pixel */
    d[16] = (uint8_t)w->save_under;
    d[17] = 1;                                /* map-is-installed */
    d[18] = (uint8_t)(w->mapped ? (window_viewable(w) ? 2 : 1) : 0);
    d[19] = (uint8_t)w->override_redirect;
    put32(d + 20, w->colormap);
    put32(extra + 0, event_mask_of(w));
    put32(extra + 4, client_mask(w, c->index));
    put16(extra + 8, w->dont_propagate);
    send_reply_var(c, (uint8_t)w->backing_store, d, extra, 12);
}

static void reply_query_tree(client_t *c, window_t *w) {
    int n = 0;
    for (window_t *ch = w->bottom; ch; ch = ch->above) n++;
    uint8_t *ids = malloc((size_t)(n ? n : 1) * 4);
    if (!ids) { x_error(c, BadAlloc, 0); return; }
    int i = 0;
    for (window_t *ch = w->bottom; ch; ch = ch->above) put32(ids + 4 * i++, ch->d.o.id);
    uint8_t d[24];
    memset(d, 0, sizeof(d));
    put32(d + 0, ROOT_WINDOW);
    put32(d + 4, w->parent ? w->parent->d.o.id : 0);
    put16(d + 8, (uint32_t)n);
    send_reply_var(c, 0, d, ids, n * 4);
    free(ids);
}

static void do_get_property(client_t *c, const uint8_t *q) {
    int del = q[1];
    window_t *w = req_window(c, r32(q + 4));
    if (!w) return;
    uint32_t name = r32(q + 8), type = r32(q + 12);
    uint32_t off = r32(q + 16), len = r32(q + 20);
    prop_t *p = prop_find(w, name);
    uint8_t d[24];
    memset(d, 0, sizeof(d));
    if (!p) { send_reply(c, 0, d); return; }
    uint32_t total = p->n * (uint32_t)(p->format / 8);
    if (type != 0 && type != p->type) {
        put32(d + 0, p->type);
        put32(d + 4, total);
        send_reply(c, (uint8_t)p->format, d);
        return;
    }
    uint64_t start = (uint64_t)off * 4;
    if (start > total) { x_error(c, BadValue, off); return; }
    uint32_t avail = total - (uint32_t)start;
    uint32_t L = (uint64_t)len * 4 < avail ? len * 4 : avail;
    L -= L % (uint32_t)(p->format / 8);
    put32(d + 0, p->type);
    put32(d + 4, avail - L);
    put32(d + 8, L / (uint32_t)(p->format / 8));
    send_reply_var(c, (uint8_t)p->format, d, p->data + start, (int)L);
    if (del && avail - L == 0) prop_delete(w, name);
}

static void do_change_property(client_t *c, const uint8_t *q, int qlen) {
    int mode = q[1];
    window_t *w = req_window(c, r32(q + 4));
    if (!w) return;
    uint32_t name = r32(q + 8), type = r32(q + 12);
    int format = q[16];
    uint32_t n = r32(q + 20);
    if (format != 8 && format != 16 && format != 32) { x_error(c, BadValue, (uint32_t)format); return; }
    uint64_t bytes = (uint64_t)n * (uint32_t)(format / 8);
    if (24 + bytes > (uint64_t)qlen) { x_error(c, BadLength, 0); return; }
    if (name == 0 || name >= natoms) { x_error(c, BadAtom, name); return; }
    prop_t *p = prop_find(w, name);
    /* Appends grow a property without bound: cap its size so the byte
     * counts (p->n * format/8, here and in GetProperty) never wrap. */
    if (mode != 0 && p && (uint64_t)p->n * (uint32_t)(p->format / 8) + bytes > PROP_MAX) {
        x_error(c, BadAlloc, 0);
        return;
    }
    if (mode == 0 || !p) {
        prop_set(w, name, type, format, q + 24, n, 1);
        return;
    }
    if (p->format != format || p->type != type) { x_error(c, BadMatch, 0); return; }
    size_t ob = (size_t)p->n * (size_t)(format / 8);
    uint8_t *nd = malloc(ob + (size_t)bytes + 1);
    if (!nd) { x_error(c, BadAlloc, 0); return; }
    if (mode == 1) { memcpy(nd, q + 24, (size_t)bytes); memcpy(nd + bytes, p->data, ob); }
    else { memcpy(nd, p->data, ob); memcpy(nd + ob, q + 24, (size_t)bytes); }
    free(p->data);
    p->data = nd;
    p->n += n;
    property_notify(w, name, 0);
    wm_property_changed(w, name);
}

static void do_configure(client_t *c, const uint8_t *q, int qlen) {
    window_t *w = req_window(c, r32(q + 4));
    if (!w) return;
    uint32_t mask = r16(q + 8);
    if (12 + 4 * mask_bits(mask & 0x7F) > qlen) { x_error(c, BadLength, 0); return; }
    const uint8_t *v = q + 12;
    int x = w->x, y = w->y, ww = w->d.w, hh = w->d.h, bw = w->bw;
    window_t *sib = NULL;
    int stack = -1;
    if (mask & 0x01) { x = rs16(v); v += 4; }
    if (mask & 0x02) { y = rs16(v); v += 4; }
    if (mask & 0x04) { ww = (int)r16(v); v += 4; }
    if (mask & 0x08) { hh = (int)r16(v); v += 4; }
    if (mask & 0x10) { bw = (int)r16(v); v += 4; }
    if (mask & 0x20) {
        sib = lookup_window(r32(v));
        if (!sib) { x_error(c, BadWindow, r32(v)); return; }
        v += 4;
    }
    if (mask & 0x40) { stack = v[0]; v += 4; }
    if (w == root) return;
    /* Xorg ConfigureWindow: a sibling needs a stack mode, must be a sibling
     * and not the window itself (that made the sibling list a cycle). */
    if (sib && (stack < 0 || sib == w || sib->parent != w->parent)) {
        x_error(c, BadMatch, sib->d.o.id);
        return;
    }
    if (stack > 4) { x_error(c, BadValue, (uint32_t)stack); return; }
    if (w->wm_max && w->parent == root) {      /* kiosk: the toplevel stays full screen */
        x = 0; y = 0; ww = scr_w; hh = scr_h;
    }
    if (stack >= 0) restack(w, sib, stack);
    configure_window(w, x, y, ww, hh, bw, w->parent == root && !w->override_redirect);
}

/* The tree walks (destroy, compositing, event delivery) recurse per level:
 * nesting is capped so a client cannot overflow the server's stack.  Real
 * toolkits nest a few tens deep. */
#define MAX_WINDOW_DEPTH 256
static int window_depth(window_t *w) {
    int d = 0;
    for (; w; w = w->parent) d++;
    return d;
}
static int subtree_height(window_t *w) {
    int h = 0;
    for (window_t *ch = w->bottom; ch; ch = ch->above) {
        int t = subtree_height(ch);
        if (t > h) h = t;
    }
    return h + 1;
}

static void do_reparent(client_t *c, const uint8_t *q) {
    window_t *w = req_window(c, r32(q + 4));
    window_t *p = req_window(c, r32(q + 8));
    if (!w || !p) return;
    if (w == root || window_is_ancestor(w, p)) { x_error(c, BadMatch, 0); return; }
    if (window_depth(p) + subtree_height(w) > MAX_WINDOW_DEPTH) { x_error(c, BadAlloc, w->d.o.id); return; }
    int was_mapped = w->mapped;
    if (was_mapped) unmap_window(w);
    unlink_sibling(w);
    link_top(p, w);
    w->x = rs16(q + 12); w->y = rs16(q + 14);
    uint8_t e[32];
    memset(e, 0, sizeof(e));
    e[0] = ReparentNotify;
    put32(e + 12, p->d.o.id);
    put16(e + 16, (uint32_t)w->x); put16(e + 18, (uint32_t)w->y);
    e[20] = (uint8_t)w->override_redirect;
    put32(e + 8, w->d.o.id);
    deliver_to_window(w, StructureNotifyMask, e, 4);
    if (w->parent) deliver_to_window(p, SubstructureNotifyMask, e, 4);
    if (was_mapped) map_window(c, w);
}

static void do_send_event(client_t *c, const uint8_t *q) {
    int propagate = q[1];
    uint32_t dest = r32(q + 4), emask = r32(q + 8);
    uint8_t ev[32];
    memcpy(ev, q + 12, 32);
    ev[0] |= 0x80;
    window_t *w;
    if (dest == 0) w = sprite_window();
    else if (dest == 1) {
        w = focus_window();
        if (w) {
            window_t *sp = sprite_window();
            if (sp && window_is_ancestor(w, sp)) w = sp;
        } else w = sprite_window();
    } else w = dest == ROOT_WINDOW ? root : lookup_window(dest);
    if (!w) { x_error(c, BadWindow, dest); return; }
    /* Messages to a window manager (EWMH/ICCCM requests on the root). */
    if (w == root && (ev[0] & 0x7F) == ClientMessage &&
        (emask & (SubstructureRedirectMask | SubstructureNotifyMask)))
        wm_client_message_to_root(c, ev);
    if (emask == 0) {
        if (w->d.o.owner >= 0 && clients[(int)w->d.o.owner].used)
            send_event(&clients[(int)w->d.o.owner], ev);
        return;
    }
    for (; w; w = w->parent) {
        if (event_mask_of(w) & emask) { deliver_to_window(w, emask, ev, -1); return; }
        if (!propagate || (w->dont_propagate & emask)) return;
    }
}

static void do_convert_selection(client_t *c, const uint8_t *q) {
    uint32_t req = r32(q + 4), sel = r32(q + 8), target = r32(q + 12);
    uint32_t prop = r32(q + 16), time = r32(q + 20);
    window_t *rw = req_window(c, req);
    if (!rw) return;
    for (int i = 0; i < nsels; i++) {
        if (sels[i].atom != sel || !sels[i].window || sels[i].client < 0) continue;
        client_t *oc = &clients[sels[i].client];
        if (!oc->used || oc->dead) break;
        uint8_t e[32];
        memset(e, 0, sizeof(e));
        e[0] = SelectionRequest;
        put32(e + 4, time);
        put32(e + 8, sels[i].window);
        put32(e + 12, req);
        put32(e + 16, sel);
        put32(e + 20, target);
        put32(e + 24, prop);
        send_event(oc, e);
        return;
    }
    uint8_t e[32];
    memset(e, 0, sizeof(e));
    e[0] = SelectionNotify;
    put32(e + 4, time);
    put32(e + 8, req);
    put32(e + 12, sel);
    put32(e + 16, target);
    put32(e + 20, 0);
    send_event(c, e);
}

static void do_set_selection_owner(client_t *c, const uint8_t *q) {
    uint32_t owner = r32(q + 4), sel = r32(q + 8), time = r32(q + 12);
    if (owner && !lookup_window(owner) && owner != ROOT_WINDOW) { x_error(c, BadWindow, owner); return; }
    int i;
    for (i = 0; i < nsels; i++) if (sels[i].atom == sel) break;
    if (i == nsels) {
        if (nsels >= MAX_SELECTIONS) return;
        nsels++;
        sels[i].atom = sel; sels[i].window = 0; sels[i].client = -1;
    }
    if (sels[i].window && sels[i].client >= 0 && sels[i].client != c->index) {
        client_t *oc = &clients[sels[i].client];
        if (oc->used && !oc->dead) {
            uint8_t e[32];
            memset(e, 0, sizeof(e));
            e[0] = SelectionClear;
            put32(e + 4, time ? time : x_time());
            put32(e + 8, sels[i].window);
            put32(e + 12, sel);
            send_event(oc, e);
        }
    }
    sels[i].window = owner;
    sels[i].client = owner ? c->index : -1;
    sels[i].time = time ? time : x_time();
}

static void do_query_pointer(client_t *c, window_t *w) {
    uint8_t d[24];
    memset(d, 0, sizeof(d));
    int ax, ay;
    window_abs(w, &ax, &ay);
    put32(d + 0, ROOT_WINDOW);
    window_t *sp = sprite_window(), *child = NULL;
    if (sp && sp != w && window_is_ancestor(w, sp))
        for (child = sp; child->parent != w; child = child->parent) ;
    put32(d + 4, child ? child->d.o.id : 0);
    put16(d + 8, (uint32_t)ptr_x); put16(d + 10, (uint32_t)ptr_y);
    put16(d + 12, (uint32_t)(ptr_x - ax)); put16(d + 14, (uint32_t)(ptr_y - ay));
    put16(d + 16, state_mask());
    send_reply(c, 1, d);
}

static void do_translate(client_t *c, const uint8_t *q) {
    window_t *s = req_window(c, r32(q + 4));
    window_t *t = req_window(c, r32(q + 8));
    if (!s || !t) return;
    int sx, sy, tx, ty;
    window_abs(s, &sx, &sy);
    window_abs(t, &tx, &ty);
    int x = rs16(q + 12) + sx - tx, y = rs16(q + 14) + sy - ty;
    window_t *child = NULL;
    for (window_t *ch = t->top; ch; ch = ch->below) {
        if (!ch->mapped) continue;
        if (x >= ch->x && y >= ch->y && x < ch->x + ch->d.w + 2 * ch->bw &&
            y < ch->y + ch->d.h + 2 * ch->bw) { child = ch; break; }
    }
    uint8_t d[24];
    memset(d, 0, sizeof(d));
    put32(d + 0, child ? child->d.o.id : 0);
    put16(d + 4, (uint32_t)x); put16(d + 6, (uint32_t)y);
    send_reply(c, 1, d);
}

static void do_ext_query(client_t *c, const uint8_t *q, int qlen) {
    int n = (int)r16(q + 4);
    char nm[64];
    if (8 + n > qlen) { x_error(c, BadLength, 0); return; }
    int k = n < 63 ? n : 63;
    memcpy(nm, q + 8, (size_t)k);
    nm[k] = 0;
    uint8_t d[24];
    memset(d, 0, sizeof(d));
    if (!strcmp(nm, "RENDER")) {
        d[0] = 1; d[1] = (uint8_t)render_major; d[2] = RENDER_EVENT_BASE; d[3] = RENDER_ERROR_BASE;
    }
    xlog("QueryExtension %s -> %s\n", nm, d[0] ? "present" : "absent");
    send_reply(c, 0, d);
}

static void do_list_extensions(client_t *c) {
    static const char *names[] = { "RENDER" };
    uint8_t buf[64];
    int n = 0;
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        int l = (int)strlen(names[i]);
        buf[n++] = (uint8_t)l;
        memcpy(buf + n, names[i], (size_t)l);
        n += l;
    }
    uint8_t d[24];
    memset(d, 0, sizeof(d));
    send_reply_var(c, (uint8_t)(sizeof(names) / sizeof(names[0])), d, buf, n);
}

static unsigned unknown_seen[8];

static void unknown_request(client_t *c, int op, int qlen) {
    if (!(unknown_seen[op >> 5] & (1u << (op & 31)))) {
        unknown_seen[op >> 5] |= 1u << (op & 31);
        xlog("unimplemented request op=%d len=%d\n", op, qlen);
    }
    x_error(c, BadRequest, 0);
}

/* ── the dispatcher ──────────────────────────────────────────────────────── */
static int core_min_len(int op) {
    switch (op) {
    case 1: return 32;   case 2: return 12;   case 3: case 4: case 5: return 8;
    case 6: return 8;    case 7: return 16;   case 8: case 9: case 10: case 11: return 8;
    case 12: return 12;  case 13: return 8;   case 14: case 15: return 8;
    case 16: return 8;   case 17: return 8;   case 18: return 24;  case 19: return 12;
    case 20: return 24;  case 21: return 8;   case 22: return 16;  case 23: return 8;
    case 24: return 24;  case 25: return 44;  case 26: return 24;  case 27: return 8;
    case 28: return 24;  case 29: return 12;  case 30: return 16;  case 31: return 16;
    case 32: return 8;   case 33: return 16;  case 34: return 12;  case 35: return 8;
    case 38: return 8;   case 39: return 16;  case 40: return 16;  case 41: return 24;
    case 42: return 12;  case 45: return 12;  case 46: case 47: return 8;
    case 48: return 8;   case 49: return 8;   case 50: return 8;   case 51: return 8;
    case 53: return 16;  case 54: return 8;   case 55: return 16;  case 56: return 12;
    case 57: return 16;  case 58: return 12;  case 59: return 12;  case 60: return 8;
    case 61: return 16;  case 62: return 28;  case 63: return 32;  case 64: case 65:
    case 66: case 67: case 68: return 12;    case 69: return 16;  case 70: case 71: return 12;
    case 72: return 24;  case 73: return 20;  case 74: case 75: return 16;
    case 76: case 77: return 16;              case 78: return 16;  case 79: return 8;
    case 80: return 12;  case 81: case 82: case 83: return 8;      case 84: return 16;
    case 85: return 12;  case 86: return 12;  case 87: return 16;  case 88: return 12;
    case 89: return 8;   case 90: return 16;  case 91: return 8;   case 92: return 12;
    case 93: return 32;  case 94: return 32;  case 95: return 8;   case 96: return 20;
    case 97: return 12;  case 98: return 8;   case 100: return 8;  case 101: return 8;
    case 102: return 8;  case 104: return 4;  case 105: return 12; case 107: return 12;
    case 109: return 8;  case 113: return 8;  case 114: return 12; case 116: return 4;
    case 118: return 4;
    default: return 4;
    }
}

void core_dispatch(client_t *c, const uint8_t *q, int qlen) {
    int op = q[0];
    if (qlen < core_min_len(op)) { x_error(c, BadLength, 0); return; }
    switch (op) {
    case 1: {                                                  /* CreateWindow */
        uint32_t wid = r32(q + 4);
        if (!res_check_new(c, wid)) return;
        window_t *p = req_window(c, r32(q + 8));
        if (!p) return;
        if (window_depth(p) >= MAX_WINDOW_DEPTH) { x_error(c, BadAlloc, wid); return; }
        int cls = (int)r16(q + 22);
        if (cls == 0) cls = p->cls ? p->cls : 1;
        window_t *w = window_new(wid, c->index, p, rs16(q + 12), rs16(q + 14),
                                 (int)r16(q + 16), (int)r16(q + 18), (int)r16(q + 20), cls);
        if (!w) { x_error(c, BadAlloc, wid); return; }
        w->d.depth = q[1] ? q[1] : 24;
        if (r32(q + 24)) w->visual = r32(q + 24);
        res_add(&w->d.o);
        if (set_window_attrs(c, w, r32(q + 28), q + 32, (qlen - 32) / 4) < 0) {
            destroy_one(w);
            return;
        }
        uint8_t e[32];
        memset(e, 0, sizeof(e));
        e[0] = CreateNotify;
        put32(e + 4, p->d.o.id);
        put32(e + 8, wid);
        put16(e + 12, (uint32_t)w->x); put16(e + 14, (uint32_t)w->y);
        put16(e + 16, (uint32_t)w->d.w); put16(e + 18, (uint32_t)w->d.h);
        put16(e + 20, (uint32_t)w->bw);
        e[22] = (uint8_t)w->override_redirect;
        deliver_to_window(p, SubstructureNotifyMask, e, -1);
        break;
    }
    case 2: {                                                  /* ChangeWindowAttributes */
        window_t *w = req_window(c, r32(q + 4));
        if (!w) return;
        set_window_attrs(c, w, r32(q + 8), q + 12, (qlen - 12) / 4);
        break;
    }
    case 3: { window_t *w = req_window(c, r32(q + 4)); if (w) reply_window_attributes(c, w); break; }
    case 4: { window_t *w = req_window(c, r32(q + 4)); if (w) destroy_window(w); break; }
    case 5: {                                                  /* DestroySubwindows */
        window_t *w = req_window(c, r32(q + 4));
        if (w) while (w->top) destroy_window(w->top);
        break;
    }
    case 6: break;                                             /* ChangeSaveSet */
    case 7: do_reparent(c, q); break;
    case 8: { window_t *w = req_window(c, r32(q + 4)); if (w) map_window(c, w); break; }
    case 9: {                                                  /* MapSubwindows */
        window_t *w = req_window(c, r32(q + 4));
        if (w) for (window_t *ch = w->bottom; ch; ch = ch->above) if (!ch->mapped) map_window(c, ch);
        break;
    }
    case 10: { window_t *w = req_window(c, r32(q + 4)); if (w) unmap_window(w); break; }
    case 11: {
        window_t *w = req_window(c, r32(q + 4));
        if (w) for (window_t *ch = w->top; ch; ch = ch->below) unmap_window(ch);
        break;
    }
    case 12: do_configure(c, q, qlen); break;
    case 13: {                                                 /* CirculateWindow */
        window_t *w = req_window(c, r32(q + 4));
        if (!w || !w->bottom) return;
        if (q[1] == 0) restack(w->bottom, NULL, 0);           /* RaiseLowest */
        else restack(w->top, NULL, 1);                        /* LowerHighest */
        break;
    }
    case 14: reply_geometry(c, r32(q + 4)); break;
    case 15: { window_t *w = req_window(c, r32(q + 4)); if (w) reply_query_tree(c, w); break; }
    case 16: {                                                 /* InternAtom */
        int n = (int)r16(q + 4);
        if (8 + n > qlen) { x_error(c, BadLength, 0); return; }
        uint8_t d[24];
        memset(d, 0, sizeof(d));
        put32(d, atom_intern((const char *)q + 8, n, q[1]));
        send_reply(c, 0, d);
        break;
    }
    case 17: {                                                 /* GetAtomName */
        int n;
        const char *s = atom_name(r32(q + 4), &n);
        if (!s) { x_error(c, BadAtom, r32(q + 4)); return; }
        uint8_t d[24];
        memset(d, 0, sizeof(d));
        put16(d, (uint32_t)n);
        send_reply_var(c, 0, d, (const uint8_t *)s, n);
        break;
    }
    case 18: do_change_property(c, q, qlen); break;
    case 19: { window_t *w = req_window(c, r32(q + 4)); if (w) prop_delete(w, r32(q + 8)); break; }
    case 20: do_get_property(c, q); break;
    case 21: {                                                 /* ListProperties */
        window_t *w = req_window(c, r32(q + 4));
        if (!w) return;
        int n = 0;
        for (prop_t *p = w->props; p; p = p->next) n++;
        uint8_t *buf = malloc((size_t)(n ? n : 1) * 4);
        if (!buf) { x_error(c, BadAlloc, 0); return; }
        int i = 0;
        for (prop_t *p = w->props; p; p = p->next) put32(buf + 4 * i++, p->name);
        uint8_t d[24];
        memset(d, 0, sizeof(d));
        put16(d, (uint32_t)n);
        send_reply_var(c, 0, d, buf, n * 4);
        free(buf);
        break;
    }
    case 22: do_set_selection_owner(c, q); break;
    case 23: {                                                 /* GetSelectionOwner */
        uint8_t d[24];
        memset(d, 0, sizeof(d));
        for (int i = 0; i < nsels; i++) if (sels[i].atom == r32(q + 4)) put32(d, sels[i].window);
        send_reply(c, 0, d);
        break;
    }
    case 24: do_convert_selection(c, q); break;
    case 25: do_send_event(c, q); break;
    case 26: {                                                 /* GrabPointer */
        window_t *w = req_window(c, r32(q + 4));
        if (!w) return;
        uint8_t d[24];
        memset(d, 0, sizeof(d));
        int status = 0;
        if (!window_viewable(w)) status = 3;                   /* GrabNotViewable */
        else if (pgrab.win && pgrab.client != c->index && !pgrab.implicit) status = 1;
        else {
            pgrab.win = w; pgrab.client = c->index; pgrab.mask = r16(q + 8);
            pgrab.owner_events = q[1]; pgrab.implicit = 0;
        }
        send_reply(c, (uint8_t)status, d);
        break;
    }
    case 27:                                                   /* UngrabPointer */
        if (pgrab.win && pgrab.client == c->index) { pgrab.win = NULL; input_window_changed(); }
        break;
    case 28: case 29: break;                                   /* Grab/UngrabButton */
    case 30:                                                   /* ChangeActivePointerGrab */
        if (pgrab.win && pgrab.client == c->index) pgrab.mask = r16(q + 12);
        break;
    case 31: {                                                 /* GrabKeyboard */
        window_t *w = req_window(c, r32(q + 4));
        if (!w) return;
        uint8_t d[24];
        memset(d, 0, sizeof(d));
        int status = 0;
        if (!window_viewable(w)) status = 3;
        else if (kgrab.win && kgrab.client != c->index) status = 1;
        else { kgrab.win = w; kgrab.client = c->index; kgrab.owner_events = q[1]; }
        send_reply(c, (uint8_t)status, d);
        break;
    }
    case 32: if (kgrab.win && kgrab.client == c->index) kgrab.win = NULL; break;
    case 33: case 34: case 35: case 36: case 37: break;        /* Grab/UngrabKey, AllowEvents, Grab/UngrabServer */
    case 38: { window_t *w = req_window(c, r32(q + 4)); if (w) do_query_pointer(c, w); break; }
    case 39: { uint8_t d[24]; memset(d, 0, sizeof(d)); send_reply(c, 0, d); break; }
    case 40: do_translate(c, q); break;
    case 41: break;                                            /* WarpPointer */
    case 42: {                                                 /* SetInputFocus */
        uint32_t id = r32(q + 4);
        if (id == 0) set_focus(NULL, q[1]);
        else if (id == 1) set_focus((window_t *)1, q[1]);
        else {
            window_t *w = req_window(c, id);
            if (!w) return;
            if (!window_viewable(w)) { x_error(c, BadMatch, id); return; }
            set_focus(w, q[1]);
        }
        break;
    }
    case 43: {                                                 /* GetInputFocus */
        uint8_t d[24];
        memset(d, 0, sizeof(d));
        put32(d, focus_mode == 2 && focus_win ? focus_win->d.o.id : (uint32_t)focus_mode);
        send_reply(c, (uint8_t)focus_revert, d);
        break;
    }
    case 44: {                                                 /* QueryKeymap */
        uint8_t d[24];
        memcpy(d, keys_down, 24);
        send_reply_var(c, 0, d, keys_down + 24, 8);
        break;
    }
    case 45: {                                                 /* OpenFont */
        int n = (int)r16(q + 8);
        if (12 + n > qlen) { x_error(c, BadLength, 0); return; }
        if (!res_check_new(c, r32(q + 4))) return;
        font_open(c, r32(q + 4), (const char *)q + 12, n);
        break;
    }
    case 46: font_close(r32(q + 4)); break;
    case 47: font_query(c, r32(q + 4)); break;
    case 48: font_text_extents(c, r32(q + 4), q + 8, (qlen - 8) / 2 - (q[1] ? 1 : 0)); break;
    case 49: case 50: {                                        /* ListFonts(WithInfo) */
        int n = (int)r16(q + 6);
        if (8 + n > qlen) { x_error(c, BadLength, 0); return; }
        font_list(c, (const char *)q + 8, n, (int)r16(q + 4), op == 50);
        break;
    }
    case 51: break;                                            /* SetFontPath */
    case 52: font_get_path(c); break;
    case 61: {                                                 /* ClearArea */
        window_t *w = req_window(c, r32(q + 4));
        if (!w) return;
        int x = rs16(q + 8), y = rs16(q + 10);
        int ww = (int)r16(q + 12), hh = (int)r16(q + 14);
        if (ww == 0) ww = w->d.w - x;
        if (hh == 0) hh = w->d.h - y;
        fill_background(w, x, y, ww, hh);
        if (q[1] && window_viewable(w)) {
            int cx = x, cy = y, cw = ww, ch = hh;
            if (clip_span(&cx, NULL, &cw, w->d.w, 0, NULL) &&
                clip_span(&cy, NULL, &ch, w->d.h, 0, NULL))
                send_expose(w, cx, cy, cw, ch);
        }
        break;
    }
    case 78: if (res_check_new(c, r32(q + 4))) {               /* CreateColormap */
        xobj_t *o = calloc(1, sizeof(*o));
        if (o) { o->id = r32(q + 4); o->type = XT_COLORMAP; o->owner = (int8_t)c->index; res_add(o); }
    } break;
    case 79: {                                                 /* FreeColormap */
        xobj_t *o = res_lookup(r32(q + 4));
        if (o && o->type == XT_COLORMAP) { res_remove(o); free(o); }
        break;
    }
    case 80: if (res_check_new(c, r32(q + 4))) {               /* CopyColormapAndFree */
        xobj_t *o = calloc(1, sizeof(*o));
        if (o) { o->id = r32(q + 4); o->type = XT_COLORMAP; o->owner = (int8_t)c->index; res_add(o); }
    } break;
    case 81: case 82: break;                                   /* (Un)InstallColormap */
    case 83: {                                                 /* ListInstalledColormaps */
        uint8_t d[24], cm[4];
        memset(d, 0, sizeof(d));
        put16(d, 1);
        put32(cm, ROOT_COLORMAP);
        send_reply_var(c, 0, d, cm, 4);
        break;
    }
    case 84: {                                                 /* AllocColor */
        uint32_t rgb = ((r16(q + 8) >> 8) << 16) | ((r16(q + 10) >> 8) << 8) | (r16(q + 12) >> 8);
        uint8_t d[24];
        memset(d, 0, sizeof(d));
        put16(d + 0, ((rgb >> 16) & 0xFF) * 257);
        put16(d + 2, ((rgb >> 8) & 0xFF) * 257);
        put16(d + 4, (rgb & 0xFF) * 257);
        put32(d + 8, rgb);
        send_reply(c, 0, d);
        break;
    }
    case 85: case 92: {                                        /* AllocNamedColor / LookupColor */
        int n = (int)r16(q + 8);
        if (12 + n > qlen) { x_error(c, BadLength, 0); return; }
        long rgb = color_lookup((const char *)q + 12, n);
        if (rgb < 0) { x_error(c, BadName, 0); return; }
        uint8_t d[24];
        memset(d, 0, sizeof(d));
        uint32_t r = ((uint32_t)rgb >> 16) & 0xFF, g = ((uint32_t)rgb >> 8) & 0xFF, b = (uint32_t)rgb & 0xFF;
        int o = 0;
        if (op == 85) { put32(d, (uint32_t)rgb); o = 4; }
        put16(d + o + 0, r * 257); put16(d + o + 2, g * 257); put16(d + o + 4, b * 257);
        put16(d + o + 6, r * 257); put16(d + o + 8, g * 257); put16(d + o + 10, b * 257);
        send_reply(c, 0, d);
        break;
    }
    case 86: case 87: x_error(c, BadAlloc, 0); break;          /* AllocColorCells/Planes */
    case 88: case 89: case 90: break;                          /* Free/Store(Named)Colors */
    case 91: {                                                 /* QueryColors */
        int n = (qlen - 8) / 4;
        uint8_t *buf = malloc((size_t)(n ? n : 1) * 8);
        if (!buf) { x_error(c, BadAlloc, 0); return; }
        for (int i = 0; i < n; i++) {
            uint32_t px = r32(q + 8 + 4 * i);
            put16(buf + 8 * i + 0, ((px >> 16) & 0xFF) * 257);
            put16(buf + 8 * i + 2, ((px >> 8) & 0xFF) * 257);
            put16(buf + 8 * i + 4, (px & 0xFF) * 257);
            put16(buf + 8 * i + 6, 0);
        }
        uint8_t d[24];
        memset(d, 0, sizeof(d));
        put16(d, (uint32_t)n);
        send_reply_var(c, 0, d, buf, n * 8);
        free(buf);
        break;
    }
    case 93: case 94: if (res_check_new(c, r32(q + 4))) {      /* CreateCursor/GlyphCursor */
        xobj_t *o = calloc(1, sizeof(*o));
        if (o) { o->id = r32(q + 4); o->type = XT_CURSOR; o->owner = (int8_t)c->index; res_add(o); }
    } break;
    case 95: {                                                 /* FreeCursor */
        xobj_t *o = res_lookup(r32(q + 4));
        if (o && o->type == XT_CURSOR) { res_remove(o); free(o); }
        break;
    }
    case 96: break;                                            /* RecolorCursor */
    case 97: {                                                 /* QueryBestSize */
        uint8_t d[24];
        memset(d, 0, sizeof(d));
        int cls = q[1];
        put16(d, cls == 0 ? 32 : r16(q + 8));
        put16(d + 2, cls == 0 ? 32 : r16(q + 10));
        send_reply(c, 0, d);
        break;
    }
    case 98: do_ext_query(c, q, qlen); break;
    case 99: do_list_extensions(c); break;
    case 100: break;                                           /* ChangeKeyboardMapping */
    case 101: {                                                /* GetKeyboardMapping */
        int first = q[4], count = q[5];
        if (count < 1 || first < X_KEYCODE_BASE || first + count - 1 > 255) {
            x_error(c, BadValue, (uint32_t)first);
            return;
        }
        uint8_t *ks = malloc((size_t)count * 8);
        if (!ks) { x_error(c, BadAlloc, 0); return; }
        for (int i = 0; i < count; i++) {
            int lk = first + i - X_KEYCODE_BASE;
            for (int j = 0; j < 2; j++)
                put32(ks + (i * 2 + j) * 4, (lk >= 0 && lk < KEYMAP_MAX) ? us_keysyms[lk][j] : 0);
        }
        send_reply_var(c, 2, NULL, ks, count * 8);
        free(ks);
        break;
    }
    case 102: case 104: case 105: case 107: case 109: case 111: case 112: case 115: break;
    case 103: {                                                /* GetKeyboardControl */
        uint8_t d[24], extra[20];
        memset(d, 0, sizeof(d));
        memset(extra, 0xFF, sizeof(extra));
        d[4] = 0; d[5] = 50; put16(d + 6, 400); put16(d + 8, 100);
        memset(d + 12, 0xFF, 12);
        send_reply_var(c, 1, d, extra, 20);
        break;
    }
    case 106: {                                                /* GetPointerControl */
        uint8_t d[24];
        memset(d, 0, sizeof(d));
        put16(d, 2); put16(d + 2, 1); put16(d + 4, 4);
        send_reply(c, 0, d);
        break;
    }
    case 108: {                                                /* GetScreenSaver */
        uint8_t d[24];
        memset(d, 0, sizeof(d));
        send_reply(c, 0, d);
        break;
    }
    case 110: { uint8_t d[24]; memset(d, 0, sizeof(d)); send_reply(c, 0, d); break; }  /* ListHosts */
    case 113: {                                                /* KillClient */
        uint32_t id = r32(q + 4);
        if (id == 0) return;
        int ci = (int)(id >> CLIENT_ID_SHIFT) - 1;
        if (ci >= 0 && ci < MAX_XCLIENTS && clients[ci].used) kill_client_windows(ci);
        break;
    }
    case 114: {                                                /* RotateProperties */
        window_t *w = req_window(c, r32(q + 4));
        if (!w) return;
        int n = (int)r16(q + 8), delta = rs16(q + 10);
        if (12 + 4 * n > qlen || n == 0) return;
        prop_t **ps = malloc((size_t)n * sizeof(prop_t *));
        if (!ps) return;
        for (int i = 0; i < n; i++) {
            ps[i] = prop_find(w, r32(q + 12 + 4 * i));
            if (!ps[i]) { free(ps); x_error(c, BadMatch, 0); return; }
        }
        uint32_t *names = malloc((size_t)n * 4);
        if (names) {
            for (int i = 0; i < n; i++) names[i] = ps[i]->name;
            for (int i = 0; i < n; i++) ps[((i + delta) % n + n) % n]->name = names[i];
            for (int i = 0; i < n; i++) property_notify(w, names[i], 0);
            free(names);
        }
        free(ps);
        break;
    }
    case 116: case 118: {                                      /* Set Pointer/Modifier Mapping */
        uint8_t d[24];
        memset(d, 0, sizeof(d));
        send_reply(c, 0, d);
        break;
    }
    case 117: {                                                /* GetPointerMapping */
        uint8_t map[8] = { 1, 2, 3, 4, 5, 6, 7, 0 };
        send_reply_var(c, 7, NULL, map, 7);
        break;
    }
    case 119: {                                                /* GetModifierMapping */
        uint8_t extra[8 * KEYCODES_PER_MODIFIER];
        for (int m = 0; m < 8; m++)
            for (int k = 0; k < KEYCODES_PER_MODIFIER; k++) {
                uint8_t lk = modifier_linux_keys[m][k];
                extra[m * KEYCODES_PER_MODIFIER + k] = lk ? (uint8_t)(lk + X_KEYCODE_BASE) : 0;
            }
        send_reply_var(c, KEYCODES_PER_MODIFIER, NULL, extra, sizeof(extra));
        break;
    }
    case 127: break;                                           /* NoOperation */
    case 53: case 54: case 55: case 56: case 57: case 58: case 59: case 60:
    case 62: case 63: case 64: case 65: case 66: case 67: case 68: case 69:
    case 70: case 71: case 72: case 73: case 74: case 75: case 76: case 77:
        draw_dispatch(c, q, qlen);
        break;
    default:
        if (op == render_major) { render_dispatch(c, q, qlen); break; }
        unknown_request(c, op, qlen);
        break;
    }
}

/* ── setup and teardown ──────────────────────────────────────────────────── */
void core_init(void) {
    res_init();
    for (int i = 0; i < N_PREDEFINED; i++) {
        if (i == 0) { atom_add("", 0); continue; }
        atom_add(predefined_atoms[i], (int)strlen(predefined_atoms[i]));
    }
    root = window_new(ROOT_WINDOW, -1, NULL, 0, 0, scr_w, scr_h, 0, 1);
    root->mapped = 1;
    root->bg_mode = BG_PIXEL;
    root->bg_pixel = 0x181C24;
    root->painted = 1;
    res_add(&root->d.o);
    fill_background(root, 0, 0, scr_w, scr_h);
    /* Defaults toolkits read from the root (Xft settings, as xrdb would set). */
    static const char rm[] = "Xft.dpi:\t96\nXft.antialias:\t1\nXft.hinting:\t1\n"
                             "Xft.hintstyle:\thintslight\nXft.rgba:\tnone\n";
    prop_set(root, 23 /* RESOURCE_MANAGER */, 31 /* STRING */, 8, rm, sizeof(rm) - 1, 0);
}

/* The root changes size with maeroX's desktop window. */
void core_resize_root(int w, int h);
void core_resize_root(int w, int h) {
    if (!root) return;
    resize_backing(root, w, h);
    fill_background(root, 0, 0, w, h);
    /* As a RandR screen change does: clients that select StructureNotify
     * on the root (window managers, GTK's screen size tracking) hear of the
     * new size.  New connections get it in the setup reply's screen. */
    send_configure_notify(root);
}

static void unselect_tree(window_t *w, int ci) {
    select_input(w, ci, 0);
    for (window_t *ch = w->bottom; ch; ch = ch->above) unselect_tree(ch, ci);
}

void core_client_gone(client_t *c) {
    int ci = c->index;
    if (pgrab.win && pgrab.client == ci) pgrab.win = NULL;
    if (kgrab.win && kgrab.client == ci) kgrab.win = NULL;
    for (int i = 0; i < nsels; i++)
        if (sels[i].client == ci) { sels[i].client = -1; sels[i].window = 0; }
    /* Windows first (a window's destruction takes its subtree, whoever owns
     * it), then everything else the client created. */
    for (;;) {
        xobj_t *found = NULL;
        for (int b = 0; b < RES_BUCKETS && !found; b++)
            for (xobj_t *o = res_hash[b]; o; o = o->hnext)
                if (o->owner == ci && o->type == XT_WINDOW) { found = o; break; }
        if (!found) break;
        destroy_window((window_t *)found);
    }
    for (int b = 0; b < RES_BUCKETS; b++) {
        xobj_t *o = res_hash[b];
        while (o) {
            xobj_t *next = o->hnext;
            if (o->owner == ci) {
                switch (o->type) {
                case XT_PIXMAP:
                    res_remove(o);
                    ((drawable_t *)o)->dead = 1;
                    drawable_unref((drawable_t *)o);
                    break;
                case XT_GC: res_remove(o); gc_free((gc_t *)o); break;
                case XT_FONT: font_close(o->id); break;
                case XT_PICTURE: case XT_GLYPHSET: res_remove(o); render_free(o); break;
                default: res_remove(o); free(o); break;
                }
            }
            o = next;
        }
    }
    /* Its event selections on other clients' windows. */
    unselect_tree(root, ci);
    input_window_changed();
}
