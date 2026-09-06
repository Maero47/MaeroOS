#include <dirent.h>
#include <fcntl.h>
#include <gui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define FILES_MAX_ENTRIES 64
#define FILES_VISIBLE_ROWS 14
#define VIEW_MAX_BYTES 4096

/* Widget layout (fixed indices) */
#define IDX_PANEL    0
#define IDX_TITLE    1
#define IDX_PATH     2
#define IDX_ROW0     3
#define IDX_UP       (IDX_ROW0 + FILES_VISIBLE_ROWS)      /* 17 */
#define IDX_PREV     (IDX_UP + 1)                          /* 18 */
#define IDX_NEXT     (IDX_UP + 2)                          /* 19 */
#define IDX_REFRESH  (IDX_UP + 3)                          /* 20 */
#define FILES_WIDGETS (IDX_REFRESH + 1)                    /* 21 */

#define ROW_START_Y 84
#define ROW_H 18

/* Button ids */
#define BTN_UP      1
#define BTN_PREV    2
#define BTN_NEXT    3
#define BTN_REFRESH 4
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

typedef struct {
    gui_window_t gui;
    int mode;
    char path[256];
    char view_name[128];
    /* Browse entries (also reused to hold viewer text lines). */
    char entries[FILES_MAX_ENTRIES][GUI_MAX_LABEL];
    char names[FILES_MAX_ENTRIES][GUI_MAX_LABEL];
    char kinds[FILES_MAX_ENTRIES];
    int entry_count;
    int scroll;
    int sel;        /* selected row (-1 = none); double-click opens */
    long sel_ms;    /* timestamp of the selecting click */
} files_app_t;

static files_app_t app;

static void sleep_ms(long ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, 0);
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
    int rows;
    if (!state) return;
    rows = files_row_capacity(&state->gui);
    if (state->scroll > state->entry_count - rows)
        state->scroll = state->entry_count - rows;
    if (state->scroll < 0) state->scroll = 0;
}

/* Return a short Linux-like type tag and a single kind char for navigation. */
static const char *type_tag(const char *dir, const struct dirent *ent, char *kind) {
    char path[256];
    struct stat st;
    *kind = '?';
    if (!ent) return "UNK";

    join_path(path, sizeof(path), dir, ent->d_name);
    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode))  { *kind = 'D'; return "DIR"; }
        if (S_ISCHR(st.st_mode))  { *kind = 'C'; return "DEV"; }
        if (S_ISBLK(st.st_mode))  { *kind = 'C'; return "DEV"; }
        if (S_ISFIFO(st.st_mode)) { *kind = 'P'; return "PIPE"; }
        if (S_ISLNK(st.st_mode))  { *kind = 'L'; return "LINK"; }
        if (S_ISREG(st.st_mode))  { *kind = 'F'; return "FILE"; }
    }
    /* Fall back to the dirent type when stat is unavailable. */
    if (ent->d_type == DT_DIR)  { *kind = 'D'; return "DIR"; }
    if (ent->d_type == DT_CHR)  { *kind = 'C'; return "DEV"; }
    if (ent->d_type == DT_FIFO) { *kind = 'P'; return "PIPE"; }
    if (ent->d_type == DT_LNK)  { *kind = 'L'; return "LINK"; }
    if (ent->d_type == DT_REG)  { *kind = 'F'; return "FILE"; }
    return "UNK";
}

static void add_entry(files_app_t *s, const char *tag, char kind, const char *name) {
    (void)tag;   /* type is conveyed by the row icon now */
    if (s->entry_count >= FILES_MAX_ENTRIES) return;
    /* Three leading spaces clear the 16px row icon. */
    snprintf(s->entries[s->entry_count], GUI_MAX_LABEL, "   %s", name);
    strncpy(s->names[s->entry_count], name, GUI_MAX_LABEL - 1);
    s->names[s->entry_count][GUI_MAX_LABEL - 1] = 0;
    s->kinds[s->entry_count] = kind;
    s->entry_count++;
}

/* Map an entry kind to its WM icon (-1 = none). */
static int icon_for_kind(char kind) {
    switch (kind) {
    case 'D': return ICON_FOLDER;
    case 'F': return ICON_FILE;
    case 'L': return ICON_FILE;
    case 'C': return ICON_DEV;
    case 'P': return ICON_DEV;
    }
    return -1;
}

static void load_entries(files_app_t *state) {
    DIR *dir;
    struct dirent *ent;

    if (!state) return;
    state->entry_count = 0;
    state->mode = MODE_BROWSE;
    state->sel = -1;
    dir = opendir(state->path);
    if (!dir) {
        char msg[GUI_MAX_LABEL];
        snprintf(msg, sizeof(msg), "cannot open %s", state->path);
        add_entry(state, "!", 0, msg);
        state->scroll = 0;
        return;
    }

    while ((ent = readdir(dir)) != 0 && state->entry_count < FILES_MAX_ENTRIES) {
        char kind;
        const char *tag;
        if (!strcmp(ent->d_name, ".")) continue;
        tag = type_tag(state->path, ent, &kind);
        add_entry(state, tag, kind, ent->d_name);
    }
    closedir(dir);
    if (state->entry_count == 0)
        add_entry(state, "", 0, "(empty)");
    clamp_scroll(state);
}

static void push_view_line(files_app_t *s, char *line, int *col) {
    if (s->entry_count >= FILES_MAX_ENTRIES) return;
    line[*col] = 0;
    strncpy(s->entries[s->entry_count], line, GUI_MAX_LABEL - 1);
    s->entries[s->entry_count][GUI_MAX_LABEL - 1] = 0;
    s->names[s->entry_count][0] = 0;     /* viewer rows are not actionable */
    s->kinds[s->entry_count] = 0;
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
        add_entry(s, "!", 0, "cannot open file");
        return;
    }
    while (total < VIEW_MAX_BYTES &&
           (n = read(fd, buf + total, VIEW_MAX_BYTES - total)) > 0)
        total += n;
    close(fd);

    for (int i = 0; i < total && s->entry_count < FILES_MAX_ENTRIES; i++) {
        char c = buf[i];
        if (c == '\r') continue;
        if (c == '\n') { push_view_line(s, line, &col); continue; }
        if (c == '\t') c = ' ';
        if (c < 32 || c > 126) c = '.';
        line[col++] = c;
        if (col >= GUI_MAX_LABEL - 1) push_view_line(s, line, &col);
    }
    if (col > 0) push_view_line(s, line, &col);
    if (s->entry_count == 0)
        push_view_line(s, line, &col);   /* empty file → one blank line */
}

static void refresh_rows(files_app_t *state) {
    char line[GUI_MAX_LABEL];
    int rows;

    if (!state) return;
    rows = files_row_capacity(&state->gui);
    clamp_scroll(state);

    if (state->mode == MODE_VIEW)
        snprintf(line, sizeof(line), "VIEW %s", state->view_name);
    else
        snprintf(line, sizeof(line), "PATH %s", state->path);
    set_label(&state->gui, IDX_PATH, line);

    for (int i = 0; i < FILES_VISIBLE_ROWS; i++) {
        int idx = state->scroll + i;
        gui_widget_t *img = gui_find(&state->gui, IMG_ROW_BASE + i);
        gui_widget_t *rowbtn = gui_find(&state->gui, BTN_ROW_BASE + i);
        if (i < rows && idx < state->entry_count) {
            int selected = state->mode == MODE_BROWSE && idx == state->sel;
            set_label(&state->gui, IDX_ROW0 + i, state->entries[idx]);
            /* Colour rows by type as a visual cue (selection wins). */
            const char *col = "black";
            if (state->mode == MODE_BROWSE) {
                if (selected) col = "white";
                else if (state->kinds[idx] == 'D') col = "blue";
                else if (state->kinds[idx] == 'C') col = "red";
                else if (state->kinds[idx] == 'L') col = "green";
            }
            set_row_color(&state->gui, IDX_ROW0 + i, col);
            if (rowbtn)
                strncpy(rowbtn->color, selected ? "#5e81ac" : "cyan",
                        sizeof(rowbtn->color) - 1);
            if (img)
                img->value = state->mode == MODE_BROWSE ?
                             icon_for_kind(state->kinds[idx]) : -1;
        } else {
            set_label(&state->gui, IDX_ROW0 + i, "");
            if (rowbtn)
                strncpy(rowbtn->color, "cyan", sizeof(rowbtn->color) - 1);
            if (img) img->value = -1;
        }
    }
}

static void layout_files(gui_window_t *gui) {
    int body_w = gui_body_width(gui);
    int panel_w = body_w - 20;
    int y = files_button_y(gui);
    int panel_h = y - 50;

    if (!gui || gui->widget_count < FILES_WIDGETS) return;
    if (panel_w < 280) panel_w = 280;
    if (panel_h < 140) panel_h = 140;

    gui->widgets[IDX_PANEL].x = 10;
    gui->widgets[IDX_PANEL].y = 38;
    gui->widgets[IDX_PANEL].w = panel_w;
    gui->widgets[IDX_PANEL].h = panel_h;
    gui->widgets[IDX_TITLE].x = 18;
    gui->widgets[IDX_TITLE].y = 50;
    gui->widgets[IDX_PATH].x = 18;
    gui->widgets[IDX_PATH].y = 68;

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

    /* Bottom button bar: UP/BACK, PREV, NEXT, REFRESH (fixed, non-overlapping). */
    gui->widgets[IDX_UP].x = 14;   gui->widgets[IDX_UP].y = y;
    gui->widgets[IDX_UP].w = 78;   gui->widgets[IDX_UP].h = 32;
    gui->widgets[IDX_PREV].x = 98; gui->widgets[IDX_PREV].y = y;
    gui->widgets[IDX_PREV].w = 88; gui->widgets[IDX_PREV].h = 32;
    gui->widgets[IDX_NEXT].x = 192; gui->widgets[IDX_NEXT].y = y;
    gui->widgets[IDX_NEXT].w = 88;  gui->widgets[IDX_NEXT].h = 32;
    gui->widgets[IDX_REFRESH].x = 286; gui->widgets[IDX_REFRESH].y = y;
    gui->widgets[IDX_REFRESH].w = 108; gui->widgets[IDX_REFRESH].h = 32;

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
        char np[256];
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

/* Extension associations: open the right app via the WM launch command. */
static int open_with_app(files_app_t *s, const char *full) {
    const char *app = 0;

    if (ends_with_ci(full, ".txt") || ends_with_ci(full, ".sh") ||
        ends_with_ci(full, ".conf") || ends_with_ci(full, ".md") ||
        ends_with_ci(full, ".c") || ends_with_ci(full, ".h"))
        app = "edit";
    else if (ends_with_ci(full, ".ppm") || ends_with_ci(full, ".bmp") ||
             ends_with_ci(full, ".png") || ends_with_ci(full, ".jpg") ||
             ends_with_ci(full, ".jpeg") || ends_with_ci(full, ".gif"))
        app = "view";
    if (!app) return 0;
    wm_command(&s->gui.wm, "launch %s %s", app, full);
    return 1;
}

static void open_view(files_app_t *s, const char *name) {
    char full[256];
    join_path(full, sizeof(full), s->path, name);
    if (open_with_app(s, full)) return;   /* handled by an app */
    strncpy(s->view_name, name, sizeof(s->view_name) - 1);
    s->view_name[sizeof(s->view_name) - 1] = 0;
    load_view(s, full);
    s->mode = MODE_VIEW;
    set_label(&s->gui, IDX_UP, "BACK");
}

static void exit_view(files_app_t *s) {
    s->mode = MODE_BROWSE;
    set_label(&s->gui, IDX_UP, "UP");
    s->scroll = 0;
    load_entries(s);
}

static void on_button(gui_window_t *gui, int id) {
    int rows = files_row_capacity(&app.gui);

    if (!gui) return;

    if (id >= BTN_ROW_BASE) {
        int idx = app.scroll + (id - BTN_ROW_BASE);
        struct timeval tv;
        long now_ms;

        if (app.mode == MODE_VIEW) return;              /* content not clickable */
        if (idx < 0 || idx >= app.entry_count) return;
        if (app.names[idx][0] == '\0') return;          /* empty/error row */

        /* Modern semantics: single click selects, double-click opens. */
        gettimeofday(&tv, 0);
        now_ms = tv.tv_sec * 1000 + tv.tv_usec / 1000;
        if (idx == app.sel && now_ms - app.sel_ms < 450) {
            if (app.kinds[idx] == 'D') enter_dir(&app, app.names[idx]);
            else if (app.kinds[idx] == 'F') open_view(&app, app.names[idx]);
            /* Devices/FIFOs/links stay unopened: reading them can block. */
            app.sel = -1;
        } else {
            app.sel = idx;
        }
        app.sel_ms = now_ms;
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
    }

    refresh_rows(&app);
    gui_draw(gui);
}

static void on_scroll(gui_window_t *gui, int delta) {
    (void)gui;
    if (app.mode != MODE_BROWSE && app.mode != MODE_VIEW) return;
    app.scroll -= delta * 2;     /* wheel up shows earlier rows */
    if (app.scroll < 0) app.scroll = 0;
    clamp_scroll(&app);
    refresh_rows(&app);
    gui_draw(&app.gui);
}

int main(int argc, char *argv[]) {
    int slot = (argc > 1) ? atoi(argv[1]) : 3;

    memset(&app, 0, sizeof(app));
    strcpy(app.path, "/");
    app.mode = MODE_BROWSE;
    app.sel = -1;
    if (slot < 1 || slot > WM_MAX_SLOTS) slot = 3;

    if (gui_open(&app.gui, slot, "Files", 436, 116, 430, 390) < 0) {
        printf("files: desktop unavailable\n");
        return 1;
    }
    gui_set_scroll_handler(&app.gui, on_scroll);

    strcpy(app.gui.bg, "white");
    if (gui_panel(&app.gui, 10, 38, 320, 220, "cyan") < 0 ||
        gui_label(&app.gui, 18, 50, "MAEROS FILES", "black") < 0 ||
        gui_label(&app.gui, 18, 68, "PATH /", "blue") < 0) {
        printf("files: setup failed\n");
        gui_close(&app.gui);
        return 1;
    }
    /* Rows are buttons coloured to match the panel so they look like a list
     * but still deliver per-row clicks for navigation / opening files. */
    for (int i = 0; i < FILES_VISIBLE_ROWS; i++) {
        if (gui_button(&app.gui, BTN_ROW_BASE + i, 14, ROW_START_Y + i * ROW_H,
                       300, ROW_H - 2, "", on_button) < 0) {
            printf("files: setup failed\n");
            gui_close(&app.gui);
            return 1;
        }
        strncpy(app.gui.widgets[IDX_ROW0 + i].color, "cyan",
                sizeof(app.gui.widgets[0].color) - 1);
        strncpy(app.gui.widgets[IDX_ROW0 + i].text_color, "black",
                sizeof(app.gui.widgets[0].text_color) - 1);
    }
    if (gui_button(&app.gui, BTN_UP, 14, 330, 78, 32, "UP", on_button) < 0 ||
        gui_button(&app.gui, BTN_PREV, 98, 330, 88, 32, "PREV", on_button) < 0 ||
        gui_button(&app.gui, BTN_NEXT, 192, 330, 88, 32, "NEXT", on_button) < 0 ||
        gui_button(&app.gui, BTN_REFRESH, 286, 330, 108, 32, "REFRESH", on_button) < 0) {
        printf("files: setup failed\n");
        gui_close(&app.gui);
        return 1;
    }
    /* Per-row type icons: themed full-color .mic when available, else the
     * legacy 16x16 hex-art (REMOVE NOTHING). */
    gui_icondef(&app.gui, ICON_FOLDER, 16, 16, folder_icon);
    gui_icondef(&app.gui, ICON_FILE, 16, 16, file_icon);
    gui_icondef(&app.gui, ICON_DEV, 16, 16, dev_icon);
    gui_icondef_mic(&app.gui, ICON_FOLDER, "/disk/icons/folder_24.mic", 20, 20);
    gui_icondef_mic(&app.gui, ICON_FILE,   "/disk/icons/file_24.mic",   20, 20);
    gui_icondef_mic(&app.gui, ICON_DEV,    "/disk/icons/disk_24.mic",   20, 20);
    for (int i = 0; i < FILES_VISIBLE_ROWS; i++) {
        if (gui_image(&app.gui, IMG_ROW_BASE + i, 16,
                      ROW_START_Y + i * ROW_H, -1) < 0) {
            printf("files: setup failed\n");
            gui_close(&app.gui);
            return 1;
        }
    }

    gui_set_layout(&app.gui, layout_files);
    load_entries(&app);
    layout_files(&app.gui);
    refresh_rows(&app);
    gui_draw(&app.gui);
    wm_status(&app.gui.wm, "FILES RUNNING");

    while (!app.gui.closed) {
        gui_poll(&app.gui);
        sleep_ms(100);
    }

    wm_status(&app.gui.wm, "FILES CLOSED");
    gui_close(&app.gui);
    return 0;
}
