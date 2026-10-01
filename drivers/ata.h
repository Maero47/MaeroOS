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

/* ── All four IDE positions ──────────────────────────────────────────────────
 * Device index: 0 hda (primary master, the boot disk the calls above serve),
 * 1 hdb (primary slave), 2 hdc (secondary master), 3 hdd (secondary slave).
 * The others are probed by ata_init() too and use plain LBA28 PIO. */
#define ATA_MAX_DEVS 4
int      ata_dev_present(int dev);
uint32_t ata_dev_sectors(int dev);           /* capacity in 512-byte sectors */
/* The drive's real size in sectors, LBA48 included; above ata_dev_sectors()
 * when the drive is bigger than LBA28 (128 GiB) reaches. */
uint64_t ata_dev_capacity(int dev);
int      ata_dev_read(int dev, uint32_t lba, uint8_t count, void *buf);
int      ata_dev_write(int dev, uint32_t lba, uint8_t count, const void *buf);
