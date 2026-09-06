#pragma once
#include <stdint.h>

/* ATA/IDE PIO driver — primary channel (IRQ 14, base 0x1F0) */

/* Initialise — detect primary master drive */
void ata_init(void);

/*
 * Read `count` 512-byte sectors starting at LBA `lba` into `buf`.
 * count == 0 is treated as 256 sectors.
 * Returns 0 on success, -1 on error.
 */
int ata_read(uint32_t lba, uint8_t count, void *buf);

/*
 * Write `count` 512-byte sectors to LBA `lba` from `buf`.
 * Returns 0 on success, -1 on error.
 */
int ata_write(uint32_t lba, uint8_t count, const void *buf);

/* Returns 1 if a drive was detected during ata_init, 0 otherwise */
int ata_present(void);
