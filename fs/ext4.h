#pragma once
#include "vfs.h"
#include "../drivers/blkpart.h"

/*
 * Read-only ext2/ext3/ext4 driver for filesystems mounted with mount(2).
 *
 * Unlike fs/ext2.c (the single read-write instance behind /disk) this driver
 * keeps all its state per instance, so any number of partitions can be
 * mounted at once.  It reads extent-mapped and block-mapped files, 64-bit
 * group descriptors, flex_bg and meta_bg layouts, files past 2 GiB (up to the
 * VFS's 4 GiB offsets), htree (dir_index) directories through their hash
 * index, and verifies metadata_csum checksums (superblock, inodes, extent
 * blocks).  It never writes: an ext4 journal cannot be honoured here, and a
 * write that bypassed it could corrupt the filesystem, so every mount is
 * read-only (see docs/ext4.md).
 */

typedef struct ext4_fs ext4_fs_t;

/* Mount the filesystem on `bp`.  On success returns 0 and sets *root and *fs.
 * -EINVAL: not an ext2/3/4 filesystem or corrupt; -EUCLEAN (117) needs journal
 * recovery; -EOPNOTSUPP (95) uses an incompatible feature this driver cannot
 * read; -ENOMEM; -EIO. */
int ext4_mount_dev(blkpart_t *bp, vfs_node_t **root, ext4_fs_t **fs);

/* Hooks for the mount table (vfs_mnt_t busy/release). */
int  ext4_busy(void *fs);
void ext4_release(void *fs);
