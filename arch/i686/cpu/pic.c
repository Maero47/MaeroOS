#include "pic.h"
#include <io.h>

/* PIC I/O port addresses */
#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

/* Initialization Command Words */
#define ICW1_INIT    0x10   /* Initialization + ICW4 needed */
#define ICW1_CASCADE 0x00   /* Cascade (two-PIC) mode */
#define ICW4_8086    0x01   /* 8086/88 mode */

/* Offset vectors after remapping */
#define PIC1_OFFSET 0x20    /* IRQs  0-7  → vectors 0x20-0x27 */
#define PIC2_OFFSET 0x28    /* IRQs  8-15 → vectors 0x28-0x2F */

/* OCW3: read ISR register command */
#define PIC_OCW3_READ_ISR 0x0B

/*
 * pic_remap — reinitialize both PICs with new vector offsets.
 *
 * By default, the 8259A PICs use vectors 0x08-0x0F (master) and 0x70-0x77
 * (slave), which collide with CPU exceptions.  We remap master to 0x20 and
 * slave to 0x28 so IRQ handlers and exception handlers don't overlap.
 */
void pic_remap(void) {
    /* Save masks */
    uint8_t mask1 = inb(PIC1_DATA);
    uint8_t mask2 = inb(PIC2_DATA);

    /* ICW1: start initialization, cascade, ICW4 needed */
    outb(PIC1_CMD,  ICW1_INIT | ICW1_CASCADE | 0x01);  io_wait();
    outb(PIC2_CMD,  ICW1_INIT | ICW1_CASCADE | 0x01);  io_wait();

    /* ICW2: vector offsets */
    outb(PIC1_DATA, PIC1_OFFSET);  io_wait();
    outb(PIC2_DATA, PIC2_OFFSET);  io_wait();

    /* ICW3: cascade wiring */
    outb(PIC1_DATA, 0x04);  io_wait();   /* Master: slave on IRQ2 (bit 2) */
    outb(PIC2_DATA, 0x02);  io_wait();   /* Slave:  cascade identity = 2 */

    /* ICW4: 8086 mode */
    outb(PIC1_DATA, ICW4_8086);  io_wait();
    outb(PIC2_DATA, ICW4_8086);  io_wait();

    /* Restore masks (unmask all) */
    outb(PIC1_DATA, mask1);
    outb(PIC2_DATA, mask2);
}

void pic_mask(uint8_t irq) {
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t  bit  = (irq < 8) ? irq : (irq - 8);
    outb(port, inb(port) | (uint8_t)(1 << bit));
}

void pic_unmask(uint8_t irq) {
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t  bit  = (irq < 8) ? irq : (irq - 8);
    outb(port, inb(port) & (uint8_t)~(1 << bit));
}

void pic_send_eoi(uint8_t irq) {
    if (irq >= 8)
        outb(PIC2_CMD, 0x20);   /* Slave EOI */
    outb(PIC1_CMD, 0x20);       /* Master EOI always */
}
