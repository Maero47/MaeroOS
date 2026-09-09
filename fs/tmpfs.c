#include "tmpfs.h"
#include "vfs.h"
#include "../mm/heap.h"
#include "../lib/string.h"
#include <stddef.h>
#include <stdint.h>

/*
 * tmpfs — in-memory writable filesystem.
 *
 * Each node is a tmpfs_node_t (with the vfs_node_t as its first field so
 * it can be cast freely between the two types).  Files carry a heap-allocated
 * data buffer; directories carry a singly-linked list of child tmpfs_node_t.
 */

typedef struct tmpfs_node {
    vfs_node_t          vnode;      /* MUST be first — callers cast to vfs_node_t* */
    /* Reference count.  One reference belongs to the link in the parent
     * directory; every long-lived holder of the node (an open file descriptor,
     * a file-backed VMA, the shared-mapping registry) takes another through
     * vfs_retain() and drops it through vfs_close().  The node is freed only
     * when the count reaches zero, so unlink() on a file that is still open
     * detaches the name and leaves the data readable through the descriptor —
     * the Unix semantics every program relies on, and which the kernel used to
     * violate by kfree()ing the node inside tmpfs_unlink(). */
    int                 refs;
    /* file backing */
    uint8_t            *data;       /* NULL for dirs */
    uint32_t            capacity;   /* allocated bytes */
    /* directory children */
    struct tmpfs_node  *first_child;/* singly-linked list (youngest first) */
} tmpfs_node_t;

/* ── Forward declarations ──────────────────────────────────────────────────── */

static uint32_t          tmpfs_read    (vfs_node_t *, uint32_t, uint32_t, uint8_t *);
static uint32_t          tmpfs_write   (vfs_node_t *, uint32_t, uint32_t, const uint8_t *);
static int               tmpfs_truncate(vfs_node_t *, uint32_t);
static int               tmpfs_readdir (vfs_node_t *, uint32_t, vfs_dirent_t *);
static vfs_node_t *      tmpfs_finddir (vfs_node_t *, const char *);
static int               tmpfs_create  (vfs_node_t *, const char *, uint32_t);
static int               tmpfs_unlink  (vfs_node_t *, const char *);
static int               tmpfs_symlink (vfs_node_t *, const char *, const char *);
static void              tmpfs_retain  (vfs_node_t *);
static void              tmpfs_release (vfs_node_t *);

/* ── Node factory ─────────────────────────────────────────────────────────── */

static tmpfs_node_t *alloc_tmpfs_node(const char *name, uint32_t flags) {
    tmpfs_node_t *tn = (tmpfs_node_t *)kmalloc(sizeof(tmpfs_node_t));
    if (!tn) return NULL;
    memset(tn, 0, sizeof(tmpfs_node_t));

    strncpy(tn->vnode.name, name, 255);
    tn->vnode.name[255] = '\0';
    tn->vnode.flags = flags;
    tn->refs = 1;                    /* the link in the parent directory */
    tn->vnode.retain_fn = tmpfs_retain;
    tn->vnode.close_fn  = tmpfs_release;
    /* /tmp is world-writable (like Unix 01777); created files get the
     * creator's uid stamped by the open path. */
    tn->vnode.mask = (flags == VFS_FLAG_DIR) ? 0777 : 0666;

    if (flags != VFS_FLAG_DIR) {
        /* Files, symlinks and the FIFO/device nodes mknod() creates: none of
         * them are directories, so they must NOT carry directory operations
         * (a FIFO with readdir/finddir would resolve as a directory). */
        tn->vnode.read_fn     = tmpfs_read;
        if (flags == VFS_FLAG_FILE) {
            tn->vnode.write_fn    = tmpfs_write;
            tn->vnode.truncate_fn = tmpfs_truncate;
        }
    } else {
        tn->vnode.readdir_fn = tmpfs_readdir;
        tn->vnode.finddir_fn = tmpfs_finddir;
        tn->vnode.create_fn  = tmpfs_create;
        tn->vnode.unlink_fn  = tmpfs_unlink;
        tn->vnode.symlink_fn = tmpfs_symlink;
    }
    return tn;
}

/* ── Reference counting ───────────────────────────────────────────────────── */

static void tmpfs_retain(vfs_node_t *node) {
    if (node) ((tmpfs_node_t *)node)->refs++;
}

/* Drop one reference; free the node once nothing holds it any more.  A
 * directory is never freed while it still has children: dropping the parent's
 * reference to a non-empty directory would strand them, and unlink() already
 * refuses to remove a non-empty directory. */
static void tmpfs_release(vfs_node_t *node) {
    if (!node) return;
    tmpfs_node_t *tn = (tmpfs_node_t *)node;
    if (--tn->refs > 0) return;
    if (tn->data) { kfree(tn->data); tn->data = NULL; }
    kfree(tn);
}

/* ── File operations ──────────────────────────────────────────────────────── */

static uint32_t tmpfs_read(vfs_node_t *node, uint32_t off, uint32_t len,
                            uint8_t *buf) {
    tmpfs_node_t *tn = (tmpfs_node_t *)node;
    if (!tn->data || off >= node->size) return 0;
    if (off + len > node->size) len = node->size - off;
    memcpy(buf, tn->data + off, len);
    return len;
}

static uint32_t tmpfs_write(vfs_node_t *node, uint32_t off, uint32_t len,
                              const uint8_t *buf) {
    tmpfs_node_t *tn = (tmpfs_node_t *)node;
    uint32_t end = off + len;

    /* Grow buffer if needed */
    if (end > tn->capacity) {
        uint32_t newcap = tn->capacity ? tn->capacity : 64;
        while (newcap < end) newcap *= 2;
        uint8_t *newbuf = (uint8_t *)kmalloc(newcap);
        if (!newbuf) return VFS_WRITE_ENOMEM;   /* → write() gets -ENOMEM */
        if (tn->data) {
            memcpy(newbuf, tn->data, tn->capacity);
            kfree(tn->data);
        }
        /* zero the gap between old size and the write start */
        if (off > (tn->capacity)) {
            memset(newbuf + tn->capacity, 0, off - tn->capacity);
        }
        tn->data     = newbuf;
        tn->capacity = newcap;
    }

    memcpy(tn->data + off, buf, len);
    if (end > node->size) node->size = end;
    return len;
}

static int tmpfs_truncate(vfs_node_t *node, uint32_t new_size) {
    tmpfs_node_t *tn = (tmpfs_node_t *)node;

    if (new_size == 0) {
        if (tn->data) { kfree(tn->data); tn->data = NULL; }
        tn->capacity = 0;
        node->size   = 0;
        return 0;
    }

    if (new_size <= node->size) {
        /* Shrink: zero the tail */
        if (tn->data) memset(tn->data + new_size, 0, node->size - new_size);
        node->size = new_size;
        return 0;
    }

    /* Extend: ensure capacity */
    if (new_size > tn->capacity) {
        uint32_t newcap = tn->capacity ? tn->capacity : 64;
        while (newcap < new_size) newcap *= 2;
        uint8_t *newbuf = (uint8_t *)kmalloc(newcap);
        if (!newbuf) return -12;        /* -ENOMEM, see sys_ftruncate */
        if (tn->data) {
            memcpy(newbuf, tn->data, node->size);
            kfree(tn->data);
        }
        memset(newbuf + node->size, 0, newcap - node->size);
        tn->data     = newbuf;
        tn->capacity = newcap;
    }
    node->size = new_size;
    return 0;
}

/* ── Directory operations ─────────────────────────────────────────────────── */

static vfs_node_t *tmpfs_finddir(vfs_node_t *node, const char *name) {
    tmpfs_node_t *dir = (tmpfs_node_t *)node;
    for (tmpfs_node_t *c = dir->first_child; c; c = (tmpfs_node_t *)c->vnode.next)
        if (strcmp(c->vnode.name, name) == 0)
            return &c->vnode;
    return NULL;
}

static int tmpfs_readdir(vfs_node_t *node, uint32_t idx, vfs_dirent_t *out) {
    tmpfs_node_t *dir = (tmpfs_node_t *)node;
    uint32_t i = 0;
    for (tmpfs_node_t *c = dir->first_child; c;
         c = (tmpfs_node_t *)c->vnode.next, i++) {
        if (i == idx) {
            out->ino  = c->vnode.inode;
            out->type = (uint8_t)c->vnode.flags;
            strncpy(out->name, c->vnode.name, 255);
            out->name[255] = '\0';
            return 0;
        }
    }
    return -1;
}

static int tmpfs_create(vfs_node_t *dir_node, const char *name, uint32_t flags) {
    tmpfs_node_t *dir = (tmpfs_node_t *)dir_node;

    /* Reject if already exists — MUST be -EEXIST (-17), not -EPERM (-1).
     * Callers like Firefox's nsLocalFile::Create / mkdir -p treat EEXIST as
     * success ("dir already there") but any other errno as FATAL — returning -1
     * made Firefox's profile-dir creation fail intermittently → "profile cannot
     * be loaded" modal that hangs startup. */
    if (tmpfs_finddir(dir_node, name)) return -17;

    tmpfs_node_t *child = alloc_tmpfs_node(name, flags);
    if (!child) return -12;   /* -ENOMEM */

    /* Prepend to children list using vnode.next as link */
    child->vnode.next = (vfs_node_t *)dir->first_child;
    dir->first_child  = child;
    return 0;
}

static int tmpfs_unlink(vfs_node_t *dir_node, const char *name) {
    tmpfs_node_t *dir = (tmpfs_node_t *)dir_node;
    tmpfs_node_t *prev = NULL;
    for (tmpfs_node_t *c = dir->first_child; c;
         c = (tmpfs_node_t *)c->vnode.next) {
        if (strcmp(c->vnode.name, name) == 0) {
            /* A directory must be empty before its name can go away. */
            if ((c->vnode.flags & VFS_FLAG_DIR) && c->first_child)
                return -39;                  /* -ENOTEMPTY */
            /* Detach from the list, then drop the directory's reference.  The
             * node survives if a descriptor or a mapping still holds one. */
            if (prev)
                prev->vnode.next = c->vnode.next;
            else
                dir->first_child = (tmpfs_node_t *)c->vnode.next;
            c->vnode.next = NULL;
            tmpfs_release(&c->vnode);
            return 0;
        }
        prev = c;
    }
    return -2;  /* -ENOENT */
}

/* ── Public API ────────────────────────────────────────────────────────────── */

vfs_node_t *tmpfs_mount(void) {
    tmpfs_node_t *root = alloc_tmpfs_node("/", VFS_FLAG_DIR);
    if (!root) return NULL;
    return &root->vnode;
}

static int tmpfs_symlink(vfs_node_t *dir, const char *name, const char *target) {
    if (!dir || !dir->create_fn) return -1;
    if (tmpfs_finddir(dir, name)) return -17;
    /* Create a symlink node in the directory */
    tmpfs_node_t *tn = alloc_tmpfs_node(name, VFS_FLAG_SYMLINK);
    if (!tn) return -12;

    /* Store target path in data buffer */
    uint32_t tlen = (uint32_t)strlen(target);
    tn->data = (uint8_t *)kmalloc(tlen + 1);
    if (!tn->data) { kfree(tn); return -12; }
    memcpy(tn->data, target, tlen + 1);
    tn->vnode.size = tlen;
    tn->capacity   = tlen + 1;

    /* Link into parent directory */
    tmpfs_node_t *tdir = (tmpfs_node_t *)dir;
    tn->vnode.next = (vfs_node_t *)tdir->first_child;
    tdir->first_child = tn;
    return 0;
}
