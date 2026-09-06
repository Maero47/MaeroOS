#include <draw.h>
#include <fcntl.h>
#include <gui.h>
#include <linux/input.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/*
 * taskmgr — live process list from /proc/processes + memory bar from
 * /proc/meminfo.  Click a row to select; End Task kills (SIGKILL).
 * CPU% is the delta of the TIME column between 1s refreshes.
 */

#define MAX_ROWS 48

typedef struct {
    int pid;
    char state;
    char name[20];
    int time_ds;          /* TIME in deciseconds (x.y * 10) */
    int cpu_pct;
} task_t;

static gui_window_t gui;
static task_t tasks[MAX_ROWS];
static int task_count;
static int prev_pids[MAX_ROWS], prev_ds[MAX_ROWS], prev_count;
static int sel_pid = -1;
static int mem_total, mem_free;
static int view;
static int dirty = 1;

static void sleep_ms(long ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, 0);
}

static int read_file(const char *path, char *buf, int cap) {
    int fd = open(path, O_RDONLY), n, total = 0;
    if (fd < 0) return -1;
    while (total < cap - 1 &&
           (n = read(fd, buf + total, cap - 1 - total)) > 0)
        total += n;
    close(fd);
    buf[total] = 0;
    return total;
}

static void refresh(void) {
    static char buf[8192];
    char *line, *next;

    prev_count = task_count;
    for (int i = 0; i < task_count; i++) {
        prev_pids[i] = tasks[i].pid;
        prev_ds[i] = tasks[i].time_ds;
    }

    task_count = 0;
    if (read_file("/proc/processes", buf, sizeof(buf)) <= 0) return;
    line = strchr(buf, '\n');             /* skip header */
    line = line ? line + 1 : buf;
    while (line && *line && task_count < MAX_ROWS) {
        task_t *t = &tasks[task_count];
        int ppid, pgrp, sid, ti = 0, td = 0;
        /* libc sscanf ignores %s field widths — buffers must fit 255 */
        static char st[256], tty[256], name[256];

        next = strchr(line, '\n');
        if (next) *next = 0;
        if (sscanf(line, "%d %d %d %d %s %s %d.%d %s",
                   &t->pid, &ppid, &pgrp, &sid, st, tty, &ti, &td,
                   name) >= 9) {
            t->state = st[0];
            t->time_ds = ti * 10 + td;
            strncpy(t->name, name, sizeof(t->name) - 1);
            t->name[sizeof(t->name) - 1] = 0;
            t->cpu_pct = 0;
            for (int j = 0; j < prev_count; j++) {
                if (prev_pids[j] == t->pid) {
                    int d = t->time_ds - prev_ds[j];   /* ds per ~1s */
                    t->cpu_pct = d > 10 ? 100 : d * 10;
                    break;
                }
            }
            task_count++;
        }
        line = next ? next + 1 : 0;
    }

    if (read_file("/proc/meminfo", buf, sizeof(buf)) > 0) {
        char *p = strstr(buf, "MemTotal:");
        if (p) mem_total = atoi(p + 9);
        p = strstr(buf, "MemFree:");
        if (p) mem_free = atoi(p + 8);
    }
    dirty = 1;
}

static void render(void) {
    draw_surface_t *s = &gui.surf;
    int rows, vis = 0, start;
    char txt[96];

    if (!s->px) return;
    draw_fill(s, draw_rgb(245, 246, 244));

    /* Memory bar */
    {
        int used = mem_total - mem_free;
        int bar_w = s->w - 16, fill = mem_total ? bar_w * used / mem_total : 0;
        snprintf(txt, sizeof(txt), "Memory: %d / %d kB", used, mem_total);
        draw_text_aa(s, 8, 6, txt, draw_rgb(30, 34, 36), &draw_font_ui);
        draw_rect(s, 8, 30, bar_w, 12, draw_rgb(210, 214, 218));
        draw_rect(s, 8, 30, fill, 12, draw_rgb(94, 129, 172));
    }

    draw_text_aa(s, 8, 50, "PID", draw_rgb(102, 110, 112), &draw_font_ui);
    draw_text_aa(s, 60, 50, "Name", draw_rgb(102, 110, 112), &draw_font_ui);
    draw_text_aa(s, 230, 50, "State", draw_rgb(102, 110, 112), &draw_font_ui);
    draw_text_aa(s, 290, 50, "CPU", draw_rgb(102, 110, 112), &draw_font_ui);
    draw_text_aa(s, 350, 50, "Time", draw_rgb(102, 110, 112), &draw_font_ui);

    rows = (s->h - 76 - 40) / 22;
    start = view;
    if (start > task_count - rows) start = task_count - rows;
    if (start < 0) start = 0;
    view = start;
    for (int i = start; i < task_count && vis < rows; i++, vis++) {
        task_t *t = &tasks[i];
        int ry = 76 + vis * 22;
        uint32_t fg = draw_rgb(30, 34, 36);

        if (t->pid == sel_pid) {
            draw_rect(s, 4, ry - 2, s->w - 8, 22, draw_rgb(94, 129, 172));
            fg = draw_rgb(238, 240, 235);
        }
        snprintf(txt, sizeof(txt), "%d", t->pid);
        draw_text_aa(s, 8, ry, txt, fg, &draw_font_ui);
        draw_text_aa(s, 60, ry, t->name, fg, &draw_font_ui);
        snprintf(txt, sizeof(txt), "%c", t->state);
        draw_text_aa(s, 230, ry, txt, fg, &draw_font_ui);
        snprintf(txt, sizeof(txt), "%d%%", t->cpu_pct);
        draw_text_aa(s, 290, ry, txt, fg, &draw_font_ui);
        snprintf(txt, sizeof(txt), "%d.%ds", t->time_ds / 10, t->time_ds % 10);
        draw_text_aa(s, 350, ry, txt, fg, &draw_font_ui);
        /* small CPU bar on the right */
        draw_rect(s, s->w - 90, ry + 3, 80, 10, draw_rgb(220, 224, 228));
        draw_rect(s, s->w - 90, ry + 3, 80 * t->cpu_pct / 100, 10,
                  t->cpu_pct > 70 ? draw_rgb(205, 83, 73)
                                  : draw_rgb(62, 156, 108));
    }

    /* End Task button */
    {
        int by = s->h - 32;
        draw_rect(s, 8, by, 110, 24, sel_pid > 1 ? draw_rgb(205, 83, 73)
                                                 : draw_rgb(190, 194, 198));
        draw_text_aa(s, 8 + (110 - draw_text_width("End Task",
                                                   &draw_font_ui)) / 2,
                     by + 3, "End Task", draw_rgb(245, 246, 244),
                     &draw_font_ui);
        snprintf(txt, sizeof(txt), "%d processes", task_count);
        draw_text_aa(s, 130, by + 3, txt, draw_rgb(102, 110, 112),
                     &draw_font_ui);
    }

    wm_commit(&gui.wm, gui.slot);
    dirty = 0;
}

static void on_click(gui_window_t *g, int x, int y) {
    draw_surface_t *s = &g->surf;

    if (y >= s->h - 32 && y < s->h - 8 && x >= 8 && x < 118) {
        if (sel_pid > 1) {               /* never kill init */
            kill(sel_pid, SIGKILL);
            sel_pid = -1;
            refresh();
        }
        return;
    }
    if (y >= 74 && y < s->h - 40) {
        int idx = view + (y - 74) / 22;
        if (idx >= 0 && idx < task_count) {
            sel_pid = tasks[idx].pid;
            dirty = 1;
        }
    }
}

static void on_scroll(gui_window_t *g, int delta) {
    (void)g;
    view -= delta * 2;
    if (view < 0) view = 0;
    dirty = 1;
}

int main(int argc, char *argv[]) {
    int slot = (argc > 1) ? atoi(argv[1]) : 1;
    int ticks = 0;

    if (slot < 1 || slot > WM_MAX_SLOTS) slot = 1;
    if (gui_open(&gui, slot, "Task Manager", 200 + slot * 14, 80 + slot * 10,
                 560, 420) < 0) {
        printf("taskmgr: desktop unavailable\n");
        return 1;
    }
    gui_set_click_handler(&gui, on_click);
    gui_set_scroll_handler(&gui, on_scroll);

    refresh();
    refresh();   /* second sample primes the CPU deltas */
    render();
    while (!gui.closed) {
        int events = gui_poll(&gui);
        if (++ticks >= 25) {            /* ~1s refresh */
            ticks = 0;
            refresh();
        }
        if (dirty || events > 0)
            render();
        sleep_ms(40);
    }
    gui_close(&gui);
    return 0;
}
