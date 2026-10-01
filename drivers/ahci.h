#pragma once
#include <stdint.h>

/*
 * AHCI (Serial ATA, PCI class 01:06:01) disk driver.
 *
 * Every AHCI controller on the PCI bus is reset and each implemented port
 * with an ATA disk behind it becomes a numbered disk (0, 1, ...).  Transfers
 * are READ/WRITE DMA EXT through command slot 0, polled with interrupts off,
 * like the IDE driver (drivers/ata.c).  Disks with a logical sector size
 * other than 512 bytes and packet (ATAPI) devices are reported and skipped.
 */

#define AHCI_MAX_DISKS 8

/* Find and reset the controllers and identify their disks.  Needs pci_init()
 * and paging (the register window is mapped into kernel space). */
void ahci_init(void);

/* Number of usable disks found by ahci_init. */
int ahci_disk_count(void);

/* Capacity of `disk` in 512-byte sectors (saturated to 32 bits), 0 if none. */
uint32_t ahci_disk_sectors(int disk);

/*
 * Read / write `count` 512-byte sectors (1..256) at `lba` on `disk`.
 * A write is followed by FLUSH CACHE EXT and fails if the flush does.
 * Returns 0 on success, -1 on error.
 */
int ahci_read(int disk, uint32_t lba, uint32_t count, void *buf);
int ahci_write(int disk, uint32_t lba, uint32_t count, const void *buf);
