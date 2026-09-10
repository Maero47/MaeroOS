#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <wm.h>

#define WMCTL_PATH "/tmp/wmctl"
#define WMEVENTS_PATH "/tmp/wmevents"
#define WM_COMMAND_MAX 128

static int valid_slot(int slot) {
    return slot >= 1 && slot <= WM_MAX_SLOTS;
}

int wm_connect(wm_client_t *wm) {
    if (!wm) return -1;
    wm->fd = open(WMCTL_PATH, O_WRONLY);
    return wm->fd < 0 ? -1 : 0;
}

void wm_close(wm_client_t *wm) {
    if (!wm || wm->fd < 0) return;
    close(wm->fd);
    wm->fd = -1;
}

int wm_command(wm_client_t *wm, const char *fmt, ...) {
    char line[WM_COMMAND_MAX];
    int len;
    va_list ap;

    if (!wm || wm->fd < 0 || !fmt) return -1;
    va_start(ap, fmt);
    len = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (len < 0 || len + 1 >= (int)sizeof(line)) return -1;
    if (write(wm->fd, line, len) != len) return -1;
    if (write(wm->fd, "\n", 1) != 1) return -1;
    return 0;
}

int wm_app(wm_client_t *wm, int slot, const char *name) {
    if (!valid_slot(slot) || !name) return -1;
    return wm_command(wm, "app %d %s", slot, name);
}

int wm_title(wm_client_t *wm, int slot, const char *title) {
    if (!valid_slot(slot) || !title) return -1;
    return wm_command(wm, "title %d %s", slot, title);
}

int wm_geom(wm_client_t *wm, int slot, int x, int y, int w, int h) {
    if (!valid_slot(slot)) return -1;
    return wm_command(wm, "geom %d %d %d %d %d", slot, x, y, w, h);
}

int wm_clear(wm_client_t *wm, int slot) {
    if (!valid_slot(slot)) return -1;
    return wm_command(wm, "clear %d", slot);
}

int wm_bg(wm_client_t *wm, int slot, const char *color) {
    if (!valid_slot(slot) || !color) return -1;
    return wm_command(wm, "bg %d %s", slot, color);
}

int wm_rect(wm_client_t *wm, int slot, int x, int y, int w, int h,
            const char *color) {
    if (!valid_slot(slot) || !color) return -1;
    return wm_command(wm, "rect %d %d %d %d %d %s", slot, x, y, w, h, color);
}

int wm_text(wm_client_t *wm, int slot, int line, const char *color,
            const char *text) {
    if (!valid_slot(slot) || line < 0 || line >= WM_MAX_TEXT_LINES || !color || !text)
        return -1;
    return wm_command(wm, "text %d %d %s %s", slot, line, color, text);
}

int wm_text_at(wm_client_t *wm, int slot, int line, int x, int y,
               const char *color, const char *text) {
    if (!valid_slot(slot) || line < 0 || line >= WM_MAX_TEXT_LINES || !color || !text)
        return -1;
    return wm_command(wm, "textat %d %d %d %d %s %s",
                      slot, line, x, y, color, text);
}

int wm_focus_app(wm_client_t *wm, int slot) {
    if (!valid_slot(slot)) return -1;
    return wm_command(wm, "focus app %d", slot);
}

int wm_status(wm_client_t *wm, const char *status) {
    if (!status) return -1;
    return wm_command(wm, "status %s", status);
}

int wm_icondef(wm_client_t *wm, int slot, int index, int w, int h,
               const char *const *hexrows) {
    if (!valid_slot(slot) || !hexrows || index < 0 || index > 7 ||
        w < 1 || w > 32 || h < 1 || h > 32)
        return -1;
    if (wm_command(wm, "icondef %d %d %d %d", slot, index, w, h) < 0)
        return -1;
    for (int r = 0; r < h; r++) {
        if (wm_command(wm, "irow %d %d %d %s", slot, index, r, hexrows[r]) < 0)
            return -1;
    }
    return 0;
}

int wm_icon(wm_client_t *wm, int slot, int index, int x, int y) {
    if (!valid_slot(slot) || index < 0 || index > 7) return -1;
    return wm_command(wm, "icon %d %d %d %d", slot, index, x, y);
}

int wm_surface(wm_client_t *wm, int slot, int shmid, int w, int h) {
    if (!valid_slot(slot) || shmid < 0 || w < 1 || h < 1) return -1;
    return wm_command(wm, "surface %d %d %d %d", slot, shmid, w, h);
}

int wm_commit(wm_client_t *wm, int slot) {
    if (!valid_slot(slot)) return -1;
    return wm_command(wm, "commit %d", slot);
}

int wm_open_events(wm_event_client_t *events, int slot) {
    char path[40];

    if (!events || !valid_slot(slot)) return -1;
    /* Per-slot FIFO: with a shared channel, concurrent clients steal each
     * other's events (first reader wins). */
    snprintf(path, sizeof(path), WMEVENTS_PATH "%d", slot);
    events->fd = open(path, O_RDONLY | O_NONBLOCK);
    events->used = 0;
    return events->fd < 0 ? -1 : 0;
}

void wm_close_events(wm_event_client_t *events) {
    if (!events || events->fd < 0) return;
    close(events->fd);
    events->fd = -1;
    events->used = 0;
}

static int parse_event_line(const char *line, wm_event_t *event) {
    int slot, a, b, c;

    memset(event, 0, sizeof(*event));
    if (sscanf(line, "mouse %d %d %d %d", &slot, &a, &b, &c) == 4) {
        event->type = WM_EVENT_MOUSE;
        event->slot = slot;
        event->x = a;
        event->y = b;
        event->button = c;
        return 1;
    }
    if (sscanf(line, "key %d %d %d %d", &slot, &a, &b, &c) == 4) {
        event->type = WM_EVENT_KEY;
        event->slot = slot;
        event->code = a;
        event->value = b;
        event->ascii = c;
        return 1;
    }
    /* "rkey" is the uncooked stream: every press AND release, the modifier
     * keys themselves included, with the live modifier mask.  It exists
     * alongside "key" (cooked, presses only) rather than replacing it so that
     * apps written against the cooked stream keep the behaviour they had. */
    if (sscanf(line, "rkey %d %d %d %d", &slot, &a, &b, &c) == 4) {
        event->type = WM_EVENT_RAWKEY;
        event->slot = slot;
        event->code = a;
        event->value = b;
        event->mods = c;
        return 1;
    }
    if (sscanf(line, "focus %d", &slot) == 1) {
        event->type = WM_EVENT_FOCUS;
        event->slot = slot;
        return 1;
    }
    if (sscanf(line, "close %d", &slot) == 1) {
        event->type = WM_EVENT_CLOSE;
        event->slot = slot;
        return 1;
    }
    if (sscanf(line, "geom %d %d %d %d %d", &slot, &a, &b, &c, &event->h) == 5) {
        event->type = WM_EVENT_GEOM;
        event->slot = slot;
        event->x = a;
        event->y = b;
        event->w = c;
        return 1;
    }
    if (sscanf(line, "scroll %d %d %d %d", &slot, &a, &b, &c) == 4) {
        event->type = WM_EVENT_SCROLL;
        event->slot = slot;
        event->x = a;
        event->y = b;
        event->value = c;
        return 1;
    }
    return -1;
}

int wm_next_event(wm_event_client_t *events, wm_event_t *event) {
    char ch;
    int n;

    if (!events || events->fd < 0 || !event) return -1;
    while ((n = read(events->fd, &ch, 1)) == 1) {
        if (ch == '\r') continue;
        if (ch == '\n') {
            events->line[events->used] = 0;
            events->used = 0;
            if (!events->line[0]) continue;
            return parse_event_line(events->line, event);
        }
        if (events->used + 1 < (int)sizeof(events->line))
            events->line[events->used++] = ch;
        else
            events->used = 0;
    }
    if (n < 0) return 0;
    return 0;
}
