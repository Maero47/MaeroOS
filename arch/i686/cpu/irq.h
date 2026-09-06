#pragma once
#include "isr.h"

void irq_install_handler(uint8_t irq, isr_handler_t handler);
void irq_remove_handler(uint8_t irq);
