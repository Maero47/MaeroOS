#pragma once
#include <stdint.h>

void idt_init(void);
void idt_load(void);   /* AP: load the shared IDT on this CPU */
void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags);
