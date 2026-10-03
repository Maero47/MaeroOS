#include <dirent.h>
#include <draw.h>
#include <fcntl.h>
#include <gui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/*
 * settings — wallpaper picker (scans /disk for .ppm/.bmp), accent color
 * swatches, keyboard layout (US / Turkish Q), clock format and time zone.
 * Writes /disk/etc/desktop.conf (keeping lines it does not manage) and tells
 * the desktop to reload.
 *
 * The Display page (the "Display" button) lists the modes the kernel's
 * display driver offers (FBIO_MAEROS_MODES on /dev/fb0) and asks the desktop
 * to switch ("setmode W H").  The desktop goes back to the old mode after 15
 * seconds unless "Keep" is pressed here ("modekeep"), which also saves
 * mode=WxH in desktop.conf for the next login.  On a boot framebuffer with
 * no mode-setting driver (UEFI GOP, an unknown card) the list holds the one
 * fixed mode.
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
static char conf_mode[24];       /* mode= (WxH) */

/* Display page */
#define MODE_CONFIRM_S 15
#define MAX_W_DESKTOP 1920        /* the desktop's widest supported mode */
static int page_display;         /* 0 general, 1 display */
static struct fb_modelist modes;
static int mode_idx[FB_MAX_MODES];   /* listed entries -> modes.modes[] */
static int mode_n;
static int sel_mode = -1;        /* index into mode_idx */
static int confirm_left;         /* seconds until the desktop reverts; 0 none */
static long confirm_deadline;
static unsigned prev_w, prev_h;  /* the mode before Apply */

/* Right-column controls (surface coordinates), set by render(). */
typedef struct { int x, y, w, h; } box_t;
static box_t b_us, b_tr, b_24, b_12, b_tzprev, b_tznext;
static box_t b_display, b_back, b_mapply, b_keep, b_revert;
static box_t b_mode[FB_MAX_MODES];

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
        } else if (!strncmp(line, "mode=", 5)) {
            snprintf(conf_mode, sizeof(conf_mode), "%s", line + 5);
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

/* Write desktop.conf from the current choices; 0 or -1. */
static int write_conf(int decode_wallpaper) {
    char buf[1024];
    int n = 0, fd, m = zones[zone].min;

    mkdir("/disk/etc", 0755);
    if (sel_wall >= 0 && decode_wallpaper) {
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
    if (conf_mode[0])
        n += snprintf(buf + n, sizeof(buf) - (size_t)n, "mode=%s\n", conf_mode);
    n += snprintf(buf + n, sizeof(buf) - (size_t)n,
                  "keymap=%s\nclock=%s\ntz=%c%02d:%02d\n%s",
                  keymap_tr ? "tr" : "us", clock12 ? "12" : "24",
                  m < 0 ? '-' : '+', (m < 0 ? -m : m) / 60,
                  (m < 0 ? -m : m) % 60, conf_other);
    fd = open("/disk/etc/desktop.conf", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        strcpy(status, "Cannot write /disk/etc/desktop.conf");
        dirty = 1;
        return -1;
    }
    write(fd, buf, n);
    close(fd);
    return 0;
}

static void apply(void) {
    int m = zones[zone].min;

    if (write_conf(1) < 0) return;
    gui_trace("settings", "applied wallpaper=%d accent=%d keymap=%s clock=%d "
              "tz=%d", sel_wall, sel_accent, keymap_tr ? "tr" : "us",
              clock12 ? 12 : 24, m);
    wm_command(&gui.wm, "reload");
    strcpy(status, "Applied (desktop reloaded)");
    dirty = 1;
}


/* ── Display page ────────────────────────────────────────────────────────── */

static long now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec;
}

/* The kernel's mode list, without what the desktop cannot drive. */
static void load_modes(void) {
    int fd = open("/dev/fb0", O_RDONLY);

    memset(&modes, 0, sizeof(modes));
    mode_n = 0;
    sel_mode = -1;
    if (fd < 0) return;
    if (ioctl(fd, FBIO_MAEROS_MODES, &modes) < 0) {
        /* an older kernel: the one current mode */
        struct fb_var_screeninfo var;
        memset(&modes, 0, sizeof(modes));
        if (ioctl(fd, FBIOGET_VSCREENINFO, &var) == 0) {
            modes.count = 1;
            modes.modes[0].w = (unsigned short)var.xres;
            modes.modes[0].h = (unsigned short)var.yres;
            strcpy(modes.driver, "boot");
        }
    }
    close(fd);
    if (modes.count > FB_MAX_MODES) modes.count = FB_MAX_MODES;
    for (unsigned i = 0; i < modes.count; i++) {
        if (modes.modes[i].w > MAX_W_DESKTOP || modes.modes[i].w < 640 ||
            modes.modes[i].h < 480)
            continue;
        if (i == modes.current) sel_mode = mode_n;
        mode_idx[mode_n++] = (int)i;
    }
}

static const struct fb_mode *listed(int i) {
    return &modes.modes[mode_idx[i]];
}

static void current_mode(unsigned *w, unsigned *h) {
    *w = *h = 0;
    if (modes.current < modes.count) {
        *w = modes.modes[modes.current].w;
        *h = modes.modes[modes.current].h;
    }
}

static void render_display(draw_surface_t *s) {
    uint32_t ink = draw_rgb(30, 34, 36), sel = draw_rgb(94, 129, 172);
    uint32_t light = draw_rgb(245, 248, 250);
    char line[96];
    unsigned cw, ch;
    int cols = mode_n > 14 ? 3 : 2, colw = (s->w - 20) / cols;

    current_mode(&cw, &ch);
    draw_text_aa(s, 10, 8, "Display", ink, &draw_font_ui_big);
    snprintf(line, sizeof(line), "Driver: %s    Current: %ux%u",
             modes.driver[0] ? modes.driver : "none", cw, ch);
    draw_text_aa(s, 10, 36, line, draw_rgb(102, 110, 112), &draw_font_ui);
    for (int i = 0; i < mode_n; i++) {
        const struct fb_mode *m = listed(i);
        box_t *b = &b_mode[i];
        b->x = 10 + (i % cols) * colw;
        b->y = 62 + (i / cols) * 26;
        b->w = colw - 8;
        b->h = 24;
        int is_cur = mode_idx[i] == (int)modes.current;
        if (i == sel_mode)
            draw_round_rect(s, b->x, b->y, b->w, b->h, 4, sel);
        snprintf(line, sizeof(line), "%ux%u%s%s", m->w, m->h,
                 m->w == modes.preferred_w && m->h == modes.preferred_h ?
                 " (display)" : "", is_cur ? "  *" : "");
        draw_text_aa(s, b->x + 8, b->y + 3, line, i == sel_mode ? light : ink,
                     &draw_font_ui);
    }
    if (!(modes.flags & FB_MODES_SETTABLE))
        draw_text_aa(s, 10, s->h - 92,
                     "This display's mode is fixed by the firmware.",
                     draw_rgb(160, 70, 60), &draw_font_ui);

    b_mapply = (box_t){ 10, s->h - 60, 110, 28 };
    b_back = (box_t){ 130, s->h - 60, 110, 28 };
    draw_rect(s, b_mapply.x, b_mapply.y, b_mapply.w, b_mapply.h, sel);
    draw_text_aa(s, b_mapply.x + (b_mapply.w - draw_text_width("Apply", &draw_font_ui)) / 2,
                 b_mapply.y + 5, "Apply", light, &draw_font_ui);
    draw_rect(s, b_back.x, b_back.y, b_back.w, b_back.h, draw_rgb(222, 226, 232));
    draw_text_aa(s, b_back.x + (b_back.w - draw_text_width("Back", &draw_font_ui)) / 2,
                 b_back.y + 5, "Back", ink, &draw_font_ui);
    draw_text_aa(s, 10, s->h - 24, status, draw_rgb(102, 110, 112), &draw_font_ui);

    /* "Keep this resolution?" over the list while the desktop waits. */
    b_keep = (box_t){ s->w / 2 - 120, s->h / 2 + 10, 110, 28 };
    b_revert = (box_t){ s->w / 2 + 10, s->h / 2 + 10, 110, 28 };
    if (confirm_left > 0) {
        int px = s->w / 2 - 150, py = s->h / 2 - 60;
        draw_round_rect(s, px - 2, py - 2, 304, 134, 8, draw_rgb(60, 66, 72));
        draw_round_rect(s, px, py, 300, 130, 7, draw_rgb(250, 251, 252));
        draw_text_aa(s, px + 16, py + 12, "Keep this resolution?", ink,
                     &draw_font_ui_big);
        snprintf(line, sizeof(line), "Reverting to %ux%u in %d s", prev_w,
                 prev_h, confirm_left);
        draw_text_aa(s, px + 16, py + 44, line, draw_rgb(102, 110, 112),
                     &draw_font_ui);
        draw_rect(s, b_keep.x, b_keep.y, b_keep.w, b_keep.h, sel);
        draw_text_aa(s, b_keep.x + (b_keep.w - draw_text_width("Keep", &draw_font_ui)) / 2,
                     b_keep.y + 5, "Keep", light, &draw_font_ui);
        draw_rect(s, b_revert.x, b_revert.y, b_revert.w, b_revert.h,
                  draw_rgb(222, 226, 232));
        draw_text_aa(s, b_revert.x + (b_revert.w - draw_text_width("Revert", &draw_font_ui)) / 2,
                     b_revert.y + 5, "Revert", ink, &draw_font_ui);
    }
}

/* Click targets for tools/smoke_gfxmode.py, window-relative. */
static void trace_display(void) {
    unsigned cw, ch;
    char buf[160];
    int n = 0;

    current_mode(&cw, &ch);
    gui_trace("settings", "display page driver=%s modes=%d current=%ux%u "
              "settable=%d flush=%d apply=%d,%d back=%d,%d keep=%d,%d revert=%d,%d",
              modes.driver[0] ? modes.driver : "none", mode_n, cw, ch,
              (modes.flags & FB_MODES_SETTABLE) != 0,
              (modes.flags & FB_MODES_FLUSH) != 0,
              GUI_BODY_X + b_mapply.x + b_mapply.w / 2, GUI_BODY_Y + b_mapply.y + 14,
              GUI_BODY_X + b_back.x + b_back.w / 2, GUI_BODY_Y + b_back.y + 14,
              GUI_BODY_X + b_keep.x + b_keep.w / 2, GUI_BODY_Y + b_keep.y + 14,
              GUI_BODY_X + b_revert.x + b_revert.w / 2, GUI_BODY_Y + b_revert.y + 14);
    for (int i = 0; i < mode_n; i++) {
        n += snprintf(buf + n, sizeof(buf) - (size_t)n, " %ux%u@%d,%d",
                      listed(i)->w, listed(i)->h,
                      GUI_BODY_X + b_mode[i].x + b_mode[i].w / 2,
                      GUI_BODY_Y + b_mode[i].y + 12);
        if (n > (int)sizeof(buf) - 32 || i == mode_n - 1) {
            gui_trace("settings", "modes%s", buf);
            n = 0;
        }
    }
}

static void open_display_page(void) {
    page_display = 1;
    load_modes();
    strcpy(status, (modes.flags & FB_MODES_SETTABLE) ?
           "Pick a resolution and Apply" : "Fixed mode (no display driver)");
    render_display(&gui.surf);          /* lay the boxes out for the trace */
    trace_display();
    dirty = 1;
}

static void display_apply(void) {
    unsigned cw, ch;

    if (sel_mode < 0 || confirm_left > 0) return;
    current_mode(&cw, &ch);
    const struct fb_mode *m = listed(sel_mode);
    if (m->w == cw && m->h == ch) {
        strcpy(status, "That is the current resolution");
        dirty = 1;
        return;
    }
    if (!(modes.flags & FB_MODES_SETTABLE)) {
        strcpy(status, "This display cannot change resolution");
        dirty = 1;
        return;
    }
    prev_w = cw;
    prev_h = ch;
    char cmdline[48];
    snprintf(cmdline, sizeof(cmdline), "setmode %u %u", m->w, m->h);
    wm_command(&gui.wm, cmdline);
    gui_trace("settings", "mode apply %ux%u (was %ux%u)", m->w, m->h, cw, ch);
    confirm_left = MODE_CONFIRM_S;
    confirm_deadline = now_s() + MODE_CONFIRM_S;
    usleep(200000);                   /* the desktop switches */
    load_modes();                     /* "Current:" and the selection */
    snprintf(status, sizeof(status), "Switched to %ux%u", m->w, m->h);
    dirty = 1;
}

static void display_keep(void) {
    unsigned w = listed(sel_mode)->w, h = listed(sel_mode)->h;

    confirm_left = 0;
    wm_command(&gui.wm, "modekeep");
    snprintf(conf_mode, sizeof(conf_mode), "%ux%u", w, h);
    write_conf(0);
    load_modes();
    gui_trace("settings", "mode keep %ux%u saved", w, h);
    snprintf(status, sizeof(status), "%ux%u kept (saved for next login)", w, h);
    dirty = 1;
}

/* Revert pressed, or the time ran out (the desktop reverts by itself then;
 * this only catches up with it). */
static void display_revert(const char *why) {
    confirm_left = 0;
    if (!strcmp(why, "button"))
        wm_command(&gui.wm, "moderevert");
    usleep(200000);                   /* let the desktop switch back first */
    load_modes();
    gui_trace("settings", "mode revert (%s) to %ux%u", why, prev_w, prev_h);
    snprintf(status, sizeof(status), "Reverted to %ux%u", prev_w, prev_h);
    dirty = 1;
}

static void display_click(int x, int y) {
    if (confirm_left > 0) {
        if (in_box(&b_keep, x, y)) display_keep();
        else if (in_box(&b_revert, x, y)) display_revert("button");
        return;
    }
    if (in_box(&b_back, x, y)) {
        page_display = 0;
        strcpy(status, "Pick a wallpaper or accent color");
        gui_trace("settings", "general page");
        dirty = 1;
        return;
    }
    if (in_box(&b_mapply, x, y)) {
        display_apply();
        return;
    }
    for (int i = 0; i < mode_n; i++)
        if (in_box(&b_mode[i], x, y)) {
            sel_mode = i;
            gui_trace("settings", "mode %ux%u selected", listed(i)->w, listed(i)->h);
            dirty = 1;
            return;
        }
}

/* Once a second while the desktop waits for Keep. */
static void display_tick(void) {
    if (confirm_left <= 0) return;
    int left = (int)(confirm_deadline - now_s());
    if (left <= -1) {                 /* a second after the desktop's own */
        display_revert("timeout");
        return;
    }
    if (left < 1) left = 1;
    if (left != confirm_left) {
        confirm_left = left;
        dirty = 1;
    }
}

static void render(void) {
    draw_surface_t *s = &gui.surf;
    char *name;

    if (!s->px) return;
    draw_fill(s, draw_rgb(245, 246, 244));
    if (page_display) {
        render_display(s);
        wm_commit(&gui.wm, gui.slot);
        dirty = 0;
        return;
    }

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
    if (b_display.y != s->h - 60 && b_display.h)   /* the window was resized */
        gui_trace("settings", "display button=%d,%d", GUI_BODY_X + 130 + 55,
                  GUI_BODY_Y + s->h - 60 + 14);
    b_display = (box_t){ 130, s->h - 60, 110, 28 };
    draw_rect(s, b_display.x, b_display.y, b_display.w, b_display.h,
              draw_rgb(222, 226, 232));
    draw_text_aa(s, b_display.x + (b_display.w - draw_text_width("Display", &draw_font_ui)) / 2,
                 b_display.y + 5, "Display", draw_rgb(30, 34, 36), &draw_font_ui);
    draw_text_aa(s, 10, s->h - 24, status, draw_rgb(102, 110, 112),
                 &draw_font_ui);

    wm_commit(&gui.wm, gui.slot);
    dirty = 0;
}

static void on_click(gui_window_t *g, int x, int y) {
    draw_surface_t *s = &g->surf;

    if (page_display) {
        display_click(x, y);
        return;
    }
    if (in_box(&b_display, x, y)) {
        open_display_page();
        return;
    }

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
    gui_trace("settings", "display button=%d,%d",
              GUI_BODY_X + b_display.x + b_display.w / 2,
              GUI_BODY_Y + b_display.y + 14);
    while (!gui.closed) {
        int events = gui_poll(&gui);
        display_tick();
        if (dirty || events > 0)
            render();
        sleep_ms(50);
    }
    /* closed while the desktop waits: it reverts by itself */
    gui_close(&gui);
    return 0;
}
