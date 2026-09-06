#include <draw.h>
#include <gui.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/*
 * calc — integer calculator with a button grid and keyboard input.
 * (32-bit integer arithmetic; division truncates — no floating point, since
 * the kernel does not preserve FPU state across context switches.)
 */

static gui_window_t gui;
static long acc, entry;
static char pending_op;
static int entering;          /* digits typed since the last op */
static int dirty = 1;
static char err[16];

#define BTN_W 64
#define BTN_H 44
#define GRID_X 10
#define GRID_Y 64

static const char *grid[5][4] = {
    { "C",  "+/-", "%", "/" },
    { "7",  "8",  "9",  "*" },
    { "4",  "5",  "6",  "-" },
    { "1",  "2",  "3",  "+" },
    { "0",  "00", "<",  "=" },
};

static void sleep_ms(long ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, 0);
}

static void apply_op(void) {
    err[0] = 0;
    switch (pending_op) {
    case '+': acc += entry; break;
    case '-': acc -= entry; break;
    case '*': acc *= entry; break;
    case '/':
        if (entry == 0) { strcpy(err, "Divide by 0"); entry = 0; return; }
        acc /= entry;
        break;
    case '%':
        if (entry == 0) { strcpy(err, "Divide by 0"); entry = 0; return; }
        acc %= entry;
        break;
    default:  acc = entry; break;
    }
    entry = acc;
}

static void press(const char *label) {
    char c = label[0];

    if (c >= '0' && c <= '9') {
        if (!entering) { entry = 0; entering = 1; }
        for (const char *p = label; *p; p++) {
            if (entry < 199999999L)
                entry = entry * 10 + (*p - '0');
        }
    } else if (c == 'C') {
        acc = entry = 0;
        pending_op = 0;
        entering = 0;
        err[0] = 0;
    } else if (c == '<') {
        entry /= 10;
    } else if (!strcmp(label, "+/-")) {
        entry = -entry;
    } else if (c == '=') {
        apply_op();
        pending_op = 0;
        entering = 0;
    } else {                       /* + - * / % */
        if (entering && pending_op) apply_op();
        else if (!pending_op) acc = entry;
        pending_op = c;
        entering = 0;
    }
    dirty = 1;
}

static void render(void) {
    draw_surface_t *s = &gui.surf;
    char txt[40];

    if (!s->px) return;
    draw_fill(s, draw_rgb(238, 240, 243));

    /* display */
    draw_rect(s, GRID_X, 10, 4 * (BTN_W + 6) - 6, 42, draw_rgb(28, 32, 42));
    if (err[0])
        snprintf(txt, sizeof(txt), "%s", err);
    else
        snprintf(txt, sizeof(txt), "%ld", entry);
    draw_text_aa(s, GRID_X + 4 * (BTN_W + 6) - 12 -
                 draw_text_width(txt, &draw_font_ui_big),
                 18, txt, draw_rgb(220, 230, 240), &draw_font_ui_big);

    for (int r = 0; r < 5; r++) {
        for (int c = 0; c < 4; c++) {
            int bx = GRID_X + c * (BTN_W + 6);
            int by = GRID_Y + r * (BTN_H + 6);
            const char *lab = grid[r][c];
            int op = !(lab[0] >= '0' && lab[0] <= '9');
            uint32_t bg = lab[0] == '=' ? draw_rgb(94, 129, 172) :
                          op ? draw_rgb(208, 213, 219) : draw_rgb(252, 252, 252);
            draw_rect(s, bx, by, BTN_W, BTN_H, bg);
            draw_frame(s, bx, by, BTN_W, BTN_H, draw_rgb(180, 186, 194));
            draw_text_aa(s, bx + (BTN_W - draw_text_width(lab,
                                                          &draw_font_ui)) / 2,
                         by + (BTN_H - draw_font_ui.line_h) / 2, lab,
                         lab[0] == '=' ? draw_rgb(245, 248, 250)
                                       : draw_rgb(30, 34, 36),
                         &draw_font_ui);
        }
    }
    wm_commit(&gui.wm, gui.slot);
    dirty = 0;
}

static void on_click(gui_window_t *g, int x, int y) {
    (void)g;
    if (x < GRID_X || y < GRID_Y) return;
    {
        int c = (x - GRID_X) / (BTN_W + 6);
        int r = (y - GRID_Y) / (BTN_H + 6);
        if (r >= 0 && r < 5 && c >= 0 && c < 4 &&
            (x - GRID_X) % (BTN_W + 6) < BTN_W &&
            (y - GRID_Y) % (BTN_H + 6) < BTN_H)
            press(grid[r][c]);
    }
}

static void on_key(gui_window_t *g, int code, int value, int ascii) {
    (void)g;
    (void)value;
    if (ascii >= '0' && ascii <= '9') {
        char lab[2] = { (char)ascii, 0 };
        press(lab);
    } else if (ascii == '+' || ascii == '-' || ascii == '*' ||
               ascii == '/' || ascii == '%') {
        char lab[2] = { (char)ascii, 0 };
        press(lab);
    } else if (code == KEY_ENTER || ascii == '=') {
        press("=");
    } else if (code == KEY_BACKSPACE) {
        press("<");
    } else if (ascii == 'c' || ascii == 'C') {
        press("C");
    }
}

int main(int argc, char *argv[]) {
    int slot = (argc > 1) ? atoi(argv[1]) : 1;

    if (slot < 1 || slot > WM_MAX_SLOTS) slot = 1;
    if (gui_open(&gui, slot, "Calculator", 320 + slot * 12, 120 + slot * 10,
                 4 * (BTN_W + 6) + 2 * GRID_X + 8,
                 GRID_Y + 5 * (BTN_H + 6) + 36) < 0) {
        printf("calc: desktop unavailable\n");
        return 1;
    }
    gui_set_click_handler(&gui, on_click);
    gui_set_key_handler(&gui, on_key);

    render();
    while (!gui.closed) {
        int events = gui_poll(&gui);
        if (dirty || events > 0)
            render();
        sleep_ms(40);
    }
    gui_close(&gui);
    return 0;
}
