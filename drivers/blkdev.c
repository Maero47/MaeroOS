#include "blkdev.h"
#include "ata.h"
#include "ahci.h"
#include "nvme.h"
#include "usb/usb.h"
#include "../lib/printf.h"
#include "../kernel/printk.h"

typedef enum { BLK_NONE, BLK_ATA, BLK_AHCI, BLK_NVME, BLK_USB } blk_kind_t;

typedef struct {
    blk_kind_t kind;
    int        unit;        /* ATA position, AHCI disk, NVMe namespace or USB disk number */
    int        gone;        /* hot-plugged disk that was unplugged */
    char       name[12];
} blk_disk_t;

static blk_disk_t disks[BLK_MAX_DISKS];
static int        ndisks;
static int        boot = -1;          /* index into disks[], -1 = none */
static char       boot_label[8];

static void add_disk(blk_kind_t kind, int unit) {
    if (ndisks >= BLK_MAX_DISKS) return;
    blk_disk_t *d = &disks[ndisks++];
    d->kind = kind;
    d->unit = unit;
    if (kind == BLK_ATA)
        snprintf(d->name, sizeof(d->name), "hd%c", 'a' + unit);
    else if (kind == BLK_AHCI)
        snprintf(d->name, sizeof(d->name), "sd%c", 'a' + unit);
    else if (kind == BLK_USB)       /* SCSI disks share the sdX names */
        snprintf(d->name, sizeof(d->name), "sd%c", 'a' + ahci_disk_count() + unit);
    else
        snprintf(d->name, sizeof(d->name), "nvme0n%d", unit + 1);
}

void blk_init(void) {
    for (int i = 0; i < ATA_MAX_DEVS; i++)
        if (ata_dev_present(i)) add_disk(BLK_ATA, i);
    for (int i = 0; i < ahci_disk_count(); i++) add_disk(BLK_AHCI, i);
    for (int i = 0; i < nvme_disk_count(); i++) add_disk(BLK_NVME, i);

    /* The boot disk: the IDE primary master, else the first AHCI disk, else
     * the first NVMe namespace (unit 0 of its kind in each case). */
    static const blk_kind_t order[] = { BLK_ATA, BLK_AHCI, BLK_NVME };
    for (unsigned k = 0; k < 3 && boot < 0; k++)
        for (int i = 0; i < ndisks; i++)
            if (disks[i].kind == order[k] && disks[i].unit == 0) { boot = i; break; }
    if (boot < 0) return;
    if (disks[boot].kind == BLK_ATA)
        snprintf(boot_label, sizeof(boot_label), "ata");
    else
        snprintf(boot_label, sizeof(boot_label), "%s%d",
                 disks[boot].kind == BLK_AHCI ? "ahci" : "nvme", disks[boot].unit);
    printk("[BLK]  boot disk: %s\n", boot_label);
}

int blk_usb_attach(int unit) {
    for (int i = 0; i < ndisks; i++)
        if (disks[i].kind == BLK_USB && disks[i].unit == unit) {
            disks[i].gone = 0;
            return i;
        }
    if (ndisks >= BLK_MAX_DISKS) return -1;
    add_disk(BLK_USB, unit);
    return ndisks - 1;
}

void blk_usb_detach(int disk) {
    if (disk >= 0 && disk < ndisks && disks[disk].kind == BLK_USB)
        disks[disk].gone = 1;
}

int blk_present(void) {
    return boot >= 0;
}

const char *blk_name(void) {
    return boot < 0 ? "none" : boot_label;
}

const char *blk_boot_devpath(void) {
    static char path[20];
    if (boot < 0) return "none";
    snprintf(path, sizeof(path), "/dev/%s", disks[boot].name);
    return path;
}

int blk_read(uint32_t lba, uint8_t count, void *buf) {
    if (boot < 0) return -1;
    if (disks[boot].kind == BLK_ATA)
        return ata_read(lba, count, buf);        /* the master's fast path */
    return blk_disk_read(boot, lba, count ? count : 256u, buf);
}

int blk_write(uint32_t lba, uint8_t count, const void *buf) {
    if (boot < 0) return -1;
    if (disks[boot].kind == BLK_ATA)
        return ata_write(lba, count, buf);
    return blk_disk_write(boot, lba, count ? count : 256u, buf);
}

int blk_disk_count(void) {
    return ndisks;
}

const char *blk_disk_devname(int disk) {
    return disk >= 0 && disk < ndisks ? disks[disk].name : "";
}

int blk_disk_is_boot(int disk) {
    return disk >= 0 && disk == boot;
}

uint32_t blk_disk_sectors(int disk) {
    if (disk < 0 || disk >= ndisks) return 0;
    const blk_disk_t *d = &disks[disk];
    if (d->gone)             return 0;
    if (d->kind == BLK_ATA)  return ata_dev_sectors(d->unit);
    if (d->kind == BLK_AHCI) return ahci_disk_sectors(d->unit);
    if (d->kind == BLK_USB)  return usb_msc_sectors();
    return nvme_disk_sectors(d->unit);
}

uint32_t blk_disk_rdev(int disk, int partno) {
    if (disk < 0 || disk >= ndisks) return 0;
    const blk_disk_t *d = &disks[disk];
    if (d->kind == BLK_ATA)
        return ((d->unit < 2 ? 3u : 22u) << 8) | ((d->unit & 1) ? 64u : 0u) |
               (uint32_t)partno;
    uint32_t n = (uint32_t)d->unit;
    if (d->kind == BLK_USB) n += (uint32_t)ahci_disk_count();
    uint32_t minor = (n * 16u + (uint32_t)partno) & 0xFFu;
    return ((d->kind == BLK_NVME ? 259u : 8u) << 8) | minor;
}

int blk_disk_read(int disk, uint32_t lba, uint32_t count, void *buf) {
    if (disk < 0 || disk >= ndisks || !count || count > 256) return -1;
    const blk_disk_t *d = &disks[disk];
    if (d->gone) return -1;
    if (d->kind == BLK_ATA)
        return ata_dev_read(d->unit, lba, (uint8_t)count, buf);   /* 256 -> 0 */
    if (d->kind == BLK_AHCI) return ahci_read(d->unit, lba, count, buf);
    if (d->kind == BLK_USB)  return usb_msc_read(lba, count, buf);
    return nvme_read(d->unit, lba, count, buf);
}

int blk_disk_write(int disk, uint32_t lba, uint32_t count, const void *buf) {
    if (disk < 0 || disk >= ndisks || !count || count > 256) return -1;
    const blk_disk_t *d = &disks[disk];
    if (d->gone) return -1;
    if (d->kind == BLK_ATA)
        return ata_dev_write(d->unit, lba, (uint8_t)count, buf);
    if (d->kind == BLK_AHCI) return ahci_write(d->unit, lba, count, buf);
    if (d->kind == BLK_USB)  return usb_msc_write(lba, count, buf);
    return nvme_write(d->unit, lba, count, buf);
}
