#include <dirent.h>
#include <fcntl.h>
#include <gui.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/*
 * files — the file manager.
 *
 * Single click selects, double click (or Enter) opens: directories are
 * entered, files go to the app for their type (edit, view) or, failing
 * that, the built-in text viewer.  The toolbar makes folders, renames,
 * deletes (after a confirmation), and copies/cuts/pastes through the
 * desktop clipboard: a copied file is its absolute path there, so a path
 * copied in the editor can be pasted here too.  Hidden (dot) files show
 * only when "Hidden" is ticked.
 *
 * Keys: Enter open, Backspace up, Delete delete, F2 rename, F5 refresh,
 * Ctrl+C/X/V copy/cut/paste, Ctrl+N new folder, Up/Down move the selection.
 * Starts in argv[2] when given (wmctl launch files /some/dir).
 */

#define FILES_MAX_ENTRIES 256
#define FILES_VISIBLE_ROWS 14
#define NAME_MAX_LEN 128
#define VIEW_MAX_BYTES 4096

/* Widget layout (fixed indices for the labels and rows) */
#define IDX_PANEL    0
#define IDX_PATH     1
#define IDX_STATUS   2
#define IDX_PROMPT   3
#define IDX_ROW0     4

#define ROW_START_Y 70
#define ROW_H 18
#define TOOLBAR_Y 6

/* Button / widget ids */
#define BTN_UP      1
#define BTN_PREV    2
#define BTN_NEXT    3
#define BTN_REFRESH 4
#define BTN_NEWDIR  5
#define BTN_RENAME  6
#define BTN_DELETE  7
#define BTN_COPY    8
#define BTN_CUT     9
#define BTN_PASTE   10
#define CHK_HIDDEN  11
#define BTN_OK      12
#define BTN_CANCEL  13
#define TXT_NAME    14
#define BTN_ROW_BASE 1000
#define IMG_ROW_BASE 2000

/* WM icon indexes */
#define ICON_FOLDER 0
#define ICON_FILE   1
#define ICON_DEV    2

static const char *const folder_icon[16] = {
    "0000000000000000",
    "0aaaaaa000000000",
    "a777777a00000000",
    "aaaaaaaaaaaaaaa0",
    "a7777777777777a0",
    "a7777777777777a0",
    "a7777777777777a0",
    "a7777777777777a0",
    "a7777777777777a0",
    "a7777777777777a0",
    "a7777777777777a0",
    "a7777777777777a0",
    "a7777777777777a0",
    "aaaaaaaaaaaaaaa0",
    "0000000000000000",
    "0000000000000000",
};

static const char *const file_icon[16] = {
    "0000000000000000",
    "0fffffffffff0000",
    "0f2222222222f000",
    "0f2eeeeeee22f000",
    "0f2222222222f000",
    "0f2eeeeeee22f000",
    "0f2222222222f000",
    "0f2eeeeeee22f000",
    "0f2222222222f000",
    "0f2eeeeeee22f000",
    "0f2222222222f000",
    "0f2eeeee2222f000",
    "0f2222222222f000",
    "0fffffffffff0000",
    "0000000000000000",
    "0000000000000000",
};

static const char *const dev_icon[16] = {
    "0000000000000000",
    "0009000900090000",
    "0099999999999000",
    "0099999999999000",
    "0099aaaaaaa99000",
    "0099a2222aa99000",
    "0099a2222aa99000",
    "0099a2222aa99000",
    "0099aaaaaaa99000",
    "0099999999999000",
    "0099999999999000",
    "0099999999999000",
    "0009000900090000",
    "0000000000000000",
    "0000000000000000",
    "0000000000000000",
};

enum files_mode { MODE_BROWSE, MODE_VIEW };
enum prompt_kind { PR_NONE, PR_NEWDIR, PR_RENAME, PR_DELETE };

typedef struct {
    char name[NAME_MAX_LEN];
    char kind;                /* D dir, F file, C device, P fifo, L link, 0 */
} entry_t;

typedef struct {
    gui_window_t gui;
    int mode;
    char path[256];
    char view_name[NAME_MAX_LEN];
    entry_t ents[FILES_MAX_ENTRIES];
    char view_lines[FILES_MAX_ENTRIES][GUI_MAX_LABEL];
    int entry_count;
    int scroll;
    int sel;        /* selected row (-1 = none); double-click opens */
    long sel_ms;    /* timestamp of the selecting click */
    int show_hidden;
    int prompt;
    char target[NAME_MAX_LEN];     /* entry a rename/delete applies to */
    int cut_pending;               /* the clipboard path is a cut */
    char cut_path[256];
    int generation;                /* bumps on every reload (for traces) */
} files_app_t;

static files_app_t app;

static void sleep_ms(long ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, 0);
}

static long now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, 0);
    return tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static void set_label(gui_window_t *gui, int widget_index, const char *text) {
    if (!gui || widget_index < 0 || widget_index >= gui->widget_count) return;
    strncpy(gui->widgets[widget_index].text, text, GUI_MAX_LABEL - 1);
    gui->widgets[widget_index].text[GUI_MAX_LABEL - 1] = 0;
}

static void set_row_color(gui_window_t *gui, int widget_index, const char *color) {
    if (!gui || widget_index < 0 || widget_index >= gui->widget_count) return;
    strncpy(gui->widgets[widget_index].text_color, color, sizeof(gui->widgets[0].text_color) - 1);
}

static void status(const char *fmt, const char *a) {
    char msg[GUI_MAX_LABEL];
    snprintf(msg, sizeof(msg), fmt, a);
    set_label(&app.gui, IDX_STATUS, msg);
}

static void join_path(char *out, int out_size, const char *dir, const char *name) {
    if (!out || out_size <= 0) return;
    if (!strcmp(dir, "/"))
        snprintf(out, out_size, "/%s", name);
    else
        snprintf(out, out_size, "%s/%s", dir, name);
}

static int files_button_y(const gui_window_t *gui) {
    int y;
    if (!gui) return 250;
    y = gui_body_height(gui) - 42;
    return y < 250 ? 250 : y;
}

static int files_row_capacity(const gui_window_t *gui) {
    int button_y = files_button_y(gui);
    int rows = (button_y - ROW_START_Y - 8) / ROW_H;
    if (rows < 3) rows = 3;
    if (rows > FILES_VISIBLE_ROWS) rows = FILES_VISIBLE_ROWS;
    return rows;
}

static void clamp_scroll(files_app_t *state) {
    int rows = files_row_capacity(&state->gui);
    if (state->scroll > state->entry_count - rows)
        state->scroll = state->entry_count - rows;
    if (state->scroll < 0) state->scroll = 0;
}

static char kind_of(const char *dir, const struct dirent *ent) {
    char path[384];
    struct stat st;

    join_path(path, sizeof(path), dir, ent->d_name);
    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode))  return 'D';
        if (S_ISCHR(st.st_mode) || S_ISBLK(st.st_mode)) return 'C';
        if (S_ISFIFO(st.st_mode)) return 'P';
        if (S_ISLNK(st.st_mode))  return 'L';
        if (S_ISREG(st.st_mode))  return 'F';
    }
    switch (ent->d_type) {
    case DT_DIR:  return 'D';
    case DT_CHR:  return 'C';
    case DT_FIFO: return 'P';
    case DT_LNK:  return 'L';
    case DT_REG:  return 'F';
    }
    return '?';
}

static void add_entry(files_app_t *s, char kind, const char *name) {
    if (s->entry_count >= FILES_MAX_ENTRIES) return;
    strncpy(s->ents[s->entry_count].name, name, NAME_MAX_LEN - 1);
    s->ents[s->entry_count].name[NAME_MAX_LEN - 1] = 0;
    s->ents[s->entry_count].kind = kind;
    s->entry_count++;
}

/* Map an entry kind to its WM icon (-1 = none). */
static int icon_for_kind(char kind) {
    switch (kind) {
    case 'D': return ICON_FOLDER;
    case 'F': case 'L': return ICON_FILE;
    case 'C': case 'P': return ICON_DEV;
    }
    return -1;
}

/* ".." first, then folders, then everything else; names case-insensitively. */
static int entry_before(const entry_t *a, const entry_t *b) {
    int da = a->kind == 'D', db = b->kind == 'D';
    if (!strcmp(a->name, "..")) return 1;
    if (!strcmp(b->name, "..")) return 0;
    if (da != db) return da;
    for (const char *p = a->name, *q = b->name; ; p++, q++) {
        int x = (*p >= 'A' && *p <= 'Z') ? *p + 32 : (unsigned char)*p;
        int y = (*q >= 'A' && *q <= 'Z') ? *q + 32 : (unsigned char)*q;
        if (x != y) return x < y;
        if (!x) return 0;
    }
}

static void sort_entries(files_app_t *s) {
    for (int i = 1; i < s->entry_count; i++) {
        entry_t e = s->ents[i];
        int j = i - 1;
        while (j >= 0 && entry_before(&e, &s->ents[j])) {
            s->ents[j + 1] = s->ents[j];
            j--;
        }
        s->ents[j + 1] = e;
    }
}

static int find_entry(files_app_t *s, const char *name) {
    for (int i = 0; i < s->entry_count; i++)
        if (s->ents[i].kind && !strcmp(s->ents[i].name, name)) return i;
    return -1;
}

static void load_entries(files_app_t *state) {
    DIR *dir;
    struct dirent *ent;

    state->entry_count = 0;
    state->mode = MODE_BROWSE;
    state->sel = -1;
    state->generation++;
    dir = opendir(state->path);
    if (!dir) {
        char msg[GUI_MAX_LABEL];
        snprintf(msg, sizeof(msg), "cannot open %s", state->path);
        add_entry(state, 0, msg);
        state->scroll = 0;
        return;
    }
    /* ".." is ours: not every filesystem lists it. */
    if (strcmp(state->path, "/")) add_entry(state, 'D', "..");
    while ((ent = readdir(dir)) != 0 && state->entry_count < FILES_MAX_ENTRIES) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        if (ent->d_name[0] == '.' && !state->show_hidden) continue;
        add_entry(state, kind_of(state->path, ent), ent->d_name);
    }
    closedir(dir);
    sort_entries(state);
    if (state->entry_count == 0)
        add_entry(state, 0, "(empty)");
    clamp_scroll(state);
}

static void push_view_line(files_app_t *s, char *line, int *col) {
    if (s->entry_count >= FILES_MAX_ENTRIES) return;
    line[*col] = 0;
    strncpy(s->view_lines[s->entry_count], line, GUI_MAX_LABEL - 1);
    s->view_lines[s->entry_count][GUI_MAX_LABEL - 1] = 0;
    s->ents[s->entry_count].name[0] = 0;    /* viewer rows are not actionable */
    s->ents[s->entry_count].kind = 0;
    s->entry_count++;
    *col = 0;
}

/* Load up to VIEW_MAX_BYTES of a file, wrapped into display lines. */
static void load_view(files_app_t *s, const char *fullpath) {
    static char buf[VIEW_MAX_BYTES + 1];
    char line[GUI_MAX_LABEL];
    int col = 0;
    int fd, total = 0, n;

    s->entry_count = 0;
    s->scroll = 0;
    fd = open(fullpath, O_RDONLY);
    if (fd < 0) {
        strcpy(line, "cannot open file");
        col = (int)strlen(line);
        push_view_line(s, line, &col);
        return;
    }
    while (total < VIEW_MAX_BYTES &&
           (n = (int)read(fd, buf + total, VIEW_MAX_BYTES - total)) > 0)
        total += n;
    close(fd);

    for (int i = 0; i < total && s->entry_count < FILES_MAX_ENTRIES; i++) {
        char c = buf[i];
        if (c == '\r') continue;
        if (c == '\n') { push_view_line(s, line, &col); continue; }
        if (c == '\t') c = ' ';
        if ((c < 32 && c >= 0) || c == 127) c = '.';
        line[col++] = c;
        if (col >= GUI_MAX_LABEL - 1) push_view_line(s, line, &col);
    }
    if (col > 0 || s->entry_count == 0) push_view_line(s, line, &col);
}

static void refresh_rows(files_app_t *state) {
    char line[GUI_MAX_LABEL + 8];
    int rows = files_row_capacity(&state->gui);

    clamp_scroll(state);
    if (state->mode == MODE_VIEW)
        snprintf(line, GUI_MAX_LABEL, "VIEW %s", state->view_name);
    else
        snprintf(line, GUI_MAX_LABEL, "%s", state->path);
    set_label(&state->gui, IDX_PATH, line);

    for (int i = 0; i < FILES_VISIBLE_ROWS; i++) {
        int idx = state->scroll + i;
        gui_widget_t *img = gui_find(&state->gui, IMG_ROW_BASE + i);
        gui_widget_t *rowbtn = gui_find(&state->gui, BTN_ROW_BASE + i);
        if (i < rows && idx < state->entry_count) {
            int selected = state->mode == MODE_BROWSE && idx == state->sel;
            const char *col = "black";
            if (state->mode == MODE_VIEW) {
                set_label(&state->gui, IDX_ROW0 + i, state->view_lines[idx]);
            } else {
                /* Three leading spaces clear the 16px row icon. */
                snprintf(line, GUI_MAX_LABEL, "   %s", state->ents[idx].name);
                set_label(&state->gui, IDX_ROW0 + i, line);
                if (selected) col = "white";
                else if (state->ents[idx].kind == 'D') col = "blue";
                else if (state->ents[idx].kind == 'C') col = "red";
                else if (state->ents[idx].kind == 'L') col = "green";
            }
            set_row_color(&state->gui, IDX_ROW0 + i, col);
            if (rowbtn)
                strncpy(rowbtn->color, selected ? "#5e81ac" : "#eef3f8",
                        sizeof(rowbtn->color) - 1);
            if (img)
                img->value = state->mode == MODE_BROWSE ?
                             icon_for_kind(state->ents[idx].kind) : -1;
        } else {
            set_label(&state->gui, IDX_ROW0 + i, "");
            if (rowbtn)
                strncpy(rowbtn->color, "#eef3f8", sizeof(rowbtn->color) - 1);
            if (img) img->value = -1;
        }
    }
}

/* Window-relative centre of a widget (what the smoke test clicks). */
static void widget_point(gui_widget_t *w, int *x, int *y) {
    *x = w ? GUI_BODY_X + w->x + w->w / 2 : -1;
    *y = w ? GUI_BODY_Y + w->y + w->h / 2 : -1;
}

/* On each new listing: trace it and where its rows are, so
 * tools/smoke_gui.py can click them (see gui_trace). */
static void trace_listing(files_app_t *s) {
    static int traced = -1;
    int rows = files_row_capacity(&s->gui);
    int dir_traced = 0;

    if (s->mode != MODE_BROWSE || traced == s->generation) return;
    traced = s->generation;
    gui_trace("files", "cwd %s entries=%d hidden=%d", s->path, s->entry_count,
              s->show_hidden);
    for (int i = 0; i < rows && s->scroll + i < s->entry_count; i++) {
        int idx = s->scroll + i;
        gui_widget_t *w = &s->gui.widgets[IDX_ROW0 + i];
        int x = GUI_BODY_X + w->x + 40, y = GUI_BODY_Y + w->y + w->h / 2;
        if (!s->ents[idx].kind) continue;
        gui_trace("files", "row %s at %d,%d", s->ents[idx].name, x, y);
        if (s->ents[idx].kind != 'D' || !strcmp(s->ents[idx].name, "..") ||
            dir_traced)
            continue;
        gui_trace("files", "dir %s at %d,%d", s->ents[idx].name, x, y);
        dir_traced = 1;
    }
}

static void place(gui_window_t *gui, int id, int x, int y, int w, int h) {
    gui_widget_t *b = gui_find(gui, id);
    if (!b) return;
    b->x = x; b->y = y;
    if (w >= 0) b->w = w;
    if (h >= 0) b->h = h;
}

static void layout_files(gui_window_t *gui) {
    int body_w = gui_body_width(gui);
    int panel_w = body_w - 20;
    int y = files_button_y(gui);
    int panel_h = y - 8 - 40;
    int x;

    if (!gui) return;
    if (panel_w < 280) panel_w = 280;
    if (panel_h < 140) panel_h = 140;

    gui->widgets[IDX_PANEL].x = 10;
    gui->widgets[IDX_PANEL].y = 40;
    gui->widgets[IDX_PANEL].w = panel_w;
    gui->widgets[IDX_PANEL].h = panel_h;
    gui->widgets[IDX_PATH].x = 18;
    gui->widgets[IDX_PATH].y = 46;

    for (int i = 0; i < FILES_VISIBLE_ROWS; i++) {
        gui_widget_t *w = &gui->widgets[IDX_ROW0 + i];
        gui_widget_t *img = gui_find(gui, IMG_ROW_BASE + i);
        w->x = 14;
        w->y = ROW_START_Y + i * ROW_H;
        w->w = panel_w - 8;
        w->h = ROW_H - 2;
        if (img) {
            img->x = 16;
            img->y = ROW_START_Y + i * ROW_H;
        }
    }

    /* Toolbar */
    x = 10;
    place(gui, BTN_NEWDIR, x, TOOLBAR_Y, 92, 26);  x += 96;
    place(gui, BTN_RENAME, x, TOOLBAR_Y, 70, 26);  x += 74;
    place(gui, BTN_DELETE, x, TOOLBAR_Y, 64, 26);  x += 68;
    place(gui, BTN_COPY, x, TOOLBAR_Y, 54, 26);    x += 58;
    place(gui, BTN_CUT, x, TOOLBAR_Y, 46, 26);     x += 50;
    place(gui, BTN_PASTE, x, TOOLBAR_Y, 56, 26);   x += 64;
    place(gui, CHK_HIDDEN, x, TOOLBAR_Y + 4, -1, -1);

    /* Bottom bar: navigation, or the name / confirm prompt. */
    if (app.prompt == PR_NONE) {
        place(gui, BTN_UP, 14, y, 78, 32);
        place(gui, BTN_PREV, 98, y, 88, 32);
        place(gui, BTN_NEXT, 192, y, 88, 32);
        place(gui, BTN_REFRESH, 286, y, 108, 32);
        gui->widgets[IDX_STATUS].x = 404;
        gui->widgets[IDX_STATUS].y = y + 8;
        gui->widgets[IDX_PROMPT].x = -1000;
        place(gui, TXT_NAME, -1000, y, -1, -1);
        place(gui, BTN_OK, -1000, y, -1, -1);
        place(gui, BTN_CANCEL, -1000, y, -1, -1);
    } else {
        int lw = app.prompt == PR_DELETE ? 0 : 60;
        place(gui, BTN_UP, -1000, y, -1, -1);
        place(gui, BTN_PREV, -1000, y, -1, -1);
        place(gui, BTN_NEXT, -1000, y, -1, -1);
        place(gui, BTN_REFRESH, -1000, y, -1, -1);
        gui->widgets[IDX_STATUS].x = -1000;
        gui->widgets[IDX_PROMPT].x = 14;
        gui->widgets[IDX_PROMPT].y = y + 8;
        if (app.prompt == PR_DELETE)
            place(gui, TXT_NAME, -1000, y, -1, -1);
        else
            place(gui, TXT_NAME, 14 + lw, y + 4, body_w - 200 - lw, 24);
        place(gui, BTN_OK, body_w - 180, y, 80, 32);
        place(gui, BTN_CANCEL, body_w - 94, y, 80, 32);
    }

    refresh_rows(&app);
}

static void go_parent(files_app_t *s) {
    int n = (int)strlen(s->path);
    int slash = -1;
    if (n <= 1) return;                       /* already at root */
    if (s->path[n - 1] == '/') s->path[--n] = 0;
    for (int i = n - 1; i >= 0; i--)
        if (s->path[i] == '/') { slash = i; break; }
    if (slash <= 0) strcpy(s->path, "/");
    else s->path[slash] = 0;
}

static void enter_dir(files_app_t *s, const char *name) {
    if (!strcmp(name, "..")) {
        go_parent(s);
    } else {
        char np[384];
        join_path(np, sizeof(np), s->path, name);
        strncpy(s->path, np, sizeof(s->path) - 1);
        s->path[sizeof(s->path) - 1] = 0;
    }
    s->scroll = 0;
    load_entries(s);
}

static int ends_with_ci(const char *str, const char *suf) {
    int sl = (int)strlen(str), fl = (int)strlen(suf);
    if (sl < fl) return 0;
    for (int i = 0; i < fl; i++) {
        char a = str[sl - fl + i], b = suf[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (a != b) return 0;
    }
    return 1;
}

/* Text unless the first block holds NUL bytes (or it is an ELF binary). */
static int looks_like_text(const char *full) {
    char buf[512];
    int fd = open(full, O_RDONLY), n;
    if (fd < 0) return 0;
    n = (int)read(fd, buf, sizeof(buf));
    close(fd);
    if (n <= 0) return 1;                     /* empty: edit it */
    if (n >= 4 && !memcmp(buf, "\177ELF", 4)) return 0;
    for (int i = 0; i < n; i++)
        if (!buf[i]) return 0;
    return 1;
}

/* Open a file with the app for its type via the WM launch command. */
static int open_with_app(files_app_t *s, const char *full) {
    static const char *const images[] = {
        ".ppm", ".bmp", ".png", ".jpg", ".jpeg", ".gif", ".mic", 0,
    };
    const char *app_name = 0;

    for (int i = 0; images[i]; i++)
        if (ends_with_ci(full, images[i])) app_name = "view";
    if (!app_name && looks_like_text(full)) app_name = "edit";
    if (!app_name) return 0;
    wm_command(&s->gui.wm, "launch %s %s", app_name, full);
    gui_trace("files", "open %s with %s", full, app_name);
    return 1;
}

static void open_view(files_app_t *s, const char *name) {
    char full[384];
    join_path(full, sizeof(full), s->path, name);
    if (open_with_app(s, full)) return;   /* handled by an app */
    strncpy(s->view_name, name, sizeof(s->view_name) - 1);
    s->view_name[sizeof(s->view_name) - 1] = 0;
    load_view(s, full);
    s->mode = MODE_VIEW;
    set_label(&s->gui, IDX_ROW0, s->view_lines[0]);
    {
        gui_widget_t *up = gui_find(&s->gui, BTN_UP);
        if (up) strcpy(up->text, "BACK");
    }
}

static void exit_view(files_app_t *s) {
    gui_widget_t *up = gui_find(&s->gui, BTN_UP);
    s->mode = MODE_BROWSE;
    if (up) strcpy(up->text, "UP");
    s->scroll = 0;
    load_entries(s);
}

static void open_entry(files_app_t *s, int idx) {
    if (idx < 0 || idx >= s->entry_count || !s->ents[idx].kind) return;
    if (s->ents[idx].kind == 'D') enter_dir(s, s->ents[idx].name);
    else if (s->ents[idx].kind == 'F' || s->ents[idx].kind == 'L')
        open_view(s, s->ents[idx].name);
    /* Devices/FIFOs stay unopened: reading them can block. */
}

/* ── File operations ─────────────────────────────────────────────────── */

static int copy_file(const char *src, const char *dst) {
    static char buf[16384];
    struct stat st;
    int in, out, n, rc = 0;

    if (stat(src, &st) < 0) return -1;
    in = open(src, O_RDONLY);
    if (in < 0) return -1;
    out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, (int)(st.st_mode & 0777));
    if (out < 0) { close(in); return -1; }
    while ((n = (int)read(in, buf, sizeof(buf))) > 0)
        if ((int)write(out, buf, (size_t)n) != n) { rc = -1; break; }
    if (n < 0) rc = -1;
    close(in);
    close(out);
    return rc;
}

static int copy_tree(const char *src, const char *dst, int depth) {
    struct stat st;
    DIR *d;
    struct dirent *ent;
    int rc = 0;

    if (depth > 16 || stat(src, &st) < 0) return -1;
    if (!S_ISDIR(st.st_mode)) return copy_file(src, dst);
    if (mkdir(dst, (int)(st.st_mode & 0777)) < 0) return -1;
    d = opendir(src);
    if (!d) return -1;
    while ((ent = readdir(d)) != 0) {
        char a[384], b[384];
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        join_path(a, sizeof(a), src, ent->d_name);
        join_path(b, sizeof(b), dst, ent->d_name);
        if (copy_tree(a, b, depth + 1) < 0) rc = -1;
    }
    closedir(d);
    return rc;
}

static int remove_tree(const char *p, int depth) {
    struct stat st;
    DIR *d;
    struct dirent *ent;

    if (depth > 16 || lstat(p, &st) < 0) return -1;
    if (!S_ISDIR(st.st_mode)) return unlink(p);
    d = opendir(p);
    if (!d) return -1;
    while ((ent = readdir(d)) != 0) {
        char c[384];
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        join_path(c, sizeof(c), p, ent->d_name);
        remove_tree(c, depth + 1);
    }
    closedir(d);
    return rmdir(p);
}

/* dir/name, or "name (copy)" / "name (copy 2)" ... when that exists. */
static void unique_dest(char *out, int size, const char *dir, const char *name) {
    char stem[NAME_MAX_LEN], cand[NAME_MAX_LEN + 16];
    const char *dot = strrchr(name, '.');
    struct stat st;

    join_path(out, size, dir, name);
    if (stat(out, &st) < 0) return;
    if (!dot || dot == name) dot = name + strlen(name);
    snprintf(stem, sizeof(stem), "%.*s", (int)(dot - name), name);
    for (int k = 1; k < 100; k++) {
        if (k == 1) snprintf(cand, sizeof(cand), "%s (copy)%s", stem, dot);
        else snprintf(cand, sizeof(cand), "%s (copy %d)%s", stem, k, dot);
        join_path(out, size, dir, cand);
        if (stat(out, &st) < 0) return;
    }
}

static const char *base_name(const char *p) {
    const char *b = strrchr(p, '/');
    return b ? b + 1 : p;
}

static int selected_entry(files_app_t *s) {
    if (s->mode != MODE_BROWSE || s->sel < 0 || s->sel >= s->entry_count ||
        !s->ents[s->sel].kind || !strcmp(s->ents[s->sel].name, ".."))
        return -1;
    return s->sel;
}

static void reselect(files_app_t *s, const char *name) {
    int rows = files_row_capacity(&s->gui);
    s->sel = find_entry(s, name);
    if (s->sel >= 0 && (s->sel < s->scroll || s->sel >= s->scroll + rows))
        s->scroll = s->sel - rows / 2;
    clamp_scroll(s);
}

static void do_copy(files_app_t *s, int cut) {
    int i = selected_entry(s);
    char full[384];

    if (i < 0) { status("Select a file first", ""); return; }
    join_path(full, sizeof(full), s->path, s->ents[i].name);
    if (gui_clipboard_set(&s->gui, full, (int)strlen(full)) < 0) {
        status("Clipboard unavailable", "");
        return;
    }
    s->cut_pending = cut;
    strncpy(s->cut_path, full, sizeof(s->cut_path) - 1);
    status(cut ? "Cut %s" : "Copied %s", s->ents[i].name);
    gui_trace("files", "%s %s", cut ? "cut" : "copied", full);
}

static void do_paste(files_app_t *s) {
    char src[256], dst[384];
    struct stat st;
    int n = gui_clipboard_get(src, sizeof(src)), len, rc;

    while (n > 0 && (src[n - 1] == '\n' || src[n - 1] == ' ')) src[--n] = 0;
    if (n <= 0 || src[0] != '/' || stat(src, &st) < 0) {
        status("Nothing to paste", "");
        return;
    }
    len = (int)strlen(src);
    /* A folder cannot go inside itself. */
    if (S_ISDIR(st.st_mode) && !strncmp(s->path, src, (size_t)len) &&
        (s->path[len] == '/' || s->path[len] == 0)) {
        status("Cannot paste a folder into itself", "");
        return;
    }
    unique_dest(dst, sizeof(dst), s->path, base_name(src));
    if (s->cut_pending && !strcmp(src, s->cut_path)) {
        rc = rename(src, dst);
        if (rc < 0 && (rc = copy_tree(src, dst, 0)) == 0)
            remove_tree(src, 0);
        if (rc == 0) {
            s->cut_pending = 0;
            gui_clipboard_set(&s->gui, dst, (int)strlen(dst));
            strncpy(s->cut_path, dst, sizeof(s->cut_path) - 1);
        }
    } else {
        rc = copy_tree(src, dst, 0);
    }
    load_entries(s);
    if (rc < 0) {
        status("Paste failed: %s", base_name(src));
        gui_trace("files", "paste failed %s -> %s", src, dst);
        return;
    }
    reselect(s, base_name(dst));
    status("Pasted %s", base_name(dst));
    gui_trace("files", "pasted %s -> %s", src, dst);
}

static void open_prompt(files_app_t *s, int kind) {
    gui_widget_t *in = gui_find(&s->gui, TXT_NAME);
    gui_widget_t *ok = gui_find(&s->gui, BTN_OK);
    int i = selected_entry(s);

    if (s->mode != MODE_BROWSE) return;
    if (kind != PR_NEWDIR && i < 0) { status("Select a file first", ""); return; }
    s->prompt = kind;
    s->target[0] = 0;
    if (i >= 0) strcpy(s->target, s->ents[i].name);
    if (in) {
        const char *init = kind == PR_RENAME ? s->target : "New folder";
        strncpy(in->input, init, GUI_INPUT_MAX - 1);
        in->input[GUI_INPUT_MAX - 1] = 0;
        in->input_len = (int)strlen(in->input);
    }
    if (ok) strcpy(ok->text, kind == PR_DELETE ? "Delete" : "OK");
    if (kind == PR_DELETE) {
        char msg[GUI_MAX_LABEL];
        snprintf(msg, sizeof(msg), "Delete %s?", s->target);
        set_label(&s->gui, IDX_PROMPT, msg);
    } else {
        set_label(&s->gui, IDX_PROMPT, "Name:");
    }
    s->gui.focus_id = kind == PR_DELETE ? 0 : TXT_NAME;
    gui_grab_escape(&s->gui, 1);         /* Esc cancels the prompt */
    layout_files(&s->gui);
    {
        int ox, oy, cx, cy;
        widget_point(ok, &ox, &oy);
        widget_point(gui_find(&s->gui, BTN_CANCEL), &cx, &cy);
        gui_trace("files", "prompt %s %s ok=%d,%d cancel=%d,%d",
                  kind == PR_NEWDIR ? "newdir" :
                  kind == PR_RENAME ? "rename" : "delete",
                  s->target[0] ? s->target : "-", ox, oy, cx, cy);
    }
}

static void close_prompt(files_app_t *s) {
    s->prompt = PR_NONE;
    s->gui.focus_id = 0;
    gui_grab_escape(&s->gui, 0);
    layout_files(&s->gui);
}

static void accept_prompt(files_app_t *s) {
    gui_widget_t *in = gui_find(&s->gui, TXT_NAME);
    const char *name = in ? in->input : "";
    char a[384], b[384];
    int kind = s->prompt;

    close_prompt(s);
    if (kind == PR_DELETE) {
        join_path(a, sizeof(a), s->path, s->target);
        if (remove_tree(a, 0) < 0) {
            status("Cannot delete %s", s->target);
            gui_trace("files", "delete failed %s", a);
        } else {
            status("Deleted %s", s->target);
            gui_trace("files", "deleted %s", a);
        }
        load_entries(s);
        return;
    }
    if (!name[0] || strchr(name, '/') || !strcmp(name, ".") ||
        !strcmp(name, "..")) {
        status("Invalid name", "");
        return;
    }
    join_path(b, sizeof(b), s->path, name);
    if (kind == PR_NEWDIR) {
        if (mkdir(b, 0755) < 0) {
            status("Cannot create %s", name);
            gui_trace("files", "mkdir failed %s", b);
            return;
        }
        gui_trace("files", "mkdir %s", b);
        status("Created %s", name);
    } else {
        struct stat st;
        join_path(a, sizeof(a), s->path, s->target);
        if (strcmp(a, b) && stat(b, &st) == 0) {
            status("%s already exists", name);
            return;
        }
        if (rename(a, b) < 0) {
            status("Cannot rename %s", s->target);
            gui_trace("files", "rename failed %s", a);
            return;
        }
        gui_trace("files", "renamed %s -> %s", a, b);
        status("Renamed to %s", name);
    }
    load_entries(s);
    reselect(s, name);
}

static void redraw(files_app_t *s) {
    refresh_rows(s);
    trace_listing(s);
    gui_draw(&s->gui);
}

static void select_row(files_app_t *s, int idx) {
    s->sel = idx;
    if (idx >= 0 && idx < s->entry_count && s->ents[idx].kind)
        gui_trace("files", "selected %s", s->ents[idx].name);
}

static void on_button(gui_window_t *gui, int id) {
    int rows = files_row_capacity(&app.gui);

    if (!gui) return;

    if (id >= BTN_ROW_BASE && id < IMG_ROW_BASE) {
        int idx = app.scroll + (id - BTN_ROW_BASE);
        long t = now_ms();

        if (app.mode == MODE_VIEW || app.prompt) return;
        if (idx < 0 || idx >= app.entry_count) return;
        if (!app.ents[idx].kind) return;          /* empty/error row */
        /* Modern semantics: single click selects, double-click opens. */
        if (idx == app.sel && t - app.sel_ms < 450) {
            app.sel = -1;
            open_entry(&app, idx);
        } else {
            select_row(&app, idx);
        }
        app.sel_ms = t;
    } else if (id == BTN_OK) {
        accept_prompt(&app);
    } else if (id == BTN_CANCEL) {
        close_prompt(&app);
    } else if (app.prompt) {
        return;                                    /* modal */
    } else if (id == BTN_UP) {
        if (app.mode == MODE_VIEW) exit_view(&app);
        else { go_parent(&app); app.scroll = 0; load_entries(&app); }
    } else if (id == BTN_PREV) {
        app.scroll -= rows;
        if (app.scroll < 0) app.scroll = 0;
    } else if (id == BTN_NEXT) {
        app.scroll += rows;
        clamp_scroll(&app);
    } else if (id == BTN_REFRESH) {
        if (app.mode == MODE_BROWSE) load_entries(&app);
    } else if (id == BTN_NEWDIR) {
        open_prompt(&app, PR_NEWDIR);
    } else if (id == BTN_RENAME) {
        open_prompt(&app, PR_RENAME);
    } else if (id == BTN_DELETE) {
        open_prompt(&app, PR_DELETE);
    } else if (id == BTN_COPY) {
        do_copy(&app, 0);
    } else if (id == BTN_CUT) {
        do_copy(&app, 1);
    } else if (id == BTN_PASTE) {
        if (app.mode == MODE_BROWSE) do_paste(&app);
    } else if (id == CHK_HIDDEN) {
        app.show_hidden = gui_value(&app.gui, CHK_HIDDEN) == 1;
        if (app.mode == MODE_BROWSE) load_entries(&app);
    }
    redraw(&app);
}

/* Prompt text field editing (the key hook bypasses libgui's routing). */
static void prompt_key(int code, int ascii) {
    gui_widget_t *in = gui_find(&app.gui, TXT_NAME);

    if (code == KEY_ESC) { close_prompt(&app); return; }
    if (code == KEY_ENTER) { accept_prompt(&app); return; }
    if (!in || app.prompt == PR_DELETE) return;
    if (code == KEY_BACKSPACE) {
        if (in->input_len) {
            do in->input_len--;
            while (in->input_len &&
                   ((unsigned char)in->input[in->input_len] & 0xC0) == 0x80);
            in->input[in->input_len] = 0;
        }
        return;
    }
    if (ascii == 21) { in->input_len = 0; in->input[0] = 0; return; }  /* ^U */
    if (ascii >= 32 && ascii != 127) {
        char u[4];
        int n = draw_utf8_encode((unsigned)ascii, u);
        if (in->input_len + n < GUI_INPUT_MAX) {
            memcpy(in->input + in->input_len, u, (size_t)n);
            in->input_len += n;
            in->input[in->input_len] = 0;
        }
    }
}

static void on_key(gui_window_t *gui, int code, int value, int ascii) {
    int rows = files_row_capacity(&app.gui);

    (void)gui;
    (void)value;
    if (app.prompt) { prompt_key(code, ascii); redraw(&app); return; }
    if (app.mode == MODE_VIEW) {
        if (code == KEY_BACKSPACE || code == KEY_ENTER) exit_view(&app);
        redraw(&app);
        return;
    }
    switch (code) {
    case KEY_UP: case KEY_DOWN: {
        int s = app.sel + (code == KEY_UP ? -1 : 1);
        if (app.sel < 0) s = 0;
        if (s >= 0 && s < app.entry_count) select_row(&app, s);
        if (app.sel < app.scroll) app.scroll = app.sel;
        if (app.sel >= app.scroll + rows) app.scroll = app.sel - rows + 1;
        break;
    }
    case KEY_ENTER: open_entry(&app, app.sel); break;
    case KEY_BACKSPACE: go_parent(&app); load_entries(&app); break;
    case KEY_DELETE: open_prompt(&app, PR_DELETE); break;
    case KEY_F2: open_prompt(&app, PR_RENAME); break;
    case KEY_F5: load_entries(&app); break;
    default:
        if (ascii == 3) do_copy(&app, 0);
        else if (ascii == 24) do_copy(&app, 1);
        else if (ascii == 22) do_paste(&app);
        else if (ascii == 14) open_prompt(&app, PR_NEWDIR);
        else return;
    }
    redraw(&app);
}

static void on_scroll(gui_window_t *gui, int delta) {
    (void)gui;
    app.scroll -= delta * 2;     /* wheel up shows earlier rows */
    if (app.scroll < 0) app.scroll = 0;
    clamp_scroll(&app);
    refresh_rows(&app);
    gui_draw(&app.gui);
}

static int setup_failed(void) {
    printf("files: setup failed\n");
    gui_close(&app.gui);
    return 1;
}

int main(int argc, char *argv[]) {
    int slot = (argc > 1) ? atoi(argv[1]) : 3;
    struct stat st;

    memset(&app, 0, sizeof(app));
    strcpy(app.path, "/");
    if (argc > 2 && argv[2] && argv[2][0] == '/' && stat(argv[2], &st) == 0 &&
        S_ISDIR(st.st_mode)) {
        strncpy(app.path, argv[2], sizeof(app.path) - 1);
        if (strlen(app.path) > 1 && app.path[strlen(app.path) - 1] == '/')
            app.path[strlen(app.path) - 1] = 0;
    }
    app.mode = MODE_BROWSE;
    app.sel = -1;
    if (slot < 1 || slot > WM_MAX_SLOTS) slot = 3;

    if (gui_open(&app.gui, slot, "Files", 400, 100, 560, 460) < 0) {
        printf("files: desktop unavailable\n");
        return 1;
    }
    gui_set_scroll_handler(&app.gui, on_scroll);
    gui_set_key_handler(&app.gui, on_key);

    strcpy(app.gui.bg, "#f6f8fa");
    if (gui_panel(&app.gui, 10, 40, 320, 220, "#eef3f8") < 0 ||
        gui_label(&app.gui, 18, 46, "/", "#3a4a60") < 0 ||
        gui_label(&app.gui, 404, 330, "", "#55606e") < 0 ||
        gui_label(&app.gui, -1000, 330, "", "#202830") < 0)
        return setup_failed();
    /* Rows are buttons coloured to match the panel so they look like a list
     * but still deliver per-row clicks for navigation / opening files. */
    for (int i = 0; i < FILES_VISIBLE_ROWS; i++) {
        if (gui_button(&app.gui, BTN_ROW_BASE + i, 14, ROW_START_Y + i * ROW_H,
                       300, ROW_H - 2, "", on_button) < 0)
            return setup_failed();
        strncpy(app.gui.widgets[IDX_ROW0 + i].color, "#eef3f8",
                sizeof(app.gui.widgets[0].color) - 1);
        strncpy(app.gui.widgets[IDX_ROW0 + i].text_color, "black",
                sizeof(app.gui.widgets[0].text_color) - 1);
    }
    if (gui_button(&app.gui, BTN_UP, 14, 330, 78, 32, "UP", on_button) < 0 ||
        gui_button(&app.gui, BTN_PREV, 98, 330, 88, 32, "PREV", on_button) < 0 ||
        gui_button(&app.gui, BTN_NEXT, 192, 330, 88, 32, "NEXT", on_button) < 0 ||
        gui_button(&app.gui, BTN_REFRESH, 286, 330, 108, 32, "REFRESH", on_button) < 0 ||
        gui_button(&app.gui, BTN_NEWDIR, 0, 0, 92, 26, "New folder", on_button) < 0 ||
        gui_button(&app.gui, BTN_RENAME, 0, 0, 70, 26, "Rename", on_button) < 0 ||
        gui_button(&app.gui, BTN_DELETE, 0, 0, 64, 26, "Delete", on_button) < 0 ||
        gui_button(&app.gui, BTN_COPY, 0, 0, 54, 26, "Copy", on_button) < 0 ||
        gui_button(&app.gui, BTN_CUT, 0, 0, 46, 26, "Cut", on_button) < 0 ||
        gui_button(&app.gui, BTN_PASTE, 0, 0, 56, 26, "Paste", on_button) < 0 ||
        gui_checkbox(&app.gui, CHK_HIDDEN, 0, 0, "Hidden", 0, on_button) < 0 ||
        gui_textinput(&app.gui, TXT_NAME, -1000, 0, 200, "", 0) < 0 ||
        gui_button(&app.gui, BTN_OK, -1000, 0, 80, 32, "OK", on_button) < 0 ||
        gui_button(&app.gui, BTN_CANCEL, -1000, 0, 80, 32, "Cancel", on_button) < 0)
        return setup_failed();
    {
        static const int light[] = { BTN_NEWDIR, BTN_RENAME, BTN_DELETE,
                                      BTN_COPY, BTN_CUT, BTN_PASTE };
        for (unsigned i = 0; i < sizeof(light) / sizeof(light[0]); i++) {
            gui_widget_t *b = gui_find(&app.gui, light[i]);
            strcpy(b->color, "#dfe6ee");
            strcpy(b->text_color, "#1e2630");
        }
        strcpy(gui_find(&app.gui, BTN_CANCEL)->color, "#8a94a3");
    }
    /* Per-row type icons: themed full-color .mic when available, else the
     * legacy 16x16 hex-art. */
    gui_icondef(&app.gui, ICON_FOLDER, 16, 16, folder_icon);
    gui_icondef(&app.gui, ICON_FILE, 16, 16, file_icon);
    gui_icondef(&app.gui, ICON_DEV, 16, 16, dev_icon);
    gui_icondef_mic(&app.gui, ICON_FOLDER, "/disk/icons/folder_24.mic", 20, 20);
    gui_icondef_mic(&app.gui, ICON_FILE,   "/disk/icons/file_24.mic",   20, 20);
    gui_icondef_mic(&app.gui, ICON_DEV,    "/disk/icons/disk_24.mic",   20, 20);
    for (int i = 0; i < FILES_VISIBLE_ROWS; i++)
        if (gui_image(&app.gui, IMG_ROW_BASE + i, 16,
                      ROW_START_Y + i * ROW_H, -1) < 0)
            return setup_failed();

    gui_set_layout(&app.gui, layout_files);
    load_entries(&app);
    layout_files(&app.gui);
    {
        int p[7][2];
        static const int ids[7] = { BTN_NEWDIR, BTN_RENAME, BTN_DELETE,
                                    BTN_COPY, BTN_CUT, BTN_PASTE, CHK_HIDDEN };
        for (int i = 0; i < 7; i++)
            widget_point(gui_find(&app.gui, ids[i]), &p[i][0], &p[i][1]);
        p[6][0] = GUI_BODY_X + gui_find(&app.gui, CHK_HIDDEN)->x + 8;
        gui_trace("files", "toolbar newdir=%d,%d rename=%d,%d delete=%d,%d "
                  "copy=%d,%d cut=%d,%d paste=%d,%d hidden=%d,%d",
                  p[0][0], p[0][1], p[1][0], p[1][1], p[2][0], p[2][1],
                  p[3][0], p[3][1], p[4][0], p[4][1], p[5][0], p[5][1],
                  p[6][0], p[6][1]);
    }
    redraw(&app);
    wm_status(&app.gui.wm, "FILES RUNNING");

    while (!app.gui.closed) {
        gui_poll(&app.gui);
        sleep_ms(50);
    }

    wm_status(&app.gui.wm, "FILES CLOSED");
    gui_close(&app.gui);
    return 0;
}
