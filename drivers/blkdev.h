#pragma once
#include <stdint.h>

/*
 * Every disk the kernel can address, in one numbered table: the four IDE
 * positions that answered ("hda".."hdd", drivers/ata.c), then each AHCI disk
 * ("sda", "sdb", ..., drivers/ahci.c), then each NVMe namespace ("nvme0n1",
 * ..., drivers/nvme.c), then USB mass-storage disks as they are plugged in
 * (drivers/usb/usb_msc.c), named after the AHCI disks ("sdb" next to one
 * AHCI disk).  drivers/blkpart.c finds partitions on all of them.
 *
 * A USB disk keeps its slot (and name) when unplugged: I/O on it fails until
 * it is plugged in again.
 *
 * One of them is the boot disk ext2 mounts at /disk: the IDE primary master
 * when there is one, else the first AHCI disk, else the first NVMe namespace.
 * blk_read/blk_write address it with the ata_read/ata_write convention:
 * `count` sectors of 512 bytes, 0 = 256.
 */

#define BLK_MAX_DISKS 20

/* Build the table and pick the boot disk.  Call after ata_init(),
 * ahci_init() and nvme_init(). */
void blk_init(void);

/* Hot-plug (USB mass storage, unit 0..): add the disk, or bring its old
 * slot back; returns its index, -1 when the table is full.  Detach makes
 * every access to it fail (the slot stays, so mounted filesystems holding
 * the index see errors, never another disk). */
int  blk_usb_attach(int unit);
void blk_usb_detach(int disk);

/* 1 if there is a disk to mount. */
int blk_present(void);

/* "ata", "ahci0".."ahci7" or "nvme0".."nvme7", for messages. */
const char *blk_name(void);

/* The boot disk's /dev path ("/dev/hda", "/dev/sda", "/dev/nvme0n1"). */
const char *blk_boot_devpath(void);

/* Make `disk` the boot disk (root= on the command line names it). */
void blk_set_boot(int disk);
/* A disk is busy while ext2 has it mounted at /disk: /dev writes to it are
 * refused. */
void blk_set_busy(int disk);
int  blk_disk_busy(int disk);

int blk_read(uint32_t lba, uint8_t count, void *buf);
int blk_write(uint32_t lba, uint8_t count, const void *buf);

/* The table.  `disk` is 0..blk_disk_count()-1; `count` 1..256 sectors.
 * Read/write return 0 on success, -1 on error. */
int         blk_disk_count(void);
const char *blk_disk_devname(int disk);     /* "hda", "sdb", "nvme0n1" */
uint32_t    blk_disk_sectors(int disk);     /* saturated to 32 bits */
/* The real size: for IDE the LBA48 count, which can exceed what
 * blk_disk_sectors() addresses; otherwise blk_disk_sectors(). */
uint64_t    blk_disk_capacity(int disk);
int         blk_disk_is_boot(int disk);
/* Linux dev_t of the whole disk and of its partition `partno` (1..15; 0 is
 * the disk), as major << 8 | minor: IDE 3/22 (slave minors from 64), SCSI
 * disks 8 (16 minors per disk; USB ones after the AHCI ones), NVMe 259. */
uint32_t    blk_disk_rdev(int disk, int partno);
int         blk_disk_read(int disk, uint32_t lba, uint32_t count, void *buf);
int         blk_disk_write(int disk, uint32_t lba, uint32_t count, const void *buf);
