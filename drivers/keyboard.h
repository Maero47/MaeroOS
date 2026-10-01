#pragma once
#include <stdint.h>

#define EV_SYN 0x00
#define EV_KEY 0x01
#define SYN_REPORT 0

typedef struct {
    int32_t tv_sec;
    int32_t tv_usec;
    uint16_t type;
    uint16_t code;
    int32_t value;
} input_event_t;

void keyboard_init(void);
/* Whether an 8042 PS/2 controller answers at 0x60/0x64.  Many current PCs
 * have none (USB input only); its ports then read 0xFF. */
int ps2_controller_present(void);
uint32_t keyboard_read_events(uint32_t len, uint8_t *buf);
int keyboard_has_events(void);
/* Feed one key event (Linux evdev code) into /dev/input/event0. */
void keyboard_input_key(uint16_t key, int pressed);
/* Lock keys as the keyboard LEDs show them, toggled by every press of Num,
 * Caps and Scroll Lock on any keyboard: bit 0 Num, 1 Caps, 2 Scroll (the HID
 * boot LED report). */
uint8_t keyboard_leds(void);
/* Register a pid that Ctrl+Alt+Backspace will SIGKILL (fullscreen escape). */
void keyboard_set_kill_target(int pid);
