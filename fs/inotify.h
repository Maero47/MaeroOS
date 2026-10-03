#pragma once
#include <stdint.h>
#include "vfs.h"

/*
 * inotify(7): file-change notification.
 *
 * An inotify instance is a synthetic vfs node held by an FD_FILE
 * descriptor (read, poll/select/epoll readiness, FIONREAD, close all go
 * through the node's hooks).  A watch names a vfs node: filesystems here keep
 * one node per inode while it is in use (ext2/ext4, tmpfs, vfat, exfat), and
 * the watch holds a reference on it, as Linux pins the inode.
 *
 * Events come from the VFS: vfs_create/vfs_unlink/vfs_rename report on the
 * directory (IN_CREATE, IN_DELETE, IN_MOVED_FROM/IN_MOVED_TO with a shared
 * cookie, IN_ISDIR for directories) and on the object (IN_DELETE_SELF,
 * IN_MOVE_SELF); vfs_setattr/vfs_settimes report IN_ATTRIB; the write,
 * truncate and close paths in proc/syscall.c report IN_MODIFY and
 * IN_CLOSE_WRITE/IN_CLOSE_NOWRITE.  An event about a file goes to the
 * file's own watches and, with its name, to its directory's (the directory
 * is the one the last path lookup of the file went through).
 *
 * The queue holds at most INOTIFY_MAX_QUEUED events; one more turns into a
 * single IN_Q_OVERFLOW (wd -1).  An event identical to the last queued one
 * is merged, as Linux does.
 */

#define IN_ACCESS        0x00000001
#define IN_MODIFY        0x00000002
#define IN_ATTRIB        0x00000004
#define IN_CLOSE_WRITE   0x00000008
#define IN_CLOSE_NOWRITE 0x00000010
#define IN_OPEN          0x00000020
#define IN_MOVED_FROM    0x00000040
#define IN_MOVED_TO      0x00000080
#define IN_CREATE        0x00000100
#define IN_DELETE        0x00000200
#define IN_DELETE_SELF   0x00000400
#define IN_MOVE_SELF     0x00000800
#define IN_UNMOUNT       0x00002000
#define IN_Q_OVERFLOW    0x00004000
#define IN_IGNORED       0x00008000
#define IN_ONLYDIR       0x01000000
#define IN_DONT_FOLLOW   0x02000000
#define IN_EXCL_UNLINK   0x04000000
#define IN_MASK_CREATE   0x10000000
#define IN_MASK_ADD      0x20000000
#define IN_ISDIR         0x40000000
#define IN_ONESHOT       0x80000000
#define IN_ALL_EVENTS    0x00000FFF

#define INOTIFY_MAX_WATCHES   8192     /* per user (fs.inotify.max_user_watches) */
#define INOTIFY_MAX_INSTANCES 128      /* per user (fs.inotify.max_user_instances) */
#define INOTIFY_MAX_QUEUED    16384    /* per instance (fs.inotify.max_queued_events) */
/* Kernel heap held by queued events (the heap is one 256 MiB window).  A
 * non-root user's instances together hold at most INOTIFY_USER_BYTES, all
 * instances of everyone (root too) INOTIFY_TOTAL_BYTES; an event past
 * either ends that queue in IN_Q_OVERFLOW, as max_queued_events does.
 * Watches: at most INOTIFY_TOTAL_WATCHES in all.
 * /proc/sys/fs/inotify/max_user_bytes and max_total_bytes show them. */
#define INOTIFY_USER_BYTES    (2U << 20)
#define INOTIFY_TOTAL_BYTES   (16U << 20)
#define INOTIFY_TOTAL_WATCHES 65536
/* Heap bytes queued now, by everyone (/proc/sys/fs/inotify/queued_bytes). */
extern uint32_t inotify_heap_bytes;

/* A new instance node (no references yet: the caller's descriptor takes the
 * first with vfs_retain), or NULL. */
vfs_node_t *inotify_new(void);
/* Is `n` an inotify instance? */
int inotify_is(vfs_node_t *n);
/* inotify_add_watch on the resolved `target`: the watch descriptor or a
 * negative errno. */
int inotify_add(vfs_node_t *inst, vfs_node_t *target, uint32_t mask);
/* inotify_rm_watch: 0 or -EINVAL. */
int inotify_rm(vfs_node_t *inst, int wd);

/* Nonzero while any watch exists (the hooks return at once otherwise). */
extern uint32_t inotify_nwatches;

/* Hooks (fs/vfs.c, proc/syscall.c). */
void inotify_dir_event(vfs_node_t *dir, uint32_t mask, uint32_t cookie,
                       const char *name);
void inotify_self_event(vfs_node_t *node, uint32_t mask);
/* The same for a directory known only by address (vfs_last_parent): never
 * dereferenced, since it may have gone since. */
void inotify_parent_event(vfs_node_t *dir, uint32_t mask, const char *name);
/* An event about `node` reached through a path: its own watches, and its
 * directory's with its name when the last lookup that produced it is
 * known.  `path` (may be NULL) is looked up first to learn it. */
void inotify_child_event(vfs_node_t *node, const char *path, uint32_t mask);
/* Object gone (unlinked for good): IN_DELETE_SELF, then IN_IGNORED, and
 * its watches go. */
void inotify_node_gone(vfs_node_t *node);
uint32_t inotify_next_cookie(void);

/* /proc/<pid>/fdinfo lines ("inotify wd:.. ino:.. mask:..") for an instance. */
void inotify_show_fdinfo(vfs_node_t *n, char *buf, uint32_t *pos, uint32_t cap);
