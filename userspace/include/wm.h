#pragma once

#define WM_MAX_TEXT_LINES 24
#define WM_MAX_SLOTS 12

typedef struct {
    int fd;
} wm_client_t;

typedef struct {
    int fd;
    char line[80];
    int used;
} wm_event_client_t;

enum {
    WM_EVENT_NONE = 0,
    WM_EVENT_MOUSE = 1,
    WM_EVENT_KEY = 2,
    WM_EVENT_FOCUS = 3,
    WM_EVENT_GEOM = 4,
    WM_EVENT_CLOSE = 5,
    WM_EVENT_SCROLL = 6,   /* mouse wheel at (x,y); value = notches (+ = up) */
    WM_EVENT_RAWKEY = 7,   /* uncooked key: code + press/release + modifier mask */
};

/*
 * Modifier mask carried by WM_EVENT_RAWKEY.  The values are deliberately the
 * X11 ones (ShiftMask, LockMask, ControlMask, Mod1Mask, Mod2Mask, Mod4Mask) so
 * maeroX can put the mask straight into a KeyPress event's `state` field
 * without a second table.
 *
 * Every bit here must be one the window manager actually tracks AND one
 * maeroX's GetModifierMapping names a keycode for.  A mask bit that is
 * advertised but never set is worse than an absent one: the toolkit believes
 * the combination exists and the user's key does nothing.
 */
enum {
    WM_MOD_SHIFT = 0x01,
    WM_MOD_LOCK  = 0x02,   /* Caps Lock */
    WM_MOD_CTRL  = 0x04,
    WM_MOD_ALT   = 0x08,   /* Mod1: either Alt */
    WM_MOD_NUM   = 0x10,   /* Mod2: Num Lock */
    WM_MOD_SUPER = 0x40,   /* Mod4: either Super/Windows key */
};

typedef struct {
    int type;
    int slot;
    int x;
    int y;
    int button;
    int code;
    int value;
    int ascii;
    int mods;                 /* WM_EVENT_RAWKEY: WM_MOD_* bitmask */
    int w;
    int h;
} wm_event_t;

int wm_connect(wm_client_t *wm);
void wm_close(wm_client_t *wm);
int wm_command(wm_client_t *wm, const char *fmt, ...);
int wm_app(wm_client_t *wm, int slot, const char *name);
int wm_title(wm_client_t *wm, int slot, const char *title);
int wm_geom(wm_client_t *wm, int slot, int x, int y, int w, int h);
int wm_clear(wm_client_t *wm, int slot);
int wm_bg(wm_client_t *wm, int slot, const char *color);
int wm_rect(wm_client_t *wm, int slot, int x, int y, int w, int h,
            const char *color);
int wm_text(wm_client_t *wm, int slot, int line, const char *color,
            const char *text);
int wm_text_at(wm_client_t *wm, int slot, int line, int x, int y,
               const char *color, const char *text);
int wm_focus_app(wm_client_t *wm, int slot);
int wm_status(wm_client_t *wm, const char *status);
/*
 * Icons: define index (0-7) as w×h (≤32) pixels given as hex rows — one char
 * per pixel indexing the WM's 16-color palette, 0 = transparent — then place
 * instances. Placements clear with wm_clear; definitions persist.
 */
int wm_icondef(wm_client_t *wm, int slot, int index, int w, int h,
               const char *const *hexrows);
int wm_icon(wm_client_t *wm, int slot, int index, int x, int y);

/*
 * Pixel surfaces (modern path): the client renders into a shared-memory
 * buffer (shm syscalls 500-502) and the compositor blits it as the window
 * body. wm_surface announces the buffer; wm_commit requests a recomposite.
 */
int wm_surface(wm_client_t *wm, int slot, int shmid, int w, int h);
int wm_commit(wm_client_t *wm, int slot);
/* Open this slot's private event channel (/tmp/wmevents<slot>). */
int wm_open_events(wm_event_client_t *events, int slot);
void wm_close_events(wm_event_client_t *events);
int wm_next_event(wm_event_client_t *events, wm_event_t *event);
