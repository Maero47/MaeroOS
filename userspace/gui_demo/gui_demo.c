#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wm.h>

static void sleep_ms(long ms) {
    struct timespec ts;

    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, 0);
}

static int draw_dashboard(wm_client_t *wm, int step) {
    char text[48];
    int cpu = 34 + (step * 11) % 56;
    int mem = 42 + (step * 7) % 46;
    int net = 24 + (step * 17) % 66;

    if (wm_clear(wm, 1) < 0) return -1;
    if (wm_bg(wm, 1, "blue") < 0) return -1;
    if (wm_rect(wm, 1, 16, 44, cpu * 2, 18, "yellow") < 0) return -1;
    if (wm_rect(wm, 1, 16, 76, mem * 2, 18, "green") < 0) return -1;
    if (wm_rect(wm, 1, 16, 108, net * 2, 18, "cyan") < 0) return -1;
    snprintf(text, sizeof(text), "CPU %d%%", cpu);
    if (wm_text(wm, 1, 0, "white", text) < 0) return -1;
    snprintf(text, sizeof(text), "MEM %d%%", mem);
    if (wm_text(wm, 1, 1, "white", text) < 0) return -1;
    snprintf(text, sizeof(text), "NET %d%%", net);
    if (wm_text(wm, 1, 2, "white", text) < 0) return -1;
    return wm_text(wm, 1, 3, "cyan", "GUI DEMO CLIENT");
}

static int draw_notes(wm_client_t *wm, int step) {
    char text[48];

    if (wm_clear(wm, 2) < 0) return -1;
    if (wm_bg(wm, 2, "white") < 0) return -1;
    if (wm_rect(wm, 2, 12, 40, 212, 22, "green") < 0) return -1;
    if (wm_rect(wm, 2, 12, 76, 176, 18, "yellow") < 0) return -1;
    if (wm_rect(wm, 2, 12, 108, 132, 18, "cyan") < 0) return -1;
    if (wm_text(wm, 2, 0, "black", "WINDOW SERVER PROTO") < 0) return -1;
    if (wm_text(wm, 2, 1, "black", "RETAINED RECTS TEXT") < 0) return -1;
    snprintf(text, sizeof(text), "FRAME %d", step);
    if (wm_text(wm, 2, 2, "blue", text) < 0) return -1;
    return wm_text(wm, 2, 3, "gray", "SEPARATE PROCESS");
}

static int draw_launcher(wm_client_t *wm) {
    if (wm_clear(wm, 3) < 0) return -1;
    if (wm_bg(wm, 3, "gray") < 0) return -1;
    if (wm_rect(wm, 3, 14, 42, 58, 58, "red") < 0) return -1;
    if (wm_rect(wm, 3, 86, 42, 58, 58, "yellow") < 0) return -1;
    if (wm_rect(wm, 3, 158, 42, 58, 58, "green") < 0) return -1;
    if (wm_text(wm, 3, 0, "white", "APPS") < 0) return -1;
    if (wm_text(wm, 3, 1, "white", "TERM FILES NET") < 0) return -1;
    if (wm_text(wm, 3, 2, "cyan", "FUTURE DESKTOP") < 0) return -1;
    return wm_text(wm, 3, 3, "white", "MAERO OS");
}

int main(int argc, char **argv) {
    int frames = 24;
    wm_client_t wm;

    if (argc > 1) {
        frames = atoi(argv[1]);
        if (frames < 1) frames = 1;
        if (frames > 240) frames = 240;
    }

    if (wm_connect(&wm) < 0) {
        printf("gui_demo: desktop control fifo unavailable\n");
        return 1;
    }

    if (wm_app(&wm, 1, "Dashboard") < 0 ||
        wm_title(&wm, 1, "Monitor") < 0 ||
        wm_geom(&wm, 1, 104, 74, 376, 230) < 0 ||
        wm_app(&wm, 2, "Notes") < 0 ||
        wm_title(&wm, 2, "Notebook") < 0 ||
        wm_geom(&wm, 2, 512, 74, 392, 230) < 0 ||
        wm_app(&wm, 3, "Launcher") < 0 ||
        wm_title(&wm, 3, "Apps") < 0 ||
        wm_geom(&wm, 3, 104, 338, 376, 190) < 0 ||
        draw_launcher(&wm) < 0) {
        printf("gui_demo: setup failed\n");
        wm_close(&wm);
        return 1;
    }

    for (int i = 0; i < frames; i++) {
        if (draw_dashboard(&wm, i) < 0 || draw_notes(&wm, i) < 0) {
            printf("gui_demo: draw failed\n");
            wm_close(&wm);
            return 1;
        }
        sleep_ms(120);
    }

    wm_focus_app(&wm, 1);
    wm_status(&wm, "GUI DEMO COMPLETE");
    wm_close(&wm);
    return 0;
}
