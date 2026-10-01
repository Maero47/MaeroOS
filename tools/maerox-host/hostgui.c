/*
 * Host stand-in for libgui: maeroX built for Linux (tools/maerox-host/run.sh)
 * so X clients from the Alpine root can be run against it on the build host
 * in seconds instead of a guest boot.  The "desktop window" is a buffer; the
 * control FIFO ($HX_CTL) takes "k <keycode> <0|1> <mods>", "p <x> <y>
 * <buttons>", "s <delta>", "d <file.ppm>" (dump the screen) and "q".
 */
#include <gui.h>
#include <wm.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
static int ctl = -1; static char line[256]; static int used;
int gui_open(gui_window_t *g, int slot, const char *t, int x, int y, int w, int h) {
    memset(g, 0, sizeof(*g)); g->slot = slot; g->width = w; g->height = h;
    g->surf.w = w; g->surf.h = h; g->surf.px = calloc((size_t)w * h, 4);
    const char *p = getenv("HX_CTL"); if (!p) p = "/tmp/hx.ctl";
    unlink(p); mkfifo(p, 0600); ctl = open(p, O_RDWR | O_NONBLOCK);
    return 0;
}
void gui_close(gui_window_t *g) { (void)g; }
void gui_set_ptr_handler(gui_window_t *g, gui_ptr_cb cb) { g->on_ptr = cb; }
void gui_set_rawkey_handler(gui_window_t *g, gui_rawkey_cb cb) { g->on_rawkey = cb; }
void gui_set_scroll_handler(gui_window_t *g, gui_scroll_cb cb) { g->on_scroll = cb; }
int wm_commit(wm_client_t *w, int s) { (void)w; (void)s; return 0; }
int wm_commit_rect(wm_client_t *w, int s, int x, int y, int ww, int hh) { (void)w; (void)s; (void)x; (void)y; (void)ww; (void)hh; return 0; }
static void dump(gui_window_t *g, const char *f) {
    FILE *o = fopen(f, "wb"); if (!o) return;
    fprintf(o, "P6\n%d %d\n255\n", g->surf.w, g->surf.h);
    for (int i = 0; i < g->surf.w * g->surf.h; i++) { uint32_t p = g->surf.px[i];
        fputc((p >> 16) & 255, o); fputc((p >> 8) & 255, o); fputc(p & 255, o); }
    fclose(o);
}
static void cmd(gui_window_t *g, char *l) {
    int a, b, c; char f[200];
    if (sscanf(l, "k %d %d %d", &a, &b, &c) == 3 && g->on_rawkey) g->on_rawkey(g, a, b, c);
    else if (sscanf(l, "p %d %d %d", &a, &b, &c) == 3 && g->on_ptr) g->on_ptr(g, a, b, c, 1);
    else if (sscanf(l, "s %d", &a) == 1 && g->on_scroll) g->on_scroll(g, a);
    else if (sscanf(l, "d %199s", f) == 1) dump(g, f);
    else if (!strcmp(l, "q")) g->closed = 1;
}
int gui_poll(gui_window_t *g) {
    char ch;
    while (ctl >= 0 && read(ctl, &ch, 1) == 1) {
        if (ch != '\n') { if (used < 255) line[used++] = ch; continue; }
        line[used] = 0; used = 0; cmd(g, line);
    }
    return 0;
}
