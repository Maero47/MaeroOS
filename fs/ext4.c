/*
 * Read-only ext2/ext3/ext4 driver (see ext4.h).
 *
 * Written for MaeroOS from the on-disk format as documented in the Linux
 * kernel's Documentation/filesystems/ext4/ (kernel.org, "ext4 Data
 * Structures and Algorithms") and the ext2 layout description; the directory
 * hashes are MD4 (RFC 1320) halved and TEA (Wheeler & Needham), as that
 * document describes them.  FreeBSD's sys/fs/ext2fs (BSD licence) and HelenOS
 * uspace/lib/ext4 (BSD-3-Clause) were consulted for the layout only; no code
 * was copied from them or from any GPL source.
 */
#include "ext4.h"
#include "vfs.h"
#include "../mm/heap.h"
#include "../lib/string.h"
#include "../kernel/printk.h"
#include "../proc/scheduler.h"
#include <stddef.h>
#include <stdint.h>

#define EXT4_ERR_INVAL     (-22)
#define EXT4_ERR_NOMEM     (-12)
#define EXT4_ERR_IO        (-5)
#define EXT4_ERR_ROFS      (-30)
#define EXT4_ERR_NOTSUP    (-95)
#define EXT4_ERR_UCLEAN    (-117)

/* Superblock feature bits */
#define COMPAT_DIR_INDEX        0x0020u
#define COMPAT_SPARSE_SUPER2    0x0200u
#define INCOMPAT_COMPRESSION    0x0001u
#define INCOMPAT_FILETYPE       0x0002u
#define INCOMPAT_RECOVER        0x0004u
#define INCOMPAT_JOURNAL_DEV    0x0008u
#define INCOMPAT_META_BG        0x0010u
#define INCOMPAT_EXTENTS        0x0040u
#define INCOMPAT_64BIT          0x0080u
#define INCOMPAT_MMP            0x0100u
#define INCOMPAT_FLEX_BG        0x0200u
#define INCOMPAT_EA_INODE       0x0400u
#define INCOMPAT_DIRDATA        0x1000u
#define INCOMPAT_CSUM_SEED      0x2000u
#define INCOMPAT_LARGEDIR       0x4000u
#define INCOMPAT_INLINE_DATA    0x8000u
#define INCOMPAT_ENCRYPT        0x10000u
#define INCOMPAT_CASEFOLD       0x20000u
#define RO_COMPAT_SPARSE_SUPER  0x0001u
#define RO_COMPAT_METADATA_CSUM 0x0400u

#define INCOMPAT_READABLE (INCOMPAT_FILETYPE | INCOMPAT_META_BG | INCOMPAT_EXTENTS | \
                           INCOMPAT_64BIT | INCOMPAT_MMP | INCOMPAT_FLEX_BG |       \
                           INCOMPAT_EA_INODE | INCOMPAT_CSUM_SEED | INCOMPAT_LARGEDIR)

/* Inode flags */
#define EXT4_INDEX_FL        0x00001000u   /* htree directory */
#define EXT4_EXTENTS_FL      0x00080000u
#define EXT4_INLINE_DATA_FL  0x10000000u

#define EXT4_S_IFMT   0xF000u
#define EXT4_S_IFSOCK 0xC000u
#define EXT4_S_IFLNK  0xA000u
#define EXT4_S_IFREG  0x8000u
#define EXT4_S_IFBLK  0x6000u
#define EXT4_S_IFDIR  0x4000u
#define EXT4_S_IFCHR  0x2000u
#define EXT4_S_IFIFO  0x1000u

#define EXT4_NODE_BUCKETS 256
#define EXT4_CACHE_BYTES  (1024u * 1024u)
#define EXT4_CACHE_WAYS   4
#define EXT4_IO_SECTORS   128u                /* one device request */

typedef struct ext4_vnode ext4_vnode_t;

struct ext4_fs {
    blkpart_t *bp;
    uint32_t   bs, log_bs, spb;               /* block size, log2, sectors/block */
    uint64_t   blocks_count;
    uint32_t   first_data_block;
    uint32_t   bpg, ipg, inodes_count, groups;
    uint32_t   inode_size, desc_size;
    uint32_t   compat, incompat, ro_compat;
    uint32_t   hash_seed[4];
    int        hash_unsigned;
    int        csum;                          /* metadata_csum: verify */
    uint32_t   csum_seed;
    uint64_t  *itable;                        /* inode table block per group */
    /* block cache: EXT4_CACHE_WAYS-way set associative, LRU within a set */
    uint32_t   bc_sets;
    uint64_t  *bc_blk;                        /* ~0 = empty */
    uint32_t  *bc_age;
    uint8_t   *bc_data;
    uint32_t   bc_clock;
    ext4_vnode_t *nodes[EXT4_NODE_BUCKETS];
    int        open_refs;
    uint32_t   csum_errors;
    uint32_t   htree_fallbacks;
};

struct ext4_vnode {
    vfs_node_t  vnode;          /* first: the VFS hands us &vnode */
    ext4_fs_t  *fs;
    ext4_vnode_t *hnext;
    uint32_t    ino;
    uint16_t    mode;
    uint32_t    iflags;
    uint64_t    size;
    uint64_t    blocks;         /* i_blocks, 512-byte units */
    uint64_t    file_acl;
    uint32_t    csum_seed;      /* per-inode checksum seed */
    uint8_t     i_block[60];
    /* last mapping found, so sequential reads skip the extent walk */
    uint32_t    m_lblk, m_len;
    uint64_t    m_pblk;
    int         m_valid;
    /* readdir cursor: position of entry number c_idx */
    uint32_t    c_idx, c_blk, c_off;
    int         c_valid;
};

/* ── Little-endian field access ─────────────────────────────────────────── */

static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* ── CRC32C (Castagnoli, reflected 0x82F63B78) ──────────────────────────────
 * ext4 feeds the running value straight through, with no final inversion:
 * the filesystem seed is crc32c(~0, uuid), each structure's checksum is
 * crc32c(seed, bytes). */
static uint32_t g_crc32c[256];
static int g_crc32c_ready;

static void crc32c_init(void) {
    if (g_crc32c_ready) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0x82F63B78u & (0u - (c & 1u)));
        g_crc32c[i] = c;
    }
    g_crc32c_ready = 1;
}

static uint32_t crc32c(uint32_t crc, const void *data, uint32_t n) {
    const uint8_t *p = (const uint8_t *)data;
    while (n--)
        crc = g_crc32c[(crc ^ *p++) & 0xFFu] ^ (crc >> 8);
    return crc;
}

/* ── Device and block cache ─────────────────────────────────────────────── */

/* Read `n` whole blocks straight from the device. */
static int ext4_dev_read(ext4_fs_t *fs, uint64_t blk, uint32_t n, void *buf) {
    if (blk >= fs->blocks_count || n > fs->blocks_count - blk) return -1;
    uint64_t sec = blk * fs->spb;
    uint64_t cnt = (uint64_t)n * fs->spb;
    if (sec + cnt > fs->bp->nsect) return -1;
    uint8_t *p = (uint8_t *)buf;
    while (cnt) {
        uint32_t k = cnt > EXT4_IO_SECTORS ? EXT4_IO_SECTORS : (uint32_t)cnt;
        if (blkpart_read(fs->bp, (uint32_t)sec, k, p) < 0) return -1;
        sec += k; cnt -= k; p += k * 512u;
    }
    return 0;
}

/* Copy `len` bytes at `off` of block `blk` into `dst`, through the cache. */
static int ext4_bread(ext4_fs_t *fs, uint64_t blk, uint32_t off, uint32_t len,
                      void *dst) {
    if (off > fs->bs || len > fs->bs - off) return -1;
    if (blk >= fs->blocks_count) return -1;
    int rc = 0;
    preempt_disable();
    uint32_t set = (uint32_t)blk & (fs->bc_sets - 1u);
    uint32_t base = set * EXT4_CACHE_WAYS, victim = base;
    for (uint32_t w = base; w < base + EXT4_CACHE_WAYS; w++) {
        if (fs->bc_blk[w] == blk) {
            fs->bc_age[w] = ++fs->bc_clock;
            memcpy(dst, fs->bc_data + (uint64_t)w * fs->bs + off, len);
            preempt_enable();
            return 0;
        }
        if (fs->bc_age[w] < fs->bc_age[victim]) victim = w;
    }
    uint8_t *slot = fs->bc_data + (uint64_t)victim * fs->bs;
    fs->bc_blk[victim] = ~0ull;
    if (ext4_dev_read(fs, blk, 1, slot) < 0) {
        rc = -1;
    } else {
        fs->bc_blk[victim] = blk;
        fs->bc_age[victim] = ++fs->bc_clock;
        memcpy(dst, slot + off, len);
    }
    preempt_enable();
    return rc;
}

/* ── Inodes ─────────────────────────────────────────────────────────────── */

#define EXT4_INODE_MAX 1024u   /* inode_size bound accepted at mount */

static int ext4_read_inode(ext4_fs_t *fs, uint32_t ino, uint8_t *raw) {
    if (ino == 0 || ino > fs->inodes_count) return -1;
    uint32_t grp = (ino - 1) / fs->ipg;
    uint32_t idx = (ino - 1) % fs->ipg;
    if (grp >= fs->groups) return -1;
    uint64_t byte = (uint64_t)idx * fs->inode_size;
    uint64_t blk  = fs->itable[grp] + (byte >> fs->log_bs);
    uint32_t off  = (uint32_t)(byte & (fs->bs - 1u));
    if (ext4_bread(fs, blk, off, fs->inode_size, raw) < 0) return -1;
    if (fs->csum) {
        uint32_t extra = fs->inode_size > 128 ? rd16(raw + 0x80) : 0;
        int has_hi = (128u + extra >= 0x84u);
        uint32_t want = rd16(raw + 0x7C) | (has_hi ? (uint32_t)rd16(raw + 0x82) << 16 : 0);
        uint8_t le[4];
        uint32_t c;
        le[0] = (uint8_t)ino; le[1] = (uint8_t)(ino >> 8);
        le[2] = (uint8_t)(ino >> 16); le[3] = (uint8_t)(ino >> 24);
        c = crc32c(fs->csum_seed, le, 4);
        c = crc32c(c, raw + 0x64, 4);                 /* i_generation */
        static const uint8_t zero2[2] = { 0, 0 };
        c = crc32c(c, raw, 0x7C);
        c = crc32c(c, zero2, 2);
        if (has_hi) {
            c = crc32c(c, raw + 0x7E, 0x82 - 0x7E);
            c = crc32c(c, zero2, 2);
            c = crc32c(c, raw + 0x84, fs->inode_size - 0x84);
        } else {
            c = crc32c(c, raw + 0x7E, fs->inode_size - 0x7E);
            c &= 0xFFFFu;
        }
        if (c != want) {
            if (fs->csum_errors++ < 8)
                printk("[EXT4] %s: inode %u fails its checksum\n",
                       fs->bp->name, (unsigned)ino);
            return -1;
        }
    }
    return 0;
}

/* ── Block mapping ──────────────────────────────────────────────────────── */

typedef struct {
    uint32_t len;      /* blocks from the requested one on */
    uint64_t pblk;     /* 0: hole or unwritten extent (reads as zeros) */
} ext4_run_t;

#define EXT4_EXT_MAGIC 0xF30Au

static int ext4_ext_csum_ok(ext4_fs_t *fs, ext4_vnode_t *vn, const uint8_t *blk) {
    if (!fs->csum) return 1;
    uint32_t tail = 12u + 12u * rd16(blk + 4);           /* after eh_max entries */
    if (tail + 4 > fs->bs) return 0;
    if (crc32c(vn->csum_seed, blk, tail) == rd32(blk + tail)) return 1;
    if (fs->csum_errors++ < 8)
        printk("[EXT4] %s: extent block of inode %u fails its checksum\n",
               fs->bp->name, (unsigned)vn->ino);
    return 0;
}

static int ext4_ext_map(ext4_vnode_t *vn, uint32_t lblk, ext4_run_t *run) {
    ext4_fs_t *fs = vn->fs;
    const uint8_t *node = vn->i_block;
    uint32_t node_size = 60;
    uint32_t limit = 0xFFFFFFFFu;     /* first block mapped by a later subtree */
    uint8_t *buf = NULL;
    int rc = -1, want_depth = -1;

    for (int level = 0; level < 8; level++) {
        uint32_t entries = rd16(node + 2), max = rd16(node + 4), depth = rd16(node + 6);
        if (rd16(node) != EXT4_EXT_MAGIC || entries > max ||
            12u + 12u * max > node_size || depth > 5 ||
            (want_depth >= 0 && depth != (uint32_t)want_depth))
            break;
        const uint8_t *e = node + 12;
        if (depth == 0) {
            run->pblk = 0;
            run->len  = limit - lblk;
            for (uint32_t i = 0; i < entries; i++, e += 12) {
                uint32_t eb = rd32(e), len = rd16(e + 4);
                int unwritten = len > 32768u;
                if (unwritten) len -= 32768u;
                if (lblk < eb) { run->len = eb - lblk; break; }
                if (lblk - eb < len) {
                    run->len  = len - (lblk - eb);
                    run->pblk = unwritten ? 0 :
                        (((uint64_t)rd16(e + 6) << 32) | rd32(e + 8)) + (lblk - eb);
                    break;
                }
            }
            if (run->len == 0) run->len = 1;
            rc = 0;
            break;
        }
        /* Index node: the last entry starting at or before lblk. */
        int pick = -1;
        for (uint32_t i = 0; i < entries; i++) {
            if (rd32(e + 12 * i) > lblk) { limit = rd32(e + 12 * i); break; }
            pick = (int)i;
        }
        if (pick < 0) {                       /* before the first subtree */
            run->pblk = 0;
            run->len  = (entries ? rd32(e) : limit) - lblk;
            if (run->len == 0) run->len = 1;
            rc = 0;
            break;
        }
        const uint8_t *ix = e + 12 * (uint32_t)pick;
        uint64_t child = ((uint64_t)rd16(ix + 8) << 32) | rd32(ix + 4);
        if (!buf && !(buf = (uint8_t *)kmalloc(fs->bs))) break;
        if (ext4_bread(fs, child, 0, fs->bs, buf) < 0) break;
        if (!ext4_ext_csum_ok(fs, vn, buf)) break;
        node = buf;
        node_size = fs->bs;
        want_depth = (int)depth - 1;
    }
    if (buf) kfree(buf);
    return rc;
}

/* Old-style block map: 12 direct, then single, double and triple indirect. */
static int ext4_ind_map(ext4_vnode_t *vn, uint32_t lblk, ext4_run_t *run) {
    ext4_fs_t *fs = vn->fs;
    uint32_t apb = fs->bs / 4u, shift = fs->log_bs - 2u;
    uint32_t path[4], depth;
    uint64_t l = lblk;
    if (l < 12) { path[0] = (uint32_t)l; depth = 0; }
    else if ((l -= 12) < apb) { path[0] = 12; path[1] = (uint32_t)l; depth = 1; }
    else if ((l -= apb) < (uint64_t)apb * apb) {
        path[0] = 13; path[1] = (uint32_t)(l >> shift);
        path[2] = (uint32_t)(l & (apb - 1)); depth = 2;
    } else if ((l -= (uint64_t)apb * apb) < (uint64_t)apb * apb * apb) {
        path[0] = 14; path[1] = (uint32_t)(l >> (2 * shift));
        path[2] = (uint32_t)((l >> shift) & (apb - 1));
        path[3] = (uint32_t)(l & (apb - 1)); depth = 3;
    } else {
        return -1;
    }
    uint32_t b = rd32(vn->i_block + 4 * path[0]);
    for (uint32_t d = 1; d <= depth && b; d++) {
        uint8_t w[4];
        if (ext4_bread(fs, b, path[d] * 4u, 4, w) < 0) return -1;
        b = rd32(w);
    }
    run->len  = 1;
    run->pblk = b;
    return 0;
}

static int ext4_map(ext4_vnode_t *vn, uint32_t lblk, ext4_run_t *run) {
    preempt_disable();
    if (vn->m_valid && lblk >= vn->m_lblk && lblk - vn->m_lblk < vn->m_len) {
        uint32_t d = lblk - vn->m_lblk;
        run->len  = vn->m_len - d;
        run->pblk = vn->m_pblk ? vn->m_pblk + d : 0;
        preempt_enable();
        return 0;
    }
    preempt_enable();
    int rc = (vn->iflags & EXT4_EXTENTS_FL) ? ext4_ext_map(vn, lblk, run)
                                            : ext4_ind_map(vn, lblk, run);
    if (rc == 0) {
        preempt_disable();
        vn->m_lblk = lblk; vn->m_len = run->len; vn->m_pblk = run->pblk;
        vn->m_valid = 1;
        preempt_enable();
    }
    return rc;
}

/* ── File and symlink data ──────────────────────────────────────────────── */

static int ext4_fast_symlink(const ext4_vnode_t *vn) {
    if ((vn->mode & EXT4_S_IFMT) != EXT4_S_IFLNK || vn->size >= 60) return 0;
    if (vn->iflags & EXT4_EXTENTS_FL) return 0;
    uint64_t ea = vn->file_acl ? (vn->fs->bs >> 9) : 0;
    return vn->blocks <= ea;
}

static uint32_t ext4_read_node(vfs_node_t *node, uint64_t off, uint32_t len,
                               uint8_t *buf) {
    ext4_vnode_t *vn = (ext4_vnode_t *)node;
    ext4_fs_t *fs = vn->fs;
    if (vn->iflags & EXT4_INLINE_DATA_FL) return 0;
    if (off >= vn->size) return 0;
    if (len > vn->size - off) len = (uint32_t)(vn->size - off);
    if (ext4_fast_symlink(vn)) {
        memcpy(buf, vn->i_block + (uint32_t)off, len);
        return len;
    }
    uint32_t done = 0;
    while (done < len) {
        uint64_t pos  = off + done;
        /* Logical block numbers are 32-bit in ext4 (2^32 blocks per file). */
        if ((pos >> fs->log_bs) > 0xFFFFFFFFull) break;
        uint32_t lblk = (uint32_t)(pos >> fs->log_bs);
        uint32_t boff = (uint32_t)pos & (fs->bs - 1u);
        ext4_run_t r;
        if (ext4_map(vn, lblk, &r) < 0) break;
        uint64_t avail = (uint64_t)r.len * fs->bs - boff;
        uint32_t n = (avail < len - done) ? (uint32_t)avail : len - done;
        if (!r.pblk) {                                   /* hole */
            memset(buf + done, 0, n);
            done += n;
            continue;
        }
        if (boff == 0 && n >= fs->bs) {
            /* Whole blocks: straight from the device, bypassing the cache. */
            uint32_t nb = n >> fs->log_bs;
            uint32_t max = EXT4_IO_SECTORS / fs->spb;
            if (max == 0) max = 1;
            if (nb > max) nb = max;
            if (ext4_dev_read(fs, r.pblk, nb, buf + done) < 0) break;
            done += nb << fs->log_bs;
            continue;
        }
        if (n > fs->bs - boff) n = fs->bs - boff;
        if (ext4_bread(fs, r.pblk, boff, n, buf + done) < 0) break;
        done += n;
    }
    return done;
}

/* ── Directories ────────────────────────────────────────────────────────── */

static uint32_t ext4_dir_nblocks(const ext4_vnode_t *vn) {
    uint64_t n = (vn->size + vn->fs->bs - 1) >> vn->fs->log_bs;
    return n > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)n;
}

/* Read logical block `idx` of a directory; 0 ok, 1 hole, -1 error. */
static int ext4_dir_block(ext4_vnode_t *vn, uint32_t idx, uint8_t *buf) {
    ext4_run_t r;
    if (ext4_map(vn, idx, &r) < 0) return -1;
    if (!r.pblk) return 1;
    return ext4_bread(vn->fs, r.pblk, 0, vn->fs->bs, buf);
}

static uint32_t ext4_rec_len(const ext4_fs_t *fs, const uint8_t *de) {
    uint32_t len = rd16(de + 4);
    if (fs->bs < 65536u) return len;
    return (len & 65532u) | ((len & 3u) << 16);
}

/* Is there a well-formed entry at `off`?  Returns its rec_len, or 0. */
static uint32_t ext4_de_ok(const ext4_fs_t *fs, const uint8_t *blk, uint32_t off) {
    if (off + 8 > fs->bs) return 0;
    const uint8_t *de = blk + off;
    uint32_t rl = ext4_rec_len(fs, de);
    if (rl < 8 || (rl & 3) || rl > fs->bs - off || 8u + de[6] > rl) return 0;
    return rl;
}

static uint8_t ext4_ftype_to_vfs(uint8_t ft) {
    switch (ft) {
    case 2: return VFS_FLAG_DIR;
    case 3: return VFS_FLAG_CHARDEV;
    case 4: return VFS_FLAG_BLKDEV;
    case 5: return VFS_FLAG_FIFO;
    case 6: return VFS_FLAG_SOCK;
    case 7: return VFS_FLAG_SYMLINK;
    default: return VFS_FLAG_FILE;
    }
}

static vfs_node_t *ext4_get_node(ext4_fs_t *fs, uint32_t ino, const char *name);

static uint8_t ext4_mode_to_vfs(uint16_t mode) {
    switch (mode & EXT4_S_IFMT) {
    case EXT4_S_IFDIR:  return VFS_FLAG_DIR;
    case EXT4_S_IFCHR:  return VFS_FLAG_CHARDEV;
    case EXT4_S_IFBLK:  return VFS_FLAG_BLKDEV;
    case EXT4_S_IFIFO:  return VFS_FLAG_FIFO;
    case EXT4_S_IFSOCK: return VFS_FLAG_SOCK;
    case EXT4_S_IFLNK:  return VFS_FLAG_SYMLINK;
    default:            return VFS_FLAG_FILE;
    }
}

static int ext4_readdir(vfs_node_t *dir, uint32_t req, vfs_dirent_t *out) {
    ext4_vnode_t *vn = (ext4_vnode_t *)dir;
    ext4_fs_t *fs = vn->fs;
    if (vn->iflags & EXT4_INLINE_DATA_FL) return -1;
    uint32_t idx = 0, b = 0, off = 0;
    preempt_disable();
    if (vn->c_valid && req >= vn->c_idx) { idx = vn->c_idx; b = vn->c_blk; off = vn->c_off; }
    preempt_enable();

    uint8_t *buf = (uint8_t *)kmalloc(fs->bs);
    if (!buf) return -1;
    uint32_t nblk = ext4_dir_nblocks(vn);
    int loaded = 0, rc = -1;
    while (b < nblk) {
        if (!loaded) {
            int r = ext4_dir_block(vn, b, buf);
            if (r < 0) break;
            if (r > 0) { b++; off = 0; continue; }
            loaded = 1;
        }
        uint32_t rl = ext4_de_ok(fs, buf, off);
        if (!rl) { b++; off = 0; loaded = 0; continue; }
        const uint8_t *de = buf + off;
        uint32_t ino = rd32(de);
        uint32_t nlen = de[6];
        if (ino && nlen) {
            if (idx == req) {
                out->ino = ino;
                uint8_t ft = (fs->incompat & INCOMPAT_FILETYPE) ? de[7] : 0;
                if (ft == 0 || ft > 7) {
                    uint8_t raw[EXT4_INODE_MAX];
                    out->type = ext4_read_inode(fs, ino, raw) == 0
                              ? ext4_mode_to_vfs(rd16(raw)) : VFS_FLAG_FILE;
                } else {
                    out->type = ext4_ftype_to_vfs(ft);
                }
                memcpy(out->name, de + 8, nlen);
                out->name[nlen] = '\0';
                preempt_disable();
                vn->c_idx = idx; vn->c_blk = b; vn->c_off = off; vn->c_valid = 1;
                preempt_enable();
                rc = 0;
                break;
            }
            idx++;
        }
        off += rl;
        if (off >= fs->bs) { b++; off = 0; loaded = 0; }
    }
    kfree(buf);
    return rc;
}

/* Find `name` in one directory block; returns the inode number or 0. */
static uint32_t ext4_block_find(ext4_fs_t *fs, const uint8_t *buf,
                                const char *name, uint32_t nlen) {
    uint32_t off = 0, rl;
    while ((rl = ext4_de_ok(fs, buf, off)) != 0) {
        const uint8_t *de = buf + off;
        if (rd32(de) && de[6] == nlen && memcmp(de + 8, name, nlen) == 0)
            return rd32(de);
        off += rl;
    }
    return 0;
}

/* ── htree hashes ───────────────────────────────────────────────────────── */

#define DX_HASH_LEGACY    0
#define DX_HASH_HALF_MD4  1
#define DX_HASH_TEA       2

/* Pack the name into `num` 32-bit words, padded with a length pattern. */
static void dx_str2hashbuf(const char *msg, uint32_t len, uint32_t *buf, int num,
                           int is_unsigned) {
    uint32_t pad = len | (len << 8);
    pad |= pad << 16;
    uint32_t val = pad;
    if (len > (uint32_t)num * 4u) len = (uint32_t)num * 4u;
    for (uint32_t i = 0; i < len; i++) {
        int c = is_unsigned ? (int)(uint8_t)msg[i] : (int)(int8_t)msg[i];
        val = (uint32_t)c + (val << 8);
        if ((i & 3) == 3) { *buf++ = val; val = pad; num--; }
    }
    if (--num >= 0) *buf++ = val;
    while (--num >= 0) *buf++ = pad;
}

static inline uint32_t rol32(uint32_t x, uint32_t s) { return (x << s) | (x >> (32 - s)); }

/* Three MD4 rounds over eight input words (MD4 proper takes sixteen). */
static void dx_half_md4(uint32_t h[4], const uint32_t in[8]) {
    static const uint8_t order[3][8] = {
        { 0, 1, 2, 3, 4, 5, 6, 7 },
        { 1, 3, 5, 7, 0, 2, 4, 6 },
        { 3, 7, 2, 6, 1, 5, 0, 4 },
    };
    static const uint8_t shifts[3][4] = { { 3, 7, 11, 19 }, { 3, 5, 9, 13 }, { 3, 9, 11, 15 } };
    static const uint32_t konst[3] = { 0, 0x5A827999u, 0x6ED9EBA1u };
    uint32_t v[4] = { h[0], h[1], h[2], h[3] };
    for (int r = 0; r < 3; r++) {
        for (int j = 0; j < 8; j++) {
            /* Steps update a, d, c, b in turn. */
            int t = (4 - (j & 3)) & 3;
            uint32_t x = v[(t + 1) & 3], y = v[(t + 2) & 3], z = v[(t + 3) & 3];
            uint32_t f = r == 0 ? (z ^ (x & (y ^ z)))
                       : r == 1 ? ((x & y) | (x & z) | (y & z))
                       : (x ^ y ^ z);
            v[t] = rol32(v[t] + f + in[order[r][j]] + konst[r], shifts[r][j & 3]);
        }
    }
    for (int i = 0; i < 4; i++) h[i] += v[i];
}

static void dx_tea(uint32_t h[2], const uint32_t in[4]) {
    uint32_t sum = 0, b0 = h[0], b1 = h[1];
    for (int n = 0; n < 16; n++) {
        sum += 0x9E3779B9u;
        b0 += ((b1 << 4) + in[0]) ^ (b1 + sum) ^ ((b1 >> 5) + in[1]);
        b1 += ((b0 << 4) + in[2]) ^ (b0 + sum) ^ ((b0 >> 5) + in[3]);
    }
    h[0] += b0;
    h[1] += b1;
}

static uint32_t dx_legacy(const char *name, uint32_t len, int is_unsigned) {
    uint32_t h0 = 0x12A3FE2Du, h1 = 0x37ABE8F9u;
    for (uint32_t i = 0; i < len; i++) {
        int c = is_unsigned ? (int)(uint8_t)name[i] : (int)(int8_t)name[i];
        uint32_t h = h1 + (h0 ^ (uint32_t)(c * 7152373));
        if (h & 0x80000000u) h -= 0x7FFFFFFFu;
        h1 = h0;
        h0 = h;
    }
    return h0 << 1;
}

/* Returns 0 and the major hash, or -1 for an unknown hash version. */
static int dx_hash(const ext4_fs_t *fs, uint32_t version, const char *name,
                   uint32_t len, uint32_t *hash) {
    int is_unsigned = fs->hash_unsigned;
    uint32_t h[4] = { 0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u };
    if (fs->hash_seed[0] | fs->hash_seed[1] | fs->hash_seed[2] | fs->hash_seed[3])
        for (int i = 0; i < 4; i++) h[i] = fs->hash_seed[i];
    uint32_t in[8], out;
    switch (version) {
    case DX_HASH_LEGACY:
        out = dx_legacy(name, len, is_unsigned);
        break;
    case DX_HASH_HALF_MD4:
        for (uint32_t p = 0; ; p += 32) {
            dx_str2hashbuf(name + p, len - p, in, 8, is_unsigned);
            dx_half_md4(h, in);
            if (len - p <= 32) break;
        }
        out = h[1];
        break;
    case DX_HASH_TEA:
        for (uint32_t p = 0; ; p += 16) {
            dx_str2hashbuf(name + p, len - p, in, 4, is_unsigned);
            dx_tea(h, in);
            if (len - p <= 16) break;
        }
        out = h[0];
        break;
    default:
        return -1;
    }
    out &= ~1u;
    if (out == 0xFFFFFFFEu) out = 0xFFFFFFFCu;   /* reserved for end-of-dir */
    *hash = out;
    return 0;
}

/* Hash-index lookup.  1: found (*ino set), 0: not in the directory,
 * -1: the index cannot be used and a linear scan must decide. */
static int ext4_htree_find(ext4_vnode_t *vn, const char *name, uint32_t nlen,
                           uint8_t *buf, uint32_t *ino) {
    ext4_fs_t *fs = vn->fs;
    uint32_t nblk = ext4_dir_nblocks(vn);
    if (ext4_dir_block(vn, 0, buf) != 0) return -1;
    uint32_t info_len = buf[0x1D], levels = buf[0x1E];
    uint32_t version = buf[0x1C];
    if (rd32(buf + 0x18) != 0 || info_len != 8) return -1;
    if (levels > ((fs->incompat & INCOMPAT_LARGEDIR) ? 2u : 1u)) return -1;
    if (version > DX_HASH_TEA) return -1;
    uint32_t hash;
    if (dx_hash(fs, version, name, nlen, &hash) < 0) return -1;

    uint32_t eoff = 0x18 + info_len;          /* count/limit + entries */
    uint32_t blk = 0, next_hash = 0, next_blk = 0;
    int has_next = 0;
    for (uint32_t level = 0; ; level++) {
        uint32_t limit = rd16(buf + eoff), count = rd16(buf + eoff + 2);
        if (count == 0 || count > limit || eoff + limit * 8u > fs->bs) return -1;
        uint32_t lo = 1, hi = count;          /* first entry > hash, in [1,count] */
        while (lo < hi) {
            uint32_t mid = (lo + hi) / 2;
            if (rd32(buf + eoff + 8 * mid) > hash) hi = mid; else lo = mid + 1;
        }
        uint32_t at = lo - 1;
        blk = rd32(buf + eoff + 8 * at + 4) & 0x0FFFFFFFu;
        has_next = (at + 1 < count);
        if (has_next) {
            next_hash = rd32(buf + eoff + 8 * (at + 1));
            next_blk  = rd32(buf + eoff + 8 * (at + 1) + 4) & 0x0FFFFFFFu;
        }
        if (blk >= nblk) return -1;
        if (level == levels) break;
        if (ext4_dir_block(vn, blk, buf) != 0) return -1;
        eoff = 8;                             /* past the empty fake dirent */
    }
    for (int hops = 0; hops < 64; hops++) {
        int r = ext4_dir_block(vn, blk, buf);
        if (r < 0) return -1;
        if (r == 0 && (*ino = ext4_block_find(fs, buf, name, nlen)) != 0)
            return 1;
        /* Names with this hash continue in the next leaf only when the next
         * index entry starts at the same hash (its low bit marks that). */
        if (!has_next) return levels ? -1 : 0;
        if ((next_hash & ~1u) != hash) return 0;
        if (levels) return -1;
        blk = next_blk;
        if (blk >= nblk) return -1;
        /* Re-read the root to find the entry after the next one. */
        if (ext4_dir_block(vn, 0, buf) != 0) return -1;
        uint32_t count = rd16(buf + eoff + 2);
        has_next = 0;
        for (uint32_t i = 1; i + 1 < count; i++) {
            if ((rd32(buf + eoff + 8 * i + 4) & 0x0FFFFFFFu) == blk) {
                has_next  = 1;
                next_hash = rd32(buf + eoff + 8 * (i + 1));
                next_blk  = rd32(buf + eoff + 8 * (i + 1) + 4) & 0x0FFFFFFFu;
                break;
            }
        }
    }
    return -1;
}

static vfs_node_t *ext4_finddir(vfs_node_t *dir, const char *name) {
    ext4_vnode_t *vn = (ext4_vnode_t *)dir;
    ext4_fs_t *fs = vn->fs;
    uint32_t nlen = (uint32_t)strlen(name);
    if (nlen == 0 || nlen > 255 || (vn->iflags & EXT4_INLINE_DATA_FL)) return NULL;
    uint8_t *buf = (uint8_t *)kmalloc(fs->bs);
    if (!buf) return NULL;
    uint32_t ino = 0;
    int r = -1;
    if ((vn->iflags & EXT4_INDEX_FL) && (fs->compat & COMPAT_DIR_INDEX))
        r = ext4_htree_find(vn, name, nlen, buf, &ino);
    if (r < 0) {
        uint32_t nblk = ext4_dir_nblocks(vn);
        for (uint32_t b = 0; b < nblk && !ino; b++)
            if (ext4_dir_block(vn, b, buf) == 0)
                ino = ext4_block_find(fs, buf, name, nlen);
        if (ino && (vn->iflags & EXT4_INDEX_FL) && fs->htree_fallbacks++ < 4)
            printk("[EXT4] %s: htree index of dir %u unusable, scanned it\n",
                   fs->bp->name, (unsigned)vn->ino);
    }
    kfree(buf);
    return ino ? ext4_get_node(fs, ino, name) : NULL;
}

/* ── Nodes ──────────────────────────────────────────────────────────────── */

static int ext4_rofs_create(vfs_node_t *d, const char *n, uint32_t f) {
    (void)d; (void)n; (void)f; return EXT4_ERR_ROFS;
}
static int ext4_rofs_unlink(vfs_node_t *d, const char *n) {
    (void)d; (void)n; return EXT4_ERR_ROFS;
}
static int ext4_rofs_symlink(vfs_node_t *d, const char *n, const char *t) {
    (void)d; (void)n; (void)t; return EXT4_ERR_ROFS;
}
static int ext4_rofs_rename(vfs_node_t *a, const char *b, vfs_node_t *c, const char *d) {
    (void)a; (void)b; (void)c; (void)d; return EXT4_ERR_ROFS;
}
static int ext4_rofs_truncate(vfs_node_t *n, uint64_t s) {
    (void)n; (void)s; return EXT4_ERR_ROFS;
}
static void ext4_fill_attrs(ext4_vnode_t *vn, const uint8_t *raw);
/* vfs_setattr() has already changed the in-memory copy; put it back. */
static int ext4_rofs_setattr(vfs_node_t *n, uint32_t m, uint32_t u, uint32_t g) {
    (void)m; (void)u; (void)g;
    ext4_vnode_t *vn = (ext4_vnode_t *)n;
    uint8_t raw[EXT4_INODE_MAX];
    if (ext4_read_inode(vn->fs, vn->ino, raw) == 0) ext4_fill_attrs(vn, raw);
    return EXT4_ERR_ROFS;
}

static void ext4_retain(vfs_node_t *n) {
    ext4_fs_t *fs = ((ext4_vnode_t *)n)->fs;
    preempt_disable();
    fs->open_refs++;
    preempt_enable();
}
static void ext4_close(vfs_node_t *n) {
    ext4_fs_t *fs = ((ext4_vnode_t *)n)->fs;
    preempt_disable();
    if (fs->open_refs > 0) fs->open_refs--;
    preempt_enable();
}

static void ext4_fill_attrs(ext4_vnode_t *vn, const uint8_t *raw) {
    vfs_node_t *n = &vn->vnode;
    n->mask  = rd16(raw) & 07777u;
    n->uid   = rd16(raw + 0x02) | ((uint32_t)rd16(raw + 0x78) << 16);
    n->gid   = rd16(raw + 0x18) | ((uint32_t)rd16(raw + 0x7A) << 16);
    n->atime = rd32(raw + 0x08);
    n->ctime = rd32(raw + 0x0C);
    n->mtime = rd32(raw + 0x10);
}

static void ext4_fill_node(ext4_vnode_t *vn, const uint8_t *raw) {
    ext4_fs_t *fs = vn->fs;
    vfs_node_t *n = &vn->vnode;
    vn->mode   = rd16(raw);
    vn->iflags = rd32(raw + 0x20);
    vn->size   = rd32(raw + 0x04);
    if ((vn->mode & EXT4_S_IFMT) == EXT4_S_IFREG || (fs->incompat & INCOMPAT_LARGEDIR))
        vn->size |= (uint64_t)rd32(raw + 0x6C) << 32;
    vn->blocks   = rd32(raw + 0x1C) | ((uint64_t)rd16(raw + 0x74) << 32);
    vn->file_acl = rd32(raw + 0x68) | ((uint64_t)rd16(raw + 0x76) << 32);
    memcpy(vn->i_block, raw + 0x28, 60);
    if (fs->csum) {
        uint8_t le[4] = { (uint8_t)vn->ino, (uint8_t)(vn->ino >> 8),
                          (uint8_t)(vn->ino >> 16), (uint8_t)(vn->ino >> 24) };
        vn->csum_seed = crc32c(crc32c(fs->csum_seed, le, 4), raw + 0x64, 4);
    }
    vn->m_valid = 0;
    vn->c_valid = 0;

    ext4_fill_attrs(vn, raw);
    n->inode = vn->ino;
    n->dev   = fs->bp->rdev;
    /* Offsets in the VFS are 32-bit: a file past 4 GiB shows its first 4 GiB. */
    n->size  = vn->size;
    n->flags = ext4_mode_to_vfs(vn->mode);
    n->setattr_fn = ext4_rofs_setattr;
    n->retain_fn  = ext4_retain;
    n->close_fn   = ext4_close;
    switch (n->flags) {
    case VFS_FLAG_DIR:
        n->finddir_fn = ext4_finddir;
        n->readdir_fn = ext4_readdir;
        n->create_fn  = ext4_rofs_create;
        n->unlink_fn  = ext4_rofs_unlink;
        n->symlink_fn = ext4_rofs_symlink;
        n->rename_fn  = ext4_rofs_rename;
        break;
    case VFS_FLAG_FILE:
        n->read_fn     = ext4_read_node;
        n->truncate_fn = ext4_rofs_truncate;
        break;
    case VFS_FLAG_SYMLINK:
        n->read_fn = ext4_read_node;
        break;
    case VFS_FLAG_CHARDEV:
    case VFS_FLAG_BLKDEV: {
        /* Device number: old encoding in i_block[0], new one in i_block[1]. */
        uint32_t d0 = rd32(vn->i_block), d1 = rd32(vn->i_block + 4);
        if (d0) n->rdev = ((d0 >> 8) & 0xFFu) << 8 | (d0 & 0xFFu);
        else    n->rdev = ((d1 & 0xFFF00u) >> 8) << 8 | (d1 & 0xFFu) | ((d1 >> 12) & 0xFFF00u);
        break;
    }
    default:                                  /* FIFO, socket: no data */
        break;
    }
}

static vfs_node_t *ext4_get_node(ext4_fs_t *fs, uint32_t ino, const char *name) {
    uint8_t raw[EXT4_INODE_MAX];
    if (ext4_read_inode(fs, ino, raw) < 0) return NULL;
    preempt_disable();
    ext4_vnode_t **bucket = &fs->nodes[ino % EXT4_NODE_BUCKETS];
    ext4_vnode_t *vn = *bucket;
    while (vn && vn->ino != ino) vn = vn->hnext;
    if (vn) {
        preempt_enable();
        return &vn->vnode;
    }
    preempt_enable();
    ext4_vnode_t *nv = (ext4_vnode_t *)kmalloc(sizeof(ext4_vnode_t));
    if (!nv) return NULL;
    memset(nv, 0, sizeof(*nv));
    nv->fs  = fs;
    nv->ino = ino;
    strncpy(nv->vnode.name, name, 255);
    ext4_fill_node(nv, raw);
    preempt_disable();
    /* Someone else may have inserted it while we were filling ours. */
    for (vn = *bucket; vn && vn->ino != ino; vn = vn->hnext) {}
    if (!vn) {
        nv->hnext = *bucket;
        *bucket = nv;
        vn = nv;
        nv = NULL;
    }
    preempt_enable();
    if (nv) kfree(nv);
    return &vn->vnode;
}

/* ── Mount ──────────────────────────────────────────────────────────────── */

static int ext4_has_super(const ext4_fs_t *fs, uint32_t g, const uint8_t *sb) {
    if (g == 0) return 1;
    if (fs->compat & COMPAT_SPARSE_SUPER2)
        return g == rd32(sb + 0x24C) || g == rd32(sb + 0x250);
    if (!(fs->ro_compat & RO_COMPAT_SPARSE_SUPER) || g == 1) return 1;
    for (uint32_t base = 3; base <= 7; base += 2) {
        uint32_t p = base;
        while (p < g) p *= base;
        if (p == g) return 1;
    }
    return 0;
}

static void ext4_free_fs(ext4_fs_t *fs) {
    for (int i = 0; i < EXT4_NODE_BUCKETS; i++) {
        ext4_vnode_t *vn = fs->nodes[i];
        while (vn) {
            ext4_vnode_t *next = vn->hnext;
            kfree(vn);
            vn = next;
        }
    }
    if (fs->itable)  kfree(fs->itable);
    if (fs->bc_blk)  kfree(fs->bc_blk);
    if (fs->bc_age)  kfree(fs->bc_age);
    if (fs->bc_data) kfree(fs->bc_data);
    kfree(fs);
}

int ext4_busy(void *p) {
    return ((ext4_fs_t *)p)->open_refs > 0;
}

void ext4_release(void *p) {
    ext4_fs_t *fs = (ext4_fs_t *)p;
    printk("[EXT4] %s: unmounted\n", fs->bp->name);
    ext4_free_fs(fs);
}

static const char *ext4_feature_name(uint32_t bit) {
    switch (bit) {
    case INCOMPAT_COMPRESSION: return "compression";
    case INCOMPAT_JOURNAL_DEV: return "journal_dev";
    case INCOMPAT_DIRDATA:     return "dirdata";
    case INCOMPAT_INLINE_DATA: return "inline_data";
    case INCOMPAT_ENCRYPT:     return "encrypt";
    case INCOMPAT_CASEFOLD:    return "casefold";
    default:                   return "unknown";
    }
}

int ext4_mount_dev(blkpart_t *bp, vfs_node_t **root_out, ext4_fs_t **fs_out) {
    crc32c_init();
    uint8_t *sb = (uint8_t *)kmalloc(1024);
    if (!sb) return EXT4_ERR_NOMEM;
    ext4_fs_t *fs = NULL;
    int rc = EXT4_ERR_INVAL;
    if (blkpart_read(bp, 2, 2, sb) < 0) { rc = EXT4_ERR_IO; goto fail; }
    if (rd16(sb + 0x38) != 0xEF53u) {
        printk("[EXT4] %s: no ext2/3/4 superblock\n", bp->name);
        goto fail;
    }
    fs = (ext4_fs_t *)kmalloc(sizeof(ext4_fs_t));
    if (!fs) { rc = EXT4_ERR_NOMEM; goto fail; }
    memset(fs, 0, sizeof(*fs));
    fs->bp        = bp;
    fs->compat    = rd32(sb + 0x5C);
    fs->incompat  = rd32(sb + 0x60);
    fs->ro_compat = rd32(sb + 0x64);
    uint32_t rev  = rd32(sb + 0x4C);
    if (rev == 0) fs->compat = fs->incompat = fs->ro_compat = 0;

    if (fs->ro_compat & RO_COMPAT_METADATA_CSUM) {
        if (sb[0x175] != 1) {
            printk("[EXT4] %s: unknown checksum type %u\n", bp->name, (unsigned)sb[0x175]);
            goto fail;
        }
        if (crc32c(~0u, sb, 0x3FC) != rd32(sb + 0x3FC)) {
            printk("[EXT4] %s: superblock fails its checksum\n", bp->name);
            goto fail;
        }
        fs->csum = 1;
        fs->csum_seed = (fs->incompat & INCOMPAT_CSUM_SEED) ? rd32(sb + 0x270)
                      : crc32c(~0u, sb + 0x68, 16);
    }
    uint32_t bad = fs->incompat & ~INCOMPAT_READABLE;
    if (bad & INCOMPAT_RECOVER) {
        printk("[EXT4] %s: journal needs recovery; not mounting (run e2fsck "
               "or mount it once on Linux)\n", bp->name);
        rc = EXT4_ERR_UCLEAN;
        goto fail;
    }
    if (bad) {
        uint32_t bit = bad & (0u - bad);
        printk("[EXT4] %s: incompatible feature %s (0x%x) not supported\n",
               bp->name, ext4_feature_name(bit), (unsigned)bad);
        rc = EXT4_ERR_NOTSUP;
        goto fail;
    }

    uint32_t log = rd32(sb + 0x18);
    if (log > 6) goto fail;
    fs->log_bs = 10 + log;
    fs->bs     = 1024u << log;
    fs->spb    = fs->bs / 512u;
    fs->first_data_block = rd32(sb + 0x14);
    fs->bpg    = rd32(sb + 0x20);
    fs->ipg    = rd32(sb + 0x28);
    fs->inodes_count = rd32(sb + 0x00);
    fs->blocks_count = rd32(sb + 0x04);
    if (fs->incompat & INCOMPAT_64BIT)
        fs->blocks_count |= (uint64_t)rd32(sb + 0x150) << 32;
    fs->inode_size = rev ? rd16(sb + 0x58) : 128;
    fs->desc_size  = (fs->incompat & INCOMPAT_64BIT) ? rd16(sb + 0xFE) : 32;
    for (int i = 0; i < 4; i++) fs->hash_seed[i] = rd32(sb + 0xEC + 4 * i);
    fs->hash_unsigned = (rd32(sb + 0x160) & 0x2u) != 0;

    if (!fs->bpg || fs->bpg > fs->bs * 8 || !fs->ipg || fs->ipg > fs->bs * 8 ||
        fs->inode_size < 128 || fs->inode_size > EXT4_INODE_MAX ||
        fs->inode_size > fs->bs || (fs->inode_size & (fs->inode_size - 1)) ||
        fs->desc_size < 32 || fs->desc_size > fs->bs ||
        (fs->desc_size & (fs->desc_size - 1)) ||
        fs->first_data_block >= fs->blocks_count || !fs->inodes_count) {
        printk("[EXT4] %s: corrupt or unsupported superblock geometry\n", bp->name);
        goto fail;
    }
    if (fs->blocks_count * fs->spb > bp->nsect) {
        printk("[EXT4] %s: filesystem is larger than its partition\n", bp->name);
        goto fail;
    }
    /* blocks_count fits 32 bits now: the partition is under 2^32 sectors. */
    uint64_t groups = ((uint32_t)fs->blocks_count - fs->first_data_block + fs->bpg - 1) / fs->bpg;
    if (groups == 0 || groups > 0x100000u ||
        (uint64_t)fs->inodes_count > groups * fs->ipg) {
        printk("[EXT4] %s: inconsistent group count\n", bp->name);
        goto fail;
    }
    fs->groups = (uint32_t)groups;

    /* Block cache */
    uint32_t slots = EXT4_CACHE_BYTES / fs->bs;
    if (slots < 16) slots = 16;
    uint32_t sets = 1;
    while (sets * 2 * EXT4_CACHE_WAYS <= slots) sets *= 2;
    fs->bc_sets = sets;
    slots = sets * EXT4_CACHE_WAYS;
    fs->bc_blk  = (uint64_t *)kmalloc(slots * sizeof(uint64_t));
    fs->bc_age  = (uint32_t *)kmalloc(slots * sizeof(uint32_t));
    fs->bc_data = (uint8_t *)kmalloc(slots * fs->bs);
    fs->itable  = (uint64_t *)kmalloc(fs->groups * sizeof(uint64_t));
    if (!fs->bc_blk || !fs->bc_age || !fs->bc_data || !fs->itable) {
        rc = EXT4_ERR_NOMEM;
        goto fail;
    }
    for (uint32_t i = 0; i < slots; i++) { fs->bc_blk[i] = ~0ull; fs->bc_age[i] = 0; }

    /* Group descriptors: only the inode table location is needed to read. */
    {
        uint32_t per = fs->bs / fs->desc_size;
        uint32_t first_meta = (fs->incompat & INCOMPAT_META_BG) ? rd32(sb + 0x104) : 0xFFFFFFFFu;
        uint8_t *gd = (uint8_t *)kmalloc(fs->bs);
        if (!gd) { rc = EXT4_ERR_NOMEM; goto fail; }
        for (uint32_t g = 0; g < fs->groups; g++) {
            uint32_t db = g / per;
            if (g % per == 0) {
                uint64_t loc;
                if (db < first_meta) {
                    loc = (uint64_t)fs->first_data_block + 1 + db;
                } else {
                    uint32_t g0 = db * per;
                    loc = (uint64_t)fs->first_data_block + (uint64_t)g0 * fs->bpg +
                          (ext4_has_super(fs, g0, sb) ? 1 : 0);
                }
                if (ext4_bread(fs, loc, 0, fs->bs, gd) < 0) {
                    kfree(gd);
                    rc = EXT4_ERR_IO;
                    goto fail;
                }
            }
            const uint8_t *d = gd + (g % per) * fs->desc_size;
            uint64_t it = rd32(d + 0x08);
            if (fs->desc_size >= 64) it |= (uint64_t)rd32(d + 0x28) << 32;
            uint64_t itblocks = ((uint64_t)fs->ipg * fs->inode_size + fs->bs - 1) >> fs->log_bs;
            if (it == 0 || it + itblocks > fs->blocks_count) {
                printk("[EXT4] %s: group %u has a bad inode table location\n",
                       bp->name, (unsigned)g);
                kfree(gd);
                goto fail;
            }
            fs->itable[g] = it;
        }
        kfree(gd);
    }

    vfs_node_t *root = ext4_get_node(fs, 2, "/");
    if (!root || root->flags != VFS_FLAG_DIR) {
        printk("[EXT4] %s: root inode unreadable\n", bp->name);
        if (fs->csum_errors) rc = EXT4_ERR_IO;
        goto fail;
    }
    uint16_t state = rd16(sb + 0x3A);
    if (!(state & 1))
        printk("[EXT4] %s: warning: not cleanly unmounted\n", bp->name);
    if (state & 2)
        printk("[EXT4] %s: warning: filesystem has errors recorded\n", bp->name);
    printk("[EXT4] %s: mounted read-only (block %u, %u groups, %s%s%s%s)\n",
           bp->name, (unsigned)fs->bs, (unsigned)fs->groups,
           (fs->incompat & INCOMPAT_EXTENTS) ? "extents " : "",
           (fs->incompat & INCOMPAT_64BIT) ? "64bit " : "",
           (fs->incompat & INCOMPAT_FLEX_BG) ? "flex_bg " : "",
           fs->csum ? "metadata_csum" : "");
    kfree(sb);
    *root_out = root;
    *fs_out = fs;
    return 0;

fail:
    if (fs) ext4_free_fs(fs);
    kfree(sb);
    return rc;
}
