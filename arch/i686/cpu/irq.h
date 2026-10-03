#pragma once
#include "isr.h"

void irq_install_handler(uint8_t irq, isr_handler_t handler);
void irq_remove_handler(uint8_t irq, isr_handler_t handler);

/* Message-signalled interrupts: MSI_VECTORS IDT vectors from MSI_VECTOR_BASE,
 * acknowledged at the Local APIC.  msi_install_handler() takes a free one
 * for `handler` and returns its vector, or -1 (none left, or no LAPIC). */
#define MSI_VECTOR_BASE 0xE0U
#define MSI_VECTORS     8   /* xHCI, virtio devices, ...; isr.asm and idt.c
                             * have one stub per vector */
int msi_install_handler(isr_handler_t handler);
