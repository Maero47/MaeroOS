#include "tmpfs.h"
#include "vfs.h"
#include "../mm/heap.h"
#include "../lib/string.h"
#include "kernel/config.h"
#include "../mm/pmm.h"
#include "../proc/pipe.h"
#include "../proc/scheduler.h"
#include "../arch/i686/mm/paging.h"
#include "../arch/i686/cpu/pit.h"
#include "../drivers/rtc.h"
#include <stddef.h>
#include <stdint.h>

/*
 * tmpfs — in-memory writable filesystem.
 *
 * Each node is a tmpfs_node_t (with the vfs_node_t as its first field so
 * it can be cast freely between the two types).  A file's body is an array of
 * physical page frames (Linux shmem keeps its pages in the page cache the same
 * way); symlinks keep their short target in a heap buffer; directories carry a
 * singly-linked list of child tmpfs_node_t.
 *
 * File bodies used to be one contiguous kmalloc() buffer that doubled as the
 * file grew.  Firefox keeps its whole profile in /tmp - the HTTP cache,
 * places.sqlite and its journal, the session store it rewrites every few
 * seconds - so browsing real sites drove the kernel heap to the end of its
 * 256 MiB window within the first page load ("[OOM] out of kernel heap
 * address space (0xe0000000)" on every --sites run): a growing file needed a
 * new contiguous block twice its size while the old one was still live, and
 * the heap never gives address space back.  Frames come from the PMM (2 GiB
 * of RAM, most of it outside any kernel window) and are reached through the
 * second temp-map slot, so a file now costs one frame per written page plus
 * four heap bytes per page for the frame array.
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
    /* file backing: one physical frame per page, 0 = a hole (reads as zero) */
    uint32_t           *pages;
    uint32_t            npages;     /* length of pages[] */
    /* symlink target (heap), NUL-terminated */
    uint8_t            *data;
    /* directory children */
    struct tmpfs_node  *first_child;/* singly-linked list (youngest first) */
    /* The directory this node is linked in (NULL for a root, or once
     * unlinked).  Only directories' ancestry is ever walked: rename uses it
     * to refuse moving a directory below itself.  A directory can only be
     * unlinked while empty, so no child ever points at a freed parent. */
    struct tmpfs_node  *parent;
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
static int               tmpfs_rename  (vfs_node_t *, const char *,
                                        vfs_node_t *, const char *);
static void              tmpfs_retain  (vfs_node_t *);
static void              tmpfs_release (vfs_node_t *);

/* ── Node factory ─────────────────────────────────────────────────────────── */

/* Wall-clock seconds, as ext2_now(): new and written files get real times. */
static uint32_t tmpfs_now(void) {
    return rtc_boot_epoch() + pit_ticks() / 100U;
}

static tmpfs_node_t *alloc_tmpfs_node(const char *name, uint32_t flags) {
    tmpfs_node_t *tn = (tmpfs_node_t *)kmalloc(sizeof(tmpfs_node_t));
    if (!tn) return NULL;
    memset(tn, 0, sizeof(tmpfs_node_t));

    strncpy(tn->vnode.name, name, 255);
    tn->vnode.name[255] = '\0';
    tn->vnode.flags = flags;
    tn->refs = 1;                    /* the link in the parent directory */
    /* Distinct inode numbers: stat() reports (st_dev 0, st_ino), and with every
     * tmpfs node at 0 cp/ln/ld.so took any two /tmp files for the same file.
     * Numbered well above initrd's and the disk's. */
    static uint32_t tmpfs_next_ino = 0x40000000U;
    tn->vnode.inode = tmpfs_next_ino++;
    tn->vnode.atime = tn->vnode.mtime = tn->vnode.ctime = tmpfs_now();
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
        /* A FIFO's pipe buffer lives exactly as long as the node, so every
         * open of the name shares it and no close can free it under the
         * node (the open path reuses node->private). */
        if (flags == VFS_FLAG_FIFO) {
            tn->vnode.private = pipe_fifo_alloc();
            if (!tn->vnode.private) { kfree(tn); return NULL; }
        }
    } else {
        tn->vnode.readdir_fn = tmpfs_readdir;
        tn->vnode.finddir_fn = tmpfs_finddir;
        tn->vnode.create_fn  = tmpfs_create;
        tn->vnode.unlink_fn  = tmpfs_unlink;
        tn->vnode.symlink_fn = tmpfs_symlink;
        tn->vnode.rename_fn  = tmpfs_rename;
    }
    return tn;
}

/* ── Page-frame file bodies ───────────────────────────────────────────────── */

/* Release every frame from page `first` on (a shrinking truncate, or the whole
 * body when the node is freed). */
static void tmpfs_drop_pages(tmpfs_node_t *tn, uint32_t first) {
    for (uint32_t pg = first; pg < tn->npages; pg++)
        if (tn->pages[pg]) {
            pmm_frame_decref(tn->pages[pg]);
            tn->pages[pg] = 0;
        }
}

/* Make pages[] at least `n` entries long.  Doubles, so a file written front
 * to back recopies the (small) array O(log n) times. */
static int tmpfs_grow_array(tmpfs_node_t *tn, uint32_t n) {
    if (n <= tn->npages) return 0;
    uint32_t cap = tn->npages ? tn->npages : 4;
    while (cap < n) cap *= 2;
    uint32_t *np = (uint32_t *)kmalloc(cap * sizeof(uint32_t));
    if (!np) return -12;
    memset(np, 0, cap * sizeof(uint32_t));
    if (tn->pages) {
        memcpy(np, tn->pages, tn->npages * sizeof(uint32_t));
        kfree(tn->pages);
    }
    tn->pages  = np;
    tn->npages = cap;
    return 0;
}

/* Copy `n` bytes between page frame `phys` (at byte `in`) and kernel buffer
 * `buf`.  Every vfs_read/vfs_write caller hands a kernel buffer (syscalls
 * bounce user data through one), and the page-population path may pass the
 * first temp-map slot itself as `buf`, so the frame goes through the second
 * slot.  A user address is copied through a small stack bounce instead, so a
 * fault on it never happens while the slot is held. */
static void tmpfs_copy(uint32_t phys, uint32_t in, uint8_t *buf, uint32_t n,
                       int to_frame) {
    if ((uintptr_t)buf >= 0xC0000000U) {
        preempt_disable();
        uint8_t *kp = (uint8_t *)paging_temp_map2(phys);
        if (to_frame) memcpy(kp + in, buf, n);
        else          memcpy(buf, kp + in, n);
        paging_temp_unmap2();
        preempt_enable();
        return;
    }
    uint8_t tmp[256];
    for (uint32_t done = 0; done < n; ) {
        uint32_t k = n - done < sizeof tmp ? n - done : sizeof tmp;
        if (to_frame) memcpy(tmp, buf + done, k);
        preempt_disable();
        uint8_t *kp = (uint8_t *)paging_temp_map2(phys);
        if (to_frame) memcpy(kp + in + done, tmp, k);
        else          memcpy(tmp, kp + in + done, k);
        paging_temp_unmap2();
        preempt_enable();
        if (!to_frame) memcpy(buf + done, tmp, k);
        done += k;
    }
}

/* Zero bytes [in, PAGE_SIZE) of a frame: the tail past a shrunk EOF, which a
 * later extension must read back as zeroes. */
static void tmpfs_zero_tail(uint32_t phys, uint32_t in) {
    preempt_disable();
    uint8_t *kp = (uint8_t *)paging_temp_map2(phys);
    memset(kp + in, 0, PAGE_SIZE - in);
    paging_temp_unmap2();
    preempt_enable();
}

/* The frame behind page `pg`, allocating a zeroed one on first write. */
static uint32_t tmpfs_frame(tmpfs_node_t *tn, uint32_t pg) {
    if (pg >= tn->npages && tmpfs_grow_array(tn, pg + 1) < 0) return 0;
    if (tn->pages[pg]) return tn->pages[pg];
    uint32_t phys = pmm_alloc_frame();
    if (!phys) return 0;
    pmm_frame_incref(phys);
    tmpfs_zero_tail(phys, 0);
    tn->pages[pg] = phys;
    return phys;
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
    tmpfs_drop_pages(tn, 0);
    if (tn->pages) { kfree(tn->pages); tn->pages = NULL; tn->npages = 0; }
    if (tn->data) { kfree(tn->data); tn->data = NULL; }
    if (node->flags == VFS_FLAG_FIFO) pipe_fifo_free((pipe_buf_t *)node->private);
    kfree(tn);
}

/* ── File operations ──────────────────────────────────────────────────────── */

static uint32_t tmpfs_read(vfs_node_t *node, uint32_t off, uint32_t len,
                            uint8_t *buf) {
    tmpfs_node_t *tn = (tmpfs_node_t *)node;
    if (off >= node->size) return 0;
    if (len > node->size - off) len = node->size - off;
    if (node->flags == VFS_FLAG_SYMLINK) {
        if (!tn->data) return 0;
        memcpy(buf, tn->data + off, len);
        return len;
    }
    for (uint32_t done = 0; done < len; ) {
        uint32_t pos = off + done;
        uint32_t pg  = pos / PAGE_SIZE, in = pos & (PAGE_SIZE - 1);
        uint32_t n   = PAGE_SIZE - in;
        if (n > len - done) n = len - done;
        uint32_t phys = pg < tn->npages ? tn->pages[pg] : 0;
        if (phys) tmpfs_copy(phys, in, buf + done, n, 0);
        else      memset(buf + done, 0, n);            /* a hole */
        done += n;
    }
    return len;
}

/* Largest file tmpfs holds.  Bodies are page frames now, not heap, so this is
 * no longer the heap window; it keeps every size computation below far from
 * 32-bit wrap and one file from taking all of RAM. */
#define TMPFS_MAX_FILE  0x40000000U

static uint32_t tmpfs_write(vfs_node_t *node, uint32_t off, uint32_t len,
                              const uint8_t *buf) {
    tmpfs_node_t *tn = (tmpfs_node_t *)node;

    /* Past the size cap nothing can be written (write(2) gets -EFBIG; 0 would
     * spin a libc write loop).  A write that straddles the cap is shortened,
     * as Linux does at s_maxbytes. */
    if (off >= TMPFS_MAX_FILE) return VFS_WRITE_EFBIG;
    if (len > TMPFS_MAX_FILE - off) len = TMPFS_MAX_FILE - off;

    uint32_t done = 0;
    while (done < len) {
        uint32_t pos = off + done;
        uint32_t in  = pos & (PAGE_SIZE - 1);
        uint32_t n   = PAGE_SIZE - in;
        if (n > len - done) n = len - done;
        uint32_t phys = tmpfs_frame(tn, pos / PAGE_SIZE);
        if (!phys) break;                           /* out of frames */
        tmpfs_copy(phys, in, (uint8_t *)(uintptr_t)(buf + done), n, 1);
        done += n;
    }
    /* Out of memory for the body: Linux tmpfs returns ENOMEM from
     * shmem_alloc_and_acct_folio; 0 would spin a libc write loop. */
    if (done == 0 && len) return VFS_WRITE_ENOMEM;
    if (off + done > node->size) node->size = off + done;
    if (done) node->mtime = node->ctime = tmpfs_now();
    return done;
}

/* Linux shmem_setattr: growing is lazy (the new pages are holes that read as
 * zero); shrinking frees the pages past the new end and zeroes the tail of the
 * last partial one, so a later extension reads zeroes, not old bytes. */
static int tmpfs_truncate(vfs_node_t *node, uint32_t new_size) {
    tmpfs_node_t *tn = (tmpfs_node_t *)node;
    if (new_size > TMPFS_MAX_FILE) return -27;      /* -EFBIG */
    if (new_size < node->size) {
        uint32_t in = new_size & (PAGE_SIZE - 1);
        uint32_t pg = new_size / PAGE_SIZE;
        if (in && pg < tn->npages && tn->pages[pg])
            tmpfs_zero_tail(tn->pages[pg], in);
        tmpfs_drop_pages(tn, (new_size + PAGE_SIZE - 1) / PAGE_SIZE);
    }
    node->size = new_size;
    node->mtime = node->ctime = tmpfs_now();
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
    child->parent     = dir;
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
            c->parent = NULL;
            tmpfs_release(&c->vnode);
            return 0;
        }
        prev = c;
    }
    return -2;  /* -ENOENT */
}

/* Unlink `c` from dir's child list (it must be there). */
static void tmpfs_detach(tmpfs_node_t *dir, tmpfs_node_t *c) {
    tmpfs_node_t **pp = &dir->first_child;
    while (*pp && *pp != c) pp = (tmpfs_node_t **)&(*pp)->vnode.next;
    if (*pp) *pp = (tmpfs_node_t *)c->vnode.next;
    c->vnode.next = NULL;
    c->parent = NULL;
}

/* rename_fn.  The whole move happens without sleeping, so no other thread can
 * ever look up new_name and find nothing: the old target leaves the list in
 * the same step that the source takes its place.  The source node itself is
 * moved, keeping its mode, owner, data and every open descriptor. */
static int tmpfs_rename(vfs_node_t *old_dir_node, const char *old_name,
                        vfs_node_t *new_dir_node, const char *new_name) {
    tmpfs_node_t *odir = (tmpfs_node_t *)old_dir_node;
    tmpfs_node_t *ndir = (tmpfs_node_t *)new_dir_node;
    tmpfs_node_t *src = (tmpfs_node_t *)tmpfs_finddir(old_dir_node, old_name);
    if (!src) return -2;                                    /* -ENOENT */
    if (strlen(new_name) > 255) return -36;                 /* -ENAMETOOLONG */
    tmpfs_node_t *dst = (tmpfs_node_t *)tmpfs_finddir(new_dir_node, new_name);
    if (dst == src) return 0;                               /* same entry */
    int src_dir = (src->vnode.flags == VFS_FLAG_DIR);
    if (dst) {
        int dst_dir = (dst->vnode.flags == VFS_FLAG_DIR);
        if (src_dir && !dst_dir) return -20;                /* -ENOTDIR */
        if (!src_dir && dst_dir) return -21;                /* -EISDIR */
        if (dst_dir && dst->first_child) return -39;        /* -ENOTEMPTY */
    }
    /* A directory may not move into its own subtree.  Checked on the nodes,
     * by walking up from the new parent: the caller's path comparison cannot
     * see through a symlink ("/tmp/l/x" with l -> /tmp/a/b), and a directory
     * linked under its own descendant would be an unreachable cycle. */
    if (src_dir)
        for (tmpfs_node_t *a = ndir; a; a = a->parent)
            if (a == src) return -22;                        /* -EINVAL */

    tmpfs_detach(odir, src);
    if (dst) {
        tmpfs_detach(ndir, dst);
        tmpfs_release(&dst->vnode);         /* the directory's link to it */
    }
    strncpy(src->vnode.name, new_name, 255);
    src->vnode.name[255] = '\0';
    src->vnode.next  = (vfs_node_t *)ndir->first_child;
    ndir->first_child = src;
    src->parent       = ndir;
    return 0;
}

/* ── Public API ────────────────────────────────────────────────────────────── */

vfs_node_t *tmpfs_mount(void) {
    tmpfs_node_t *root = alloc_tmpfs_node("/", VFS_FLAG_DIR);
    if (!root) return NULL;
    /* Like Linux /tmp and /dev/shm: world-writable with the sticky bit, so a
     * user can remove or replace only their own entries. */
    root->vnode.mask = 01777;
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

    /* Link into parent directory */
    tmpfs_node_t *tdir = (tmpfs_node_t *)dir;
    tn->vnode.next = (vfs_node_t *)tdir->first_child;
    tdir->first_child = tn;
    tn->parent = tdir;
    return 0;
}
