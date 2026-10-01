#pragma once
#include "vfs.h"
#include <stdint.h>

/*
 * Mount the ext2 filesystem at `lba_offset` on the boot disk (drivers/blkdev.c),
 * in a device or partition of `nsect` sectors.  A superblock claiming more
 * blocks than that is refused: block numbers would run past the partition (or
 * wrap in an LBA28 command) onto other data.
 * Returns a VFS node for the filesystem root, or NULL on failure.
 * Caller should pass the returned node to vfs_mount("/mountpoint", root).
 */
vfs_node_t *ext2_mount(uint32_t lba_offset, uint32_t nsect);

/*
 * Live filesystem geometry for statfs(): block size, total/free blocks and
 * total/free inodes, read from the superblock counters the allocator keeps up
 * to date.  Returns 0 when a filesystem is mounted, -1 otherwise (in which case
 * nothing is written).
 */
int ext2_statfs(uint32_t *block_size, uint32_t *blocks, uint32_t *bfree,
                uint32_t *inodes, uint32_t *ifree);
