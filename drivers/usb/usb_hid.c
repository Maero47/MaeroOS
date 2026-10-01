/*
 * USB HID class driver: boot-protocol keyboards and mice, and report-protocol
 * pointers (absolute tablets such as QEMU's usb-tablet, and mice without a
 * boot interface) through a small report-descriptor parser.
 *
 * Written from the Device Class Definition for HID 1.11 (report descriptor
 * items, section 6.2.2; boot reports, appendix B) and the HID Usage Tables
 * (keyboard page 0x07, generic desktop page 0x01, button page 0x09).
 *
 * Output goes into the same rings as the PS/2 drivers: keys into
 * /dev/input/event0 (keyboard_input_key), pointer reports into
 * /dev/input/event1 (mouse_input).  That pointer interface is relative, so an
 * absolute device is turned into motion: its first report pins the pointer
 * into the top-left corner (consumers clamp there) and moves it to the
 * absolute position, every later report moves by the difference.  Positions
 * are scaled to the framebuffer size, so the pointer lands where the device
 * says.
 */
#include "usb.h"
#include "../framebuffer.h"
#include "../keyboard.h"
#include "../mouse.h"
#include "../../lib/string.h"
#include <stdint.h>

/* HID keyboard usage (page 0x07) -> Linux evdev key code.  The table is the
 * usage list of the HID Usage Tables 1.12 section 10 matched to the key
 * names of <linux/input-event-codes.h> (numbers, which are ABI). */
static const uint8_t usage_to_key[0x66] = {
    [0x04] = 30, [0x05] = 48, [0x06] = 46, [0x07] = 32,   /* a b c d */
    [0x08] = 18, [0x09] = 33, [0x0A] = 34, [0x0B] = 35,   /* e f g h */
    [0x0C] = 23, [0x0D] = 36, [0x0E] = 37, [0x0F] = 38,   /* i j k l */
    [0x10] = 50, [0x11] = 49, [0x12] = 24, [0x13] = 25,   /* m n o p */
    [0x14] = 16, [0x15] = 19, [0x16] = 31, [0x17] = 20,   /* q r s t */
    [0x18] = 22, [0x19] = 47, [0x1A] = 17, [0x1B] = 45,   /* u v w x */
    [0x1C] = 21, [0x1D] = 44,                             /* y z */
    [0x1E] = 2,  [0x1F] = 3,  [0x20] = 4,  [0x21] = 5,    /* 1 2 3 4 */
    [0x22] = 6,  [0x23] = 7,  [0x24] = 8,  [0x25] = 9,    /* 5 6 7 8 */
    [0x26] = 10, [0x27] = 11,                             /* 9 0 */
    [0x28] = 28,                                          /* Enter */
    [0x29] = 1,                                           /* Esc */
    [0x2A] = 14,                                          /* Backspace */
    [0x2B] = 15,                                          /* Tab */
    [0x2C] = 57,                                          /* Space */
    [0x2D] = 12, [0x2E] = 13,                             /* - = */
    [0x2F] = 26, [0x30] = 27,                             /* [ ] */
    [0x31] = 43,                                          /* \ */
    [0x32] = 43,                                          /* non-US # */
    [0x33] = 39, [0x34] = 40, [0x35] = 41,                /* ; ' ` */
    [0x36] = 51, [0x37] = 52, [0x38] = 53,                /* , . / */
    [0x39] = 58,                                          /* Caps Lock */
    [0x3A] = 59, [0x3B] = 60, [0x3C] = 61, [0x3D] = 62,   /* F1-F4 */
    [0x3E] = 63, [0x3F] = 64, [0x40] = 65, [0x41] = 66,   /* F5-F8 */
    [0x42] = 67, [0x43] = 68, [0x44] = 87, [0x45] = 88,   /* F9-F12 */
    [0x46] = 99,                                          /* PrintScreen */
    [0x47] = 70,                                          /* Scroll Lock */
    [0x48] = 119,                                         /* Pause */
    [0x49] = 110, [0x4A] = 102, [0x4B] = 104,             /* Ins Home PgUp */
    [0x4C] = 111, [0x4D] = 107, [0x4E] = 109,             /* Del End PgDn */
    [0x4F] = 106, [0x50] = 105, [0x51] = 108, [0x52] = 103, /* → ← ↓ ↑ */
    [0x53] = 69,                                          /* Num Lock */
    [0x54] = 98,  [0x55] = 55,  [0x56] = 74,  [0x57] = 78, /* KP / * - + */
    [0x58] = 28,                                          /* KP Enter: as the
                                                           * PS/2 driver does */
    [0x59] = 79, [0x5A] = 80, [0x5B] = 81, [0x5C] = 75,   /* KP 1-4 */
    [0x5D] = 76, [0x5E] = 77, [0x5F] = 71, [0x60] = 72,   /* KP 5-8 */
    [0x61] = 73, [0x62] = 82, [0x63] = 83,                /* KP 9 0 . */
    [0x64] = 86,                                          /* non-US \ (102nd) */
    [0x65] = 127,                                         /* Application */
};

/* Modifier byte bits 0-7: LCtrl LShift LAlt LGUI RCtrl RShift RAlt RGUI. */
static const uint8_t modifier_keys[8] = { 29, 42, 56, 125, 97, 54, 100, 126 };

const char *hid_kind_name(int kind) {
    switch (kind) {
    case HID_KIND_KEYBOARD: return "keyboard";
    case HID_KIND_MOUSE:    return "mouse";
    case HID_KIND_POINTER:  return "pointer";
    default:                return "none";
    }
}

/* ── report descriptor parser (HID 1.11 6.2.2) ───────────────────────────── */

#define MAX_USAGES 16

/* Find the first report (one report ID) carrying generic-desktop X and Y
 * with buttons and an optional wheel, and record where they sit. */
static int parse_pointer(hid_state_t *st, const uint8_t *d, uint32_t len) {
    uint32_t usage_page = 0, report_size = 0, report_count = 0;
    int32_t lmin = 0, lmax = 0;
    uint32_t report_id = 0;
    uint32_t usages[MAX_USAGES];
    uint32_t nusages = 0, umin = 0, umax = 0;
    int have_range = 0;
    uint32_t bitpos[256];
    memset(bitpos, 0, sizeof(bitpos));
    int found_id = -1;

    for (uint32_t i = 0; i < len;) {
        uint8_t prefix = d[i];
        if (prefix == 0xFE) {                     /* long item: skip */
            if (i + 2 >= len) break;
            i += 3u + d[i + 1];
            continue;
        }
        uint32_t size = prefix & 3;
        if (size == 3) size = 4;
        if (i + 1 + size > len) break;
        uint32_t uval = 0;
        for (uint32_t k = 0; k < size; k++)
            uval |= (uint32_t)d[i + 1 + k] << (8 * k);
        int32_t sval = (int32_t)uval;
        if (size == 1) sval = (int8_t)uval;
        else if (size == 2) sval = (int16_t)uval;
        uint8_t tag = prefix & 0xFC;
        i += 1 + size;

        switch (tag) {
        case 0x04: usage_page = uval; break;                  /* Usage Page */
        case 0x14: lmin = sval; break;                        /* Logical Min */
        case 0x24: lmax = sval; break;                        /* Logical Max */
        case 0x74: report_size = uval; break;                 /* Report Size */
        case 0x94: report_count = uval; break;                /* Report Count */
        case 0x84: report_id = uval & 0xFF; break;            /* Report ID */
        case 0x08:                                            /* Usage */
            if (nusages < MAX_USAGES)
                usages[nusages++] = size == 4 ? uval : (usage_page << 16) | uval;
            break;
        case 0x18: umin = size == 4 ? uval : (usage_page << 16) | uval;
                   have_range = 1; break;                     /* Usage Min */
        case 0x28: umax = size == 4 ? uval : (usage_page << 16) | uval;
                   have_range = 1; break;                     /* Usage Max */
        case 0x80: {                                          /* Input */
            uint32_t start = bitpos[report_id];
            bitpos[report_id] += report_size * report_count;
            int constant = uval & 1;
            int relative = (uval >> 2) & 1;
            if (!constant && (found_id < 0 || found_id == (int)report_id)) {
                for (uint32_t n = 0; n < report_count; n++) {
                    uint32_t u;
                    if (have_range) u = umin + n;
                    else if (nusages) u = usages[n < nusages ? n : nusages - 1];
                    else break;
                    if (have_range && u > umax) break;
                    hid_field_t *f = 0;
                    if ((u >> 16) == 0x09) {                  /* buttons */
                        if (!st->buttons.size) {
                            st->buttons.offset = (uint16_t)(start + n * report_size);
                            st->buttons.size = (uint8_t)report_size;
                            st->buttons.count = 0;
                        }
                        if (st->buttons.count < 8) st->buttons.count++;
                        continue;
                    }
                    if (u == 0x00010030) f = &st->x;
                    else if (u == 0x00010031) f = &st->y;
                    else if (u == 0x00010038) f = &st->wheel;
                    if (!f || f->size) continue;
                    f->offset = (uint16_t)(start + n * report_size);
                    f->size = (uint8_t)(report_size > 32 ? 32 : report_size);
                    f->relative = (uint8_t)relative;
                    f->lmin = lmin;
                    f->lmax = lmax;
                    f->is_signed = lmin < 0;
                    if (f == &st->x || f == &st->y) found_id = (int)report_id;
                }
                if (found_id >= 0) st->report_id = (uint8_t)found_id;
            }
            nusages = 0; have_range = 0;
            break;
        }
        case 0x90: case 0xB0:                                 /* Output/Feature */
        case 0xA0: case 0xC0:                                 /* Collection */
            nusages = 0; have_range = 0;
            break;
        default:
            break;
        }
    }
    return st->x.size && st->y.size ? 0 : -1;
}

static int32_t get_field(const hid_field_t *f, const uint8_t *data,
                         uint32_t len) {
    uint32_t v = 0;
    for (uint32_t b = 0; b < f->size; b++) {
        uint32_t bit = f->offset + b;
        if (bit / 8 >= len) break;
        if (data[bit / 8] & (1U << (bit % 8))) v |= 1U << b;
    }
    if (f->is_signed && f->size < 32 && (v & (1U << (f->size - 1))))
        v |= ~0U << f->size;
    return (int32_t)v;
}

void hid_setup(hid_state_t *st, const usb_interface_desc_t *intf,
               const uint8_t *report_desc, uint32_t len) {
    st->kind = HID_KIND_NONE;
    if (intf->bInterfaceSubClass == 1 && intf->bInterfaceProtocol == 1) {
        st->kind = HID_KIND_KEYBOARD;
        return;
    }
    if (intf->bInterfaceSubClass == 1 && intf->bInterfaceProtocol == 2) {
        st->kind = HID_KIND_MOUSE;
        return;
    }
    if (report_desc && parse_pointer(st, report_desc, len) == 0)
        st->kind = HID_KIND_POINTER;
}

/* ── reports ─────────────────────────────────────────────────────────────── */

static int key_in(const uint8_t *keys, uint8_t usage) {
    for (int i = 2; i < 8; i++)
        if (keys[i] == usage) return 1;
    return 0;
}

/* Boot keyboard report: modifiers, reserved, six key usages (HID B.1). */
static void keyboard_report(hid_state_t *st, const uint8_t *r, uint32_t len) {
    uint8_t cur[8];
    memset(cur, 0, sizeof(cur));
    memcpy(cur, r, len < 8 ? len : 8);
    /* Phantom state (rollover error: every slot 0x01) carries no news. */
    if (cur[2] == 0x01) return;

    uint8_t changed = cur[0] ^ st->prev[0];
    for (int b = 0; b < 8; b++)
        if (changed & (1U << b))
            keyboard_input_key(modifier_keys[b], (cur[0] >> b) & 1);
    for (int i = 2; i < 8; i++) {
        uint8_t u = st->prev[i];
        if (u > 3 && !key_in(cur, u) && u < sizeof(usage_to_key) &&
            usage_to_key[u])
            keyboard_input_key(usage_to_key[u], 0);
    }
    for (int i = 2; i < 8; i++) {
        uint8_t u = cur[i];
        if (u > 3 && !key_in(st->prev, u) && u < sizeof(usage_to_key) &&
            usage_to_key[u])
            keyboard_input_key(usage_to_key[u], 1);
    }
    memcpy(st->prev, cur, 8);
}

static void screen_size(int32_t *w, int32_t *h) {
    fb_var_screeninfo_t var;
    if (framebuffer_ioctl(FBIOGET_VSCREENINFO, &var) == 0 && var.xres &&
        var.yres) {
        *w = (int32_t)var.xres;
        *h = (int32_t)var.yres;
    } else {
        *w = 1024;
        *h = 768;
    }
}

static int32_t scale_abs(const hid_field_t *f, int32_t v, int32_t extent) {
    if (f->lmax <= f->lmin) return 0;
    if (v < f->lmin) v = f->lmin;
    if (v > f->lmax) v = f->lmax;
    /* 32-bit arithmetic only (no libgcc 64-bit division in the kernel). */
    uint32_t num = (uint32_t)(v - f->lmin);
    uint32_t range = (uint32_t)(f->lmax - f->lmin) + 1;
    while (range > 0x10000U) {
        num >>= 1;
        range >>= 1;
    }
    return (int32_t)((num * (uint32_t)extent) / range);
}

static void pointer_report(hid_state_t *st, const uint8_t *r, uint32_t len) {
    if (st->report_id) {
        if (len < 1 || r[0] != st->report_id) return;
        r++;
        len--;
    }
    uint8_t buttons = 0;
    for (uint32_t b = 0; b < st->buttons.count && b < 3; b++) {
        hid_field_t one = st->buttons;
        one.offset = (uint16_t)(st->buttons.offset + b * st->buttons.size);
        one.is_signed = 0;
        if (get_field(&one, r, len)) buttons |= (uint8_t)(1U << b);
    }
    int32_t x = get_field(&st->x, r, len);
    int32_t y = get_field(&st->y, r, len);
    int32_t wheel = st->wheel.size ? get_field(&st->wheel, r, len) : 0;
    int32_t dx = x, dy = y;

    if (!st->x.relative) {
        int32_t w, h;
        screen_size(&w, &h);
        int32_t sx = scale_abs(&st->x, x, w);
        int32_t sy = scale_abs(&st->y, y, h);
        if (!st->have_abs) {
            /* Pin to the corner first: from there, relative == absolute. */
            mouse_input(-16384, -16384, 0, buttons);
            st->last_x = 0;
            st->last_y = 0;
            st->have_abs = 1;
        }
        dx = sx - st->last_x;
        dy = sy - st->last_y;
        st->last_x = sx;
        st->last_y = sy;
    }
    mouse_input(dx, dy, wheel, buttons);
}

void hid_report(hid_state_t *st, const uint8_t *data, uint32_t len) {
    if (!len) return;
    switch (st->kind) {
    case HID_KIND_KEYBOARD:
        keyboard_report(st, data, len);
        break;
    case HID_KIND_MOUSE: {
        /* Boot mouse (HID B.2): buttons, dx, dy, optional wheel. */
        if (len < 3) return;
        int32_t wheel = len >= 4 ? (int8_t)data[3] : 0;
        mouse_input((int8_t)data[1], (int8_t)data[2], wheel, data[0] & 7);
        break;
    }
    case HID_KIND_POINTER:
        pointer_report(st, data, len);
        break;
    default:
        break;
    }
}
