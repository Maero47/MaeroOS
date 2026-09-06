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
uint32_t keyboard_read_events(uint32_t len, uint8_t *buf);
int keyboard_has_events(void);
/* Register a pid that Ctrl+Alt+Backspace will SIGKILL (fullscreen escape). */
void keyboard_set_kill_target(int pid);
