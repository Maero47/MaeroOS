#pragma once
#include "vfs.h"

/*
 * tmpfs — in-memory writable filesystem.
 *
 * Creates an empty root directory.  Files are backed by kmalloc'd buffers
 * that grow automatically on write.  Directories support create/finddir/readdir.
 */
vfs_node_t *tmpfs_mount(void);
