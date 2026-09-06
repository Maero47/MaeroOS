#pragma once
#include <stdint.h>

/* Write a byte to an I/O port */
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" :: "a"(val), "Nd"(port) : "memory");
}

/* Read a byte from an I/O port */
static inline uint8_t inb(uint16_t port) {
    uint8_t val;
    __asm__ volatile("inb %1, %0" : "=a"(val) : "Nd"(port) : "memory");
    return val;
}

/* Write a word (16-bit) to an I/O port */
static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile("outw %0, %1" :: "a"(val), "Nd"(port) : "memory");
}

/* Read a word (16-bit) from an I/O port */
static inline uint16_t inw(uint16_t port) {
    uint16_t val;
    __asm__ volatile("inw %1, %0" : "=a"(val) : "Nd"(port) : "memory");
    return val;
}

/* Write a dword (32-bit) to an I/O port */
static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile("outl %0, %1" :: "a"(val), "Nd"(port) : "memory");
}

/* Read a dword (32-bit) from an I/O port */
static inline uint32_t inl(uint16_t port) {
    uint32_t val;
    __asm__ volatile("inl %1, %0" : "=a"(val) : "Nd"(port) : "memory");
    return val;
}

/* Read 16-bit words from port into buffer (for ATA) */
static inline void insw(uint16_t port, void *buf, uint32_t count) {
    __asm__ volatile("rep insw" : "+D"(buf), "+c"(count) : "d"(port) : "memory");
}

/* Write 16-bit words from buffer to port (for ATA) */
static inline void outsw(uint16_t port, const void *buf, uint32_t count) {
    __asm__ volatile("rep outsw" : "+S"(buf), "+c"(count) : "d"(port) : "memory");
}

/*
 * ~1 µs delay by writing to the POST diagnostic port (0x80).
 * Required after PIC/serial/other chip commands that need settling time.
 */
static inline void io_wait(void) {
    outb(0x80, 0);
}
