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
 * Renders its scrollback into a shared-memory surface; multiple instances
 * run side by side (the launcher hands each one a free slot via argv[1]).
 */

#define TIOCGPTN   0x80045430U
#define TIOCSPTLCK 0x40045431U

#define SCROLLBACK 100
#define LINE_MAX   160

static gui_window_t gui;
static int shell_pid = -1;
static int master_fd = -1;

static char lines[SCROLLBACK][LINE_MAX];
static int line_count;
static char live[LINE_MAX];
static int live_len;
static int esc_state;
static int view;        /* scrollback offset, 0 = bottom */
static int dirty = 1;

static void sleep_ms(long ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, 0);
}

static void push_line(void) {
    live[live_len] = 0;
    if (line_count == SCROLLBACK) {
        for (int i = 1; i < SCROLLBACK; i++)
            strcpy(lines[i - 1], lines[i]);
        line_count--;
    }
    strcpy(lines[line_count++], live);
    live_len = 0;
}

static void feed_output(const char *buf, int n) {
    for (int i = 0; i < n; i++) {
        char c = buf[i];
        if (esc_state == 1) { esc_state = (c == '[') ? 2 : 0; continue; }
        if (esc_state == 2) {
            if (c >= 0x40 && c <= 0x7e) {
                if (c == 'J') { line_count = 0; live_len = 0; }
                esc_state = 0;
            }
            continue;
        }
        if (c == 27) { esc_state = 1; continue; }
        if (c == '\r') continue;
        if (c == '\n') { push_line(); continue; }
        if (c == '\b') { if (live_len) live_len--; continue; }
        if (c < 32 || c >= 127) continue;
        if (live_len + 1 >= LINE_MAX) push_line();
        live[live_len++] = c;
    }
    dirty = 1;
}

#define TERM_LH  DRAW_MONO_LH    /* line pitch (px) */
#define TERM_CW  DRAW_MONO_CW    /* monospace cell width (px) */

static void render(void) {
    draw_surface_t *s = &gui.surf;
    int rows = (s->h - 12) / TERM_LH;
    int total, start, vis;
    uint32_t fg = draw_rgb(222, 226, 230);

    if (!s->px || rows < 1) return;
    draw_fill(s, draw_rgb(24, 27, 33));

    total = line_count + 1;
    start = total > rows ? total - rows : 0;
    if (view > start) view = start;
    if (view < 0) view = 0;
    start -= view;

    vis = 0;
    for (int i = start; i < line_count && vis < rows; i++, vis++)
        draw_text_aa(s, 8, 6 + vis * TERM_LH, lines[i], fg, &draw_font_mono);
    if (vis < rows) {
        char tmp[LINE_MAX];
        memcpy(tmp, live, (size_t)live_len);
        tmp[live_len] = 0;
        draw_text_aa(s, 8, 6 + vis * TERM_LH, tmp, fg, &draw_font_mono);
        if (gui.focused && shell_pid >= 0)
            draw_rect(s, 8 + live_len * TERM_CW, 6 + vis * TERM_LH,
                      TERM_CW, TERM_LH - 3, draw_rgb(94, 129, 172));
    }
    wm_commit(&gui.wm, gui.slot);
    dirty = 0;
}

static void on_key(gui_window_t *g, int code, int value, int ascii) {
    char out;

    (void)g;
    (void)value;
    if (master_fd < 0) return;
    if (code == KEY_ENTER)          out = '\n';
    else if (code == KEY_BACKSPACE) out = 127;
    else if (code == KEY_TAB)       out = '\t';
    else if (ascii >= 1 && ascii < 127) out = (char)ascii;
    else return;
    view = 0;
    write(master_fd, &out, 1);
}

static void on_scroll(gui_window_t *g, int delta) {
    (void)g;
    view += delta * 3;
    if (view < 0) view = 0;
    dirty = 1;
}

static int start_shell(void) {
    char pts_path[20];
    const char *shell_path =
        access("/disk/shell", X_OK) == 0 ? "/disk/shell" : "/shell";
    char *argv[] = { (char *)shell_path, 0 };
    int unlock = 0;
    int pty_num = -1;
    int slave;

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

    shell_pid = fork();
    if (shell_pid < 0) {
        close(slave);
        close(master_fd);
        return -1;
    }
    if (shell_pid == 0) {
        close(master_fd);
        dup2(slave, 0);
        dup2(slave, 1);
        dup2(slave, 2);
        close(slave);
        execve(shell_path, argv, 0);
        exit(127);
    }
    close(slave);
    return 0;
}

static void drain_pty(void) {
    char buf[256];

    if (master_fd < 0) return;
    for (;;) {
        struct pollfd p;
        int n;
        p.fd = master_fd;
        p.events = POLLIN;
        p.revents = 0;
        if (poll(&p, 1, 0) <= 0 || !(p.revents & POLLIN)) return;
        n = read(master_fd, buf, (int)sizeof(buf));
        if (n <= 0) return;
        feed_output(buf, n);
    }
}

int main(int argc, char *argv[]) {
    int slot = (argc > 1) ? atoi(argv[1]) : 1;
    int x = 80 + (slot - 1) * 32;
    int y = 48 + (slot - 1) * 28;

    if (slot < 1 || slot > WM_MAX_SLOTS) slot = 1;
    if (gui_open(&gui, slot, "Terminal", x, y, 600, 380) < 0) {
        printf("term: desktop unavailable\n");
        return 1;
    }
    gui_set_key_handler(&gui, on_key);
    gui_set_scroll_handler(&gui, on_scroll);

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
        sleep_ms(30);
    }

    if (shell_pid >= 0) {
        int status;
        kill(shell_pid, SIGTERM);
        waitpid(shell_pid, &status, 0);
    }
    if (master_fd >= 0) close(master_fd);
    gui_close(&gui);
    return 0;
}
