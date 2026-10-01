#include <draw.h>
#include <fcntl.h>
#include <gui.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/*
 * GUI Terminal — a regular window-manager client with its own PTY + shell.
 *
 * A VT100/xterm-style emulator: a cell grid sized to the window (the size
 * goes to the PTY with TIOCSWINSZ, so vi/less/top fill it and follow
 * resizes), cursor addressing, scroll regions, 16/256 colours and SGR
 * attributes, the alternate screen, application cursor keys, bracketed
 * paste, and a scrollback for the main screen.  Text is UTF-8 both ways.
 *
 * Mouse: drag selects; Ctrl+Shift+C copies the selection to the desktop
 * clipboard, Ctrl+Shift+V pastes it.  Wheel or Shift+PgUp/PgDn scroll back.
 * Multiple instances run side by side (the launcher hands each one a free
 * slot via argv[1]).
 */

#define TIOCGPTN   0x80045430U
#define TIOCSPTLCK 0x40045431U
#ifndef TIOCSWINSZ
#define TIOCSWINSZ 0x5414
#endif
#ifndef TIOCSCTTY
#define TIOCSCTTY  0x540E
#endif

#define MAX_COLS   200
#define MAX_ROWS   80
#define SCROLLBACK 500

#define TERM_LH  DRAW_MONO_LH    /* line pitch (px) */
#define TERM_CW  DRAW_MONO_CW    /* monospace cell width (px) */
#define PAD_X    6
#define PAD_Y    4

/* Cell attributes */
#define A_BOLD    0x01
#define A_UNDER   0x02
#define A_REV     0x04
#define A_FGDEF   0x08            /* default foreground (fg ignored) */
#define A_BGDEF   0x10            /* default background (bg ignored) */
#define A_DIM     0x20

typedef struct {
    uint16_t ch;                  /* BMP code point; 0 = blank */
    uint8_t fg, bg;               /* xterm 256-colour indexes */
    uint8_t attr;
} cell_t;

typedef struct {
    cell_t cells[MAX_ROWS][MAX_COLS];
    int cx, cy;                   /* cursor */
    int saved_cx, saved_cy;
    uint8_t saved_attr, saved_fg, saved_bg;
} screen_t;

static gui_window_t gui;
static int shell_pid = -1;
static int master_fd = -1;

static screen_t screens[2];       /* [0] main, [1] alternate */
static screen_t *scr = &screens[0];
static int alt_active;
static int cols = 80, rows = 24;

static cell_t history[SCROLLBACK][MAX_COLS];
static int hist_head, hist_count; /* ring: oldest at hist_head */
static int view;                  /* lines scrolled back, 0 = live */

/* Current pen */
static uint8_t cur_attr = A_FGDEF | A_BGDEF;
static uint8_t cur_fg = 7, cur_bg = 0;
static int wrap_pending;          /* xterm deferred autowrap */
static int top_margin, bot_margin;/* scroll region (inclusive rows) */
static int cursor_visible = 1;
static int app_cursor;            /* DECCKM: ESC O A for the arrows */
static int bracketed_paste;
static int autowrap = 1;
static int insert_mode;
static int charset_g0_dec;        /* ESC ( 0: DEC special graphics */
static uint8_t tabstops[MAX_COLS];

/* Parser */
enum { ST_GROUND, ST_ESC, ST_CSI, ST_OSC, ST_OSC_ESC, ST_CHARSET };
static int pstate;
static int params[16];
static int nparams;
static int private_mark;          /* '?' or '>' etc. */
static unsigned utf_cp;
static int utf_need;

/* Selection (in "absolute" line numbers: history lines then screen rows) */
static int sel_active, sel_dragging;
static int sel_a_line, sel_a_col, sel_b_line, sel_b_col;

static int dirty = 1;

static void sleep_ms(long ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, 0);
}

static void pty_write(const char *s, int n) {
    if (master_fd >= 0 && n > 0) write(master_fd, s, (size_t)n);
}

static void pty_puts(const char *s) {
    pty_write(s, (int)strlen(s));
}

/* ── Screen operations ─────────────────────────────────────────────────── */

static cell_t blank_cell(void) {
    cell_t c;
    c.ch = 0;
    c.fg = cur_fg;
    c.bg = cur_bg;
    /* Erased cells take the current background (xterm BCE). */
    c.attr = (uint8_t)((cur_attr & A_BGDEF) | A_FGDEF);
    return c;
}

static void clear_cells(int row, int c0, int c1) {
    cell_t b = blank_cell();
    if (row < 0 || row >= rows) return;
    if (c0 < 0) c0 = 0;
    if (c1 > cols) c1 = cols;
    for (int c = c0; c < c1; c++) scr->cells[row][c] = b;
}

static void clear_rows(int r0, int r1) {
    for (int r = r0; r < r1; r++) clear_cells(r, 0, cols);
}

static void push_history(const cell_t *line) {
    int slot;
    if (hist_count < SCROLLBACK) {
        slot = (hist_head + hist_count) % SCROLLBACK;
        hist_count++;
    } else {
        slot = hist_head;
        hist_head = (hist_head + 1) % SCROLLBACK;
    }
    memcpy(history[slot], line, sizeof(cell_t) * MAX_COLS);
    if (view) view++;            /* keep the scrolled-back view still */
}

static void scroll_up(int top, int bot, int n) {
    if (n <= 0) return;
    if (n > bot - top + 1) n = bot - top + 1;
    for (int i = 0; i < n; i++) {
        if (!alt_active && top == 0) push_history(scr->cells[top]);
        memmove(scr->cells[top], scr->cells[top + 1],
                sizeof(scr->cells[0]) * (size_t)(bot - top));
        clear_cells(bot, 0, MAX_COLS);
    }
}

static void scroll_down(int top, int bot, int n) {
    if (n <= 0) return;
    if (n > bot - top + 1) n = bot - top + 1;
    for (int i = 0; i < n; i++) {
        memmove(scr->cells[top + 1], scr->cells[top],
                sizeof(scr->cells[0]) * (size_t)(bot - top));
        clear_cells(top, 0, MAX_COLS);
    }
}

static void index_down(void) {          /* LF / IND */
    if (scr->cy == bot_margin) scroll_up(top_margin, bot_margin, 1);
    else if (scr->cy < rows - 1) scr->cy++;
}

static void reverse_index(void) {       /* RI */
    if (scr->cy == top_margin) scroll_down(top_margin, bot_margin, 1);
    else if (scr->cy > 0) scr->cy--;
}

static void clamp_cursor(void) {
    if (scr->cx < 0) scr->cx = 0;
    if (scr->cx >= cols) scr->cx = cols - 1;
    if (scr->cy < 0) scr->cy = 0;
    if (scr->cy >= rows) scr->cy = rows - 1;
}

static void reset_tabs(void) {
    for (int i = 0; i < MAX_COLS; i++) tabstops[i] = (i % 8) == 0;
}

static void reset_terminal(void) {
    cur_attr = A_FGDEF | A_BGDEF;
    cur_fg = 7;
    cur_bg = 0;
    top_margin = 0;
    bot_margin = rows - 1;
    cursor_visible = 1;
    app_cursor = 0;
    bracketed_paste = 0;
    autowrap = 1;
    insert_mode = 0;
    charset_g0_dec = 0;
    wrap_pending = 0;
    reset_tabs();
    scr = &screens[0];
    alt_active = 0;
    clear_rows(0, rows);
    scr->cx = scr->cy = 0;
}

static void set_alt_screen(int on) {
    if (on == alt_active) return;
    if (on) {
        screens[0].saved_cx = scr->cx;
        screens[0].saved_cy = scr->cy;
        screens[1].cx = scr->cx;
        screens[1].cy = scr->cy;
        scr = &screens[1];
        alt_active = 1;
        clear_rows(0, rows);
    } else {
        scr = &screens[0];
        alt_active = 0;
        scr->cx = screens[0].saved_cx;
        scr->cy = screens[0].saved_cy;
        clamp_cursor();
    }
    view = 0;
    sel_active = 0;
    gui_trace("term", "altscreen %s", on ? "on" : "off");
}

/* DEC special graphics (ESC ( 0) as the box-drawing code points. */
static unsigned dec_graphics(unsigned c) {
    switch (c) {
    case 'j': return 0x2518; case 'k': return 0x2510; case 'l': return 0x250C;
    case 'm': return 0x2514; case 'n': return 0x253C; case 'q': return 0x2500;
    case 't': return 0x251C; case 'u': return 0x2524; case 'v': return 0x2534;
    case 'w': return 0x252C; case 'x': return 0x2502; case 'a': return 0x2592;
    case '`': return 0x25C6; case '~': return 0xB7;
    }
    return c;
}

static void put_char(unsigned cp) {
    cell_t *cell;

    if (charset_g0_dec && cp < 128) cp = dec_graphics(cp);
    if (cp > 0xFFFF) cp = 0xFFFD;
    if (wrap_pending) {
        wrap_pending = 0;
        if (autowrap) {
            scr->cx = 0;
            index_down();
        }
    }
    if (insert_mode && scr->cx < cols - 1)
        memmove(&scr->cells[scr->cy][scr->cx + 1], &scr->cells[scr->cy][scr->cx],
                sizeof(cell_t) * (size_t)(cols - scr->cx - 1));
    cell = &scr->cells[scr->cy][scr->cx];
    cell->ch = (uint16_t)cp;
    cell->fg = cur_fg;
    cell->bg = cur_bg;
    cell->attr = cur_attr;
    if (scr->cx == cols - 1) wrap_pending = 1;
    else scr->cx++;
}

/* ── Escape sequences ──────────────────────────────────────────────────── */

static int param(int i, int def) {
    return (i < nparams && params[i] > 0) ? params[i] : def;
}

static void sgr(void) {
    if (nparams == 0) { cur_attr = A_FGDEF | A_BGDEF; return; }
    for (int i = 0; i < nparams; i++) {
        int p = params[i];
        if (p == 0) cur_attr = A_FGDEF | A_BGDEF;
        else if (p == 1) cur_attr |= A_BOLD;
        else if (p == 2) cur_attr |= A_DIM;
        else if (p == 4) cur_attr |= A_UNDER;
        else if (p == 7) cur_attr |= A_REV;
        else if (p == 22) cur_attr &= (uint8_t)~(A_BOLD | A_DIM);
        else if (p == 24) cur_attr &= (uint8_t)~A_UNDER;
        else if (p == 27) cur_attr &= (uint8_t)~A_REV;
        else if (p >= 30 && p <= 37) { cur_fg = (uint8_t)(p - 30); cur_attr &= (uint8_t)~A_FGDEF; }
        else if (p == 39) cur_attr |= A_FGDEF;
        else if (p >= 40 && p <= 47) { cur_bg = (uint8_t)(p - 40); cur_attr &= (uint8_t)~A_BGDEF; }
        else if (p == 49) cur_attr |= A_BGDEF;
        else if (p >= 90 && p <= 97) { cur_fg = (uint8_t)(p - 90 + 8); cur_attr &= (uint8_t)~A_FGDEF; }
        else if (p >= 100 && p <= 107) { cur_bg = (uint8_t)(p - 100 + 8); cur_attr &= (uint8_t)~A_BGDEF; }
        else if ((p == 38 || p == 48) && i + 2 < nparams && params[i + 1] == 5) {
            if (p == 38) { cur_fg = (uint8_t)params[i + 2]; cur_attr &= (uint8_t)~A_FGDEF; }
            else { cur_bg = (uint8_t)params[i + 2]; cur_attr &= (uint8_t)~A_BGDEF; }
            i += 2;
        } else if ((p == 38 || p == 48) && i + 4 < nparams && params[i + 1] == 2) {
            /* Truecolour: nearest of the 6x6x6 cube. */
            int r = params[i + 2] * 5 / 255, g = params[i + 3] * 5 / 255;
            int b = params[i + 4] * 5 / 255;
            uint8_t idx = (uint8_t)(16 + r * 36 + g * 6 + b);
            if (p == 38) { cur_fg = idx; cur_attr &= (uint8_t)~A_FGDEF; }
            else { cur_bg = idx; cur_attr &= (uint8_t)~A_BGDEF; }
            i += 4;
        }
    }
}

static void set_mode(int on) {
    for (int i = 0; i < nparams || i == 0; i++) {
        int p = i < nparams ? params[i] : 0;
        if (private_mark == '?') {
            switch (p) {
            case 1:    app_cursor = on; break;
            case 7:    autowrap = on; break;
            case 25:   cursor_visible = on; break;
            case 47: case 1047: set_alt_screen(on); break;
            case 1048:
                if (on) { scr->saved_cx = scr->cx; scr->saved_cy = scr->cy; }
                else { scr->cx = scr->saved_cx; scr->cy = scr->saved_cy; }
                break;
            case 1049:
                if (on) {
                    screens[0].saved_cx = scr->cx;
                    screens[0].saved_cy = scr->cy;
                }
                set_alt_screen(on);
                break;
            case 2004: bracketed_paste = on; break;
            }
        } else if (p == 4) {
            insert_mode = on;
        }
    }
}

static void csi_dispatch(char f) {
    char reply[32];
    int n;

    if (private_mark && private_mark != '?' && f != 'c') return;
    switch (f) {
    case 'A': scr->cy -= param(0, 1);
              if (scr->cy < top_margin && scr->cy + param(0, 1) >= top_margin)
                  scr->cy = top_margin;
              break;
    case 'B': case 'e':
              scr->cy += param(0, 1);
              if (scr->cy > bot_margin && scr->cy - param(0, 1) <= bot_margin)
                  scr->cy = bot_margin;
              break;
    case 'C': case 'a': scr->cx += param(0, 1); break;
    case 'D': scr->cx -= param(0, 1); break;
    case 'E': scr->cy += param(0, 1); scr->cx = 0; break;
    case 'F': scr->cy -= param(0, 1); scr->cx = 0; break;
    case 'G': case '`': scr->cx = param(0, 1) - 1; break;
    case 'd': scr->cy = param(0, 1) - 1; break;
    case 'H': case 'f':
        scr->cy = param(0, 1) - 1;
        scr->cx = param(1, 1) - 1;
        break;
    case 'J': {
        int m = nparams ? params[0] : 0;
        if (m == 0) {
            clear_cells(scr->cy, scr->cx, cols);
            clear_rows(scr->cy + 1, rows);
        } else if (m == 1) {
            clear_rows(0, scr->cy);
            clear_cells(scr->cy, 0, scr->cx + 1);
        } else if (m == 2 || m == 3) {
            clear_rows(0, rows);
            if (m == 3) hist_count = 0;
        }
        break;
    }
    case 'K': {
        int m = nparams ? params[0] : 0;
        if (m == 0) clear_cells(scr->cy, scr->cx, cols);
        else if (m == 1) clear_cells(scr->cy, 0, scr->cx + 1);
        else clear_cells(scr->cy, 0, cols);
        break;
    }
    case 'L':
        if (scr->cy >= top_margin && scr->cy <= bot_margin)
            scroll_down(scr->cy, bot_margin, param(0, 1));
        break;
    case 'M':
        if (scr->cy >= top_margin && scr->cy <= bot_margin) {
            /* Deleting lines never feeds the scrollback. */
            int save = alt_active;
            alt_active = 1;
            scroll_up(scr->cy, bot_margin, param(0, 1));
            alt_active = save;
        }
        break;
    case 'P': {
        int k = param(0, 1);
        if (k > cols - scr->cx) k = cols - scr->cx;
        memmove(&scr->cells[scr->cy][scr->cx], &scr->cells[scr->cy][scr->cx + k],
                sizeof(cell_t) * (size_t)(cols - scr->cx - k));
        clear_cells(scr->cy, cols - k, cols);
        break;
    }
    case '@': {
        int k = param(0, 1);
        if (k > cols - scr->cx) k = cols - scr->cx;
        memmove(&scr->cells[scr->cy][scr->cx + k], &scr->cells[scr->cy][scr->cx],
                sizeof(cell_t) * (size_t)(cols - scr->cx - k));
        clear_cells(scr->cy, scr->cx, scr->cx + k);
        break;
    }
    case 'X': clear_cells(scr->cy, scr->cx, scr->cx + param(0, 1)); break;
    case 'S': {
        int save = alt_active;
        alt_active = 1;
        scroll_up(top_margin, bot_margin, param(0, 1));
        alt_active = save;
        break;
    }
    case 'T': scroll_down(top_margin, bot_margin, param(0, 1)); break;
    case 'm': sgr(); break;
    case 'r': {
        int t = param(0, 1) - 1, b = param(1, rows) - 1;
        if (b >= rows) b = rows - 1;
        if (t < b) {
            top_margin = t;
            bot_margin = b;
            scr->cx = 0;
            scr->cy = 0;
        }
        break;
    }
    case 'h': set_mode(1); break;
    case 'l': set_mode(0); break;
    case 's': scr->saved_cx = scr->cx; scr->saved_cy = scr->cy; break;
    case 'u': scr->cx = scr->saved_cx; scr->cy = scr->saved_cy; break;
    case 'n':
        if (param(0, 0) == 6) {
            n = snprintf(reply, sizeof(reply), "\033[%d;%dR",
                         scr->cy + 1, scr->cx + 1);
            pty_write(reply, n);
        } else if (param(0, 0) == 5) {
            pty_puts("\033[0n");
        }
        break;
    case 'c':
        if (private_mark == '>') pty_puts("\033[>0;10;0c");
        else if (!private_mark) pty_puts("\033[?1;2c");
        break;
    case 'g':
        if (param(0, 0) == 3) memset(tabstops, 0, sizeof(tabstops));
        else if (scr->cx < MAX_COLS) tabstops[scr->cx] = 0;
        break;
    case 't':                         /* window ops: report the size */
        if (param(0, 0) == 18) {
            n = snprintf(reply, sizeof(reply), "\033[8;%d;%dt", rows, cols);
            pty_write(reply, n);
        }
        break;
    }
    wrap_pending = 0;
    clamp_cursor();
}

static void esc_dispatch(char c) {
    switch (c) {
    case '7':
        scr->saved_cx = scr->cx; scr->saved_cy = scr->cy;
        scr->saved_attr = cur_attr; scr->saved_fg = cur_fg; scr->saved_bg = cur_bg;
        break;
    case '8':
        scr->cx = scr->saved_cx; scr->cy = scr->saved_cy;
        cur_attr = scr->saved_attr ? scr->saved_attr : (A_FGDEF | A_BGDEF);
        cur_fg = scr->saved_fg; cur_bg = scr->saved_bg;
        clamp_cursor();
        break;
    case 'D': index_down(); break;
    case 'E': scr->cx = 0; index_down(); break;
    case 'M': reverse_index(); break;
    case 'H': if (scr->cx < MAX_COLS) tabstops[scr->cx] = 1; break;
    case 'c': reset_terminal(); break;
    }
    wrap_pending = 0;
}

static void feed_byte(unsigned char c) {
    /* UTF-8 assembly happens in the ground state only. */
    if (pstate == ST_GROUND && utf_need) {
        if ((c & 0xC0) == 0x80) {
            utf_cp = (utf_cp << 6) | (c & 0x3F);
            if (--utf_need == 0) put_char(utf_cp);
            return;
        }
        utf_need = 0;
        put_char(0xFFFD);
    }
    if (c == 0x18 || c == 0x1A) { pstate = ST_GROUND; return; }  /* CAN/SUB */
    if (c == 27 && pstate != ST_OSC) {
        pstate = ST_ESC;
        return;
    }
    if (c < 32 && pstate != ST_OSC) {          /* C0 controls act anywhere */
        switch (c) {
        case 7: break;                          /* BEL */
        case 8: if (scr->cx > 0) scr->cx--; wrap_pending = 0; break;
        case 9: {
            int x = scr->cx + 1;
            while (x < cols - 1 && !tabstops[x]) x++;
            scr->cx = x < cols ? x : cols - 1;
            break;
        }
        case 10: case 11: case 12: index_down(); wrap_pending = 0; break;
        case 13: scr->cx = 0; wrap_pending = 0; break;
        case 14: case 15: break;                /* SO/SI */
        }
        return;
    }
    switch (pstate) {
    case ST_GROUND:
        if (c < 0x80) { if (c != 127) put_char(c); }
        else if ((c & 0xE0) == 0xC0) { utf_cp = c & 0x1F; utf_need = 1; }
        else if ((c & 0xF0) == 0xE0) { utf_cp = c & 0x0F; utf_need = 2; }
        else if ((c & 0xF8) == 0xF0) { utf_cp = c & 0x07; utf_need = 3; }
        else put_char(0xFFFD);
        break;
    case ST_ESC:
        if (c == '[') {
            pstate = ST_CSI;
            nparams = 0;
            private_mark = 0;
            memset(params, 0, sizeof(params));
        } else if (c == ']') {
            pstate = ST_OSC;
        } else if (c == '(' || c == ')' || c == '*' || c == '+') {
            pstate = c == '(' ? ST_CHARSET : ST_CHARSET + 100;
        } else if (c == '#' || c == '%' || c == ' ') {
            pstate = ST_CHARSET + 100;          /* swallow one more byte */
        } else {
            esc_dispatch((char)c);
            pstate = ST_GROUND;
        }
        break;
    case ST_CHARSET:
        charset_g0_dec = c == '0';
        pstate = ST_GROUND;
        break;
    case ST_CHARSET + 100:
        pstate = ST_GROUND;
        break;
    case ST_CSI:
        if (c >= '0' && c <= '9') {
            if (nparams == 0) nparams = 1;
            if (params[nparams - 1] < 10000)
                params[nparams - 1] = params[nparams - 1] * 10 + (c - '0');
        } else if (c == ';' || c == ':') {
            if (nparams == 0) nparams = 1;
            if (nparams < 16) nparams++;
        } else if (c >= 0x3C && c <= 0x3F) {
            private_mark = c;
        } else if (c >= 0x20 && c <= 0x2F) {
            /* intermediates (e.g. "CSI ! p", "CSI SP q"): ignore */
            private_mark = private_mark ? private_mark : c;
        } else if (c >= 0x40 && c <= 0x7E) {
            csi_dispatch((char)c);
            pstate = ST_GROUND;
        }
        break;
    case ST_OSC:                                /* titles etc.: ignored */
        if (c == 7) pstate = ST_GROUND;
        else if (c == 27) pstate = ST_OSC_ESC;
        break;
    case ST_OSC_ESC:
        pstate = c == '\\' ? ST_GROUND : ST_OSC;
        break;
    }
}

static void feed_output(const char *buf, int n) {
    for (int i = 0; i < n; i++) feed_byte((unsigned char)buf[i]);
    dirty = 1;
}

/* ── Rendering ─────────────────────────────────────────────────────────── */

static uint32_t palette_color(int idx) {
    static const uint32_t base[16] = {
        0x1c1f26, 0xd0675f, 0x8fbf6f, 0xe0c070, 0x5e95d4, 0xb48ead, 0x63b5bf,
        0xd8dce2, 0x5c6370, 0xef8078, 0xa7d68a, 0xf0d58c, 0x7fb1eb, 0xcba6c6,
        0x88cfd6, 0xffffff,
    };
    if (idx < 16) return base[idx];
    if (idx < 232) {
        int v = idx - 16, r = v / 36, g = (v / 6) % 6, b = v % 6;
        int lv[6] = { 0, 95, 135, 175, 215, 255 };
        return draw_rgb((unsigned)lv[r], (unsigned)lv[g], (unsigned)lv[b]);
    }
    {
        unsigned l = (unsigned)(8 + (idx - 232) * 10);
        return draw_rgb(l, l, l);
    }
}

#define COL_BG draw_rgb(24, 27, 33)
#define COL_FG draw_rgb(222, 226, 230)

/* The line shown at visible row r: history while scrolled back. */
static const cell_t *visible_line(int r, int *abs_line) {
    int first = hist_count - view;          /* absolute index of row 0 */
    int a = first + r;
    *abs_line = a;
    if (a < hist_count)
        return history[(hist_head + a) % SCROLLBACK];
    return scr->cells[a - hist_count];
}

static void sel_bounds(int *l0, int *c0, int *l1, int *c1) {
    if (sel_a_line < sel_b_line ||
        (sel_a_line == sel_b_line && sel_a_col <= sel_b_col)) {
        *l0 = sel_a_line; *c0 = sel_a_col; *l1 = sel_b_line; *c1 = sel_b_col;
    } else {
        *l0 = sel_b_line; *c0 = sel_b_col; *l1 = sel_a_line; *c1 = sel_a_col;
    }
}

static int in_selection(int line, int col) {
    int l0, c0, l1, c1;
    if (!sel_active) return 0;
    sel_bounds(&l0, &c0, &l1, &c1);
    if (line < l0 || line > l1) return 0;
    if (line == l0 && col < c0) return 0;
    if (line == l1 && col > c1) return 0;
    return 1;
}

static void render(void) {
    draw_surface_t *s = &gui.surf;

    if (!s->px) return;
    draw_fill(s, COL_BG);
    for (int r = 0; r < rows; r++) {
        int abs_line;
        const cell_t *line = visible_line(r, &abs_line);
        int y = PAD_Y + r * TERM_LH;
        for (int c = 0; c < cols; c++) {
            const cell_t *cell = &line[c];
            uint32_t fg, bg;
            int has_bg = !(cell->attr & A_BGDEF);
            int x = PAD_X + c * TERM_CW;

            fg = (cell->attr & A_FGDEF) ? COL_FG :
                 palette_color((cell->attr & A_BOLD) && cell->fg < 8 ?
                               cell->fg + 8 : cell->fg);
            bg = has_bg ? palette_color(cell->bg) : COL_BG;
            if (cell->attr & A_REV) {
                uint32_t t = fg; fg = bg; bg = t;
                has_bg = 1;
            }
            if (in_selection(abs_line, c)) {
                bg = draw_rgb(70, 100, 150);
                has_bg = 1;
            }
            if (!view && cursor_visible && gui.focused && shell_pid >= 0 &&
                r == scr->cy && c == scr->cx) {
                uint32_t t = fg; fg = bg; bg = draw_rgb(140, 170, 210);
                (void)t;
                has_bg = 1;
            }
            if (has_bg) draw_rect(s, x, y, TERM_CW, TERM_LH, bg);
            if (cell->attr & A_DIM) fg = draw_blend(fg, COL_BG, 90);
            if (cell->ch > 32)
                draw_glyph(s, x, y, cell->ch, fg, &draw_font_mono);
            if (cell->attr & A_UNDER)
                draw_rect(s, x, y + TERM_LH - 3, TERM_CW, 1, fg);
        }
    }
    if (view) {                             /* scrollback position marker */
        char tag[32];
        snprintf(tag, sizeof(tag), "-%d", view);
        draw_rect(s, s->w - 60, 2, 56, 18, draw_rgb(60, 70, 90));
        draw_text_aa(s, s->w - 54, 2, tag, COL_FG, &draw_font_ui);
    }
    wm_commit(&gui.wm, gui.slot);
    dirty = 0;
}

/* ── Size ──────────────────────────────────────────────────────────────── */

static void apply_size(void) {
    int nc = (gui_body_width(&gui) - 2 * PAD_X) / TERM_CW;
    int nr = (gui_body_height(&gui) - 2 * PAD_Y) / TERM_LH;
    struct { unsigned short row, col, xp, yp; } ws;

    if (nc < 10) nc = 10;
    if (nr < 3) nr = 3;
    if (nc > MAX_COLS) nc = MAX_COLS;
    if (nr > MAX_ROWS) nr = MAX_ROWS;
    if (nc == cols && nr == rows) return;

    /* Shrinking rows: keep the cursor's line on screen by pushing the top
     * of the main screen into the scrollback. */
    if (nr < rows) {
        int drop = screens[0].cy - (nr - 1);
        for (int i = 0; i < drop; i++) {
            push_history(screens[0].cells[0]);
            memmove(screens[0].cells[0], screens[0].cells[1],
                    sizeof(screens[0].cells[0]) * (size_t)(MAX_ROWS - 1));
        }
        if (drop > 0) screens[0].cy -= drop;
    }
    for (int k = 0; k < 2; k++) {               /* blank the newly exposed */
        for (int r = 0; r < MAX_ROWS; r++)
            for (int c = (r < rows ? cols : 0); c < MAX_COLS; c++) {
                screens[k].cells[r][c].ch = 0;
                screens[k].cells[r][c].attr = A_FGDEF | A_BGDEF;
            }
    }
    cols = nc;
    rows = nr;
    top_margin = 0;
    bot_margin = rows - 1;
    for (int k = 0; k < 2; k++) {
        if (screens[k].cx >= cols) screens[k].cx = cols - 1;
        if (screens[k].cy >= rows) screens[k].cy = rows - 1;
    }
    wrap_pending = 0;
    view = 0;
    sel_active = 0;
    ws.row = (unsigned short)rows;
    ws.col = (unsigned short)cols;
    ws.xp = (unsigned short)(cols * TERM_CW);
    ws.yp = (unsigned short)(rows * TERM_LH);
    if (master_fd >= 0) ioctl(master_fd, TIOCSWINSZ, &ws);
    gui_trace("term", "grid %dx%d", cols, rows);
    dirty = 1;
}

static void on_layout(gui_window_t *g) {
    (void)g;
    apply_size();
    dirty = 1;
}

/* ── Clipboard ─────────────────────────────────────────────────────────── */

static void copy_selection(void) {
    static char buf[MAX_COLS * 4 * 64];
    int l0, c0, l1, c1, n = 0;

    if (!sel_active) return;
    sel_bounds(&l0, &c0, &l1, &c1);
    for (int l = l0; l <= l1 && n < (int)sizeof(buf) - MAX_COLS * 4; l++) {
        const cell_t *line;
        int from = l == l0 ? c0 : 0, to = l == l1 ? c1 : cols - 1, end;
        if (l < hist_count) line = history[(hist_head + l) % SCROLLBACK];
        else if (l - hist_count < rows) line = scr->cells[l - hist_count];
        else break;
        end = to;
        while (end >= from && line[end].ch <= 32) end--;   /* trim right */
        for (int c = from; c <= end; c++)
            n += draw_utf8_encode(line[c].ch ? line[c].ch : ' ', buf + n);
        if (l != l1) buf[n++] = '\n';
    }
    gui_clipboard_set(&gui, buf, n);
    gui_trace("term", "copied %d bytes", n);
}

static void paste_clipboard(void) {
    static char buf[16384];
    int n = gui_clipboard_get(buf, sizeof(buf));

    if (n <= 0) return;
    for (int i = 0; i < n; i++)
        if (buf[i] == '\n') buf[i] = '\r';     /* what Enter sends */
    if (bracketed_paste) pty_puts("\033[200~");
    pty_write(buf, n);
    if (bracketed_paste) pty_puts("\033[201~");
    view = 0;
    gui_trace("term", "pasted %d bytes", n);
}

/* ── Input ─────────────────────────────────────────────────────────────── */

static void on_key(gui_window_t *g, int code, int value, int ascii) {
    int mods = g->key_mods;
    int shift = mods & WM_MOD_SHIFT, ctrl = mods & WM_MOD_CTRL;
    int alt = mods & WM_MOD_ALT;
    const char *seq = 0;
    char buf[8];

    (void)value;
    if (master_fd < 0) return;

    if (ctrl && shift && code == KEY_C) { copy_selection(); return; }
    if (ctrl && shift && code == KEY_V) { paste_clipboard(); return; }
    if (shift && (code == KEY_PAGEUP || code == KEY_PAGEDOWN)) {
        view += (code == KEY_PAGEUP ? 1 : -1) * (rows - 1);
        if (view > hist_count) view = hist_count;
        if (view < 0) view = 0;
        dirty = 1;
        return;
    }

    switch (code) {
    case KEY_UP:    seq = app_cursor ? "\033OA" : "\033[A"; break;
    case KEY_DOWN:  seq = app_cursor ? "\033OB" : "\033[B"; break;
    case KEY_RIGHT: seq = app_cursor ? "\033OC" : "\033[C"; break;
    case KEY_LEFT:  seq = app_cursor ? "\033OD" : "\033[D"; break;
    case KEY_HOME:  seq = app_cursor ? "\033OH" : "\033[H"; break;
    case KEY_END:   seq = app_cursor ? "\033OF" : "\033[F"; break;
    case KEY_INSERT:   seq = "\033[2~"; break;
    case KEY_DELETE:   seq = "\033[3~"; break;
    case KEY_PAGEUP:   seq = "\033[5~"; break;
    case KEY_PAGEDOWN: seq = "\033[6~"; break;
    case KEY_F1: seq = "\033OP"; break;
    case KEY_F2: seq = "\033OQ"; break;
    case KEY_F3: seq = "\033OR"; break;
    case KEY_F4: seq = "\033OS"; break;
    case KEY_F5: seq = "\033[15~"; break;
    case KEY_F6: seq = "\033[17~"; break;
    case KEY_F7: seq = "\033[18~"; break;
    case KEY_F8: seq = "\033[19~"; break;
    case KEY_F9: seq = "\033[20~"; break;
    case KEY_F10: seq = "\033[21~"; break;
    case KEY_F11: seq = "\033[23~"; break;
    case KEY_F12: seq = "\033[24~"; break;
    case KEY_ESC: seq = "\033"; break;
    case KEY_ENTER: seq = "\r"; break;
    case KEY_BACKSPACE: seq = "\177"; break;
    case KEY_TAB: seq = shift ? "\033[Z" : "\t"; break;
    }
    if (seq) {
        pty_puts(seq);
    } else if (ascii > 0) {
        int n = 0;
        if (alt) buf[n++] = 27;                 /* Meta sends ESC prefix */
        n += draw_utf8_encode((unsigned)ascii, buf + n);
        pty_write(buf, n);
    } else {
        return;
    }
    if (view) { view = 0; }
    sel_active = 0;
    dirty = 1;
}

static void on_scroll(gui_window_t *g, int delta) {
    (void)g;
    if (alt_active) {               /* full-screen apps get arrow keys */
        const char *k = delta > 0 ? (app_cursor ? "\033OA" : "\033[A")
                                  : (app_cursor ? "\033OB" : "\033[B");
        for (int i = 0; i < 3 * (delta > 0 ? delta : -delta); i++) pty_puts(k);
        return;
    }
    view += delta * 3;
    if (view > hist_count) view = hist_count;
    if (view < 0) view = 0;
    dirty = 1;
}

static void cell_at(int x, int y, int *line, int *col) {
    int r = (y - PAD_Y) / TERM_LH, c = (x - PAD_X + TERM_CW / 2) / TERM_CW;
    if (r < 0) r = 0;
    if (r >= rows) r = rows - 1;
    if (c < 0) c = 0;
    if (c > cols) c = cols;
    *line = hist_count - view + r;
    *col = c;
}

static void on_mouse(gui_window_t *g, int x, int y, int buttons) {
    int line, col;

    (void)g;
    cell_at(x, y, &line, &col);
    if (buttons & 1) {
        if (!sel_dragging) {
            sel_dragging = 1;
            sel_active = 0;
            sel_a_line = sel_b_line = line;
            sel_a_col = sel_b_col = col;
        } else {
            sel_b_line = line;
            sel_b_col = col;
            sel_active = sel_a_line != sel_b_line || sel_a_col != sel_b_col;
        }
        dirty = 1;
    } else if (sel_dragging) {
        sel_dragging = 0;
        if (sel_active) {
            /* The end column is exclusive while dragging. */
            if (sel_b_col > 0 && (sel_b_line > sel_a_line ||
                (sel_b_line == sel_a_line && sel_b_col > sel_a_col)))
                sel_b_col--;
            else if (sel_a_col > 0) sel_a_col--;
            gui_trace("term", "selected %d,%d-%d,%d", sel_a_line, sel_a_col,
                      sel_b_line, sel_b_col);
        }
        dirty = 1;
    }
}

/* ── Shell ─────────────────────────────────────────────────────────────── */

extern char **environ;

static int start_shell(void) {
    char pts_path[20];
    const char *shell_path =
        access("/disk/shell", X_OK) == 0 ? "/disk/shell" : "/shell";
    char *argv[] = { (char *)shell_path, 0 };
    static char *envp[64];
    int unlock = 0;
    int pty_num = -1;
    int slave, ne = 0;

    master_fd = open("/dev/ptmx", O_RDWR);
    if (master_fd < 0) return -1;
    if (ioctl(master_fd, TIOCSPTLCK, &unlock) < 0 ||
        ioctl(master_fd, TIOCGPTN, &pty_num) < 0) {
        close(master_fd);
        return -1;
    }
    sprintf(pts_path, "/dev/pts/%d", pty_num);
    slave = open(pts_path, O_RDWR);
    if (slave < 0) {
        close(master_fd);
        return -1;
    }
    {
        struct { unsigned short row, col, xp, yp; } ws;
        ws.row = (unsigned short)rows;
        ws.col = (unsigned short)cols;
        ws.xp = ws.yp = 0;
        ioctl(master_fd, TIOCSWINSZ, &ws);
    }

    /* The desktop's environment plus TERM for full-screen programs. */
    envp[ne++] = "TERM=xterm-256color";
    for (char **e = environ; e && *e && ne < 62; e++)
        if (strncmp(*e, "TERM=", 5) && strncmp(*e, "COLUMNS=", 8) &&
            strncmp(*e, "LINES=", 6))
            envp[ne++] = *e;
    envp[ne] = 0;

    shell_pid = fork();
    if (shell_pid < 0) {
        close(slave);
        close(master_fd);
        return -1;
    }
    if (shell_pid == 0) {
        close(master_fd);
        setsid();                         /* own session: ^C, SIGWINCH */
        ioctl(slave, TIOCSCTTY, 0);
        dup2(slave, 0);
        dup2(slave, 1);
        dup2(slave, 2);
        if (slave > 2) close(slave);
        execve(shell_path, argv, envp);
        exit(127);
    }
    close(slave);
    /* tools/smoke_gui.py checks that what it types runs in this shell. */
    gui_trace("term", "shell pid=%d tty=%s", shell_pid, pts_path);
    return 0;
}

static void drain_pty(void) {
    char buf[1024];
    int budget = 64;                      /* keep the UI responsive */

    if (master_fd < 0) return;
    while (budget-- > 0) {
        struct pollfd p;
        int n;
        p.fd = master_fd;
        p.events = POLLIN;
        p.revents = 0;
        if (poll(&p, 1, 0) <= 0 || !(p.revents & POLLIN)) return;
        n = (int)read(master_fd, buf, (int)sizeof(buf));
        if (n <= 0) return;
        feed_output(buf, n);
    }
}

int main(int argc, char *argv[]) {
    int slot = (argc > 1) ? atoi(argv[1]) : 1;
    int x = 80 + (slot - 1) * 32;
    int y = 48 + (slot - 1) * 28;

    if (slot < 1 || slot > WM_MAX_SLOTS) slot = 1;
    if (gui_open(&gui, slot, "Terminal", x, y, 680, 420) < 0) {
        printf("term: desktop unavailable\n");
        return 1;
    }
    gui_set_key_handler(&gui, on_key);
    gui_set_scroll_handler(&gui, on_scroll);
    gui_set_mouse_handler(&gui, on_mouse);
    gui_set_layout(&gui, on_layout);
    gui_grab_escape(&gui, 1);                /* vi needs Escape */

    rows = cols = 0;
    reset_tabs();
    apply_size();
    reset_terminal();

    if (start_shell() < 0) {
        printf("term: pty unavailable\n");
        gui_close(&gui);
        return 1;
    }

    render();
    while (!gui.closed) {
        int events = gui_poll(&gui);
        drain_pty();
        /* Shell gone → close the window. */
        if (shell_pid >= 0) {
            int status;
            if (waitpid(shell_pid, &status, 1 /* WNOHANG */) == shell_pid) {
                shell_pid = -1;
                break;
            }
        }
        if (dirty || events > 0)
            render();
        sleep_ms(dirty ? 5 : 20);
    }

    if (shell_pid >= 0) {
        int status;
        kill(shell_pid, SIGHUP);
        kill(shell_pid, SIGTERM);
        waitpid(shell_pid, &status, 0);
    }
    if (master_fd >= 0) close(master_fd);
    gui_close(&gui);
    return 0;
}
