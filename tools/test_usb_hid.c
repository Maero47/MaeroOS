/*
 * Host test for the USB HID report-descriptor parser and report handling
 * (drivers/usb/usb_hid.c); run by tools/test_usb_hid.py.
 *
 * A USB device chooses its own report descriptor and reports, so everything
 * here is hostile input: the full 32-bit logical range that once wrapped the
 * scaler's divisor to 0, zero-sized and oversized fields, reversed ranges,
 * truncated items, and a few hundred thousand random descriptors built from
 * the item grammar, each fed random reports.  The driver file is compiled in
 * directly with its kernel hooks stubbed out; ASan/UBSan (and SIGFPE) catch
 * what the asserts do not.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "usb/usb_hid.c"

/* ── kernel stubs ────────────────────────────────────────────────────────── */
static int32_t fb_w = 1024, fb_h = 768;
static int32_t ptr_x, ptr_y;
static int mouse_calls;

int framebuffer_ioctl(uint32_t req, void *arg) {
    if (req != FBIOGET_VSCREENINFO) return -1;
    fb_var_screeninfo_t *v = arg;
    memset(v, 0, sizeof(*v));
    v->xres = (uint32_t)fb_w;
    v->yres = (uint32_t)fb_h;
    return 0;
}
void keyboard_input_key(uint16_t key, int pressed) { (void)key; (void)pressed; }
void mouse_input(int32_t dx, int32_t dy, int32_t wheel, uint8_t buttons) {
    (void)wheel; (void)buttons;
    /* the consumers clamp to the screen, as the desktop does */
    int64_t x = (int64_t)ptr_x + dx, y = (int64_t)ptr_y + dy;
    ptr_x = x < 0 ? 0 : x > INT32_MAX ? INT32_MAX : (int32_t)x;
    ptr_y = y < 0 ? 0 : y > INT32_MAX ? INT32_MAX : (int32_t)y;
    mouse_calls++;
}
uint32_t pit_ticks(void) { return 0; }

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static void setup(hid_state_t *st, const uint8_t *d, uint32_t len) {
    usb_interface_desc_t intf;
    memset(&intf, 0, sizeof(intf));
    intf.bInterfaceClass = 3;
    memset(st, 0, sizeof(*st));
    hid_setup(st, &intf, d, len);
}

/* Absolute X/Y, 32 bits each, Logical Min INT32_MIN .. Max INT32_MAX: the
 * descriptor from the review (17 00 00 00 80 27 FF FF FF 7F). */
static const uint8_t full_range_desc[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31,
    0x17, 0x00, 0x00, 0x00, 0x80,           /* Logical Min  INT32_MIN */
    0x27, 0xFF, 0xFF, 0xFF, 0x7F,           /* Logical Max  INT32_MAX */
    0x75, 0x20, 0x95, 0x02, 0x81, 0x02,     /* 2 x 32 bits, Data,Var,Abs */
    0xC0,
};

static void put32(uint8_t *p, int32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)((uint32_t)v >> (8 * i));
}

static void test_full_range(void) {
    hid_state_t st;
    setup(&st, full_range_desc, sizeof(full_range_desc));
    CHECK(st.kind == HID_KIND_POINTER, "full-range tablet not a pointer (%d)", st.kind);
    CHECK(st.x.lmin == INT32_MIN && st.x.lmax == INT32_MAX, "range not parsed");
    uint8_t r[8];
    ptr_x = ptr_y = 0;
    static const int32_t xs[] = { INT32_MIN, -1, 0, 1, INT32_MAX };
    for (unsigned i = 0; i < sizeof(xs) / sizeof(xs[0]); i++) {
        put32(r, xs[i]);
        put32(r + 4, xs[i]);
        hid_report(&st, r, sizeof(r));
        CHECK(ptr_x >= 0 && ptr_x < fb_w, "x %d off screen for %d", ptr_x, xs[i]);
        CHECK(ptr_y >= 0 && ptr_y < fb_h, "y %d off screen for %d", ptr_y, xs[i]);
    }
    /* the middle of the range is the middle of the screen */
    put32(r, 0);
    put32(r + 4, 0);
    hid_report(&st, r, sizeof(r));
    CHECK(ptr_x == fb_w / 2 && ptr_y == fb_h / 2, "centre at %d,%d", ptr_x, ptr_y);
    put32(r, INT32_MAX);
    put32(r + 4, INT32_MAX);
    hid_report(&st, r, sizeof(r));
    CHECK(ptr_x == fb_w - 1 && ptr_y == fb_h - 1, "max at %d,%d", ptr_x, ptr_y);
}

/* QEMU's usb-tablet: 0..32767 on both axes. */
static void test_qemu_tablet(void) {
    static const uint8_t d[] = {
        0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01, 0xA1, 0x00,
        0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01,
        0x95, 0x03, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05,
        0x81, 0x01, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x00,
        0x26, 0xFF, 0x7F, 0x35, 0x00, 0x46, 0xFF, 0x7F, 0x75, 0x10,
        0x95, 0x02, 0x81, 0x02, 0x05, 0x01, 0x09, 0x38, 0x15, 0x81,
        0x25, 0x7F, 0x35, 0x00, 0x45, 0x00, 0x75, 0x08, 0x95, 0x01,
        0x81, 0x06, 0xC0, 0xC0,
    };
    hid_state_t st;
    setup(&st, d, sizeof(d));
    CHECK(st.kind == HID_KIND_POINTER, "qemu tablet not a pointer");
    ptr_x = ptr_y = 0;
    uint8_t r[6] = { 0, 0x00, 0x40, 0x00, 0x20, 0 };      /* x 16384, y 8192 */
    hid_report(&st, r, sizeof(r));
    CHECK(ptr_x == 512 && ptr_y == 192, "tablet at %d,%d", ptr_x, ptr_y);
}

/* Degenerate ranges: equal, reversed, zero-size fields; and a wild screen. */
static void test_degenerate(void) {
    hid_field_t f;
    memset(&f, 0, sizeof(f));
    f.size = 16;
    f.lmin = 5; f.lmax = 5;
    CHECK(scale_abs(&f, 5, 1024) == 0, "empty range");
    f.lmin = 10; f.lmax = -10;
    CHECK(scale_abs(&f, 0, 1024) == 0, "reversed range");
    f.lmin = INT32_MIN; f.lmax = INT32_MAX;
    CHECK(scale_abs(&f, INT32_MAX, 0x7FFFFFFF) < 0x10000, "huge extent");
    CHECK(scale_abs(&f, 0, -5) == 0, "negative extent");
    f.lmin = INT32_MIN; f.lmax = INT32_MIN + 1;
    CHECK(scale_abs(&f, INT32_MAX, 100) == 50, "two-value range");
    f.size = 0;
    uint8_t r[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
    f.is_signed = 1;
    CHECK(get_field(&f, r, 4) == 0, "zero-size field");
}

/* ── fuzz ────────────────────────────────────────────────────────────────── */
static uint32_t rng = 0x12345678;
static uint32_t rnd(void) {
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return rng;
}

static const int32_t edge[] = {
    0, 1, -1, 127, -128, 255, 32767, -32768, 65535, INT32_MAX, INT32_MIN,
    INT32_MAX - 1, INT32_MIN + 1, 0x10000, 0x10001,
};

static uint32_t emit_item(uint8_t *d, uint32_t at, uint8_t tag, uint32_t v) {
    uint32_t size = rnd() % 4;
    d[at++] = (uint8_t)(tag | size);
    uint32_t n = size == 3 ? 4 : size;
    for (uint32_t k = 0; k < n; k++) d[at++] = (uint8_t)(v >> (8 * k));
    return at;
}

static uint32_t random_desc(uint8_t *d, uint32_t cap) {
    static const uint8_t tags[] = {
        0x04, 0x14, 0x24, 0x74, 0x94, 0x84, 0x08, 0x18, 0x28,
        0x80, 0x90, 0xB0, 0xA0, 0xC0, 0x34, 0x44,
    };
    uint32_t at = 0;
    uint32_t items = 1 + rnd() % 40;
    for (uint32_t i = 0; i < items && at + 5 < cap; i++) {
        uint8_t tag = tags[rnd() % sizeof(tags)];
        uint32_t v;
        switch (rnd() % 4) {
        case 0: v = (uint32_t)edge[rnd() % (sizeof(edge) / sizeof(edge[0]))]; break;
        case 1: v = rnd() & 0xFF; break;
        default: v = rnd(); break;
        }
        if (tag == 0x04) v = (rnd() & 1) ? 0x01 : (rnd() & 1) ? 0x09 : 0x0C;
        if (tag == 0x08) v = 0x30 + rnd() % 10;
        if (tag == 0x74) v = rnd() % 40;
        if (tag == 0x94) v = rnd() % 20;
        at = emit_item(d, at, tag, v);
        if (rnd() % 64 == 0 && at + 3 < cap) {   /* a long item */
            d[at++] = 0xFE;
            d[at++] = (uint8_t)(rnd() % 8);
            d[at++] = 0;
        }
    }
    /* sometimes chop the last item short */
    if (at && rnd() % 8 == 0) at -= 1 + rnd() % (at < 3 ? at : 3);
    return at;
}

static void fuzz(int rounds) {
    uint8_t d[256], r[64];
    for (int i = 0; i < rounds; i++) {
        uint32_t len = random_desc(d, sizeof(d));
        hid_state_t st;
        fb_w = (rnd() % 8 == 0) ? (int32_t)(rnd() % 100000) : 1024;
        fb_h = (rnd() % 8 == 0) ? (int32_t)(rnd() % 100000) : 768;
        setup(&st, d, len);
        for (int k = 0; k < 8; k++) {
            uint32_t rl = rnd() % sizeof(r);
            for (uint32_t b = 0; b < rl; b++) r[b] = (uint8_t)rnd();
            if (rl && st.report_id && (rnd() & 1)) r[0] = st.report_id;
            hid_report(&st, r, rl);
        }
    }
    fb_w = 1024;
    fb_h = 768;
}

int main(void) {
    capture = 0;
    test_full_range();
    test_qemu_tablet();
    test_degenerate();
    fuzz(200000);
    CHECK(hid_selftest() == 0, "hid_selftest");
    if (fails) {
        printf("test_usb_hid: %d failure(s)\n", fails);
        return 1;
    }
    printf("test_usb_hid: ok\n");
    return 0;
}
