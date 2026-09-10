#include "keyboard.h"
#include "../arch/i686/cpu/irq.h"
#include "../arch/i686/cpu/pic.h"
#include "../arch/i686/cpu/pit.h"
#include "../arch/i686/include/io.h"
#include "../kernel/printk.h"
#include "../proc/scheduler.h"
#include "../proc/process.h"
#include "../proc/signal.h"
#include <registers.h>
#include <stdint.h>

#define KBD_DATA_PORT   0x60
#define KBD_STATUS_PORT 0x64
#define KBD_STATUS_OUT  0x01
#define KBD_RING_SIZE   128

/* Linux input-event key codes for common Set 1 scancodes. */
#define KEY_ESC        1
#define KEY_1          2
#define KEY_2          3
#define KEY_3          4
#define KEY_4          5
#define KEY_5          6
#define KEY_6          7
#define KEY_7          8
#define KEY_8          9
#define KEY_9          10
#define KEY_0          11
#define KEY_MINUS      12
#define KEY_EQUAL      13
#define KEY_BACKSPACE  14
#define KEY_TAB        15
#define KEY_Q          16
#define KEY_W          17
#define KEY_E          18
#define KEY_R          19
#define KEY_T          20
#define KEY_Y          21
#define KEY_U          22
#define KEY_I          23
#define KEY_O          24
#define KEY_P          25
#define KEY_LEFTBRACE  26
#define KEY_RIGHTBRACE 27
#define KEY_ENTER      28
#define KEY_LEFTCTRL   29
#define KEY_A          30
#define KEY_S          31
#define KEY_D          32
#define KEY_F          33
#define KEY_G          34
#define KEY_H          35
#define KEY_J          36
#define KEY_K          37
#define KEY_L          38
#define KEY_SEMICOLON  39
#define KEY_APOSTROPHE 40
#define KEY_GRAVE      41
#define KEY_LEFTSHIFT  42
#define KEY_BACKSLASH  43
#define KEY_Z          44
#define KEY_X          45
#define KEY_C          46
#define KEY_V          47
#define KEY_B          48
#define KEY_N          49
#define KEY_M          50
#define KEY_COMMA      51
#define KEY_DOT        52
#define KEY_SLASH      53
#define KEY_RIGHTSHIFT 54
#define KEY_KPASTERISK 55
#define KEY_LEFTALT    56
#define KEY_SPACE      57
#define KEY_CAPSLOCK   58
#define KEY_F1         59
#define KEY_F2         60
#define KEY_F3         61
#define KEY_F4         62
#define KEY_F5         63
#define KEY_F6         64
#define KEY_F7         65
#define KEY_F8         66
#define KEY_F9         67
#define KEY_F10        68
#define KEY_NUMLOCK    69
#define KEY_SCROLLLOCK 70
#define KEY_KP7        71
#define KEY_KP8        72
#define KEY_KP9        73
#define KEY_KPMINUS    74
#define KEY_KP4        75
#define KEY_KP5        76
#define KEY_KP6        77
#define KEY_KPPLUS     78
#define KEY_KP1        79
#define KEY_KP2        80
#define KEY_KP3        81
#define KEY_KP0        82
#define KEY_KPDOT      83

static input_event_t ring[KBD_RING_SIZE];
static volatile uint32_t head;
static volatile uint32_t tail;
static uint8_t got_e0;

static const uint16_t set1_keys[128] = {
    [0x01] = KEY_ESC,        [0x02] = KEY_1,          [0x03] = KEY_2,
    [0x04] = KEY_3,          [0x05] = KEY_4,          [0x06] = KEY_5,
    [0x07] = KEY_6,          [0x08] = KEY_7,          [0x09] = KEY_8,
    [0x0a] = KEY_9,          [0x0b] = KEY_0,          [0x0c] = KEY_MINUS,
    [0x0d] = KEY_EQUAL,      [0x0e] = KEY_BACKSPACE,  [0x0f] = KEY_TAB,
    [0x10] = KEY_Q,          [0x11] = KEY_W,          [0x12] = KEY_E,
    [0x13] = KEY_R,          [0x14] = KEY_T,          [0x15] = KEY_Y,
    [0x16] = KEY_U,          [0x17] = KEY_I,          [0x18] = KEY_O,
    [0x19] = KEY_P,          [0x1a] = KEY_LEFTBRACE,  [0x1b] = KEY_RIGHTBRACE,
    [0x1c] = KEY_ENTER,      [0x1d] = KEY_LEFTCTRL,   [0x1e] = KEY_A,
    [0x1f] = KEY_S,          [0x20] = KEY_D,          [0x21] = KEY_F,
    [0x22] = KEY_G,          [0x23] = KEY_H,          [0x24] = KEY_J,
    [0x25] = KEY_K,          [0x26] = KEY_L,          [0x27] = KEY_SEMICOLON,
    [0x28] = KEY_APOSTROPHE, [0x29] = KEY_GRAVE,      [0x2a] = KEY_LEFTSHIFT,
    [0x2b] = KEY_BACKSLASH,  [0x2c] = KEY_Z,          [0x2d] = KEY_X,
    [0x2e] = KEY_C,          [0x2f] = KEY_V,          [0x30] = KEY_B,
    [0x31] = KEY_N,          [0x32] = KEY_M,          [0x33] = KEY_COMMA,
    [0x34] = KEY_DOT,        [0x35] = KEY_SLASH,      [0x36] = KEY_RIGHTSHIFT,
    [0x37] = KEY_KPASTERISK, [0x38] = KEY_LEFTALT,    [0x39] = KEY_SPACE,
    [0x3a] = KEY_CAPSLOCK,   [0x3b] = KEY_F1,         [0x3c] = KEY_F2,
    [0x3d] = KEY_F3,         [0x3e] = KEY_F4,         [0x3f] = KEY_F5,
    [0x40] = KEY_F6,         [0x41] = KEY_F7,         [0x42] = KEY_F8,
    [0x43] = KEY_F9,         [0x44] = KEY_F10,        [0x45] = KEY_NUMLOCK,
    [0x46] = KEY_SCROLLLOCK, [0x47] = KEY_KP7,        [0x48] = KEY_KP8,
    [0x49] = KEY_KP9,        [0x4a] = KEY_KPMINUS,    [0x4b] = KEY_KP4,
    [0x4c] = KEY_KP5,        [0x4d] = KEY_KP6,        [0x4e] = KEY_KPPLUS,
    [0x4f] = KEY_KP1,        [0x50] = KEY_KP2,        [0x51] = KEY_KP3,
    [0x52] = KEY_KP0,        [0x53] = KEY_KPDOT,
};

/* E0-prefixed scancodes: navigation cluster, the right-hand modifiers and the
 * Windows-key cluster (Linux evdev key codes).
 *
 * The right-hand modifiers matter more than they look.  A key this table does
 * not name is dropped outright by keyboard_irq(), so while right Ctrl and right
 * Alt were missing here they did not exist as far as the rest of the system was
 * concerned: userspace tracked KEY_RIGHTCTRL/KEY_RIGHTALT and maeroX's
 * GetModifierMapping advertised their keycodes, but no press could ever arrive
 * to set the modifier, so every right-Alt (AltGr) combination silently did
 * nothing. */
static const uint16_t e0_keys[128] = {
    [0x48] = 103,  /* KEY_UP    */
    [0x50] = 108,  /* KEY_DOWN  */
    [0x4b] = 105,  /* KEY_LEFT  */
    [0x4d] = 106,  /* KEY_RIGHT */
    [0x47] = 102,  /* KEY_HOME  */
    [0x4f] = 107,  /* KEY_END   */
    [0x49] = 104,  /* KEY_PAGEUP   */
    [0x51] = 109,  /* KEY_PAGEDOWN */
    [0x52] = 110,  /* KEY_INSERT   */
    [0x53] = 111,  /* KEY_DELETE   */
    [0x1c] = 28,   /* keypad Enter → KEY_ENTER */
    [0x1d] = 97,   /* KEY_RIGHTCTRL */
    [0x35] = 98,   /* KEY_KPSLASH   */
    [0x38] = 100,  /* KEY_RIGHTALT  */
    [0x5b] = 125,  /* KEY_LEFTMETA  */
    [0x5c] = 126,  /* KEY_RIGHTMETA */
    [0x5d] = 127,  /* KEY_COMPOSE (menu) */
};

/* Emergency "kill foreground fullscreen app" hotkey (Ctrl+Alt+Backspace).
 * The desktop registers the pid of a fullscreen app it can't otherwise reach
 * (DOOM owns the keyboard); this lets the user always escape.  -1 = none. */
volatile int kbd_kill_target_pid = -1;

void keyboard_set_kill_target(int pid) {
    kbd_kill_target_pid = pid;
}

static int kbd_ctrl_down, kbd_alt_down;

static void push_event(uint16_t type, uint16_t code, int32_t value) {
    uint32_t next = (head + 1) % KBD_RING_SIZE;
    if (next == tail) {
        tail = (tail + 1) % KBD_RING_SIZE;
    }

    uint32_t ticks = pit_ticks();
    ring[head].tv_sec = (int32_t)(ticks / 100);
    ring[head].tv_usec = (int32_t)((ticks % 100) * 10000);
    ring[head].type = type;
    ring[head].code = code;
    ring[head].value = value;
    head = next;
    io_wake();
}

static void keyboard_irq(registers_t *regs) {
    (void)regs;
    while (inb(KBD_STATUS_PORT) & KBD_STATUS_OUT) {
        uint8_t sc = inb(KBD_DATA_PORT);
        if (sc == 0xe0) {
            got_e0 = 1;
            continue;
        }

        uint8_t release = sc & 0x80;
        uint8_t base = sc & 0x7f;
        uint16_t key = got_e0 ? e0_keys[base] : set1_keys[base];
        got_e0 = 0;
        if (!key) continue;

        /* Track Ctrl/Alt for the emergency kill hotkey. */
        if (key == 29 /*KEY_LEFTCTRL*/ || key == 97 /*KEY_RIGHTCTRL*/)
            kbd_ctrl_down = !release;
        else if (key == 56 /*KEY_LEFTALT*/ || key == 100 /*KEY_RIGHTALT*/)
            kbd_alt_down = !release;
        else if (!release && key == 14 /*KEY_BACKSPACE*/ &&
                 kbd_ctrl_down && kbd_alt_down && kbd_kill_target_pid > 0) {
            /* Ctrl+Alt+Backspace: SIGKILL the registered fullscreen app.
             * Works no matter who is reading /dev/input/event0 (DOOM). */
            for (int i = 0; i < MAX_PROCS; i++) {
                if (ptable[i].state != PROC_UNUSED &&
                    ptable[i].pid == kbd_kill_target_pid) {
                    signal_send(&ptable[i], SIGKILL);
                    break;
                }
            }
            kbd_kill_target_pid = -1;
            /* Don't deliver the Backspace itself. */
            continue;
        }

        push_event(EV_KEY, key, release ? 0 : 1);
        push_event(EV_SYN, SYN_REPORT, 0);
    }
}

void keyboard_init(void) {
    head = tail = 0;
    got_e0 = 0;

    while (inb(KBD_STATUS_PORT) & KBD_STATUS_OUT)
        (void)inb(KBD_DATA_PORT);

    irq_install_handler(1, keyboard_irq);
    pic_unmask(1);
    printk("[KBD]  PS/2 keyboard driver active on IRQ1\n");
}

int keyboard_has_events(void) {
    return head != tail;
}

uint32_t keyboard_read_events(uint32_t len, uint8_t *buf) {
    uint32_t event_size = sizeof(input_event_t);
    uint32_t copied = 0;

    while (len >= event_size && tail != head) {
        input_event_t ev = ring[tail];
        tail = (tail + 1) % KBD_RING_SIZE;
        __builtin_memcpy(buf + copied, &ev, event_size);
        copied += event_size;
        len -= event_size;
    }

    return copied;
}
