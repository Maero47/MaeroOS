#pragma once
#include <stdint.h>

void serial_init(void);
void serial_putc(char c);
char serial_getc(void);      /* block until a byte arrives on COM1 */
int  serial_data_ready(void);/* 1 if COM1 has a byte waiting, 0 otherwise */
void serial_puts(const char *s);
void serial_write(const char *s, uint32_t len);
void serial_write_hex(uint32_t val);

/* Transmit through IRQ4 from now on instead of busy-waiting on the UART
 * (interrupts and the PIC must be set up).  Output is queued and keeps its
 * order; see drivers/serial.c. */
void serial_enable_async(void);
/* Timer-tick backstop for the transmit interrupt. */
void serial_tx_poll(void);
/* Synchronous output until the matching end (panic, double fault, the NMI
 * dump): drains what is queued first.  Nests; panic never ends it. */
void serial_sync_begin(void);
void serial_sync_end(void);
