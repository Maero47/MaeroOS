#pragma once
#include <stdint.h>
#include "../fs/vfs.h"

/*
 * Block devices by name: every disk in drivers/blkdev.c's table (IDE "hda"..
 * "hdd", AHCI "sda".., NVMe "nvme0n1"..) and every partition found in its MBR
 * (primary 1-4, logical from 5) or GPT (entry index + 1), Linux-style
 * ("hdb1", "sda2", "nvme0n1p1").  devfs exposes them as /dev/<name>
 * (read-only raw access); mount(2) finds its source device through
 * blkpart_from_node().  A blkpart_t is a view (disk, first sector, sector
 * count) over that table.
 */
#define BLKPART_MAX 32

typedef struct blkpart {
    char       name[16];    /* "hdb1", "nvme0n1p2" */
    int        dev;         /* disk index in drivers/blkdev.c's table */
    uint32_t   start;       /* first sector on the device */
    uint32_t   nsect;       /* length in 512-byte sectors */
    uint32_t   rdev;        /* Linux dev_t (blk_disk_rdev) */
    int        partno;      /* 0 = whole disk */
    vfs_node_t node;        /* the /dev node */
} blkpart_t;

/* Scan every disk for partitions.  Call after blk_init(). */
void blkpart_init(void);

blkpart_t *blkpart_find(const char *name);
blkpart_t *blkpart_get(uint32_t idx);          /* idx-th device, NULL past the end */
/* The device behind a /dev node, or NULL when `node` is not one of ours. */
blkpart_t *blkpart_from_node(vfs_node_t *node);

/* Sector I/O relative to the start of the partition, bounds-checked against
 * its length.  `count` 1..128.  0 on success, -1 on error. */
int blkpart_read(blkpart_t *bp, uint32_t sector, uint32_t count, void *buf);
int blkpart_write(blkpart_t *bp, uint32_t sector, uint32_t count, const void *buf);

/* /proc/partitions text into buf (at most `size` bytes); returns its length. */
uint32_t blkpart_format(char *buf, uint32_t size);
