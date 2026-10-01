#pragma once
#include <stdint.h>

/*
 * NVMe (NVM Express, PCI class 01:08:02) disk driver.
 *
 * Every NVMe controller on the PCI bus is reset and enabled with an admin
 * queue pair and one I/O queue pair; each active namespace with 512-byte
 * logical blocks and no metadata becomes a numbered disk (0, 1, ...), in
 * controller then namespace-ID order.  Commands are polled for completion
 * with interrupts off, one at a time, like the AHCI driver (drivers/ahci.c).
 */

#define NVME_MAX_DISKS 8

/* Find, reset and enable the controllers and identify their namespaces.
 * Needs pci_init() and paging (the register window is mapped into kernel
 * space). */
void nvme_init(void);

/* Number of usable namespaces found by nvme_init. */
int nvme_disk_count(void);

/* Capacity of `disk` in 512-byte sectors (saturated to 32 bits), 0 if none. */
uint32_t nvme_disk_sectors(int disk);

/*
 * Read / write `count` 512-byte sectors (1..256) at `lba` on `disk`.
 * When the controller has a volatile write cache a write is followed by a
 * FLUSH and fails if the flush does.  Returns 0 on success, -1 on error.
 */
int nvme_read(int disk, uint32_t lba, uint32_t count, void *buf);
int nvme_write(int disk, uint32_t lba, uint32_t count, const void *buf);

/* Orderly shutdown (CC.SHN) of every controller, before reboot/power-off. */
void nvme_shutdown(void);
