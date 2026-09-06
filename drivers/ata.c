#include "ata.h"
#include "../kernel/printk.h"
#include <io.h>
#include <stdint.h>

/*
 * ATA PIO is a stateful transaction (issue command → poll DRQ → insw sectors).
 * With preemptible multithreaded processes (Firefox), two threads doing
 * concurrent file-backed mmaps hit ext2 → ata_read concurrently; if one is
 * preempted mid-transfer and another issues a command, the controller state is
 * corrupted and the first thread reads garbage → non-deterministic crashes.
 * ext2 reads are small (1 block = 2 sectors), so serialize each transaction
 * with a saved-IF cli/sti (single CPU).
 */
static inline uint32_t ata_irq_save(void) {
    uint32_t f;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(f) :: "memory");
    return f;
}
static inline void ata_irq_restore(uint32_t f) {
    if (f & 0x200) __asm__ volatile("sti" ::: "memory");
}

/* Primary channel I/O registers */
#define ATA_DATA     0x1F0   /* 16-bit data port */
#define ATA_FEAT     0x1F1   /* features / error (read) */
#define ATA_NSECT    0x1F2   /* sector count */
#define ATA_LBAL     0x1F3   /* LBA bits 0-7 */
#define ATA_LBAM     0x1F4   /* LBA bits 8-15 */
#define ATA_LBAH     0x1F5   /* LBA bits 16-23 */
#define ATA_DRIVE    0x1F6   /* drive/head, LBA bits 24-27 */
#define ATA_STATUS   0x1F7   /* status (read) / command (write) */
#define ATA_CMD      0x1F7
#define ATA_ALT      0x3F6   /* alternate status / device control */

/* Status register bits */
#define ATA_SR_BSY  0x80   /* busy */
#define ATA_SR_DRDY 0x40   /* drive ready */
#define ATA_SR_DF   0x20   /* drive fault */
#define ATA_SR_DRQ  0x08   /* data request (ready to transfer) */
#define ATA_SR_ERR  0x01   /* error */

/* Commands */
#define ATA_CMD_READ   0x20
#define ATA_CMD_WRITE  0x30
#define ATA_CMD_FLUSH  0xE7
#define ATA_CMD_IDENT  0xEC

static int drive_present = 0;

/* Read alternate status 4 times (~400 ns delay) */
static void ata_delay(void) {
    inb(ATA_ALT); inb(ATA_ALT); inb(ATA_ALT); inb(ATA_ALT);
}

/* Wait until BSY clears; return -1 on error/timeout */
static int ata_wait_bsy(void) {
    for (int i = 0; i < 0x100000; i++) {
        uint8_t s = inb(ATA_STATUS);
        if (!(s & ATA_SR_BSY))
            return 0;
    }
    return -1;  /* timeout */
}

/* Wait until DRQ is set (data ready); return -1 on error */
static int ata_wait_drq(void) {
    for (int i = 0; i < 0x100000; i++) {
        uint8_t s = inb(ATA_STATUS);
        if (s & (ATA_SR_ERR | ATA_SR_DF))
            return -1;
        if (s & ATA_SR_DRQ)
            return 0;
    }
    return -1;  /* timeout */
}

void ata_init(void) {
    /* Software reset */
    outb(ATA_ALT, 0x04);
    ata_delay();
    outb(ATA_ALT, 0x00);
    ata_delay();

    /* Select master drive */
    outb(ATA_DRIVE, 0xA0);
    ata_delay();

    uint8_t status = inb(ATA_STATUS);
    if (status == 0xFF) {
        printk("[ATA]  No drive (floating bus).\n");
        return;
    }

    /* Identify drive — just check it responds */
    ata_wait_bsy();
    outb(ATA_DRIVE, 0xA0);
    outb(ATA_NSECT, 0);
    outb(ATA_LBAL,  0);
    outb(ATA_LBAM,  0);
    outb(ATA_LBAH,  0);
    outb(ATA_CMD,   ATA_CMD_IDENT);
    ata_delay();

    status = inb(ATA_STATUS);
    if (status == 0) {
        printk("[ATA]  Drive not present.\n");
        return;
    }

    /* Wait for IDENTIFY data (or error if ATAPI) */
    if (inb(ATA_LBAM) || inb(ATA_LBAH)) {
        printk("[ATA]  Non-ATA device detected — skipping.\n");
        return;
    }

    ata_wait_bsy();

    /* Drain the 256-word identify data */
    for (int i = 0; i < 256; i++)
        inw(ATA_DATA);

    drive_present = 1;
    printk("[ATA]  Primary master ready.\n");
}

int ata_present(void) {
    return drive_present;
}

int ata_read(uint32_t lba, uint8_t count, void *buf) {
    if (!drive_present) return -1;

    uint32_t irq = ata_irq_save();   /* serialize the whole PIO transaction */
    ata_wait_bsy();

    outb(ATA_DRIVE, 0xE0 | ((lba >> 24) & 0x0F));  /* LBA mode, master */
    outb(ATA_FEAT,  0x00);
    outb(ATA_NSECT, count);
    outb(ATA_LBAL,  (uint8_t)(lba));
    outb(ATA_LBAM,  (uint8_t)(lba >> 8));
    outb(ATA_LBAH,  (uint8_t)(lba >> 16));
    outb(ATA_CMD,   ATA_CMD_READ);
    ata_delay();

    int nsect = (count == 0) ? 256 : (int)count;
    uint16_t *ptr = (uint16_t *)buf;

    for (int s = 0; s < nsect; s++) {
        if (ata_wait_drq() < 0) { ata_irq_restore(irq); return -1; }
        insw(ATA_DATA, ptr, 256);  /* 256 words = 512 bytes */
        ptr += 256;
        ata_delay();
    }
    ata_irq_restore(irq);
    return 0;
}

int ata_write(uint32_t lba, uint8_t count, const void *buf) {
    if (!drive_present) return -1;

    uint32_t irq = ata_irq_save();   /* serialize the whole PIO transaction */
    ata_wait_bsy();

    outb(ATA_DRIVE, 0xE0 | ((lba >> 24) & 0x0F));
    outb(ATA_FEAT,  0x00);
    outb(ATA_NSECT, count);
    outb(ATA_LBAL,  (uint8_t)(lba));
    outb(ATA_LBAM,  (uint8_t)(lba >> 8));
    outb(ATA_LBAH,  (uint8_t)(lba >> 16));
    outb(ATA_CMD,   ATA_CMD_WRITE);
    ata_delay();

    int nsect = (count == 0) ? 256 : (int)count;
    const uint16_t *ptr = (const uint16_t *)buf;

    for (int s = 0; s < nsect; s++) {
        if (ata_wait_drq() < 0) { ata_irq_restore(irq); return -1; }
        outsw(ATA_DATA, ptr, 256);
        ptr += 256;
        ata_delay();
    }

    /* Flush write cache */
    outb(ATA_CMD, ATA_CMD_FLUSH);
    ata_wait_bsy();
    ata_irq_restore(irq);

    return 0;
}
