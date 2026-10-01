#pragma once
#include "vfs.h"
#include "vfat.h"
#include "../drivers/blkpart.h"

/*
 * Read-write exFAT driver, for USB sticks and SD cards larger than 32 GiB,
 * mounted with mount(2) (`mount -t exfat /dev/sdb1 /mnt`).  Like fs/vfat.c it
 * keeps all its state per instance and writes every change through before the
 * system call returns; the boot sector's VolumeDirty flag is set while the
 * volume is mounted read-write.  See docs/exfat.md.
 */

typedef struct exfat_fs exfat_fs_t;

/* Mount the filesystem on `bp` (options as for vfat: uid=, gid=, umask=,
 * dmask=, fmask=).  On success returns 0 and sets *root and *fs.
 * -EINVAL: not an exFAT volume (or an invalid one); -EROFS: read-write asked
 * for a volume this driver only reads; -ENOMEM; -EIO. */
int exfat_mount_dev(blkpart_t *bp, int ro, const vfat_opts_t *o,
                    vfs_node_t **root, exfat_fs_t **fs);

/* Hooks for the mount table (vfs_mnt_t busy/release/set_ro/statfs). */
int  exfat_busy(void *fs);
void exfat_release(void *fs);
int  exfat_set_ro(void *fs, int ro);
int  exfat_statfs(void *fs, vfs_statfs_t *out);
