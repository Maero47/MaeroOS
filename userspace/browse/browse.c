#include <arpa/inet.h>
#include <errno.h>
#include <draw.h>
#include <gui.h>
#include <linux/input.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/*
 * browse 2.0 — small HTTP browser.
 *   - clickable <a href> links (relative URLs resolved against the page)
 *   - back/forward history
 *   - follows 301/302/303/307 redirects (up to 3 hops)
 *   - typography: h1/h2 headings, <pre>/<code> in monospace
 * Fetches run on a worker thread; the UI thread owns all render state.
 */

#define URL_MAX   160
#define PAGE_MAX  (160 * 1024)
#define LINES_MAX 1200
#define LINE_LEN  160
#define MAX_LINKS 200
#define HIST_MAX  32

/* Per-character styles in the flattened text stream. */
#define ST_BODY 0
#define ST_H1   1
#define ST_H2   2
#define ST_PRE  3

#define NO_LINK 0xFF

static gui_window_t gui;
static char url[URL_MAX] = "http://10.0.2.2:8000/";
static int url_len;

/* Flattened page text + parallel per-char link index / style. */
static char text[PAGE_MAX];
static unsigned char text_link[PAGE_MAX];
static unsigned char text_style[PAGE_MAX];

/* Wrapped lines: each is a slice of chars copied with its metadata. */
static char linebuf[LINES_MAX][LINE_LEN];
static unsigned char line_link[LINES_MAX][LINE_LEN];
static unsigned char line_style[LINES_MAX];
static int line_count;

static char links[MAX_LINKS][URL_MAX];
static int link_count;

static char history[HIST_MAX][URL_MAX];
static int hist_len, hist_pos = -1;

static int view;
static int dirty = 1;
static char status[96] = "Enter a URL and press Enter";

static void sleep_ms(long ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, 0);
}

static void set_status(const char *s) {
    strncpy(status, s, sizeof(status) - 1);
    status[sizeof(status) - 1] = 0;
    dirty = 1;
}

static const draw_font_t *font_for(int style) {
    if (style == ST_H1) return &draw_font_ui_big;
    return &draw_font_ui;   /* ST_PRE renders mono via draw_text */
}

static int line_height(int style) {
    if (style == ST_H1) return 30;
    if (style == ST_H2) return 24;
    if (style == ST_PRE) return 18;
    return 20;
}

static int char_width(char c, int style) {
    if (style == ST_PRE) return 8;
    {
        unsigned char ch = (unsigned char)c;
        if (ch < 32 || ch > 126) ch = '?';
        return font_for(style)->widths[ch - 32];
    }
}

/* ── URL helpers ────────────────────────────────────────────────────────── */

/* Split http://host[:port]/path; returns 0 on success. */
static int split_url(const char *u, char *host, int hcap, int *port,
                     char *path, int pcap) {
    const char *p = u;
    int hi = 0;

    if (!strncmp(p, "http://", 7)) p += 7;
    else if (strstr(p, "://")) return -1;     /* https etc: unsupported */
    *port = 80;
    while (*p && *p != '/' && *p != ':' && hi < hcap - 1)
        host[hi++] = *p++;
    host[hi] = 0;
    if (*p == ':') {
        p++;
        *port = 0;
        while (*p >= '0' && *p <= '9') *port = *port * 10 + (*p++ - '0');
        if (*port < 1 || *port > 65535) *port = 80;
    }
    strncpy(path, *p ? p : "/", (size_t)pcap - 1);
    path[pcap - 1] = 0;
    return host[0] ? 0 : -1;
}

/* Resolve href against base into out (out may alias neither input). */
static void resolve_url(const char *base, const char *href, char *out,
                        int cap) {
    char host[96], path[160];
    int port;

    if (!strncmp(href, "http://", 7)) {
        strncpy(out, href, (size_t)cap - 1);
        out[cap - 1] = 0;
        return;
    }
    if (!strncmp(href, "//", 2)) {
        snprintf(out, (size_t)cap, "http:%s", href);
        return;
    }
    if (split_url(base, host, sizeof(host), &port, path, sizeof(path)) < 0) {
        strncpy(out, href, (size_t)cap - 1);
        out[cap - 1] = 0;
        return;
    }
    if (href[0] == '/') {
        if (port != 80)
            snprintf(out, (size_t)cap, "http://%s:%d%s", host, port, href);
        else
            snprintf(out, (size_t)cap, "http://%s%s", host, href);
        return;
    }
    /* relative: strip base path to its directory */
    {
        char *slash = strrchr(path, '/');
        if (slash) slash[1] = 0;
        if (port != 80)
            snprintf(out, (size_t)cap, "http://%s:%d%s%s", host, port,
                     path, href);
        else
            snprintf(out, (size_t)cap, "http://%s%s%s", host, path, href);
    }
}

/* ── HTML → flattened styled text ───────────────────────────────────────── */

static int tag_is(const char *p, const char *name) {
    int n = (int)strlen(name);
    if (strncasecmp(p + 1, name, (size_t)n)) return 0;
    {
        char c = p[1 + n];
        return c == '>' || c == ' ' || c == '\t' || c == '\n' || c == '/';
    }
}

static void render_html(char *body) {
    int o = 0, nl_run = 0;
    int style = ST_BODY, cur_link = NO_LINK;

    link_count = 0;
    for (char *p = body; *p && o < PAGE_MAX - 2; p++) {
        if (*p == '<') {
            if (tag_is(p, "script") || tag_is(p, "style")) {
                const char *closer = tag_is(p, "script") ? "</script"
                                                         : "</style";
                char *end = strstr(p + 1, closer);
                if (!end) break;
                p = end;
            }
            if (tag_is(p, "a")) {                 /* opening <a ...> */
                char *href = strstr(p, "href");
                char *gt = strchr(p, '>');
                if (href && gt && href < gt && link_count < MAX_LINKS - 1) {
                    char *q = strchr(href, '=');
                    if (q && q < gt) {
                        char quote = 0;
                        q++;
                        while (*q == ' ') q++;
                        if (*q == '"' || *q == '\'') quote = *q++;
                        {
                            char raw[URL_MAX];
                            int ri = 0;
                            while (*q && q < gt && ri < URL_MAX - 1 &&
                                   *q != (quote ? quote : ' '))
                                raw[ri++] = *q++;
                            raw[ri] = 0;
                            if (ri && raw[0] != '#' &&
                                strncmp(raw, "javascript:", 11) &&
                                strncmp(raw, "mailto:", 7)) {
                                resolve_url(url, raw, links[link_count],
                                            URL_MAX);
                                cur_link = (unsigned char)link_count;
                                link_count++;
                            }
                        }
                    }
                }
            } else if (tag_is(p, "/a")) {
                cur_link = NO_LINK;
            } else if (tag_is(p, "h1")) {
                style = ST_H1;
            } else if (tag_is(p, "h2") || tag_is(p, "h3")) {
                style = ST_H2;
            } else if (tag_is(p, "/h1") || tag_is(p, "/h2") ||
                       tag_is(p, "/h3")) {
                style = ST_BODY;
            } else if (tag_is(p, "pre") || tag_is(p, "code")) {
                style = ST_PRE;
            } else if (tag_is(p, "/pre") || tag_is(p, "/code")) {
                style = ST_BODY;
            }
            /* block tags become newlines */
            if (tag_is(p, "br") || tag_is(p, "p") || tag_is(p, "/p") ||
                tag_is(p, "div") || tag_is(p, "/div") || tag_is(p, "li") ||
                tag_is(p, "tr") || tag_is(p, "h1") || tag_is(p, "h2") ||
                tag_is(p, "h3") || tag_is(p, "/h1") || tag_is(p, "/h2") ||
                tag_is(p, "/h3") || tag_is(p, "title") ||
                tag_is(p, "/title") || tag_is(p, "pre") || tag_is(p, "/pre") ||
                tag_is(p, "ul") || tag_is(p, "/ul"))
                if (nl_run < 2) {
                    text[o] = '\n';
                    text_link[o] = NO_LINK;
                    text_style[o] = (unsigned char)style;
                    o++;
                    nl_run++;
                }
            {   /* skip to the closing '>' */
                char *gt = strchr(p, '>');
                if (!gt) break;
                p = gt;
            }
            continue;
        }
        if (*p == '&') {
            char c = '&';
            int adv = 0;
            if (!strncmp(p, "&amp;", 5)) { c = '&'; adv = 4; }
            else if (!strncmp(p, "&lt;", 4)) { c = '<'; adv = 3; }
            else if (!strncmp(p, "&gt;", 4)) { c = '>'; adv = 3; }
            else if (!strncmp(p, "&nbsp;", 6)) { c = ' '; adv = 5; }
            else if (!strncmp(p, "&quot;", 6)) { c = '"'; adv = 5; }
            else if (!strncmp(p, "&#39;", 5)) { c = '\''; adv = 4; }
            text[o] = c;
            text_link[o] = (unsigned char)cur_link;
            text_style[o] = (unsigned char)style;
            o++;
            p += adv;
            nl_run = 0;
            continue;
        }
        if (*p == '\n' && style != ST_PRE) {
            if (nl_run < 2) {
                text[o] = '\n';
                text_link[o] = NO_LINK;
                text_style[o] = (unsigned char)style;
                o++;
                nl_run++;
            }
            continue;
        }
        {
            char c = *p;
            if (c == '\r') continue;
            if (c == '\t') c = ' ';
            if ((c < 32 && c != '\n') || (unsigned char)c > 126) continue;
            text[o] = c;
            text_link[o] = (unsigned char)cur_link;
            text_style[o] = (unsigned char)style;
            o++;
            if (c != '\n') nl_run = 0;
        }
    }
    text[o] = 0;

    /* ── wrap into lines with metadata ── */
    {
        int max_w = gui.surf.w - 16;
        int col = 0, px = 0;

        if (max_w < 120) max_w = 120;
        line_count = 0;
        line_style[0] = ST_BODY;
        for (int i = 0; i < o && line_count < LINES_MAX - 1; i++) {
            char c = text[i];
            if (c == '\n') {
                linebuf[line_count][col] = 0;
                line_count++;
                col = 0;
                px = 0;
                line_style[line_count] = ST_BODY;
                continue;
            }
            if (col == 0)
                line_style[line_count] = text_style[i];
            linebuf[line_count][col] = c;
            line_link[line_count][col] = text_link[i];
            px += char_width(c, text_style[i]);
            col++;
            linebuf[line_count][col] = 0;
            if (px >= max_w || col >= LINE_LEN - 2) {
                /* soft break at the last space when close */
                int brk = col;
                for (int k = col - 1; k > col - 20 && k > 0; k--)
                    if (linebuf[line_count][k] == ' ') { brk = k; break; }
                {
                    char carry[24];
                    unsigned char carrl[24];
                    int cl = col - brk -
                             (linebuf[line_count][brk] == ' ' ? 1 : 0);
                    int style_here = line_style[line_count];
                    if (cl > 0 && cl < 24 && brk < col) {
                        memcpy(carry, linebuf[line_count] + col - cl,
                               (size_t)cl);
                        memcpy(carrl, line_link[line_count] + col - cl,
                               (size_t)cl);
                    } else {
                        cl = 0;
                    }
                    linebuf[line_count][brk] = 0;
                    line_count++;
                    col = 0;
                    px = 0;
                    line_style[line_count] = (unsigned char)style_here;
                    if (line_count < LINES_MAX - 1 && cl > 0) {
                        memcpy(linebuf[line_count], carry, (size_t)cl);
                        memcpy(line_link[line_count], carrl, (size_t)cl);
                        col = cl;
                        for (int k = 0; k < cl; k++)
                            px += char_width(carry[k], style_here);
                        linebuf[line_count][col] = 0;
                    }
                }
            }
        }
        if (col) line_count++;
    }
    view = 0;
}

/* ── Fetch (worker thread) with redirects ───────────────────────────────── */

static volatile int fetch_busy;
static volatile int fetch_done;
static char fetch_url[URL_MAX];
static char *fetch_body;
static pthread_t fetch_thread;
static char page[PAGE_MAX];

/* One HTTP GET; returns bytes, fills page; -1 on error (status set). */
static int http_get(const char *u) {
    char host[96], path[160];
    int port, fd, n, total = 0;
    unsigned ip;
    struct sockaddr_in addr;
    char req[320];

    if (split_url(u, host, sizeof(host), &port, path, sizeof(path)) < 0) {
        set_status("Bad URL (only http:// supported)");
        return -1;
    }
    ip = resolve_a(host);
    if (!ip) { set_status("Cannot resolve host"); return -1; }

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { set_status("No socket"); return -1; }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = ip;
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        set_status("Connection failed");
        return -1;
    }
    snprintf(req, sizeof(req),
             "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n",
             path, host);
    if (send(fd, req, strlen(req), 0) < 0) {
        close(fd);
        set_status("Send failed");
        return -1;
    }
    {
        int idle = 0;
        while (total < PAGE_MAX - 1) {
            n = recv(fd, page + total, (size_t)(PAGE_MAX - 1 - total), 0);
            if (n > 0) { total += n; idle = 0; continue; }
            if (n == 0) break;
            if (errno == EAGAIN && ++idle < 800) {
                sleep_ms(10);
                continue;
            }
            break;
        }
    }
    close(fd);
    page[total] = 0;
    if (!total) { set_status("Empty reply"); return -1; }
    return total;
}

static void fetch(void) {
    for (int hop = 0; hop < 4; hop++) {
        char *body, *loc;
        int code = 0;

        if (http_get(fetch_url) < 0) return;
        if (!strncmp(page, "HTTP/", 5)) {
            char *sp = strchr(page, ' ');
            if (sp) code = atoi(sp + 1);
        }
        body = strstr(page, "\r\n\r\n");
        if ((code == 301 || code == 302 || code == 303 || code == 307) &&
            hop < 3) {
            /* follow Location: */
            loc = strstr(page, "\r\nLocation:");
            if (!loc) loc = strstr(page, "\r\nlocation:");
            if (loc && (!body || loc < body)) {
                char target[URL_MAX];
                int ti = 0;
                loc += 11;
                while (*loc == ' ') loc++;
                while (*loc && *loc != '\r' && *loc != '\n' &&
                       ti < URL_MAX - 1)
                    target[ti++] = *loc++;
                target[ti] = 0;
                if (!strncmp(target, "https://", 8)) {
                    set_status("Redirects to HTTPS (not supported)");
                    return;
                }
                {
                    char resolved[URL_MAX];
                    resolve_url(fetch_url, target, resolved, URL_MAX);
                    strncpy(fetch_url, resolved, sizeof(fetch_url) - 1);
                    fetch_url[sizeof(fetch_url) - 1] = 0;
                }
                continue;          /* next hop */
            }
        }
        fetch_body = body ? body + 4 : page;
        set_status("Done");
        return;
    }
    set_status("Too many redirects");
}

static void *fetch_worker(void *arg) {
    (void)arg;
    fetch_body = 0;
    fetch();
    fetch_done = 1;
    return 0;
}

/* Navigate to `url`, recording history (push truncates forward entries). */
static void start_fetch(int record) {
    pthread_t t;

    if (fetch_busy) return;
    strncpy(fetch_url, url, sizeof(fetch_url) - 1);
    fetch_url[sizeof(fetch_url) - 1] = 0;
    if (record) {
        if (hist_pos < HIST_MAX - 1) {
            hist_pos++;
            hist_len = hist_pos + 1;
        } else {
            memmove(history[0], history[1],
                    (size_t)(HIST_MAX - 1) * URL_MAX);
        }
        strncpy(history[hist_pos], url, URL_MAX - 1);
        history[hist_pos][URL_MAX - 1] = 0;
    }
    fetch_busy = 1;
    fetch_done = 0;
    set_status("Loading...");
    if (pthread_create(&t, 0, fetch_worker, 0) != 0)
        fetch_worker(0);
    fetch_thread = t;
}

static void go_history(int delta) {
    int np = hist_pos + delta;

    if (np < 0 || np >= hist_len || fetch_busy) return;
    hist_pos = np;
    strncpy(url, history[np], sizeof(url) - 1);
    url[sizeof(url) - 1] = 0;
    url_len = (int)strlen(url);
    start_fetch(0);
}

/* ── Rendering ──────────────────────────────────────────────────────────── */

#define TOOLBAR_H 34
#define NAV_W 26

static uint32_t link_color(void) { return draw_rgb(36, 100, 184); }

static void render(void) {
    draw_surface_t *s = &gui.surf;
    int y, i;

    if (!s->px) return;
    draw_fill(s, draw_rgb(250, 250, 248));

    /* toolbar: back/forward + URL bar + GO */
    draw_rect(s, 0, 0, s->w, TOOLBAR_H, draw_rgb(52, 60, 76));
    draw_rect(s, 6, 6, NAV_W, 22,
              hist_pos > 0 ? draw_rgb(94, 129, 172) : draw_rgb(70, 78, 94));
    draw_text_aa(s, 6 + 9, 7, "<", draw_rgb(238, 240, 235), &draw_font_ui);
    draw_rect(s, 6 + NAV_W + 4, 6, NAV_W, 22,
              hist_pos < hist_len - 1 ? draw_rgb(94, 129, 172)
                                      : draw_rgb(70, 78, 94));
    draw_text_aa(s, 6 + NAV_W + 4 + 9, 7, ">", draw_rgb(238, 240, 235),
                 &draw_font_ui);
    {
        int ux = 6 + 2 * (NAV_W + 4);
        draw_rect(s, ux, 6, s->w - ux - 72, 22,
                  gui.focused ? draw_rgb(238, 240, 235)
                              : draw_rgb(210, 214, 218));
        {
            int pen = draw_text_aa(s, ux + 4, 8, url, draw_rgb(20, 24, 26),
                                   &draw_font_ui);
            draw_rect(s, ux + 4 + pen, 8, 2, 18, draw_rgb(94, 129, 172));
        }
    }
    draw_rect(s, s->w - 64, 6, 56, 22, draw_rgb(94, 129, 172));
    draw_text_aa(s, s->w - 64 + (56 - draw_text_width("GO", &draw_font_ui)) / 2,
                 7, "GO", draw_rgb(238, 240, 235), &draw_font_ui);
    draw_text_aa(s, 8, s->h - 21, status, draw_rgb(102, 110, 112),
                 &draw_font_ui);

    /* page text */
    if (view > line_count - 4) view = line_count - 4;
    if (view < 0) view = 0;
    y = TOOLBAR_H + 6;
    for (i = view; i < line_count && y < s->h - 26; i++) {
        int style = line_style[i];
        int lh = line_height(style);
        int x = 8;

        if (style == ST_PRE) {
            draw_text(s, x, y, linebuf[i], draw_rgb(60, 66, 74));
        } else {
            const draw_font_t *f = font_for(style);
            uint32_t body_col = style == ST_BODY ? draw_rgb(30, 34, 36)
                                                 : draw_rgb(16, 20, 24);
            char run[LINE_LEN];
            int ci = 0;

            while (linebuf[i][ci]) {
                unsigned char lid = line_link[i][ci];
                int rs = ci, rl = 0;
                while (linebuf[i][ci] && line_link[i][ci] == lid) {
                    ci++;
                    rl++;
                }
                memcpy(run, linebuf[i] + rs, (size_t)rl);
                run[rl] = 0;
                {
                    uint32_t col = lid == NO_LINK ? body_col : link_color();
                    int w = draw_text_aa(s, x, y, run, col, f);
                    if (lid != NO_LINK)        /* underline links */
                        draw_rect(s, x, y + f->line_h - 3, w, 1, col);
                    x += w;
                }
            }
        }
        y += lh;
    }

    wm_commit(&gui.wm, gui.slot);
    dirty = 0;
}

/* Map a click in the page body to a link id (or NO_LINK). */
static int link_at(int mx, int my) {
    int y = TOOLBAR_H + 6, i;

    for (i = view; i < line_count && y < gui.surf.h - 26; i++) {
        int lh = line_height(line_style[i]);
        if (my >= y && my < y + lh) {
            int x = 8, ci = 0;
            int style = line_style[i];
            if (style == ST_PRE) return NO_LINK;
            while (linebuf[i][ci]) {
                int w = char_width(linebuf[i][ci], style);
                if (mx >= x && mx < x + w)
                    return line_link[i][ci];
                x += w;
                ci++;
            }
            return NO_LINK;
        }
        y += lh;
    }
    return NO_LINK;
}

/* ── Input ──────────────────────────────────────────────────────────────── */

static void on_key(gui_window_t *g, int code, int value, int ascii) {
    (void)g;
    (void)value;
    if (code == KEY_ENTER) {
        start_fetch(1);
        dirty = 1;
        return;
    }
    if (code == KEY_BACKSPACE) {
        if (url_len) url[--url_len] = 0;
        dirty = 1;
        return;
    }
    if (code == KEY_PAGEUP)   { view -= 10; dirty = 1; return; }
    if (code == KEY_PAGEDOWN) { view += 10; dirty = 1; return; }
    if (ascii >= 32 && ascii < 127 && url_len + 1 < URL_MAX) {
        url[url_len++] = (char)ascii;
        url[url_len] = 0;
        dirty = 1;
    }
}

static void on_scroll(gui_window_t *g, int delta) {
    (void)g;
    view -= delta * 3;
    if (view < 0) view = 0;
    dirty = 1;
}

static void on_click(gui_window_t *g, int x, int y) {
    draw_surface_t *s = &g->surf;

    if (y < TOOLBAR_H) {
        if (x >= 6 && x < 6 + NAV_W)                  { go_history(-1); return; }
        if (x >= 6 + NAV_W + 4 && x < 6 + 2 * NAV_W + 4) {
            go_history(1);
            return;
        }
        if (x >= s->w - 64 && x < s->w - 8) {         /* GO */
            start_fetch(1);
            dirty = 1;
        }
        return;
    }
    {
        int lid = link_at(x, y);
        if (lid != NO_LINK && lid < link_count && !fetch_busy) {
            strncpy(url, links[lid], sizeof(url) - 1);
            url[sizeof(url) - 1] = 0;
            url_len = (int)strlen(url);
            start_fetch(1);
            dirty = 1;
        }
    }
}

int main(int argc, char *argv[]) {
    int slot = (argc > 1) ? atoi(argv[1]) : 1;

    if (slot < 1 || slot > WM_MAX_SLOTS) slot = 1;
    if (argc > 2) {                       /* open a URL passed by launch */
        strncpy(url, argv[2], sizeof(url) - 1);
        url[sizeof(url) - 1] = 0;
    }
    url_len = (int)strlen(url);

    if (gui_open(&gui, slot, "Browser", 120 + slot * 16, 60 + slot * 12,
                 680, 480) < 0) {
        printf("browse: desktop unavailable\n");
        return 1;
    }
    gui_set_key_handler(&gui, on_key);
    gui_set_scroll_handler(&gui, on_scroll);
    gui_set_click_handler(&gui, on_click);

    render();
    while (!gui.closed) {
        int events = gui_poll(&gui);
        if (fetch_done) {
            fetch_done = 0;
            if (fetch_body) {
                /* the fetch may have been redirected: show the final URL */
                strncpy(url, fetch_url, sizeof(url) - 1);
                url[sizeof(url) - 1] = 0;
                url_len = (int)strlen(url);
                if (hist_pos >= 0) {
                    strncpy(history[hist_pos], url, URL_MAX - 1);
                    history[hist_pos][URL_MAX - 1] = 0;
                }
                render_html(fetch_body);
            }
            pthread_join(fetch_thread, 0);
            fetch_busy = 0;
            dirty = 1;
        }
        if (dirty || events > 0)
            render();
        sleep_ms(40);
    }
    gui_close(&gui);
    return 0;
}
