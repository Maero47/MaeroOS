#include <draw.h>
#include <fcntl.h>
#include <gui.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/*
 * store — the MaeroOS Store.  Lists repo packages (via the pkg CLI),
 * installs/removes with a click, shows progress; tells the desktop to
 * rescan apps afterwards.
 */

#define MAX_PKGS 32

typedef struct {
    char name[32], version[16], caption[96];
    int size_kb;
    int installed;
} entry_t;

static gui_window_t gui;
static entry_t entries[MAX_PKGS];
static int entry_count;
static int sel = -1;
static int dirty = 1;
static char status[96] = "Loading package list...";

/* worker-thread op: 0 idle, 1 update, 2 install, 3 remove */
static volatile int op_busy, op_done;
static int op_kind;
static char op_target[32];
static int op_result;
static pthread_t op_thread;

static void sleep_ms(long ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, 0);
}

static void set_status(const char *s) {
    strncpy(status, s, sizeof(status) - 1);
    status[sizeof(status) - 1] = 0;
    dirty = 1;
}

static int run_pkg(const char *verb, const char *arg) {
    int pid = fork();

    if (pid == 0) {
        char *argv[4];
        const char *bin = access("/disk/pkg", 1) == 0 ? "/disk/pkg" : "/pkg";
        argv[0] = (char *)bin;
        argv[1] = (char *)verb;
        argv[2] = (char *)arg;     /* may be NULL */
        argv[3] = 0;
        execve(bin, argv, 0);
        exit(127);
    }
    if (pid > 0) {
        int st = 0;
        waitpid(pid, &st, 0);
        return st;
    }
    return -1;
}

static void load_index(void) {
    static char buf[8192];
    int fd = open("/disk/etc/pkg-index.txt", O_RDONLY), n;
    char *line, *next;

    entry_count = 0;
    if (fd < 0) return;
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return;
    buf[n] = 0;
    line = buf;
    while (line && *line && entry_count < MAX_PKGS) {
        char *f[8] = {0};
        int nf = 0;
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        f[nf++] = line;
        for (char *p = line; *p && nf < 8; p++)
            if (*p == '|') { *p = 0; f[nf++] = p + 1; }
        if (nf >= 5) {
            entry_t *e = &entries[entry_count++];
            char mpath[128];
            strncpy(e->name, f[0], sizeof(e->name) - 1);
            strncpy(e->version, f[1], sizeof(e->version) - 1);
            e->size_kb = atoi(f[2]) / 1024;
            strncpy(e->caption, f[4], sizeof(e->caption) - 1);
            snprintf(mpath, sizeof(mpath), "/disk/apps/%s/manifest", e->name);
            e->installed = access(mpath, 0) == 0;
        }
        line = next;
    }
}

static void *op_worker(void *arg) {
    (void)arg;
    if (op_kind == 1)
        op_result = run_pkg("update", 0);
    else if (op_kind == 2)
        op_result = run_pkg("install", op_target);
    else if (op_kind == 3)
        op_result = run_pkg("remove", op_target);
    op_done = 1;
    return 0;
}

static void start_op(int kind, const char *target) {
    if (op_busy) return;
    op_kind = kind;
    if (target) {
        strncpy(op_target, target, sizeof(op_target) - 1);
        op_target[sizeof(op_target) - 1] = 0;
    }
    op_busy = 1;
    op_done = 0;
    set_status(kind == 1 ? "Refreshing package list..." :
               kind == 2 ? "Downloading and installing..." : "Removing...");
    if (pthread_create(&op_thread, 0, op_worker, 0) != 0)
        op_worker(0);
}

static void render(void) {
    draw_surface_t *s = &gui.surf;
    char txt[128];

    if (!s->px) return;
    draw_fill(s, draw_rgb(246, 247, 249));

    /* header */
    draw_rect(s, 0, 0, s->w, 44, draw_rgb(52, 60, 76));
    draw_text_aa(s, 14, 8, "MaeroOS Store", draw_rgb(240, 244, 250),
                 &draw_font_ui_big);
    /* refresh button */
    draw_rect(s, s->w - 92, 9, 80, 26, draw_rgb(94, 129, 172));
    draw_text_aa(s, s->w - 92 + (80 - draw_text_width("Refresh",
                                                      &draw_font_ui)) / 2,
                 13, "Refresh", draw_rgb(245, 248, 250), &draw_font_ui);

    if (!entry_count)
        draw_text_aa(s, 14, 60, "No packages. Click Refresh (server: host:8000).",
                     draw_rgb(110, 116, 122), &draw_font_ui);

    for (int i = 0; i < entry_count; i++) {
        int ry = 54 + i * 64;
        uint32_t card = draw_rgb(255, 255, 255);

        if (i == sel) card = draw_rgb(232, 240, 250);
        draw_rect(s, 8, ry, s->w - 16, 56, card);
        draw_frame(s, 8, ry, s->w - 16, 56, draw_rgb(216, 220, 226));
        snprintf(txt, sizeof(txt), "%s %s", entries[i].name,
                 entries[i].version);
        draw_text_aa(s, 20, ry + 6, txt, draw_rgb(28, 32, 38),
                     &draw_font_ui);
        snprintf(txt, sizeof(txt), "%s  (%d KB)", entries[i].caption,
                 entries[i].size_kb);
        draw_text_aa(s, 20, ry + 30, txt, draw_rgb(110, 116, 122),
                     &draw_font_ui);
        /* action button */
        {
            const char *lab = entries[i].installed ? "Remove" : "Install";
            uint32_t bc = entries[i].installed ? draw_rgb(180, 76, 66)
                                               : draw_rgb(74, 144, 96);
            draw_rect(s, s->w - 110, ry + 12, 90, 32, bc);
            draw_text_aa(s, s->w - 110 + (90 - draw_text_width(lab,
                                                               &draw_font_ui)) / 2,
                         ry + 18, lab, draw_rgb(248, 250, 252),
                         &draw_font_ui);
        }
    }

    /* busy bar + status */
    if (op_busy) {
        static int phase;
        int bw = s->w - 16;
        phase = (phase + 7) % bw;
        draw_rect(s, 8, s->h - 40, bw, 6, draw_rgb(222, 226, 232));
        draw_rect(s, 8 + phase, s->h - 40, 60, 6, draw_rgb(94, 129, 172));
    }
    draw_text_aa(s, 10, s->h - 26, status, draw_rgb(102, 110, 112),
                 &draw_font_ui);
    wm_commit(&gui.wm, gui.slot);
    dirty = 0;
}

static void on_click(gui_window_t *g, int x, int y) {
    draw_surface_t *s = &g->surf;

    if (y < 44 && x >= s->w - 92) {        /* Refresh */
        start_op(1, 0);
        return;
    }
    for (int i = 0; i < entry_count; i++) {
        int ry = 54 + i * 64;
        if (y >= ry && y < ry + 56) {
            sel = i;
            dirty = 1;
            if (x >= s->w - 110 && x < s->w - 20 && !op_busy)
                start_op(entries[i].installed ? 3 : 2, entries[i].name);
            return;
        }
    }
}

int main(int argc, char *argv[]) {
    int slot = (argc > 1) ? atoi(argv[1]) : 1;

    if (slot < 1 || slot > WM_MAX_SLOTS) slot = 1;
    if (gui_open(&gui, slot, "Store", 200 + slot * 12, 70 + slot * 10,
                 560, 460) < 0) {
        printf("store: desktop unavailable\n");
        return 1;
    }
    gui_set_click_handler(&gui, on_click);

    load_index();
    if (!entry_count)
        start_op(1, 0);
    else
        set_status("Ready");
    render();
    while (!gui.closed) {
        int events = gui_poll(&gui);
        if (op_done) {
            op_done = 0;
            pthread_join(op_thread, 0);
            op_busy = 0;
            load_index();
            set_status(op_result == 0 ? "Done" :
                       op_result == 2 ?
                       "Cannot reach the app store server. On your computer "
                       "run: make repo-serve" :
                       "Operation failed (see console)");
            wm_command(&gui.wm, "apps-changed");
            dirty = 1;
        }
        /* Keep redrawing while an op runs so the progress bar animates
         * (render() clears dirty each frame, so op_busy must drive it). */
        if (dirty || events > 0 || op_busy)
            render();
        sleep_ms(40);
    }
    gui_close(&gui);
    return 0;
}
