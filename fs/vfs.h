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
/* AF_UNIX socket inode (S_IFSOCK), made by bind().  Outside the low three
 * bits on purpose: several callers test those bits as a mask. */
#define VFS_FLAG_SOCK    0x8

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
    uint64_t size;     /* file size in bytes (64-bit: files past 4 GiB) */
    uint32_t mask;     /* Unix permission bits */
    uint32_t uid, gid;
    uint32_t atime, mtime, ctime;
    uint32_t nlink;    /* hard links (0 = not tracked, stat reports 1) */
    uint32_t dev;      /* st_dev: device of the filesystem (0 = unspecified) */
    uint32_t rdev;     /* st_rdev: the device a BLKDEV/CHARDEV node stands for */

    /* ── Operations (NULL = use built-in defaults) ──────────────────── */
    /* Offsets are 64-bit; one call moves at most 4 GiB - 1 bytes. */
    uint32_t          (*read_fn)   (struct vfs_node *, uint64_t off,
                                    uint32_t len, uint8_t *buf);
    uint32_t          (*write_fn)  (struct vfs_node *, uint64_t off,
                                    uint32_t len, const uint8_t *buf);
    int               (*create_fn) (struct vfs_node *dir, const char *name,
                                    uint32_t flags);
    int               (*truncate_fn)(struct vfs_node *, uint64_t new_size);
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
    /* Move the entry `old_name` of this directory to `new_name` in `new_dir`
     * (a directory of the same filesystem), replacing any entry already there
     * in one step: `new_name` names either the old or the new object at every
     * point, never nothing.  The moved object keeps its inode, and with it its
     * mode, owner and contents.  Returns 0 or a negative errno.  Called only
     * through vfs_rename(), after the permission checks. */
    int               (*rename_fn) (struct vfs_node *old_dir, const char *old_name,
                                    struct vfs_node *new_dir, const char *new_name);
    /* Persist atime/mtime (utimensat); NULL = in-memory only. */
    int               (*settimes_fn)(struct vfs_node *, uint32_t atime,
                                     uint32_t mtime);
    /* link(2): add the name `name` in this directory for `target`, a node of
     * the same filesystem.  Returns 0 or a negative errno.  NULL = the
     * filesystem has no hard links (-EPERM). */
    int               (*link_fn)(struct vfs_node *dir, const char *name,
                                 struct vfs_node *target);

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

/*
 * vfs_lookup — the resolver behind vfs_open/vfs_open_nofollow.  Follows at
 * most 40 symlinks in total (absolute and relative targets); on failure
 * returns NULL and, if `err` is non-NULL, stores -ENOENT, -ELOOP or
 * -ENAMETOOLONG there.
 */
vfs_node_t *vfs_lookup(const char *path, int follow_final, int *err);

/* Read up to `size` bytes at `offset` from a file node */
uint32_t vfs_read(vfs_node_t *node, uint64_t offset, uint32_t size,
                  uint8_t *buf);

/*
 * A write_fn may return VFS_WRITE_ENOMEM instead of a byte count to say "I
 * allocated nothing and wrote nothing".  A plain 0 cannot carry that: write(2)
 * returning 0 for a non-zero request is not an error to its caller, so a libc
 * write loop spins on it forever.  tmpfs, whose file bodies come straight out
 * of the kernel heap, is the writer that can hit it.
 *
 * Every caller of vfs_write() must test for it before using the value as a
 * length.
 */
#define VFS_WRITE_ENOMEM  0xFFFFFFFFU

/* Likewise, VFS_WRITE_EFBIG says "`offset` is at or past the largest file this
 * filesystem can hold" (write(2) gets -EFBIG, as at Linux's s_maxbytes). */
#define VFS_WRITE_EFBIG   0xFFFFFFFEU

/* The largest file offset (Linux loff_t is signed 64-bit). */
#define VFS_OFF_MAX       0x7FFFFFFFFFFFFFFFULL

/* Write up to `size` bytes at `offset` to a file node; returns bytes written,
 * VFS_WRITE_ENOMEM or VFS_WRITE_EFBIG. */
uint32_t vfs_write(vfs_node_t *node, uint64_t offset, uint32_t size,
                   const uint8_t *buf);

/* Create a file or directory named `name` inside `dir`; returns 0 or -1 */
int vfs_create(vfs_node_t *dir, const char *name, uint32_t flags);

/* Truncate (or extend with zeros) a file to `new_size`; returns 0 or -1 */
int vfs_truncate(vfs_node_t *node, uint64_t new_size);

/* Remove file named `name` from directory `dir`; returns 0 or -errno */
int vfs_unlink(vfs_node_t *dir, const char *name);

/* Rename old_dir/old_name to new_dir/new_name atomically (see rename_fn).
 * Mount-point shims are looked through.  -EXDEV (-18) across filesystems,
 * -EPERM (-1) when the filesystem cannot rename. */
int vfs_rename(vfs_node_t *old_dir, const char *old_name,
               vfs_node_t *new_dir, const char *new_name);

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
/* The same, where the group class also applies when the file's group is any of
 * the `ngroups` supplementary gids in `groups` (Linux in_group_p). */
int vfs_access_check_groups(vfs_node_t *node, uint32_t uid, uint32_t gid,
                            const uint32_t *groups, uint32_t ngroups, int want);

/* Update mode/uid/gid (in-memory + persisted via setattr_fn if present). */
int vfs_setattr(vfs_node_t *node, uint32_t mode, uint32_t uid, uint32_t gid);

/* link(2): `name` in `dir` becomes another name of `target` (-EPERM without
 * filesystem support, -EXDEV across filesystems). */
int vfs_link(vfs_node_t *dir, const char *name, vfs_node_t *target);

/* utimensat: set atime and mtime (a filesystem hook also sets ctime). */
int vfs_settimes(vfs_node_t *node, uint32_t atime, uint32_t mtime);

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

/* ── Mount table (mount(2) / umount2(2)) ───────────────────────────────────
 *
 * A mount attaches the root of a filesystem instance to a directory node.
 * Path walks cross it: whenever vfs_finddir() yields a node that is mounted
 * on, the walk continues from the mounted root instead (repeatedly, so mounts
 * stack).  ".." out of a mounted root returns to the directory that held the
 * mountpoint, since the walk keeps its own parent stack.
 *
 * Mountpoints are matched by node identity.  That holds for every filesystem
 * here: initrd, devfs and procfs nodes are static, tmpfs nodes live as long as
 * their link, and ext2/ext4 keep one node per inode for the life of the mount.
 *
 * The boot-time mounts made with vfs_mount() above (/disk, /tmp, /dev, /proc)
 * stay as they are; vfs_mount_note() only lists them in /proc/mounts.
 */
#define VFS_MS_RDONLY   0x0001u     /* Linux MS_RDONLY */
#define VFS_MS_NOSUID   0x0002u
#define VFS_MS_NODEV    0x0004u
#define VFS_MS_NOEXEC   0x0008u
#define VFS_MS_REMOUNT  0x0020u
#define VFS_MNT_FORCE   0x0001u     /* umount2 flags */
#define VFS_MNT_DETACH  0x0002u

#define VFS_MNT_MAX 16

typedef struct vfs_statfs {
    uint32_t type;              /* f_type magic */
    uint32_t bsize;
    uint64_t blocks, bfree;
    uint64_t files, ffree;
    uint32_t namelen;
} vfs_statfs_t;

typedef struct vfs_mnt {
    int          used;
    int          boot;          /* listed only; not crossed, never unmounted */
    vfs_node_t  *mp;            /* the directory mounted on */
    vfs_node_t  *root;          /* the mounted filesystem's root */
    struct vfs_mnt *parent;     /* mount `mp` lives on, NULL for the boot tree */
    uint32_t     flags;         /* VFS_MS_* */
    uint32_t     seq;           /* mount order (newer = larger), never reused */
    /* Bind mounts: the mount the source directory lives on.  It cannot be
     * unmounted while this entry exists (its nodes would be freed under us). */
    struct vfs_mnt *src;
    char         source[64];
    char         target[256];
    char         fstype[16];
    void        *fs;            /* instance, for the two hooks below */
    /* Nonzero while the instance has files open (umount gets -EBUSY). */
    int        (*busy)(void *fs);
    /* Called once the mount is gone; frees the instance. */
    void       (*release)(void *fs);
    /* Switch the instance between read-only and read-write (remount);
     * NULL = only read-only is possible for a read-write request. */
    int        (*set_ro)(void *fs, int ro);
    /* statfs(2) numbers for the instance; NULL = the generic answer. */
    int        (*statfs)(void *fs, struct vfs_statfs *out);
} vfs_mnt_t;

/* Like vfs_lookup, and also report the mount the result lives on (NULL for
 * the boot tree). */
/* vfs_lookup_mnt as seen from `croot` (a process's pinned chroot, NULL for
 * the global root) rather than the caller's: for checks made on behalf of
 * another process. */
vfs_node_t *vfs_lookup_from(vfs_node_t *croot, const char *path,
                            int follow_final, int *err, vfs_mnt_t **mnt);
vfs_node_t *vfs_lookup_mnt(const char *path, int follow_final, int *err,
                           vfs_mnt_t **mnt);

/* Attach `root` at the directory `target` (an absolute, canonical path).
 * The other fields of `tmpl` (source, fstype, flags, fs, hooks) are copied.
 * 0 or -errno: -ENOENT, -ENOTDIR, -EBUSY (already a mount root of the same
 * instance), -ENOMEM (table full). */
int vfs_mount_add(const char *target, vfs_node_t *root, const vfs_mnt_t *tmpl);

/* Nonzero while any mount(2) mount exists. */
int vfs_mounts_active(void);

/* 1 when `m` is still the mount numbered `seq` and is read-only (open files
 * keep both, so a slot reused after umount is not mistaken for theirs). */
int vfs_mnt_rdonly(const vfs_mnt_t *m, uint32_t seq);

/* The mount whose root `path` names, or NULL (with *err set). */
vfs_mnt_t *vfs_mount_find(const char *path, int *err);

/* Detach a mount found with vfs_mount_find.  -EBUSY when another mount sits
 * on top of it, or when its instance is busy and VFS_MNT_DETACH is not given.
 * Calls the release hook unless the instance is still busy. */
int vfs_mount_remove(vfs_mnt_t *m, uint32_t flags);

/* Power-off and reboot: every writable mount whose filesystem has a set_ro
 * hook (vfat, ext2), and every lazily unmounted instance still in use, goes
 * read-only (flushed, its volume marked clean). */
void vfs_mounts_shutdown(void);

/* The filesystem instance a path lives on, for rename(2) and link(2), which
 * must stay within one (-EXDEV): two mounts of the same driver share their
 * node operations, so comparing those cannot tell instances apart.  Bind
 * mounts answer for the mount their source is on; NULL is the boot tree
 * (whose filesystems differ in their operations).  `path` is resolved like
 * vfs_lookup_mnt (following the final symlink); *ok is 0 when it does not
 * resolve. */
const void *vfs_path_instance(const char *path, int *ok);
const void *vfs_mnt_instance(const vfs_mnt_t *m);

/* 1 when a mount sits on `n` (rmdir/unlink of it is -EBUSY). */
int vfs_is_mountpoint(vfs_node_t *n);

/* List a boot-time mount in /proc/mounts. */
void vfs_mount_note(const char *source, const char *target, const char *fstype,
                    uint32_t flags);

/* /proc/mounts text; returns its length (at most `size`). */
uint32_t vfs_mounts_format(char *buf, uint32_t size);

/* 1 when the object `path` names (or, if it does not exist or `parent` is
 * set, the directory that would hold it) is on a read-only mount. */
int vfs_path_rdonly(const char *path, int parent);

/* 1 when `source` is mounted, or was lazily unmounted (MNT_DETACH) and its
 * instance still has open files (used to refuse a second mount of a device,
 * and raw writes to it).  Releases detached instances that have gone idle. */
int vfs_mount_has_fs_source(const char *source);
