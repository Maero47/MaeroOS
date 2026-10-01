/*
 * linuxapps — the "Linux Apps" window: the curated Alpine X11 applications
 * (xapps.h), each with Install/Remove and Open.  Installing and removing run
 * the set-uid helper /disk/xapp (apk in the Alpine chroot, then a launcher
 * entry in /disk/apps); Open asks the desktop to launch the entry, which
 * runs `/disk/xapp <slot> <name>` and so starts maeroX when needed.
 *
 * Every state change prints one "[linuxapps] ..." line (row and button
 * positions included) for tools/smoke_alpinex.py.
 */
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
#include "xapps.h"

#define HEADER_H 44
#define ROW_H    52
#define BTN_W    84

static gui_window_t gui;
static int inst[N_XAPPS];
static int dirty = 1;
static char status[120] = "Ready";
static int have_root;

static volatile int op_busy, op_done;
static int op_idx, op_remove, op_result;
static pthread_t op_thread;

static void sleep_ms(long ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, 0);
}

static void set_status(const char *s) {
    snprintf(status, sizeof(status), "%s", s);
    dirty = 1;
}

static void scan(void) {
    for (int i = 0; i < N_XAPPS; i++) {
        char p[160];
        snprintf(p, sizeof(p), ALPINE_ROOT "%s", xapps[i].binary);
        inst[i] = access(p, F_OK) == 0;
    }
}

/* The helper's output, line by line, becomes the status line. */
static void *op_worker(void *arg) {
    (void)arg;
    int fds[2];
    if (pipe(fds) < 0) { op_result = -1; op_done = 1; return 0; }
    pid_t pid = fork();
    if (pid == 0) {
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        close(fds[0]);
        close(fds[1]);
        char *argv[] = { (char *)XAPP_HELPER, op_remove ? (char *)"remove" : (char *)"install",
                         (char *)xapps[op_idx].name, 0 };
        execve(argv[0], argv, 0);
        _exit(127);
    }
    close(fds[1]);
    char line[120];
    int n = 0;
    char ch;
    while (read(fds[0], &ch, 1) == 1) {
        if (ch == '\n' || n == (int)sizeof(line) - 1) {
            line[n] = 0;
            if (n) { set_status(line); gui_trace("linuxapps", "apk: %s", line); }
            n = 0;
            continue;
        }
        line[n++] = ch;
    }
    close(fds[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    op_result = WIFEXITED(st) ? WEXITSTATUS(st) : 128;
    op_done = 1;
    return 0;
}

static void start_op(int i, int remove) {
    if (op_busy) return;
    op_idx = i;
    op_remove = remove;
    op_busy = 1;
    op_done = 0;
    set_status(remove ? "Removing..." : "Installing from the offline repository...");
    gui_trace("linuxapps", "%s %s", remove ? "remove" : "install", xapps[i].name);
    if (pthread_create(&op_thread, 0, op_worker, 0) != 0) op_worker(0);
}

static void render(void) {
    draw_surface_t *s = &gui.surf;
    if (!s->px) return;
    draw_fill(s, draw_rgb(246, 247, 249));
    draw_rect(s, 0, 0, s->w, HEADER_H, draw_rgb(44, 62, 92));
    draw_text_aa(s, 14, 8, "Linux Apps", draw_rgb(240, 244, 250), &draw_font_ui_big);
    draw_text_aa(s, 150, 14, "Alpine Linux X11 applications", draw_rgb(170, 186, 210),
                 &draw_font_ui);
    if (!have_root) {
        draw_text_aa(s, 14, 60, "No Alpine root at " ALPINE_ROOT ".", draw_rgb(180, 76, 66),
                     &draw_font_ui);
        draw_text_aa(s, 14, 84, "Boot with disk-alpinex.img (docs/alpinex.md).",
                     draw_rgb(110, 116, 122), &draw_font_ui);
    }
    for (int i = 0; have_root && i < N_XAPPS; i++) {
        int ry = HEADER_H + 8 + i * ROW_H;
        draw_rect(s, 8, ry, s->w - 16, ROW_H - 6, draw_rgb(255, 255, 255));
        draw_frame(s, 8, ry, s->w - 16, ROW_H - 6, draw_rgb(216, 220, 226));
        draw_text_aa(s, 18, ry + 4, xapps[i].title, draw_rgb(28, 32, 38), &draw_font_ui);
        draw_text_aa(s, 18, ry + 24, xapps[i].about, draw_rgb(110, 116, 122), &draw_font_ui);
        int bx = s->w - 16 - BTN_W;
        const char *lab = inst[i] ? "Remove" : "Install";
        uint32_t bc = inst[i] ? draw_rgb(180, 76, 66) : draw_rgb(74, 144, 96);
        draw_rect(s, bx, ry + 8, BTN_W, 30, bc);
        draw_text_aa(s, bx + (BTN_W - draw_text_width(lab, &draw_font_ui)) / 2, ry + 13, lab,
                     draw_rgb(248, 250, 252), &draw_font_ui);
        if (inst[i]) {
            int ox = bx - BTN_W - 8;
            draw_rect(s, ox, ry + 8, BTN_W, 30, draw_rgb(58, 121, 200));
            draw_text_aa(s, ox + (BTN_W - draw_text_width("Open", &draw_font_ui)) / 2, ry + 13,
                         "Open", draw_rgb(248, 250, 252), &draw_font_ui);
        }
    }
    if (op_busy) {
        static int phase;
        int bw = s->w - 16;
        phase = (phase + 9) % bw;
        draw_rect(s, 8, s->h - 40, bw, 6, draw_rgb(222, 226, 232));
        draw_rect(s, 8 + phase, s->h - 40, 60, 6, draw_rgb(94, 129, 172));
    }
    draw_text_aa(s, 10, s->h - 26, status, draw_rgb(90, 98, 104), &draw_font_ui);
    wm_commit(&gui.wm, gui.slot);
    dirty = 0;
}

/* Button positions in body coordinates, for the smoke test. */
static void trace_rows(void) {
    int w = gui.surf.w;
    for (int i = 0; i < N_XAPPS; i++) {
        int ry = HEADER_H + 8 + i * ROW_H;
        int bx = w - 16 - BTN_W;
        gui_trace("linuxapps", "row %s %s button=%d,%d open=%d,%d", xapps[i].name,
                  inst[i] ? "installed" : "available", bx + BTN_W / 2, ry + 23,
                  bx - 8 - BTN_W / 2, ry + 23);
    }
}

static void on_click(gui_window_t *g, int x, int y) {
    int w = g->surf.w;
    if (!have_root || op_busy) return;
    for (int i = 0; i < N_XAPPS; i++) {
        int ry = HEADER_H + 8 + i * ROW_H;
        if (y < ry + 8 || y >= ry + 38) continue;
        int bx = w - 16 - BTN_W;
        if (x >= bx && x < bx + BTN_W) { start_op(i, inst[i]); return; }
        if (inst[i] && x >= bx - 8 - BTN_W && x < bx - 8) {
            gui_trace("linuxapps", "open %s", xapps[i].name);
            wm_command(&gui.wm, "launch %s", xapps[i].name);
            set_status("Starting...");
            return;
        }
    }
}

int main(int argc, char *argv[]) {
    int slot = argc > 1 ? atoi(argv[1]) : 1;
    if (slot < 1 || slot > WM_MAX_SLOTS) slot = 1;
    have_root = access(ALPINE_ROOT "/etc/alpine-release", F_OK) == 0 &&
                access(XAPP_HELPER, X_OK) == 0;
    int h = HEADER_H + 16 + N_XAPPS * ROW_H + 50;
    if (gui_open(&gui, slot, "Linux Apps", 180 + slot * 12, 40 + slot * 10, 600, h) < 0) {
        printf("linuxapps: desktop unavailable\n");
        return 1;
    }
    gui_set_click_handler(&gui, on_click);
    scan();
    gui_trace("linuxapps", "ready root=%d", have_root);
    trace_rows();
    render();
    while (!gui.closed) {
        int events = gui_poll(&gui);
        if (op_done) {
            op_done = 0;
            pthread_join(op_thread, 0);
            op_busy = 0;
            scan();
            if (op_result != 0) set_status("Failed - see the console");
            gui_trace("linuxapps", "%s %s exit=%d", op_remove ? "removed" : "installed",
                      xapps[op_idx].name, op_result);
            trace_rows();
            dirty = 1;
        }
        if (dirty || events > 0 || op_busy) render();
        sleep_ms(40);
    }
    gui_close(&gui);
    return 0;
}
