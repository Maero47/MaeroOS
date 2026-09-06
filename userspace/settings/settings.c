#include <dirent.h>
#include <draw.h>
#include <fcntl.h>
#include <gui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/*
 * settings — wallpaper picker (scans /disk for .ppm/.bmp) and accent color
 * swatches.  Writes /disk/etc/desktop.conf and tells the desktop to reload.
 */

#define MAX_WALLS 16

static gui_window_t gui;
static char walls[MAX_WALLS][64];
static int wall_count;
static int sel_wall = -1;
static int sel_accent = -1;
static int dirty = 1;
static char status[96] = "Pick a wallpaper or accent color";

static const uint32_t accents[6] = {
    0x5E81AC, 0xBF616A, 0xA3BE8C, 0xD08770, 0xB48EAD, 0x4C9A8F,
};

static void sleep_ms(long ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, 0);
}

static int ends_with(const char *s, const char *suf) {
    int sl = (int)strlen(s), fl = (int)strlen(suf);
    return sl >= fl && !strcmp(s + sl - fl, suf);
}

static int is_image(const char *name) {
    return ends_with(name, ".ppm") || ends_with(name, ".bmp") ||
           ends_with(name, ".png") || ends_with(name, ".jpg") ||
           ends_with(name, ".jpeg") || ends_with(name, ".gif");
}

static void scan_walls(void) {
    DIR *d = opendir("/disk");
    struct dirent *e;

    wall_count = 0;
    if (!d) return;
    while ((e = readdir(d)) && wall_count < MAX_WALLS) {
        if (is_image(e->d_name)) {
            snprintf(walls[wall_count], sizeof(walls[0]), "/disk/%s",
                     e->d_name);
            wall_count++;
        }
    }
    closedir(d);
}

/* PPM loads directly; any other format (PNG/JPG/GIF/BMP) is decoded to a
 * cached PPM by the imgconv helper.  Returns the path the desktop should use. */
static const char *resolve_wallpaper(const char *path) {
    static char cache[80];
    if (ends_with(path, ".ppm")) return path;

    const char *conv = access("/disk/bin/imgconv", 1) == 0 ?
                       "/disk/bin/imgconv" : "/bin/imgconv";
    snprintf(cache, sizeof(cache), "/disk/etc/wallpaper-cache.ppm");
    mkdir("/disk/etc", 0755);
    int pid = fork();
    if (pid == 0) {
        char *argv[] = { (char *)conv, (char *)path, cache, 0 };
        execve(conv, argv, 0);
        _exit(127);
    }
    if (pid > 0) {
        int st = 0;
        waitpid(pid, &st, 0);
        if (st == 0 && access(cache, 4) == 0)
            return cache;
    }
    return path;   /* fall back; desktop will reject non-PPM gracefully */
}

static void apply(void) {
    char buf[256];
    int n = 0, fd;

    mkdir("/disk/etc", 0755);
    if (sel_wall >= 0) {
        strcpy(status, "Decoding image...");
        dirty = 1;
        const char *wp = resolve_wallpaper(walls[sel_wall]);
        n += snprintf(buf + n, sizeof(buf) - (size_t)n,
                      "wallpaper=%s\n", wp);
    }
    if (sel_accent >= 0)
        n += snprintf(buf + n, sizeof(buf) - (size_t)n,
                      "accent=#%06x\n", (unsigned)accents[sel_accent]);
    fd = open("/disk/etc/desktop.conf", O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        strcpy(status, "Cannot write /disk/etc/desktop.conf");
        dirty = 1;
        return;
    }
    write(fd, buf, n);
    close(fd);
    wm_command(&gui.wm, "reload");
    strcpy(status, "Applied (desktop reloaded)");
    dirty = 1;
}

static void render(void) {
    draw_surface_t *s = &gui.surf;
    char *name;

    if (!s->px) return;
    draw_fill(s, draw_rgb(245, 246, 244));

    draw_text_aa(s, 10, 8, "Wallpaper", draw_rgb(30, 34, 36),
                 &draw_font_ui_big);
    for (int i = 0; i < wall_count; i++) {
        int ry = 42 + i * 26;
        uint32_t fg = draw_rgb(30, 34, 36);
        if (i == sel_wall) {
            draw_rect(s, 6, ry - 2, s->w / 2 - 12, 24, draw_rgb(94, 129, 172));
            fg = draw_rgb(238, 240, 235);
        }
        name = strrchr(walls[i], '/');
        draw_text_aa(s, 14, ry, name ? name + 1 : walls[i], fg, &draw_font_ui);
    }
    if (!wall_count)
        draw_text_aa(s, 14, 42, "(no .ppm/.bmp files in /disk)",
                     draw_rgb(120, 126, 130), &draw_font_ui);

    draw_text_aa(s, s->w / 2 + 10, 8, "Accent", draw_rgb(30, 34, 36),
                 &draw_font_ui_big);
    for (int i = 0; i < 6; i++) {
        int bx = s->w / 2 + 10 + (i % 3) * 56;
        int by = 42 + (i / 3) * 56;
        draw_rect(s, bx, by, 44, 44, accents[i]);
        if (i == sel_accent)
            draw_frame(s, bx - 2, by - 2, 48, 48, draw_rgb(30, 34, 36));
    }

    /* Apply button */
    draw_rect(s, 10, s->h - 60, 110, 28, draw_rgb(94, 129, 172));
    draw_text_aa(s, 10 + (110 - draw_text_width("Apply", &draw_font_ui)) / 2,
                 s->h - 55, "Apply", draw_rgb(245, 248, 250), &draw_font_ui);
    draw_text_aa(s, 10, s->h - 24, status, draw_rgb(102, 110, 112),
                 &draw_font_ui);

    wm_commit(&gui.wm, gui.slot);
    dirty = 0;
}

static void on_click(gui_window_t *g, int x, int y) {
    draw_surface_t *s = &g->surf;

    if (y >= s->h - 60 && y < s->h - 32 && x >= 10 && x < 120) {
        apply();
        return;
    }
    if (x < s->w / 2 && y >= 40) {
        int idx = (y - 40) / 26;
        if (idx >= 0 && idx < wall_count) { sel_wall = idx; dirty = 1; }
        return;
    }
    if (x >= s->w / 2 + 10 && y >= 42 && y < 42 + 2 * 56) {
        int c = (x - s->w / 2 - 10) / 56;
        int r = (y - 42) / 56;
        int idx = r * 3 + c;
        if (c >= 0 && c < 3 && idx >= 0 && idx < 6) {
            sel_accent = idx;
            dirty = 1;
        }
    }
}

int main(int argc, char *argv[]) {
    int slot = (argc > 1) ? atoi(argv[1]) : 1;

    if (slot < 1 || slot > WM_MAX_SLOTS) slot = 1;
    if (gui_open(&gui, slot, "Settings", 240 + slot * 12, 110 + slot * 10,
                 480, 360) < 0) {
        printf("settings: desktop unavailable\n");
        return 1;
    }
    gui_set_click_handler(&gui, on_click);

    scan_walls();
    render();
    while (!gui.closed) {
        int events = gui_poll(&gui);
        if (dirty || events > 0)
            render();
        sleep_ms(50);
    }
    gui_close(&gui);
    return 0;
}
