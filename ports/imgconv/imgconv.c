/*
 * imgconv — decode an image file to PPM P6 for MaeroOS.
 *
 *   imgconv <input> <output.ppm>
 *
 * Supports PNG (libpng), JPEG (libjpeg), BMP (24/32-bit uncompressed), and
 * PPM passthrough.  Built musl-static in the cross container and run under
 * MaeroOS's Linux-ABI emulation (like links/busybox).  The desktop/settings/
 * viewer shell out to it so the OS gains real PNG/JPEG support without a
 * hand-rolled decoder.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <png.h>
#include <jpeglib.h>
#include <setjmp.h>

/* Write a tightly-packed RGB buffer as PPM P6. */
static int write_ppm(const char *path, int w, int h, const uint8_t *rgb) {
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "imgconv: cannot write %s\n", path); return 1; }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    fwrite(rgb, 1, (size_t)w * h * 3, f);
    fclose(f);
    return 0;
}

/* ── PNG ─────────────────────────────────────────────────────────────────── */
static int load_png(FILE *f, int *w, int *h, uint8_t **rgb) {
    unsigned char sig[8];
    if (fread(sig, 1, 8, f) != 8 || png_sig_cmp(sig, 0, 8)) return -1;

    png_structp p = png_create_read_struct(PNG_LIBPNG_VER_STRING, 0, 0, 0);
    png_infop info = png_create_info_struct(p);
    if (!p || !info) return -1;
    if (setjmp(png_jmpbuf(p))) { png_destroy_read_struct(&p, &info, 0); return -1; }
    png_init_io(p, f);
    png_set_sig_bytes(p, 8);
    png_read_info(p, info);

    int W = png_get_image_width(p, info), H = png_get_image_height(p, info);
    int ct = png_get_color_type(p, info), bd = png_get_bit_depth(p, info);
    if (bd == 16) png_set_strip_16(p);
    if (ct == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(p);
    if (ct == PNG_COLOR_TYPE_GRAY && bd < 8) png_set_expand_gray_1_2_4_to_8(p);
    if (png_get_valid(p, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(p);
    if (ct == PNG_COLOR_TYPE_GRAY || ct == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(p);
    /* Composite alpha onto white → solid RGB. */
    png_set_strip_alpha(p);
    png_read_update_info(p, info);

    uint8_t *out = malloc((size_t)W * H * 3);
    png_bytep *rows = malloc(sizeof(png_bytep) * H);
    if (!out || !rows) return -1;
    for (int y = 0; y < H; y++) rows[y] = out + (size_t)y * W * 3;
    png_read_image(p, rows);
    png_destroy_read_struct(&p, &info, 0);
    free(rows);
    *w = W; *h = H; *rgb = out;
    return 0;
}

/* ── JPEG ────────────────────────────────────────────────────────────────── */
struct jerr { struct jpeg_error_mgr mgr; jmp_buf jb; };
static void jpeg_panic(j_common_ptr ci) {
    longjmp(((struct jerr *)ci->err)->jb, 1);
}
static int load_jpeg(FILE *f, int *w, int *h, uint8_t **rgb) {
    struct jpeg_decompress_struct ci;
    struct jerr err;
    ci.err = jpeg_std_error(&err.mgr);
    err.mgr.error_exit = jpeg_panic;
    if (setjmp(err.jb)) { jpeg_destroy_decompress(&ci); return -1; }
    jpeg_create_decompress(&ci);
    jpeg_stdio_src(&ci, f);
    if (jpeg_read_header(&ci, TRUE) != JPEG_HEADER_OK) return -1;
    ci.out_color_space = JCS_RGB;
    jpeg_start_decompress(&ci);
    int W = ci.output_width, H = ci.output_height;
    uint8_t *out = malloc((size_t)W * H * 3);
    if (!out) return -1;
    while ((int)ci.output_scanline < H) {
        uint8_t *row = out + (size_t)ci.output_scanline * W * 3;
        jpeg_read_scanlines(&ci, &row, 1);
    }
    jpeg_finish_decompress(&ci);
    jpeg_destroy_decompress(&ci);
    *w = W; *h = H; *rgb = out;
    return 0;
}

/* ── BMP (24/32-bit uncompressed) ────────────────────────────────────────── */
static uint32_t rd32(const uint8_t *p){return p[0]|p[1]<<8|p[2]<<16|(uint32_t)p[3]<<24;}
static int load_bmp(FILE *f, int *w, int *h, uint8_t **rgb) {
    uint8_t hd[54];
    if (fread(hd, 1, 54, f) != 54 || hd[0] != 'B' || hd[1] != 'M') return -1;
    uint32_t off = rd32(hd + 10);
    int W = (int)rd32(hd + 18), H = (int)rd32(hd + 22);
    int bpp = hd[28] | hd[29] << 8;
    if ((bpp != 24 && bpp != 32) || rd32(hd + 30) != 0 || W <= 0 || H == 0)
        return -1;
    int flip = H > 0; if (H < 0) H = -H;
    int bypp = bpp / 8, stride = (W * bypp + 3) & ~3;
    uint8_t *raw = malloc((size_t)stride * H);
    if (!raw) return -1;
    fseek(f, off, SEEK_SET);
    if (fread(raw, 1, (size_t)stride * H, f) != (size_t)stride * H) { free(raw); return -1; }
    uint8_t *out = malloc((size_t)W * H * 3);
    if (!out) { free(raw); return -1; }
    for (int y = 0; y < H; y++) {
        const uint8_t *srow = raw + (size_t)(flip ? H - 1 - y : y) * stride;
        uint8_t *drow = out + (size_t)y * W * 3;
        for (int x = 0; x < W; x++) {
            drow[x*3+0] = srow[x*bypp+2];  /* BMP is BGR */
            drow[x*3+1] = srow[x*bypp+1];
            drow[x*3+2] = srow[x*bypp+0];
        }
    }
    free(raw);
    *w = W; *h = H; *rgb = out;
    return 0;
}

/* ── PPM P6 passthrough ──────────────────────────────────────────────────── */
static int load_ppm(FILE *f, int *w, int *h, uint8_t **rgb) {
    char m[3] = {0};
    int W, H, mx;
    rewind(f);
    if (fscanf(f, "%2s", m) != 1 || strcmp(m, "P6")) return -1;
    if (fscanf(f, "%d %d %d", &W, &H, &mx) != 3 || W <= 0 || H <= 0) return -1;
    fgetc(f);  /* single whitespace after maxval */
    uint8_t *out = malloc((size_t)W * H * 3);
    if (!out) return -1;
    if (fread(out, 1, (size_t)W * H * 3, f) != (size_t)W * H * 3) { free(out); return -1; }
    *w = W; *h = H; *rgb = out;
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: imgconv <input> <output.ppm>\n");
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "imgconv: cannot open %s\n", argv[1]); return 1; }

    int w = 0, h = 0;
    uint8_t *rgb = 0;
    int ok = -1;
    /* Probe by trying each decoder (each rewinds/handles its own header). */
    rewind(f); ok = load_png(f, &w, &h, &rgb);
    if (ok) { rewind(f); ok = load_jpeg(f, &w, &h, &rgb); }
    if (ok) { rewind(f); ok = load_bmp(f, &w, &h, &rgb); }
    if (ok) { rewind(f); ok = load_ppm(f, &w, &h, &rgb); }
    fclose(f);
    if (ok || !rgb) {
        fprintf(stderr, "imgconv: unsupported or corrupt image %s\n", argv[1]);
        return 1;
    }
    int rc = write_ppm(argv[2], w, h, rgb);
    free(rgb);
    return rc;
}
