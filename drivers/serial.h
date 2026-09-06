#pragma once
#include <stdint.h>

void serial_init(void);
void serial_putc(char c);
char serial_getc(void);      /* block until a byte arrives on COM1 */
int  serial_data_ready(void);/* 1 if COM1 has a byte waiting, 0 otherwise */
void serial_puts(const char *s);
void serial_write_hex(uint32_t val);
