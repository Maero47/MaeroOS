#pragma once
#include <stdint.h>

/*
 * The disk ext2 mounts at /disk: the IDE primary master when there is one
 * (drivers/ata.c), else the first AHCI disk (drivers/ahci.c), else the first
 * NVMe namespace (drivers/nvme.c).  Same calling
 * convention as ata_read/ata_write: `count` sectors of 512 bytes, 0 = 256.
 */

/* Pick the boot disk.  Call after ata_init(), ahci_init() and nvme_init(). */
void blk_init(void);

/* 1 if there is a disk to mount. */
int blk_present(void);

/* "ata", "ahci0".."ahci7" or "nvme0".."nvme7", for messages. */
const char *blk_name(void);

int blk_read(uint32_t lba, uint8_t count, void *buf);
int blk_write(uint32_t lba, uint8_t count, const void *buf);
