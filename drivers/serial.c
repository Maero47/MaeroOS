#include "serial.h"
#include <io.h>

#define COM1 0x3F8

/* COM1 register offsets */
#define UART_DATA   0   /* Data register (DLAB=0) / Divisor LSB (DLAB=1) */
#define UART_IER    1   /* Interrupt enable (DLAB=0) / Divisor MSB (DLAB=1) */
#define UART_FCR    2   /* FIFO control (write) */
#define UART_LCR    3   /* Line control */
#define UART_MCR    4   /* Modem control */
#define UART_LSR    5   /* Line status */

#define UART_LCR_DLAB   0x80    /* Divisor latch access bit */
#define UART_LCR_8N1    0x03    /* 8 data bits, no parity, 1 stop bit */
#define UART_LSR_THRE   0x20    /* Transmit-hold-register empty */
#define UART_LSR_DR     0x01    /* Data ready (receive buffer full) */

void serial_init(void) {
    outb(COM1 + UART_IER, 0x00);   /* Disable interrupts */
    outb(COM1 + UART_LCR, UART_LCR_DLAB); /* Enable DLAB to set baud rate */
    outb(COM1 + UART_DATA, 0x01);  /* Divisor low  byte: 1 → 115200 baud */
    outb(COM1 + UART_IER,  0x00);  /* Divisor high byte: 0 */
    outb(COM1 + UART_LCR, UART_LCR_8N1); /* 8N1, clear DLAB */
    outb(COM1 + UART_FCR, 0xC7);   /* Enable FIFO, clear, 14-byte threshold */
    outb(COM1 + UART_MCR, 0x0B);   /* RTS + DSR set, IRQs enabled */
}

void serial_putc(char c) {
    /* Spin until transmit-hold register is empty (THR empty = LSR bit 5) */
    while (!(inb(COM1 + UART_LSR) & UART_LSR_THRE))
        ;
    outb(COM1 + UART_DATA, (uint8_t)c);
}

char serial_getc(void) {
    while (!(inb(COM1 + UART_LSR) & UART_LSR_DR))
        ;
    return (char)inb(COM1 + UART_DATA);
}

int serial_data_ready(void) {
    return (inb(COM1 + UART_LSR) & UART_LSR_DR) != 0;
}

void serial_puts(const char *s) {
    while (*s)
        serial_putc(*s++);
}

/* Print a 32-bit value as 8 hex digits (no prefix) */
void serial_write_hex(uint32_t val) {
    static const char hex[] = "0123456789ABCDEF";
    for (int i = 28; i >= 0; i -= 4)
        serial_putc(hex[(val >> i) & 0xF]);
}
