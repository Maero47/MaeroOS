#include "blkdev.h"
#include "ata.h"
#include "ahci.h"
#include "../kernel/printk.h"

static enum { BLK_NONE, BLK_ATA, BLK_AHCI } blk_kind;
static int  blk_unit;
static char blk_label[8];

void blk_init(void) {
    if (ata_present()) {
        blk_kind = BLK_ATA;
        blk_label[0] = 'a'; blk_label[1] = 't'; blk_label[2] = 'a'; blk_label[3] = 0;
    } else if (ahci_disk_count() > 0) {
        blk_kind = BLK_AHCI;
        blk_unit = 0;
        blk_label[0] = 'a'; blk_label[1] = 'h'; blk_label[2] = 'c'; blk_label[3] = 'i';
        blk_label[4] = (char)('0' + blk_unit); blk_label[5] = 0;
    } else {
        blk_kind = BLK_NONE;
        return;
    }
    printk("[BLK]  boot disk: %s\n", blk_label);
}

int blk_present(void) {
    return blk_kind != BLK_NONE;
}

const char *blk_name(void) {
    return blk_kind == BLK_NONE ? "none" : blk_label;
}

int blk_read(uint32_t lba, uint8_t count, void *buf) {
    if (blk_kind == BLK_ATA)
        return ata_read(lba, count, buf);
    if (blk_kind == BLK_AHCI)
        return ahci_read(blk_unit, lba, count ? count : 256u, buf);
    return -1;
}

int blk_write(uint32_t lba, uint8_t count, const void *buf) {
    if (blk_kind == BLK_ATA)
        return ata_write(lba, count, buf);
    if (blk_kind == BLK_AHCI)
        return ahci_write(blk_unit, lba, count ? count : 256u, buf);
    return -1;
}
