#include "vfs.h"
#include "tmpfs.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../proc/process.h"
#include "../lib/printf.h"
#include "../proc/scheduler.h"
#include "inotify.h"
#include <stddef.h>

vfs_node_t *vfs_root = NULL;
static vfs_node_t *vfs_root_overlay = NULL;

void vfs_init(void) {
    /* root is populated by initrd_init or ext2_mount */
}

/* ── Low-level node operations ────────────────────────────────────────────── */

uint32_t vfs_read(vfs_node_t *node, uint64_t offset, uint32_t size,
                  uint8_t *buf) {
    if (!node) return 0;
    if (node->read_fn)
        return node->read_fn(node, offset, size, buf);
    /* Default: initrd in-memory data */
    if (!(node->flags & VFS_FLAG_FILE) || !node->data) return 0;
    if (offset >= node->size) return 0;
    if (size > node->size - offset) size = (uint32_t)(node->size - offset);
    memcpy(buf, node->data + (uint32_t)offset, size);
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

uint32_t vfs_write(vfs_node_t *node, uint64_t offset, uint32_t size,
                   const uint8_t *buf) {
    if (!node || !node->write_fn) return 0;
    return node->write_fn(node, offset, size, buf);
}

int vfs_create(vfs_node_t *dir, const char *name, uint32_t flags) {
    if (!dir || !dir->create_fn) return -1;
    int r = dir->create_fn(dir, name, flags);
    if (r == 0)
        inotify_dir_event(dir, IN_CREATE | (flags == VFS_FLAG_DIR ? IN_ISDIR : 0), 0, name);
    return r;
}

int vfs_truncate(vfs_node_t *node, uint64_t new_size) {
    if (!node || !node->truncate_fn) return -1;
    int r = node->truncate_fn(node, new_size);
    if (r == 0) inotify_child_event(node, NULL, IN_MODIFY);
    return r;
}

int vfs_unlink(vfs_node_t *dir, const char *name) {
    if (!dir || !dir->unlink_fn) return -1;
    /* inotify: the entry being removed, looked up before it goes (the
     * pointer is only compared afterwards; a watched node is kept alive by
     * its watch's reference). */
    vfs_node_t *child = inotify_nwatches ? vfs_finddir(dir, name) : NULL;
    uint32_t isdir = child && child->flags == VFS_FLAG_DIR ? IN_ISDIR : 0;
    int last = child && (isdir || child->nlink <= 1);
    int r = dir->unlink_fn(dir, name);
    if (r == 0 && inotify_nwatches) {
        inotify_dir_event(dir, IN_DELETE | isdir, 0, name);
        if (last) inotify_node_gone(child);
    }
    return r;
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
    return vfs_access_check_groups(node, euid, egid, NULL, 0, want);
}

int vfs_access_check_groups(vfs_node_t *node, uint32_t euid, uint32_t egid,
                            const uint32_t *groups, uint32_t ngroups, int want) {
    uint32_t m, bits;
    int in_group;

    if (!node) return -2;            /* -ENOENT */
    node = vfs_perm_node(node);      /* shared permissions (perm_of) */
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
    in_group = (egid == node->gid);
    for (uint32_t i = 0; !in_group && i < ngroups; i++)
        in_group = (groups[i] == node->gid);
    bits = (euid == node->uid) ? (m >> 6)
         : in_group ? (m >> 3)
         : m;
    return (((int)bits & 7 & want) == want) ? 0 : -13;   /* -EACCES */
}

int vfs_setattr_quiet(vfs_node_t *node, uint32_t mode, uint32_t uid, uint32_t gid) {
    if (!node) return -2;
    node = vfs_perm_node(node);
    node->mask = mode & 07777;
    node->uid = uid;
    node->gid = gid;
    return node->setattr_fn ? node->setattr_fn(node, node->mask, uid, gid) : 0;
}

int vfs_setattr(vfs_node_t *node, uint32_t mode, uint32_t uid, uint32_t gid) {
    if (!node) return -2;
    node = vfs_perm_node(node);
    vfs_node_t *parent = NULL;
    char name[256];
    int haveparent = inotify_nwatches && vfs_last_parent(node, &parent, name);
    node->mask = mode & 07777;
    node->uid = uid;
    node->gid = gid;
    int r = node->setattr_fn ? node->setattr_fn(node, node->mask, uid, gid) : 0;
    if (r == 0 && inotify_nwatches) {
        uint32_t m = IN_ATTRIB | (node->flags == VFS_FLAG_DIR ? IN_ISDIR : 0);
        inotify_self_event(node, m);
        if (haveparent) inotify_parent_event(parent, m, name);
    }
    return r;
}

int vfs_settimes(vfs_node_t *node, uint32_t atime, uint32_t mtime) {
    if (!node) return -2;
    vfs_node_t *parent = NULL;
    char name[256];
    int haveparent = inotify_nwatches && vfs_last_parent(node, &parent, name);
    node->atime = atime;
    node->mtime = mtime;
    int r = node->settimes_fn ? node->settimes_fn(node, atime, mtime) : 0;
    if (r == 0 && inotify_nwatches) {
        uint32_t m = IN_ATTRIB | (node->flags == VFS_FLAG_DIR ? IN_ISDIR : 0);
        inotify_self_event(node, m);
        if (haveparent) inotify_parent_event(parent, m, name);
    }
    return r;
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

static vfs_node_t *mnt_resolve(vfs_node_t *dir);

/* The last path walk's final component (inotify: a file's directory and
 * name for the events on it).  Only kept while watches exist. */
static struct {
    struct proc *who;
    vfs_node_t  *parent, *node;
    char         name[256];
} last_lookup;

void vfs_forget_last_lookup(void) {
    last_lookup.who = NULL;
    last_lookup.parent = last_lookup.node = NULL;
}

int vfs_last_parent(vfs_node_t *node, vfs_node_t **parent, char *name) {
    if (!node || last_lookup.node != node || last_lookup.who != current_proc ||
        !last_lookup.parent)
        return 0;
    *parent = last_lookup.parent;
    memcpy(name, last_lookup.name, sizeof(last_lookup.name));
    return 1;
}

/* Linux's MAXSYMLINKS: symlinks one lookup may follow before -ELOOP. */
#define VFS_MAXSYMLINKS  40
#define VFS_PATH_MAX     512

enum { WALK_FOUND, WALK_MISS, WALK_RESTART };

/*
 * Walk `path` from `root`.  Symlinks are never followed by recursing: on one,
 * the walk writes the path to resolve instead into `alt` (the link target,
 * prefixed with the directory holding the link when the target is relative,
 * followed by the components not yet walked) and returns WALK_RESTART.  The
 * caller then starts over on `alt`, so a lookup costs one stack frame however
 * many links it crosses, and one budget bounds all of them: with `may_follow`
 * clear, a link that would have to be followed fails the walk with -ELOOP.
 *
 * While walking, `alt` holds the textual path of the directory reached so far;
 * that is the prefix a relative target is resolved against.
 */
static vfs_mnt_t g_mnt[VFS_MNT_MAX];
static int g_mnt_active;          /* entries that are crossed (not boot notes) */
static uint32_t g_mnt_seq;        /* last mount number handed out */

/* The newest mount whose mountpoint is `n` (by mount order, not by slot: a
 * slot freed by umount is reused by a later mount). */
static vfs_mnt_t *mnt_on(vfs_node_t *n) {
    vfs_mnt_t *best = NULL;
    for (int i = 0; i < VFS_MNT_MAX; i++)
        if (g_mnt[i].used && !g_mnt[i].boot && g_mnt[i].mp == n &&
            (!best || g_mnt[i].seq > best->seq))
            best = &g_mnt[i];
    return best;
}

/* Step from a mountpoint to the root mounted on it, through stacked mounts. */
static vfs_node_t *mnt_cross(vfs_node_t *n, vfs_mnt_t **m) {
    if (!g_mnt_active) return n;
    for (int hops = 0; hops < VFS_MNT_MAX && n; hops++) {
        vfs_mnt_t *e = mnt_on(n);
        if (!e) break;
        *m = e;
        n = e->root;
    }
    return n;
}

static int vfs_walk(vfs_node_t *root, const char *path, int follow_final,
                    int may_follow, int top_restarts, char *alt,
                    vfs_node_t **out, int *err, vfs_mnt_t **mnt_out) {
    const char *p = path + 1;
    vfs_node_t *cur = root;
    vfs_node_t *parents[64];
    vfs_mnt_t *mnts[64];
    vfs_mnt_t *curm = NULL;
    uint32_t depth = 0;
    int alen = 0;                /* length of the prefix in alt; -1: too long */
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
        /* A name over 255 bytes is an error, not two components. */
        if (*p && *p != '/') { *err = -36; return WALK_MISS; } /* -ENAMETOOLONG */
        const char *after_component = p;
        while (*after_component == '/') after_component++;
        int is_final = (*after_component == '\0');

        /* handle "." */
        if (len == 1 && component[0] == '.') continue;

        /* handle ".." — walk up (root stays root) */
        if (len == 2 && component[0] == '.' && component[1] == '.') {
            if (depth > 0) {
                --depth;
                cur = parents[depth];
                curm = mnts[depth];
            } else {
                cur = root;
                curm = NULL;
            }
            /* A chrooted walk through /dev or /proc that climbs back to the
             * top is at the chroot's "/", not the global one: start over
             * with the rest of the path there (a symlink to /proc/.. must
             * not lead out of the root). */
            if (top_restarts && depth == 0) {
                uint32_t rlen = (uint32_t)strlen(after_component);
                if (rlen + 2 > VFS_PATH_MAX) { *err = -36; return WALK_MISS; }
                alt[0] = '/';
                memcpy(alt + 1, after_component, rlen + 1);
                return WALK_RESTART;
            }
            if (alen > 0) {
                while (alen > 0 && alt[alen - 1] != '/') alen--;
                if (alen > 0) alen--;
            }
            continue;
        }

        vfs_node_t *parent = cur;
        vfs_mnt_t *parent_m = curm;
        cur = vfs_finddir(cur, component);
        if (!cur) { *err = -2; return WALK_MISS; }            /* -ENOENT */
        cur = mnt_cross(cur, &curm);

        if (cur->flags == VFS_FLAG_SYMLINK && (!is_final || follow_final)) {
            if (!may_follow) { *err = -40; return WALK_MISS; } /* -ELOOP */
            char target[256];
            /* A target that does not fit would be followed truncated. */
            if (cur->size >= sizeof(target)) { *err = -36; return WALK_MISS; }
            /* A procfs link may refuse the caller (-EACCES as a negative
             * count: /proc/<pid>/cwd of another user's process). */
            int32_t tl = (int32_t)vfs_read(cur, 0, sizeof(target) - 1,
                                           (uint8_t *)target);
            if (tl <= 0) { *err = tl < 0 ? tl : -2; return WALK_MISS; }
            uint32_t tlen = (uint32_t)tl;
            if (tlen > sizeof(target) - 1) tlen = sizeof(target) - 1;
            target[tlen] = '\0';
            if (target[0] == '/') alen = 0;
            uint32_t rlen = (uint32_t)strlen(after_component);
            /* prefix + '/' + target + '/' + rest + NUL */
            if (alen < 0 ||
                (uint32_t)alen + 1 + tlen + 1 + rlen + 1 > VFS_PATH_MAX) {
                *err = -36;                                   /* -ENAMETOOLONG */
                return WALK_MISS;
            }
            if (target[0] != '/') alt[alen++] = '/';
            memcpy(alt + alen, target, tlen);
            alen += (int)tlen;
            if (rlen) {
                alt[alen++] = '/';
                memcpy(alt + alen, after_component, rlen);
                alen += (int)rlen;
            }
            alt[alen] = '\0';
            return WALK_RESTART;
        }

        if (is_final && inotify_nwatches) {
            last_lookup.who = current_proc;
            last_lookup.parent = mnt_resolve(parent);
            last_lookup.node = cur;
            memcpy(last_lookup.name, component, (uint32_t)len + 1);
        }

        if (depth < (sizeof(parents) / sizeof(parents[0]))) {
            mnts[depth] = parent_m;
            parents[depth++] = parent;
        }
        if (alen >= 0 && alen + 1 + len < VFS_PATH_MAX) {
            alt[alen++] = '/';
            memcpy(alt + alen, component, (uint32_t)len);
            alen += len;
        } else {
            alen = -1;
        }
    }
    *out = cur;
    if (mnt_out) *mnt_out = curm;
    return WALK_FOUND;
}

/*
 * Paths a chrooted process still resolves globally: /dev and /proc are not
 * bind-mounted into the new root, so they are passed through instead.
 */
static int vfs_path_skips_chroot(const char *path) {
    char first[256];
    if (path_first_component(path, first) <= 0) return 0;
    return strcmp(first, "dev") == 0 || strcmp(first, "proc") == 0;
}

/*
 * Resolve an absolute path.  With `croot` set (a chrooted caller) the walk
 * starts there, except under /dev and /proc; otherwise paths outside the
 * reserved mount points are tried on the root overlay (the mounted disk)
 * first and on vfs_root otherwise.  Mounts are crossed either way, and ".."
 * at the top of a chroot stays there, so a mountpoint inside the new root
 * leads only into its mount and back.  On failure NULL is returned and *err
 * (if given) says why: -ENOENT, -ELOOP past VFS_MAXSYMLINKS links, or
 * -ENAMETOOLONG.  *mnt (if given) gets the mount the node was reached
 * through (NULL for the boot filesystems).
 */
static vfs_node_t *vfs_lookup_in(vfs_node_t *croot, const char *path,
                                 int follow_final, int *err, vfs_mnt_t **mnt) {
    char bufs[2][VFS_PATH_MAX];
    if (mnt) *mnt = NULL;
    if (err) *err = -2;                                       /* -ENOENT */
    if (!path || path[0] != '/' || !vfs_root) return NULL;
    uint32_t plen = (uint32_t)strlen(path);
    if (plen >= VFS_PATH_MAX) { if (err) *err = -36; return NULL; }
    memcpy(bufs[0], path, plen + 1);

    int cur = 0, links = 0;
    for (;;) {
        char *pth = bufs[cur], *alt = bufs[cur ^ 1];
        int may_follow = links < VFS_MAXSYMLINKS;
        vfs_node_t *node = NULL;
        int r = WALK_MISS, e = -2, e2 = -2;
        if (croot && !vfs_path_skips_chroot(pth)) {
            /* ".." at the top stays at croot, and an absolute symlink target
             * comes back through here, so neither leaves the new root. */
            r = vfs_walk(croot, pth, follow_final, may_follow, 0, alt, &node,
                         &e, mnt);
        } else {
            if (vfs_root_overlay && vfs_path_uses_root_overlay(pth))
                r = vfs_walk(vfs_root_overlay, pth, follow_final, may_follow,
                             0, alt, &node, &e, mnt);
            if (r == WALK_MISS) {
                r = vfs_walk(vfs_root, pth, follow_final, may_follow,
                             croot != NULL, alt, &node, &e2, mnt);
                if (e == -2) e = e2;     /* report a loop over a plain miss */
            }
        }
        if (r == WALK_FOUND) return node;
        if (r == WALK_MISS) {
            if (err) *err = e;
            return NULL;
        }
        links++;
        cur ^= 1;
    }
}

vfs_node_t *vfs_lookup_from(vfs_node_t *croot, const char *path,
                            int follow_final, int *err, vfs_mnt_t **mnt) {
    return vfs_lookup_in(croot, path, follow_final, err, mnt);
}

/* A chrooted process walks from the node sys_chroot pinned. */
vfs_node_t *vfs_lookup_mnt(const char *path, int follow_final, int *err,
                           vfs_mnt_t **mnt) {
    struct proc *p = current_proc;
    return vfs_lookup_in(p ? p->root_node : NULL, path, follow_final, err, mnt);
}

vfs_node_t *vfs_lookup(const char *path, int follow_final, int *err) {
    return vfs_lookup_mnt(path, follow_final, err, NULL);
}

vfs_node_t *vfs_open(const char *path) {
    return vfs_lookup(path, 1, NULL);
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

/* ── Rename ───────────────────────────────────────────────────────────────── */

/* The directory a mount-point shim stands for (the shim itself otherwise). */
static vfs_node_t *mnt_resolve(vfs_node_t *dir) {
    while (dir && dir->finddir_fn == mnt_finddir && dir->private)
        dir = (vfs_node_t *)dir->private;
    return dir;
}

const void *vfs_mnt_instance(const vfs_mnt_t *m) {
    /* A bind mount ("none") shows a directory of the mount in src. */
    for (int hops = 0; m && hops < VFS_MNT_MAX; hops++) {
        if (strcmp(m->fstype, "none") != 0)
            return m->fs ? m->fs : (const void *)m->root;
        m = m->src;
    }
    return NULL;
}

const void *vfs_path_instance(const char *path, int *ok) {
    int err;
    vfs_mnt_t *m = NULL;
    *ok = vfs_lookup_mnt(path, 1, &err, &m) != NULL;
    return vfs_mnt_instance(m);
}

int vfs_rename(vfs_node_t *old_dir, const char *old_name,
               vfs_node_t *new_dir, const char *new_name) {
    old_dir = mnt_resolve(old_dir);
    new_dir = mnt_resolve(new_dir);
    if (!old_dir || !new_dir) return -2;               /* -ENOENT */
    if (!old_dir->rename_fn) return -1;                /* -EPERM */
    if (new_dir->rename_fn != old_dir->rename_fn) return -18;   /* -EXDEV */
    vfs_node_t *moved = NULL, *victim = NULL;
    if (inotify_nwatches) {
        moved = vfs_finddir(old_dir, old_name);
        victim = vfs_finddir(new_dir, new_name);
        if (victim == moved) victim = NULL;
    }
    /* Everything about the replaced node is read now: the rename may free
     * it (only its address is compared afterwards). */
    uint32_t isdir = moved && moved->flags == VFS_FLAG_DIR ? IN_ISDIR : 0;
    int victim_last = victim && (victim->flags == VFS_FLAG_DIR || victim->nlink <= 1);
    int r = old_dir->rename_fn(old_dir, old_name, new_dir, new_name);
    if (r == 0 && inotify_nwatches) {
        uint32_t cookie = inotify_next_cookie();
        inotify_dir_event(old_dir, IN_MOVED_FROM | isdir, cookie, old_name);
        inotify_dir_event(new_dir, IN_MOVED_TO | isdir, cookie, new_name);
        if (moved) inotify_self_event(moved, IN_MOVE_SELF);
        if (victim_last) inotify_node_gone(victim);
    }
    return r;
}

int vfs_link(vfs_node_t *dir, const char *name, vfs_node_t *target) {
    dir = mnt_resolve(dir);
    if (!dir || !target) return -2;                    /* -ENOENT */
    if (!dir->link_fn) return -1;                      /* -EPERM */
    if (target->link_fn != dir->link_fn) return -18;   /* -EXDEV */
    int r = dir->link_fn(dir, name, target);
    if (r == 0) {
        inotify_dir_event(dir, IN_CREATE, 0, name);
        inotify_self_event(target, IN_ATTRIB);
    }
    return r;
}

vfs_node_t *vfs_resolve_mount(vfs_node_t *dir) {
    return mnt_resolve(dir);
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
        /* Truncating would create the link in whatever directory the cut
         * prefix happens to name. */
        if (slash > 255) return -36;                          /* -ENAMETOOLONG */
        memcpy(dir_path, path, (uint32_t)slash);
        dir_path[slash] = '\0';
    }
    int blen = plen - slash - 1;
    if (blen <= 0 || blen > 255) return -22;
    memcpy(name, path + slash + 1, (uint32_t)blen);
    name[blen] = '\0';

    vfs_node_t *dir = vfs_open(dir_path);
    if (!dir) return -2;
    if (!dir->symlink_fn) return -38;

    int r = dir->symlink_fn(dir, name, target);
    if (r == 0) inotify_dir_event(dir, IN_CREATE, 0, name);
    return r;
}

/* ── No-follow path resolution ────────────────────────────────────────────── */

vfs_node_t *vfs_open_nofollow(const char *path) {
    return vfs_lookup(path, 0, NULL);
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

/* ── Mount table ──────────────────────────────────────────────────────────── */

static void mnt_copy_str(char *dst, const char *src, uint32_t size) {
    strncpy(dst, src ? src : "none", size - 1);
    dst[size - 1] = '\0';
}

int vfs_mount_add(const char *target, vfs_node_t *root, const vfs_mnt_t *tmpl) {
    if (!target || target[0] != '/' || !root || !tmpl) return -22;
    int err;
    vfs_mnt_t *pm = NULL;
    vfs_node_t *mp = vfs_lookup_mnt(target, 1, &err, &pm);
    if (!mp) return err;
    if (mp->flags != VFS_FLAG_DIR) return -20;                /* -ENOTDIR */
    /* "/" itself is not a node any walk crosses from. */
    if (mp == vfs_root || mp == vfs_root_overlay) return -16; /* -EBUSY */
    /* The table keeps raw pointers: neither end may be a node that goes
     * once nothing holds it (per-process /proc nodes: -EINVAL). */
    int pr = vfs_may_pin(mp, VFS_PIN_MOUNT);
    if (!pr) pr = vfs_may_pin(root, VFS_PIN_MOUNT);
    if (pr) return pr;
    preempt_disable();
    vfs_mnt_t *m = NULL;
    for (int i = 0; i < VFS_MNT_MAX; i++)
        if (!g_mnt[i].used) { m = &g_mnt[i]; break; }
    if (!m) { preempt_enable(); return -12; }                 /* -ENOMEM */
    *m = *tmpl;
    m->used   = 1;
    m->boot   = 0;
    m->mp     = mp;
    m->root   = root;
    m->parent = pm;
    m->seq    = ++g_mnt_seq;
    mnt_copy_str(m->target, target, sizeof(m->target));
    mnt_copy_str(m->source, tmpl->source, sizeof(m->source));
    mnt_copy_str(m->fstype, tmpl->fstype, sizeof(m->fstype));
    g_mnt_active++;
    preempt_enable();
    return 0;
}

vfs_mnt_t *vfs_mount_find(const char *path, int *err) {
    /* The walk reports the mount it crossed last to reach the result: that
     * is the one `path` names, even when two mounts share a root node (a
     * bind of a mounted root). */
    vfs_mnt_t *m = NULL;
    vfs_node_t *n = vfs_lookup_mnt(path, 1, err, &m);
    if (!n) return NULL;
    if (m && m->used && !m->boot && m->root == n) return m;
    if (err) *err = -22;                                      /* -EINVAL */
    return NULL;
}

int vfs_mounts_active(void) {
    return g_mnt_active;
}

int vfs_mnt_rdonly(const vfs_mnt_t *m, uint32_t seq) {
    return m && m->used && !m->boot && m->seq == seq &&
           (m->flags & VFS_MS_RDONLY);
}

/* Instances detached by a lazy umount while still busy.  Their descriptors
 * keep reading and writing the device, so the device stays taken: it cannot
 * be mounted again, or written through /dev, until the last one closes.  The
 * instance is released when a later check finds it idle. */
typedef struct {
    int    used;
    char   source[64];
    void  *fs;
    int  (*busy)(void *fs);
    void (*release)(void *fs);
    int  (*set_ro)(void *fs, int ro);   /* poweroff: flush and mark clean */
    int    ro;
} vfs_detached_t;
static vfs_detached_t g_detached[VFS_MNT_MAX];

/* Release every detached instance that has gone idle. */
static void detached_reap(void) {
    for (int i = 0; i < VFS_MNT_MAX; i++) {
        void (*release)(void *) = NULL;
        void *fs = NULL;
        preempt_disable();
        vfs_detached_t *d = &g_detached[i];
        if (d->used && !(d->busy && d->busy(d->fs))) {
            release = d->release;
            fs = d->fs;
            d->used = 0;
        }
        preempt_enable();
        if (release) release(fs);
    }
}

int vfs_mount_remove(vfs_mnt_t *m, uint32_t flags) {
    if (!m || !m->used || m->boot) return -22;
    /* Busy while a mount sits inside it, or a bind mount shows one of its
     * directories elsewhere. */
    for (int i = 0; i < VFS_MNT_MAX; i++)
        if (g_mnt[i].used && (g_mnt[i].parent == m || g_mnt[i].src == m))
            return -16;                                           /* -EBUSY */
    int busy = m->busy ? m->busy(m->fs) : 0;
    if (busy && !(flags & VFS_MNT_DETACH)) return -16;            /* -EBUSY */
    void (*release)(void *) = m->release;
    void *fs = m->fs;
    preempt_disable();
    m->used = 0;
    m->mp = m->root = NULL;
    m->src = NULL;
    g_mnt_active--;
    preempt_enable();
    /* A lazily detached instance that still has open files is left alive:
     * its descriptors keep working, and it keeps its device until it is
     * idle (see g_detached). */
    if (busy) {
        preempt_disable();
        for (int i = 0; i < VFS_MNT_MAX; i++) {
            vfs_detached_t *d = &g_detached[i];
            if (d->used) continue;
            d->used = 1;
            mnt_copy_str(d->source, m->source, sizeof(d->source));
            d->fs = fs;
            d->busy = m->busy;
            d->release = release;
            d->set_ro = m->set_ro;
            d->ro = (m->flags & VFS_MS_RDONLY) != 0;
            break;
        }
        preempt_enable();
    } else if (release) {
        release(fs);
    }
    return 0;
}

void vfs_mounts_shutdown(void) {
    for (int i = 0; i < VFS_MNT_MAX; i++) {
        vfs_mnt_t *m = &g_mnt[i];
        if (m->used && !m->boot && m->set_ro && !(m->flags & VFS_MS_RDONLY)) {
            m->set_ro(m->fs, 1);
            m->flags |= VFS_MS_RDONLY;
        }
        /* A lazily unmounted instance still in use writes too. */
        vfs_detached_t *d = &g_detached[i];
        if (d->used && d->set_ro && !d->ro) {
            d->set_ro(d->fs, 1);
            d->ro = 1;
        }
    }
}

int vfs_is_mountpoint(vfs_node_t *n) {
    return g_mnt_active && n && mnt_on(n) != NULL;
}

void vfs_mount_note(const char *source, const char *target, const char *fstype,
                    uint32_t flags) {
    for (int i = 0; i < VFS_MNT_MAX; i++) {
        if (g_mnt[i].used) continue;
        memset(&g_mnt[i], 0, sizeof(g_mnt[i]));
        g_mnt[i].used = 1;
        g_mnt[i].boot = 1;
        g_mnt[i].flags = flags;
        mnt_copy_str(g_mnt[i].source, source, sizeof(g_mnt[i].source));
        mnt_copy_str(g_mnt[i].target, target, sizeof(g_mnt[i].target));
        mnt_copy_str(g_mnt[i].fstype, fstype, sizeof(g_mnt[i].fstype));
        return;
    }
}

int vfs_mount_has_fs_source(const char *source) {
    detached_reap();
    for (int i = 0; i < VFS_MNT_MAX; i++) {
        if (g_mnt[i].used && strcmp(g_mnt[i].source, source) == 0) return 1;
        if (g_detached[i].used && strcmp(g_detached[i].source, source) == 0) return 1;
    }
    return 0;
}

uint32_t vfs_mounts_format(char *buf, uint32_t size) {
    uint32_t pos = 0;
    if (!size) return 0;
    buf[0] = '\0';
    /* Boot notes first (slot order), then mounts in the order they were
     * made, so a parent is always listed before what is mounted inside it. */
    uint32_t last = 0;
    for (int i = 0; ; i++) {
        vfs_mnt_t *m = NULL;
        if (i < VFS_MNT_MAX) {
            if (!g_mnt[i].used || !g_mnt[i].boot) continue;
            m = &g_mnt[i];
        } else {
            for (int j = 0; j < VFS_MNT_MAX; j++)
                if (g_mnt[j].used && !g_mnt[j].boot && g_mnt[j].seq > last &&
                    (!m || g_mnt[j].seq < m->seq))
                    m = &g_mnt[j];
            if (!m) break;
            last = m->seq;
        }
        if (pos + 1 >= size) break;
        pos += (uint32_t)snprintf(buf + pos, size - pos,
                                  "%s %s %s %s%s%s%s 0 0\n",
                                  m->source, m->target, m->fstype,
                                  (m->flags & VFS_MS_RDONLY) ? "ro" : "rw",
                                  (m->flags & VFS_MS_NOSUID) ? ",nosuid" : "",
                                  (m->flags & VFS_MS_NODEV) ? ",nodev" : "",
                                  (m->flags & VFS_MS_NOEXEC) ? ",noexec" : "");
        if (pos >= size) pos = size - 1;
    }
    return pos;
}

/* /proc/<pid>/mountinfo: "id parent major:minor root mountpoint options
 * - fstype source superoptions", ids 20 + slot (1 for the boot root). */
uint32_t vfs_mountinfo_format(char *buf, uint32_t size) {
    uint32_t pos = 0;
    if (!size) return 0;
    buf[0] = '\0';
    for (int i = 0; i < VFS_MNT_MAX; i++) {
        vfs_mnt_t *m = &g_mnt[i];
        if (!m->used) continue;
        if (pos + 1 >= size) break;
        int parent = m->parent ? 20 + (int)(m->parent - g_mnt) : 1;
        if (!strcmp(m->target, "/")) parent = 1;
        pos += (uint32_t)snprintf(buf + pos, size - pos,
                                  "%d %d 0:%d / %s %s%s%s%s - %s %s %s\n",
                                  20 + i, parent, 20 + i, m->target,
                                  (m->flags & VFS_MS_RDONLY) ? "ro" : "rw",
                                  (m->flags & VFS_MS_NOSUID) ? ",nosuid" : "",
                                  (m->flags & VFS_MS_NODEV) ? ",nodev" : "",
                                  (m->flags & VFS_MS_NOEXEC) ? ",noexec" : "",
                                  m->fstype, m->source,
                                  (m->flags & VFS_MS_RDONLY) ? "ro" : "rw");
        if (pos >= size) pos = size - 1;
    }
    return pos;
}

int vfs_path_rdonly(const char *path, int parent) {
    if (!g_mnt_active || !path || path[0] != '/') return 0;
    vfs_mnt_t *m = NULL;
    int err;
    if (!parent && vfs_lookup_mnt(path, 1, &err, &m))
        return m && (m->flags & VFS_MS_RDONLY);
    /* The directory that holds (or would hold) the last component. */
    char dir[VFS_PATH_MAX];
    uint32_t len = (uint32_t)strlen(path);
    if (len >= sizeof(dir)) return 0;
    memcpy(dir, path, len + 1);
    while (len > 1 && dir[len - 1] == '/') dir[--len] = '\0';
    while (len > 1 && dir[len - 1] != '/') len--;
    if (len > 1) len--;                       /* drop the separator */
    dir[len] = '\0';
    m = NULL;
    if (!vfs_lookup_mnt(dir, 1, &err, &m)) return 0;
    return m && (m->flags & VFS_MS_RDONLY);
}
