#pragma once
#include <stdint.h>
#include "../fs/vfs.h"

/*
 * Block devices by name: every ATA disk ("hda".."hdd") and every partition
 * found in its MBR (primary 1-4, logical from 5) or GPT (entry index + 1),
 * Linux-style ("hdb1").  devfs exposes them as /dev/<name> (read-only raw
 * access); mount(2) finds its source device through blkpart_from_node().
 *
 * Kept deliberately small: when a general block layer arrives, a blkpart_t
 * becomes a thin view (device, first sector, sector count) over it and only
 * blkpart_read()/blkpart_write() change.
 */
#define BLKPART_MAX 32

typedef struct blkpart {
    char       name[8];     /* "hdb1" */
    int        dev;         /* ATA device index (drivers/ata.h) */
    uint32_t   start;       /* first sector on the device */
    uint32_t   nsect;       /* length in 512-byte sectors */
    uint32_t   rdev;        /* Linux dev_t: major 3 (hda/hdb) or 22, minor */
    int        partno;      /* 0 = whole disk */
    vfs_node_t node;        /* the /dev node */
} blkpart_t;

/* Scan every present ATA disk for partitions.  Call after ata_init(). */
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
