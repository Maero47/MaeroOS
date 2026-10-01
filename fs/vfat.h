#pragma once
#include "vfs.h"
#include "../drivers/blkpart.h"

/*
 * Read-write FAT12/FAT16/FAT32 driver with long file names (VFAT), for USB
 * sticks, SD cards and EFI system partitions mounted with mount(2)
 * (`mount -t vfat /dev/sdb1 /mnt`).  Like fs/ext4.c it keeps all its state per
 * instance.  See docs/vfat.md.
 *
 * Every mutating operation is written through to the device before the
 * system call returns (metadata goes through a small sector cache that is
 * flushed at the end of each operation), so the volume is consistent between
 * calls; the boot sector's dirty flag is set while the volume is mounted
 * read-write and cleared again by umount.
 */

typedef struct vfat_fs vfat_fs_t;

/* Mount options (from mount(2)'s data string): uid=, gid=, umask=, dmask=,
 * fmask=.  Defaults: owner root, dmask/fmask 022. */
typedef struct {
    uint32_t uid, gid;
    uint32_t dmask, fmask;
} vfat_opts_t;

void vfat_parse_opts(const char *data, vfat_opts_t *o);

/* Mount the filesystem on `bp`.  On success returns 0 and sets *root and *fs.
 * -EINVAL: not a FAT filesystem (exFAT is fs/exfat.c's);
 * -ENOMEM; -EIO. */
int vfat_mount_dev(blkpart_t *bp, int ro, const vfat_opts_t *o,
                   vfs_node_t **root, vfat_fs_t **fs);

/* Hooks for the mount table (vfs_mnt_t busy/release/set_ro). */
int  vfat_busy(void *fs);
void vfat_release(void *fs);
int  vfat_set_ro(void *fs, int ro);
int  vfat_statfs(void *fs, vfs_statfs_t *out);
