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
 * settings — wallpaper picker (scans /disk for .ppm/.bmp), accent color
 * swatches, keyboard layout (US / Turkish Q), clock format and time zone.
 * Writes /disk/etc/desktop.conf (keeping lines it does not manage) and tells
 * the desktop to reload.  Display resolution is not here: the framebuffer
 * mode is set by the boot loader (GRUB gfxmode), see README.
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

/* Keyboard layout, clock and time zone (as in desktop.conf). */
static int keymap_tr;            /* keymap=tr */
static int clock12;              /* clock=12 */
static const struct { int min; const char *name; } zones[] = {
    { -600, "Honolulu" }, { -480, "Los Angeles" }, { -360, "Chicago" },
    { -300, "New York" }, { -180, "Sao Paulo" }, { 0, "London (UTC)" },
    { 60, "Berlin, Paris" }, { 120, "Athens, Kyiv" },
    { 180, "Istanbul, Moscow" }, { 240, "Dubai" }, { 330, "India" },
    { 480, "Beijing" }, { 540, "Tokyo" }, { 600, "Sydney" },
};
#define ZONES ((int)(sizeof(zones) / sizeof(zones[0])))
static int zone = 5;             /* index into zones */
static char conf_other[512];     /* desktop.conf lines we leave alone */
static char conf_wall[160];      /* the wallpaper= value already set */
static char conf_accent[16];     /* an accent= that is not a swatch */

/* Right-column controls (surface coordinates), set by render(). */
typedef struct { int x, y, w, h; } box_t;
static box_t b_us, b_tr, b_24, b_12, b_tzprev, b_tznext;

static int in_box(const box_t *b, int x, int y) {
    return x >= b->x && y >= b->y && x < b->x + b->w && y < b->y + b->h;
}

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

/* A per-user file for the decoded wallpaper, which desktop.conf points
 * at so it must persist: /disk/etc is root-only, and a fixed
 * name in the shared /tmp could be pre-planted by another user. */
static void user_cache_path(char *out, int cap, const char *name) {
    const char *home = getenv("HOME");
    if (home && home[0] == '/')
        snprintf(out, cap, "%s/.%s", home, name);
    else
        snprintf(out, cap, "/tmp/%s-%d", name, getuid());
}

/* PPM loads directly; any other format (PNG/JPG/GIF/BMP) is decoded to a
 * cached PPM by the imgconv helper.  Returns the path the desktop should use. */
static const char *resolve_wallpaper(const char *path) {
    static char cache[256];
    if (ends_with(path, ".ppm")) return path;

    const char *conv = access("/disk/bin/imgconv", 1) == 0 ?
                       "/disk/bin/imgconv" : "/bin/imgconv";
    user_cache_path(cache, sizeof(cache), "wallpaper-cache.ppm");
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

/* Read desktop.conf: the values this app manages, and the other lines. */
static void load_conf(void) {
    char buf[1024];
    int fd = open("/disk/etc/desktop.conf", O_RDONLY), n, o = 0;

    if (fd < 0) return;
    n = (int)read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return;
    buf[n] = 0;
    for (char *line = buf; line && *line; ) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        if (!strncmp(line, "keymap=", 7)) {
            keymap_tr = !strcmp(line + 7, "tr");
        } else if (!strncmp(line, "clock=", 6)) {
            clock12 = !strcmp(line + 6, "12");
        } else if (!strncmp(line, "tz=", 3)) {
            const char *t = line + 3;
            int sign = 1, hh = 0, mm = 0;
            if (!strncmp(t, "UTC", 3)) t += 3;
            if (*t == '-') { sign = -1; t++; } else if (*t == '+') t++;
            while (*t >= '0' && *t <= '9') hh = hh * 10 + (*t++ - '0');
            if (*t == ':') { t++; while (*t >= '0' && *t <= '9') mm = mm * 10 + (*t++ - '0'); }
            for (int i = 0; i < ZONES; i++)
                if (zones[i].min == sign * (hh * 60 + mm)) zone = i;
        } else if (!strncmp(line, "wallpaper=", 10)) {
            snprintf(conf_wall, sizeof(conf_wall), "%s", line + 10);
        } else if (!strncmp(line, "accent=#", 8)) {
            unsigned v = (unsigned)strtoul(line + 8, 0, 16);
            for (int i = 0; i < 6; i++)
                if (accents[i] == v) sel_accent = i;
            if (sel_accent < 0)
                snprintf(conf_accent, sizeof(conf_accent), "%s", line + 7);
        } else if (*line && o < (int)sizeof(conf_other) - 1) {
            o += snprintf(conf_other + o, sizeof(conf_other) - (size_t)o,
                          "%s\n", line);
        }
        line = nl ? nl + 1 : 0;
    }
}

static void apply(void) {
    char buf[1024];
    int n = 0, fd, m = zones[zone].min;

    mkdir("/disk/etc", 0755);
    if (sel_wall >= 0) {
        strcpy(status, "Decoding image...");
        dirty = 1;
        const char *wp = resolve_wallpaper(walls[sel_wall]);
        n += snprintf(buf + n, sizeof(buf) - (size_t)n,
                      "wallpaper=%s\n", wp);
    } else if (conf_wall[0]) {
        n += snprintf(buf + n, sizeof(buf) - (size_t)n,
                      "wallpaper=%s\n", conf_wall);
    }
    if (sel_accent >= 0)
        n += snprintf(buf + n, sizeof(buf) - (size_t)n,
                      "accent=#%06x\n", (unsigned)accents[sel_accent]);
    else if (conf_accent[0])
        n += snprintf(buf + n, sizeof(buf) - (size_t)n,
                      "accent=%s\n", conf_accent);
    n += snprintf(buf + n, sizeof(buf) - (size_t)n,
                  "keymap=%s\nclock=%s\ntz=%c%02d:%02d\n%s",
                  keymap_tr ? "tr" : "us", clock12 ? "12" : "24",
                  m < 0 ? '-' : '+', (m < 0 ? -m : m) / 60,
                  (m < 0 ? -m : m) % 60, conf_other);
    fd = open("/disk/etc/desktop.conf", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        strcpy(status, "Cannot write /disk/etc/desktop.conf");
        dirty = 1;
        return;
    }
    write(fd, buf, n);
    close(fd);
    gui_trace("settings", "applied wallpaper=%d accent=%d keymap=%s clock=%d "
              "tz=%d", sel_wall, sel_accent, keymap_tr ? "tr" : "us",
              clock12 ? 12 : 24, m);
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

    /* Keyboard / clock / time zone, under the swatches. */
    {
        int x0 = s->w / 2 + 10, y = 156;
        uint32_t on = draw_rgb(94, 129, 172), off = draw_rgb(222, 226, 232);
        uint32_t ton = draw_rgb(245, 248, 250), toff = draw_rgb(30, 34, 36);
        char tz[48];
        int m = zones[zone].min;
#define SEG(b, X, Y, W, label, sel) do { \
            (b).x = (X); (b).y = (Y); (b).w = (W); (b).h = 26; \
            draw_round_rect(s, (b).x, (b).y, (b).w, (b).h, 5, (sel) ? on : off); \
            draw_text_aa(s, (b).x + ((b).w - draw_text_width(label, &draw_font_ui)) / 2, \
                         (b).y + 4, label, (sel) ? ton : toff, &draw_font_ui); \
        } while (0)
        draw_text_aa(s, x0, y, "Keyboard", draw_rgb(30, 34, 36), &draw_font_ui);
        SEG(b_us, x0, y + 20, 80, "US", !keymap_tr);
        SEG(b_tr, x0 + 86, y + 20, 100, "T\xc3\xbc" "rk\xc3\xa7" "e Q", keymap_tr);
        y += 56;
        draw_text_aa(s, x0, y, "Clock", draw_rgb(30, 34, 36), &draw_font_ui);
        SEG(b_24, x0, y + 20, 80, "24-hour", !clock12);
        SEG(b_12, x0 + 86, y + 20, 80, "12-hour", clock12);
        y += 56;
        draw_text_aa(s, x0, y, "Time zone", draw_rgb(30, 34, 36), &draw_font_ui);
        SEG(b_tzprev, x0, y + 20, 26, "<", 0);
        SEG(b_tznext, x0 + 190, y + 20, 26, ">", 0);
#undef SEG
        snprintf(tz, sizeof(tz), "UTC%c%d%s %s", m < 0 ? '-' : '+',
                 (m < 0 ? -m : m) / 60, (m % 60) ? ":30" : "",
                 zones[zone].name);
        draw_text_aa(s, x0 + 32, y + 24, tz, draw_rgb(30, 34, 36),
                     &draw_font_ui);
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

    if (in_box(&b_us, x, y) || in_box(&b_tr, x, y)) {
        keymap_tr = in_box(&b_tr, x, y);
        gui_trace("settings", "keymap %s selected", keymap_tr ? "tr" : "us");
        dirty = 1;
        return;
    }
    if (in_box(&b_24, x, y) || in_box(&b_12, x, y)) {
        clock12 = in_box(&b_12, x, y);
        gui_trace("settings", "clock %d selected", clock12 ? 12 : 24);
        dirty = 1;
        return;
    }
    if (in_box(&b_tzprev, x, y) || in_box(&b_tznext, x, y)) {
        zone = (zone + (in_box(&b_tznext, x, y) ? 1 : ZONES - 1)) % ZONES;
        gui_trace("settings", "tz %d selected", zones[zone].min);
        dirty = 1;
        return;
    }
    if (y >= s->h - 60 && y < s->h - 32 && x >= 10 && x < 120) {
        apply();
        return;
    }
    if (x < s->w / 2 && y >= 40 && y < s->h - 64) {
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
            gui_trace("settings", "accent %d selected", idx);
            dirty = 1;
        }
    }
}

int main(int argc, char *argv[]) {
    int slot = (argc > 1) ? atoi(argv[1]) : 1;

    if (slot < 1 || slot > WM_MAX_SLOTS) slot = 1;
    if (gui_open(&gui, slot, "Settings", 240 + slot * 12, 90 + slot * 10,
                 520, 440) < 0) {
        printf("settings: desktop unavailable\n");
        return 1;
    }
    gui_set_click_handler(&gui, on_click);

    scan_walls();
    load_conf();
    render();
    /* Click targets for tools/smoke_gui.py: the last accent swatch, Apply. */
    gui_trace("settings", "ready walls=%d accent5=%d,%d apply=%d,%d", wall_count,
              GUI_BODY_X + gui.surf.w / 2 + 10 + 2 * 56 + 22,
              GUI_BODY_Y + 42 + 56 + 22,
              GUI_BODY_X + 65, GUI_BODY_Y + gui.surf.h - 46);
    gui_trace("settings", "controls us=%d,%d tr=%d,%d clock24=%d,%d "
              "clock12=%d,%d tzprev=%d,%d tznext=%d,%d",
              GUI_BODY_X + b_us.x + b_us.w / 2, GUI_BODY_Y + b_us.y + 13,
              GUI_BODY_X + b_tr.x + b_tr.w / 2, GUI_BODY_Y + b_tr.y + 13,
              GUI_BODY_X + b_24.x + b_24.w / 2, GUI_BODY_Y + b_24.y + 13,
              GUI_BODY_X + b_12.x + b_12.w / 2, GUI_BODY_Y + b_12.y + 13,
              GUI_BODY_X + b_tzprev.x + 13, GUI_BODY_Y + b_tzprev.y + 13,
              GUI_BODY_X + b_tznext.x + 13, GUI_BODY_Y + b_tznext.y + 13);
    while (!gui.closed) {
        int events = gui_poll(&gui);
        if (dirty || events > 0)
            render();
        sleep_ms(50);
    }
    gui_close(&gui);
    return 0;
}
