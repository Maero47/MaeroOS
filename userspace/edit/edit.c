#include <draw.h>
#include <fcntl.h>
#include <gui.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/*
 * edit — a small text editor.  Line-array buffer, click-to-place cursor,
 * arrow keys, wheel scroll, Ctrl+S saves.  Opens the path in argv[2].
 */

#define MAX_LINES 1024
#define LINE_CAP  200
#define ROW_H     20

static gui_window_t gui;
static char lines[MAX_LINES][LINE_CAP];
static int line_count = 1;
static int cx, cy;            /* cursor column/row (in characters/lines) */
static int view;              /* first visible line */
static int dirty = 1;
static int modified;
static char path[160] = "/untitled.txt";
static char status[96] = "New file";

static void sleep_ms(long ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, 0);
}

static void set_status(const char *s) {
    strncpy(status, s, sizeof(status) - 1);
    status[sizeof(status) - 1] = 0;
    dirty = 1;
}

static void load_file(void) {
    int fd = open(path, O_RDONLY);
    static char buf[64 * 1024];
    int n, total = 0, col = 0;

    line_count = 1;
    lines[0][0] = 0;
    if (fd < 0) { set_status("New file"); return; }
    while (total < (int)sizeof(buf) - 1 &&
           (n = read(fd, buf + total, (int)sizeof(buf) - 1 - total)) > 0)
        total += n;
    close(fd);
    buf[total] = 0;

    line_count = 0;
    col = 0;
    for (int i = 0; i < total && line_count < MAX_LINES - 1; i++) {
        char c = buf[i];
        if (c == '\n') {
            lines[line_count][col] = 0;
            line_count++;
            col = 0;
            continue;
        }
        if (c == '\r') continue;
        if (c == '\t') c = ' ';
        if (col < LINE_CAP - 1)
            lines[line_count][col++] = c;
    }
    lines[line_count][col] = 0;
    line_count++;
    set_status("Loaded");
}

static void save_file(void) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    char msg[128];

    if (fd < 0) { set_status("Save FAILED (read-only fs?)"); return; }
    for (int i = 0; i < line_count; i++) {
        write(fd, lines[i], (int)strlen(lines[i]));
        write(fd, "\n", 1);
    }
    close(fd);
    modified = 0;
    snprintf(msg, sizeof(msg), "Saved %s", path);
    set_status(msg);
}

static void clamp_cursor(void) {
    if (cy < 0) cy = 0;
    if (cy >= line_count) cy = line_count - 1;
    if (cx < 0) cx = 0;
    {
        int len = (int)strlen(lines[cy]);
        if (cx > len) cx = len;
    }
}

static void insert_char(char c) {
    char *l = lines[cy];
    int len = (int)strlen(l);

    if (len >= LINE_CAP - 2) return;
    memmove(l + cx + 1, l + cx, (size_t)(len - cx + 1));
    l[cx] = c;
    cx++;
    modified = 1;
}

static void newline(void) {
    if (line_count >= MAX_LINES - 1) return;
    memmove(&lines[cy + 2], &lines[cy + 1],
            (size_t)(line_count - cy - 1) * LINE_CAP);
    strcpy(lines[cy + 1], lines[cy] + cx);
    lines[cy][cx] = 0;
    line_count++;
    cy++;
    cx = 0;
    modified = 1;
}

static void backspace(void) {
    char *l = lines[cy];

    if (cx > 0) {
        int len = (int)strlen(l);
        memmove(l + cx - 1, l + cx, (size_t)(len - cx + 1));
        cx--;
        modified = 1;
        return;
    }
    if (cy == 0) return;
    /* join with the previous line */
    {
        int plen = (int)strlen(lines[cy - 1]);
        int room = LINE_CAP - 1 - plen;
        strncpy(lines[cy - 1] + plen, l, (size_t)room);
        lines[cy - 1][LINE_CAP - 1] = 0;
        memmove(&lines[cy], &lines[cy + 1],
                (size_t)(line_count - cy - 1) * LINE_CAP);
        line_count--;
        cy--;
        cx = plen;
        modified = 1;
    }
}

static void render(void) {
    draw_surface_t *s = &gui.surf;
    int rows, vis = 0;
    char hdr[192];

    if (!s->px) return;
    rows = (s->h - 30 - 24) / ROW_H;
    if (cy < view) view = cy;
    if (cy >= view + rows) view = cy - rows + 1;

    draw_fill(s, draw_rgb(250, 250, 248));
    draw_rect(s, 0, 0, s->w, 26, draw_rgb(52, 60, 76));
    snprintf(hdr, sizeof(hdr), "%s%s   (Ctrl+S saves)",
             path, modified ? " *" : "");
    draw_text_aa(s, 8, 4, hdr, draw_rgb(238, 240, 235), &draw_font_ui);

    for (int i = view; i < line_count && vis < rows; i++, vis++) {
        int ry = 30 + vis * ROW_H;
        if (i == cy) {
            draw_rect(s, 0, ry - 1, s->w, ROW_H, draw_rgb(238, 242, 246));
            /* caret at the cursor column */
            {
                char tmp = lines[i][cx];
                int px;
                lines[i][cx] = 0;
                px = 8 + draw_text_width(lines[i], &draw_font_ui);
                lines[i][cx] = tmp;
                draw_rect(s, px, ry, 2, ROW_H - 3, draw_rgb(94, 129, 172));
            }
        }
        draw_text_aa(s, 8, ry, lines[i], draw_rgb(30, 34, 36), &draw_font_ui);
    }

    draw_text_aa(s, 8, s->h - 21, status, draw_rgb(102, 110, 112),
                 &draw_font_ui);
    wm_commit(&gui.wm, gui.slot);
    dirty = 0;
}

static void on_key(gui_window_t *g, int code, int value, int ascii) {
    (void)g;
    (void)value;
    if (ascii == 19) { save_file(); return; }   /* Ctrl+S */
    if (code == KEY_UP)    { cy--; clamp_cursor(); dirty = 1; return; }
    if (code == KEY_DOWN)  { cy++; clamp_cursor(); dirty = 1; return; }
    if (code == KEY_LEFT)  { cx--; clamp_cursor(); dirty = 1; return; }
    if (code == KEY_RIGHT) { cx++; clamp_cursor(); dirty = 1; return; }
    if (code == KEY_ENTER) { newline(); dirty = 1; return; }
    if (code == KEY_BACKSPACE) { backspace(); dirty = 1; return; }
    if (code == KEY_TAB) {
        insert_char(' '); insert_char(' ');
        dirty = 1;
        return;
    }
    if (ascii >= 32 && ascii < 127) {
        insert_char((char)ascii);
        dirty = 1;
    }
}

static void on_click(gui_window_t *g, int x, int y) {
    (void)g;
    if (y < 30) return;
    cy = view + (y - 30) / ROW_H;
    clamp_cursor();
    /* place the column by measuring prefixes */
    {
        int len = (int)strlen(lines[cy]);
        int best = 0, px = 8;
        for (int i = 0; i <= len; i++) {
            char tmp = lines[cy][i];
            int w;
            lines[cy][i] = 0;
            w = 8 + draw_text_width(lines[cy], &draw_font_ui);
            lines[cy][i] = tmp;
            if (w <= x) best = i;
            else break;
            px = w;
        }
        (void)px;
        cx = best;
    }
    dirty = 1;
}

static void on_scroll(gui_window_t *g, int delta) {
    (void)g;
    view -= delta * 3;
    if (view < 0) view = 0;
    if (view >= line_count) view = line_count - 1;
    dirty = 1;
}

int main(int argc, char *argv[]) {
    int slot = (argc > 1) ? atoi(argv[1]) : 1;

    if (slot < 1 || slot > WM_MAX_SLOTS) slot = 1;
    if (argc > 2) {
        strncpy(path, argv[2], sizeof(path) - 1);
        path[sizeof(path) - 1] = 0;
    }

    if (gui_open(&gui, slot, "Editor", 160 + slot * 14, 60 + slot * 10,
                 640, 460) < 0) {
        printf("edit: desktop unavailable\n");
        return 1;
    }
    gui_set_key_handler(&gui, on_key);
    gui_set_click_handler(&gui, on_click);
    gui_set_scroll_handler(&gui, on_scroll);

    load_file();
    render();
    while (!gui.closed) {
        int events = gui_poll(&gui);
        if (dirty || events > 0)
            render();
        sleep_ms(30);
    }
    gui_close(&gui);
    return 0;
}
