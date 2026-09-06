#pragma once
#include "vfs.h"

/* Returns the root /proc directory node to be mounted at /proc */
vfs_node_t *procfs_mount(void);
