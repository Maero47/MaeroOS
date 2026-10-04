#pragma once
#include "vfs.h"

struct proc;

/* Returns the root /proc directory node to be mounted at /proc */
vfs_node_t *procfs_mount(void);

/* Linux ptrace_may_access(PTRACE_MODE_READ_FSCREDS) for the calling process
 * on `target`: the same thread group, root, or the same user and group in
 * all of the target's real/effective/saved ids while it is dumpable.  The
 * /proc/<pid> files that expose a process's memory, environment and open
 * files (environ, auxv, maps, io, fd/, fdinfo/, exe, cwd, root) answer
 * -EACCES without it. */
int procfs_may_read(struct proc *target);

/* Let go of the /proc nodes p's syscall looked up (at its return, and at
 * exit); until then the node cache cannot free them. */
void procfs_walk_done(struct proc *p);
