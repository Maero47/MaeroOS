#pragma once
#include <stdint.h>
#include "vfs.h"

/*
 * mount(2) / umount2(2) behind the syscall layer.  All strings are kernel
 * copies; `target` is absolute and canonical.  Return 0 or a negative errno.
 * The caller has already checked that the process is privileged.
 */
int mount_do(const char *source, const char *target, const char *fstype,
             uint32_t flags, const char *data);
int umount_do(const char *target, uint32_t flags);

/* The boot-time procfs and devfs roots, which `mount -t proc` / `-t devtmpfs`
 * attach again elsewhere (one instance each, as on Linux). */
void mount_set_boot_roots(vfs_node_t *proc_root, vfs_node_t *dev_root);
