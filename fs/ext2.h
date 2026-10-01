#pragma once
#include "vfs.h"
#include "../drivers/blkpart.h"
#include <stdint.h>

/*
 * Read-write ext2 driver.  Every mounted filesystem is one instance with its
 * own superblock geometry, block cache, node cache and open-inode table: /disk
 * (ext2_mount, at boot) and any number of mount(2) instances (ext2_mount_dev).
 */
typedef struct ext2_fs ext2_fs_t;

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
 * Mount the filesystem on `bp` for mount(2), read-only when `ro`.  On success
 * returns 0 and sets *root and *fs.  -EINVAL: not ext2/3/4 or corrupt;
 * -EOPNOTSUPP (95): an incompatible feature this driver does not read (the
 * read-only ext4 driver may); -EUCLEAN (117): a journal needs recovery;
 * -EROFS: a read-write request on a filesystem with a read-only-compatible
 * feature this driver cannot keep up to date; -ENOMEM; -EIO.
 */
int ext2_mount_dev(blkpart_t *bp, int ro, vfs_node_t **root, ext2_fs_t **fs);

/* sync(2)/fsync(2): commit the journal of every ext3/ext4 instance. */
void ext2_sync_all(void);

/* Test hook: lose power after the commit that ends the next write(2). */
void ext2_test_crash(ext2_fs_t *fs);

/* Hooks for the mount table (vfs_mnt_t busy/release/set_ro). */
int  ext2_busy(void *fs);
void ext2_release(void *fs);
int  ext2_set_ro(void *fs, int ro);

/*
 * Live filesystem geometry for statfs(): block size, total/free blocks and
 * total/free inodes, read from the superblock counters the allocator keeps up
 * to date.  ext2_statfs is /disk's.  Returns 0 when a filesystem is mounted,
 * -1 otherwise (in which case nothing is written).
 */
int ext2_statfs(uint32_t *block_size, uint32_t *blocks, uint32_t *bfree,
                uint32_t *inodes, uint32_t *ifree);
int ext2_statfs_fs(ext2_fs_t *fs, uint32_t *block_size, uint32_t *blocks,
                   uint32_t *bfree, uint32_t *inodes, uint32_t *ifree);
