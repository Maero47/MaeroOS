#include <gui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Widget ids */
#define ID_INPUT   10
#define ID_GREET   11
#define ID_CHECK   12
#define ID_LIST    13
#define ID_SCROLL  14

#define STATUS_IDX 3   /* widget index of the status label */

static const char *fruits[] = {
    "Apple", "Banana", "Cherry", "Date", "Elderberry", "Fig",
    "Grape", "Kiwi", "Lemon", "Mango", "Orange", "Peach",
};
#define FRUITS ((int)(sizeof(fruits) / sizeof(fruits[0])))
#define LIST_ROWS 6

static gui_window_t gui;

static void sleep_ms(long ms) {
    struct timespec ts;

    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, 0);
}

static void set_status(const char *text) {
    strncpy(gui.widgets[STATUS_IDX].text, text, GUI_MAX_LABEL - 1);
    gui.widgets[STATUS_IDX].text[GUI_MAX_LABEL - 1] = 0;
}

static void on_greet(gui_window_t *g, int id) {
    char line[GUI_MAX_LABEL];

    (void)id;
    snprintf(line, sizeof(line), "Hello, %s!", gui_input_text(g, ID_INPUT));
    set_status(line);
    gui_draw(g);
}

static void on_toggle(gui_window_t *g, int id) {
    set_status(gui_value(g, id) ? "Option enabled" : "Option disabled");
    gui_draw(g);
}

static void on_select(gui_window_t *g, int id) {
    char line[GUI_MAX_LABEL];
    int sel = gui_value(g, id);

    if (sel >= 0 && sel < FRUITS) {
        snprintf(line, sizeof(line), "Picked %s", fruits[sel]);
        set_status(line);
        gui_draw(g);
    }
}

static void on_scroll(gui_window_t *g, int id) {
    gui_list_set_scroll(g, ID_LIST, gui_value(g, id));
    gui_draw(g);
}

int main(int argc, char *argv[]) {
    int slot = (argc > 1) ? atoi(argv[1]) : 2;

    if (slot < 1 || slot > WM_MAX_SLOTS) slot = 2;
    if (gui_open(&gui, slot, "Widgets", 512, 200, 380, 360) < 0) {
        printf("uidemo: desktop unavailable\n");
        return 1;
    }

    strcpy(gui.bg, "white");
    if (gui_label(&gui, 16, 16, "Type a name and press Enter:", "black") < 0 ||
        gui_textinput(&gui, ID_INPUT, 16, 38, 238, "world", on_greet) < 0 ||
        gui_button(&gui, ID_GREET, 264, 38, 84, 24, "Greet", on_greet) < 0 ||
        gui_label(&gui, 16, 76, "Hello, world!", "#5e81ac") < 0 ||
        gui_checkbox(&gui, ID_CHECK, 16, 102, "Enable option", 0, on_toggle) < 0 ||
        gui_label(&gui, 16, 134, "Pick a fruit:", "black") < 0 ||
        gui_listbox(&gui, ID_LIST, 16, 156, 238, LIST_ROWS,
                    fruits, FRUITS, on_select) < 0 ||
        gui_scrollbar(&gui, ID_SCROLL, 264, 156, LIST_ROWS * GUI_LIST_ROW_H + 4,
                      FRUITS - LIST_ROWS, on_scroll) < 0 ||
        gui_draw(&gui) < 0) {
        printf("uidemo: setup failed\n");
        gui_close(&gui);
        return 1;
    }
    gui_bind_scroll(&gui, ID_LIST, ID_SCROLL);

    wm_status(&gui.wm, "WIDGETS RUNNING");
    while (!gui.closed) {
        gui_poll(&gui);
        sleep_ms(100);
    }

    wm_status(&gui.wm, "WIDGETS CLOSED");
    gui_close(&gui);
    return 0;
}
