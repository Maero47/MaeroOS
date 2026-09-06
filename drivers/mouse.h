#pragma once
#include <stdint.h>

void mouse_init(void);
uint32_t mouse_read_events(uint32_t len, uint8_t *buf);
int mouse_has_events(void);

