/*
 * gfxbench — compositor cost of a small animating region.
 *
 *   gfxbench gui SECS [full|small] [FPS]
 *                              libgui window, redraw a 446x174 region
 *   gfxbench x   SECS [full|small] [FPS]
 *                              raw X client on a windowed maeroX (spawned on
 *                              slot 4 when none is listening), PutImage of a
 *                              446x174 region, GetInputFocus round trip per
 *                              frame (what XSync does)
 *
 * `full` redraws the whole client area instead, for the "full-screen updates
 * are not slower" side of the comparison.  The client renders as fast as the
 * path lets it, or paced to FPS frames a second (then the CPU shares are the
 * absolute cost of that frame rate); at the end one line reports frames, fps and how the CPU split
 * between the desktop, maeroX and the client over the run, from the per-thread
 * tick counters in /proc/processes (1 tick = 10 ms, every tick is charged to
 * whoever was running, so the shares add up to ~100% minus idle).
 *
 *   GFXBENCH mode=gui region=446x174 secs=10 frames=N fps=F desktop=D% maerox=M% client=C% idle=I%
 */
#include <fcntl.h>
#include <gui.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define RW 446
#define RH 174

/* ── CPU accounting ─────────────────────────────────────────────────────── */

typedef struct { unsigned desktop, maerox, client, idle; } cpu_t;

/* Sum exact on-CPU microseconds by process name from /proc/cputime (the
 * scheduler's dispatch timestamps: tick sampling misses work that wakes on a
 * tick and is done before the next, i.e. most of a paced compositor). */
static void cpu_snapshot(cpu_t *c) {
    static char buf[16384];
    int fd = open("/proc/cputime", O_RDONLY), n = 0, r;
    memset(c, 0, sizeof(*c));
    if (fd < 0) return;
    while (n < (int)sizeof(buf) - 1 && (r = read(fd, buf + n, sizeof(buf) - 1 - n)) > 0)
        n += r;
    close(fd);
    buf[n] = 0;
    for (char *line = buf; line && *line; ) {
        char *end = strchr(line, '\n');
        if (end) *end = 0;
        if (!strncmp(line, "idle ", 5)) {
            c->idle = (unsigned)strtoul(line + 5, 0, 10);
        } else {
            /* PID TGID US NAME */
            char *p = line;
            for (int f = 0; f < 2 && p; f++) { p = strchr(p, ' '); if (p) p++; }
            if (p) {
                unsigned us = (unsigned)strtoul(p, 0, 10);
                const char *name = strchr(p, ' ');
                if (name) {
                    const char *slash = strrchr(++name, '/');
                    if (slash) name = slash + 1;
                    if (!strncmp(name, "desktop", 7)) c->desktop += us;
                    else if (!strncmp(name, "maerox", 6)) c->maerox += us;
                    else if (!strncmp(name, "gfxbench", 8)) c->client += us;
                }
            }
        }
        line = end ? end + 1 : 0;
    }
}

static unsigned now_cs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned)ts.tv_sec * 100u + (unsigned)(ts.tv_nsec / 10000000);
}

static int pace_fps;

static unsigned now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned)ts.tv_sec * 1000000u + (unsigned)(ts.tv_nsec / 1000);
}

/* Paced runs: sleep until frame `n`'s slot (t0 + n/fps). */
static void pace(unsigned t0_us, unsigned n) {
    if (pace_fps <= 0) return;
    unsigned due = t0_us + (unsigned)((unsigned long long)n * 1000000u / (unsigned)pace_fps);
    unsigned now = now_us();
    if ((int)(due - now) > 0) usleep(due - now);
}

/* Shares in tenths of a percent of the elapsed wall time. */
static unsigned pm(unsigned us, unsigned el_us) {
    return (unsigned)((unsigned long long)us * 1000u / (el_us ? el_us : 1));
}

static void report(const char *mode, int full, int w, int h, int secs,
                   unsigned frames, const cpu_t *a, const cpu_t *b,
                   unsigned el_us) {
    unsigned d = b->desktop - a->desktop, m = b->maerox - a->maerox;
    unsigned c = b->client - a->client, idle = b->idle - a->idle;
    unsigned fps10 = (unsigned)((unsigned long long)frames * 10000000u / (el_us ? el_us : 1));
#define PCT(v) pm(v, el_us) / 10, pm(v, el_us) % 10
    printf("GFXBENCH mode=%s%s%s region=%dx%d secs=%d frames=%u fps=%u.%u "
           "desktop=%u.%u%% maerox=%u.%u%% client=%u.%u%% idle=%u.%u%% "
           "per-frame(desktop+maerox)=%uus\n",
           mode, full ? "-full" : "", pace_fps ? "-paced" : "", w, h, secs,
           frames, fps10 / 10, fps10 % 10,
           PCT(d), PCT(m), PCT(c), PCT(idle),
           frames ? (d + m) / frames : 0);
#undef PCT
}

/* ── libgui mode ────────────────────────────────────────────────────────── */

static int run_gui(int secs, int full) {
    static gui_window_t g;
    int w = full ? 1000 : RW + 40, h = full ? 700 : RH + 60;
    if (gui_open(&g, 11, "gfxbench", 40, 40, w, h) < 0) {
        printf("GFXBENCH_FAIL gui_open\n");
        return 1;
    }
    /* Let the window settle (geometry events, first composite). */
    for (int i = 0; i < 20; i++) { gui_poll(&g); usleep(20000); }
    g.closed = 0;   /* a close queued for the slot by an earlier run */
    draw_fill(&g.surf, draw_rgb(40, 40, 48));
    wm_commit(&g.wm, g.slot);

    int rx = full ? 0 : 12, ry = full ? 0 : 12;
    int rw = full ? g.surf.w : RW, rh = full ? g.surf.h : RH;
    cpu_t a, b;
    unsigned frames = 0;
    cpu_snapshot(&a);
    unsigned t0 = now_cs(), end = t0 + (unsigned)secs * 100u, t0us = now_us();
    while (now_cs() < end && !g.closed) {
        pace(t0us, frames);
        uint32_t col = draw_rgb((frames * 7) & 255, (frames * 3) & 255, 160);
        for (int y = 0; y < rh; y++) {
            uint32_t *p = g.surf.px + (size_t)(ry + y) * g.surf.w + rx;
            for (int x = 0; x < rw; x++) p[x] = col + (unsigned)(x >> 4);
        }
        wm_commit_rect(&g.wm, g.slot, rx, ry, rw, rh);
        frames++;
        if ((frames & 15) == 0) gui_poll(&g);
    }
    unsigned el = now_us() - t0us;
    cpu_snapshot(&b);
    report("gui", full, rw, rh, secs, frames, &a, &b, el);
    wm_command(&g.wm, "close %d", g.slot);
    gui_close(&g);
    return 0;
}

/* ── raw X mode ─────────────────────────────────────────────────────────── */

static int X = -1;
static unsigned id_base, id_mask, root_win, next_id;

static unsigned u16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static unsigned u32(const unsigned char *p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24);
}
static int rd(unsigned char *b, int n) {
    int got = 0;
    while (got < n) {
        int r = read(X, b + got, n - got);
        if (r <= 0) return -1;
        got += r;
    }
    return got;
}
static int wr(const void *b, int n) {
    const unsigned char *p = b;
    while (n > 0) {
        int r = write(X, p, n);
        if (r <= 0) return -1;
        p += r; n -= r;
    }
    return 0;
}
static void w8(unsigned char **p, unsigned v) { *(*p)++ = (unsigned char)v; }
static void w16(unsigned char **p, unsigned v) { w8(p, v); w8(p, v >> 8); }
static void w32(unsigned char **p, unsigned v) { w16(p, v); w16(p, v >> 16); }

static int x_connect(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a;
    if (fd < 0) return -1;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    strcpy(a.sun_path, "/tmp/.X11-unix/X0");
    if (connect(fd, (struct sockaddr *)&a, sizeof(a.sun_family) + strlen(a.sun_path)) == 0)
        return fd;
    close(fd);
    return -1;
}

static int x_handshake(void) {
    unsigned char req[12] = {0}, h[8], body[2048];
    req[0] = 'l'; req[2] = 11;
    if (wr(req, 12) < 0 || rd(h, 8) != 8 || h[0] != 1) return -1;
    unsigned extra = u16(h + 6) * 4;
    if (extra > sizeof(body)) return -1;
    if (rd(body, (int)extra) != (int)extra) return -1;
    id_base = u32(body + 4); id_mask = u32(body + 8);
    unsigned vlen = u16(body + 16);
    unsigned off = 32 + ((vlen + 3) & ~3u) + 2 * 8;
    root_win = u32(body + off);
    return 0;
}

/* GetInputFocus and wait for its reply, skipping events (XSync). */
static int x_sync(void) {
    unsigned char b[4] = {43, 0, 1, 0}, rep[32];
    if (wr(b, 4) < 0) return -1;
    for (;;) {
        if (rd(rep, 32) != 32) return -1;
        if (rep[0] == 1) {
            unsigned extra = u32(rep + 4) * 4;
            unsigned char skip[256];
            while (extra) {
                unsigned n = extra > sizeof(skip) ? sizeof(skip) : extra;
                if (rd(skip, (int)n) < 0) return -1;
                extra -= n;
            }
            return 0;
        }
        if (rep[0] == 0) return -1;
    }
}

static unsigned char *put_buf;

/* PutImage ZPixmap, split so each request fits maximum-request-length. */
static int x_put(unsigned d, unsigned gc, int dx, int dy, int w, int h, uint32_t col) {
    int stride = w * 4;
    int max_rows = (65535 * 4 - 24) / stride;
    for (int y0 = 0; y0 < h; y0 += max_rows) {
        int rows = h - y0 < max_rows ? h - y0 : max_rows;
        unsigned len = (unsigned)(24 + stride * rows) / 4;
        unsigned char *p = put_buf;
        w8(&p, 72); w8(&p, 2); w16(&p, len);
        w32(&p, d); w32(&p, gc);
        w16(&p, w); w16(&p, rows); w16(&p, dx); w16(&p, dy + y0);
        w8(&p, 0); w8(&p, 24); w16(&p, 0);
        uint32_t *px = (uint32_t *)p;
        for (int i = 0; i < w * rows; i++) px[i] = col + (unsigned)((i % w) >> 4);
        if (wr(put_buf, 24 + stride * rows) < 0) return -1;
    }
    return 0;
}

static int run_x(int secs, int full) {
    pid_t srv = 0;
    if ((X = x_connect()) < 0) {
        srv = fork();
        if (srv == 0) {
            execl("/disk/maerox", "maerox", "4", (char *)0);
            execl("/maerox", "maerox", "4", (char *)0);
            _exit(127);
        }
        for (int i = 0; i < 250 && (X = x_connect()) < 0; i++) usleep(20000);
    }
    if (X < 0 || x_handshake() < 0) {
        printf("GFXBENCH_FAIL x connect\n");
        if (srv > 0) kill(srv, 9);
        return 1;
    }
    int ww = full ? 800 : RW + 24, wh = full ? 520 : RH + 24;
    unsigned win = id_base | (next_id++ & id_mask);
    unsigned gc = id_base | (next_id++ & id_mask);
    unsigned char b[64], *p = b;
    w8(&p, 1); w8(&p, 24); w16(&p, 8);                    /* CreateWindow */
    w32(&p, win); w32(&p, root_win);
    w16(&p, 20); w16(&p, 40); w16(&p, ww); w16(&p, wh);
    w16(&p, 0); w16(&p, 1); w32(&p, 0); w32(&p, 0);
    w8(&p, 55); w8(&p, 0); w16(&p, 4);                    /* CreateGC */
    w32(&p, gc); w32(&p, win); w32(&p, 0);
    w8(&p, 8); w8(&p, 0); w16(&p, 2); w32(&p, win);       /* MapWindow */
    wr(b, (int)(p - b));
    put_buf = malloc(65536 * 4 + 64);
    int rx = full ? 0 : 12, ry = full ? 0 : 12;
    int rw = full ? ww : RW, rh = full ? wh : RH;
    x_put(win, gc, 0, 0, ww, wh, 0x303040);
    x_sync();
    usleep(300000);

    cpu_t a, c;
    unsigned frames = 0;
    cpu_snapshot(&a);
    unsigned t0 = now_cs(), end = t0 + (unsigned)secs * 100u, t0us = now_us();
    while (now_cs() < end) {
        pace(t0us, frames);
        uint32_t col = ((frames * 7) & 255) << 16 | ((frames * 3) & 255) << 8 | 160;
        if (x_put(win, gc, rx, ry, rw, rh, col) < 0 || x_sync() < 0) {
            printf("GFXBENCH_FAIL x io\n");
            break;
        }
        frames++;
    }
    unsigned el = now_us() - t0us;
    cpu_snapshot(&c);
    report("x", full, rw, rh, secs, frames, &a, &c, el);
    close(X);
    if (srv > 0) { kill(srv, 15); waitpid(srv, 0, 0); }
    return 0;
}

int main(int argc, char **argv) {
    int secs = argc > 2 ? atoi(argv[2]) : 10;
    int full = argc > 3 && !strcmp(argv[3], "full");
    if (secs < 1) secs = 10;
    if (argc > 4) pace_fps = atoi(argv[4]);
    signal(SIGPIPE, SIG_IGN);
    /* Run as the desktop's user: the compositor (uid 1000) can only map a
     * client's shared surface (mode 0600) when it owns it, so a root client
     * - say, started from the serial console - would never be shown. */
    if (getuid() == 0) {
        setgid(100);
        setuid(1000);
    }
    if (argc > 1 && !strcmp(argv[1], "x")) return run_x(secs, full);
    if (argc > 1 && !strcmp(argv[1], "gui")) return run_gui(secs, full);
    printf("usage: gfxbench gui|x SECS [full]\n");
    return 2;
}
