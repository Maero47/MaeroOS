#include <gui.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <syscall.h>

/* Window body origin relative to the window: 1px frame, 28px title bar.
 * Events arrive window-relative; widgets live in surface coordinates. */
#define GUI_BODY_X 1
#define GUI_BODY_Y 28

/* MaeroOS shared-memory syscalls */
#define SYS_SHM_CREATE 500
#define SYS_SHM_MAP    501
#define SYS_SHM_UNMAP  502

/* Modern flat theme (light, matches the Aero desktop accent). */
#define COL_ACCENT       draw_rgb(58, 121, 200)   /* primary buttons/selection */
#define COL_ACCENT_TXT   draw_rgb(248, 250, 252)
#define COL_FIELD_BORDER draw_rgb(176, 184, 196)
#define COL_FIELD_BG     draw_rgb(252, 253, 254)
#define COL_FIELD_TEXT   draw_rgb(28, 32, 38)
#define COL_TRACK        draw_rgb(224, 228, 234)
#define COL_THUMB        draw_rgb(150, 160, 174)
#define COL_SELECT       COL_ACCENT
#define COL_BORDER       draw_rgb(200, 206, 214)
#define GUI_RADIUS       5

static void copy_text(char *dst, int dst_len, const char *src) {
    if (!dst || dst_len <= 0) return;
    if (!src) src = "";
    strncpy(dst, src, dst_len - 1);
    dst[dst_len - 1] = 0;
}

static gui_widget_t *add_widget(gui_window_t *gui, int type) {
    gui_widget_t *widget;

    if (!gui || gui->widget_count >= GUI_MAX_WIDGETS) return 0;
    widget = &gui->widgets[gui->widget_count++];
    memset(widget, 0, sizeof(*widget));
    widget->type = type;
    copy_text(widget->color, sizeof(widget->color), "gray");
    copy_text(widget->text_color, sizeof(widget->text_color), "white");
    return widget;
}

/* (Re)create the window's pixel surface to match its current size and
 * announce it to the compositor. */
static int gui_make_surface(gui_window_t *gui) {
    int sw = gui->width - 2;
    int sh = gui->height - GUI_BODY_Y - 1;
    int old_id = gui->shm_id;
    int npages, id, addr;

    if (sw < 16) sw = 16;
    if (sh < 16) sh = 16;
    npages = (sw * sh * 4 + 4095) / 4096;
    id = syscall1(SYS_SHM_CREATE, npages);
    if (id < 0) return -1;
    addr = syscall1(SYS_SHM_MAP, id);
    if (addr <= 0) return -1;

    gui->surf.px = (uint32_t *)(uintptr_t)(unsigned)addr;
    gui->surf.w = sw;
    gui->surf.h = sh;
    gui->shm_id = id;
    draw_fill(&gui->surf, draw_color(gui->bg));
    /* Announce the new buffer; the desktop unmaps the old one itself. */
    wm_surface(&gui->wm, gui->slot, id, sw, sh);
    if (old_id >= 0)
        syscall1(SYS_SHM_UNMAP, old_id);
    return 0;
}

int gui_open(gui_window_t *gui, int slot, const char *title,
             int x, int y, int w, int h) {
    if (!gui || slot < 1 || slot > WM_MAX_SLOTS) return -1;
    memset(gui, 0, sizeof(*gui));
    gui->slot = slot;
    gui->width = w;
    gui->height = h;
    gui->shm_id = -1;
    copy_text(gui->bg, sizeof(gui->bg), "white");

    if (wm_connect(&gui->wm) < 0) return -1;
    if (wm_open_events(&gui->events, slot) < 0) {
        wm_close(&gui->wm);
        return -1;
    }
    if (wm_app(&gui->wm, slot, title ? title : "GUI App") < 0 ||
        wm_title(&gui->wm, slot, title ? title : "GUI") < 0 ||
        wm_geom(&gui->wm, slot, x, y, w, h) < 0 ||
        wm_focus_app(&gui->wm, slot) < 0 ||
        gui_make_surface(gui) < 0) {
        gui_close(gui);
        return -1;
    }
    return 0;
}

void gui_close(gui_window_t *gui) {
    if (!gui) return;
    if (gui->shm_id >= 0) {
        syscall1(SYS_SHM_UNMAP, gui->shm_id);
        gui->shm_id = -1;
        gui->surf.px = 0;
    }
    wm_close_events(&gui->events);
    wm_close(&gui->wm);
}

int gui_panel(gui_window_t *gui, int x, int y, int w, int h,
              const char *color) {
    gui_widget_t *widget = add_widget(gui, GUI_WIDGET_PANEL);

    if (!widget) return -1;
    widget->x = x;
    widget->y = y;
    widget->w = w;
    widget->h = h;
    copy_text(widget->color, sizeof(widget->color), color ? color : "gray");
    return 0;
}

int gui_label(gui_window_t *gui, int x, int y, const char *text,
              const char *color) {
    gui_widget_t *widget = add_widget(gui, GUI_WIDGET_LABEL);

    if (!widget) return -1;
    widget->x = x;
    widget->y = y;
    copy_text(widget->text, sizeof(widget->text), text);
    copy_text(widget->text_color, sizeof(widget->text_color), color ? color : "black");
    return 0;
}

int gui_button(gui_window_t *gui, int id, int x, int y, int w, int h,
               const char *text, gui_button_cb callback) {
    gui_widget_t *widget = add_widget(gui, GUI_WIDGET_BUTTON);

    if (!widget) return -1;
    widget->id = id;
    widget->x = x;
    widget->y = y;
    widget->w = w;
    widget->h = h;
    widget->callback = callback;
    copy_text(widget->text, sizeof(widget->text), text);
    copy_text(widget->color, sizeof(widget->color), "blue");
    copy_text(widget->text_color, sizeof(widget->text_color), "white");
    return 0;
}

int gui_textinput(gui_window_t *gui, int id, int x, int y, int w,
                  const char *initial, gui_button_cb on_enter) {
    gui_widget_t *widget = add_widget(gui, GUI_WIDGET_TEXTINPUT);

    if (!widget) return -1;
    widget->id = id;
    widget->x = x;
    widget->y = y;
    widget->w = w;
    widget->h = 24;
    widget->callback = on_enter;
    copy_text(widget->input, sizeof(widget->input), initial ? initial : "");
    widget->input_len = (int)strlen(widget->input);
    return 0;
}

int gui_checkbox(gui_window_t *gui, int id, int x, int y, const char *label,
                 int checked, gui_button_cb on_toggle) {
    gui_widget_t *widget = add_widget(gui, GUI_WIDGET_CHECKBOX);

    if (!widget) return -1;
    widget->id = id;
    widget->x = x;
    widget->y = y;
    widget->w = 16 + 8 + (int)strlen(label ? label : "") * 8;
    widget->h = 16;
    widget->checked = checked ? 1 : 0;
    widget->callback = on_toggle;
    copy_text(widget->text, sizeof(widget->text), label);
    copy_text(widget->text_color, sizeof(widget->text_color), "black");
    return 0;
}

int gui_scrollbar(gui_window_t *gui, int id, int x, int y, int h, int range,
                  gui_button_cb on_change) {
    gui_widget_t *widget = add_widget(gui, GUI_WIDGET_SCROLLBAR);

    if (!widget) return -1;
    widget->id = id;
    widget->x = x;
    widget->y = y;
    widget->w = 14;
    widget->h = h;
    widget->range = range > 0 ? range : 0;
    widget->callback = on_change;
    return 0;
}

int gui_listbox(gui_window_t *gui, int id, int x, int y, int w,
                int visible_rows, const char **items, int item_count,
                gui_button_cb on_select) {
    gui_widget_t *widget = add_widget(gui, GUI_WIDGET_LISTBOX);

    if (!widget) return -1;
    widget->id = id;
    widget->x = x;
    widget->y = y;
    widget->w = w;
    widget->h = visible_rows * GUI_LIST_ROW_H + 4;
    widget->items = items;
    widget->item_count = item_count;
    widget->visible_rows = visible_rows;
    widget->value = -1;       /* nothing selected */
    widget->callback = on_select;
    return 0;
}

int gui_icondef(gui_window_t *gui, int index, int w, int h,
                const char *const *hexrows) {
    if (!gui || index < 0 || index >= GUI_MAX_ICONDEFS) return -1;
    /* Rendering is client-side now; just remember the (static) pixel data. */
    gui->icondefs[index].rows = hexrows;
    gui->icondefs[index].w = w;
    gui->icondefs[index].h = h;
    return 0;
}

/* Define icon `index` from a full-color themed .mic file (Reversal-blue).
 * Takes precedence over hex rows for that slot.  Returns 0 on success, -1 if
 * the file is missing (caller can fall back to gui_icondef hex-art). */
int gui_icondef_mic(gui_window_t *gui, int index, const char *path,
                    int draw_w, int draw_h) {
    if (!gui || index < 0 || index >= GUI_MAX_ICONDEFS) return -1;
    draw_image_t *img = draw_load_mic(path);
    if (!img) return -1;
    gui->icondefs[index].mic = img;
    gui->icondefs[index].w = draw_w;
    gui->icondefs[index].h = draw_h;
    return 0;
}

int gui_image(gui_window_t *gui, int id, int x, int y, int index) {
    gui_widget_t *widget = add_widget(gui, GUI_WIDGET_IMAGE);

    if (!widget) return -1;
    widget->id = id;
    widget->x = x;
    widget->y = y;
    widget->value = index;
    return 0;
}

gui_widget_t *gui_find(gui_window_t *gui, int id) {
    if (!gui) return 0;
    for (int i = 0; i < gui->widget_count; i++) {
        if (gui->widgets[i].id == id && gui->widgets[i].type != GUI_WIDGET_PANEL &&
            gui->widgets[i].type != GUI_WIDGET_LABEL)
            return &gui->widgets[i];
    }
    return 0;
}

const char *gui_input_text(gui_window_t *gui, int id) {
    gui_widget_t *w = gui_find(gui, id);
    return (w && w->type == GUI_WIDGET_TEXTINPUT) ? w->input : "";
}

int gui_value(gui_window_t *gui, int id) {
    gui_widget_t *w = gui_find(gui, id);
    if (!w) return -1;
    if (w->type == GUI_WIDGET_CHECKBOX) return w->checked;
    return w->value;
}

void gui_list_set_scroll(gui_window_t *gui, int id, int scroll) {
    gui_widget_t *w = gui_find(gui, id);
    if (!w || w->type != GUI_WIDGET_LISTBOX) return;
    if (scroll < 0) scroll = 0;
    if (scroll > w->item_count - w->visible_rows)
        scroll = w->item_count - w->visible_rows;
    if (scroll < 0) scroll = 0;
    w->scroll = scroll;
}

void gui_bind_scroll(gui_window_t *gui, int list_id, int scrollbar_id) {
    gui_widget_t *list = gui_find(gui, list_id);
    gui_widget_t *sb = gui_find(gui, scrollbar_id);

    if (!list || list->type != GUI_WIDGET_LISTBOX) return;
    if (!sb || sb->type != GUI_WIDGET_SCROLLBAR) return;
    list->link_id = scrollbar_id;
}

void gui_set_layout(gui_window_t *gui, gui_layout_cb callback) {
    if (!gui) return;
    gui->layout = callback;
}

void gui_set_rawkey_handler(gui_window_t *gui, gui_rawkey_cb callback) {
    if (gui) gui->on_rawkey = callback;
}

void gui_set_key_handler(gui_window_t *gui, gui_key_cb callback) {
    if (!gui) return;
    gui->on_key = callback;
}

void gui_set_click_handler(gui_window_t *gui, gui_click_cb callback) {
    if (!gui) return;
    gui->on_click = callback;
}

void gui_set_scroll_handler(gui_window_t *gui, gui_scroll_cb callback) {
    if (!gui) return;
    gui->on_scroll = callback;
}

int gui_body_width(const gui_window_t *gui) {
    if (!gui) return 0;
    return gui->surf.w > 0 ? gui->surf.w : gui->width - 2;
}

int gui_body_height(const gui_window_t *gui) {
    if (!gui) return 0;
    return gui->surf.h > 0 ? gui->surf.h : gui->height - GUI_BODY_Y - 1;
}

int gui_draw(gui_window_t *gui) {
    draw_surface_t *s;

    if (!gui || !gui->surf.px) return -1;
    s = &gui->surf;
    draw_fill(s, draw_color(gui->bg));

    for (int i = 0; i < gui->widget_count; i++) {
        gui_widget_t *w = &gui->widgets[i];

        switch (w->type) {
        case GUI_WIDGET_PANEL:
            draw_rect(s, w->x, w->y, w->w, w->h, draw_color(w->color));
            break;
        case GUI_WIDGET_LABEL:
            draw_text_aa(s, w->x, w->y, w->text, draw_color(w->text_color),
                         &draw_font_ui);
            break;
        case GUI_WIDGET_BUTTON: {
            /* Narrow buttons center their label; wide ones (list rows)
             * stay left-aligned next to their icons. */
            int tw = draw_text_width(w->text, &draw_font_ui);
            int tx = w->w <= 200 ? w->x + (w->w - tw) / 2 : w->x + 10;
            uint32_t bc = draw_color(w->color);
            draw_round_rect(s, w->x, w->y, w->w, w->h, GUI_RADIUS, bc);
            draw_text_aa(s, tx, w->y + (w->h - draw_font_ui.line_h) / 2,
                         w->text, draw_color(w->text_color), &draw_font_ui);
            break;
        }
        case GUI_WIDGET_TEXTINPUT: {
            int focused = gui->focus_id == w->id;
            draw_round_rect(s, w->x, w->y, w->w, w->h, GUI_RADIUS, COL_FIELD_BG);
            draw_round_frame(s, w->x, w->y, w->w, w->h, GUI_RADIUS,
                             focused ? COL_ACCENT : COL_FIELD_BORDER);
            if (w->input_len)
                draw_text_aa(s, w->x + 8,
                             w->y + (w->h - draw_font_ui.line_h) / 2,
                             w->input, COL_FIELD_TEXT, &draw_font_ui);
            if (focused) {
                int cw = draw_text_width(w->input, &draw_font_ui);
                draw_rect(s, w->x + 8 + cw, w->y + 5, 2, w->h - 10, COL_ACCENT);
            }
            break;
        }
        case GUI_WIDGET_CHECKBOX:
            draw_round_rect(s, w->x, w->y, 18, 18, 4,
                            w->checked ? COL_SELECT : COL_FIELD_BG);
            draw_round_frame(s, w->x, w->y, 18, 18, 4,
                             w->checked ? COL_SELECT : COL_FIELD_BORDER);
            if (w->checked) {            /* white checkmark */
                draw_rect(s, w->x + 4, w->y + 8, 2, 4, COL_ACCENT_TXT);
                draw_rect(s, w->x + 5, w->y + 10, 2, 3, COL_ACCENT_TXT);
                draw_rect(s, w->x + 7, w->y + 9, 2, 3, COL_ACCENT_TXT);
                draw_rect(s, w->x + 9, w->y + 6, 2, 4, COL_ACCENT_TXT);
                draw_rect(s, w->x + 11, w->y + 4, 2, 4, COL_ACCENT_TXT);
            }
            draw_text_aa(s, w->x + 26, w->y + (18 - draw_font_ui.line_h) / 2,
                         w->text, draw_color(w->text_color), &draw_font_ui);
            break;
        case GUI_WIDGET_SCROLLBAR: {
            int track = w->h - 24;
            int ty = w->range ? w->y + 4 + track * w->value / w->range
                              : w->y + 4;
            int tw2 = w->w - 6 < 4 ? 4 : w->w - 6;
            draw_round_rect(s, w->x + (w->w - tw2) / 2, w->y, tw2, w->h,
                            tw2 / 2, COL_TRACK);
            draw_round_rect(s, w->x + (w->w - tw2) / 2, ty, tw2, 18,
                            tw2 / 2, COL_THUMB);
            break;
        }
        case GUI_WIDGET_LISTBOX:
            draw_round_rect(s, w->x, w->y, w->w, w->h, GUI_RADIUS, COL_FIELD_BG);
            draw_round_frame(s, w->x, w->y, w->w, w->h, GUI_RADIUS, COL_BORDER);
            for (int r = 0; r < w->visible_rows; r++) {
                int idx = w->scroll + r;
                if (idx >= w->item_count) break;
                if (idx == w->value)
                    draw_round_rect(s, w->x + 3, w->y + 3 + r * GUI_LIST_ROW_H,
                                    w->w - 6, GUI_LIST_ROW_H, 3, COL_SELECT);
                draw_text_aa(s, w->x + 10,
                             w->y + 3 + r * GUI_LIST_ROW_H +
                             (GUI_LIST_ROW_H - draw_font_ui.line_h) / 2,
                             w->items[idx],
                             idx == w->value ? COL_ACCENT_TXT : COL_FIELD_TEXT,
                             &draw_font_ui);
            }
            break;
        case GUI_WIDGET_IMAGE:
            if (w->value >= 0 && w->value < GUI_MAX_ICONDEFS) {
                if (gui->icondefs[w->value].mic)
                    draw_image_blit_scaled(s, w->x, w->y,
                                           gui->icondefs[w->value].w,
                                           gui->icondefs[w->value].h,
                                           gui->icondefs[w->value].mic);
                else if (gui->icondefs[w->value].rows)
                    draw_icon(s, w->x, w->y, gui->icondefs[w->value].w,
                              gui->icondefs[w->value].h,
                              gui->icondefs[w->value].rows);
            }
            break;
        }
    }
    return wm_commit(&gui->wm, gui->slot);
}

static int inside_widget(const gui_widget_t *widget, int x, int y) {
    if (!widget) return 0;
    return x >= widget->x && y >= widget->y &&
           x < widget->x + widget->w && y < widget->y + widget->h;
}

static void scrollbar_set_from_y(gui_window_t *gui, gui_widget_t *w, int y) {
    int track = w->h - 24;
    int v = track > 0 ? (y - w->y - 4) * w->range / track : 0;

    if (v < 0) v = 0;
    if (v > w->range) v = w->range;
    if (v == w->value) return;
    w->value = v;
    gui_draw(gui);
    if (w->callback) w->callback(gui, w->id);
}

static void handle_mouse(gui_window_t *gui, const wm_event_t *event) {
    int x = event->x - GUI_BODY_X;
    int y = event->y - GUI_BODY_Y;
    int press = event->button && !gui->mouse_down;
    int motion = event->button && gui->mouse_down;

    gui->mouse_down = event->button ? 1 : 0;

    if (!event->button) {              /* release ends any drag */
        gui->drag_id = 0;
        return;
    }

    if (motion) {                      /* drag: only scrollbars care */
        gui_widget_t *w = gui->drag_id ? gui_find(gui, gui->drag_id) : 0;
        if (w && w->type == GUI_WIDGET_SCROLLBAR)
            scrollbar_set_from_y(gui, w, y);
        return;
    }

    if (!press) return;
    /* Raw click hook first (apps with custom-rendered controls). */
    if (gui->on_click) {
        gui->on_click(gui, x, y);
        return;
    }
    for (int i = gui->widget_count - 1; i >= 0; i--) {
        gui_widget_t *w = &gui->widgets[i];

        if (!inside_widget(w, x, y)) continue;
        switch (w->type) {
        case GUI_WIDGET_BUTTON:
            if (w->callback) w->callback(gui, w->id);
            return;
        case GUI_WIDGET_TEXTINPUT:
            gui->focus_id = w->id;
            gui_draw(gui);
            return;
        case GUI_WIDGET_CHECKBOX:
            w->checked = !w->checked;
            gui_draw(gui);
            if (w->callback) w->callback(gui, w->id);
            return;
        case GUI_WIDGET_SCROLLBAR:
            gui->drag_id = w->id;      /* thumb follows until release */
            scrollbar_set_from_y(gui, w, y);
            return;
        case GUI_WIDGET_LISTBOX: {
            int idx = w->scroll + (y - w->y - 2) / GUI_LIST_ROW_H;
            if (idx < 0 || idx >= w->item_count) return;
            w->value = idx;
            gui_draw(gui);
            if (w->callback) w->callback(gui, w->id);
            return;
        }
        default:
            break;
        }
    }
}

/* Wheel over a listbox scrolls it (syncing a bound scrollbar); wheel over a
 * scrollbar nudges its value. */
static void handle_scroll(gui_window_t *gui, const wm_event_t *event) {
    int x = event->x - GUI_BODY_X;
    int y = event->y - GUI_BODY_Y;
    int delta = event->value;

    if (gui->on_scroll) {
        gui->on_scroll(gui, delta);
        return;
    }
    for (int i = gui->widget_count - 1; i >= 0; i--) {
        gui_widget_t *w = &gui->widgets[i];

        if (!inside_widget(w, x, y)) continue;
        if (w->type == GUI_WIDGET_LISTBOX) {
            int max_scroll = w->item_count - w->visible_rows;
            int s = w->scroll - delta;     /* wheel up (+) shows earlier */
            if (max_scroll < 0) max_scroll = 0;
            if (s < 0) s = 0;
            if (s > max_scroll) s = max_scroll;
            if (s == w->scroll) return;
            w->scroll = s;
            if (w->link_id) {
                gui_widget_t *sb = gui_find(gui, w->link_id);
                if (sb && sb->type == GUI_WIDGET_SCROLLBAR)
                    sb->value = s <= sb->range ? s : sb->range;
            }
            gui_draw(gui);
            return;
        }
        if (w->type == GUI_WIDGET_SCROLLBAR) {
            int v = w->value - delta;
            if (v < 0) v = 0;
            if (v > w->range) v = w->range;
            if (v == w->value) return;
            w->value = v;
            gui_draw(gui);
            if (w->callback) w->callback(gui, w->id);
            return;
        }
    }
}

static void handle_key(gui_window_t *gui, const wm_event_t *event) {
    gui_widget_t *w;

    if (event->value != 1) return;        /* presses only */
    if (gui->on_key) {
        gui->on_key(gui, event->code, event->value, event->ascii);
        return;
    }
    if (!gui->focus_id) return;
    w = gui_find(gui, gui->focus_id);
    if (!w || w->type != GUI_WIDGET_TEXTINPUT) return;

    if (event->code == KEY_BACKSPACE) {
        if (w->input_len) {
            w->input[--w->input_len] = 0;
            gui_draw(gui);
        }
        return;
    }
    if (event->code == KEY_ENTER) {
        if (w->callback) w->callback(gui, w->id);
        return;
    }
    if (event->ascii >= 32 && event->ascii < 127 &&
        w->input_len + 1 < (int)sizeof(w->input)) {
        w->input[w->input_len++] = (char)event->ascii;
        w->input[w->input_len] = 0;
        gui_draw(gui);
    }
}

int gui_poll(gui_window_t *gui) {
    wm_event_t event;
    int count = 0;
    int got;

    if (!gui) return -1;
    while ((got = wm_next_event(&gui->events, &event)) > 0) {
        if (event.slot != gui->slot) continue;
        count++;
        if (event.type == WM_EVENT_MOUSE) {
            handle_mouse(gui, &event);
        } else if (event.type == WM_EVENT_SCROLL) {
            handle_scroll(gui, &event);
        } else if (event.type == WM_EVENT_KEY) {
            handle_key(gui, &event);
        } else if (event.type == WM_EVENT_RAWKEY) {
            if (gui->on_rawkey)
                gui->on_rawkey(gui, event.code, event.value, event.mods);
        } else if (event.type == WM_EVENT_FOCUS) {
            gui->focused = 1;
        } else if (event.type == WM_EVENT_GEOM) {
            int size_changed = event.w != gui->width || event.h != gui->height;

            gui->width = event.w;
            gui->height = event.h;
            if (size_changed) {
                /* New body size → new shared surface, then re-render. */
                gui_make_surface(gui);
                if (gui->layout)
                    gui->layout(gui);
                gui_draw(gui);
            }
        } else if (event.type == WM_EVENT_CLOSE) {
            gui->closed = 1;
            break;   /* let the app's main loop see closed and exit */
        }
    }
    return count;
}
