#pragma once
#include <stdint.h>

/* AC'97 PCM-out driver (QEMU -device AC97).  Fixed 48 kHz S16LE stereo. */

void ac97_init(void);            /* probe PCI; safe to call when absent */
void ac97_start_thread(void);    /* call after proc_init (spawns ksoundd) */
int  ac97_present(void);
int  ac97_write(const uint8_t *data, uint32_t len);   /* blocking */
uint32_t ac97_irq_count(void);   /* diagnostics */
