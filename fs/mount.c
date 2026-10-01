#include "mount.h"
#include "ext2.h"
#include "ext4.h"
#include "vfat.h"
#include "exfat.h"
#include "tmpfs.h"
#include "../drivers/blkpart.h"
#include "../drivers/blkdev.h"
#include "../lib/string.h"
#include "../kernel/printk.h"
#include "../proc/process.h"
#include <stddef.h>

static vfs_node_t *g_proc_root, *g_dev_root;

void mount_set_boot_roots(vfs_node_t *proc_root, vfs_node_t *dev_root) {
    g_proc_root = proc_root;
    g_dev_root  = dev_root;
}

#define MS_REMOUNT      0x0020u
#define MS_BIND         0x1000u
#define MS_MOVE         0x2000u
#define MS_REC          0x4000u
#define MS_UNBINDABLE   (1u << 17)
#define MS_PRIVATE      (1u << 18)
#define MS_SLAVE        (1u << 19)
#define MS_SHARED       (1u << 20)
#define MS_PROPAGATION  (MS_UNBINDABLE | MS_PRIVATE | MS_SLAVE | MS_SHARED)
/* Flags recorded on the mount; the rest (atime handling, sync, ...) are
 * accepted and have no effect here. */
#define MS_KEEP (VFS_MS_RDONLY | VFS_MS_NOSUID | VFS_MS_NODEV | VFS_MS_NOEXEC)

static int is_ext_type(const char *t) {
    return strcmp(t, "ext4") == 0 || strcmp(t, "ext3") == 0 ||
           strcmp(t, "ext2") == 0;
}

static int is_fat_type(const char *t) {
    return strcmp(t, "vfat") == 0 || strcmp(t, "msdos") == 0 ||
           strcmp(t, "fat") == 0;
}

static int is_exfat_type(const char *t) {
    return strcmp(t, "exfat") == 0;
}

/* statfs(2) of a mount(2) ext2 instance, in the vfs_mnt_t hook's form. */
static int ext2_statfs_hook(void *fs, vfs_statfs_t *out) {
    uint32_t bs, blocks, bfree, inodes, ifree;
    int r = ext2_statfs_fs((ext2_fs_t *)fs, &bs, &blocks, &bfree, &inodes, &ifree);
    if (r < 0) return r;
    out->type    = 0xEF53;                         /* EXT2_SUPER_MAGIC */
    out->bsize   = bs;
    out->blocks  = blocks;
    out->bfree   = bfree;
    out->files   = inodes;
    out->ffree   = ifree;
    out->namelen = 255;
    return 0;
}

/* Is `opt` one of the comma-separated options in `data`? */
static int mount_has_opt(const char *data, const char *opt) {
    size_t n = strlen(opt);
    for (const char *p = data; *p; ) {
        const char *e = p;
        while (*e && *e != ',') e++;
        if ((size_t)(e - p) == n && strncmp(p, opt, n) == 0) return 1;
        p = *e ? e + 1 : e;
    }
    return 0;
}

static int mount_block(const char *source, const char *target, const char *fstype,
                       uint32_t flags, const char *data) {
    if (!source) return -22;                                  /* -EINVAL */
    int err;
    vfs_node_t *dn = vfs_lookup(source, 1, &err);
    if (!dn) return err;
    blkpart_t *bp = blkpart_from_node(dn);
    if (!bp) return -15;                                      /* -ENOTBLK */
    /* The boot disk (IDE, AHCI or NVMe) is /disk already, through fs/ext2.c,
     * read-write. */
    if (blk_disk_is_boot(bp->dev) && vfs_get_root_overlay() && bp->partno == 0)
        return -16;
    char devname[24];
    devname[0] = '\0';
    strncpy(devname, "/dev/", sizeof(devname));
    strncat(devname, bp->name, sizeof(devname) - strlen(devname) - 1);
    if (vfs_mount_has_fs_source(devname)) return -16;         /* -EBUSY */

    vfs_mnt_t t;
    memset(&t, 0, sizeof(t));
    strncpy(t.source, devname, sizeof(t.source) - 1);
    strncpy(t.fstype, fstype, sizeof(t.fstype) - 1);
    t.flags = flags & MS_KEEP;
    vfs_node_t *root;
    int r;
    if (is_fat_type(fstype)) {
        vfat_opts_t o;
        vfat_parse_opts(data, &o);
        vfat_fs_t *vfs;
        r = vfat_mount_dev(bp, (flags & VFS_MS_RDONLY) != 0, &o, &root, &vfs);
        if (r < 0) return r;
        t.fs      = vfs;
        t.busy    = vfat_busy;
        t.release = vfat_release;
        t.set_ro  = vfat_set_ro;
        t.statfs  = vfat_statfs;
        r = vfs_mount_add(target, root, &t);
        if (r < 0) vfat_release(vfs);
        return r;
    }
    if (is_exfat_type(fstype)) {
        vfat_opts_t o;
        vfat_parse_opts(data, &o);
        exfat_fs_t *xfs;
        r = exfat_mount_dev(bp, (flags & VFS_MS_RDONLY) != 0, &o, &root, &xfs);
        if (r < 0) return r;
        t.fs      = xfs;
        t.busy    = exfat_busy;
        t.release = exfat_release;
        t.set_ro  = exfat_set_ro;
        t.statfs  = exfat_statfs;
        r = vfs_mount_add(target, root, &t);
        if (r < 0) exfat_release(xfs);
        return r;
    }

    /* Read-write, whatever the type asked for, is fs/ext2.c's: it writes
     * ext2, and ext3/ext4 that use no incompatible feature and whose journal
     * is empty.  Anything else is refused with -EROFS rather than silently
     * downgraded; mount(8) then retries read-only ("is write-protected,
     * mounting read-only"), which the read-only ext4 driver serves.  An
     * explicit read-only ext2 mount stays with the ext2 driver when it can
     * read the filesystem, so remount,rw works on it. */
    int want_rw = !(flags & VFS_MS_RDONLY);
    if (want_rw || strcmp(fstype, "ext2") == 0) {
        ext2_fs_t *e2;
        r = ext2_mount_dev(bp, !want_rw, &root, &e2);
        if (r == 0) {
            if (data && want_rw && mount_has_opt(data, "x4crash")) ext2_test_crash(e2);
            if (data && want_rw && mount_has_opt(data, "x4smalltxn")) ext2_test_opt(e2, "x4smalltxn");
            if (data && want_rw && mount_has_opt(data, "x4crashunlink")) ext2_test_opt(e2, "x4crashunlink");
            t.fs      = e2;
            t.busy    = ext2_busy;
            t.release = ext2_release;
            t.set_ro  = ext2_set_ro;
            t.statfs  = ext2_statfs_hook;
            r = vfs_mount_add(target, root, &t);
            if (r < 0) ext2_release(e2);
            return r;
        }
        if (want_rw) return r == -12 ? r : -30;               /* -EROFS */
    }

    ext4_fs_t *fs;
    r = ext4_mount_dev(bp, &root, &fs);
    if (r < 0) return r;
    t.fs      = fs;
    t.busy    = ext4_busy;
    t.release = ext4_release;
    r = vfs_mount_add(target, root, &t);
    if (r < 0) ext4_release(fs);
    return r;
}

static int mount_remount(const char *target, uint32_t flags) {
    int err;
    vfs_mnt_t *m = vfs_mount_find(target, &err);
    if (!m) return err;
    int want_ro = (flags & VFS_MS_RDONLY) != 0;
    /* The read-only ext4 driver has no set_ro: it cannot go read-write. */
    if (!want_ro && (m->flags & VFS_MS_RDONLY) && is_ext_type(m->fstype) &&
        !m->set_ro)
        return -30;                                           /* -EROFS */
    /* Linux: going read-only while a file of the mount is open for writing
     * is -EBUSY, so no descriptor can write to a read-only mount. */
    if (want_ro && !(m->flags & VFS_MS_RDONLY)) {
        for (int i = 0; i < MAX_PROCS; i++) {
            struct proc *p = &ptable[i];
            /* Kernel threads have no descriptor table, and an exited
             * process may have dropped its (shared) one already. */
            if (p->state == PROC_UNUSED || p->state == PROC_ZOMBIE || !p->ofile)
                continue;
            for (int fd = 0; fd < MAX_FD; fd++) {
                proc_file_t *f = &p->ofile[fd];
                if (f->type != FD_NONE && f->mnt == m && f->mnt_seq == m->seq &&
                    (f->flags & O_ACCMODE) != O_RDONLY)
                    return -16;                               /* -EBUSY */
            }
        }
    }
    /* A filesystem that writes (vfat, ext2) is told, so it can flush and mark
     * the volume clean, or dirty again. */
    if (m->set_ro && want_ro != ((m->flags & VFS_MS_RDONLY) != 0)) {
        int r = m->set_ro(m->fs, want_ro);
        if (r < 0) return r;
    }
    m->flags = flags & MS_KEEP;
    return 0;
}

int mount_do(const char *source, const char *target, const char *fstype,
             uint32_t flags, const char *data) {
    /* Pre-2.4 magic in the high half (MS_MGC_VAL). */
    if ((flags & 0xFFFF0000u) == 0xC0ED0000u) flags &= 0xFFFFu;
    if (!target || target[0] != '/') return -22;

    if (flags & MS_REMOUNT)
        return mount_remount(target, flags);
    /* Propagation changes (mount --make-private ...): nothing propagates
     * here, so they all hold already. */
    if (flags & MS_PROPAGATION) {
        int err;
        return vfs_lookup(target, 1, &err) ? 0 : err;
    }
    if (flags & MS_MOVE) return -22;
    if (flags & MS_BIND) {
        /* A bind mount shows an existing directory at a second place. */
        if (!source) return -22;
        int err;
        vfs_mnt_t *src_m = NULL;
        vfs_node_t *src = vfs_lookup_mnt(source, 1, &err, &src_m);
        if (!src) return err;
        if (src->flags != VFS_FLAG_DIR) return -20;           /* -ENOTDIR */
        vfs_mnt_t t;
        memset(&t, 0, sizeof(t));
        t.src = src_m;        /* pins the mount the source lives on */
        strncpy(t.source, source, sizeof(t.source) - 1);
        strncpy(t.fstype, "none", sizeof(t.fstype) - 1);
        t.flags = flags & MS_KEEP;
        return vfs_mount_add(target, src, &t);
    }
    if (!fstype) return -22;

    if (is_ext_type(fstype) || is_fat_type(fstype) || is_exfat_type(fstype))
        return mount_block(source, target, fstype, flags, data);

    vfs_node_t *root = NULL;
    if (strcmp(fstype, "tmpfs") == 0) {
        root = tmpfs_mount();
        if (!root) return -12;
        root->mask = 01777;
        /* tmpfs has no teardown: an unmounted instance's memory is kept. */
    } else if (strcmp(fstype, "proc") == 0) {
        root = g_proc_root;
    } else if (strcmp(fstype, "devtmpfs") == 0 || strcmp(fstype, "devfs") == 0) {
        root = g_dev_root;
    }
    if (!root) return -19;                                    /* -ENODEV */
    vfs_mnt_t t;
    memset(&t, 0, sizeof(t));
    strncpy(t.source, source ? source : fstype, sizeof(t.source) - 1);
    strncpy(t.fstype, fstype, sizeof(t.fstype) - 1);
    t.flags = flags & MS_KEEP;
    return vfs_mount_add(target, root, &t);
}

/* Is process `p`'s working directory inside mount `m`?  The cwd is a path
 * relative to p's own root (a chrooted process's "/mnt" is somewhere else
 * globally), so it is resolved from there, and the mounts crossed to reach
 * it are compared, not the text. */
static int cwd_within(struct proc *p, vfs_mnt_t *m) {
    vfs_mnt_t *cm = NULL;
    int err;
    if (!vfs_lookup_from(p->root_node, p->cwd, 1, &err, &cm)) return 0;
    for (int hops = 0; cm && hops < VFS_MNT_MAX; hops++, cm = cm->parent)
        if (cm == m) return 1;
    return 0;
}

int umount_do(const char *target, uint32_t flags) {
    if (flags & ~(VFS_MNT_FORCE | VFS_MNT_DETACH | 0x4u | 0x8u)) return -22;
    int err;
    vfs_mnt_t *m = vfs_mount_find(target, &err);
    if (!m) return err;
    /* A process whose working directory is inside keeps it busy (the cwd
     * is a path here, so this is the only trace of it). */
    if (!(flags & VFS_MNT_DETACH)) {
        for (int i = 0; i < MAX_PROCS; i++) {
            struct proc *p = &ptable[i];
            if (p->state == PROC_UNUSED || p->state == PROC_ZOMBIE) continue;
            if (cwd_within(p, m)) return -16;                 /* -EBUSY */
        }
    }
    return vfs_mount_remove(m, flags);
}
