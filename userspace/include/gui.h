#pragma once

#include <draw.h>
#include <wm.h>

#define GUI_MAX_WIDGETS 40
#define GUI_MAX_ICONDEFS 8
#define GUI_MAX_LABEL 40
#define GUI_INPUT_MAX 64
#define GUI_LIST_ROW_H 22

typedef struct gui_window gui_window_t;
typedef void (*gui_button_cb)(gui_window_t *gui, int id);
typedef void (*gui_layout_cb)(gui_window_t *gui);
/* Raw input hooks for apps that render their own content (terminals etc.) */
typedef void (*gui_key_cb)(gui_window_t *gui, int code, int value, int ascii);
/* Uncooked keys: every press and release, modifier keys included, with the
 * live WM_MOD_* mask.  Set this instead of the cooked hook when the app is
 * itself a keyboard consumer that needs keycodes (maeroX turns them into X11
 * KeyPress/KeyRelease). */
typedef void (*gui_rawkey_cb)(gui_window_t *gui, int code, int value, int mods);
typedef void (*gui_scroll_cb)(gui_window_t *gui, int delta);
typedef void (*gui_click_cb)(gui_window_t *gui, int x, int y);

enum {
    GUI_WIDGET_PANEL = 1,
    GUI_WIDGET_LABEL = 2,
    GUI_WIDGET_BUTTON = 3,
    GUI_WIDGET_TEXTINPUT = 4,
    GUI_WIDGET_CHECKBOX = 5,
    GUI_WIDGET_SCROLLBAR = 6,
    GUI_WIDGET_LISTBOX = 7,
    GUI_WIDGET_IMAGE = 8,
};

typedef struct {
    int type;
    int id;
    int x;
    int y;
    int w;
    int h;
    char text[GUI_MAX_LABEL];
    char color[12];
    char text_color[12];
    gui_button_cb callback;   /* click / toggle / enter / select / scroll */
    /* Text input */
    char input[GUI_INPUT_MAX];
    int input_len;
    /* Checkbox */
    int checked;
    /* Scrollbar: value in [0, range]; Listbox: value = selected index */
    int value;
    int range;
    /* Listbox */
    const char **items;
    int item_count;
    int visible_rows;
    int scroll;               /* first visible item index */
    int link_id;              /* paired widget (listbox <-> scrollbar), 0=none */
} gui_widget_t;

struct gui_window {
    wm_client_t wm;
    wm_event_client_t events;
    int slot;
    int width;
    int height;
    int focused;
    int closed;
    int focus_id;             /* widget with keyboard focus (0 = none) */
    int mouse_down;           /* left button held inside the window */
    int drag_id;              /* widget being dragged (scrollbar), 0 = none */
    gui_layout_cb layout;
    gui_key_cb on_key;        /* cooked key hook (bypasses widget routing) */
    gui_rawkey_cb on_rawkey;  /* uncooked key hook (press + release + mods) */
    gui_scroll_cb on_scroll;  /* raw wheel hook (when no widget is hit) */
    gui_click_cb on_click;    /* raw click hook (bypasses widget routing) */
    char bg[12];
    gui_widget_t widgets[GUI_MAX_WIDGETS];
    int widget_count;
    /* Pixel surface (rendered client-side, composited by the desktop) */
    draw_surface_t surf;
    int shm_id;               /* -1 = none */
    /* Client-side icon definitions (hex rows live in the app's data; or a
     * full-color themed .mic loaded into `mic`, which takes precedence). */
    struct {
        const char *const *rows;
        int w, h;
        struct draw_image *mic;   /* NULL = use hex rows */
    } icondefs[GUI_MAX_ICONDEFS];
};

int gui_open(gui_window_t *gui, int slot, const char *title,
             int x, int y, int w, int h);
void gui_close(gui_window_t *gui);
int gui_panel(gui_window_t *gui, int x, int y, int w, int h,
              const char *color);
int gui_label(gui_window_t *gui, int x, int y, const char *text,
              const char *color);
int gui_button(gui_window_t *gui, int id, int x, int y, int w, int h,
               const char *text, gui_button_cb callback);
/* Single-line text input; on_enter fires when Enter is pressed inside it. */
int gui_textinput(gui_window_t *gui, int id, int x, int y, int w,
                  const char *initial, gui_button_cb on_enter);
/* Checkbox with a label to its right; on_toggle fires on click. */
int gui_checkbox(gui_window_t *gui, int id, int x, int y, const char *label,
                 int checked, gui_button_cb on_toggle);
/* Vertical scrollbar; value in [0,range]; click positions the thumb. */
int gui_scrollbar(gui_window_t *gui, int id, int x, int y, int h, int range,
                  gui_button_cb on_change);
/* List of items; click selects a row; on_select fires with the widget id. */
int gui_listbox(gui_window_t *gui, int id, int x, int y, int w,
                int visible_rows, const char **items, int item_count,
                gui_button_cb on_select);

/* Widget state access (by widget id; NULL/-1 when not found). */
gui_widget_t *gui_find(gui_window_t *gui, int id);
const char *gui_input_text(gui_window_t *gui, int id);
int gui_value(gui_window_t *gui, int id);
void gui_list_set_scroll(gui_window_t *gui, int id, int scroll);
/* Pair a listbox with a scrollbar: wheel over the list moves the thumb. */
void gui_bind_scroll(gui_window_t *gui, int list_id, int scrollbar_id);

/* Define WM icon `index` (0-7) from hex rows (see wm_icondef). */
int gui_icondef(gui_window_t *gui, int index, int w, int h,
                const char *const *hexrows);
/* Define icon `index` from a full-color themed .mic file (drawn at draw_w x
 * draw_h).  Takes precedence over hex rows.  Returns 0 ok / -1 if missing. */
int gui_icondef_mic(gui_window_t *gui, int index, const char *path,
                    int draw_w, int draw_h);
/* Image widget showing icon `index` at (x,y); index < 0 hides it.  Change
 * the shown icon later via gui_find(gui,id)->value. */
int gui_image(gui_window_t *gui, int id, int x, int y, int index);

void gui_set_layout(gui_window_t *gui, gui_layout_cb callback);
void gui_set_key_handler(gui_window_t *gui, gui_key_cb callback);
void gui_set_rawkey_handler(gui_window_t *gui, gui_rawkey_cb callback);
void gui_set_scroll_handler(gui_window_t *gui, gui_scroll_cb callback);
void gui_set_click_handler(gui_window_t *gui, gui_click_cb callback);
int gui_body_width(const gui_window_t *gui);
int gui_body_height(const gui_window_t *gui);
int gui_draw(gui_window_t *gui);
int gui_poll(gui_window_t *gui);
