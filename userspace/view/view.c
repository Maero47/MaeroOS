#include <draw.h>
#include <fcntl.h>
#include <gui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/*
 * view — image viewer for PPM (P6) and uncompressed BMP (24/32-bit).
 * Fits the image to the window with nearest-neighbor scaling.
 */

static gui_window_t gui;
static uint32_t *img;
static int img_w, img_h;
static char path[160];
static char status[96] = "No image";
static int dirty = 1;

static void sleep_ms(long ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, 0);
}

static uint8_t *read_all(const char *p, int *out_len) {
    int fd = open(p, O_RDONLY), n, total = 0, cap = 256 * 1024;
    uint8_t *buf;

    if (fd < 0) return 0;
    buf = (uint8_t *)malloc((size_t)cap);
    if (!buf) { close(fd); return 0; }
    for (;;) {
        if (total == cap) {
            uint8_t *nb = (uint8_t *)malloc((size_t)cap * 2);
            if (!nb) break;
            memcpy(nb, buf, (size_t)total);
            free(buf);
            buf = nb;
            cap *= 2;
        }
        n = read(fd, buf + total, cap - total);
        if (n <= 0) break;
        total += n;
    }
    close(fd);
    *out_len = total;
    return buf;
}

static int load_ppm(const uint8_t *d, int len) {
    int pos = 2, field = 0, w = 0, h = 0, maxv = 0;

    if (len < 12 || d[0] != 'P' || d[1] != '6') return -1;
    while (pos < len && field < 3) {
        while (pos < len && (d[pos] == ' ' || d[pos] == '\n' ||
                             d[pos] == '\t' || d[pos] == '\r'))
            pos++;
        if (pos < len && d[pos] == '#') {        /* comment line */
            while (pos < len && d[pos] != '\n') pos++;
            continue;
        }
        {
            int v = 0;
            while (pos < len && d[pos] >= '0' && d[pos] <= '9')
                v = v * 10 + (d[pos++] - '0');
            if (field == 0) w = v;
            else if (field == 1) h = v;
            else maxv = v;
            field++;
        }
    }
    pos++;
    if (w < 1 || h < 1 || w > 4096 || h > 4096 || maxv != 255) return -1;
    if (len - pos < w * h * 3) return -1;
    img = (uint32_t *)malloc((size_t)w * h * 4);
    if (!img) return -1;
    for (int i = 0; i < w * h; i++)
        img[i] = ((uint32_t)d[pos + i * 3] << 16) |
                 ((uint32_t)d[pos + i * 3 + 1] << 8) | d[pos + i * 3 + 2];
    img_w = w;
    img_h = h;
    return 0;
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int load_bmp(const uint8_t *d, int len) {
    uint32_t off, hdr, bpp, neg = 0;
    int w, h;

    if (len < 54 || d[0] != 'B' || d[1] != 'M') return -1;
    off = rd32(d + 10);
    hdr = rd32(d + 14);
    if (hdr < 40) return -1;
    w = (int)rd32(d + 18);
    h = (int)rd32(d + 22);
    if (h < 0) { h = -h; neg = 1; }    /* top-down DIB */
    bpp = (uint32_t)d[28] | ((uint32_t)d[29] << 8);
    if (rd32(d + 30) != 0) return -1;  /* compressed: unsupported */
    if ((bpp != 24 && bpp != 32) || w < 1 || h < 1 || w > 4096 || h > 4096)
        return -1;

    {
        int stride = (int)((w * (int)bpp / 8 + 3) & ~3);
        if ((int)off + stride * h > len) return -1;
        img = (uint32_t *)malloc((size_t)w * h * 4);
        if (!img) return -1;
        for (int y = 0; y < h; y++) {
            int src_y = neg ? y : h - 1 - y;   /* BMP rows are bottom-up */
            const uint8_t *row = d + off + (size_t)src_y * stride;
            for (int x = 0; x < w; x++) {
                const uint8_t *px = row + x * ((int)bpp / 8);
                img[(size_t)y * w + x] = ((uint32_t)px[2] << 16) |
                                         ((uint32_t)px[1] << 8) | px[0];
            }
        }
    }
    img_w = w;
    img_h = h;
    return 0;
}

static int ends_with(const char *s, const char *suf) {
    int sl = (int)strlen(s), fl = (int)strlen(suf);
    return sl >= fl && !strcmp(s + sl - fl, suf);
}

/* For PNG/JPEG/GIF, decode to a temp PPM via imgconv and load that. */
static const char *decode_path(const char *p) {
    static char cache[80];
    if (ends_with(p, ".ppm") || ends_with(p, ".bmp")) return p;
    const char *conv = access("/disk/bin/imgconv", 1) == 0 ?
                       "/disk/bin/imgconv" : "/bin/imgconv";
    mkdir("/disk/etc", 0755);
    snprintf(cache, sizeof(cache), "/disk/etc/view-cache.ppm");
    int pid = fork();
    if (pid == 0) {
        char *argv[] = { (char *)conv, (char *)p, cache, 0 };
        execve(conv, argv, 0);
        _exit(127);
    }
    if (pid > 0) {
        int st = 0;
        waitpid(pid, &st, 0);
        if (st == 0 && access(cache, 4) == 0) return cache;
    }
    return p;
}

static void load_image(void) {
    int len = 0;
    const char *lp = decode_path(path);
    uint8_t *d = read_all(lp, &len);

    if (img) { free(img); img = 0; }
    img_w = img_h = 0;
    if (!d) { snprintf(status, sizeof(status), "Cannot open %s", path); return; }
    if (load_ppm(d, len) == 0 || load_bmp(d, len) == 0)
        snprintf(status, sizeof(status), "%s  (%dx%d)", path, img_w, img_h);
    else
        snprintf(status, sizeof(status), "Unsupported format: %s", path);
    free(d);
    dirty = 1;
}

static void render(void) {
    draw_surface_t *s = &gui.surf;

    if (!s->px) return;
    draw_fill(s, draw_rgb(38, 41, 48));

    if (img) {
        /* fit, preserving aspect, nearest neighbor */
        int avail_w = s->w - 8, avail_h = s->h - 32;
        int dw = avail_w, dh = img_h * avail_w / img_w;
        if (dh > avail_h) { dh = avail_h; dw = img_w * avail_h / img_h; }
        if (dw > img_w && dh > img_h) { dw = img_w; dh = img_h; }  /* no upscale */
        {
            int ox = (s->w - dw) / 2, oy = (s->h - 24 - dh) / 2;
            for (int y = 0; y < dh; y++) {
                uint32_t *dst = s->px + (size_t)(oy + y) * s->w + ox;
                const uint32_t *src = img + (size_t)(y * img_h / dh) * img_w;
                for (int x = 0; x < dw; x++)
                    dst[x] = src[x * img_w / dw];
            }
        }
    }
    draw_text_aa(s, 8, s->h - 22, status, draw_rgb(200, 206, 214),
                 &draw_font_ui);
    wm_commit(&gui.wm, gui.slot);
    dirty = 0;
}

int main(int argc, char *argv[]) {
    int slot = (argc > 1) ? atoi(argv[1]) : 1;

    if (slot < 1 || slot > WM_MAX_SLOTS) slot = 1;
    if (argc > 2) {
        strncpy(path, argv[2], sizeof(path) - 1);
        path[sizeof(path) - 1] = 0;
    } else {
        strcpy(path, "/disk/wallpaper.ppm");
    }

    if (gui_open(&gui, slot, "Viewer", 180 + slot * 14, 70 + slot * 10,
                 560, 440) < 0) {
        printf("view: desktop unavailable\n");
        return 1;
    }
    load_image();
    render();
    while (!gui.closed) {
        int events = gui_poll(&gui);
        if (dirty || events > 0)
            render();
        sleep_ms(50);
    }
    gui_close(&gui);
    return 0;
}
