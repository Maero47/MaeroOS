#pragma once
#include "vfs.h"
#include <stdint.h>

/*
 * Mount an ext2 filesystem starting at `lba_offset` on the ATA primary master.
 * Returns a VFS node for the filesystem root, or NULL on failure.
 * Caller should pass the returned node to vfs_mount("/mountpoint", root).
 */
vfs_node_t *ext2_mount(uint32_t lba_offset);

/*
 * Live filesystem geometry for statfs(): block size, total/free blocks and
 * total/free inodes, read from the superblock counters the allocator keeps up
 * to date.  Returns 0 when a filesystem is mounted, -1 otherwise (in which case
 * nothing is written).
 */
int ext2_statfs(uint32_t *block_size, uint32_t *blocks, uint32_t *bfree,
                uint32_t *inodes, uint32_t *ifree);
