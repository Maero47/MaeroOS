#pragma once
#include "vfs.h"
#include <stdint.h>

/*
 * Mount an ext2 filesystem starting at `lba_offset` on the ATA primary master.
 * Returns a VFS node for the filesystem root, or NULL on failure.
 * Caller should pass the returned node to vfs_mount("/mountpoint", root).
 */
vfs_node_t *ext2_mount(uint32_t lba_offset);
