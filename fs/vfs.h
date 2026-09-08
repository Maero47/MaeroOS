#pragma once
#include <stdint.h>

/* Node type flags */
#define VFS_FLAG_FILE    0x1
#define VFS_FLAG_DIR     0x2
#define VFS_FLAG_CHARDEV 0x3
#define VFS_FLAG_BLKDEV  0x4
#define VFS_FLAG_PIPE    0x5
#define VFS_FLAG_SYMLINK 0x6
#define VFS_FLAG_FIFO    0x7   /* named pipe (FIFO) */

/* Returned by vfs_readdir for each entry */
typedef struct vfs_dirent {
    uint32_t ino;
    uint8_t  type;   /* VFS_FLAG_* */
    char     name[256];
} vfs_dirent_t;

/*
 * VFS node — represents a file or directory.
 *
 * Operation pointers:
 *   NULL  → fall back to built-in default (initrd in-memory behaviour).
 *   !NULL → use the supplied function (ext2, devfs, …).
 *
 * Backward-compatible with the original initrd implementation: initrd nodes
 * leave all function pointers NULL and use the `data` / `children` fields.
 */
typedef struct vfs_node {
    char     name[256];
    uint32_t flags;    /* VFS_FLAG_* */
    uint32_t inode;    /* filesystem inode number */
    uint32_t size;     /* file size in bytes */
    uint32_t mask;     /* Unix permission bits */
    uint32_t uid, gid;
    uint32_t atime, mtime, ctime;

    /* ── Operations (NULL = use built-in defaults) ──────────────────── */
    uint32_t          (*read_fn)   (struct vfs_node *, uint32_t off,
                                    uint32_t len, uint8_t *buf);
    uint32_t          (*write_fn)  (struct vfs_node *, uint32_t off,
                                    uint32_t len, const uint8_t *buf);
    int               (*create_fn) (struct vfs_node *dir, const char *name,
                                    uint32_t flags);
    int               (*truncate_fn)(struct vfs_node *, uint32_t new_size);
    int               (*unlink_fn) (struct vfs_node *dir, const char *name);
    int               (*symlink_fn)(struct vfs_node *dir, const char *name,
                                    const char *target);
    int               (*readdir_fn)(struct vfs_node *, uint32_t idx,
                                    vfs_dirent_t *out);
    struct vfs_node * (*finddir_fn)(struct vfs_node *, const char *name);
    int               (*ioctl_fn)  (struct vfs_node *, uint32_t req,
                                    void *arg);
    int               (*read_ready_fn)(struct vfs_node *);
    int               (*write_ready_fn)(struct vfs_node *);
    void              (*retain_fn) (struct vfs_node *);
    void              (*close_fn)  (struct vfs_node *);
    /* Cloning device (Linux /dev/ptmx): the node a *lookup* returns only
     * describes the device, and the node a *descriptor* holds is a fresh one
     * this hook allocates.  The open path calls it once the open is certain to
     * succeed — after the permission check and after a free descriptor slot has
     * been found — and retains whatever it returns, so a lookup that never
     * becomes an open (stat, access, execve, a failed open) reserves nothing.
     * Returning NULL means the device has no capacity left; the open then fails
     * exactly as a missing node would.  A finddir_fn that allocates per lookup
     * instead of setting this leaks on every stat(). */
    struct vfs_node * (*open_fn)   (struct vfs_node *);
    /* Persist mode/uid/gid changes (chmod/chown); NULL = in-memory only. */
    int               (*setattr_fn)(struct vfs_node *, uint32_t mode,
                                    uint32_t uid, uint32_t gid);

    /* ── initrd in-memory backing ────────────────────────────────────── */
    const uint8_t   *data;       /* file: pointer into initrd memory */
    struct vfs_node *children;   /* dir:  first child node */
    struct vfs_node *next;       /* sibling in the same directory */

    /* ── filesystem-specific private data (e.g. ext2_priv_t *) ──────── */
    void            *private;
} vfs_node_t;

/* Global root directory */
extern vfs_node_t *vfs_root;

/* Initialise VFS (no-op; root is populated by initrd_init or ext2_mount) */
void vfs_init(void);

/*
 * vfs_open — resolve an absolute path and return the vfs_node.
 * Handles multi-level paths (e.g. "/usr/bin/ls").
 * Returns NULL if not found or path is invalid.
 */
vfs_node_t *vfs_open(const char *path);

/* Read up to `size` bytes at `offset` from a file node */
uint32_t vfs_read(vfs_node_t *node, uint32_t offset, uint32_t size,
                  uint8_t *buf);

/* Write up to `size` bytes at `offset` to a file node; returns bytes written */
uint32_t vfs_write(vfs_node_t *node, uint32_t offset, uint32_t size,
                   const uint8_t *buf);

/* Create a file or directory named `name` inside `dir`; returns 0 or -1 */
int vfs_create(vfs_node_t *dir, const char *name, uint32_t flags);

/* Truncate (or extend with zeros) a file to `new_size`; returns 0 or -1 */
int vfs_truncate(vfs_node_t *node, uint32_t new_size);

/* Remove file named `name` from directory `dir`; returns 0 or -errno */
int vfs_unlink(vfs_node_t *dir, const char *name);

/* Fill `out` with entry `index` of directory `dir`; returns 0 or -1 */
int vfs_readdir(vfs_node_t *dir, uint32_t index, vfs_dirent_t *out);

/* Look up `name` in directory `dir`; returns node or NULL */
vfs_node_t *vfs_finddir(vfs_node_t *dir, const char *name);

/* Release a node (no-op for initrd nodes) */
void vfs_close(vfs_node_t *node);
void vfs_retain(vfs_node_t *node);

/* ── Permission checking (Phase 24) ──────────────────────────────────────── */
#define VFS_WANT_R 4
#define VFS_WANT_W 2
#define VFS_WANT_X 1
/* Returns 0 if (euid,egid) may access node for `want` (R/W/X bits), else
 * -13 (-EACCES).  euid 0 (root) bypasses, except X on a file still needs an
 * execute bit somewhere. */
int vfs_access_check(vfs_node_t *node, uint32_t euid, uint32_t egid, int want);

/* Update mode/uid/gid (in-memory + persisted via setattr_fn if present). */
int vfs_setattr(vfs_node_t *node, uint32_t mode, uint32_t uid, uint32_t gid);

/*
 * vfs_mount — attach `fs_root` at `path` inside the current VFS tree.
 * Creates a directory node at `path` whose finddir_fn delegates to `fs_root`.
 * Only handles single-level mount points under "/" (e.g. "/disk").
 */
int vfs_mount(const char *path, vfs_node_t *fs_root);

/*
 * vfs_set_root_overlay — make normal absolute paths prefer a mounted disk root.
 * Reserved kernel mount points (/dev, /proc, /tmp, /disk) remain on vfs_root.
 */
void vfs_set_root_overlay(vfs_node_t *fs_root);
vfs_node_t *vfs_get_root_overlay(void);
int vfs_path_uses_root_overlay(const char *path);

/*
 * vfs_symlink — create a symlink named `path` pointing at `target`.
 * Returns 0 on success or a negative errno.
 */
int vfs_symlink(const char *target, const char *path);

/*
 * vfs_open_nofollow — like vfs_open but does NOT follow the final symlink.
 * Used by readlink().
 */
vfs_node_t *vfs_open_nofollow(const char *path);
