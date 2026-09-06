#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <wm.h>

static void sleep_ms(long ms) {
    struct timespec ts;

    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, 0);
}

static int setup_window(wm_client_t *wm) {
    if (wm_app(wm, 3, "Event Probe") < 0) return -1;
    if (wm_title(wm, 3, "Events") < 0) return -1;
    if (wm_geom(wm, 3, 104, 338, 420, 210) < 0) return -1;
    if (wm_clear(wm, 3) < 0) return -1;
    if (wm_bg(wm, 3, "gray") < 0) return -1;
    if (wm_rect(wm, 3, 14, 44, 250, 22, "cyan") < 0) return -1;
    if (wm_text(wm, 3, 0, "white", "CLICK OR TYPE IN THIS WINDOW") < 0) return -1;
    if (wm_text(wm, 3, 1, "white", "WAITING FOR EVENTS") < 0) return -1;
    if (wm_text(wm, 3, 2, "yellow", "RUNS FOR A SHORT TIME") < 0) return -1;
    if (wm_text(wm, 3, 3, "white", "FOCUS APP 3 ACTIVE") < 0) return -1;
    return wm_focus_app(wm, 3);
}

int main(int argc, char **argv) {
    int ticks = 300;
    int seen = 0;
    wm_client_t wm;
    wm_event_client_t events;

    if (argc > 1) {
        ticks = atoi(argv[1]) * 10;
        if (ticks < 10) ticks = 10;
        if (ticks > 1200) ticks = 1200;
    }

    if (wm_connect(&wm) < 0) {
        printf("wmevtest: desktop control fifo unavailable\n");
        return 1;
    }
    if (wm_open_events(&events, 3) < 0) {
        printf("wmevtest: desktop event fifo unavailable\n");
        wm_close(&wm);
        return 1;
    }
    if (setup_window(&wm) < 0) {
        printf("wmevtest: setup failed\n");
        wm_close_events(&events);
        wm_close(&wm);
        return 1;
    }

    for (int i = 0; i < ticks; i++) {
        wm_event_t ev;
        int got;

        while ((got = wm_next_event(&events, &ev)) > 0) {
            char line[64];
            if (ev.slot != 3) continue;
            seen++;
            if (ev.type == WM_EVENT_MOUSE) {
                snprintf(line, sizeof(line), "MOUSE %d %d BUTTON %d",
                         ev.x, ev.y, ev.button);
                wm_text(&wm, 3, 1, "yellow", line);
            } else if (ev.type == WM_EVENT_KEY) {
                snprintf(line, sizeof(line), "KEY CODE %d ASCII %d",
                         ev.code, ev.ascii);
                wm_text(&wm, 3, 2, "green", line);
            } else if (ev.type == WM_EVENT_FOCUS) {
                wm_text(&wm, 3, 1, "cyan", "FOCUS SLOT 3");
            } else if (ev.type == WM_EVENT_GEOM) {
                snprintf(line, sizeof(line), "GEOM %d %d %d %d",
                         ev.x, ev.y, ev.w, ev.h);
                wm_text(&wm, 3, 2, "yellow", line);
            } else if (ev.type == WM_EVENT_CLOSE) {
                wm_text(&wm, 3, 1, "red", "CLOSE SLOT 3");
            }
            snprintf(line, sizeof(line), "EVENT COUNT %d", seen);
            wm_text(&wm, 3, 3, "white", line);
        }
        sleep_ms(100);
    }

    wm_status(&wm, "EVENT PROBE COMPLETE");
    wm_close_events(&events);
    wm_close(&wm);
    return 0;
}
