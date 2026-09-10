#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <syscall.h>
#include <unistd.h>
#include <wm.h>          /* WM_MOD_* — the modifier mask sent to clients */

#include "font8x16.h"
#include "font_ui16.h"   /* AA proportional UI font (tools/mkfont.py) */
#include <font_mono16.h> /* AA monospace font (Menlo 16px) for the console */

#define MONO_ADV 10      /* font_mono16 is fixed-width: every glyph 10px wide */

/* Environment handed to every program the desktop spawns (terminal shell,
 * slot apps, store apps).  Mirrors what login(1) sets so GUI-terminal commands
 * behave like a real login: PATH for command lookup, HOME, and the GTK/X11
 * stack vars (LD_LIBRARY_PATH, DISPLAY, GDK_PIXBUF_MODULE_FILE) so X clients
 * launched from the terminal can find their libraries and the X server.
 * Previously these execve()s passed a NULL environment, so PATH/HOME were
 * unset and any dynamically-linked GTK/X app failed to start. */
static char *const desktop_envp[] = {
    "PATH=/disk:/disk/bin:/:/bin",
    "HOME=/home/user",          /* the desktop runs as the unprivileged user */
    "USER=user",
    "LOGNAME=user",
    "SHELL=/disk/shell",
    "TERM=linux",
    "LD_LIBRARY_PATH=/lib:/disk/lib:/disk/firefox",
    "DISPLAY=:0",
    "GDK_PIXBUF_MODULE_FILE=/disk/firefox/pixbuf-loaders/loaders.cache",
    "XDG_CACHE_HOME=/tmp",      /* fontconfig writes its cache here (tmpfs 1777) */
    "FONTCONFIG_PATH=/etc/fonts",  /* fontconfig was built with prefix=/sysroot; point
                                    * it at our real config so it finds <cachedir>. */
    (char *)0
};

#define MAX_W 1920
#define GLYPH_W 8
#define GLYPH_H 16

/* ── Modern-flat chrome metrics ──────────────────────────────────────────── */
#define TASKBAR_H    40
#define TITLEBAR_H   28
#define WIN_BTN      18   /* legacy button size (kept for layout math) */
/* Win7 caption buttons: grouped top-right, touching the top edge */
#define CAP_BTN_H   18
#define CAP_CLOSE_W 42
#define CAP_BTN_W   28
#define CAP_RIGHT   8     /* inset of the group from the window's right edge */
#define LAUNCHER_W   96   /* taskbar launcher button width */
#define MENU_W       176
#define MENU_ITEM_H  30
#define MENU_ITEMS   11
#define TB_BTN_W     48   /* Win7 icon-only taskbar button */
#define ORB_X        4    /* orb left edge on the taskbar */
#define MAX_LOG 14
#define LINE_MAX 96
#define LOG_MAX 80
#define MAX_CLIENT_WINDOWS 12
#define MAX_WINDOWS (2 + MAX_CLIENT_WINDOWS)
#define CLIENT_RECTS 24
#define CLIENT_LINES 24
#define CLIENT_ICONS 8        /* icon definitions per client */
#define CLIENT_ICON_USES 32   /* icon placements per client surface */
#define ICON_MAX 32           /* max icon dimension */
#define WM_LINE_MAX 128
#define WM_EVENTS_PATH "/tmp/wmevents"
#define TIOCGPTN 0x80045430U
#define TIOCSPTLCK 0x40045431U
#define CTRL_D 4

enum {
    WIN_TERMINAL = 0,
    WIN_STATUS = 1,
    WIN_CLIENT_BASE = 2,
};

typedef struct {
    int id;
    int x;
    int y;
    int w;
    int h;
    int visible;
    int minimized;   /* hidden from the desktop but shown in the taskbar */
    int maximized;
    int snapped;     /* Aero-snapped to a half screen */
    int sx, sy, sw, sh;   /* saved geometry for un-maximize / un-snap */
    char title[24];
} desktop_window_t;

typedef struct {
    int used;
    int x;
    int y;
    int w;
    int h;
    uint32_t color;
} client_rect_t;

typedef struct {
    uint8_t w, h;                       /* 0 = undefined */
    uint8_t pix[ICON_MAX * ICON_MAX];   /* palette index per pixel, 0=clear */
} client_icon_t;

typedef struct {
    int used;
    int icon;
    int x;
    int y;
} icon_use_t;

typedef struct {
    uint32_t bg;
    client_rect_t rects[CLIENT_RECTS];
    icon_use_t icons[CLIENT_ICON_USES];
    char lines[CLIENT_LINES][LOG_MAX];
    uint32_t line_color[CLIENT_LINES];
    int line_x[CLIENT_LINES];
    int line_y[CLIENT_LINES];
    /* Pixel surface (modern path): client-rendered shared-memory buffer.
     * When present it replaces the retained rect/text/icon content. */
    uint32_t *surf;
    int surf_w;
    int surf_h;
    int surf_shm;   /* shm id, -1 = none */
} client_surface_t;

/* MaeroOS shared-memory syscalls */
#define SYS_SHM_CREATE 500
#define SYS_SHM_MAP    501
#define SYS_SHM_UNMAP  502

#define TERM_LINES 28   /* terminal scrollback (display tail) */

static unsigned fb_w, fb_h, fb_pitch;
static int fb_fd = -1;
/*
 * Double buffering: all drawing targets `row`, which points either into the
 * full-screen `backbuf` (one scanline at a time) or, if the back buffer could
 * not be allocated, into `fallback_row` for the legacy direct-write path.
 * Compositing into RAM and presenting once per frame eliminates the flicker
 * and tearing of the old draw-straight-to-/dev/fb0 approach.
 */
static uint32_t fallback_row[MAX_W];
static uint32_t *row = fallback_row;
static uint32_t *backbuf;
/* Last frame written to the screen, so present() can skip unchanged scanlines.
 * shadow_valid is cleared whenever something other than us draws (a fullscreen
 * app), which forces the next present to be a full blit. */
static uint32_t *shadow;
static int       shadow_valid;
static void present_invalidate(void) { shadow_valid = 0; }
/* Desktop event log (shown in the System window). */
static char log_lines[MAX_LOG][LOG_MAX];
static int log_count;
/* Terminal scrollback: pure shell PTY output, nothing else. */
static char term_lines[TERM_LINES][LOG_MAX];
static int term_count;
static char output_line[LOG_MAX];   /* live (unterminated) PTY output line */
static int output_used;
static int esc_state;               /* ANSI filter: 0 none, 1 ESC, 2 CSI */
static int shift_down;
static int ctrl_down;
static int caps_on;
static int running = 1;

static const char *preferred_shell_path(void) {
    return access("/disk/shell", X_OK) == 0 ? "/disk/shell" : "/shell";
}

static const char *preferred_term_path(void) {
    return access("/disk/term", X_OK) == 0 ? "/disk/term" : "/term";
}

static const char *preferred_browse_path(void) {
    return access("/disk/browse", X_OK) == 0 ? "/disk/browse" : "/browse";
}

static const char *preferred_uidemo_path(void) {
    return access("/disk/uidemo", X_OK) == 0 ? "/disk/uidemo" : "/uidemo";
}

static const char *preferred_files_path(void) {
    return access("/disk/files", X_OK) == 0 ? "/disk/files" : "/files";
}

static int mouse_fd = -1;
static int mouse_x;
static int mouse_y;
static int mouse_buttons;
static int prev_mouse_buttons;
static int shell_pid = -1;
static int shell_fd = -1;
/* One owning pid per client slot (-1 = free); apps get their slot via argv */
static int client_pids[MAX_CLIENT_WINDOWS] = { [0 ... MAX_CLIENT_WINDOWS - 1] = -1 };
static int wm_fd = -1;
static int wm_keepalive_fd = -1;
/* One event FIFO per client slot: a shared channel lets concurrent clients
 * steal each other's events (first reader consumes the line). */
static int wm_event_fds[MAX_CLIENT_WINDOWS] = { [0 ... MAX_CLIENT_WINDOWS - 1] = -1 };
static int wm_event_keepalives[MAX_CLIENT_WINDOWS] = { [0 ... MAX_CLIENT_WINDOWS - 1] = -1 };
static desktop_window_t windows[MAX_WINDOWS];
static int window_count;
static int active_window = WIN_TERMINAL;
static int drag_mode;
static int drag_win_id = -1;
static int drag_dx;
static int drag_dy;
static int drag_edges;            /* RESIZE_L/R/T/B mask during DRAG_RESIZE */
static int drag_right, drag_bottom;   /* anchored opposite edges */
static char wm_line[WM_LINE_MAX];
static int wm_line_used;
static char client_status[LOG_MAX] = "WMCTL READY";
static char client_text[MAX_CLIENT_WINDOWS][LOG_MAX];
static client_surface_t client_surfaces[MAX_CLIENT_WINDOWS];
/* Icon definitions persist across surface clears (apps define once). */
static client_icon_t client_icons[MAX_CLIENT_WINDOWS][CLIENT_ICONS];
static int launcher_open;        /* taskbar launcher menu visible */
static int clock_secs = -1;      /* last uptime second rendered in the clock */
static int term_view;            /* terminal scrollback offset (0 = bottom) */
static int client_drag_slot;     /* client receiving drag motion (0 = none) */
static int alt_down;             /* Alt held (Alt-Tab window switching) */
static uint32_t *wallpaper;      /* optional PPM wallpaper (fb-sized rows) */
static int wallpaper_w, wallpaper_h;
static uint32_t *wallpaper_blur; /* blurred copy: the Aero glass backdrop */

/* Aero orb art: 44x44, 3 states (normal/hover/pressed); 0 = transparent */
#define ORB_SIZE 44
static uint32_t orb_px[3][ORB_SIZE * ORB_SIZE];

/* Taskbar hover thumbnail (live preview card) */
#define THUMB_W 180
#define THUMB_H 120
#define THUMB_CARD_W (THUMB_W + 16)
#define THUMB_CARD_H (THUMB_H + 40)
static int thumb_win_id = -1;     /* window previewed (-1 = none) */
static int thumb_x;               /* card left edge */

/* Desktop gadgets (right edge; drawn beneath windows) */
#define GADGET_W 110
#define GADGET_X ((int)fb_w - GADGET_W - 14)
static int gadgets_visible = 1;
static int show_desktop_active;
static int sd_saved[MAX_WINDOWS];   /* minimized-state snapshot */
static char note_text[160];       /* sticky note (persisted) */
static int note_len;
static int note_focus;            /* keyboard goes to the note */

#define DRAG_MOVE   1
#define DRAG_RESIZE 2

static void reset_client_surface(int idx);
static void drop_client_pixels(int idx);
static void emit_client_focus_event(int id);
static void emit_client_geom_event(const desktop_window_t *win);
static void emit_client_close_event(int idx);
static void copy_text(char *dst, unsigned size, const char *src);
static void focus_client_app(int idx);
static void load_desktop_conf(void);
static void load_wallpaper(void);
static void civil_from_days(long z, int *yy, unsigned *mm, unsigned *dd);
static void note_save(void);
static int in_note(int x, int y);
static void scan_installed_apps(void);
static void blank_framebuffer(void);
#define MAX_INST_APPS 12
typedef struct {
    char name[32];
    char exec[96];
    char args[96];
    int fullscreen;
    int rawinput;   /* app reads /dev/input/event0 itself (e.g. DOOM) —
                     * the desktop must NOT touch event0 while it runs. */
} inst_app_t;
extern inst_app_t inst_apps[MAX_INST_APPS];
extern int inst_app_count;
extern const char *const month_names[12];
static int taskbar_buttons(int *ids, int *xs, int max);

static uint32_t rgb(unsigned r, unsigned g, unsigned b) {
    return (r << 16) | (g << 8) | b;
}

/* ── Modern-flat palette ─────────────────────────────────────────────────── */
#define COL_WALL_TOP     rgb(52, 60, 76)    /* wallpaper gradient start    */
#define COL_WALL_BOT     rgb(22, 26, 36)    /* wallpaper gradient end      */
#define COL_TASKBAR      rgb(18, 21, 29)
#define COL_TASKBAR_EDGE rgb(62, 70, 88)
static uint32_t col_accent = 0x5E81AC;  /* active title/borders (desktop.conf) */
#define COL_INACTIVE     rgb(49, 54, 66)    /* inactive title bar          */
#define COL_TEXT         rgb(220, 226, 235)
#define COL_TEXT_DIM     rgb(140, 148, 162)
#define COL_CLOSE        rgb(191, 97, 106)
#define COL_MIN          rgb(76, 86, 106)
#define COL_BTN_HOVER    rgb(46, 54, 70)
#define COL_MENU_BG      rgb(30, 34, 44)

/* Linear blend a→b by t/255 (per channel). */
static uint32_t mix_color(uint32_t a, uint32_t b, unsigned t) {
    int ar = (int)((a >> 16) & 0xff), ag = (int)((a >> 8) & 0xff), ab = (int)(a & 0xff);
    int br = (int)((b >> 16) & 0xff), bg = (int)((b >> 8) & 0xff), bb = (int)(b & 0xff);
    int ti = (int)t;
    return rgb((unsigned)(ar + (br - ar) * ti / 255),
               (unsigned)(ag + (bg - ag) * ti / 255),
               (unsigned)(ab + (bb - ab) * ti / 255));
}

/* 16-color icon palette; index 0 = transparent. */
static uint32_t icon_palette(uint8_t idx) {
    switch (idx) {
    case 1:  return rgb(20, 24, 26);     /* black      */
    case 2:  return rgb(238, 240, 235);  /* white      */
    case 3:  return rgb(102, 110, 112);  /* gray       */
    case 4:  return rgb(205, 83, 73);    /* red        */
    case 5:  return rgb(62, 156, 108);   /* green      */
    case 6:  return rgb(36, 127, 174);   /* blue       */
    case 7:  return rgb(255, 214, 88);   /* yellow     */
    case 8:  return rgb(85, 190, 205);   /* cyan       */
    case 9:  return rgb(94, 129, 172);   /* accent     */
    case 10: return rgb(40, 44, 52);     /* dark       */
    case 11: return rgb(222, 144, 70);   /* orange     */
    case 12: return rgb(166, 120, 195);  /* purple     */
    case 13: return rgb(140, 100, 60);   /* brown      */
    case 14: return rgb(200, 204, 210);  /* light gray */
    case 15: return rgb(60, 64, 72);     /* dark gray  */
    }
    return 0;
}

/* Blit one scanline of an icon, clipped to [cx,cx+cw)×[cy,cy+ch). */
static void draw_client_icon_on_row(unsigned y, const client_icon_t *ic,
                                    int ix, int iy,
                                    int cx, int cy, int cw, int ch) {
    int ry, cx2;

    if (!ic->w || !ic->h) return;
    if ((int)y < iy || (int)y >= iy + ic->h) return;
    if ((int)y < cy || (int)y >= cy + ch) return;
    ry = (int)y - iy;
    cx2 = cx + cw;
    if (cx < 0) cx = 0;
    if (cx2 > (int)fb_w) cx2 = (int)fb_w;
    if (cx2 > MAX_W) cx2 = MAX_W;
    for (int px = 0; px < ic->w; px++) {
        int dx = ix + px;
        uint8_t v = ic->pix[ry * ic->w + px];
        if (!v) continue;
        if (dx >= cx && dx < cx2)
            row[dx] = icon_palette(v);
    }
}

/* Darken existing row pixels (legacy hard shadow; kept for reference). */
static void shade_rect_on_row(unsigned y, int x, int ry, int w, int h)
    __attribute__((unused));
static void shade_rect_on_row(unsigned y, int x, int ry, int w, int h) {
    if ((int)y < ry || (int)y >= ry + h) return;
    int start = x < 0 ? 0 : x;
    int end = x + w;
    if (end > (int)fb_w) end = (int)fb_w;
    if (end > MAX_W) end = MAX_W;
    for (int px = start; px < end; px++) {
        uint32_t c = row[px];
        row[px] = ((c >> 1) & 0x7f7f7f) + ((c >> 2) & 0x3f3f3f); /* ~75% */
    }
}

/* ── Alpha compositing helpers (the 2017 look) ───────────────────────────── */

/* Blend color over dst at opacity a (0..255). */
static uint32_t blend_px(uint32_t dst, uint32_t color, unsigned a) {
    unsigned ia = 255 - a;
    unsigned r = (((color >> 16) & 0xff) * a + ((dst >> 16) & 0xff) * ia) >> 8;
    unsigned g = (((color >> 8) & 0xff) * a + ((dst >> 8) & 0xff) * ia) >> 8;
    unsigned b = ((color & 0xff) * a + (dst & 0xff) * ia) >> 8;
    return rgb(r, g, b);
}

/* Translucent filled rect on the current scanline. */
static void blend_rect_on_row(unsigned y, int x, int ry, int w, int h,
                              uint32_t color, unsigned alpha) {
    if ((int)y < ry || (int)y >= ry + h) return;
    int start = x < 0 ? 0 : x;
    int end = x + w;
    if (end > (int)fb_w) end = (int)fb_w;
    if (end > MAX_W) end = MAX_W;
    for (int px = start; px < end; px++)
        row[px] = blend_px(row[px], color, alpha);
}

/* ── Themed full-color icons (.mic, from tools/mkicons.py) ──────────────── */
/* Each icon is loaded once into a malloc'd 0xAARRGGBB array and blended onto
 * the current scanline.  Falls back to the legacy hex-art when a theme icon
 * is missing (REMOVE NOTHING). */
typedef struct {
    char     name[24];
    int      w, h;
    uint32_t *px;     /* 0xAARRGGBB, NULL if not loaded / missing */
    int      tried;
} themed_icon_t;

#define MAX_THEMED_ICONS 48
static themed_icon_t themed_icons[MAX_THEMED_ICONS];
static int themed_icon_count;

static themed_icon_t *load_themed_icon(const char *name) {
    for (int i = 0; i < themed_icon_count; i++)
        if (!strcmp(themed_icons[i].name, name)) return &themed_icons[i];
    if (themed_icon_count >= MAX_THEMED_ICONS) return NULL;

    themed_icon_t *ic = &themed_icons[themed_icon_count++];
    strncpy(ic->name, name, sizeof(ic->name) - 1);
    ic->tried = 1;

    char path[64];
    snprintf(path, sizeof(path), "/disk/icons/%s.mic", name);
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        snprintf(path, sizeof(path), "/icons/%s.mic", name);
        fd = open(path, O_RDONLY);
    }
    if (fd < 0) return ic;   /* leave px NULL → caller uses hex-art */

    unsigned char hdr[8];
    if (read(fd, hdr, 8) != 8 ||
        (uint32_t)(hdr[0] | (hdr[1] << 8) | (hdr[2] << 16) |
                   ((uint32_t)hdr[3] << 24)) != 0x3143494DU) {
        close(fd);
        return ic;
    }
    int w = hdr[4] | (hdr[5] << 8), h = hdr[6] | (hdr[7] << 8);
    if (w <= 0 || h <= 0 || w > 256 || h > 256) { close(fd); return ic; }
    uint32_t *px = (uint32_t *)malloc((size_t)w * h * 4);
    if (!px) { close(fd); return ic; }
    unsigned char *buf = (unsigned char *)px;
    size_t need = (size_t)w * h * 4, got = 0;
    while (got < need) {
        int n = read(fd, buf + got, (int)(need - got));
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(fd);
    if (got != need) { free(px); return ic; }
    for (int i = w * h - 1; i >= 0; i--) {
        unsigned char r = buf[i * 4], g = buf[i * 4 + 1],
                      b = buf[i * 4 + 2], a = buf[i * 4 + 3];
        px[i] = ((uint32_t)a << 24) | ((uint32_t)r << 16) |
                ((uint32_t)g << 8) | b;
    }
    ic->w = w; ic->h = h; ic->px = px;
    return ic;
}

/* Blend a themed icon's current scanline, nearest-scaled to dw x dh at (ix,iy).
 * Returns 1 if the icon was drawable (so callers can skip the hex-art path). */
/*
 * Blit a themed RGBA icon scaled to dw×dh using premultiplied bilinear
 * filtering — smooth at any size (no nearest-neighbor "pixelation"), with no
 * dark fringing at transparent edges.  Weights are 4-bit fixed point (0..16).
 */
static int themed_icon_on_row(unsigned y, int ix, int iy, int dw, int dh,
                              const themed_icon_t *ic) {
    if (!ic || !ic->px || dw <= 0 || dh <= 0) return 0;
    if ((int)y < iy || (int)y >= iy + dh) return 1;  /* claimed, nothing here */
    int sw = ic->w, sh = ic->h;

    /* source row pair for this destination scanline (src_y in 4-bit fixed) */
    int syf = (((int)y - iy) * 2 + 1) * sh * 8 / dh - 8;
    int maxsy = (sh - 1) * 16;
    if (syf < 0) syf = 0;
    if (syf > maxsy) syf = maxsy;
    int y0 = syf >> 4, wy1 = syf & 15, wy0 = 16 - wy1;
    int y1 = y0 + 1 < sh ? y0 + 1 : y0;
    const uint32_t *r0 = ic->px + (size_t)y0 * sw;
    const uint32_t *r1 = ic->px + (size_t)y1 * sw;

    int end = ix + dw;
    if (end > (int)fb_w) end = (int)fb_w;
    if (end > MAX_W) end = MAX_W;
    int maxsx = (sw - 1) * 16;

    for (int px = ix < 0 ? 0 : ix; px < end; px++) {
        int sxf = ((px - ix) * 2 + 1) * sw * 8 / dw - 8;
        if (sxf < 0) sxf = 0;
        if (sxf > maxsx) sxf = maxsx;
        int x0 = sxf >> 4, wx1 = sxf & 15, wx0 = 16 - wx1;
        int x1 = x0 + 1 < sw ? x0 + 1 : x0;

        uint32_t p00 = r0[x0], p01 = r0[x1], p10 = r1[x0], p11 = r1[x1];
        int w00 = wx0 * wy0, w01 = wx1 * wy0,
            w10 = wx0 * wy1, w11 = wx1 * wy1;        /* Σ = 256 */

        int a00 = p00 >> 24, a01 = p01 >> 24, a10 = p10 >> 24, a11 = p11 >> 24;
        int a = (a00 * w00 + a01 * w01 + a10 * w10 + a11 * w11) >> 8;
        if (!a) continue;

        /* premultiplied (channel*alpha) bilinear, then composite over dst */
        int pr = (((p00 >> 16 & 0xFF) * a00) * w00 +
                  ((p01 >> 16 & 0xFF) * a01) * w01 +
                  ((p10 >> 16 & 0xFF) * a10) * w10 +
                  ((p11 >> 16 & 0xFF) * a11) * w11) >> 8;
        int pg = (((p00 >> 8 & 0xFF) * a00) * w00 +
                  ((p01 >> 8 & 0xFF) * a01) * w01 +
                  ((p10 >> 8 & 0xFF) * a10) * w10 +
                  ((p11 >> 8 & 0xFF) * a11) * w11) >> 8;
        int pb = (((p00 & 0xFF) * a00) * w00 +
                  ((p01 & 0xFF) * a01) * w01 +
                  ((p10 & 0xFF) * a10) * w10 +
                  ((p11 & 0xFF) * a11) * w11) >> 8;

        uint32_t d = row[px];
        int inv = 255 - a;
        int orr = (pr + ((int)(d >> 16 & 0xFF)) * inv) / 255;
        int og  = (pg + ((int)(d >> 8  & 0xFF)) * inv) / 255;
        int ob  = (pb + ((int)(d       & 0xFF)) * inv) / 255;
        if (orr > 255) orr = 255;
        if (og  > 255) og  = 255;
        if (ob  > 255) ob  = 255;
        row[px] = ((uint32_t)orr << 16) | ((uint32_t)og << 8) | (uint32_t)ob;
    }
    return 1;
}

/* ── Aero glass ─────────────────────────────────────────────────────────── */

/*
 * Glass samples a pre-blurred copy of the wallpaper (built once at startup)
 * instead of blurring the live frame — same frosted look, zero per-frame
 * blur cost.  (Real Win7 blurs what's beneath; this is the classic cheat.)
 */
static uint32_t glass_sample(int x, int y) {
    if (!wallpaper_blur) return COL_WALL_BOT;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= (int)fb_w) x = (int)fb_w - 1;
    if (y >= (int)fb_h) y = (int)fb_h - 1;
    return wallpaper_blur[(size_t)y * fb_w + x];
}

/* Frosted rect on this scanline: blurred backdrop + tint + soft gradient. */
static void glass_rect_on_row(unsigned y, int x, int ry, int w, int h,
                              uint32_t tint, unsigned alpha) {
    int dy, start, end;
    unsigned a;

    if ((int)y < ry || (int)y >= ry + h) return;
    dy = (int)y - ry;
    /* vertical sheen: brighter top third, subtle dark base */
    a = alpha;
    start = x < 0 ? 0 : x;
    end = x + w;
    if (end > (int)fb_w) end = (int)fb_w;
    if (end > MAX_W) end = MAX_W;
    for (int px = start; px < end; px++) {
        uint32_t g = blend_px(glass_sample(px, (int)y), tint, a);
        if (h > 4 && dy < h / 3)
            g = blend_px(g, 0xFFFFFF, (unsigned)(46 - dy * 40 / (h / 3 + 1)));
        row[px] = g;
    }
}

/* One-time box blur of the wallpaper (radius ~10, two passes). */
static void make_wallpaper_blur(void) {
    int w = (int)fb_w, h = (int)fb_h;
    uint32_t *tmp;

    wallpaper_blur = (uint32_t *)malloc((size_t)w * h * 4);
    tmp = (uint32_t *)malloc((size_t)w * h * 4);
    if (!wallpaper_blur || !tmp) {
        if (tmp) free(tmp);
        if (wallpaper_blur) { free(wallpaper_blur); wallpaper_blur = 0; }
        return;
    }
    /* source: wallpaper image or the gradient fallback */
    for (int yy = 0; yy < h; yy++) {
        for (int xx = 0; xx < w; xx++) {
            uint32_t c;
            if (wallpaper && yy < wallpaper_h && xx < wallpaper_w)
                c = wallpaper[(size_t)yy * wallpaper_w + xx];
            else
                c = mix_color(COL_WALL_TOP, COL_WALL_BOT,
                              (unsigned)(yy * 255 / (h ? h : 1)));
            tmp[(size_t)yy * w + xx] = c;
        }
    }
#define BLUR_R 10
    /* horizontal pass: sliding sum */
    for (int yy = 0; yy < h; yy++) {
        uint32_t *src = tmp + (size_t)yy * w;
        uint32_t *dst = wallpaper_blur + (size_t)yy * w;
        unsigned sr = 0, sg = 0, sb = 0, n = 0;
        for (int xx = -BLUR_R; xx <= BLUR_R && xx < w; xx++) {
            uint32_t c = src[xx < 0 ? 0 : xx];
            sr += (c >> 16) & 0xff; sg += (c >> 8) & 0xff; sb += c & 0xff;
            n++;
        }
        for (int xx = 0; xx < w; xx++) {
            dst[xx] = ((sr / n) << 16) | ((sg / n) << 8) | (sb / n);
            {
                int add = xx + BLUR_R + 1, drop = xx - BLUR_R;
                uint32_t ca = src[add >= w ? w - 1 : add];
                uint32_t cd = src[drop < 0 ? 0 : drop];
                sr += ((ca >> 16) & 0xff) - ((cd >> 16) & 0xff);
                sg += ((ca >> 8) & 0xff) - ((cd >> 8) & 0xff);
                sb += (ca & 0xff) - (cd & 0xff);
            }
        }
    }
    /* vertical pass (read blur, write tmp, copy back) */
    for (int xx = 0; xx < w; xx++) {
        unsigned sr = 0, sg = 0, sb = 0, n = 0;
        for (int yy = -BLUR_R; yy <= BLUR_R && yy < h; yy++) {
            uint32_t c = wallpaper_blur[(size_t)(yy < 0 ? 0 : yy) * w + xx];
            sr += (c >> 16) & 0xff; sg += (c >> 8) & 0xff; sb += c & 0xff;
            n++;
        }
        for (int yy = 0; yy < h; yy++) {
            tmp[(size_t)yy * w + xx] =
                ((sr / n) << 16) | ((sg / n) << 8) | (sb / n);
            {
                int add = yy + BLUR_R + 1, drop = yy - BLUR_R;
                uint32_t ca = wallpaper_blur[(size_t)(add >= h ? h - 1 : add) * w + xx];
                uint32_t cd = wallpaper_blur[(size_t)(drop < 0 ? 0 : drop) * w + xx];
                sr += ((ca >> 16) & 0xff) - ((cd >> 16) & 0xff);
                sg += ((ca >> 8) & 0xff) - ((cd >> 8) & 0xff);
                sb += (ca & 0xff) - (cd & 0xff);
            }
        }
    }
    memcpy(wallpaper_blur, tmp, (size_t)w * h * 4);
    free(tmp);
}

/* Integer sqrt for orb shading. */
static int isqrt(int v) {
    int r = 0;
    while ((r + 1) * (r + 1) <= v) r++;
    return r;
}

/* Procedurally paint the glossy Aero orb (3 hover states). */
static void make_orb(void) {
    const int C = ORB_SIZE / 2, R = ORB_SIZE / 2 - 2;

    for (int st = 0; st < 3; st++) {
        for (int yy = 0; yy < ORB_SIZE; yy++) {
            for (int xx = 0; xx < ORB_SIZE; xx++) {
                int dx = xx - C, dy = yy - C;
                int d2 = dx * dx + dy * dy;
                uint32_t c = 0;
                if (d2 <= R * R) {
                    int d = isqrt(d2);
                    /* deep blue sphere, brighter when hovered/pressed */
                    int base = 60 + (R - d) * 3 + st * 22;
                    int rr = base / 4, gg = base / 2 + 10, bb = base + 40;
                    /* top specular highlight */
                    int hx = xx - C, hy = yy - (C - 7);
                    if (hx * hx + 3 * hy * hy < 110) {
                        rr += 80; gg += 90; bb += 90;
                    }
                    /* rim darkening */
                    if (d > R - 3) { rr = rr / 2; gg = gg / 2; bb = bb / 2; }
                    if (rr > 255) rr = 255;
                    if (gg > 255) gg = 255;
                    if (bb > 255) bb = 255;
                    c = 0xFF000000u | ((uint32_t)rr << 16) |
                        ((uint32_t)gg << 8) | (uint32_t)bb;
                }
                orb_px[st][yy * ORB_SIZE + xx] = c;
            }
        }
        /* the four-color flag: 2x2 wavy panes around the center */
        {
            static const uint32_t pane[4] = {
                0xFFE0452C, 0xFF7DBF3C, 0xFF2C9BE0, 0xFFF3C722,
            };
            for (int yy = -8; yy < 8; yy++) {
                for (int xx = -8; xx < 8; xx++) {
                    int q = (yy < 0 ? 0 : 2) + (xx < 0 ? 0 : 1);
                    int px_x, px_y;
                    if (xx == -1 || xx == 0 || yy == -1 || yy == 0)
                        continue;          /* 2px gap between panes */
                    px_x = C + xx;
                    px_y = C + yy + ((xx * xx) / 40) - 1;
                    if (px_x < 0 || px_x >= ORB_SIZE ||
                        px_y < 0 || px_y >= ORB_SIZE)
                        continue;
                    if (orb_px[st][px_y * ORB_SIZE + px_x])
                        orb_px[st][px_y * ORB_SIZE + px_x] = pane[q];
                }
            }
        }
    }
}

/*
 * Soft drop shadow: a SHADOW_R-pixel falloff ring around the window rect,
 * shifted slightly down.  Chebyshev distance keeps it cheap per scanline;
 * the interior (d == 0) is skipped — the window fill overdraws it anyway.
 */
#define SHADOW_R 8
static const unsigned char shadow_alpha[SHADOW_R + 1] = {
    0, 84, 62, 44, 30, 19, 11, 5, 2
};

static void soft_shadow_on_row(unsigned y, int wx, int wy, int ww, int wh) {
    int sy = wy + 3;                       /* shadow biased downward */
    int dy = 0;

    if ((int)y < sy - SHADOW_R || (int)y >= sy + wh + SHADOW_R) return;
    if ((int)y < sy) dy = sy - (int)y;
    else if ((int)y >= sy + wh) dy = (int)y - (sy + wh) + 1;

    int start = wx - SHADOW_R, end = wx + ww + SHADOW_R;
    if (start < 0) start = 0;
    if (end > (int)fb_w) end = (int)fb_w;
    if (end > MAX_W) end = MAX_W;
    for (int px = start; px < end; px++) {
        int dx = 0;
        if (px < wx) dx = wx - px;
        else if (px >= wx + ww) dx = px - (wx + ww) + 1;
        int d = dx > dy ? dx : dy;
        if (d == 0 || d > SHADOW_R) continue;
        row[px] = blend_px(row[px], 0, shadow_alpha[d]);
    }
}

static int in_rect(unsigned x, unsigned y, unsigned rx, unsigned ry,
                   unsigned rw, unsigned rh) {
    return x >= rx && y >= ry && x < rx + rw && y < ry + rh;
}

static int is_client_window(int id) {
    return id >= WIN_CLIENT_BASE && id < WIN_CLIENT_BASE + MAX_CLIENT_WINDOWS;
}

static int client_index_for_window(int id) {
    return is_client_window(id) ? id - WIN_CLIENT_BASE : -1;
}

static int parse_client_index(const char **p) {
    int v = 0, any = 0;
    const char *s = *p;

    while (*s == ' ') s++;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        any = 1;
        s++;
    }
    if (!any || v < 1 || v > MAX_CLIENT_WINDOWS) return -1;
    if (*s && *s != ' ') return -1;
    while (*s == ' ') s++;
    *p = s;
    return v - 1;
}

static desktop_window_t *find_window(int id) {
    for (int i = 0; i < window_count; i++) {
        if (windows[i].id == id) return &windows[i];
    }
    return (desktop_window_t *)0;
}

static void raise_window(int id) {
    desktop_window_t win;
    int idx = -1;

    for (int i = 0; i < window_count; i++) {
        if (windows[i].id == id) {
            idx = i;
            break;
        }
    }
    if (idx < 0 || idx == window_count - 1) return;
    win = windows[idx];
    for (int i = idx; i + 1 < window_count; i++)
        windows[i] = windows[i + 1];
    windows[window_count - 1] = win;
}

static void focus_window(int id) {
    if (find_window(id)) {
        active_window = id;
        raise_window(id);
        emit_client_focus_event(id);
    }
}

static void clamp_window(desktop_window_t *win) {
    int min_w = 240;
    int bottom = (int)fb_h - TASKBAR_H;   /* keep windows above the taskbar */
    int min_h;
    int max_w;
    int max_h;

    if (!win) return;
    min_h = win->id == WIN_TERMINAL ? 180 : 86;
    max_w = (int)fb_w - 16;
    max_h = bottom - 16;
    if (max_w < min_w) max_w = min_w;
    if (max_h < min_h) max_h = min_h;
    if (win->w < min_w) win->w = min_w;
    if (win->h < min_h) win->h = min_h;
    if (win->w > max_w) win->w = max_w;
    if (win->h > max_h) win->h = max_h;
    if (win->x < 8) win->x = 8;
    if (win->y < 8) win->y = 8;
    if (win->x + win->w > (int)fb_w - 8)
        win->x = (int)fb_w - 8 - win->w;
    if (win->y + win->h > bottom - 8)
        win->y = bottom - 8 - win->h;
}

static void add_window(int id, int x, int y, int w, int h, const char *title) {
    desktop_window_t *win;

    if (window_count >= MAX_WINDOWS) return;
    win = &windows[window_count++];
    win->id = id;
    win->x = x;
    win->y = y;
    win->w = w;
    win->h = h;
    win->visible = 1;
    win->minimized = 0;
    win->maximized = 0;
    strncpy(win->title, title, sizeof(win->title) - 1);
    win->title[sizeof(win->title) - 1] = 0;
}

static void init_windows(void) {
    int avail_h = (int)fb_h - TASKBAR_H;
    int width = (int)fb_w - 48;
    int status_h = 156;
    int term_h = avail_h - status_h - 56;
    if (term_h < 160) term_h = 160;

    window_count = 0;
    add_window(WIN_TERMINAL, 24, 16, width, term_h, "Console");
    add_window(WIN_STATUS, 24, 28 + term_h, width, status_h, "System");
    for (int i = 0; i < MAX_CLIENT_WINDOWS; i++) {
        char title[24];
        desktop_window_t *client;
        sprintf(title, "App %d", i + 1);
        add_window(WIN_CLIENT_BASE + i, 80 + i * 28, 64 + i * 32,
                   width > 200 ? width - 160 : width, 320, title);
        client = find_window(WIN_CLIENT_BASE + i);
        if (client) client->visible = 0;
        sprintf(client_text[i], "CLIENT %d READY", i + 1);
        reset_client_surface(i);
    }
    active_window = WIN_TERMINAL;
}

static desktop_window_t *window_at(int x, int y) {
    for (int i = window_count - 1; i >= 0; i--) {
        desktop_window_t *win = &windows[i];
        if (!win->visible || win->minimized) continue;
        if (in_rect((unsigned)x, (unsigned)y, (unsigned)win->x,
                    (unsigned)win->y, (unsigned)win->w, (unsigned)win->h))
            return win;
    }
    return (desktop_window_t *)0;
}

static int in_titlebar(const desktop_window_t *win, int x, int y) {
    if (!win || !win->visible || win->minimized) return 0;
    return in_rect((unsigned)x, (unsigned)y, (unsigned)win->x,
                   (unsigned)win->y, (unsigned)win->w, TITLEBAR_H);
}

static int in_close_button(const desktop_window_t *win, int x, int y) {
    if (!win || !win->visible || win->minimized) return 0;
    return in_rect((unsigned)x, (unsigned)y,
                   (unsigned)(win->x + win->w - CAP_RIGHT - CAP_CLOSE_W),
                   (unsigned)(win->y + 1), CAP_CLOSE_W, CAP_BTN_H);
}

static int in_max_button(const desktop_window_t *win, int x, int y) {
    if (!win || !win->visible || win->minimized) return 0;
    return in_rect((unsigned)x, (unsigned)y,
                   (unsigned)(win->x + win->w - CAP_RIGHT - CAP_CLOSE_W -
                              CAP_BTN_W),
                   (unsigned)(win->y + 1), CAP_BTN_W, CAP_BTN_H);
}

static int in_min_button(const desktop_window_t *win, int x, int y) {
    if (!win || !win->visible || win->minimized) return 0;
    return in_rect((unsigned)x, (unsigned)y,
                   (unsigned)(win->x + win->w - CAP_RIGHT - CAP_CLOSE_W -
                              2 * CAP_BTN_W),
                   (unsigned)(win->y + 1), CAP_BTN_W, CAP_BTN_H);
}

static void toggle_maximize(desktop_window_t *win) {
    if (!win) return;
    if (!win->maximized) {
        if (!win->snapped) {     /* a snapped window's true size is saved */
            win->sx = win->x;
            win->sy = win->y;
            win->sw = win->w;
            win->sh = win->h;
        }
        win->x = 8;
        win->y = 8;
        win->w = (int)fb_w - 16;
        win->h = (int)fb_h - TASKBAR_H - 16;
        win->maximized = 1;
        win->snapped = 0;
    } else {
        win->x = win->sx;
        win->y = win->sy;
        win->w = win->sw;
        win->h = win->sh;
        win->maximized = 0;
        win->snapped = 0;
    }
    clamp_window(win);
    emit_client_geom_event(win);   /* surface apps re-create their buffer */
}

static int in_resize_grip(const desktop_window_t *win, int x, int y) {
    if (!win || !win->visible || win->minimized) return 0;
    return in_rect((unsigned)x, (unsigned)y,
                   (unsigned)(win->x + win->w - 18),
                   (unsigned)(win->y + win->h - 18), 18, 18);
}

/* Which edges a point grabs for resizing (6px zones; top zone is 4px so
 * the title bar stays draggable).  The legacy SE grip maps to R|B. */
#define RESIZE_L 1
#define RESIZE_R 2
#define RESIZE_T 4
#define RESIZE_B 8

static int resize_edges_at(const desktop_window_t *win, int x, int y) {
    int e = 0;

    if (!win || !win->visible || win->minimized) return 0;
    if (win->maximized) return 0;
    if (x < win->x || x >= win->x + win->w ||
        y < win->y || y >= win->y + win->h) return 0;
    if (x < win->x + 6) e |= RESIZE_L;
    if (x >= win->x + win->w - 6) e |= RESIZE_R;
    if (y < win->y + 4) e |= RESIZE_T;
    if (y >= win->y + win->h - 6) e |= RESIZE_B;
    /* Edges only grab near the border; the 4 corners get a bigger zone. */
    if ((e & (RESIZE_L | RESIZE_R)) && (e & (RESIZE_T | RESIZE_B)))
        return e;
    if (in_resize_grip(win, x, y)) return RESIZE_R | RESIZE_B;
    return e;
}

static uint8_t glyph_row(unsigned char ch, int rowi) {
    if (rowi < 0 || rowi >= GLYPH_H) return 0;
    if (ch >= 128) ch = 0;            /* non-ASCII → blank */
    return font8x16[ch][rowi];
}

static void draw_text_on_row(unsigned y, unsigned tx, unsigned ty,
                             const char *s, uint32_t color, unsigned scale) {
    if (y < ty || y >= ty + GLYPH_H * scale) return;
    unsigned gy = (y - ty) / scale;
    unsigned x = tx;
    for (; *s; s++, x += GLYPH_W * scale) {
        uint8_t bits = glyph_row((unsigned char)*s, (int)gy);
        for (unsigned col = 0; col < GLYPH_W; col++) {
            if (!(bits & (0x80u >> col))) continue;
            unsigned px = x + col * scale;
            for (unsigned sx = 0; sx < scale; sx++) {
                if (px + sx < fb_w && px + sx < MAX_W) row[px + sx] = color;
            }
        }
    }
}

static void draw_text_clip_on_row(unsigned y, int tx, int ty, const char *s,
                                  uint32_t color, unsigned scale,
                                  int cx, int cy, int cw, int ch) {
    if ((int)y < cy || (int)y >= cy + ch) return;
    if ((int)y < ty || (int)y >= ty + (int)(GLYPH_H * scale)) return;

    unsigned gy = (unsigned)((int)y - ty) / scale;
    int x = tx;
    int cx2 = cx + cw;
    if (cx < 0) cx = 0;
    if (cx2 > (int)fb_w) cx2 = (int)fb_w;
    if (cx2 > MAX_W) cx2 = MAX_W;
    if (cx2 <= cx) return;

    for (; *s; s++, x += (int)(GLYPH_W * scale)) {
        uint8_t bits = glyph_row((unsigned char)*s, (int)gy);
        for (unsigned col = 0; col < GLYPH_W; col++) {
            if (!(bits & (0x80u >> col))) continue;
            int px = x + (int)(col * scale);
            for (unsigned sx = 0; sx < scale; sx++) {
                int draw_x = px + (int)sx;
                if (draw_x >= cx && draw_x < cx2)
                    row[draw_x] = color;
            }
        }
    }
}

/* AA proportional text on the current scanline, clipped to [cx,cx2). */
static int aa_text_clip_on_row(unsigned y, int tx, int ty, const char *s,
                               uint32_t color, int cx, int cx2) {
    int pen = tx;
    int gy = (int)y - ty;
    int paint = gy >= 0 && gy < FONT_UI16_LINE_H;

    if (cx < 0) cx = 0;
    if (cx2 > (int)fb_w) cx2 = (int)fb_w;
    if (cx2 > MAX_W) cx2 = MAX_W;
    for (; *s; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch < 32 || ch > 126) ch = '?';
        int gi = ch - 32;
        int gw = font_ui16_widths[gi];
        if (paint && pen < cx2 && pen + gw > cx) {
            const unsigned char *arow =
                font_ui16_alpha + font_ui16_offsets[gi] + gy * gw;
            for (int gx = 0; gx < gw; gx++) {
                unsigned a = arow[gx];
                int px = pen + gx;
                if (!a || px < cx || px >= cx2) continue;
                row[px] = blend_px(row[px], color, a);
            }
        }
        pen += gw;
        if (pen >= cx2) break;
    }
    return pen - tx;
}

static int aa_text_on_row(unsigned y, int tx, int ty, const char *s,
                          uint32_t color) {
    return aa_text_clip_on_row(y, tx, ty, s, color, 0, MAX_W);
}

static int aa_text_width(const char *s) {
    int w = 0;
    for (; *s; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch < 32 || ch > 126) ch = '?';
        w += font_ui16_widths[ch - 32];
    }
    return w;
}

/* AA monospace text on the current scanline, clipped to [cx,cx2).
 * Fixed MONO_ADV advance so console columns stay aligned. */
static void mono_text_clip_on_row(unsigned y, int tx, int ty, const char *s,
                                  uint32_t color, int cx, int cx2) {
    int pen = tx;
    int gy = (int)y - ty;
    int paint = gy >= 0 && gy < FONT_MONO16_LINE_H;

    if (cx < 0) cx = 0;
    if (cx2 > (int)fb_w) cx2 = (int)fb_w;
    if (cx2 > MAX_W) cx2 = MAX_W;
    for (; *s; s++, pen += MONO_ADV) {
        unsigned char ch = (unsigned char)*s;
        if (ch < 32 || ch > 126) ch = '?';
        int gi = ch - 32;
        int gw = font_mono16_widths[gi];
        if (paint && pen < cx2 && pen + gw > cx) {
            const unsigned char *arow =
                font_mono16_alpha + font_mono16_offsets[gi] + gy * gw;
            for (int gx = 0; gx < gw; gx++) {
                unsigned a = arow[gx];
                int px = pen + gx;
                if (!a || px < cx || px >= cx2) continue;
                row[px] = blend_px(row[px], color, a);
            }
        }
        if (pen >= cx2) break;
    }
}

static void fill_rect_on_row(unsigned y, int x, int ry, int w, int h, uint32_t color) {
    if ((int)y < ry || (int)y >= ry + h) return;
    int start = x < 0 ? 0 : x;
    int end = x + w;
    if (end > (int)fb_w) end = (int)fb_w;
    if (end > MAX_W) end = MAX_W;
    for (int px = start; px < end; px++)
        row[px] = color;
}

static void fill_rect_clip_on_row(unsigned y, int x, int ry, int w, int h,
                                  int cx, int cy, int cw, int ch,
                                  uint32_t color) {
    int x2 = x + w;
    int y2 = ry + h;
    int cx2 = cx + cw;
    int cy2 = cy + ch;

    if (x < cx) x = cx;
    if (ry < cy) ry = cy;
    if (x2 > cx2) x2 = cx2;
    if (y2 > cy2) y2 = cy2;
    if (x2 <= x || y2 <= ry) return;
    fill_rect_on_row(y, x, ry, x2 - x, y2 - ry, color);
}

/* Rounded-corner inset per row distance from the corner (radius 6). */
static const int corner_inset[6] = { 5, 3, 2, 1, 1, 0 };

static int win_row_inset(const desktop_window_t *win, unsigned y) {
    int dy = (int)y - win->y;
    if (dy < 6) return corner_inset[dy];
    if (dy >= win->h - 6) return corner_inset[win->h - 1 - dy];
    return 0;
}

/* Horizontal inset for a rounded card of height h at this scanline (radius 6). */
static int card_row_inset(unsigned y, int ry, int h) {
    int dy = (int)y - ry;
    if (dy < 0 || dy >= h) return -1;          /* off-card */
    if (dy < 6) return corner_inset[dy];
    if (dy >= h - 6) return corner_inset[h - 1 - dy];
    return 0;
}

/* Solid rounded-rect fill on the current scanline. */
static void round_fill_on_row(unsigned y, int x, int ry, int w, int h,
                              uint32_t color) {
    int ins = card_row_inset(y, ry, h);
    if (ins < 0) return;
    fill_rect_on_row(y, x + ins, (int)y, w - 2 * ins, 1, color);
}

/* Alpha-blended rounded-rect fill on the current scanline. */
static void round_blend_on_row(unsigned y, int x, int ry, int w, int h,
                               uint32_t color, unsigned alpha) {
    int ins = card_row_inset(y, ry, h);
    if (ins < 0) return;
    blend_rect_on_row(y, x + ins, (int)y, w - 2 * ins, 1, color, alpha);
}

/* Frosted-glass rounded card (keeps the sheen of glass_rect_on_row). */
static void round_glass_on_row(unsigned y, int x, int ry, int w, int h,
                               uint32_t tint, unsigned alpha) {
    int ins = card_row_inset(y, ry, h);
    if (ins < 0) return;
    glass_rect_on_row(y, x + ins, ry, w - 2 * ins, h, tint, alpha);
}

static void draw_window_on_row(unsigned y, const desktop_window_t *win) {
    int active = win->id == active_window;
    uint32_t body = win->id == WIN_TERMINAL ? rgb(17, 21, 22) :
                    is_client_window(win->id) ? rgb(236, 242, 244) :
                    rgb(248, 248, 240);
    int inset, x0, ww, dy;

    if (!win->visible || win->minimized) return;
    if ((int)y < win->y || (int)y >= win->y + win->h) {
        soft_shadow_on_row(y, win->x, win->y, win->w, win->h);
        return;
    }
    soft_shadow_on_row(y, win->x, win->y, win->w, win->h);

    inset = win_row_inset(win, y);
    x0 = win->x + inset;
    ww = win->w - 2 * inset;
    dy = (int)y - win->y;

    if (dy < TITLEBAR_H) {
        /* Aero glass title bar (accent-tinted when focused). */
        uint32_t tint = active ? col_accent : rgb(96, 104, 116);
        glass_rect_on_row(y, x0, win->y, ww, TITLEBAR_H,
                          tint, active ? 120 : 90);
        /* 1px luminous border + top highlight */
        blend_rect_on_row(y, x0, win->y, 1, TITLEBAR_H, 0xFFFFFF, 70);
        blend_rect_on_row(y, x0 + ww - 1, win->y, 1, TITLEBAR_H, 0xFFFFFF, 70);
        if (dy == 0)
            blend_rect_on_row(y, x0, win->y, ww, 1, 0xFFFFFF, 110);

        /* caption buttons */
        {
            int cx = win->x + win->w - CAP_RIGHT - CAP_CLOSE_W;
            int mx = cx - CAP_BTN_W;
            int nx = mx - CAP_BTN_W;
            if (dy >= 1 && dy < 1 + CAP_BTN_H) {
                /* close: red gradient */
                uint32_t red_top = rgb(224, 86, 70), red_bot = rgb(150, 30, 24);
                uint32_t rc = mix_color(red_top, red_bot,
                                        (unsigned)((dy - 1) * 255 / CAP_BTN_H));
                fill_rect_on_row(y, cx, win->y + 1, CAP_CLOSE_W, CAP_BTN_H, rc);
                /* min/max: glass squares */
                blend_rect_on_row(y, nx, win->y + 1, CAP_BTN_W, CAP_BTN_H,
                                  0xFFFFFF, 36);
                blend_rect_on_row(y, mx, win->y + 1, CAP_BTN_W, CAP_BTN_H,
                                  0xFFFFFF, 36);
                /* separators */
                blend_rect_on_row(y, nx, win->y + 1, 1, CAP_BTN_H, 0, 90);
                blend_rect_on_row(y, mx, win->y + 1, 1, CAP_BTN_H, 0, 90);
                blend_rect_on_row(y, cx, win->y + 1, 1, CAP_BTN_H, 0, 90);
            }
            /* glyphs */
            aa_text_on_row(y, nx + (CAP_BTN_W - aa_text_width("-")) / 2,
                           win->y + 1, "-", COL_TEXT);
            if (dy >= 7 && dy <= 13) {   /* tiny square for maximize */
                int sq = mx + CAP_BTN_W / 2 - 4;
                if (dy == 7 || dy == 13)
                    fill_rect_on_row(y, sq, win->y + 7, 8, 7, COL_TEXT);
                else {
                    fill_rect_on_row(y, sq, win->y, 1, fb_h, COL_TEXT);
                    fill_rect_on_row(y, sq + 7, win->y, 1, fb_h, COL_TEXT);
                }
            }
            aa_text_on_row(y, cx + (CAP_CLOSE_W - aa_text_width("x")) / 2,
                           win->y, "x", 0xFFFFFF);
            aa_text_clip_on_row(y, win->x + 12, win->y + 5, win->title,
                                0xFFFFFF, win->x + 12, nx - 6);
        }
        return;
    }

    /* body with 1px frame */
    fill_rect_on_row(y, x0, win->y, 1, win->h, rgb(70, 82, 100));
    fill_rect_on_row(y, x0 + ww - 1, win->y, 1, win->h, rgb(70, 82, 100));
    fill_rect_on_row(y, x0 + 1, win->y + TITLEBAR_H, ww - 2,
                     win->h - TITLEBAR_H - 1, body);
    if (dy == win->h - 1)
        fill_rect_on_row(y, x0, (int)y, ww, 1, rgb(70, 82, 100));
}

static void add_log(const char *s) {
    unsigned i;
    if (log_count < MAX_LOG) {
        for (i = 0; i + 1 < LOG_MAX && s[i]; i++) log_lines[log_count][i] = s[i];
        log_lines[log_count][i] = 0;
        log_count++;
        return;
    }
    for (int i = 1; i < MAX_LOG; i++) strcpy(log_lines[i - 1], log_lines[i]);
    for (i = 0; i + 1 < LOG_MAX && s[i]; i++) log_lines[MAX_LOG - 1][i] = s[i];
    log_lines[MAX_LOG - 1][i] = 0;
}

/* Append one finished line to the terminal scrollback (oldest scrolls off). */
static void add_term_line(const char *s) {
    unsigned i;
    if (term_count == TERM_LINES) {
        for (int j = 1; j < TERM_LINES; j++)
            strcpy(term_lines[j - 1], term_lines[j]);
        term_count--;
    }
    for (i = 0; i + 1 < LOG_MAX && s[i]; i++) term_lines[term_count][i] = s[i];
    term_lines[term_count][i] = 0;
    term_count++;
}

static void commit_output_line(void) {
    output_line[output_used] = 0;
    add_term_line(output_line);
    output_used = 0;
}

static void flush_output_line(void) {
    if (!output_used) return;
    commit_output_line();
}

/*
 * Feed raw PTY output into the terminal. Handles what the kernel PTY emits:
 * '\b' erases (echoed erase is "\b \b", which nets out correctly), ANSI CSI
 * sequences are filtered (ESC[...J additionally clears the screen so a
 * `clear`-style program works).
 */
static void add_output_chunk(const char *buf, int len) {
    for (int i = 0; i < len; i++) {
        char c = buf[i];
        if (esc_state == 1) {
            esc_state = (c == '[') ? 2 : 0;
            continue;
        }
        if (esc_state == 2) {
            if (c >= 0x40 && c <= 0x7e) {       /* CSI final byte */
                if (c == 'J') {                  /* erase display → clear */
                    term_count = 0;
                    output_used = 0;
                }
                esc_state = 0;
            }
            continue;
        }
        if (c == 27) { esc_state = 1; continue; }
        if (c == '\r') continue;
        if (c == '\n') {
            commit_output_line();
            continue;
        }
        if (c == '\b') {
            if (output_used) output_used--;
            continue;
        }
        if (c < 32 || c >= 127) continue;
        if (output_used + 1 >= LOG_MAX)
            commit_output_line();
        output_line[output_used++] = c;
    }
}

static void finish_shell(int status) {
    char msg[48];

    flush_output_line();
    if (shell_fd >= 0) {
        close(shell_fd);
        shell_fd = -1;
    }
    sprintf(msg, "SHELL EXIT %d", status);
    add_log(msg);
    shell_pid = -1;
}

static void poll_shell_exit(void) {
    int status = 0;

    if (shell_pid < 0) return;
    int r = waitpid(shell_pid, &status, WNOHANG);
    if (r == shell_pid)
        finish_shell(status);
}

static void handle_shell_output(short revents) {
    char buf[256];

    if (shell_fd < 0) return;
    if (revents & (POLLIN | POLLHUP)) {
        /* Drain everything currently buffered (PTY reads return whatever is
         * available), so a burst of output costs one render, not one per byte. */
        for (;;) {
            struct pollfd p;
            int n;
            p.fd = shell_fd;
            p.events = POLLIN;
            p.revents = 0;
            if (poll(&p, 1, 0) <= 0 || !(p.revents & POLLIN)) return;
            n = read(shell_fd, buf, (int)sizeof(buf));
            if (n > 0) {
                add_output_chunk(buf, n);
                continue;
            }
            if (n == 0) {
                close(shell_fd);
                shell_fd = -1;
            }
            break;
        }
    }
    poll_shell_exit();
}

static void stop_shell(void) {
    if (shell_pid >= 0) {
        kill(shell_pid, SIGTERM);
        add_log("STOPPING SHELL");
    }
}

static int start_shell(void) {
    char pts_path[20];
    const char *shell_path = preferred_shell_path();
    char *argv[] = { (char *)shell_path, 0 };
    int master;
    int slave;

    if (shell_pid >= 0) return 0;
    master = open("/dev/ptmx", O_RDWR);
    if (master < 0) {
        add_log("PTY MASTER FAILED");
        return -1;
    }
    int unlock = 0;
    int pty_num = -1;
    if (ioctl(master, TIOCSPTLCK, &unlock) < 0 ||
        ioctl(master, TIOCGPTN, &pty_num) < 0) {
        close(master);
        add_log("PTY IOCTL FAILED");
        return -1;
    }
    sprintf(pts_path, "/dev/pts/%d", pty_num);
    slave = open(pts_path, O_RDWR);
    if (slave < 0) {
        close(master);
        add_log("PTY SLAVE FAILED");
        return -1;
    }

    int pid = fork();
    if (pid < 0) {
        close(slave);
        close(master);
        add_log("FORK FAILED");
        return -1;
    }

    if (pid == 0) {
        close(master);
        dup2(slave, 0);
        dup2(slave, 1);
        dup2(slave, 2);
        close(slave);

        execve(shell_path, argv, desktop_envp);
        printf("exec failed: %s\n", shell_path);
        exit(127);
    }

    close(slave);
    shell_pid = pid;
    shell_fd = master;
    output_used = 0;
    add_log("SHELL STARTED");
    return 0;
}

static int app_pid_running(int pid) {
    int status = 0;
    int r;

    if (pid < 0) return 0;
    r = waitpid(pid, &status, WNOHANG);
    return r == 0;
}

/* Hide a dead client's window (no close event — the process is gone). */
static void drop_app_window(int idx) {
    desktop_window_t *win = find_window(WIN_CLIENT_BASE + idx);

    if (win && win->visible) win->visible = 0;
    drop_client_pixels(idx);   /* release its shared surface */
    if (active_window == WIN_CLIENT_BASE + idx)
        focus_window(WIN_TERMINAL);
}

static int poll_app_exits(void) {
    int status = 0;
    int exited = 0;

    for (int i = 0; i < MAX_CLIENT_WINDOWS; i++) {
        if (client_pids[i] >= 0 &&
            waitpid(client_pids[i], &status, WNOHANG) == client_pids[i]) {
            client_pids[i] = -1;
            drop_app_window(i);
            copy_text(client_status, sizeof(client_status), "APP EXITED");
            exited++;
        }
    }
    return exited;
}

static int any_client_running(void) {
    for (int i = 0; i < MAX_CLIENT_WINDOWS; i++)
        if (client_pids[i] >= 0) return 1;
    return 0;
}

/* Spawn `path` owning client slot idx (0-based); the slot number (1-based)
 * is passed as argv[1] so the app knows where to open its window. */
static int spawn_app2(const char *path, int idx, const char *logname,
                      const char *file_arg) {
    char slotstr[4];
    char *argv[4];
    int pid;

    reset_client_surface(idx);
    sprintf(slotstr, "%d", idx + 1);
    argv[0] = (char *)path;
    argv[1] = slotstr;
    argv[2] = (char *)file_arg;   /* may be 0 */
    argv[3] = 0;

    pid = fork();
    if (pid < 0) {
        add_log("APP FORK FAILED");
        return -1;
    }
    if (pid == 0) {
        execve(path, argv, desktop_envp);
        printf("exec failed: %s\n", path);
        exit(127);
    }
    client_pids[idx] = pid;
    copy_text(client_status, sizeof(client_status), "APP STARTING");
    add_log(logname);
    return 0;
}

static int spawn_app(const char *path, int idx, const char *logname) {
    return spawn_app2(path, idx, logname, 0);
}

/* Single-instance launch: focus/restore if alive, else respawn in place. */
static int start_single(const char *path, int idx, const char *logname) {
    desktop_window_t *win = find_window(WIN_CLIENT_BASE + idx);

    if (client_pids[idx] >= 0 && app_pid_running(client_pids[idx])) {
        if (win && (!win->visible || win->minimized)) {
            win->visible = 1;
            win->minimized = 0;
        }
        focus_client_app(idx);
        return 0;
    }
    client_pids[idx] = -1;
    return spawn_app(path, idx, logname);
}

static int start_uidemo(void) {
    return start_single(preferred_uidemo_path(), 1, "UI DEMO LAUNCHED");
}

static int start_files(void) {
    return start_single(preferred_files_path(), 2, "FILES LAUNCHED");
}

/* Multi-instance launch: each call takes a fresh free slot.  Slots 2 and 3
 * (idx 1/2) are reserved for the single-instance apps (UI Demo / Files) so
 * their focus-if-running tracking stays unambiguous. */
static int start_multi_file(const char *path, const char *logname,
                            const char *file_arg) {
    static const int order[] = { 0, 3, 4, 5, 6, 7, 8, 9, 10, 11 };

    for (unsigned k = 0; k < sizeof(order) / sizeof(order[0]); k++) {
        int i = order[k];
        desktop_window_t *win = find_window(WIN_CLIENT_BASE + i);
        if (client_pids[i] >= 0 && app_pid_running(client_pids[i])) continue;
        client_pids[i] = -1;
        if (win && (win->visible || win->minimized)) continue;
        return spawn_app2(path, i, logname, file_arg);
    }
    add_log("NO FREE WINDOW SLOT");
    return -1;
}

static int start_multi(const char *path, const char *logname) {
    return start_multi_file(path, logname, 0);
}

static int start_term(void) {
    return start_multi(preferred_term_path(), "TERMINAL LAUNCHED");
}

static int start_browse(void) {
    return start_multi(preferred_browse_path(), "BROWSER LAUNCHED");
}

static const char *disk_or_initrd(const char *disk, const char *initrd) {
    return access(disk, X_OK) == 0 ? disk : initrd;
}

static int start_edit(void) {
    return start_multi(disk_or_initrd("/disk/edit", "/edit"), "EDITOR LAUNCHED");
}

static int start_calc(void) {
    return start_multi(disk_or_initrd("/disk/calc", "/calc"), "CALC LAUNCHED");
}

static int start_taskmgr(void) {
    return start_multi(disk_or_initrd("/disk/taskmgr", "/taskmgr"),
                       "TASKMGR LAUNCHED");
}

static int start_view(void) __attribute__((unused));
static int start_view(void) {
    return start_multi(disk_or_initrd("/disk/view", "/view"), "VIEWER LAUNCHED");
}

static int start_settings(void) {
    return start_multi(disk_or_initrd("/disk/settings", "/settings"),
                       "SETTINGS LAUNCHED");
}

/* wmctl "launch <app> [path]": apps open files in other apps (Files!). */
static void launch_by_name(const char *arg) {
    char app[24];
    int ai = 0;
    const char *file;

    while (*arg == ' ') arg++;
    while (*arg && *arg != ' ' && ai < (int)sizeof(app) - 1)
        app[ai++] = *arg++;
    app[ai] = 0;
    while (*arg == ' ') arg++;
    file = *arg ? arg : 0;

    if (!strcmp(app, "edit"))
        start_multi_file(disk_or_initrd("/disk/edit", "/edit"),
                         "EDITOR LAUNCHED", file);
    else if (!strcmp(app, "view"))
        start_multi_file(disk_or_initrd("/disk/view", "/view"),
                         "VIEWER LAUNCHED", file);
    else if (!strcmp(app, "term"))
        start_term();
    else if (!strcmp(app, "browse"))
        start_browse();
    else
        add_log("LAUNCH: UNKNOWN APP");
}

static void stop_apps(void) {
    int status = 0;

    for (int i = 0; i < MAX_CLIENT_WINDOWS; i++) {
        if (client_pids[i] <= 1) continue;   /* 0/-1: empty; never kill(0)! */
        kill(client_pids[i], SIGTERM);
        waitpid(client_pids[i], &status, 0);
        client_pids[i] = -1;
    }
}

static void clear_desktop_log(void) {
    log_count = 0;
    term_count = 0;
    output_used = 0;
}

static void copy_text(char *dst, unsigned size, const char *src) {
    unsigned i;

    if (!size) return;
    for (i = 0; i + 1 < size && src[i]; i++)
        dst[i] = src[i];
    dst[i] = 0;
}

static const char *command_arg(const char *line, const char *cmd) {
    unsigned len = strlen(cmd);

    if (strncmp(line, cmd, len) != 0) return 0;
    if (line[len] == 0) return line + len;
    if (line[len] != ' ') return 0;
    return line + len + 1;
}

static int parse_int_arg(const char **p, int *out) {
    const char *s = *p;
    int sign = 1;
    int value = 0;
    int any = 0;

    while (*s == ' ') s++;
    if (*s == '-') {
        sign = -1;
        s++;
    }
    while (*s >= '0' && *s <= '9') {
        any = 1;
        value = value * 10 + (*s - '0');
        s++;
    }
    if (!any || (*s && *s != ' ')) return -1;
    while (*s == ' ') s++;
    *out = value * sign;
    *p = s;
    return 0;
}

static int parse_word_arg(const char **p, char *out, unsigned size) {
    const char *s = *p;
    unsigned i = 0;

    while (*s == ' ') s++;
    if (!*s || !size) return -1;
    while (*s && *s != ' ' && i + 1 < size)
        out[i++] = *s++;
    if (*s && *s != ' ') return -1;
    out[i] = 0;
    /* Leave trailing spaces in place: when the remainder of the line is text
     * content, its leading spaces are significant (e.g. icon padding).  The
     * int/word parsers all skip leading spaces themselves. */
    *p = s;
    return 0;
}

static uint32_t parse_color(const char *name, int *ok) {
    *ok = 1;
    if (!strcmp(name, "black")) return rgb(20, 24, 26);
    if (!strcmp(name, "white")) return rgb(238, 240, 235);
    if (!strcmp(name, "gray")) return rgb(102, 110, 112);
    if (!strcmp(name, "red")) return rgb(205, 83, 73);
    if (!strcmp(name, "green")) return rgb(62, 156, 108);
    if (!strcmp(name, "blue")) return rgb(36, 127, 174);
    if (!strcmp(name, "yellow")) return rgb(255, 214, 88);
    if (!strcmp(name, "cyan")) return rgb(85, 190, 205);
    /* True color: #RRGGBB (6 hex digits). */
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
    }
    *ok = 0;
    return 0;
}

/* Detach a client's pixel surface (unmap shm). */
static void drop_client_pixels(int idx) {
    client_surface_t *cs;

    if (idx < 0 || idx >= MAX_CLIENT_WINDOWS) return;
    cs = &client_surfaces[idx];
    /* surf gates the unmap: surf_shm is 0 (a valid id) in fresh static
     * storage, but surf is only non-NULL once a mapping really exists. */
    if (cs->surf && cs->surf_shm >= 0)
        syscall1(SYS_SHM_UNMAP, cs->surf_shm);
    cs->surf = 0;
    cs->surf_w = 0;
    cs->surf_h = 0;
    cs->surf_shm = -1;
}

static void reset_client_surface(int idx) {
    if (idx < 0 || idx >= MAX_CLIENT_WINDOWS) return;
    drop_client_pixels(idx);
    client_surfaces[idx].bg = rgb(236, 242, 244);
    for (int i = 0; i < CLIENT_RECTS; i++)
        client_surfaces[idx].rects[i].used = 0;
    for (int i = 0; i < CLIENT_ICON_USES; i++)
        client_surfaces[idx].icons[i].used = 0;
    for (int i = 0; i < CLIENT_LINES; i++) {
        client_surfaces[idx].lines[i][0] = 0;
        client_surfaces[idx].line_color[i] = rgb(30, 34, 36);
        client_surfaces[idx].line_x[i] = 10;
        client_surfaces[idx].line_y[i] = 34 + i * 18;
    }
}

static int client_surface_has_content(int idx) {
    if (idx < 0 || idx >= MAX_CLIENT_WINDOWS) return 0;
    for (int i = 0; i < CLIENT_RECTS; i++) {
        if (client_surfaces[idx].rects[i].used)
            return 1;
    }
    for (int i = 0; i < CLIENT_LINES; i++) {
        if (client_surfaces[idx].lines[i][0])
            return 1;
    }
    return 0;
}

static void show_client_app(int idx, const char *text) {
    desktop_window_t *client;
    char status[LOG_MAX];

    if (idx < 0 || idx >= MAX_CLIENT_WINDOWS) return;
    client = find_window(WIN_CLIENT_BASE + idx);
    copy_text(client_text[idx], sizeof(client_text[idx]),
              text && *text ? text : "CLIENT APP ONLINE");
    sprintf(status, "APP %d WINDOW UPDATED", idx + 1);
    copy_text(client_status, sizeof(client_status), status);
    if (client) {
        client->visible = 1;
        client->minimized = 0;
        focus_window(client->id);
    }
}

static void title_client_app(int idx, const char *title) {
    desktop_window_t *client;
    char status[LOG_MAX];

    if (idx < 0 || idx >= MAX_CLIENT_WINDOWS) return;
    client = find_window(WIN_CLIENT_BASE + idx);
    if (!client) return;
    copy_text(client->title, sizeof(client->title), title && *title ? title : "APP");
    sprintf(status, "APP %d TITLE UPDATED", idx + 1);
    copy_text(client_status, sizeof(client_status), status);
}

static void hide_client_app(int idx) {
    desktop_window_t *client;
    char status[LOG_MAX];

    if (idx < 0 || idx >= MAX_CLIENT_WINDOWS) return;
    client = find_window(WIN_CLIENT_BASE + idx);
    if (client) client->visible = 0;
    if (active_window == WIN_CLIENT_BASE + idx)
        focus_window(WIN_TERMINAL);
    emit_client_close_event(idx);
    sprintf(status, "APP %d WINDOW HIDDEN", idx + 1);
    copy_text(client_status, sizeof(client_status), status);
}

static void focus_client_app(int idx) {
    desktop_window_t *client;

    if (idx < 0 || idx >= MAX_CLIENT_WINDOWS) return;
    client = find_window(WIN_CLIENT_BASE + idx);
    if (client && client->visible) focus_window(client->id);
}

static void set_client_geometry(int idx, const char *arg) {
    int x, y, w, h;
    desktop_window_t *client;
    char status[LOG_MAX];

    if (idx < 0 || idx >= MAX_CLIENT_WINDOWS) return;
    if (parse_int_arg(&arg, &x) < 0 || parse_int_arg(&arg, &y) < 0 ||
        parse_int_arg(&arg, &w) < 0 || parse_int_arg(&arg, &h) < 0) {
        add_log("WMCTL BAD GEOM");
        return;
    }
    client = find_window(WIN_CLIENT_BASE + idx);
    if (!client) return;
    client->x = x;
    client->y = y;
    client->w = w;
    client->h = h;
    clamp_window(client);
    emit_client_geom_event(client);
    sprintf(status, "APP %d GEOMETRY UPDATED", idx + 1);
    copy_text(client_status, sizeof(client_status), status);
}

static void set_client_bg(int idx, const char *arg) {
    char color_name[16];
    int ok;

    if (idx < 0 || idx >= MAX_CLIENT_WINDOWS) return;
    if (parse_word_arg(&arg, color_name, sizeof(color_name)) < 0) {
        add_log("WMCTL BAD COLOR");
        return;
    }
    client_surfaces[idx].bg = parse_color(color_name, &ok);
    if (!ok) {
        add_log("WMCTL BAD COLOR");
        return;
    }
    copy_text(client_status, sizeof(client_status), "APP BACKGROUND UPDATED");
}

static void add_client_rect(int idx, const char *arg) {
    int x, y, w, h;
    char color_name[16];
    int ok;
    client_rect_t *rect = 0;

    if (idx < 0 || idx >= MAX_CLIENT_WINDOWS) return;
    if (parse_int_arg(&arg, &x) < 0 || parse_int_arg(&arg, &y) < 0 ||
        parse_int_arg(&arg, &w) < 0 || parse_int_arg(&arg, &h) < 0 ||
        parse_word_arg(&arg, color_name, sizeof(color_name)) < 0) {
        add_log("WMCTL BAD RECT");
        return;
    }
    for (int i = 0; i < CLIENT_RECTS; i++) {
        if (!client_surfaces[idx].rects[i].used) {
            rect = &client_surfaces[idx].rects[i];
            break;
        }
    }
    if (!rect) rect = &client_surfaces[idx].rects[CLIENT_RECTS - 1];
    rect->color = parse_color(color_name, &ok);
    if (!ok) {
        add_log("WMCTL BAD COLOR");
        return;
    }
    rect->used = 1;
    rect->x = x;
    rect->y = y;
    rect->w = w;
    rect->h = h;
    copy_text(client_status, sizeof(client_status), "APP RECT ADDED");
}

static void set_client_line(int idx, const char *arg) {
    int line;
    char color_name[16];
    int ok;

    if (idx < 0 || idx >= MAX_CLIENT_WINDOWS) return;
    if (parse_int_arg(&arg, &line) < 0 ||
        parse_word_arg(&arg, color_name, sizeof(color_name)) < 0) {
        add_log("WMCTL BAD TEXT");
        return;
    }
    if (line < 0 || line >= CLIENT_LINES) {
        add_log("WMCTL BAD TEXT LINE");
        return;
    }
    client_surfaces[idx].line_color[line] = parse_color(color_name, &ok);
    if (!ok) {
        add_log("WMCTL BAD COLOR");
        return;
    }
    if (*arg == ' ') arg++;   /* exactly one separator; keep padding spaces */
    copy_text(client_surfaces[idx].lines[line], sizeof(client_surfaces[idx].lines[line]), arg);
    copy_text(client_status, sizeof(client_status), "APP TEXT UPDATED");
}

/* surface SHMID W H — adopt a client-rendered pixel buffer as the window
 * content (the modern path: the client draws, we composite). */
static void set_client_pixels(int idx, const char *arg) {
    int shmid, w, h;
    int addr;

    if (idx < 0 || idx >= MAX_CLIENT_WINDOWS) return;
    if (parse_int_arg(&arg, &shmid) < 0 || parse_int_arg(&arg, &w) < 0 ||
        parse_int_arg(&arg, &h) < 0 ||
        shmid < 0 || w < 1 || h < 1 || w > 2048 || h > 2048) {
        add_log("WMCTL BAD SURFACE");
        return;
    }
    drop_client_pixels(idx);
    addr = syscall1(SYS_SHM_MAP, shmid);
    if (addr <= 0) {
        add_log("WMCTL SURFACE MAP FAILED");
        return;
    }
    client_surfaces[idx].surf = (uint32_t *)(uintptr_t)(unsigned)addr;
    client_surfaces[idx].surf_w = w;
    client_surfaces[idx].surf_h = h;
    client_surfaces[idx].surf_shm = shmid;
}

/* icondef INDEX W H — declare an icon's size (clears its pixels). */
static void define_client_icon(int idx, const char *arg) {
    int icon, w, h;

    if (idx < 0 || idx >= MAX_CLIENT_WINDOWS) return;
    if (parse_int_arg(&arg, &icon) < 0 || parse_int_arg(&arg, &w) < 0 ||
        parse_int_arg(&arg, &h) < 0 ||
        icon < 0 || icon >= CLIENT_ICONS ||
        w < 1 || w > ICON_MAX || h < 1 || h > ICON_MAX) {
        add_log("WMCTL BAD ICONDEF");
        return;
    }
    client_icons[idx][icon].w = (uint8_t)w;
    client_icons[idx][icon].h = (uint8_t)h;
    memset(client_icons[idx][icon].pix, 0, sizeof(client_icons[idx][icon].pix));
}

/* irow INDEX ROW HEXPIXELS — one row of palette indexes (one hex digit each). */
static void fill_client_icon_row(int idx, const char *arg) {
    int icon, rowi;
    char hex[ICON_MAX + 4];
    client_icon_t *ic;

    if (idx < 0 || idx >= MAX_CLIENT_WINDOWS) return;
    if (parse_int_arg(&arg, &icon) < 0 || parse_int_arg(&arg, &rowi) < 0 ||
        parse_word_arg(&arg, hex, sizeof(hex)) < 0 ||
        icon < 0 || icon >= CLIENT_ICONS) {
        add_log("WMCTL BAD IROW");
        return;
    }
    ic = &client_icons[idx][icon];
    if (!ic->w || rowi < 0 || rowi >= ic->h) return;
    for (int col = 0; col < ic->w && hex[col]; col++) {
        char c = hex[col];
        uint8_t v = 0;
        if (c >= '0' && c <= '9') v = (uint8_t)(c - '0');
        else if (c >= 'a' && c <= 'f') v = (uint8_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v = (uint8_t)(c - 'A' + 10);
        ic->pix[rowi * ic->w + col] = v;
    }
}

/* icon INDEX X Y — place an instance of a defined icon on the surface. */
static void place_client_icon(int idx, const char *arg) {
    int icon, x, y;

    if (idx < 0 || idx >= MAX_CLIENT_WINDOWS) return;
    if (parse_int_arg(&arg, &icon) < 0 || parse_int_arg(&arg, &x) < 0 ||
        parse_int_arg(&arg, &y) < 0 || icon < 0 || icon >= CLIENT_ICONS) {
        add_log("WMCTL BAD ICON");
        return;
    }
    for (int i = 0; i < CLIENT_ICON_USES; i++) {
        icon_use_t *use = &client_surfaces[idx].icons[i];
        if (use->used) continue;
        use->used = 1;
        use->icon = icon;
        use->x = x;
        use->y = y;
        return;
    }
}

static void set_client_line_at(int idx, const char *arg) {
    int line;
    int x;
    int y;
    char color_name[16];
    int ok;

    if (idx < 0 || idx >= MAX_CLIENT_WINDOWS) return;
    if (parse_int_arg(&arg, &line) < 0 ||
        parse_int_arg(&arg, &x) < 0 ||
        parse_int_arg(&arg, &y) < 0 ||
        parse_word_arg(&arg, color_name, sizeof(color_name)) < 0) {
        add_log("WMCTL BAD TEXTAT");
        return;
    }
    if (line < 0 || line >= CLIENT_LINES) {
        add_log("WMCTL BAD TEXT LINE");
        return;
    }
    client_surfaces[idx].line_color[line] = parse_color(color_name, &ok);
    if (!ok) {
        add_log("WMCTL BAD COLOR");
        return;
    }
    client_surfaces[idx].line_x[line] = x;
    client_surfaces[idx].line_y[line] = y;
    if (*arg == ' ') arg++;   /* exactly one separator; keep padding spaces */
    copy_text(client_surfaces[idx].lines[line], sizeof(client_surfaces[idx].lines[line]), arg);
    copy_text(client_status, sizeof(client_status), "APP TEXT UPDATED");
}

static void handle_wmctl_line(char *line) {
    const char *arg;

    while (*line == ' ') line++;
    if (!*line) return;

    arg = command_arg(line, "log");
    if (arg) {
        add_log(arg);
        return;
    }
    arg = command_arg(line, "launch");
    if (arg) {
        launch_by_name(arg);
        return;
    }
    if (!strncmp(line, "apps-changed", 12)) {   /* Store installed/removed */
        scan_installed_apps();
        add_log("APPS RESCANNED");
        return;
    }
    if (!strncmp(line, "reload", 6)) {     /* settings changed */
        if (wallpaper) { free(wallpaper); wallpaper = 0; }
        if (wallpaper_blur) { free(wallpaper_blur); wallpaper_blur = 0; }
        load_desktop_conf();
        load_wallpaper();
        make_wallpaper_blur();
        add_log("CONF RELOADED");
        return;
    }
    arg = command_arg(line, "status");
    if (arg) {
        copy_text(client_status, sizeof(client_status), arg);
        return;
    }
    arg = command_arg(line, "app");
    if (arg) {
        int idx = parse_client_index(&arg);
        if (idx < 0) idx = 0;
        show_client_app(idx, arg);
        return;
    }
    arg = command_arg(line, "title");
    if (arg) {
        int idx = parse_client_index(&arg);
        if (idx >= 0) title_client_app(idx, arg);
        else add_log("WMCTL BAD APP ID");
        return;
    }
    arg = command_arg(line, "close");
    if (arg) {
        int idx = parse_client_index(&arg);
        if (idx >= 0) hide_client_app(idx);
        else add_log("WMCTL BAD APP ID");
        return;
    }
    arg = command_arg(line, "geom");
    if (arg) {
        int idx = parse_client_index(&arg);
        if (idx >= 0) set_client_geometry(idx, arg);
        else add_log("WMCTL BAD APP ID");
        return;
    }
    arg = command_arg(line, "bg");
    if (arg) {
        int idx = parse_client_index(&arg);
        if (idx >= 0) set_client_bg(idx, arg);
        else add_log("WMCTL BAD APP ID");
        return;
    }
    arg = command_arg(line, "rect");
    if (arg) {
        int idx = parse_client_index(&arg);
        if (idx >= 0) add_client_rect(idx, arg);
        else add_log("WMCTL BAD APP ID");
        return;
    }
    arg = command_arg(line, "surface");
    if (arg) {
        int idx = parse_client_index(&arg);
        if (idx >= 0) set_client_pixels(idx, arg);
        else add_log("WMCTL BAD APP ID");
        return;
    }
    arg = command_arg(line, "commit");
    if (arg) {
        /* Content already lives in the shared buffer; the dirty flag set by
         * command arrival triggers the recomposite. */
        return;
    }
    arg = command_arg(line, "icondef");
    if (arg) {
        int idx = parse_client_index(&arg);
        if (idx >= 0) define_client_icon(idx, arg);
        else add_log("WMCTL BAD APP ID");
        return;
    }
    arg = command_arg(line, "irow");
    if (arg) {
        int idx = parse_client_index(&arg);
        if (idx >= 0) fill_client_icon_row(idx, arg);
        else add_log("WMCTL BAD APP ID");
        return;
    }
    arg = command_arg(line, "icon");
    if (arg) {
        int idx = parse_client_index(&arg);
        if (idx >= 0) place_client_icon(idx, arg);
        else add_log("WMCTL BAD APP ID");
        return;
    }
    arg = command_arg(line, "textat");
    if (arg) {
        int idx = parse_client_index(&arg);
        if (idx >= 0) set_client_line_at(idx, arg);
        else add_log("WMCTL BAD APP ID");
        return;
    }
    arg = command_arg(line, "text");
    if (arg) {
        int idx = parse_client_index(&arg);
        if (idx >= 0) set_client_line(idx, arg);
        else add_log("WMCTL BAD APP ID");
        return;
    }
    if (!strcmp(line, "hide-app")) {
        hide_client_app(0);
        return;
    }
    arg = command_arg(line, "clear");
    if (arg && *arg) {
        int idx = parse_client_index(&arg);
        if (idx >= 0) reset_client_surface(idx);
        else add_log("WMCTL BAD APP ID");
        return;
    }
    if (!strcmp(line, "clear")) {
        clear_desktop_log();
        add_log("OUTPUT CLEARED");
        return;
    }
    if (!strcmp(line, "quit")) {
        add_log("EXITING DESKTOP");
        running = 0;
        return;
    }
    if (!strcmp(line, "focus terminal")) {
        focus_window(WIN_TERMINAL);
        return;
    }
    if (!strcmp(line, "focus status")) {
        focus_window(WIN_STATUS);
        return;
    }
    arg = command_arg(line, "focus app");
    if (arg) {
        int idx = parse_client_index(&arg);
        if (idx < 0) idx = 0;
        focus_client_app(idx);
        return;
    }
    if (!strcmp(line, "shell start")) {
        start_shell();
        return;
    }
    if (!strcmp(line, "shell stop")) {
        stop_shell();
        return;
    }
    if (!strcmp(line, "shell restart")) {
        if (shell_pid >= 0) stop_shell();
        else start_shell();
        return;
    }

    add_log("WMCTL UNKNOWN COMMAND");
}

static void handle_wmctl_input(void) {
    char c;

    if (wm_fd < 0) return;
    /*
     * Drain every byte currently buffered in the FIFO and apply all complete
     * command lines, so a client's whole draw burst is processed before the
     * single render this loop iteration performs — rather than one byte (and a
     * full-screen render) per loop pass. pipe reads block for exactly the
     * requested length and ignore O_NONBLOCK, so we poll(timeout 0) to detect
     * when the FIFO is empty and stop.
     */
    for (;;) {
        struct pollfd p;
        p.fd = wm_fd;
        p.events = POLLIN;
        p.revents = 0;
        if (poll(&p, 1, 0) <= 0 || !(p.revents & POLLIN)) break;
        if (read(wm_fd, &c, 1) <= 0) break;
        if (c == '\r') continue;
        if (c == '\n') {
            wm_line[wm_line_used] = 0;
            handle_wmctl_line(wm_line);
            wm_line_used = 0;
            continue;
        }
        if (wm_line_used + 1 < (int)sizeof(wm_line))
            wm_line[wm_line_used++] = c;
    }
}

static void setup_wmctl(void) {
    mkdir("/tmp", 0755);
    mkfifo("/tmp/wmctl", 0600);
    wm_fd = open("/tmp/wmctl", O_RDONLY);
    if (wm_fd >= 0)
        wm_keepalive_fd = open("/tmp/wmctl", O_WRONLY);
    if (wm_fd >= 0 && wm_keepalive_fd >= 0)
        add_log("WMCTL READY");
    else
        add_log("WMCTL UNAVAILABLE");
}

static void setup_wmevents(void) {
    int ok = 1;

    for (int i = 0; i < MAX_CLIENT_WINDOWS; i++) {
        char path[40];
        sprintf(path, WM_EVENTS_PATH "%d", i + 1);
        mkfifo(path, 0600);
        wm_event_keepalives[i] = open(path, O_RDONLY | O_NONBLOCK);
        wm_event_fds[i] = open(path, O_WRONLY | O_NONBLOCK);
        if (wm_event_fds[i] < 0) ok = 0;
    }
    add_log(ok ? "WM EVENTS READY" : "WM EVENTS UNAVAILABLE");
}

static void close_wm_channels(void) {
    for (int i = 0; i < MAX_CLIENT_WINDOWS; i++) {
        if (wm_event_fds[i] >= 0) close(wm_event_fds[i]);
        if (wm_event_keepalives[i] >= 0) close(wm_event_keepalives[i]);
        wm_event_fds[i] = -1;
        wm_event_keepalives[i] = -1;
    }
    if (wm_keepalive_fd >= 0) close(wm_keepalive_fd);
    if (wm_fd >= 0) close(wm_fd);
    wm_keepalive_fd = -1;
    wm_fd = -1;
}

/* In every event the first format arg is the 1-based client slot; the line
 * is written to that slot's private FIFO. */
static void emit_wm_event(const char *fmt, int a, int b, int c, int d) {
    char line[80];
    int len;

    if (a < 1 || a > MAX_CLIENT_WINDOWS || wm_event_fds[a - 1] < 0) return;
    len = snprintf(line, sizeof(line), fmt, a, b, c, d);
    if (len < 0 || len + 1 >= (int)sizeof(line)) return;
    line[len++] = '\n';
    write(wm_event_fds[a - 1], line, len);
}

static void emit_wm_event5(const char *fmt, int a, int b, int c, int d, int e) {
    char line[80];
    int len;

    if (a < 1 || a > MAX_CLIENT_WINDOWS || wm_event_fds[a - 1] < 0) return;
    len = snprintf(line, sizeof(line), fmt, a, b, c, d, e);
    if (len < 0 || len + 1 >= (int)sizeof(line)) return;
    line[len++] = '\n';
    write(wm_event_fds[a - 1], line, len);
}

static void emit_client_focus_event(int id) {
    int idx = client_index_for_window(id);

    if (idx < 0) return;
    emit_wm_event("focus %d %d %d %d", idx + 1, 0, 0, 0);
}

static void emit_client_geom_event(const desktop_window_t *win) {
    int idx;

    if (!win) return;
    idx = client_index_for_window(win->id);
    if (idx < 0) return;
    emit_wm_event5("geom %d %d %d %d %d", idx + 1, win->x, win->y, win->w, win->h);
}

static void emit_client_close_event(int idx) {
    if (idx < 0 || idx >= MAX_CLIENT_WINDOWS) return;
    emit_wm_event("close %d %d %d %d", idx + 1, 0, 0, 0);
}

static void emit_client_mouse_event(int x, int y, int button) {
    desktop_window_t *win = window_at(x, y);
    int slot;

    if (!win || !is_client_window(win->id)) return;
    slot = client_index_for_window(win->id) + 1;
    emit_wm_event("mouse %d %d %d %d", slot, x - win->x, y - win->y, button);
}

static void emit_client_key_event(uint16_t code, int value, char ch) {
    int slot;

    if (!is_client_window(active_window)) return;
    slot = client_index_for_window(active_window) + 1;
    emit_wm_event("key %d %d %d %d", slot, (int)code, value, (int)ch);
}

/* The uncooked key stream.  "key" above is cooked for text widgets: presses
 * only, the modifier keys swallowed, Ctrl already folded into the character.
 * An app that is itself a keyboard consumer (maeroX, which has to build X11
 * KeyPress/KeyRelease events) needs the opposite — the Linux keycode, whether
 * it went down or up, and which modifiers were held.  Both streams go out, so
 * nothing that reads "key" changes behaviour.
 *
 * `mods` is the state BEFORE this event, which is what X11 defines `state` to
 * be: pressing Shift reports a mask without ShiftMask, releasing it reports one
 * with it. */
static void emit_client_rawkey_event(uint16_t code, int value, int mods) {
    int slot;

    if (!is_client_window(active_window)) return;
    slot = client_index_for_window(active_window) + 1;
    emit_wm_event("rkey %d %d %d %d", slot, (int)code, value, mods);
}

/* Re-show a system window that was closed (hidden) or minimized. */
static void reopen_window(int id) {
    desktop_window_t *win = find_window(id);
    if (!win) return;
    win->visible = 1;
    win->minimized = 0;
    focus_window(id);
}

static void launcher_menu_action(int item) {
    switch (item) {
    case 0: start_term(); break;       /* GUI terminal app, multi-instance */
    case 1: start_browse(); break;
    case 2: start_files();  break;
    case 3: start_edit(); break;
    case 4: start_calc(); break;
    case 5: start_taskmgr(); break;
    case 6: start_settings(); break;
    case 7: start_uidemo(); break;
    case 8:   /* built-in console (fallback shell) */
        if (shell_pid < 0) start_shell();
        reopen_window(WIN_TERMINAL);
        break;
    case 9: reopen_window(WIN_STATUS); break;
    case 10:  /* Power off */
        add_log("EXITING DESKTOP");
        running = 0;
        break;
    case 11:
        start_multi(disk_or_initrd("/disk/store", "/store"),
                    "STORE LAUNCHED");
        break;
    }
}

/* ── Minimize/restore animation (translucent rect tween, ~7 frames) ─────── */
static struct {
    int active, frame;
    int x0, y0, w0, h0;
    int x1, y1, w1, h1;
} win_anim;

#define ANIM_FRAMES 7

static void taskbar_button_rect_for(int id, int *bx, int *by, int *bw, int *bh);

static void start_win_anim(int x0, int y0, int w0, int h0,
                           int x1, int y1, int w1, int h1) {
    win_anim.active = 1;
    win_anim.frame = 0;
    win_anim.x0 = x0; win_anim.y0 = y0; win_anim.w0 = w0; win_anim.h0 = h0;
    win_anim.x1 = x1; win_anim.y1 = y1; win_anim.w1 = w1; win_anim.h1 = h1;
}

static void draw_win_anim_on_row(unsigned y) {
    int t, x, ry, w, h;

    if (!win_anim.active) return;
    t = win_anim.frame * 255 / ANIM_FRAMES;
    x  = win_anim.x0 + (win_anim.x1 - win_anim.x0) * t / 255;
    ry = win_anim.y0 + (win_anim.y1 - win_anim.y0) * t / 255;
    w  = win_anim.w0 + (win_anim.w1 - win_anim.w0) * t / 255;
    h  = win_anim.h0 + (win_anim.h1 - win_anim.h0) * t / 255;
    blend_rect_on_row(y, x, ry, w, h, col_accent, (unsigned)(150 - t / 2));
}

static void tick_win_anim(void) {
    if (!win_anim.active) return;
    if (++win_anim.frame > ANIM_FRAMES) win_anim.active = 0;
}

static void minimize_window(desktop_window_t *win) {
    int bx, by, bw, bh;

    if (!win) return;
    win->minimized = 1;
    taskbar_button_rect_for(win->id, &bx, &by, &bw, &bh);
    start_win_anim(win->x, win->y, win->w, win->h, bx, by, bw, bh);
    if (active_window == win->id) {
        desktop_window_t *term = find_window(WIN_TERMINAL);
        if (term && term->visible && !term->minimized)
            focus_window(WIN_TERMINAL);
    }
}

static void restore_window(desktop_window_t *win) {
    int bx, by, bw, bh;

    if (!win) return;
    win->minimized = 0;
    taskbar_button_rect_for(win->id, &bx, &by, &bw, &bh);
    start_win_anim(bx, by, bw, bh, win->x, win->y, win->w, win->h);
    focus_window(win->id);
}

/* ── Desktop icons (double-click launches) ──────────────────────────────── */

static const char *const desk_art_term[16] = {
    "0000000000000000",
    "0aaaaaaaaaaaaaa0",
    "0a999999999999a0",
    "0aaaaaaaaaaaaaa0",
    "0aaaaaaaaaaaaaa0",
    "0a5aaaaaaaaaaaa0",
    "0aa5aaaaaaaaaaa0",
    "0aaa5aaaaaaaaaa0",
    "0aa5aaaaaaaaaaa0",
    "0a5aaaaaaaaaaaa0",
    "0aaaaa22222aaaa0",
    "0aaaaaaaaaaaaaa0",
    "0aaaaaaaaaaaaaa0",
    "0aaaaaaaaaaaaaa0",
    "0aaaaaaaaaaaaaa0",
    "0000000000000000",
};
static const char *const desk_art_web[16] = {
    "0000000000000000",
    "0000066666600000",
    "0006666666666000",
    "0066662666266600",
    "0666626666626660",
    "0666626666626660",
    "0622222222222260",
    "0666626666626660",
    "0666626666626660",
    "0622222222222260",
    "0666626666626660",
    "0666626666626660",
    "0066662666266600",
    "0006666666666000",
    "0000066666600000",
    "0000000000000000",
};
static const char *const desk_art_files[16] = {
    "0000000000000000",
    "0777777000000000",
    "7eeeeee770000000",
    "7777777777777770",
    "7eeeeeeeeeeeee70",
    "7eeeeeeeeeeeee70",
    "7eeeeeeeeeeeee70",
    "7eeeeeeeeeeeee70",
    "7eeeeeeeeeeeee70",
    "7eeeeeeeeeeeee70",
    "7eeeeeeeeeeeee70",
    "7eeeeeeeeeeeee70",
    "7eeeeeeeeeeeee70",
    "7777777777777770",
    "0000000000000000",
    "0000000000000000",
};
static const char *const desk_art_demo[16] = {
    "0000000000000000",
    "0ffffffffffffff0",
    "0ffffffffffffff0",
    "0fff9ffffff9fff0",
    "0f99999999999ff0",
    "0fff9ffffff9fff0",
    "0ffffffffffffff0",
    "0ff9ffffff9ffff0",
    "0f99999999999ff0",
    "0ff9ffffff9ffff0",
    "0ffffffffffffff0",
    "0fffff9fffff9ff0",
    "0f99999999999ff0",
    "0fffff9fffff9ff0",
    "0ffffffffffffff0",
    "0000000000000000",
};

typedef struct {
    const char *label;
    const char *const *art;
    int (*launch)(void);
    const char *icon;     /* themed .mic name, NULL = hex-art only */
} desk_icon_t;

static const desk_icon_t desk_icons[] = {
    { "Terminal", desk_art_term,  start_term,    "terminal" },
    { "Browser",  desk_art_web,   start_browse,  "browser" },
    { "Files",    desk_art_files, start_files,   "files" },
    { "Widgets",  desk_art_demo,  start_uidemo,  "sysmon" },
};
#define DESK_ICONS   ((int)(sizeof(desk_icons) / sizeof(desk_icons[0])))
#define DESK_CELL_W  76
#define DESK_CELL_H  84
#define DESK_X       20
#define DESK_Y       24

static int desk_sel = -1;
static long desk_sel_ms;

static long now_ms(void) {
    struct timeval tv;
    if (gettimeofday(&tv, 0) != 0) return 0;
    return (long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static int desk_icon_at(int x, int y) {
    for (int i = 0; i < DESK_ICONS; i++) {
        int ix = DESK_X, iy = DESK_Y + i * DESK_CELL_H;
        if (x >= ix && x < ix + DESK_CELL_W && y >= iy && y < iy + DESK_CELL_H)
            return i;
    }
    return -1;
}

static void draw_desk_icons_on_row(unsigned y) {
    for (int i = 0; i < DESK_ICONS; i++) {
        int ix = DESK_X, iy = DESK_Y + i * DESK_CELL_H;
        int ax = ix + (DESK_CELL_W - 32) / 2, ay = iy + 8;
        int gy;

        if ((int)y < iy || (int)y >= iy + DESK_CELL_H) continue;
        if (i == desk_sel)
            blend_rect_on_row(y, ix, iy, DESK_CELL_W, DESK_CELL_H - 6,
                              col_accent, 110);
        /* Themed 48x48 icon (centered); else legacy 16x16 art scaled x2. */
        themed_icon_t *tic = desk_icons[i].icon ?
                             load_themed_icon(desk_icons[i].icon) : NULL;
        if (tic && tic->px) {
            int tw = 48, tax = ix + (DESK_CELL_W - tw) / 2;
            themed_icon_on_row(y, tax, iy + 6, tw, tw, tic);
        } else {
            gy = ((int)y - ay) / 2;
            if ((int)y >= ay && gy >= 0 && gy < 16) {
                const char *r = desk_icons[i].art[gy];
                for (int gx = 0; gx < 16 && r[gx]; gx++) {
                    char c = r[gx];
                    int v = (c >= '0' && c <= '9') ? c - '0' :
                            (c >= 'a' && c <= 'f') ? c - 'a' + 10 : 0;
                    if (!v) continue;
                    fill_rect_on_row(y, ax + gx * 2, iy, 2, DESK_CELL_H,
                                     icon_palette((uint8_t)v));
                }
            }
        }
        aa_text_on_row(y, ix + (DESK_CELL_W -
                                aa_text_width(desk_icons[i].label)) / 2,
                       iy + 46, desk_icons[i].label, COL_TEXT);
    }
}

/* ── Right-click context menus ──────────────────────────────────────────── */

#define CTX_W      168
#define CTX_ITEM_H 28

static int ctx_open;      /* 0 closed, 1 desktop menu, 2 taskbar-window menu */
static int ctx_x, ctx_y;
static int ctx_target;    /* window id for the taskbar-window menu */

static const char *const ctx_desktop_items[] = {
    "New Terminal", "New Browser", "Files", "Task Manager", "Settings",
};
static const char *const ctx_window_items[] = {
    "Restore", "Minimize", "Close",
};

static int ctx_item_count(void) {
    return ctx_open == 1 ? 5 : 3;
}

static void open_ctx_menu(int type, int x, int y, int target) {
    int n;

    ctx_open = type;
    ctx_target = target;
    n = ctx_item_count();
    ctx_x = x;
    ctx_y = y;
    if (ctx_x + CTX_W > (int)fb_w - 4) ctx_x = (int)fb_w - 4 - CTX_W;
    if (ctx_y + n * CTX_ITEM_H + 2 > (int)fb_h - TASKBAR_H - 4)
        ctx_y = (int)fb_h - TASKBAR_H - 4 - n * CTX_ITEM_H - 2;
    if (ctx_x < 0) ctx_x = 0;
    if (ctx_y < 0) ctx_y = 0;
}

static void close_window_like_button(desktop_window_t *win);

static void ctx_action(int item) {
    if (ctx_open == 1) {
        switch (item) {
        case 0: start_term(); break;
        case 1: start_browse(); break;
        case 2: start_files(); break;
        case 3: start_taskmgr(); break;
        case 4: start_settings(); break;
        }
    } else if (ctx_open == 2) {
        desktop_window_t *win = find_window(ctx_target);
        if (!win) return;
        switch (item) {
        case 0:
            if (win->minimized) restore_window(win);
            else focus_window(win->id);
            break;
        case 1:
            if (!win->minimized) minimize_window(win);
            break;
        case 2:
            close_window_like_button(win);
            break;
        }
    }
}

static void draw_ctx_menu_on_row(unsigned y) {
    int n = ctx_item_count();
    int mh = n * CTX_ITEM_H + 2;
    const char *const *items =
        ctx_open == 1 ? ctx_desktop_items : ctx_window_items;

    if (!ctx_open) return;
    soft_shadow_on_row(y, ctx_x, ctx_y, CTX_W, mh);
    fill_rect_on_row(y, ctx_x, ctx_y, CTX_W, mh, COL_TASKBAR_EDGE);
    blend_rect_on_row(y, ctx_x + 1, ctx_y + 1, CTX_W - 2, mh - 2,
                      COL_MENU_BG, 235);
    for (int i = 0; i < n; i++) {
        int iy = ctx_y + 1 + i * CTX_ITEM_H;
        if (in_rect((unsigned)mouse_x, (unsigned)mouse_y,
                    (unsigned)(ctx_x + 1), (unsigned)iy,
                    CTX_W - 2, CTX_ITEM_H))
            fill_rect_on_row(y, ctx_x + 1, iy, CTX_W - 2, CTX_ITEM_H,
                             col_accent);
        aa_text_on_row(y, ctx_x + 14, iy + (CTX_ITEM_H - FONT_UI16_LINE_H) / 2,
                       items[i], COL_TEXT);
    }
}

/* Right press: open the appropriate context menu. */
static void handle_right_click(int x, int y) {
    if (y >= (int)fb_h - TASKBAR_H) {
        int btn_y = (int)fb_h - TASKBAR_H + 4;
        int btn_h = TASKBAR_H - 8;
        int ids[MAX_WINDOWS], xs[MAX_WINDOWS];
        int n = taskbar_buttons(ids, xs, MAX_WINDOWS);

        for (int i = 0; i < n; i++) {
            if (in_rect((unsigned)x, (unsigned)y, (unsigned)xs[i],
                        (unsigned)btn_y, TB_BTN_W, (unsigned)btn_h)) {
                open_ctx_menu(2, x, y, ids[i]);
                return;
            }
        }
        ctx_open = 0;
        return;
    }
    if (!window_at(x, y)) {
        open_ctx_menu(1, x, y, -1);
        return;
    }
    ctx_open = 0;
}

/* ── Clock calendar popup ───────────────────────────────────────────────── */

static int cal_open;

/* Howard Hinnant's civil-from-days (days since 1970-01-01). */
static void civil_from_days(long z, int *yy, unsigned *mm, unsigned *dd) {
    long era;
    unsigned doe, yoe, doy, mp, d, m;
    long yr;

    z += 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = (unsigned)(z - era * 146097);
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    yr  = (long)yoe + era * 400;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp  = (5 * doy + 2) / 153;
    d   = doy - (153 * mp + 2) / 5 + 1;
    m   = mp < 10 ? mp + 3 : mp - 9;
    *yy = (int)(yr + (m <= 2));
    *mm = m;
    *dd = d;
}

const char *const month_names[12] = {
    "January", "February", "March", "April", "May", "June", "July",
    "August", "September", "October", "November", "December",
};

#define CAL_W 232
#define CAL_H 196

static void draw_calendar_on_row(unsigned y) {
    static const char *const dows = "SMTWTFS";
    long days, first_dow;
    int yy;
    unsigned mm, dd, dim;
    int cx = (int)fb_w - CAL_W - 8;
    int cy = (int)fb_h - TASKBAR_H - CAL_H - 8;
    char hdr[32];
    struct timeval tv;

    if (!cal_open) return;
    if ((int)y < cy - SHADOW_R || (int)y >= cy + CAL_H + SHADOW_R) return;
    if (gettimeofday(&tv, 0) != 0) return;
    days = tv.tv_sec / 86400;
    civil_from_days(days, &yy, &mm, &dd);
    {
        static const unsigned dim_tab[12] =
            { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
        dim = dim_tab[mm - 1];
        if (mm == 2 && ((yy % 4 == 0 && yy % 100 != 0) || yy % 400 == 0))
            dim = 29;
    }
    first_dow = ((days - (long)dd + 1) + 4) % 7;   /* dow of the 1st; 0=Sun */
    if (first_dow < 0) first_dow += 7;

    soft_shadow_on_row(y, cx, cy, CAL_W, CAL_H);
    fill_rect_on_row(y, cx, cy, CAL_W, CAL_H, COL_TASKBAR_EDGE);
    blend_rect_on_row(y, cx + 1, cy + 1, CAL_W - 2, CAL_H - 2,
                      COL_MENU_BG, 242);
    sprintf(hdr, "%s %u, %d", month_names[mm - 1], dd, yy);
    aa_text_on_row(y, cx + (CAL_W - aa_text_width(hdr)) / 2, cy + 8,
                   hdr, COL_TEXT);
    for (int c = 0; c < 7; c++) {
        char s[2] = { dows[c], 0 };
        aa_text_on_row(y, cx + 12 + c * 30 + 10, cy + 34, s, COL_TEXT_DIM);
    }
    for (unsigned day = 1; day <= dim; day++) {
        int cell = (int)first_dow + (int)day - 1;
        int col = cell % 7, r = cell / 7;
        int dx = cx + 12 + col * 30, dy = cy + 58 + r * 22;
        char s[4];
        sprintf(s, "%u", day);
        if (day == dd)
            fill_rect_on_row(y, dx + 2, dy - 2, 26, 21, col_accent);
        aa_text_on_row(y, dx + (30 - aa_text_width(s)) / 2, dy, s,
                       day == dd ? COL_TEXT : rgb(184, 192, 204));
    }
}

/* Click on the bare desktop: select an icon; double-click launches it. */
static void handle_desktop_click(int x, int y) {
    int hit = desk_icon_at(x, y);
    long t = now_ms();

    if (hit < 0) { desk_sel = -1; return; }
    if (hit == desk_sel && t - desk_sel_ms < 450) {
        desk_icons[hit].launch();
        desk_sel = -1;
        return;
    }
    desk_sel = hit;
    desk_sel_ms = t;
}

/* ── Win7 two-pane start menu ──────────────────────────────────────────── */
#define SM_LEFT_W   244
#define SM_RIGHT_W  158
#define SM_W        (SM_LEFT_W + SM_RIGHT_W)
#define SM_H        428
#define SM_ROW_H    34
#define SM_SEARCH_H 30

static char sm_search[28];
static int sm_search_len;

/* Left-pane program list (action = launcher_menu_action index). */
typedef struct { const char *label; int action; } sm_item_t;
static const sm_item_t sm_items[] = {
    { "Store",        11 }, { "Terminal", 0 }, { "Browser",      1 },
    { "Files",        2 }, { "Editor",   3 }, { "Calculator",   4 },
    { "Task Manager", 5 }, { "Console",  8 }, { "System Monitor", 9 },
};
#define SM_ITEMS ((int)(sizeof(sm_items) / sizeof(sm_items[0])))

static int sm_x(void) { return 2; }
static int sm_y(void) { return (int)fb_h - TASKBAR_H - SM_H - 2; }

/* substring match, case-insensitive (search filter) */
static int sm_match(const char *label) {
    int ll = (int)strlen(label), sl = sm_search_len;
    if (!sl) return 1;
    for (int i = 0; i + sl <= ll; i++) {
        int k = 0;
        while (k < sl) {
            char a = label[i + k], b = sm_search[k];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (a != b) break;
            k++;
        }
        if (k == sl) return 1;
    }
    return 0;
}

/* Filtered left-pane rows; returns count, fills indices.
 * Indices >= 1000 are installed packages (idx - 1000 into inst_apps). */
static int sm_filtered(int *out, int max) {
    int n = 0;
    for (int i = 0; i < SM_ITEMS && n < max; i++)
        if (sm_match(sm_items[i].label))
            out[n++] = i;
    for (int i = 0; i < inst_app_count && n < max; i++)
        if (sm_match(inst_apps[i].name))
            out[n++] = 1000 + i;
    return n;
}

/* Installed packages (from /disk/apps/<name>/manifest) */
inst_app_t inst_apps[MAX_INST_APPS];
int inst_app_count;

static void scan_installed_apps(void) {
    DIR *d = opendir("/disk/apps");
    struct dirent *e;

    inst_app_count = 0;
    if (!d) return;
    while ((e = readdir(d)) && inst_app_count < MAX_INST_APPS) {
        char mpath[160];
        char buf[512];
        int fd, n;
        inst_app_t *a;

        if (e->d_name[0] == '.') continue;
        snprintf(mpath, sizeof(mpath), "/disk/apps/%s/manifest", e->d_name);
        fd = open(mpath, O_RDONLY);
        if (fd < 0) continue;
        n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0) continue;
        buf[n] = 0;
        a = &inst_apps[inst_app_count];
        memset(a, 0, sizeof(*a));
        strncpy(a->name, e->d_name, sizeof(a->name) - 1);
        for (char *line = buf; line && *line; ) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = 0;
            if (!strncmp(line, "exec=", 5))
                strncpy(a->exec, line + 5, sizeof(a->exec) - 1);
            else if (!strncmp(line, "args=", 5))
                strncpy(a->args, line + 5, sizeof(a->args) - 1);
            else if (!strncmp(line, "fullscreen=", 11))
                a->fullscreen = atoi(line + 11);
            else if (!strncmp(line, "rawinput=", 9))
                a->rawinput = atoi(line + 9);
            line = nl ? nl + 1 : 0;
        }
        /* Fallback for manifests written before the rawinput field existed:
         * DOOM reads /dev/input/event0 directly, so it needs raw input. */
        if (!a->rawinput && (strstr(a->exec, "doom") || strstr(a->name, "doom")))
            a->rawinput = 1;
        if (a->exec[0])
            inst_app_count++;
    }
    closedir(d);
}

/* Launch an installed app.  GUI apps get a slot via start_multi_file;
 * fullscreen apps (DOOM) pause the desktop and own the screen until exit. */
static volatile int fullscreen_pid = -1;
static int fullscreen_kbd_fd = -1;   /* PTY master: desktop pumps keys in */
static int fullscreen_rawinput = 0;  /* app reads event0 itself (DOOM) */

#define TIOCSPTLCK 0x40045431U
#define TIOCGPTN_D 0x80045430U

static void launch_installed(const inst_app_t *a) {
    if (a->fullscreen) {
        int master = -1, slave = -1, pid;
        int unlock = 0, pty_num = 0;
        char pts_path[24];

        fullscreen_rawinput = a->rawinput;

        /* Clear the screen so the app's uncovered borders are black, not the
         * leftover desktop showing through (DOOM letterboxes when its integer
         * scale doesn't exactly fill the framebuffer). */
        blank_framebuffer();

        /* A raw-input app (DOOM) opens /dev/input/event0 itself; the desktop
         * must NOT also read event0 or the two would split keystrokes.  Such
         * apps still get a PTY as a controlling tty but we never pump keys. */
        if (a->rawinput) {
            int pid2 = fork();
            if (pid2 == 0) {
                int lfd = open("/tmp/app-out", O_WRONLY | O_CREAT | O_TRUNC);
                if (lfd >= 0) { dup2(lfd, 1); dup2(lfd, 2);
                                if (lfd > 2) close(lfd); }
                char *argv[8]; int ac = 0; static char argbuf[96];
                argv[ac++] = (char *)a->exec;
                if (a->args[0]) {
                    strncpy(argbuf, a->args, sizeof(argbuf) - 1);
                    char *p = argbuf;
                    while (*p && ac < 7) {
                        while (*p == ' ') *p++ = 0;
                        if (*p) argv[ac++] = p;
                        while (*p && *p != ' ') p++;
                    }
                }
                argv[ac] = 0;
                execve(a->exec, argv, desktop_envp);
                exit(127);
            }
            if (pid2 > 0) {
                fullscreen_pid = pid2;
                fullscreen_kbd_fd = -1;
                /* Register the emergency Ctrl+Alt+Backspace kill target: the
                 * desktop can't reach a raw-input app's keyboard, so the
                 * kernel keyboard IRQ provides the guaranteed escape. */
                syscall1(505, pid2);
                add_log("FULLSCREEN APP RUNNING (RAW INPUT)");
                add_log("PRESS CTRL+ALT+BACKSPACE TO QUIT");
            }
            return;
        }

        /* stdin PTY: fullscreen apps need a tty + raw keys from us */
        master = open("/dev/ptmx", O_RDWR);
        if (master >= 0 &&
            (ioctl(master, TIOCSPTLCK, &unlock) < 0 ||
             ioctl(master, TIOCGPTN_D, &pty_num) < 0)) {
            close(master);
            master = -1;
        }
        if (master >= 0) {
            sprintf(pts_path, "/dev/pts/%d", pty_num);
            slave = open(pts_path, O_RDWR);
            if (slave < 0) {
                close(master);
                master = -1;
            }
        }

        pid = fork();
        if (pid == 0) {
            char *argv[8];
            int ac = 0;
            static char argbuf[96];
            if (slave >= 0) {
                dup2(slave, 0);
                if (slave > 2) close(slave);
            }
            if (master >= 0) close(master);
            /* capture the app's output for debugging (cat /tmp/app-out) */
            int lfd = open("/tmp/app-out", O_WRONLY | O_CREAT | O_TRUNC);
            if (lfd >= 0) {
                dup2(lfd, 1);
                dup2(lfd, 2);
                if (lfd > 2) close(lfd);
            }
            argv[ac++] = (char *)a->exec;
            if (a->args[0]) {
                char *p;
                strncpy(argbuf, a->args, sizeof(argbuf) - 1);
                argbuf[sizeof(argbuf) - 1] = 0;
                p = argbuf;
                while (*p && ac < 7) {
                    argv[ac++] = p;
                    while (*p && *p != ' ') p++;
                    if (*p) *p++ = 0;
                }
            }
            argv[ac] = 0;
            execve(a->exec, argv, desktop_envp);
            exit(127);
        }
        if (slave >= 0) close(slave);
        if (pid > 0) {
            fullscreen_pid = pid;
            fullscreen_kbd_fd = master;       /* keep master for key pump */
            add_log("FULLSCREEN APP RUNNING");
        } else if (master >= 0) {
            close(master);
        }
        return;
    }
    start_multi_file(a->exec, "APP LAUNCHED", a->args[0] ? a->args : 0);
}

static const char *const sm_right_labels[] = {
    "Documents", "Pictures", "Computer", "Settings",
};
#define SM_RIGHT_ITEMS 4

/* Close semantics shared by the title-bar X and the context menu. */
static void close_window_like_button(desktop_window_t *win) {
    int idx;

    if (!win) return;
    idx = client_index_for_window(win->id);
    if (idx >= 0) {
        hide_client_app(idx);
    } else {
        win->visible = 0;
        if (active_window == win->id)
            focus_window(WIN_TERMINAL);
    }
}

static void handle_click(int x, int y) {
    desktop_window_t *win;

    /* Context menu gets the click first: pick an item or dismiss. */
    if (ctx_open) {
        int n = ctx_item_count();
        int was = ctx_open;
        if (x >= ctx_x && x < ctx_x + CTX_W && y >= ctx_y &&
            y < ctx_y + n * CTX_ITEM_H + 2) {
            int item = (y - ctx_y - 1) / CTX_ITEM_H;
            if (item >= 0 && item < n) ctx_action(item);
        }
        (void)was;
        ctx_open = 0;
        return;
    }
    if (cal_open) {           /* any click outside dismisses the calendar */
        cal_open = 0;
        if (y < (int)fb_h - TASKBAR_H) return;
    }

    /* Start menu: route the click to programs / places / power. */
    if (launcher_open) {
        int mx = sm_x(), my = sm_y();

        if (x < mx || x >= mx + SM_W || y < my || y >= my + SM_H) {
            launcher_open = 0;                /* clicked outside: dismiss */
            sm_search_len = 0;
            sm_search[0] = 0;
            return;
        }
        if (x < mx + SM_LEFT_W) {
            /* program rows (filtered) */
            int filt[SM_ITEMS + MAX_INST_APPS];
            int n = sm_filtered(filt, SM_ITEMS + MAX_INST_APPS);
            int idx = (y - (my + 12)) / SM_ROW_H;
            if (y >= my + 12 && idx >= 0 && idx < n) {
                launcher_open = 0;
                sm_search_len = 0;
                sm_search[0] = 0;
                if (filt[idx] >= 1000)
                    launch_installed(&inst_apps[filt[idx] - 1000]);
                else
                    launcher_menu_action(sm_items[filt[idx]].action);
            }
            return;   /* clicks in the pane (incl. search box) keep it open */
        }
        /* right pane */
        {
            int rx = mx + SM_LEFT_W;
            int py = my + SM_H - 42;
            if (y >= py && y < py + 28 && x >= rx + 18 &&
                x < rx + SM_RIGHT_W - 18) {
                launcher_open = 0;
                launcher_menu_action(10);      /* Power off */
                return;
            }
            for (int i = 0; i < SM_RIGHT_ITEMS; i++) {
                int iy = my + 100 + i * 32;
                if (y >= iy && y < iy + 32) {
                    launcher_open = 0;
                    sm_search_len = 0;
                    sm_search[0] = 0;
                    launcher_menu_action(i == 3 ? 6 : 2);  /* Settings/Files */
                    return;
                }
            }
        }
        return;
    }

    /* Taskbar. */
    if (y >= (int)fb_h - TASKBAR_H) {
        int btn_y = (int)fb_h - TASKBAR_H + 4;
        int btn_h = TASKBAR_H - 8;
        int ids[MAX_WINDOWS], xs[MAX_WINDOWS];
        int n;

        if (in_rect((unsigned)x, (unsigned)y, ORB_X,
                    (unsigned)((int)fb_h - TASKBAR_H - 4),
                    ORB_SIZE, ORB_SIZE)) {
            launcher_open = 1;
            return;
        }
        if (x >= (int)fb_w - 10) {         /* Show Desktop sliver */
            if (!show_desktop_active) {
                for (int i = 0; i < window_count; i++) {
                    sd_saved[i] = windows[i].minimized;
                    if (windows[i].visible) windows[i].minimized = 1;
                }
                show_desktop_active = 1;
            } else {
                for (int i = 0; i < window_count; i++)
                    windows[i].minimized = sd_saved[i];
                show_desktop_active = 0;
            }
            return;
        }
        if (x >= (int)fb_w - 100) {        /* clock → calendar popup */
            cal_open = !cal_open;
            return;
        }
        n = taskbar_buttons(ids, xs, MAX_WINDOWS);
        for (int i = 0; i < n; i++) {
            if (!in_rect((unsigned)x, (unsigned)y, (unsigned)xs[i],
                         (unsigned)btn_y, TB_BTN_W, (unsigned)btn_h))
                continue;
            win = find_window(ids[i]);
            if (!win) return;
            if (win->minimized) {
                restore_window(win);
            } else if (active_window == win->id) {
                minimize_window(win);   /* click active button → minimize */
            } else {
                focus_window(win->id);
            }
            return;
        }
        return;
    }

    /* Sticky note: click to focus (keyboard goes to the note). */
    if (in_note(x, y) && !window_at(x, y)) {
        note_focus = 1;
        return;
    }
    if (note_focus) {
        note_focus = 0;
        note_save();
    }

    win = window_at(x, y);
    if (!win) {
        handle_desktop_click(x, y);
        return;
    }
    if (win) {
        int win_id = win->id;
        if (in_close_button(win, x, y)) {
            close_window_like_button(win);
            return;
        }
        if (in_min_button(win, x, y)) {
            minimize_window(win);
            return;
        }
        if (in_max_button(win, x, y)) {
            focus_window(win_id);
            toggle_maximize(win);
            return;
        }
        focus_window(win_id);
        if (is_client_window(win_id)) {
            emit_client_mouse_event(x, y, mouse_buttons);
            /* Body presses start an app-level drag (motion streaming). */
            if (!in_titlebar(win, x, y) && !resize_edges_at(win, x, y))
                client_drag_slot = client_index_for_window(win_id) + 1;
        }
    }
}

static void begin_window_drag(int x, int y) {
    desktop_window_t *win = window_at(x, y);
    int win_id;
    int edges;
    int titlebar;

    if (!win) return;
    if (in_close_button(win, x, y)) return;
    if (in_min_button(win, x, y)) return;
    if (in_max_button(win, x, y)) return;
    win_id = win->id;
    edges = resize_edges_at(win, x, y);
    titlebar = in_titlebar(win, x, y);
    focus_window(win_id);
    drag_win_id = win_id;
    if (edges) {
        drag_mode = DRAG_RESIZE;
        drag_edges = edges;
        drag_dx = (edges & RESIZE_L) ? x - win->x : win->x + win->w - x;
        drag_dy = (edges & RESIZE_T) ? y - win->y : win->y + win->h - y;
        drag_right  = win->x + win->w;
        drag_bottom = win->y + win->h;
        return;
    }
    if (titlebar) {
        /* Dragging a maximized/snapped window tears it free at its
         * saved size, grabbed proportionally under the cursor. */
        if (win->maximized || win->snapped) {
            int rel = win->w ? (x - win->x) * win->sw / win->w : 0;
            win->w = win->sw;
            win->h = win->sh;
            win->maximized = 0;
            win->snapped = 0;
            win->x = x - rel;
            win->y = y - TITLEBAR_H / 2;
            clamp_window(win);   /* never leave the titlebar off-screen */
        }
        drag_mode = DRAG_MOVE;
        drag_dx = x - win->x;
        drag_dy = y - win->y;
        return;
    }
    drag_mode = 0;
    drag_win_id = -1;
}

#define WIN_MIN_W 180
#define WIN_MIN_H 120

static void update_window_drag(int x, int y) {
    desktop_window_t *win;

    if (!drag_mode || drag_win_id < 0) return;
    win = find_window(drag_win_id);
    if (!win) return;
    if (drag_mode == DRAG_MOVE) {
        win->x = x - drag_dx;
        win->y = y - drag_dy;
    } else if (drag_mode == DRAG_RESIZE) {
        if (drag_edges & RESIZE_L) {
            int nx = x - drag_dx;
            if (nx > drag_right - WIN_MIN_W) nx = drag_right - WIN_MIN_W;
            win->x = nx;
            win->w = drag_right - nx;
        }
        if (drag_edges & RESIZE_R) {
            win->w = x + drag_dx - win->x;
            if (win->w < WIN_MIN_W) win->w = WIN_MIN_W;
        }
        if (drag_edges & RESIZE_T) {
            int ny = y - drag_dy;
            if (ny > drag_bottom - WIN_MIN_H) ny = drag_bottom - WIN_MIN_H;
            win->y = ny;
            win->h = drag_bottom - ny;
        }
        if (drag_edges & RESIZE_B) {
            win->h = y + drag_dy - win->y;
            if (win->h < WIN_MIN_H) win->h = WIN_MIN_H;
        }
    }
    clamp_window(win);
}

/* Aero Snap: save geometry once, then pin to a half/full screen. */
static void snap_window(desktop_window_t *win, int mode) {
    if (!win->maximized && !win->snapped) {
        win->sx = win->x;
        win->sy = win->y;
        win->sw = win->w;
        win->sh = win->h;
    }
    if (mode == 0) {                      /* top edge → maximize */
        win->x = 8;
        win->y = 8;
        win->w = (int)fb_w - 16;
        win->h = (int)fb_h - TASKBAR_H - 16;
        win->maximized = 1;
        win->snapped = 0;
    } else {                              /* 1 = left half, 2 = right half */
        win->x = mode == 1 ? 4 : (int)fb_w / 2 + 2;
        win->y = 6;
        win->w = (int)fb_w / 2 - 6;
        win->h = (int)fb_h - TASKBAR_H - 12;
        win->snapped = 1;
        win->maximized = 0;
    }
    clamp_window(win);
}

static void end_window_drag(void) {
    desktop_window_t *win;

    win = drag_win_id >= 0 ? find_window(drag_win_id) : (desktop_window_t *)0;
    /* Aero Snap on drop at a screen edge (move drags only). */
    if (win && drag_mode == DRAG_MOVE) {
        if (mouse_x <= 2)                      snap_window(win, 1);
        else if (mouse_x >= (int)fb_w - 3)     snap_window(win, 2);
        else if (mouse_y <= 2)                 snap_window(win, 0);
    }
    emit_client_geom_event(win);
    drag_mode = 0;
    drag_win_id = -1;
}

static char key_to_char(uint16_t key) {
    static const char normal[] = {
        [KEY_1] = '1', [KEY_2] = '2', [KEY_3] = '3', [KEY_4] = '4',
        [KEY_5] = '5', [KEY_6] = '6', [KEY_7] = '7', [KEY_8] = '8',
        [KEY_9] = '9', [KEY_0] = '0', [KEY_MINUS] = '-', [KEY_EQUAL] = '=',
        [KEY_Q] = 'q', [KEY_W] = 'w', [KEY_E] = 'e', [KEY_R] = 'r',
        [KEY_T] = 't', [KEY_Y] = 'y', [KEY_U] = 'u', [KEY_I] = 'i',
        [KEY_O] = 'o', [KEY_P] = 'p', [KEY_A] = 'a', [KEY_S] = 's',
        [KEY_D] = 'd', [KEY_F] = 'f', [KEY_G] = 'g', [KEY_H] = 'h',
        [KEY_J] = 'j', [KEY_K] = 'k', [KEY_L] = 'l', [KEY_Z] = 'z',
        [KEY_X] = 'x', [KEY_C] = 'c', [KEY_V] = 'v', [KEY_B] = 'b',
        [KEY_N] = 'n', [KEY_M] = 'm', [KEY_SPACE] = ' ',
        [KEY_COMMA] = ',', [KEY_DOT] = '.', [KEY_SLASH] = '/',
        [KEY_SEMICOLON] = ':', [KEY_APOSTROPHE] = '\'', [KEY_LEFTBRACE] = '[',
        [KEY_RIGHTBRACE] = ']', [KEY_BACKSLASH] = '\\', [KEY_GRAVE] = '`'
    };
    static const char shifted[] = {
        [KEY_1] = '!', [KEY_2] = '@', [KEY_3] = '#', [KEY_4] = '$',
        [KEY_5] = '%', [KEY_6] = '^', [KEY_7] = '&', [KEY_8] = '*',
        [KEY_9] = '(', [KEY_0] = ')', [KEY_MINUS] = '_', [KEY_EQUAL] = '+',
        [KEY_COMMA] = '<', [KEY_DOT] = '>', [KEY_SLASH] = '?',
        [KEY_SEMICOLON] = ';', [KEY_APOSTROPHE] = '"', [KEY_LEFTBRACE] = '{',
        [KEY_RIGHTBRACE] = '}', [KEY_BACKSLASH] = '|', [KEY_GRAVE] = '~'
    };
    char c = 0;
    if (key < sizeof(normal)) c = normal[key];
    if (shift_down && key < sizeof(shifted) && shifted[key]) c = shifted[key];
    if (c >= 'a' && c <= 'z' && (shift_down ^ caps_on)) c -= 32;
    return c;
}

/* Alt-Tab: focus the next visible, non-minimized window after the active. */
static void cycle_windows(void) {
    int ids[MAX_WINDOWS];
    int n = 0, cur = -1;

    for (int i = 0; i < MAX_WINDOWS; i++) {
        int id = i < 2 ? i : WIN_CLIENT_BASE + (i - 2);
        desktop_window_t *win = find_window(id);
        if (!win || !win->visible || win->minimized) continue;
        if (id == active_window) cur = n;
        ids[n++] = id;
    }
    if (n < 2) return;
    focus_window(ids[(cur + 1) % n]);
}

static void handle_key(uint16_t code, int value) {
    char out;

    /* Uncooked forwarding first, with the modifier mask as it was BEFORE this
     * event and before any of the desktop's own key handling below consumes
     * it.  Skipped for the keys the desktop keeps for itself (Alt-Tab, Escape)
     * and while a desktop text field owns the keyboard, so a client never sees
     * a keystroke the desktop also acted on. */
    if (!note_focus && !launcher_open && code != KEY_ESC &&
        !(alt_down && code == KEY_TAB)) {
        int mods = (shift_down ? WM_MOD_SHIFT : 0) | (caps_on ? WM_MOD_LOCK : 0) |
                   (ctrl_down ? WM_MOD_CTRL : 0) | (alt_down ? WM_MOD_ALT : 0);
        emit_client_rawkey_event(code, value, mods);
    }

    if (code == KEY_LEFTSHIFT || code == KEY_RIGHTSHIFT) {
        shift_down = value != 0;
        return;
    }
    if (code == KEY_LEFTCTRL || code == KEY_RIGHTCTRL) {
        ctrl_down = value != 0;
        return;
    }
    if (code == KEY_LEFTALT) {
        alt_down = value != 0;
        return;
    }
    if (!value) return;
    if (alt_down && code == KEY_TAB) {
        cycle_windows();
        return;
    }
    if (code == KEY_CAPSLOCK) {
        caps_on = !caps_on;
        return;
    }
    if (note_focus) {               /* sticky-note editing */
        if (code == KEY_ESC || code == KEY_ENTER) {
            note_focus = 0;
            note_save();
            return;
        }
        if (code == KEY_BACKSPACE) {
            if (note_len) note_text[--note_len] = 0;
            return;
        }
        {
            char ch = key_to_char(code);
            if (ch && note_len + 1 < (int)sizeof(note_text)) {
                note_text[note_len++] = ch;
                note_text[note_len] = 0;
            }
        }
        return;
    }
    if (launcher_open) {            /* start-menu search capture */
        if (code == KEY_ESC) {
            launcher_open = 0;
            sm_search_len = 0;
            sm_search[0] = 0;
            return;
        }
        if (code == KEY_BACKSPACE) {
            if (sm_search_len) sm_search[--sm_search_len] = 0;
            return;
        }
        if (code == KEY_ENTER) {    /* launch the first match */
            int filt[SM_ITEMS + MAX_INST_APPS];
            int n = sm_filtered(filt, SM_ITEMS + MAX_INST_APPS);
            launcher_open = 0;
            if (n > 0) {
                if (filt[0] >= 1000)
                    launch_installed(&inst_apps[filt[0] - 1000]);
                else
                    launcher_menu_action(sm_items[filt[0]].action);
            }
            sm_search_len = 0;
            sm_search[0] = 0;
            return;
        }
        {
            char ch = key_to_char(code);
            if (ch && sm_search_len + 1 < (int)sizeof(sm_search)) {
                sm_search[sm_search_len++] = ch;
                sm_search[sm_search_len] = 0;
            }
        }
        return;
    }
    if (code == KEY_ESC) {
        if (is_client_window(active_window)) {
            int idx = client_index_for_window(active_window);
            hide_client_app(idx);
        }
        return;
    }
    if (is_client_window(active_window)) {
        char ch = key_to_char(code);
        /* Apply Ctrl here: clients receive ready-to-use control bytes. */
        if (ctrl_down) {
            if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 1);
            else if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 1);
        }
        emit_client_key_event(code, value, ch);
        return;
    }
    if (active_window != WIN_TERMINAL) return;

    /* Real terminal: pass keystrokes straight to the shell's PTY.  The PTY
     * does line editing, echo, ^C → SIGINT, etc.; we just render its output. */
    if (shell_fd < 0) {
        if (code == KEY_ENTER) start_shell();
        return;
    }
    if (code == KEY_ENTER)          out = '\n';
    else if (code == KEY_BACKSPACE) out = 127;
    else if (code == KEY_TAB)       out = '\t';
    else {
        out = key_to_char(code);
        if (!out) return;
        if (ctrl_down) {
            if (out >= 'a' && out <= 'z') out = (char)(out - 'a' + 1);
            else if (out >= 'A' && out <= 'Z') out = (char)(out - 'A' + 1);
            else return;
        }
    }
    term_view = 0;   /* typing snaps the scrollback to the bottom */
    write(shell_fd, &out, 1);
}

static void clamp_mouse(void) {
    if (mouse_x < 0) mouse_x = 0;
    if (mouse_y < 0) mouse_y = 0;
    if (mouse_x >= (int)fb_w) mouse_x = (int)fb_w - 1;
    if (mouse_y >= (int)fb_h) mouse_y = (int)fb_h - 1;
}

/* Route a wheel notch: terminal scrolls its scrollback, client windows get
 * a "scroll" event at the cursor position. */
static void handle_wheel(int delta) {
    desktop_window_t *win = window_at(mouse_x, mouse_y);

    if (!win) return;
    if (win->id == WIN_TERMINAL) {
        term_view += delta * 3;
        if (term_view < 0) term_view = 0;
        return;
    }
    if (is_client_window(win->id)) {
        int slot = client_index_for_window(win->id) + 1;
        emit_wm_event5("scroll %d %d %d %d %d", slot,
                       mouse_x - win->x, mouse_y - win->y, delta, 0);
    }
}

/* While a press that began in a client body is held, stream motion events so
 * apps can implement dragging (scrollbar thumbs etc.). */
static void emit_client_drag_motion(void) {
    desktop_window_t *win;

    if (!client_drag_slot) return;
    win = find_window(WIN_CLIENT_BASE + client_drag_slot - 1);
    if (!win || !win->visible || win->minimized) {
        client_drag_slot = 0;
        return;
    }
    emit_wm_event("mouse %d %d %d %d", client_drag_slot,
                  mouse_x - win->x, mouse_y - win->y, 1);
}

static void end_client_drag(void) {
    desktop_window_t *win;

    if (!client_drag_slot) return;
    win = find_window(WIN_CLIENT_BASE + client_drag_slot - 1);
    if (win)
        emit_wm_event("mouse %d %d %d %d", client_drag_slot,
                      mouse_x - win->x, mouse_y - win->y, 0);
    client_drag_slot = 0;
}

static void handle_mouse(const struct input_event *ev) {
    if (ev->type == EV_REL) {
        if (ev->code == REL_WHEEL) {
            handle_wheel(ev->value);
            return;
        }
        if (ev->code == REL_X) mouse_x += ev->value;
        if (ev->code == REL_Y) mouse_y += ev->value;
        clamp_mouse();
        update_window_drag(mouse_x, mouse_y);
        if (mouse_buttons & 1)
            emit_client_drag_motion();
        return;
    }

    if (ev->type == EV_KEY) {
        int bit = 0;
        if (ev->code == BTN_LEFT) bit = 1;
        if (ev->code == BTN_RIGHT) bit = 2;
        if (ev->code == BTN_MIDDLE) bit = 4;
        if (bit) {
            prev_mouse_buttons = mouse_buttons;
            if (ev->value) mouse_buttons |= bit;
            else mouse_buttons &= ~bit;
            if (bit == 1 && (mouse_buttons & 1) && !(prev_mouse_buttons & 1)) {
                if (ctx_open) {
                    handle_click(mouse_x, mouse_y);
                } else {
                    begin_window_drag(mouse_x, mouse_y);
                    handle_click(mouse_x, mouse_y);
                }
            }
            if (bit == 1 && !(mouse_buttons & 1) && (prev_mouse_buttons & 1)) {
                end_window_drag();
                end_client_drag();
            }
            if (bit == 2 && (mouse_buttons & 2) && !(prev_mouse_buttons & 2))
                handle_right_click(mouse_x, mouse_y);
        }
    }
}

/* Cursor shapes: arrow, resize variants, text I-beam (16x16 masks). */
#define CUR_ARROW    0
#define CUR_RESIZE   1
#define CUR_IBEAM    2
#define CUR_RESIZE_H 3
#define CUR_RESIZE_V 4
static int cursor_shape = CUR_ARROW;

static const uint16_t cursor_shapes[5][16] = {
    { 0x8000, 0xc000, 0xe000, 0xf000,
      0xf800, 0xfc00, 0xfe00, 0xff00,
      0xff80, 0xf800, 0xdc00, 0x8c00,
      0x0600, 0x0600, 0x0300, 0x0300 },
    { 0x0000, 0x7800, 0x7000, 0x7800,
      0x5c00, 0x0e00, 0x0700, 0x0380,
      0x01c0, 0x00e8, 0x0078, 0x0038,
      0x0078, 0x00f8, 0x0000, 0x0000 },   /* NW-SE resize arrows */
    { 0x0000, 0x6c00, 0x1000, 0x1000,
      0x1000, 0x1000, 0x1000, 0x1000,
      0x1000, 0x1000, 0x1000, 0x1000,
      0x1000, 0x6c00, 0x0000, 0x0000 },   /* I-beam */
    { 0x0000, 0x0000, 0x0000, 0x0480,
      0x0c60, 0x1c70, 0x3ffc, 0x7ffe,
      0x3ffc, 0x1c70, 0x0c60, 0x0480,
      0x0000, 0x0000, 0x0000, 0x0000 },   /* ↔ horizontal resize */
    { 0x0100, 0x0380, 0x07c0, 0x0fe0,
      0x0380, 0x0380, 0x0380, 0x0380,
      0x0380, 0x0380, 0x0380, 0x0fe0,
      0x07c0, 0x0380, 0x0100, 0x0000 },   /* ↕ vertical resize */
};

static void draw_cursor_on_row(unsigned y, unsigned w) {
    const uint16_t *shape = cursor_shapes[cursor_shape];
    if (y < (unsigned)mouse_y || y >= (unsigned)mouse_y + 16) return;

    unsigned cy = y - (unsigned)mouse_y;
    for (unsigned x = 0; x < 16; x++) {
        if ((shape[cy] & (0x8000 >> x)) == 0) continue;
        int px = mouse_x + (int)x;
        if (px < 0 || px >= (int)w) continue;

        int edge = 0;
        if (x == 0 || cy == 0) edge = 1;
        if (x > 0 && (shape[cy] & (0x8000 >> (x - 1))) == 0) edge = 1;
        if (cy > 0 && (shape[cy - 1] & (0x8000 >> x)) == 0) edge = 1;
        row[px] = edge ? rgb(0, 0, 0) :
                         (mouse_buttons ? rgb(255, 214, 88) : rgb(255, 255, 255));
    }
}

/* Pick the cursor for the current hover position (called once per frame). */
static int cursor_for_edges(int e) {
    int horiz = e & (RESIZE_L | RESIZE_R);
    int vert  = e & (RESIZE_T | RESIZE_B);

    if (horiz && vert) return CUR_RESIZE;
    if (horiz) return CUR_RESIZE_H;
    if (vert) return CUR_RESIZE_V;
    return CUR_ARROW;
}

static void update_cursor_shape(void) {
    desktop_window_t *win = window_at(mouse_x, mouse_y);

    cursor_shape = CUR_ARROW;
    if (drag_mode == DRAG_RESIZE) {
        cursor_shape = cursor_for_edges(drag_edges);
        return;
    }
    if (drag_mode) return;
    if (win)
        cursor_shape = cursor_for_edges(resize_edges_at(win, mouse_x, mouse_y));
}

static void draw_window_content_on_row(unsigned y, const desktop_window_t *win,
                                       const char *status) {
    if (!win || !win->visible || win->minimized) return;

    if (win->id == WIN_TERMINAL) {
        /* Pure shell console: scrollback tail + live line + block cursor. */
        unsigned base_y = (unsigned)win->y + TITLEBAR_H + 12;
        int rows = (win->h - TITLEBAR_H - 26) / 18;
        int total = term_count + 1;          /* +1 = live (prompt) line */
        int start = total > rows ? total - rows : 0;
        int vis = 0;

        int ccx = win->x + 1, ccx2 = win->x + win->w - 1;
        if (rows < 1) return;
        /* Mouse-wheel scrollback: view older lines; 0 = pinned to bottom. */
        if (term_view > start) term_view = start;
        if (term_view < 0) term_view = 0;
        start -= term_view;
        for (int i = start; i < term_count && vis < rows; i++, vis++)
            mono_text_clip_on_row(y, win->x + 16, (int)(base_y + (unsigned)vis * 18),
                                  term_lines[i], rgb(220, 220, 210), ccx, ccx2);
        if (vis < rows) {
            char live[LOG_MAX];
            unsigned ly = base_y + (unsigned)vis * 18;
            memcpy(live, output_line, (size_t)output_used);
            live[output_used] = 0;
            mono_text_clip_on_row(y, win->x + 16, (int)ly, live,
                                  rgb(220, 220, 210), ccx, ccx2);
            if (active_window == WIN_TERMINAL && shell_fd >= 0)
                fill_rect_on_row(y, win->x + 16 + output_used * MONO_ADV,
                                 (int)ly, MONO_ADV, FONT_MONO16_LINE_H - 3, col_accent);
        }
        return;
    }

    if (win->id == WIN_STATUS) {
        const char *focus_status = active_window == WIN_TERMINAL ?
            "FOCUS TERMINAL" : is_client_window(active_window) ? "FOCUS APP" : "FOCUS SYSTEM";
        if (drag_mode == DRAG_MOVE) focus_status = "DRAG MOVE";
        if (drag_mode == DRAG_RESIZE) focus_status = "DRAG RESIZE";

        int scx = win->x + 1, scx2 = win->x + win->w - 1;
        aa_text_clip_on_row(y, win->x + 16, win->y + 40,
                            status, rgb(30, 34, 36), scx, scx2);
        aa_text_clip_on_row(y, win->x + 16, win->y + 62,
                            focus_status, rgb(30, 34, 36), scx, scx2);
        aa_text_clip_on_row(y, win->x + 16, win->y + 84,
                            client_status, rgb(30, 34, 36), scx, scx2);
        /* Recent desktop events live here now, not in the terminal. */
        for (int i = 0; i < 2; i++) {
            int idx = log_count - 2 + i;
            if (idx < 0) continue;
            aa_text_clip_on_row(y, win->x + 16,
                                win->y + 108 + i * 18,
                                log_lines[idx], rgb(96, 104, 116), scx, scx2);
        }
        return;
    }

    if (is_client_window(win->id)) {
        int idx = client_index_for_window(win->id);
        int has_content = client_surface_has_content(idx);
        char label[32];
        int bx = win->x + 8;
        int by = win->y + 34;
        int bw = win->w - 16;
        int bh = win->h - 44;

        /* Modern path: blit the client's shared pixel buffer (full body). */
        if (client_surfaces[idx].surf) {
            client_surface_t *cs = &client_surfaces[idx];
            int px0 = win->x + 1;
            int py0 = win->y + TITLEBAR_H;
            int pw = win->w - 2;
            int ph = win->h - TITLEBAR_H - 1;
            int sy = (int)y - py0;

            if (pw > cs->surf_w) pw = cs->surf_w;
            if (ph > cs->surf_h) ph = cs->surf_h;
            if (sy >= 0 && sy < ph) {
                uint32_t *src = cs->surf + (size_t)sy * cs->surf_w;
                int x0 = px0 < 0 ? 0 : px0;
                int x1 = px0 + pw;
                if (x1 > (int)fb_w) x1 = (int)fb_w;
                if (x1 > MAX_W) x1 = MAX_W;
                for (int dx = x0; dx < x1; dx++)
                    row[dx] = src[dx - px0];
            }
            return;
        }

        fill_rect_clip_on_row(y, bx, by, bw, bh, bx, by, bw, bh,
                              client_surfaces[idx].bg);
        for (int i = 0; i < CLIENT_RECTS; i++) {
            client_rect_t *rect = &client_surfaces[idx].rects[i];
            if (!rect->used || rect->w <= 0 || rect->h <= 0) continue;
            fill_rect_clip_on_row(y, bx + rect->x, by + rect->y,
                                  rect->w, rect->h, bx, by, bw, bh,
                                  rect->color);
        }
        for (int i = 0; i < CLIENT_ICON_USES; i++) {
            icon_use_t *use = &client_surfaces[idx].icons[i];
            if (!use->used) continue;
            draw_client_icon_on_row(y, &client_icons[idx][use->icon],
                                    bx + use->x, by + use->y,
                                    bx, by, bw, bh);
        }
        if (!has_content) {
            sprintf(label, "WM CLIENT SURFACE %d", idx + 1);
            draw_text_clip_on_row(y, bx + 10, by + 8, label,
                                  rgb(30, 34, 36), 1, bx, by, bw, bh);
        }
        for (int i = 0; i < CLIENT_LINES; i++) {
            if (!client_surfaces[idx].lines[i][0]) continue;
            draw_text_clip_on_row(y, bx + client_surfaces[idx].line_x[i],
                                  by + client_surfaces[idx].line_y[i],
                                  client_surfaces[idx].lines[i],
                                  client_surfaces[idx].line_color[i], 1,
                                  bx, by, bw, bh);
        }
        if (!has_content) {
            draw_text_clip_on_row(y, bx + 10, by + bh - 28,
                                  client_text[idx], rgb(30, 34, 36), 1,
                                  bx, by, bw, bh);
        }
    }
}

/* ── Taskbar / launcher menu ─────────────────────────────────────────────── */

/* Stable taskbar button layout (z-order independent). Returns count. */
static int taskbar_buttons(int *ids, int *xs, int max) {
    int n = 0;
    int x = ORB_X + ORB_SIZE + 10;

    for (int i = 0; i < MAX_WINDOWS && n < max; i++) {
        int id = i < 2 ? i : WIN_CLIENT_BASE + (i - 2);
        desktop_window_t *win = find_window(id);
        if (!win || (!win->visible && !win->minimized)) continue;
        if (x + TB_BTN_W > (int)fb_w - 88) break;   /* leave room for clock */
        ids[n] = id;
        xs[n] = x;
        n++;
        x += TB_BTN_W + 4;
    }
    return n;
}

/* App icon art for a taskbar button, by window title. */
static const char *const *taskbar_art_for_title(const char *title) {
    if (!strncmp(title, "Terminal", 8) || !strncmp(title, "Console", 7))
        return desk_art_term;
    if (!strncmp(title, "Browser", 7))  return desk_art_web;
    if (!strncmp(title, "Files", 5))    return desk_art_files;
    if (!strncmp(title, "Widgets", 7) || !strncmp(title, "UI", 2) ||
        !strncmp(title, "System", 6))
        return desk_art_demo;
    return desk_art_demo;
}

/* Themed .mic icon name for an app label/window title (NULL = use hex-art). */
static const char *icon_name_for_label(const char *t) {
    if (!t) return NULL;
    if (!strncmp(t, "Terminal", 8) || !strncmp(t, "Console", 7)) return "terminal";
    if (!strncmp(t, "Browser", 7) || !strncmp(t, "links", 5))    return "browser";
    if (!strncmp(t, "Files", 5))                                 return "files";
    if (!strncmp(t, "Editor", 6))                                return "editor";
    if (!strncmp(t, "Calculator", 10))                           return "calc";
    if (!strncmp(t, "Task", 4) || !strncmp(t, "System Mon", 10)) return "sysmon";
    if (!strncmp(t, "Settings", 8))                              return "settings";
    if (!strncmp(t, "Store", 5))                                 return "store";
    if (!strncmp(t, "doom", 4) || !strncmp(t, "DOOM", 4))        return "doom";
    if (!strncmp(t, "firefox", 7) || !strncmp(t, "Firefox", 7))  return "firefox";
    if (!strncmp(t, "System", 6))                                return "sysmon";
    if (!strncmp(t, "Viewer", 6) || !strncmp(t, "Image", 5))     return "image";
    /* Generic themed app icon — never fall back to blocky hex-art. */
    return "exec";
}

/* Taskbar-button rect for a window (animation target / context menus). */
static void taskbar_button_rect_for(int id, int *bx, int *by, int *bw, int *bh) {
    int ids[MAX_WINDOWS], xs[MAX_WINDOWS];
    int n = taskbar_buttons(ids, xs, MAX_WINDOWS);

    *bx = (int)fb_w / 2;
    *by = (int)fb_h - TASKBAR_H + 4;
    *bw = TB_BTN_W;
    *bh = TASKBAR_H - 8;
    for (int i = 0; i < n; i++)
        if (ids[i] == id) { *bx = xs[i]; break; }
}


/* ── Taskbar live thumbnails ────────────────────────────────────────────── */

/* Draw one scanline of the live preview for win into a THUMB_W x THUMB_H
 * box at (bx, by): client surfaces are downscaled; console/system windows
 * get a mini text rendering. */
static void thumb_content_on_row(unsigned y, const desktop_window_t *win,
                                 int bx, int by) {
    int ty = (int)y - by;

    if (ty < 0 || ty >= THUMB_H) return;
    if (is_client_window(win->id)) {
        int idx = client_index_for_window(win->id);
        client_surface_t *cs = &client_surfaces[idx];
        if (cs->surf && cs->surf_w > 0 && cs->surf_h > 0) {
            const uint32_t *src =
                cs->surf + (size_t)(ty * cs->surf_h / THUMB_H) * cs->surf_w;
            for (int tx = 0; tx < THUMB_W; tx++) {
                int px = bx + tx;
                if (px >= 0 && px < (int)fb_w && px < MAX_W)
                    row[px] = src[tx * cs->surf_w / THUMB_W];
            }
            return;
        }
    }
    /* console/system: dark card + a few mini text lines */
    fill_rect_on_row(y, bx, by, THUMB_W, THUMB_H,
                     win->id == WIN_TERMINAL ? rgb(14, 18, 20)
                                             : rgb(244, 244, 238));
    if (win->id == WIN_TERMINAL) {
        int li = ty / 14;
        if (li >= 0 && li < 8 && li < term_count)
            aa_text_clip_on_row(y, bx + 4, by + li * 14,
                                term_lines[term_count - 1 - li < 0 ? 0 :
                                           term_count - 1 - li],
                                rgb(170, 180, 175), bx + 4,
                                bx + THUMB_W - 4);
    }
}

static void draw_thumbnail_on_row(unsigned y) {
    desktop_window_t *win;
    int cy = (int)fb_h - TASKBAR_H - THUMB_CARD_H - 8;

    if (thumb_win_id < 0) return;
    win = find_window(thumb_win_id);
    if (!win || !win->visible) { return; }
    if ((int)y < cy - SHADOW_R || (int)y >= cy + THUMB_CARD_H + SHADOW_R)
        return;

    soft_shadow_on_row(y, thumb_x, cy, THUMB_CARD_W, THUMB_CARD_H);
    glass_rect_on_row(y, thumb_x, cy, THUMB_CARD_W, THUMB_CARD_H,
                      rgb(20, 26, 36), 170);
    if ((int)y == cy)
        blend_rect_on_row(y, thumb_x, cy, THUMB_CARD_W, 1, 0xFFFFFF, 80);
    aa_text_clip_on_row(y, thumb_x + 8, cy + 6, win->title, 0xFFFFFF,
                        thumb_x + 8, thumb_x + THUMB_CARD_W - 8);
    thumb_content_on_row(y, win, thumb_x + 8, cy + 30);
}

/* Track which taskbar button the mouse is over (called per frame). */
static void update_thumbnail(void) {
    int bar_y = (int)fb_h - TASKBAR_H;
    int ids[MAX_WINDOWS], xs[MAX_WINDOWS];
    int n;

    thumb_win_id = -1;
    if (mouse_y < bar_y || launcher_open || ctx_open) return;
    n = taskbar_buttons(ids, xs, MAX_WINDOWS);
    for (int i = 0; i < n; i++) {
        if (mouse_x >= xs[i] && mouse_x < xs[i] + TB_BTN_W) {
            desktop_window_t *win = find_window(ids[i]);
            if (!win || win->minimized) return;   /* no live preview */
            thumb_win_id = ids[i];
            thumb_x = xs[i] + TB_BTN_W / 2 - THUMB_CARD_W / 2;
            if (thumb_x < 4) thumb_x = 4;
            if (thumb_x + THUMB_CARD_W > (int)fb_w - 4)
                thumb_x = (int)fb_w - 4 - THUMB_CARD_W;
            return;
        }
    }
}

/* ── Desktop gadgets (clock, calendar mini, sticky note, slideshow) ─────── */

static void note_load(void) {
    int fd = open("/disk/etc/notes.txt", O_RDONLY);
    int n;

    if (fd < 0) return;
    n = read(fd, note_text, sizeof(note_text) - 1);
    close(fd);
    if (n > 0) {
        note_text[n] = 0;
        note_len = n;
    }
}

static void note_save(void) {
    int fd;

    mkdir("/disk/etc", 0755);
    fd = open("/disk/etc/notes.txt", O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return;
    write(fd, note_text, note_len);
    close(fd);
}

/* Bresenham line on the current scanline only (for clock hands): we just
 * test whether the line crosses this y and paint the x range it spans. */
static void hand_on_row(unsigned y, int cx, int cy, int ex, int ey,
                        uint32_t color, int thick) {
    int x0 = cx, y0 = cy, x1 = ex, y1 = ey;
    int dx = x1 - x0, dy = y1 - y0;
    int steps = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy)
                ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy);

    if (steps == 0) steps = 1;
    for (int i = 0; i <= steps; i++) {
        int px = x0 + dx * i / steps;
        int py = y0 + dy * i / steps;
        if (py == (int)y)
            fill_rect_on_row(y, px, py, thick, 1, color);
    }
}

/* sin/cos via small integer table (degrees in minutes-of-clock: 0..59). */
static const int sin60[60] = {
      0,  10,  21,  31,  41,  50,  59,  67,  74,  81,
     87,  91,  95,  98, 100, 100, 100,  98,  95,  91,
     87,  81,  74,  67,  59,  50,  41,  31,  21,  10,
      0, -10, -21, -31, -41, -50, -59, -67, -74, -81,
    -87, -91, -95, -98,-100,-100,-100, -98, -95, -91,
    -87, -81, -74, -67, -59, -50, -41, -31, -21, -10,
};
#define COS60(m) sin60[((m) + 15) % 60]

static void draw_gadgets_on_row(unsigned y) {
    int gx = GADGET_X;

    if (!gadgets_visible) return;

    /* ── analog clock (top) ── */
    {
        int cy0 = 24, R = GADGET_W / 2 - 4;
        int cx = gx + GADGET_W / 2, cyc = cy0 + GADGET_W / 2;
        int dy = (int)y - cyc;
        if (dy >= -R - 2 && dy <= R + 2) {
            /* Anti-aliased face + rim: per-pixel radial coverage (distance in
             * quarter-pixel fixed point) fades the outer edge over ~1px so the
             * disc and its dark rim read smooth, not stair-stepped. */
            int R4 = R * 4, rim4 = R4 - 8;     /* rim = outer ~2px ring */
            int x0 = cx - R - 2, x1 = cx + R + 2;
            if (x0 < 0) x0 = 0;
            for (int px = x0; px <= x1 && px < (int)fb_w && px < MAX_W; px++) {
                int dx = px - cx;
                int dd4 = isqrt((dx * dx + dy * dy) * 16);   /* distance * 4 */
                if (dd4 > R4 + 4) continue;                  /* outside + AA */
                uint32_t col = (dd4 >= rim4) ? rgb(60, 66, 76) : 0xFFFFFFu;
                unsigned a = 210;
                if (dd4 > R4) a = a * (unsigned)(R4 + 4 - dd4) / 4;  /* edge fade */
                blend_rect_on_row(y, px, (int)y, 1, 1, col, a);
            }
            /* hands from RTC time */
            {
                struct timeval tv;
                if (gettimeofday(&tv, 0) == 0) {
                    int secs = (int)(tv.tv_sec % 86400);
                    int hh = (secs / 3600) % 12, mm = (secs / 60) % 60;
                    int hm = hh * 5 + mm / 12;        /* hour hand pos */
                    hand_on_row(y, cx, cyc,
                                cx + sin60[hm] * (R - 22) / 100,
                                cyc - COS60(hm) * (R - 22) / 100,
                                rgb(20, 24, 30), 3);
                    hand_on_row(y, cx, cyc,
                                cx + sin60[mm] * (R - 10) / 100,
                                cyc - COS60(mm) * (R - 10) / 100,
                                rgb(20, 24, 30), 2);
                }
            }
        }
    }

    /* ── sticky note ── */
    {
        int ny = 24 + GADGET_W + 16, nh = 96;
        soft_shadow_on_row(y, gx, ny, GADGET_W, nh);
        if ((int)y >= ny && (int)y < ny + nh) {
            round_fill_on_row(y, gx, ny, GADGET_W, nh, rgb(250, 238, 130));
            if ((int)y == ny + 1)
                round_fill_on_row(y, gx, ny, GADGET_W, nh, rgb(216, 200, 90));
            if (note_focus && (int)y >= ny + 6 && (int)y < ny + nh - 6)
                fill_rect_on_row(y, gx, ny, 2, nh, rgb(214, 120, 40));
            /* wrap note text to ~12 chars/row, 4 rows */
            {
                int li = ((int)y - ny - 6) / 20;
                if (li >= 0 && li < 4) {
                    char seg[16];
                    int per = 13;
                    int off = li * per;
                    int k = 0;
                    while (k < per && off + k < note_len) {
                        seg[k] = note_text[off + k];
                        k++;
                    }
                    seg[k] = 0;
                    if (k)
                        aa_text_on_row(y, gx + 6, ny + 6 + li * 20, seg,
                                       rgb(70, 60, 20));
                    else if (li == 0 && !note_len)
                        aa_text_on_row(y, gx + 6, ny + 6, "Click to note",
                                       rgb(160, 146, 80));
                }
            }
        }
    }

    /* ── mini calendar card ── */
    {
        int my = 24 + GADGET_W + 16 + 96 + 16, mh = 64;
        soft_shadow_on_row(y, gx, my, GADGET_W, mh);
        if ((int)y >= my && (int)y < my + mh) {
            round_glass_on_row(y, gx, my, GADGET_W, mh, rgb(20, 26, 36), 150);
            {
                struct timeval tv;
                char buf[16];
                if (gettimeofday(&tv, 0) == 0) {
                    int yy;
                    unsigned mm, dd;
                    civil_from_days(tv.tv_sec / 86400, &yy, &mm, &dd);
                    sprintf(buf, "%u", dd);
                    aa_text_on_row(y, gx + (GADGET_W -
                                            aa_text_width(buf)) / 2,
                                   my + 8, buf, 0xFFFFFF);
                    sprintf(buf, "%s %d", month_names[mm - 1], yy);
                    aa_text_on_row(y, gx + (GADGET_W -
                                            aa_text_width(buf)) / 2,
                                   my + 38, buf, rgb(200, 208, 220));
                }
            }
        }
    }
}

/* note gadget hit box (for clicks) */
static int in_note(int x, int y) {
    int ny = 24 + GADGET_W + 16;
    return gadgets_visible && x >= GADGET_X && x < GADGET_X + GADGET_W &&
           y >= ny && y < ny + 96;
}

/* Blend the Aero orb onto this scanline (overhangs the taskbar top). */
static void draw_orb_on_row(unsigned y) {
    int oy = (int)fb_h - TASKBAR_H - 4;          /* orb top (overhang 4px) */
    int state = launcher_open ? 2 :
                in_rect((unsigned)mouse_x, (unsigned)mouse_y, ORB_X,
                        (unsigned)oy, ORB_SIZE, ORB_SIZE) ? 1 : 0;
    int gy = (int)y - oy;

    if (gy < 0 || gy >= ORB_SIZE) return;

    /* Preferred: a crisp themed Start icon from the Reversal-blue pack. */
    themed_icon_t *st = load_themed_icon("start");
    if (st && st->px) {
        /* Soft round highlight behind the icon on hover / when open. */
        if (state) {
            int C = ORB_SIZE / 2, R = ORB_SIZE / 2 - 2;
            int dyc = gy - C, span2 = R * R - dyc * dyc;
            if (span2 > 0) {
                int half = isqrt(span2);
                unsigned a = state == 2 ? 70 : 42;
                blend_rect_on_row(y, ORB_X + C - half, (int)y, 2 * half, 1,
                                  0xFFFFFF, a);
            }
        }
        int sz = ORB_SIZE - 6;                   /* 38px icon in the 44px slot */
        themed_icon_on_row(y, ORB_X + (ORB_SIZE - sz) / 2,
                           oy + (ORB_SIZE - sz) / 2, sz, sz, st);
        return;
    }

    /* Fallback: the procedural Aero orb (kept so the bar always has a button). */
    for (int gx = 0; gx < ORB_SIZE; gx++) {
        uint32_t c = orb_px[state][gy * ORB_SIZE + gx];
        int px = ORB_X + gx;
        if (!c || px >= (int)fb_w || px >= MAX_W) continue;
        row[px] = c & 0xFFFFFF;
    }
}

static void draw_taskbar_on_row(unsigned y, const char *clock_text) {
    int bar_y = (int)fb_h - TASKBAR_H;
    int btn_y = bar_y + 3;
    int btn_h = TASKBAR_H - 6;
    int ids[MAX_WINDOWS], xs[MAX_WINDOWS];
    int n;

    if ((int)y >= bar_y) {
        /* Black glass: blurred wallpaper, heavily darkened, light top edge. */
        glass_rect_on_row(y, 0, bar_y, (int)fb_w, TASKBAR_H, rgb(8, 11, 17),
                          205);
        if ((int)y == bar_y)
            blend_rect_on_row(y, 0, bar_y, (int)fb_w, 1, 0xFFFFFF, 60);

        /* Win7 icon buttons. */
        n = taskbar_buttons(ids, xs, MAX_WINDOWS);
        for (int i = 0; i < n; i++) {
            desktop_window_t *win = find_window(ids[i]);
            if (!win) continue;
            int active = ids[i] == active_window && !win->minimized;
            int hov = in_rect((unsigned)mouse_x, (unsigned)mouse_y,
                              (unsigned)xs[i], (unsigned)btn_y,
                              TB_BTN_W, (unsigned)btn_h);
            if (active)
                blend_rect_on_row(y, xs[i], btn_y, TB_BTN_W, btn_h,
                                  0xFFFFFF, 56);
            else if (hov)
                blend_rect_on_row(y, xs[i], btn_y, TB_BTN_W, btn_h,
                                  0xFFFFFF, 30);
            /* Themed 28px icon (centered); else legacy 16px art scaled 2x. */
            {
                const char *iname = icon_name_for_label(win->title);
                themed_icon_t *tic = iname ? load_themed_icon(iname) : NULL;
                if (tic && tic->px) {
                    int sz = 28;
                    themed_icon_on_row(y, xs[i] + (TB_BTN_W - sz) / 2,
                                       btn_y + (btn_h - sz) / 2, sz, sz, tic);
                } else {
                    const char *const *art = taskbar_art_for_title(win->title);
                    int ax = xs[i] + (TB_BTN_W - 32) / 2;
                    int ay = btn_y + (btn_h - 32) / 2;
                    int gy = ((int)y - ay) / 2;
                    if (art && (int)y >= ay && gy >= 0 && gy < 16) {
                        const char *r = art[gy];
                        for (int gx = 0; gx < 16 && r[gx]; gx++) {
                            int v = (r[gx] >= '0' && r[gx] <= '9') ? r[gx]-'0' :
                                    (r[gx] >= 'a' && r[gx] <= 'f')
                                        ? r[gx] - 'a' + 10 : 0;
                            if (v)
                                fill_rect_on_row(y, ax + gx * 2, btn_y, 2,
                                                 btn_h, icon_palette((uint8_t)v));
                        }
                    }
                }
            }
            /* running indicator */
            if (!win->minimized)
                fill_rect_on_row(y, xs[i] + 6, bar_y + TASKBAR_H - 3,
                                 TB_BTN_W - 12, 2, col_accent);
        }

        /* Show Desktop sliver (Win7 far-right). */
        blend_rect_on_row(y, (int)fb_w - 8, bar_y + 2, 6, TASKBAR_H - 4,
                          0xFFFFFF, 28);
        fill_rect_on_row(y, (int)fb_w - 9, bar_y + 2, 1, TASKBAR_H - 4,
                         rgb(60, 68, 80));

        /* Clock + date, stacked right (Win7 style). */
        {
            char datebuf[20];
            struct timeval tv;
            if (gettimeofday(&tv, 0) == 0) {
                int yy;
                unsigned mm, dd;
                civil_from_days(tv.tv_sec / 86400, &yy, &mm, &dd);
                sprintf(datebuf, "%02u.%02u.%d", dd, mm, yy);
            } else {
                strcpy(datebuf, "--.--.----");
            }
            aa_text_on_row(y, (int)fb_w - aa_text_width(clock_text) - 14,
                           bar_y + 2, clock_text, COL_TEXT);
            aa_text_on_row(y, (int)fb_w - aa_text_width(datebuf) - 14,
                           bar_y + 20, datebuf, COL_TEXT_DIM);
        }
    }

    draw_orb_on_row(y);
}

static void draw_launcher_menu_on_row(unsigned y) {
    int mx = sm_x(), my = sm_y();
    int filt[SM_ITEMS + MAX_INST_APPS];
    int n;

    if (!launcher_open) return;
    if ((int)y < my - SHADOW_R || (int)y >= my + SM_H + SHADOW_R) return;
    n = sm_filtered(filt, SM_ITEMS + MAX_INST_APPS);

    soft_shadow_on_row(y, mx, my, SM_W, SM_H);
    /* outer glass frame */
    glass_rect_on_row(y, mx, my, SM_W, SM_H, col_accent, 95);
    /* left pane: near-white sheet */
    blend_rect_on_row(y, mx + 6, my + 6, SM_LEFT_W - 10, SM_H - 12,
                      rgb(250, 251, 252), 235);
    /* program rows */
    for (int i = 0; i < n; i++) {
        int iy = my + 12 + i * SM_ROW_H;
        sm_item_t pkg_it;
        const sm_item_t *it;
        if (filt[i] >= 1000) {
            pkg_it.label = inst_apps[filt[i] - 1000].name;
            pkg_it.action = -1;
            it = &pkg_it;
        } else {
            it = &sm_items[filt[i]];
        }
        int hov = in_rect((unsigned)mouse_x, (unsigned)mouse_y,
                          (unsigned)(mx + 8), (unsigned)iy,
                          SM_LEFT_W - 14, SM_ROW_H);
        if (hov)
            blend_rect_on_row(y, mx + 8, iy, SM_LEFT_W - 14, SM_ROW_H,
                              col_accent, 90);
        /* Themed 24px icon (else 16px hex-art). */
        {
            const char *iname = icon_name_for_label(it->label);
            themed_icon_t *tic = iname ? load_themed_icon(iname) : NULL;
            if (tic && tic->px) {
                int sz = 24;
                themed_icon_on_row(y, mx + 12, iy + (SM_ROW_H - sz) / 2,
                                   sz, sz, tic);
            } else {
                const char *const *art = taskbar_art_for_title(it->label);
                int gy = (int)y - (iy + (SM_ROW_H - 16) / 2);
                if (art && gy >= 0 && gy < 16) {
                    const char *r = art[gy];
                    for (int gx = 0; gx < 16 && r[gx]; gx++) {
                        int v = (r[gx] >= '0' && r[gx] <= '9') ? r[gx] - '0' :
                                (r[gx] >= 'a' && r[gx] <= 'f') ? r[gx]-'a'+10
                                                               : 0;
                        if (v)
                            fill_rect_on_row(y, mx + 16 + gx, iy, 1, SM_ROW_H,
                                             icon_palette((uint8_t)v));
                    }
                }
            }
        }
        aa_text_on_row(y, mx + 42, iy + (SM_ROW_H - FONT_UI16_LINE_H) / 2,
                       it->label, hov ? rgb(255, 255, 255) : rgb(28, 32, 36));
    }
    if (!n)
        aa_text_on_row(y, mx + 20, my + 16, "No matches",
                       rgb(120, 126, 130));

    /* search box at the bottom of the left pane */
    {
        int sy = my + SM_H - SM_SEARCH_H - 10;
        fill_rect_on_row(y, mx + 10, sy, SM_LEFT_W - 18, SM_SEARCH_H,
                         rgb(255, 255, 255));
        blend_rect_on_row(y, mx + 10, sy, SM_LEFT_W - 18, 1, 0, 70);
        if (sm_search_len) {
            int pen = aa_text_on_row(y, mx + 16, sy + 5, sm_search,
                                     rgb(28, 32, 36));
            fill_rect_on_row(y, mx + 16 + pen, sy + 5, 2,
                             FONT_UI16_LINE_H - 2, col_accent);
        } else {
            aa_text_on_row(y, mx + 16, sy + 5, "Search programs...",
                           rgb(150, 156, 162));
        }
    }

    /* right pane: user tile + places + power */
    {
        int rx = mx + SM_LEFT_W;
        /* user tile: small orb + name */
        {
            int oy = my + 14;
            int gy = (int)y - oy;
            if (gy >= 0 && gy < ORB_SIZE) {
                for (int gx = 0; gx < ORB_SIZE; gx++) {
                    uint32_t c = orb_px[0][gy * ORB_SIZE + gx];
                    int px = rx + (SM_RIGHT_W - ORB_SIZE) / 2 + gx;
                    if (c && px < (int)fb_w && px < MAX_W)
                        row[px] = c & 0xFFFFFF;
                }
            }
            aa_text_on_row(y, rx + (SM_RIGHT_W -
                                    aa_text_width("MaeroOS")) / 2,
                           oy + ORB_SIZE + 4, "MaeroOS", 0xFFFFFF);
        }
        for (int i = 0; i < SM_RIGHT_ITEMS; i++) {
            int iy = my + 100 + i * 32;
            int hov = in_rect((unsigned)mouse_x, (unsigned)mouse_y,
                              (unsigned)(rx + 6), (unsigned)iy,
                              SM_RIGHT_W - 12, 32);
            if (hov)
                blend_rect_on_row(y, rx + 6, iy, SM_RIGHT_W - 12, 32,
                                  0xFFFFFF, 50);
            aa_text_on_row(y, rx + 16, iy + 6, sm_right_labels[i],
                           rgb(235, 240, 248));
        }
        /* power button */
        {
            int py = my + SM_H - 42;
            int hov = in_rect((unsigned)mouse_x, (unsigned)mouse_y,
                              (unsigned)(rx + 18), (unsigned)py,
                              SM_RIGHT_W - 36, 28);
            uint32_t base = hov ? rgb(225, 90, 74) : rgb(180, 50, 40);
            fill_rect_on_row(y, rx + 18, py, SM_RIGHT_W - 36, 28, base);
            aa_text_on_row(y, rx + 18 + (SM_RIGHT_W - 36 -
                                         aa_text_width("Power off")) / 2,
                           py + 4, "Power off", 0xFFFFFF);
        }
    }
}

/* Load a binary PPM (P6) wallpaper into a malloc'd pixel buffer. */
static char conf_wallpaper[160];

/* /disk/etc/desktop.conf (persistent) or /etc/desktop.conf:
 *   wallpaper=/disk/foo.ppm
 *   accent=#RRGGBB
 */
static void load_desktop_conf(void) {
    const char *cp = access("/disk/etc/desktop.conf", 0) == 0 ?
                     "/disk/etc/desktop.conf" : "/etc/desktop.conf";
    int fd = open(cp, O_RDONLY);
    char buf[512];
    int n;

    if (fd < 0) return;
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return;
    buf[n] = 0;
    for (char *line = buf; line && *line; ) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        if (!strncmp(line, "wallpaper=", 10)) {
            strncpy(conf_wallpaper, line + 10, sizeof(conf_wallpaper) - 1);
            conf_wallpaper[sizeof(conf_wallpaper) - 1] = 0;
        } else if (!strncmp(line, "accent=#", 8)) {
            uint32_t v = 0;
            for (char *h = line + 8; *h; h++) {
                int d = (*h >= '0' && *h <= '9') ? *h - '0' :
                        (*h >= 'a' && *h <= 'f') ? *h - 'a' + 10 :
                        (*h >= 'A' && *h <= 'F') ? *h - 'A' + 10 : -1;
                if (d < 0) break;
                v = (v << 4) | (uint32_t)d;
            }
            if (v) col_accent = v & 0xFFFFFF;
        }
        line = nl ? nl + 1 : 0;
    }
}

static void load_wallpaper(void) {
    const char *path = (conf_wallpaper[0] && access(conf_wallpaper, 0) == 0)
                       ? conf_wallpaper
                       : access("/disk/wallpaper.ppm", 0) == 0 ?
                         "/disk/wallpaper.ppm" : "/wallpaper.ppm";
    int fd = open(path, O_RDONLY);
    char hdr[64];
    int n, w = 0, h = 0, maxv = 0, pos = 0, field = 0;

    if (fd < 0) return;
    n = read(fd, hdr, sizeof(hdr));
    if (n < 10 || hdr[0] != 'P' || hdr[1] != '6') {
        close(fd);
        return;
    }
    /* Parse "P6 <w> <h> <max>" (whitespace-separated, no comments). */
    pos = 2;
    while (pos < n && field < 3) {
        while (pos < n && (hdr[pos] == ' ' || hdr[pos] == '\n' ||
                           hdr[pos] == '\t' || hdr[pos] == '\r'))
            pos++;
        int v = 0;
        while (pos < n && hdr[pos] >= '0' && hdr[pos] <= '9')
            v = v * 10 + (hdr[pos++] - '0');
        if (field == 0) w = v;
        else if (field == 1) h = v;
        else maxv = v;
        field++;
    }
    pos++;   /* single whitespace after maxval, then binary data */
    if (w < 1 || h < 1 || w > 2048 || h > 2048 || maxv != 255) {
        close(fd);
        return;
    }

    wallpaper = (uint32_t *)malloc((size_t)w * h * 4);
    if (!wallpaper) {
        close(fd);
        return;
    }
    {
        /* Stream RGB triplets, converting to 0xRRGGBB. */
        static uint8_t chunk[3 * 1024];
        int total = w * h, idx = 0, have = n - pos, off = 0;
        memcpy(chunk, hdr + pos, (size_t)have);
        while (idx < total) {
            while (have - off >= 3) {
                wallpaper[idx++] = rgb(chunk[off], chunk[off + 1], chunk[off + 2]);
                off += 3;
                if (idx == total) break;
            }
            if (idx == total) break;
            memcpy(chunk, chunk + off, (size_t)(have - off));
            have -= off;
            off = 0;
            n = read(fd, (char *)chunk + have, (int)sizeof(chunk) - have);
            if (n <= 0) break;
            have += n;
        }
    }
    close(fd);
    wallpaper_w = w;
    wallpaper_h = h;

    /* Scale-to-fill (cover) to the framebuffer so any-resolution wallpapers
     * fill the screen with correct aspect (crop overflow), not tile/clip. */
    if ((unsigned)w != fb_w || (unsigned)h != fb_h) {
        uint32_t *dst = (uint32_t *)malloc((size_t)fb_w * fb_h * 4);
        if (dst) {
            /* cover scale: source step = min over axes so image covers fb */
            long sx_num = (long)w, sy_num = (long)h;
            /* pick scale = max(fb_w/w, fb_h/h); express src coord per dst */
            long scale_w = (long)w * 1000 / (long)fb_w;
            long scale_h = (long)h * 1000 / (long)fb_h;
            long scale = scale_w < scale_h ? scale_w : scale_h;  /* cover */
            if (scale < 1) scale = 1;
            long crop_w = (long)fb_w * scale / 1000;
            long crop_h = (long)fb_h * scale / 1000;
            long ox = (sx_num - crop_w) / 2;
            long oy = (sy_num - crop_h) / 2;
            for (unsigned dy = 0; dy < fb_h; dy++) {
                long syc = oy + (long)dy * scale / 1000;
                if (syc < 0) syc = 0; if (syc >= h) syc = h - 1;
                uint32_t *srow = wallpaper + (size_t)syc * w;
                uint32_t *drow = dst + (size_t)dy * fb_w;
                for (unsigned dx = 0; dx < fb_w; dx++) {
                    long sxc = ox + (long)dx * scale / 1000;
                    if (sxc < 0) sxc = 0; if (sxc >= w) sxc = w - 1;
                    drow[dx] = srow[sxc];
                }
            }
            free(wallpaper);
            wallpaper = dst;
            wallpaper_w = (int)fb_w;
            wallpaper_h = (int)fb_h;
        }
    }
}

/* Blit the completed back buffer to the framebuffer in one pass (no tearing). */
static int present(void);

/* Paint the whole framebuffer black.  Used before handing the screen to a
 * fullscreen app (DOOM) so any area it doesn't cover — letterbox borders from
 * integer scaling — shows clean black instead of the leftover desktop. */
static void blank_framebuffer(void) {
    if (!backbuf) return;
    for (size_t i = 0; i < (size_t)fb_w * fb_h; i++)
        backbuf[i] = 0;
    present_invalidate();     /* a fullscreen app draws next; forget the shadow */
    present();
}

/* Copy `rows` scanlines starting at `y0` from the back buffer to the screen. */
static int present_rows(unsigned y0, unsigned rows) {
    if (fb_pitch == fb_w * 4) {          /* rows are contiguous: one write */
        lseek(fb_fd, (int)(y0 * fb_pitch), 0);
        return write(fb_fd, backbuf + (size_t)y0 * fb_w,
                     (int)(rows * fb_w * 4)) < 0 ? -1 : 0;
    }
    for (unsigned y = y0; y < y0 + rows; y++) {
        lseek(fb_fd, (int)(y * fb_pitch), 0);
        if (write(fb_fd, backbuf + (size_t)y * fb_w, (int)(fb_w * 4)) < 0)
            return -1;
    }
    return 0;
}

/*
 * Present the composited frame.
 *
 * The screen is device memory, so a full-screen blit runs at MMIO speed rather
 * than RAM speed: 4 MiB at 1280x800 costs ~10 ms, and the compositor recomposes
 * and presents every frame even when only a few scanlines changed.  Measured
 * over a Firefox startup that was ~80 s of the ~185 s to first paint - the
 * single largest cost in the system.
 *
 * So keep a shadow of the last frame actually presented, in ordinary cached
 * memory, and write only the runs of scanlines that differ.  Comparing 4 MiB of
 * cached RAM is roughly an order of magnitude cheaper than writing it to the
 * framebuffer, and during a browser startup almost every row is identical from
 * frame to frame.  Contiguous dirty rows are coalesced into one write so the
 * syscall count stays low.
 *
 * The shadow is only valid while the desktop is the only thing drawing.  A
 * fullscreen app owns the screen directly, so blank_framebuffer() (the handoff)
 * and the app's exit both invalidate it and the next present is a full blit.
 * If the shadow cannot be allocated, every present is a full blit - the
 * behaviour this replaced.
 */
static int present(void) {
    if (!backbuf) return 0;
    const unsigned rowpx = fb_w;

    if (!shadow || !shadow_valid) {
        if (present_rows(0, fb_h) < 0) return -1;
        if (shadow) {
            memcpy(shadow, backbuf, (size_t)fb_w * fb_h * 4);
            shadow_valid = 1;
        }
        return 0;
    }

    for (unsigned y = 0; y < fb_h; ) {
        if (memcmp(backbuf + (size_t)y * rowpx, shadow + (size_t)y * rowpx,
                   rowpx * 4) == 0) {
            y++;
            continue;
        }
        unsigned start = y;
        while (y < fb_h &&
               memcmp(backbuf + (size_t)y * rowpx, shadow + (size_t)y * rowpx,
                      rowpx * 4) != 0) {
            memcpy(shadow + (size_t)y * rowpx, backbuf + (size_t)y * rowpx,
                   rowpx * 4);
            y++;
        }
        if (present_rows(start, y - start) < 0) return -1;
    }
    return 0;
}

static int render(void) {
    unsigned w = fb_w < MAX_W ? fb_w : MAX_W;
    char status[64];
    char clock_text[16];

    /* Safety net: keep every window's titlebar on-screen.  Catches any path
     * (tear-off, restore, a resolution change) that could leave a window
     * stranded above the top edge.  Skip the one being actively dragged. */
    for (int i = 0; i < window_count; i++) {
        if (drag_mode && windows[i].id == drag_win_id) continue;
        if (windows[i].visible && !windows[i].minimized)
            clamp_window(&windows[i]);
    }

    if (shell_pid >= 0)
        sprintf(status, "FB %ux%u  CAPS %s  SHELL PID %d",
                fb_w, fb_h, caps_on ? "ON" : "OFF", shell_pid);
    else
        sprintf(status, "FB %ux%u  CAPS %s  NO SHELL",
                fb_w, fb_h, caps_on ? "ON" : "OFF");

    {
        /* Wall-clock time of day (gettimeofday is RTC-anchored now). */
        int s = clock_secs < 0 ? 0 : clock_secs % 86400;
        sprintf(clock_text, "%02d:%02d", s / 3600, (s / 60) % 60);
    }

    update_cursor_shape();
    update_thumbnail();

    for (unsigned y = 0; y < fb_h; y++) {
        /* Aim drawing at this scanline of the back buffer (or fallback row). */
        row = backbuf ? backbuf + (size_t)y * fb_w : fallback_row;

        /* Wallpaper: image if one loaded, else vertical gradient. */
        if (wallpaper && (int)y < wallpaper_h) {
            uint32_t *src = wallpaper + (size_t)y * wallpaper_w;
            unsigned wn = w < (unsigned)wallpaper_w ? w : (unsigned)wallpaper_w;
            for (unsigned x = 0; x < wn; x++)
                row[x] = src[x];
            for (unsigned x = wn; x < w; x++)
                row[x] = COL_WALL_BOT;
        } else {
            uint32_t wall = mix_color(COL_WALL_TOP, COL_WALL_BOT,
                                      (unsigned)(y * 255 / (fb_h ? fb_h : 1)));
            for (unsigned x = 0; x < w; x++)
                row[x] = wall;
        }

        draw_desk_icons_on_row(y);
        draw_gadgets_on_row(y);

        for (int i = 0; i < window_count; i++) {
            draw_window_on_row(y, &windows[i]);
            draw_window_content_on_row(y, &windows[i], status);
        }

        draw_win_anim_on_row(y);
        draw_taskbar_on_row(y, clock_text);
        draw_thumbnail_on_row(y);
        draw_ctx_menu_on_row(y);
        draw_calendar_on_row(y);
        draw_launcher_menu_on_row(y);

        if (!running) {
            draw_text_on_row(y, fb_w / 2 - 120, fb_h / 2 - 16,
                             "POWERING OFF...", COL_TEXT, 2);
        }
        if (mouse_fd >= 0) draw_cursor_on_row(y, w);

        /* Direct-write fallback flushes per scanline; the back buffer is
         * presented once after the whole frame is composited. */
        if (!backbuf) {
            lseek(fb_fd, (int)(y * fb_pitch), 0);
            if (write(fb_fd, row, (int)(w * 4)) < 0) return -1;
        }
    }
    tick_win_anim();
    return backbuf ? present() : 0;
}

int main(void) {
    fb_fd = open("/dev/fb0", O_RDWR);
    if (fb_fd < 0) {
        printf("desktop: /dev/fb0 unavailable\n");
        return 1;
    }
    int ev_fd = open("/dev/input/event0", O_RDONLY);
    if (ev_fd < 0) {
        printf("desktop: /dev/input/event0 unavailable\n");
        close(fb_fd);
        return 1;
    }

    struct fb_var_screeninfo var;
    int vr;
    struct fb_fix_screeninfo fix;
    vr = ioctl(fb_fd, FBIOGET_VSCREENINFO, &var);
    if (vr < 0 ||
        ioctl(fb_fd, FBIOGET_FSCREENINFO, &fix) < 0 ||
        var.bits_per_pixel != 32 || var.xres == 0 || var.yres == 0) {
        printf("desktop: unsupported framebuffer (r=%d xres=%u yres=%u bpp=%u)\n",
               vr, var.xres, var.yres, var.bits_per_pixel);
        close(ev_fd);
        close(fb_fd);
        return 1;
    }
    fb_w = var.xres;
    fb_h = var.yres;
    fb_pitch = fix.line_length;
    mouse_x = (int)fb_w / 2;
    mouse_y = (int)fb_h / 2;
    if (fb_w > MAX_W) {
        printf("desktop: width too large\n");
        close(ev_fd);
        close(fb_fd);
        return 1;
    }
    init_windows();

    /* Allocate the full-screen back buffer; fall back to direct writes if the
     * heap can't satisfy it so the desktop still boots on tight memory. */
    backbuf = (uint32_t *)malloc((size_t)fb_w * fb_h * 4);
    shadow  = backbuf ? (uint32_t *)malloc((size_t)fb_w * fb_h * 4) : 0;
    shadow_valid = 0;
    load_desktop_conf();
    load_wallpaper();
    make_wallpaper_blur();
    make_orb();
    note_load();
    scan_installed_apps();

    add_log("GRAPHICS ONLINE");
    add_log(backbuf ? "COMPOSITOR DOUBLE BUFFERED" : "COMPOSITOR DIRECT MODE");
    add_log("KEYBOARD ONLINE");
    mouse_fd = open("/dev/input/event1", O_RDONLY);
    if (mouse_fd >= 0) add_log("MOUSE ONLINE");
    else add_log("NO MOUSE DEVICE");
    setup_wmctl();
    setup_wmevents();
    start_shell();
    if (render() < 0) {
        printf("desktop: framebuffer write failed\n");
        close_wm_channels();
        if (mouse_fd >= 0) close(mouse_fd);
        close(ev_fd);
        close(fb_fd);
        return 1;
    }

    /* Diagnostic one-shot: if /disk/ffauto exists, auto-launch the Firefox
     * launcher once the wm is up, so the X-protocol trace can be captured
     * without GUI interaction.  Remove the marker file to disable. */
    static int ff_auto = -1, ff_tick = 0;
    if (ff_auto < 0) {
        if      (access("/disk/gtkauto", 0) == 0) ff_auto = 2;  /* run gtkprobe */
        else if (access("/disk/ffauto",  0) == 0) ff_auto = 1;  /* run Firefox  */
        else                                       ff_auto = 0;
    }

    while (running) {
        struct input_event ev;
        int n;
        int dirty = 0;

        if (ff_auto > 0 && ++ff_tick == 120) {   /* ~a few seconds in, once */
            int which = ff_auto; ff_auto = 0;
            int fpid = fork();
            if (fpid == 0) {
                if (which == 2) {
                    char *a[] = { "/disk/gtkprobe", (char *)0 };
                    execve("/disk/gtkprobe", a, desktop_envp);
                } else {
                    char *a[] = { "/disk/ff", (char *)0 };
                    execve("/disk/ff", a, desktop_envp);
                }
                exit(127);
            }
        }

        struct pollfd pfds[4];
        unsigned long nfds = 1;
        unsigned long mouse_idx = 0;
        unsigned long wm_idx = 0;
        unsigned long shell_idx = 0;
        pfds[0].fd = ev_fd;
        pfds[0].events = POLLIN;
        pfds[0].revents = 0;
        if (mouse_fd >= 0 && fullscreen_pid <= 0) {
            mouse_idx = nfds;
            pfds[nfds].fd = mouse_fd;
            pfds[nfds].events = POLLIN;
            pfds[nfds].revents = 0;
            nfds++;
        }
        if (wm_fd >= 0) {
            wm_idx = nfds;
            pfds[nfds].fd = wm_fd;
            pfds[nfds].events = POLLIN;
            pfds[nfds].revents = 0;
            nfds++;
        }
        if (shell_fd >= 0) {
            shell_idx = nfds;
            pfds[nfds].fd = shell_fd;
            pfds[nfds].events = POLLIN;
            pfds[nfds].revents = 0;
            nfds++;
        }

        if (poll(pfds, nfds, win_anim.active ? 16 :
                 (shell_pid >= 0 || any_client_running()) ? 50 : 1000) < 0) {
            printf("desktop: poll failed\n");
            close_wm_channels();
            if (mouse_fd >= 0) close(mouse_fd);
            close(ev_fd);
            close(fb_fd);
            return 1;
        }

        if (fullscreen_pid <= 0 && (pfds[0].revents & POLLIN)) {
            while ((n = read(ev_fd, &ev, sizeof(ev))) == (int)sizeof(ev)) {
                if (ev.type == EV_KEY) {
                    handle_key(ev.code, ev.value);
                    dirty = 1;
                }
            }
        }
        /* While a fullscreen app runs it owns /dev/input/event1 (links'
         * gpm reads it directly) — don't steal its mouse events. */
        if (fullscreen_pid <= 0 && mouse_fd >= 0 &&
            (pfds[mouse_idx].revents & POLLIN)) {
            while ((n = read(mouse_fd, &ev, sizeof(ev))) == (int)sizeof(ev)) {
                if (ev.type == EV_REL || ev.type == EV_KEY) {
                    handle_mouse(&ev);
                    dirty = 1;
                }
            }
        }
        if (wm_fd >= 0 && (pfds[wm_idx].revents & POLLIN)) {
            handle_wmctl_input();
            dirty = 1;
        }
        if (shell_fd >= 0) {
            if (pfds[shell_idx].revents) {
                handle_shell_output(pfds[shell_idx].revents);
                dirty = 1;
            }
        }
        if (shell_pid >= 0 && shell_fd < 0) {
            int before = shell_pid;
            poll_shell_exit();
            if (shell_pid != before) dirty = 1;
        }
        if (any_client_running() && poll_app_exits() > 0)
            dirty = 1;
        /* Taskbar clock: redraw when the uptime second changes. */
        {
            struct timeval tv;
            if (gettimeofday(&tv, 0) == 0 && (int)tv.tv_sec != clock_secs) {
                clock_secs = (int)tv.tv_sec;
                dirty = 1;
            }
        }
        /* Fullscreen app owns the screen: forward keys, skip rendering. */
        if (fullscreen_pid > 0) {
            int st = 0;
            /* Raw-input app (DOOM) reads event0 itself — stay off it entirely
             * so every keystroke reaches the app. */
            if (fullscreen_rawinput && (pfds[0].revents & POLLIN)) {
                /* don't read event0; let the app consume it */
            } else if (pfds[0].revents & POLLIN) {
                struct input_event fev;
                while (read(ev_fd, &fev, sizeof(fev)) == (int)sizeof(fev)) {
                    char seq[4];
                    int sl = 0;
                    if (fev.type != EV_KEY) continue;
                    if (fev.code == KEY_LEFTSHIFT ||
                        fev.code == KEY_RIGHTSHIFT) {
                        shift_down = fev.value != 0;
                        continue;
                    }
                    if (fev.code == KEY_LEFTCTRL ||
                        fev.code == KEY_RIGHTCTRL) {
                        ctrl_down = fev.value != 0;
                        continue;
                    }
                    if (fev.code == KEY_LEFTALT) {
                        alt_down = fev.value != 0;
                        continue;
                    }
                    if (fev.value != 1) continue;
                    /* Ctrl+Alt+Q: force-quit a stuck fullscreen app */
                    if (ctrl_down && alt_down && fev.code == KEY_Q) {
                        kill((int)fullscreen_pid, SIGKILL);
                        add_log("FULLSCREEN APP KILLED (CTRL+ALT+Q)");
                        continue;
                    }
                    if (fullscreen_kbd_fd < 0) continue;
                    if (ctrl_down) {
                        char ch = key_to_char(fev.code);
                        if (ch >= 'a' && ch <= 'z') {
                            char cc = (char)(ch - 'a' + 1);
                            write(fullscreen_kbd_fd, &cc, 1);
                        }
                        continue;
                    }
                    switch (fev.code) {
                    case KEY_ENTER:     seq[sl++] = '\r'; break;
                    case KEY_BACKSPACE: seq[sl++] = 127; break;
                    case KEY_TAB:       seq[sl++] = '\t'; break;
                    case KEY_ESC:       seq[sl++] = 27; break;
                    case KEY_UP:    seq[0]=27; seq[1]='['; seq[2]='A'; sl=3; break;
                    case KEY_DOWN:  seq[0]=27; seq[1]='['; seq[2]='B'; sl=3; break;
                    case KEY_RIGHT: seq[0]=27; seq[1]='['; seq[2]='C'; sl=3; break;
                    case KEY_LEFT:  seq[0]=27; seq[1]='['; seq[2]='D'; sl=3; break;
                    default: {
                        char ch = key_to_char(fev.code);
                        if (ch) seq[sl++] = ch;
                        break;
                    }
                    }
                    if (sl)
                        write(fullscreen_kbd_fd, seq, sl);
                }
            }
            if (waitpid((int)fullscreen_pid, &st, 1 /* WNOHANG */) ==
                (int)fullscreen_pid) {
                fullscreen_pid = -1;
                fullscreen_rawinput = 0;
                syscall1(505, -1);   /* clear the kill-hotkey target */
                if (fullscreen_kbd_fd >= 0) {
                    close(fullscreen_kbd_fd);
                    fullscreen_kbd_fd = -1;
                }
                add_log("FULLSCREEN APP EXITED");
                present_invalidate();   /* the app drew straight to the screen */
                dirty = 1;
            } else {
                /* Raw-input app owns event0; we don't drain it, so poll()
                 * would return instantly — sleep a beat to avoid busy-spin. */
                if (fullscreen_rawinput) {
                    struct timespec ts = {0, 20000000};  /* 20 ms */
                    nanosleep(&ts, 0);
                }
                continue;
            }
        }
        if (win_anim.active) dirty = 1;   /* keep animation frames flowing */
        if (dirty && render() < 0) {
            printf("desktop: framebuffer write failed\n");
            close_wm_channels();
            if (mouse_fd >= 0) close(mouse_fd);
            close(ev_fd);
            close(fb_fd);
            return 1;
        }
    }
    render();
    struct timespec ts = {0, 250000000};
    nanosleep(&ts, 0);
    if (shell_pid >= 0) {
        stop_shell();
        int status = 0;
        waitpid(shell_pid, &status, 0);
    }
    stop_apps();
    close_wm_channels();
    if (mouse_fd >= 0) close(mouse_fd);
    close(ev_fd);
    close(fb_fd);
    printf("desktop: exited\n");
    /* The display stays in framebuffer mode after we exit (no text-mode
     * switch exists), so "returning to shell" would just freeze the screen.
     * QUIT means leave the machine: power off cleanly. */
    syscall3(88, 0, 0, (int)0x4321FEDCu);   /* reboot(LINUX_REBOOT_CMD_POWER_OFF) */
    return 0;
}
