#pragma once
#include <stdint.h>

void mouse_init(void);
uint32_t mouse_read_events(uint32_t len, uint8_t *buf);
int mouse_has_events(void);
int mouse_present(void);
/* Feed one pointer report into /dev/input/event1 (see mouse.c). */
void mouse_input(int32_t dx, int32_t dy, int32_t wheel, uint8_t buttons);

