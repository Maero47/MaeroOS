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
 * edit — a small text editor.  Line-array buffer of UTF-8 text.
 *
 *   Ctrl+N new          Ctrl+O open...       Ctrl+S save
 *   Ctrl+Shift+S save as...                   Ctrl+F find (Enter/F3: next)
 *   Ctrl+A select all   Ctrl+C/X/V copy/cut/paste (desktop clipboard)
 *   Shift+arrows/Home/End select; mouse drag selects; wheel scrolls.
 *
 * Closing the window (or New/Open) with unsaved changes asks first.  Opens
 * the path in argv[2].
 */

#define MAX_LINES 2048
#define LINE_CAP  256
#define ROW_H     20
#define TEXT_X    8
#define TOOLBAR_H 30
#define TEXT_Y    (TOOLBAR_H + 4)
#define STATUS_H  24

static gui_window_t gui;
static char lines[MAX_LINES][LINE_CAP];
static int line_count = 1;
static int cx, cy;            /* cursor: byte offset in line / line */
static int view;              /* first visible line */
static int dirty = 1;
static int modified;
static char path[160] = "/untitled.txt";
static char status[128] = "New file";

/* Selection anchor (active when sel_on); the other end is the cursor. */
static int sel_on, sel_x, sel_y;
static int mouse_selecting;

/* Mini-buffer prompt at the bottom. */
enum { P_NONE, P_OPEN, P_SAVEAS, P_FIND, P_CONFIRM };
enum { AFTER_NOTHING, AFTER_CLOSE, AFTER_NEW, AFTER_OPEN };
static int prompt;
static int after_confirm;     /* what a Yes/No in P_CONFIRM goes on to do */
static char prompt_text[160];
static int prompt_len;
static char find_text[80];
static char open_target[160];

/* Toolbar */
enum { TB_NEW, TB_OPEN, TB_SAVE, TB_SAVEAS, TB_FIND, TB_COUNT };
static const char *const tb_labels[TB_COUNT] = {
    "New", "Open", "Save", "Save as", "Find",
};
static int tb_x[TB_COUNT], tb_w[TB_COUNT];

static void sleep_ms(long ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, 0);
}

static void set_status(const char *s) {
    strncpy(status, s, sizeof(status) - 1);
    status[sizeof(status) - 1] = 0;
    dirty = 1;
}

static void update_title(void) {
    char t[64];
    const char *base = strrchr(path, '/');
    snprintf(t, sizeof(t), "%s%s - Editor", modified ? "*" : "",
             base ? base + 1 : path);
    wm_title(&gui.wm, gui.slot, t);
}

static void set_modified(int m) {
    if (m != modified) {
        modified = m;
        update_title();
    }
}

/* ── UTF-8 cursor helpers ─────────────────────────────────────────────── */

static int is_cont(char c) { return ((unsigned char)c & 0xC0) == 0x80; }

static int prev_char(const char *l, int x) {
    if (x <= 0) return 0;
    x--;
    while (x > 0 && is_cont(l[x])) x--;
    return x;
}

static int next_char(const char *l, int x) {
    int len = (int)strlen(l);
    if (x >= len) return len;
    x++;
    while (x < len && is_cont(l[x])) x++;
    return x;
}

/* Pixel x of byte offset `x` in line `l`. */
static int col_px(const char *l, int x) {
    char tmp[LINE_CAP];
    memcpy(tmp, l, (size_t)x);
    tmp[x] = 0;
    return TEXT_X + draw_text_width(tmp, &draw_font_ui);
}

/* Byte offset nearest pixel x in line `l`. */
static int px_col(const char *l, int px) {
    int x = 0, len = (int)strlen(l);
    while (x < len) {
        int nx = next_char(l, x);
        int mid = (col_px(l, x) + col_px(l, nx)) / 2;
        if (px < mid) break;
        x = nx;
    }
    return x;
}

static void clamp_cursor(void) {
    int len;
    if (cy < 0) cy = 0;
    if (cy >= line_count) cy = line_count - 1;
    len = (int)strlen(lines[cy]);
    if (cx < 0) cx = 0;
    if (cx > len) cx = len;
    while (cx > 0 && is_cont(lines[cy][cx])) cx--;
}

/* ── Selection ────────────────────────────────────────────────────────── */

static int has_selection(void) {
    return sel_on && (sel_x != cx || sel_y != cy);
}

static void sel_range(int *x0, int *y0, int *x1, int *y1) {
    if (sel_y < cy || (sel_y == cy && sel_x < cx)) {
        *x0 = sel_x; *y0 = sel_y; *x1 = cx; *y1 = cy;
    } else {
        *x0 = cx; *y0 = cy; *x1 = sel_x; *y1 = sel_y;
    }
}

/* Copy the selected text into buf (NUL-terminated); returns its length. */
static int selection_text(char *buf, int max) {
    int x0, y0, x1, y1, n = 0;

    if (!has_selection()) { buf[0] = 0; return 0; }
    sel_range(&x0, &y0, &x1, &y1);
    for (int y = y0; y <= y1 && n < max - 1; y++) {
        int from = y == y0 ? x0 : 0;
        int to = y == y1 ? x1 : (int)strlen(lines[y]);
        for (int i = from; i < to && n < max - 1; i++) buf[n++] = lines[y][i];
        if (y != y1 && n < max - 1) buf[n++] = '\n';
    }
    buf[n] = 0;
    return n;
}

static void delete_selection(void) {
    int x0, y0, x1, y1, room;

    if (!has_selection()) { sel_on = 0; return; }
    sel_range(&x0, &y0, &x1, &y1);
    /* Join the head of y0 with the tail of y1, then drop the lines between. */
    room = LINE_CAP - 1 - x0;
    {
        char tail[LINE_CAP];
        strncpy(tail, lines[y1] + x1, sizeof(tail) - 1);
        tail[sizeof(tail) - 1] = 0;
        lines[y0][x0] = 0;
        strncat(lines[y0], tail, (size_t)room);
    }
    if (y1 > y0) {
        memmove(&lines[y0 + 1], &lines[y1 + 1],
                (size_t)(line_count - y1 - 1) * LINE_CAP);
        line_count -= y1 - y0;
    }
    cx = x0;
    cy = y0;
    sel_on = 0;
    set_modified(1);
}

/* ── Editing ──────────────────────────────────────────────────────────── */

static void insert_bytes(const char *s, int n) {
    char *l = lines[cy];
    int len = (int)strlen(l);

    if (len + n >= LINE_CAP - 1) return;
    memmove(l + cx + n, l + cx, (size_t)(len - cx + 1));
    memcpy(l + cx, s, (size_t)n);
    cx += n;
    set_modified(1);
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
    set_modified(1);
}

static void insert_text(const char *s) {
    while (*s) {
        if (*s == '\n') { newline(); s++; continue; }
        if (*s == '\r') { s++; continue; }
        if (*s == '\t') { insert_bytes("    ", 4); s++; continue; }
        {
            const char *p = s;
            draw_utf8_next(&p);
            insert_bytes(s, (int)(p - s));
            s = p;
        }
    }
}

static void backspace(void) {
    char *l = lines[cy];

    if (cx > 0) {
        int len = (int)strlen(l), p = prev_char(l, cx);
        memmove(l + p, l + cx, (size_t)(len - cx + 1));
        cx = p;
        set_modified(1);
        return;
    }
    if (cy == 0) return;
    {
        int plen = (int)strlen(lines[cy - 1]);
        int room = LINE_CAP - 1 - plen;
        strncat(lines[cy - 1], l, (size_t)room);
        memmove(&lines[cy], &lines[cy + 1],
                (size_t)(line_count - cy - 1) * LINE_CAP);
        line_count--;
        cy--;
        cx = plen;
        set_modified(1);
    }
}

static void delete_forward(void) {
    int len = (int)strlen(lines[cy]);
    if (cx < len) {
        cx = next_char(lines[cy], cx);
        backspace();
    } else if (cy < line_count - 1) {
        cy++;
        cx = 0;
        backspace();
    }
}

/* ── Files ────────────────────────────────────────────────────────────── */

static void new_buffer(void) {
    line_count = 1;
    lines[0][0] = 0;
    cx = cy = view = 0;
    sel_on = 0;
    strcpy(path, "/untitled.txt");
    modified = 0;
    update_title();
    set_status("New file");
}

static int load_file(const char *p) {
    int fd = open(p, O_RDONLY);
    static char buf[256 * 1024];
    int n, total = 0, col = 0;
    char msg[200];

    if (fd < 0) return -1;
    strncpy(path, p, sizeof(path) - 1);
    path[sizeof(path) - 1] = 0;
    while (total < (int)sizeof(buf) - 1 &&
           (n = (int)read(fd, buf + total, (size_t)((int)sizeof(buf) - 1 - total))) > 0)
        total += n;
    close(fd);
    buf[total] = 0;

    line_count = 0;
    col = 0;
    lines[0][0] = 0;
    for (int i = 0; i < total && line_count < MAX_LINES - 1; i++) {
        char c = buf[i];
        if (c == '\n') {
            lines[line_count][col] = 0;
            line_count++;
            col = 0;
            continue;
        }
        if (c == '\r') continue;
        if (c == '\t') {
            for (int k = 0; k < 4 && col < LINE_CAP - 2; k++)
                lines[line_count][col++] = ' ';
            continue;
        }
        if (col < LINE_CAP - 2)
            lines[line_count][col++] = c;
    }
    lines[line_count][col] = 0;
    /* A trailing newline does not make an extra empty line. */
    if (col > 0 || line_count == 0) line_count++;
    cx = cy = view = 0;
    sel_on = 0;
    modified = 0;
    update_title();
    snprintf(msg, sizeof(msg), "Opened %s (%d lines)", path, line_count);
    set_status(msg);
    gui_trace("edit", "opened %s lines=%d", path, line_count);
    return 0;
}

static int save_file(void) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    char msg[200];
    int bytes = 0;

    if (fd < 0) {
        snprintf(msg, sizeof(msg), "Cannot save %s", path);
        set_status(msg);
        gui_trace("edit", "save failed %s", path);
        return -1;
    }
    for (int i = 0; i < line_count; i++) {
        int n = (int)strlen(lines[i]);
        if (i == line_count - 1 && n == 0) break;   /* no extra blank line */
        write(fd, lines[i], (size_t)n);
        write(fd, "\n", 1);
        bytes += n + 1;
    }
    close(fd);
    set_modified(0);
    snprintf(msg, sizeof(msg), "Saved %s (%d bytes)", path, bytes);
    set_status(msg);
    gui_trace("edit", "saved %s bytes=%d", path, bytes);
    return 0;
}

/* ── Find ─────────────────────────────────────────────────────────────── */

static void find_next(void) {
    int y = cy, x = has_selection() ? (sel_y == cy && sel_x > cx ? sel_x : cx)
                                    : cx;
    int flen = (int)strlen(find_text);
    char msg[160];

    if (!flen) return;
    for (int pass = 0; pass <= line_count; pass++) {
        char *hit = strstr(lines[y] + x, find_text);
        if (hit) {
            sel_on = 1;
            sel_y = cy = y;
            sel_x = (int)(hit - lines[y]);
            cx = sel_x + flen;
            snprintf(msg, sizeof(msg), "Found \"%s\" at line %d", find_text,
                     y + 1);
            set_status(msg);
            gui_trace("edit", "found %s at %d:%d", find_text, y + 1, sel_x);
            return;
        }
        y = (y + 1) % line_count;
        x = 0;
    }
    snprintf(msg, sizeof(msg), "\"%s\" not found", find_text);
    set_status(msg);
    gui_trace("edit", "not found %s", find_text);
}

/* ── Prompts ──────────────────────────────────────────────────────────── */

static void open_prompt(int kind, const char *initial) {
    prompt = kind;
    strncpy(prompt_text, initial ? initial : "", sizeof(prompt_text) - 1);
    prompt_text[sizeof(prompt_text) - 1] = 0;
    prompt_len = (int)strlen(prompt_text);
    dirty = 1;
    gui_trace("edit", "prompt %s",
              kind == P_OPEN ? "open" : kind == P_SAVEAS ? "saveas" :
              kind == P_FIND ? "find" : "unsaved");
}

static void do_after(int what) {
    if (what == AFTER_CLOSE) gui.closed = 1;
    else if (what == AFTER_NEW) new_buffer();
    else if (what == AFTER_OPEN) {
        if (load_file(open_target) < 0) {
            char msg[200];
            snprintf(msg, sizeof(msg), "Cannot open %s", open_target);
            set_status(msg);
        }
    }
}

/* Run `what` now, or ask first when there are unsaved changes. */
static void guarded(int what) {
    if (!modified) { do_after(what); return; }
    after_confirm = what;
    open_prompt(P_CONFIRM, "");
}

static void prompt_accept(void) {
    int kind = prompt;

    prompt = P_NONE;
    dirty = 1;
    if (kind == P_OPEN) {
        if (!prompt_len) return;
        strncpy(open_target, prompt_text, sizeof(open_target) - 1);
        open_target[sizeof(open_target) - 1] = 0;
        guarded(AFTER_OPEN);
    } else if (kind == P_SAVEAS) {
        if (!prompt_len) return;
        strncpy(path, prompt_text, sizeof(path) - 1);
        path[sizeof(path) - 1] = 0;
        save_file();
        update_title();
    } else if (kind == P_FIND) {
        strncpy(find_text, prompt_text, sizeof(find_text) - 1);
        find_text[sizeof(find_text) - 1] = 0;
        find_next();
    }
}

static void prompt_key(int code, int ascii) {
    if (prompt == P_CONFIRM) {
        if (ascii == 'y' || ascii == 'Y' || code == KEY_ENTER) {
            prompt = P_NONE;
            if (save_file() == 0) do_after(after_confirm);
        } else if (ascii == 'n' || ascii == 'N') {
            prompt = P_NONE;
            set_modified(0);
            gui_trace("edit", "discarded changes");
            do_after(after_confirm);
        } else if (code == KEY_ESC || ascii == 'c' || ascii == 'C') {
            prompt = P_NONE;
            set_status("Cancelled");
        }
        dirty = 1;
        return;
    }
    if (code == KEY_ESC) { prompt = P_NONE; dirty = 1; return; }
    if (code == KEY_ENTER) { prompt_accept(); return; }
    if (code == KEY_BACKSPACE) {
        if (prompt_len) {
            prompt_len = prev_char(prompt_text, prompt_len);
            prompt_text[prompt_len] = 0;
        }
        dirty = 1;
        return;
    }
    if (ascii == 22) {                         /* Ctrl+V into the prompt */
        char clip[160];
        int n = gui_clipboard_get(clip, sizeof(clip));
        for (int i = 0; i < n && clip[i] != '\n' &&
             prompt_len + 1 < (int)sizeof(prompt_text); i++)
            prompt_text[prompt_len++] = clip[i];
        prompt_text[prompt_len] = 0;
        dirty = 1;
        return;
    }
    if (ascii >= 32 && ascii != 127) {
        char u[4];
        int n = draw_utf8_encode((unsigned)ascii, u);
        if (prompt_len + n < (int)sizeof(prompt_text)) {
            memcpy(prompt_text + prompt_len, u, (size_t)n);
            prompt_len += n;
            prompt_text[prompt_len] = 0;
        }
        dirty = 1;
    }
}

/* ── Rendering ────────────────────────────────────────────────────────── */

static int text_rows(void) {
    int r = (gui.surf.h - TEXT_Y - STATUS_H - 4) / ROW_H;
    return r < 1 ? 1 : r;
}

static void render(void) {
    draw_surface_t *s = &gui.surf;
    int rows, vis = 0, x;
    int sx0 = 0, sy0 = 0, sx1 = 0, sy1 = 0, sel = has_selection();
    uint32_t accent = draw_rgb(58, 121, 200);

    if (!s->px) return;
    rows = text_rows();
    if (cy < view) view = cy;
    if (cy >= view + rows) view = cy - rows + 1;
    if (sel) sel_range(&sx0, &sy0, &sx1, &sy1);

    draw_fill(s, draw_rgb(250, 250, 248));

    /* Toolbar */
    draw_rect(s, 0, 0, s->w, TOOLBAR_H, draw_rgb(236, 239, 243));
    draw_rect(s, 0, TOOLBAR_H - 1, s->w, 1, draw_rgb(208, 214, 222));
    x = 6;
    for (int i = 0; i < TB_COUNT; i++) {
        int w = draw_text_width(tb_labels[i], &draw_font_ui) + 20;
        tb_x[i] = x;
        tb_w[i] = w;
        draw_round_rect(s, x, 4, w, TOOLBAR_H - 9, 4, draw_rgb(252, 253, 254));
        draw_round_frame(s, x, 4, w, TOOLBAR_H - 9, 4, draw_rgb(190, 198, 208));
        draw_text_aa(s, x + 10, 5, tb_labels[i], draw_rgb(30, 36, 44),
                     &draw_font_ui);
        x += w + 6;
    }

    for (int i = view; i < line_count && vis < rows; i++, vis++) {
        int ry = TEXT_Y + vis * ROW_H;
        if (i == cy && !sel)
            draw_rect(s, 0, ry - 1, s->w, ROW_H, draw_rgb(238, 242, 246));
        if (sel && i >= sy0 && i <= sy1) {
            int a = i == sy0 ? col_px(lines[i], sx0) : TEXT_X;
            int b = i == sy1 ? col_px(lines[i], sx1)
                             : col_px(lines[i], (int)strlen(lines[i])) + 6;
            draw_rect(s, a, ry - 1, b - a, ROW_H, draw_rgb(184, 210, 240));
        }
        draw_text_aa(s, TEXT_X, ry, lines[i], draw_rgb(30, 34, 36),
                     &draw_font_ui);
        if (i == cy && !prompt)
            draw_rect(s, col_px(lines[i], cx), ry, 2, ROW_H - 3, accent);
    }

    /* Status bar / prompt */
    {
        int by = s->h - STATUS_H;
        char buf[256];
        draw_rect(s, 0, by, s->w, STATUS_H, draw_rgb(236, 239, 243));
        draw_rect(s, 0, by, s->w, 1, draw_rgb(208, 214, 222));
        if (prompt == P_CONFIRM) {
            draw_rect(s, 0, by, s->w, STATUS_H, draw_rgb(255, 236, 200));
            draw_text_aa(s, 8, by + 3,
                         "Unsaved changes. Save them?  [Y]es  [N]o  [Esc] Cancel",
                         draw_rgb(60, 40, 10), &draw_font_ui);
        } else if (prompt) {
            const char *lab = prompt == P_OPEN ? "Open file: " :
                              prompt == P_SAVEAS ? "Save as: " : "Find: ";
            int lw = draw_text_aa(s, 8, by + 3, lab, draw_rgb(60, 70, 90),
                                  &draw_font_ui);
            draw_rect(s, 8 + lw, by + 2, s->w - 16 - lw, STATUS_H - 4,
                      draw_rgb(255, 255, 255));
            draw_text_aa(s, 12 + lw, by + 3, prompt_text, draw_rgb(20, 24, 30),
                         &draw_font_ui);
            draw_rect(s, 12 + lw + draw_text_width(prompt_text, &draw_font_ui),
                      by + 4, 2, STATUS_H - 8, accent);
        } else {
            snprintf(buf, sizeof(buf), "Ln %d, Col %d   %s", cy + 1,
                     draw_utf8_len(lines[cy]) -
                     draw_utf8_len(lines[cy] + cx) + 1, status);
            draw_text_aa(s, 8, by + 3, buf, draw_rgb(90, 98, 110),
                         &draw_font_ui);
        }
    }
    wm_commit(&gui.wm, gui.slot);
    dirty = 0;
}

/* ── Input ────────────────────────────────────────────────────────────── */

static void copy_out(int cut) {
    static char buf[64 * 1024];
    int n = selection_text(buf, sizeof(buf));
    if (!n) return;
    gui_clipboard_set(&gui, buf, n);
    gui_trace("edit", "%s %d bytes", cut ? "cut" : "copied", n);
    if (cut) delete_selection();
}

static void paste_in(void) {
    static char buf[64 * 1024];
    int n = gui_clipboard_get(buf, sizeof(buf));
    if (!n) return;
    if (has_selection()) delete_selection();
    sel_on = 0;
    insert_text(buf);
    gui_trace("edit", "pasted %d bytes", n);
}

static void move_cursor(int code, int shift) {
    int rows = text_rows();

    if (shift && !sel_on) { sel_on = 1; sel_x = cx; sel_y = cy; }
    if (!shift && has_selection() && (code == KEY_LEFT || code == KEY_RIGHT)) {
        int x0, y0, x1, y1;
        sel_range(&x0, &y0, &x1, &y1);
        if (code == KEY_LEFT) { cx = x0; cy = y0; }
        else { cx = x1; cy = y1; }
        sel_on = 0;
        return;
    }
    if (!shift) sel_on = 0;
    switch (code) {
    case KEY_UP:    if (cy > 0) { int px = col_px(lines[cy], cx); cy--; cx = px_col(lines[cy], px); } break;
    case KEY_DOWN:  if (cy < line_count - 1) { int px = col_px(lines[cy], cx); cy++; cx = px_col(lines[cy], px); } break;
    case KEY_LEFT:
        if (cx > 0) cx = prev_char(lines[cy], cx);
        else if (cy > 0) { cy--; cx = (int)strlen(lines[cy]); }
        break;
    case KEY_RIGHT:
        if (cx < (int)strlen(lines[cy])) cx = next_char(lines[cy], cx);
        else if (cy < line_count - 1) { cy++; cx = 0; }
        break;
    case KEY_HOME:  cx = 0; break;
    case KEY_END:   cx = (int)strlen(lines[cy]); break;
    case KEY_PAGEUP:   cy -= rows; view -= rows; if (view < 0) view = 0; break;
    case KEY_PAGEDOWN: cy += rows; view += rows; break;
    }
    clamp_cursor();
}

static void on_key(gui_window_t *g, int code, int value, int ascii) {
    int shift = g->key_mods & WM_MOD_SHIFT;

    (void)value;
    dirty = 1;
    if (prompt) { prompt_key(code, ascii); return; }

    if (ascii == 19) {                          /* Ctrl+S / Ctrl+Shift+S */
        if (shift) open_prompt(P_SAVEAS, path);
        else save_file();
        return;
    }
    if (ascii == 15) { open_prompt(P_OPEN, path); return; }          /* ^O */
    if (ascii == 14) { guarded(AFTER_NEW); return; }                  /* ^N */
    if (ascii == 6)  { open_prompt(P_FIND, find_text); return; }      /* ^F */
    if (code == KEY_F3) { find_next(); return; }
    if (ascii == 1) {                                                 /* ^A */
        sel_on = 1; sel_x = 0; sel_y = 0;
        cy = line_count - 1; cx = (int)strlen(lines[cy]);
        return;
    }
    if (ascii == 3)  { copy_out(0); return; }                         /* ^C */
    if (ascii == 24) { copy_out(1); return; }                         /* ^X */
    if (ascii == 22) { paste_in(); return; }                          /* ^V */
    if (code == KEY_ESC) { sel_on = 0; return; }

    switch (code) {
    case KEY_UP: case KEY_DOWN: case KEY_LEFT: case KEY_RIGHT:
    case KEY_HOME: case KEY_END: case KEY_PAGEUP: case KEY_PAGEDOWN:
        move_cursor(code, shift);
        return;
    case KEY_ENTER:
        delete_selection();
        newline();
        return;
    case KEY_BACKSPACE:
        if (has_selection()) delete_selection(); else backspace();
        sel_on = 0;
        return;
    case KEY_DELETE:
        if (has_selection()) delete_selection(); else delete_forward();
        sel_on = 0;
        return;
    case KEY_TAB:
        delete_selection();
        insert_bytes("    ", 4);
        return;
    }
    if (ascii >= 32 && ascii != 127) {
        char u[4];
        int n = draw_utf8_encode((unsigned)ascii, u);
        delete_selection();
        insert_bytes(u, n);
    }
}

static void toolbar_click(int i) {
    switch (i) {
    case TB_NEW:    guarded(AFTER_NEW); break;
    case TB_OPEN:   open_prompt(P_OPEN, path); break;
    case TB_SAVE:   save_file(); break;
    case TB_SAVEAS: open_prompt(P_SAVEAS, path); break;
    case TB_FIND:   open_prompt(P_FIND, find_text); break;
    }
}

static void on_mouse(gui_window_t *g, int x, int y, int buttons) {
    int pressed = buttons & 1;

    (void)g;
    dirty = 1;
    if (!pressed) { mouse_selecting = 0; return; }
    if (!mouse_selecting && y < TOOLBAR_H) {
        for (int i = 0; i < TB_COUNT; i++)
            if (x >= tb_x[i] && x < tb_x[i] + tb_w[i]) toolbar_click(i);
        return;
    }
    if (prompt == P_CONFIRM) return;
    if (y < TEXT_Y) y = TEXT_Y;
    cy = view + (y - TEXT_Y) / ROW_H;
    if (cy >= line_count) cy = line_count - 1;
    clamp_cursor();
    cx = px_col(lines[cy], x);
    if (!mouse_selecting) {
        mouse_selecting = 1;
        sel_on = 1;
        sel_x = cx;
        sel_y = cy;
    }
}

static void on_scroll(gui_window_t *g, int delta) {
    (void)g;
    view -= delta * 3;
    if (view > line_count - 1) view = line_count - 1;
    if (view < 0) view = 0;
    /* Keep the cursor inside the view so render() does not snap back. */
    if (cy < view) cy = view;
    if (cy >= view + text_rows()) cy = view + text_rows() - 1;
    clamp_cursor();
    dirty = 1;
}

static void trace_ready(void) {
    gui_trace("edit", "ready path=%s save=%d,%d saveas=%d,%d text=%d,%d",
              path, GUI_BODY_X + tb_x[TB_SAVE] + tb_w[TB_SAVE] / 2,
              GUI_BODY_Y + TOOLBAR_H / 2,
              GUI_BODY_X + tb_x[TB_SAVEAS] + tb_w[TB_SAVEAS] / 2,
              GUI_BODY_Y + TOOLBAR_H / 2,
              GUI_BODY_X + TEXT_X + 4, GUI_BODY_Y + TEXT_Y + ROW_H / 2);
}

int main(int argc, char *argv[]) {
    int slot = (argc > 1) ? atoi(argv[1]) : 1;

    if (slot < 1 || slot > WM_MAX_SLOTS) slot = 1;

    if (gui_open(&gui, slot, "Editor", 160 + slot * 14, 60 + slot * 10,
                 640, 460) < 0) {
        printf("edit: desktop unavailable\n");
        return 1;
    }
    gui_set_key_handler(&gui, on_key);
    gui_set_mouse_handler(&gui, on_mouse);
    gui_set_scroll_handler(&gui, on_scroll);
    gui_grab_escape(&gui);           /* Esc cancels prompts / selection */

    if (argc > 2) {
        if (load_file(argv[2]) < 0) {
            strncpy(path, argv[2], sizeof(path) - 1);
            path[sizeof(path) - 1] = 0;
            update_title();
            set_status("New file");
        }
    } else {
        update_title();
    }
    render();
    trace_ready();
    while (1) {
        int events = gui_poll(&gui);
        if (gui.closed) {
            if (!modified) break;
            /* The desktop hid the window: bring it back and ask. */
            gui.closed = 0;
            wm_app(&gui.wm, gui.slot, "Editor");
            wm_focus_app(&gui.wm, gui.slot);
            update_title();
            after_confirm = AFTER_CLOSE;
            open_prompt(P_CONFIRM, "");
            events++;
        }
        if (dirty || events > 0)
            render();
        if (gui.closed) break;       /* a confirmed close */
        sleep_ms(30);
    }
    gui_close(&gui);
    return 0;
}
