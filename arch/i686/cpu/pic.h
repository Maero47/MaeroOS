#pragma once
#include <stdint.h>

void pic_remap(void);
void pic_mask(uint8_t irq);
void pic_unmask(uint8_t irq);
void pic_send_eoi(uint8_t irq);
