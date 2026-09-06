#include "vfs.h"
#include "tmpfs.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include <stddef.h>

vfs_node_t *vfs_root = NULL;
static vfs_node_t *vfs_root_overlay = NULL;

void vfs_init(void) {
    /* root is populated by initrd_init or ext2_mount */
}

/* ── Low-level node operations ────────────────────────────────────────────── */

uint32_t vfs_read(vfs_node_t *node, uint32_t offset, uint32_t size,
                  uint8_t *buf) {
    if (!node) return 0;
    if (node->read_fn)
        return node->read_fn(node, offset, size, buf);
    /* Default: initrd in-memory data */
    if (!(node->flags & VFS_FLAG_FILE) || !node->data) return 0;
    if (offset >= node->size) return 0;
    if (offset + size > node->size) size = node->size - offset;
    memcpy(buf, node->data + offset, size);
    return size;
}

int vfs_readdir(vfs_node_t *dir, uint32_t index, vfs_dirent_t *out) {
    if (!dir || !(dir->flags & VFS_FLAG_DIR)) return -1;
    if (dir->readdir_fn)
        return dir->readdir_fn(dir, index, out);
    /* Default: walk initrd children list */
    vfs_node_t *n = dir->children;
    uint32_t i = 0;
    while (n) {
        if (i == index) {
            out->ino  = n->inode;
            out->type = (uint8_t)n->flags;
            strncpy(out->name, n->name, 255);
            out->name[255] = '\0';
            return 0;
        }
        n = n->next;
        i++;
    }
    return -1;  /* no more entries */
}

vfs_node_t *vfs_finddir(vfs_node_t *dir, const char *name) {
    if (!dir || !(dir->flags & VFS_FLAG_DIR)) return NULL;
    if (dir->finddir_fn)
        return dir->finddir_fn(dir, name);
    /* Default: linear scan of initrd children */
    for (vfs_node_t *n = dir->children; n; n = n->next)
        if (strcmp(n->name, name) == 0) return n;
    return NULL;
}

uint32_t vfs_write(vfs_node_t *node, uint32_t offset, uint32_t size,
                   const uint8_t *buf) {
    if (!node || !node->write_fn) return 0;
    return node->write_fn(node, offset, size, buf);
}

int vfs_create(vfs_node_t *dir, const char *name, uint32_t flags) {
    if (!dir || !dir->create_fn) return -1;
    return dir->create_fn(dir, name, flags);
}

int vfs_truncate(vfs_node_t *node, uint32_t new_size) {
    if (!node || !node->truncate_fn) return -1;
    return node->truncate_fn(node, new_size);
}

int vfs_unlink(vfs_node_t *dir, const char *name) {
    if (!dir || !dir->unlink_fn) return -1;
    return dir->unlink_fn(dir, name);
}

void vfs_close(vfs_node_t *node) {
    if (!node) return;
    if (node->close_fn) node->close_fn(node);
}

void vfs_retain(vfs_node_t *node) {
    if (!node) return;
    if (node->retain_fn) node->retain_fn(node);
}

int vfs_access_check(vfs_node_t *node, uint32_t euid, uint32_t egid, int want) {
    uint32_t m, bits;

    if (!node) return -2;            /* -ENOENT */
    if (euid == 0) {                 /* root bypasses, except X needs an x bit */
        if ((want & VFS_WANT_X) && node->flags == VFS_FLAG_FILE &&
            !(node->mask & 0111))
            return -13;
        return 0;
    }
    m = node->mask;
    /* Many synthetic nodes (devfs, procfs, freshly-created tmpfs dirs) are
     * built without an explicit mode.  A zero mask should not lock everyone
     * out: device/pipe/fifo nodes default to world rw (0666); read-only
     * synthetic files/dirs default to world read + traverse (0555).  Real
     * on-disk files always carry a non-zero ext2 mode, so this never weakens
     * an intentionally-restricted file (e.g. /etc/shadow at 0600). */
    if (m == 0) {
        if (node->flags != VFS_FLAG_FILE && node->flags != VFS_FLAG_DIR)
            m = 0666;
        else
            m = 0555;
    }
    bits = (euid == node->uid) ? (m >> 6)
         : (egid == node->gid) ? (m >> 3)
         : m;
    return (((int)bits & 7 & want) == want) ? 0 : -13;   /* -EACCES */
}

int vfs_setattr(vfs_node_t *node, uint32_t mode, uint32_t uid, uint32_t gid) {
    if (!node) return -2;
    node->mask = mode & 07777;
    node->uid = uid;
    node->gid = gid;
    if (node->setattr_fn)
        return node->setattr_fn(node, node->mask, uid, gid);
    return 0;
}

/* ── Path resolution ──────────────────────────────────────────────────────── */

static int path_first_component(const char *path, char *component) {
    if (!path || path[0] != '/') return 0;
    const char *p = path + 1;
    while (*p == '/') p++;
    if (*p == '\0') return 0;
    int len = 0;
    while (*p && *p != '/' && len < 255)
        component[len++] = *p++;
    component[len] = '\0';
    return len;
}

int vfs_path_uses_root_overlay(const char *path) {
    char first[256];
    int len = path_first_component(path, first);
    if (len <= 0 || !vfs_root_overlay) return 0;
    if (strcmp(first, "dev") == 0) return 0;
    if (strcmp(first, "proc") == 0) return 0;
    if (strcmp(first, "tmp") == 0) return 0;
    if (strcmp(first, "disk") == 0) return 0;
    return 1;
}

void vfs_set_root_overlay(vfs_node_t *fs_root) {
    vfs_root_overlay = fs_root;
}

vfs_node_t *vfs_get_root_overlay(void) {
    return vfs_root_overlay;
}

static vfs_node_t *vfs_open_from(vfs_node_t *root, const char *path,
                                 int follow_final) {
    if (!path || path[0] != '/' || !root) return NULL;

    const char *p = path + 1;
    if (*p == '\0') return root;

    vfs_node_t *cur = root;
    vfs_node_t *parents[64];
    uint32_t depth = 0;
    char component[256];

    while (*p) {
        /* skip consecutive slashes */
        while (*p == '/') p++;
        if (*p == '\0') break;

        /* extract next component */
        int len = 0;
        while (*p && *p != '/' && len < 255)
            component[len++] = *p++;
        component[len] = '\0';
        const char *after_component = p;
        while (*after_component == '/') after_component++;
        int is_final = (*after_component == '\0');

        /* handle "." */
        if (len == 1 && component[0] == '.') continue;

        /* handle ".." — walk up (root stays root) */
        if (len == 2 && component[0] == '.' && component[1] == '.') {
            if (depth > 0)
                cur = parents[--depth];
            else
                cur = root;
            continue;
        }

        vfs_node_t *parent = cur;
        cur = vfs_finddir(cur, component);
        if (!cur) return NULL;
        if (depth < (sizeof(parents) / sizeof(parents[0])))
            parents[depth++] = parent;

        /* Follow symlinks (up to 8 levels deep) */
        int sym_loops = 0;
        while ((!is_final || follow_final) &&
               cur && cur->flags == VFS_FLAG_SYMLINK && sym_loops < 8) {
            char target[256];
            uint32_t tlen = vfs_read(cur, 0, sizeof(target) - 1,
                                     (uint8_t *)target);
            target[tlen] = '\0';
            if (target[0] == '/') {
                cur = vfs_open(target);
            } else {
                /* Relative: can't easily resolve without parent; skip */
                break;
            }
            sym_loops++;
        }
        if (!cur) return NULL;
    }

    /* Resolve final node if it's a symlink */
    int sym_loops2 = 0;
    while (follow_final && cur && cur->flags == VFS_FLAG_SYMLINK && sym_loops2 < 8) {
        char target[256];
        uint32_t tlen = vfs_read(cur, 0, sizeof(target) - 1, (uint8_t *)target);
        target[tlen] = '\0';
        if (target[0] == '/') cur = vfs_open(target);
        else break;
        sym_loops2++;
    }
    return cur;
}

vfs_node_t *vfs_open(const char *path) {
    if (!path || path[0] != '/' || !vfs_root) return NULL;

    if (vfs_path_uses_root_overlay(path)) {
        vfs_node_t *node = vfs_open_from(vfs_root_overlay, path, 1);
        if (node) return node;
    }

    return vfs_open_from(vfs_root, path, 1);
}

/* ── Mount ────────────────────────────────────────────────────────────────── */

/*
 * Mount-point shim: a directory node whose finddir_fn / readdir_fn delegate
 * directly to the mounted filesystem root.
 */
static vfs_node_t *mnt_finddir(vfs_node_t *self, const char *name) {
    vfs_node_t *fs_root = (vfs_node_t *)self->private;
    return vfs_finddir(fs_root, name);
}

static int mnt_readdir(vfs_node_t *self, uint32_t idx, vfs_dirent_t *out) {
    vfs_node_t *fs_root = (vfs_node_t *)self->private;
    return vfs_readdir(fs_root, idx, out);
}

static int mnt_create(vfs_node_t *self, const char *name, uint32_t flags) {
    vfs_node_t *fs_root = (vfs_node_t *)self->private;
    return vfs_create(fs_root, name, flags);
}

static int mnt_unlink(vfs_node_t *self, const char *name) {
    vfs_node_t *fs_root = (vfs_node_t *)self->private;
    return vfs_unlink(fs_root, name);
}

static int mnt_symlink(vfs_node_t *self, const char *name, const char *target) {
    vfs_node_t *fs_root = (vfs_node_t *)self->private;
    if (!fs_root || !fs_root->symlink_fn) return -38;
    return fs_root->symlink_fn(fs_root, name, target);
}

/* ── Symlink creation ─────────────────────────────────────────────────────── */

int vfs_symlink(const char *target, const char *path) {
    if (!target || !path) return -22;
    /* Split path into dir + name */
    int plen = (int)strlen(path);
    int slash = -1;
    for (int i = plen - 1; i >= 0; i--) {
        if (path[i] == '/') { slash = i; break; }
    }
    if (slash < 0) return -22;

    char dir_path[256], name[256];
    if (slash == 0) {
        dir_path[0] = '/'; dir_path[1] = '\0';
    } else {
        int dl = slash < 255 ? slash : 255;
        memcpy(dir_path, path, (uint32_t)dl);
        dir_path[dl] = '\0';
    }
    int blen = plen - slash - 1;
    if (blen <= 0 || blen > 255) return -22;
    memcpy(name, path + slash + 1, (uint32_t)blen);
    name[blen] = '\0';

    vfs_node_t *dir = vfs_open(dir_path);
    if (!dir) return -2;
    if (!dir->symlink_fn) return -38;

    return dir->symlink_fn(dir, name, target);
}

/* ── No-follow path resolution ────────────────────────────────────────────── */

vfs_node_t *vfs_open_nofollow(const char *path) {
    if (!path || path[0] != '/' || !vfs_root) return NULL;

    if (vfs_path_uses_root_overlay(path)) {
        vfs_node_t *node = vfs_open_from(vfs_root_overlay, path, 0);
        if (node) return node;
    }

    return vfs_open_from(vfs_root, path, 0);
}

/* ── Mount ────────────────────────────────────────────────────────────────── */

int vfs_mount(const char *path, vfs_node_t *fs_root) {
    if (!path || path[0] != '/' || !fs_root) return -1;

    const char *name = path + 1;   /* e.g. "disk" from "/disk" */

    /* Allocate a mount-point node */
    vfs_node_t *mp = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
    if (!mp) return -1;
    memset(mp, 0, sizeof(vfs_node_t));

    strncpy(mp->name, name, 255);
    mp->flags       = VFS_FLAG_DIR;
    mp->finddir_fn  = mnt_finddir;
    mp->readdir_fn  = mnt_readdir;
    mp->create_fn   = mnt_create;
    mp->unlink_fn   = mnt_unlink;
    mp->symlink_fn  = mnt_symlink;
    mp->private     = fs_root;
    /* Inherit the mounted filesystem root's ownership and permissions, so an
     * access check against the mountpoint matches the real fs behind it.
     * Without this, mp->mask stays 0 → vfs_access_check's "synthetic dir"
     * fallback (0555, read-only) wrongly denied unprivileged writes to a
     * world-writable mount like the /tmp tmpfs (0777). */
    mp->mask = fs_root->mask;
    mp->uid  = fs_root->uid;
    mp->gid  = fs_root->gid;

    /* Prepend to vfs_root->children */
    mp->next = vfs_root->children;
    vfs_root->children = mp;
    return 0;
}
