/*
 * exFAT, read-write (see exfat.h).
 *
 * Written for MaeroOS from Microsoft's "exFAT file system specification"
 * (2019, public): the boot region and its checksum, the FAT, the allocation
 * bitmap, the up-case table and its compression, the file, stream-extension
 * and file-name directory entries with SetChecksum and NameHash, timestamps
 * with 10 ms increments and UTC offsets, NoFatChain (contiguous) allocation,
 * and the VolumeDirty flag.  The OSDev wiki's exFAT page was read for
 * orientation.  No code was copied from Linux (fs/exfat, GPL), exfatprogs or
 * any other implementation.
 *
 * The layout follows fs/vfat.c:
 *   - a small write-back cache of sectors for metadata (FAT, bitmap,
 *     directories, boot sector), flushed when each operation ends, so every
 *     system call leaves the volume consistent;
 *   - file data moves straight between the caller's buffer and the device,
 *     in runs of contiguous clusters;
 *   - one node per entry set, keyed by the byte position of its file entry on
 *     the volume, kept until umount (the mount table matches mountpoints by
 *     node identity), re-keyed when a rename moves the entry set;
 *   - one sleeping lock per instance serialises every operation.
 *
 * Sizes are 64-bit on disk and in the driver.  The VFS has 32-bit offsets,
 * so a file of 4 GiB or more shows (and can be read and written in) its first
 * 4 GiB - 1 bytes; its real length is kept.
 *
 * Everything read from the medium is checked before it is used: the boot
 * region checksum and geometry, cluster numbers and chain links, lengths
 * against the cluster heap, entry-set counts and checksums, name lengths
 * (copies into fixed buffers are bounded by the buffers, never by on-disk
 * counts).
 */
#include "exfat.h"
#include "vfs.h"
#include "../mm/heap.h"
#include "../lib/string.h"
#include "../kernel/printk.h"
#include "../proc/scheduler.h"
#include "../proc/process.h"
#include "../drivers/rtc.h"
#include "../arch/i686/cpu/pit.h"
#include <stddef.h>
#include <stdint.h>

#define E_PERM        (-1)
#define E_NOENT       (-2)
#define E_IO          (-5)
#define E_NOMEM       (-12)
#define E_EXIST       (-17)
#define E_NOTDIR      (-20)
#define E_ISDIR       (-21)
#define E_INVAL       (-22)
#define E_FBIG        (-27)
#define E_NOSPC       (-28)
#define E_ROFS        (-30)
#define E_NAMETOOLONG (-36)
#define E_NOTEMPTY    (-39)

#define ATTR_RO     0x01
#define ATTR_HIDDEN 0x02
#define ATTR_SYSTEM 0x04
#define ATTR_DIR    0x10
#define ATTR_ARCH   0x20

/* Directory entry types (EntryType byte, InUse bit set). */
#define ET_EOD      0x00
#define ET_BITMAP   0x81
#define ET_UPCASE   0x82
#define ET_LABEL    0x83
#define ET_FILE     0x85
#define ET_STREAM   0xC0
#define ET_NAME     0xC1
#define ET_INUSE    0x80

/* GeneralSecondaryFlags */
#define SF_ALLOC    0x01
#define SF_NOFAT    0x02

#define VF_DIRTY    0x0002               /* VolumeFlags */

#define FAT_EOC     0xFFFFFFFFu
#define FAT_BAD     0xFFFFFFF7u

#define XF_NODE_BUCKETS 256
#define XF_CACHE_BYTES  (256u * 1024u)
#define XF_CACHE_WAYS   4
#define XF_MAX_DIRBYTES (256u << 20)     /* the specification's limit */
#define XF_ZBUF         8192u
#define XF_MAX_SEC      18               /* secondaries of a file entry set */
#define NAME_UNITS      255

typedef struct exfat_vnode xf_vnode_t;

typedef struct {
    uint32_t sec;                       /* ~0 = empty */
    uint32_t age;
    uint8_t  dirty;
} bc_meta_t;

struct exfat_fs {
    blkpart_t *bp;
    volatile int lock;
    int        ro;
    int        ro_only;                 /* never read-write: two FATs, the backup boot
                                         * region, or no usable up-case table */
    uint32_t   bps, s512;               /* bytes per sector, 512-byte units per sector */
    uint32_t   spc, cshift, cb;         /* sectors per cluster, log2 and bytes per cluster */
    uint32_t   total;                   /* sectors in the volume */
    uint32_t   fat_start, fat_len;      /* the active FAT */
    uint32_t   heap;                    /* first sector of cluster 2 */
    uint32_t   nclus;                   /* clusters 2 .. nclus+1 */
    uint32_t   root_clus;
    uint32_t   bm_clus;                 /* allocation bitmap, contiguous */
    uint32_t   bm_bytes;
    /* Clusters of the volume's own structures (bitmap, up-case table, the
     * root's first cluster): never handed to or freed through a file. */
    uint32_t   sys_lo[3], sys_n[3];
    uint16_t  *upcase;                  /* 65536 entries */
    uint32_t   free_count;              /* 0xFFFFFFFF = unknown */
    uint32_t   next_free;
    int        was_dirty;
    uint32_t   uid, gid, dmask, fmask;
    /* metadata cache */
    uint32_t   bc_sets;
    bc_meta_t *bc;
    uint8_t   *bc_data;
    uint32_t   bc_clock;
    uint8_t   *sbuf;                    /* one sector, for partial writes */
    uint8_t   *zbuf;                    /* zeros */
    xf_vnode_t *nodes[XF_NODE_BUCKETS];
    xf_vnode_t *dead;                   /* unlinked nodes, freed at umount */
    xf_vnode_t *root;
    int        open_refs;
    uint32_t   io_errors;
};

struct exfat_vnode {
    vfs_node_t  vnode;                  /* first: the VFS hands us &vnode */
    exfat_fs_t *fs;
    xf_vnode_t *hnext;
    xf_vnode_t *parent;                 /* containing directory (root: itself) */
    uint64_t    key;                    /* byte position of the file entry; 0 = root */
    uint32_t    ent_idx;                /* index of the file entry in parent */
    uint32_t    nsec;                   /* its SecondaryCount */
    uint32_t    first;                  /* first cluster, 0 = none */
    int         nofat;                  /* clusters first.. are contiguous, FAT unused */
    int         bad;                    /* the entry set describes impossible storage */
    uint64_t    size;                   /* DataLength */
    uint64_t    valid;                  /* ValidDataLength: bytes past it read as zeros */
    uint16_t    attr;
    int         is_dir;
    int         refs;
    int         deleted;
    uint32_t    atime, mtime, ctime;
    /* chain cursor: cluster c_clus is the c_idx-th of the chain */
    uint32_t    c_idx, c_clus;
    /* readdir cursor, as in fs/vfat.c */
    uint32_t    r_idx, r_pos, r_prev;
    int         r_valid;
};

/* ── Little-endian helpers ──────────────────────────────────────────────── */

static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
static inline uint64_t rd64(const uint8_t *p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}
static inline void wr16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static inline void wr64(uint8_t *p, uint64_t v) { wr32(p, (uint32_t)v); wr32(p + 4, (uint32_t)(v >> 32)); }

/* ── Locking ────────────────────────────────────────────────────────────── */

static int bc_flush(exfat_fs_t *fs);

static void fs_lock(exfat_fs_t *fs) {
    while (__sync_lock_test_and_set(&fs->lock, 1)) {
        if (current_proc) yield();
    }
}

/* End of an operation: everything it changed goes to the device. */
static int fs_unlock(exfat_fs_t *fs) {
    int rc = 0;
    if (!fs->ro) rc = bc_flush(fs);
    __sync_lock_release(&fs->lock);
    return rc;
}

/* ── Time ───────────────────────────────────────────────────────────────── */

/* Seconds since the epoch, and the 10 ms units within the second. */
static uint32_t xf_now(uint32_t *cs) {
    uint32_t t = pit_ticks();
    if (cs) *cs = t % 100u;
    return rtc_boot_epoch() + t / 100U;
}

static void civil_from_days(int32_t z, int *y, unsigned *m, unsigned *d) {
    z += 719468;
    int32_t era = (z >= 0 ? z : z - 146096) / 146097;
    uint32_t doe = (uint32_t)(z - era * 146097);
    uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int32_t yy = (int32_t)yoe + era * 400;
    uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    uint32_t mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = (int)(yy + (*m <= 2));
}

static int32_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    int32_t era = (y >= 0 ? y : y - 399) / 400;
    uint32_t yoe = (uint32_t)(y - era * 400);
    uint32_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int32_t)doe - 719468;
}

/* exFAT timestamp (DOS date and time in one 32-bit word) of UTC time `t`;
 * the caller stores UtcOffset 0x80 (valid, +0) next to it. */
static uint32_t unix_to_ts(uint32_t t) {
    int y; unsigned mo, d;
    civil_from_days((int32_t)(t / 86400u), &y, &mo, &d);
    uint32_t s = t % 86400u;
    if (y < 1980) { y = 1980; mo = 1; d = 1; s = 0; }
    if (y > 2107) { y = 2107; mo = 12; d = 31; s = 86399; }
    return ((uint32_t)(y - 1980) << 25) | (mo << 21) | (d << 16) |
           ((s / 3600) << 11) | (((s / 60) % 60) << 5) | ((s % 60) / 2);
}

/* Back to UTC: `ms10` adds the odd second, `utc` is the UtcOffset byte (bit 7
 * set: bits 0-6 are a signed count of 15-minute steps; else local time,
 * taken as UTC). */
static uint32_t ts_to_unix(uint32_t ts, uint8_t ms10, uint8_t utc) {
    unsigned d = (ts >> 16) & 31, mo = (ts >> 21) & 15;
    int y = 1980 + (int)(ts >> 25);
    if (d == 0 || mo == 0 || mo > 12) return 315532800u;          /* 1980-01-01 */
    uint32_t s = ((ts >> 11) & 31) * 3600u + ((ts >> 5) & 63) * 60u + (ts & 31) * 2u;
    if (ms10 < 200) s += ms10 / 100u;
    int64_t t = (int64_t)days_from_civil(y, mo, d) * 86400 + s;
    if (utc & 0x80) {
        int off = utc & 0x7F;
        if (off & 0x40) off -= 0x80;
        t -= (int64_t)off * 15 * 60;
    }
    if (t < 0) t = 0;
    if (t > 0xFFFFFFFFll) t = 0xFFFFFFFFll;
    return (uint32_t)t;
}

/* ── Device I/O ─────────────────────────────────────────────────────────── */

/* `n` whole sectors (of bps bytes) at `sec` of the volume. */
static int dev_io(exfat_fs_t *fs, uint32_t sec, uint32_t n, void *buf, int wr) {
    if (sec >= fs->total || n > fs->total - sec) return -1;
    uint64_t s = (uint64_t)sec * fs->s512, cnt = (uint64_t)n * fs->s512;
    uint8_t *p = (uint8_t *)buf;
    while (cnt) {
        uint32_t k = cnt > 128 ? 128 : (uint32_t)cnt;
        int r = wr ? blkpart_write(fs->bp, (uint32_t)s, k, p)
                   : blkpart_read(fs->bp, (uint32_t)s, k, p);
        if (r < 0) {
            if (fs->io_errors++ < 8)
                printk("[EXFAT] %s: %s error at sector %u\n", fs->bp->name,
                       wr ? "write" : "read", (unsigned)s);
            return -1;
        }
        s += k; cnt -= k; p += k * 512u;
    }
    return 0;
}

/* ── Metadata cache (as fs/vfat.c) ──────────────────────────────────────── */

static int bc_writeback(exfat_fs_t *fs, uint32_t w) {
    bc_meta_t *m = &fs->bc[w];
    int rc = dev_io(fs, m->sec, 1, fs->bc_data + (uint64_t)w * fs->bps, 1);
    if (rc == 0) m->dirty = 0;
    return rc;
}

static uint8_t *bc_get(exfat_fs_t *fs, uint32_t sec, int load, uint32_t *slot) {
    uint32_t set = sec & (fs->bc_sets - 1u);
    uint32_t base = set * XF_CACHE_WAYS, victim = base;
    for (uint32_t w = base; w < base + XF_CACHE_WAYS; w++) {
        if (fs->bc[w].sec == sec) {
            fs->bc[w].age = ++fs->bc_clock;
            if (slot) *slot = w;
            return fs->bc_data + (uint64_t)w * fs->bps;
        }
        if (fs->bc[w].age < fs->bc[victim].age) victim = w;
    }
    if (fs->bc[victim].sec != ~0u && fs->bc[victim].dirty &&
        bc_writeback(fs, victim) < 0)
        return NULL;
    uint8_t *data = fs->bc_data + (uint64_t)victim * fs->bps;
    fs->bc[victim].sec = ~0u;
    fs->bc[victim].dirty = 0;
    if (load && dev_io(fs, sec, 1, data, 0) < 0) return NULL;
    if (!load) memset(data, 0, fs->bps);
    fs->bc[victim].sec = sec;
    fs->bc[victim].age = ++fs->bc_clock;
    if (slot) *slot = victim;
    return data;
}

static void bc_mark(exfat_fs_t *fs, uint32_t slot) {
    fs->bc[slot].dirty = 1;
}

static int bc_flush(exfat_fs_t *fs) {
    int rc = 0;
    uint32_t n = fs->bc_sets * XF_CACHE_WAYS;
    for (uint32_t w = 0; w < n; w++)
        if (fs->bc[w].sec != ~0u && fs->bc[w].dirty && bc_writeback(fs, w) < 0)
            rc = -1;
    return rc;
}

/* Forget sectors [sec, sec+n) without writing them: clusters that are freed
 * or handed to file data, which bypasses the cache.  Large ranges scan the
 * cache once instead of probing every sector. */
static void bc_drop(exfat_fs_t *fs, uint32_t sec, uint32_t n) {
    uint32_t slots = fs->bc_sets * XF_CACHE_WAYS;
    if (n > slots) {
        for (uint32_t w = 0; w < slots; w++)
            if (fs->bc[w].sec != ~0u && fs->bc[w].sec >= sec && fs->bc[w].sec - sec < n) {
                fs->bc[w].sec = ~0u; fs->bc[w].dirty = 0; fs->bc[w].age = 0;
            }
        return;
    }
    for (uint32_t s = sec; s < sec + n; s++) {
        uint32_t base = (s & (fs->bc_sets - 1u)) * XF_CACHE_WAYS;
        for (uint32_t w = base; w < base + XF_CACHE_WAYS; w++)
            if (fs->bc[w].sec == s) { fs->bc[w].sec = ~0u; fs->bc[w].dirty = 0; fs->bc[w].age = 0; }
    }
}

/* ── Clusters, FAT and bitmap ───────────────────────────────────────────── */

static int clus_ok(const exfat_fs_t *fs, uint32_t c) {
    return c >= 2 && c - 2 < fs->nclus;
}

static uint32_t clus_sec(const exfat_fs_t *fs, uint32_t c) {
    return fs->heap + (c - 2) * fs->spc;
}

static uint32_t clusters_for(const exfat_fs_t *fs, uint64_t bytes) {
    return (uint32_t)((bytes + fs->cb - 1) >> fs->cshift);
}

static int fat_rd(exfat_fs_t *fs, uint32_t c, uint32_t *v) {
    if (c >= fs->nclus + 2) return -1;
    uint32_t off = c * 4;
    uint8_t *p = bc_get(fs, fs->fat_start + off / fs->bps, 1, NULL);
    if (!p) return -1;
    *v = rd32(p + off % fs->bps);
    return 0;
}

static int fat_wr(exfat_fs_t *fs, uint32_t c, uint32_t v) {
    if (c >= fs->nclus + 2) return -1;
    uint32_t off = c * 4, slot;
    uint8_t *p = bc_get(fs, fs->fat_start + off / fs->bps, 1, &slot);
    if (!p) return -1;
    wr32(p + off % fs->bps, v);
    bc_mark(fs, slot);
    return 0;
}

/* The cluster after `c` in a FAT chain: 0 at the end; -1 for an I/O error or
 * a link to a cluster that does not exist (a corrupt chain). */
static int fat_next(exfat_fs_t *fs, uint32_t c, uint32_t *next) {
    uint32_t v;
    if (fat_rd(fs, c, &v) < 0) return -1;
    if (v >= 0xFFFFFFF8u) { *next = 0; return 0; }
    if (!clus_ok(fs, v)) {
        if (fs->io_errors++ < 8)
            printk("[EXFAT] %s: cluster %u links to %x: corrupt chain (run fsck.exfat)\n",
                   fs->bp->name, (unsigned)c, (unsigned)v);
        return -1;
    }
    *next = v;
    return 0;
}

/* Bitmap byte holding cluster c's bit. */
static uint8_t *bm_byte(exfat_fs_t *fs, uint32_t c, uint32_t *slot) {
    uint32_t bit = c - 2, byte = bit >> 3;
    uint64_t sec = (uint64_t)clus_sec(fs, fs->bm_clus) + byte / fs->bps;
    uint8_t *p = bc_get(fs, (uint32_t)sec, 1, slot);
    return p ? p + byte % fs->bps : NULL;
}

/* 1 allocated, 0 free, -1 error. */
static int bm_get(exfat_fs_t *fs, uint32_t c) {
    if (!clus_ok(fs, c)) return -1;
    uint8_t *p = bm_byte(fs, c, NULL);
    if (!p) return -1;
    return (*p >> ((c - 2) & 7)) & 1;
}

/* Does [c, c+n) overlap the volume's own structures? */
static int sys_overlap(const exfat_fs_t *fs, uint32_t c, uint32_t n) {
    for (int i = 0; i < 3; i++)
        if (fs->sys_n[i] && c < fs->sys_lo[i] + fs->sys_n[i] && fs->sys_lo[i] < c + n)
            return 1;
    return 0;
}

static int bm_set(exfat_fs_t *fs, uint32_t c, int on) {
    if (!clus_ok(fs, c)) return -1;
    /* A crafted entry pointing at the bitmap or the root must not get them
     * freed (and handed out again). */
    if (!on && sys_overlap(fs, c, 1)) return 0;
    uint32_t slot;
    uint8_t *p = bm_byte(fs, c, &slot);
    if (!p) return -1;
    uint8_t m = (uint8_t)(1u << ((c - 2) & 7));
    int was = (*p & m) != 0;
    if (on) *p |= m; else *p &= (uint8_t)~m;
    bc_mark(fs, slot);
    if (was != on && fs->free_count != 0xFFFFFFFFu)
        fs->free_count += on ? (uint32_t)-1 : 1u;
    return 0;
}

/* Free clusters [c, c+n): a contiguous run. */
static int bm_free_run(exfat_fs_t *fs, uint32_t c, uint32_t n) {
    if (!clus_ok(fs, c) || n > fs->nclus - (c - 2)) return -1;
    for (uint32_t i = 0; i < n; i++)
        if (bm_set(fs, c + i, 0) < 0) return -1;
    bc_drop(fs, clus_sec(fs, c), n * fs->spc);
    return 0;
}

/* A free cluster, searching from `hint` (wrapping); the bit is set. */
static int clus_alloc(exfat_fs_t *fs, uint32_t hint, uint32_t *out) {
    if (!clus_ok(fs, hint)) hint = fs->next_free;
    if (!clus_ok(fs, hint)) hint = 2;
    uint32_t i = 0;
    while (i < fs->nclus) {
        uint32_t c = 2 + (hint - 2 + i) % fs->nclus;
        /* Skip full bytes eight clusters at a time. */
        if (((c - 2) & 7) == 0 && fs->nclus - (c - 2) >= 8) {
            uint8_t *p = bm_byte(fs, c, NULL);
            if (!p) return E_IO;
            if (*p == 0xFF) { i += 8; continue; }
        }
        int b = bm_get(fs, c);
        if (b < 0) return E_IO;
        if (b == 0) {
            if (bm_set(fs, c, 1) < 0) return E_IO;
            fs->next_free = c + 1 < fs->nclus + 2 ? c + 1 : 2;
            bc_drop(fs, clus_sec(fs, c), fs->spc);
            *out = c;
            return 0;
        }
        i++;
    }
    return E_NOSPC;
}

/* Free a FAT chain from `c` (at most `max` clusters; the chain's links are
 * left as they were, as the specification allows for free clusters). */
static int chain_free(exfat_fs_t *fs, uint32_t c, uint32_t max) {
    for (uint32_t n = 0; clus_ok(fs, c) && n < max; n++) {
        uint32_t next;
        int r = fat_next(fs, c, &next);
        if (bm_set(fs, c, 0) < 0) return E_IO;
        bc_drop(fs, clus_sec(fs, c), fs->spc);
        if (r < 0) return E_IO;
        c = next;
    }
    return 0;
}

static int set_volflags(exfat_fs_t *fs, int dirty) {
    uint32_t slot;
    uint8_t *p = bc_get(fs, 0, 1, &slot);
    if (!p) return -1;
    uint16_t f = rd16(p + 106);
    f = dirty ? (uint16_t)(f | VF_DIRTY) : (uint16_t)(f & ~VF_DIRTY);
    wr16(p + 106, f);
    /* PercentInUse, like VolumeFlags, is outside the boot checksum. */
    if (fs->free_count != 0xFFFFFFFFu && fs->nclus)
        p[112] = (uint8_t)((uint64_t)(fs->nclus - fs->free_count) * 100u / fs->nclus);
    bc_mark(fs, slot);
    return bc_flush(fs);
}

/* ── Unicode ────────────────────────────────────────────────────────────── */

/* Built-in up-case mapping, used when the volume's table is missing or does
 * not match its checksum: ASCII, Latin-1, Latin Extended-A (with the Turkish
 * letters), Greek and Cyrillic. */
static uint32_t uc_default(uint32_t c) {
    if (c < 0x80) return (c >= 'a' && c <= 'z') ? c - 32 : c;
    if (c < 0x100) {
        if ((c >= 0xE0 && c <= 0xFE && c != 0xF7)) return c - 32;
        if (c == 0xFF) return 0x178;
        return c;
    }
    if (c < 0x180) {
        /* Dotted and dotless i fold to themselves, as in the table
         * mkfs.exfat and Windows write. */
        if (c == 0x130 || c == 0x131 || c == 0x138 || c == 0x149 || c == 0x178) return c;
        if (c == 0x17F) return 'S';
        if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E))
            return (c & 1) ? c : c - 1;
        return (c & 1) ? c - 1 : c;
    }
    if (c >= 0x3B1 && c <= 0x3C9 && c != 0x3C2) return c - 32;
    if (c >= 0x430 && c <= 0x44F) return c - 32;
    if (c >= 0x450 && c <= 0x45F) return c - 80;
    return c;
}

static uint16_t upc(const exfat_fs_t *fs, uint16_t c) {
    return fs->upcase[c];
}

static int32_t utf8_next(const char **pp) {
    const uint8_t *p = (const uint8_t *)*pp;
    uint32_t c = *p;
    if (!c) return 0;
    int n;
    uint32_t min;
    if (c < 0x80) { *pp += 1; return (int32_t)c; }
    else if ((c & 0xE0) == 0xC0) { n = 1; c &= 0x1F; min = 0x80; }
    else if ((c & 0xF0) == 0xE0) { n = 2; c &= 0x0F; min = 0x800; }
    else if ((c & 0xF8) == 0xF0) { n = 3; c &= 0x07; min = 0x10000; }
    else return -1;
    for (int i = 1; i <= n; i++) {
        if ((p[i] & 0xC0) != 0x80) return -1;
        c = (c << 6) | (p[i] & 0x3F);
    }
    if (c < min || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) return -1;
    *pp += n + 1;
    return (int32_t)c;
}

static int utf8_put(char *out, uint32_t *len, uint32_t cap, uint32_t c) {
    uint8_t b[4];
    int n;
    if (c < 0x80) { b[0] = (uint8_t)c; n = 1; }
    else if (c < 0x800) { b[0] = (uint8_t)(0xC0 | (c >> 6)); b[1] = (uint8_t)(0x80 | (c & 0x3F)); n = 2; }
    else if (c < 0x10000) {
        b[0] = (uint8_t)(0xE0 | (c >> 12)); b[1] = (uint8_t)(0x80 | ((c >> 6) & 0x3F));
        b[2] = (uint8_t)(0x80 | (c & 0x3F)); n = 3;
    } else {
        b[0] = (uint8_t)(0xF0 | (c >> 18)); b[1] = (uint8_t)(0x80 | ((c >> 12) & 0x3F));
        b[2] = (uint8_t)(0x80 | ((c >> 6) & 0x3F)); b[3] = (uint8_t)(0x80 | (c & 0x3F)); n = 4;
    }
    if (*len + (uint32_t)n + 1 > cap) return -1;
    for (int i = 0; i < n; i++) out[(*len)++] = (char)b[i];
    out[*len] = '\0';
    return 0;
}

/* Characters a name may not hold (specification, FileName field). */
static int name_char_ok(uint32_t c) {
    return c >= 0x20 && !(c < 0x80 && strchr("\"*/:<>?\\|", (int)c));
}

/* `name` (UTF-8) as UTF-16 units in u[0..*nu).  Trailing dots are dropped,
 * as Windows and Linux do.  `strict` also refuses characters a stored name
 * may not hold.  0 or a negative errno. */
static int name_to_u16(const char *name, uint16_t *u, uint32_t *nu, int strict) {
    uint32_t k = 0;
    const char *p = name;
    for (;;) {
        int32_t c = utf8_next(&p);
        if (c < 0) return E_INVAL;
        if (!c) break;
        if (strict && !name_char_ok((uint32_t)c)) return E_INVAL;
        if (c >= 0x10000) {
            if (k + 2 > NAME_UNITS) return E_NAMETOOLONG;
            uint32_t x = (uint32_t)c - 0x10000;
            u[k++] = (uint16_t)(0xD800 | (x >> 10));
            u[k++] = (uint16_t)(0xDC00 | (x & 0x3FF));
        } else {
            if (k + 1 > NAME_UNITS) return E_NAMETOOLONG;
            u[k++] = (uint16_t)c;
        }
    }
    while (k && u[k - 1] == '.') k--;
    if (k == 0) return E_INVAL;
    *nu = k;
    return 0;
}

static uint16_t name_hash(const exfat_fs_t *fs, const uint16_t *u, uint32_t n) {
    uint16_t h = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint16_t c = upc(fs, u[i]);
        h = (uint16_t)(((h & 1) ? 0x8000 : 0) + (h >> 1) + (c & 0xFF));
        h = (uint16_t)(((h & 1) ? 0x8000 : 0) + (h >> 1) + (c >> 8));
    }
    return h;
}

static uint16_t set_checksum(const uint8_t *set, uint32_t nent) {
    uint16_t s = 0;
    for (uint32_t i = 0; i < nent * 32; i++) {
        if (i == 2 || i == 3) continue;
        s = (uint16_t)(((s & 1) ? 0x8000 : 0) + (s >> 1) + set[i]);
    }
    return s;
}

static uint32_t boot_checksum_add(uint32_t s, const uint8_t *p, uint32_t n, int first) {
    for (uint32_t i = 0; i < n; i++) {
        if (first && (i == 106 || i == 107 || i == 112)) continue;
        s = ((s & 1) ? 0x80000000u : 0) + (s >> 1) + p[i];
    }
    return s;
}

/* ── Directories ────────────────────────────────────────────────────────── */

/* Cluster `ci` of node vn's storage.  0, 1 past the end, -1 error. */
static int node_clus(exfat_fs_t *fs, xf_vnode_t *vn, uint32_t ci, uint32_t *out) {
    if (!vn->first || vn->bad) return vn->bad ? -1 : 1;
    if ((uint64_t)ci >= clusters_for(fs, vn->size)) return 1;
    if (vn->nofat) {
        uint32_t c = vn->first + ci;
        if (!clus_ok(fs, c) || c < vn->first) return -1;
        *out = c;
        return 0;
    }
    uint32_t c, k;
    if (vn->c_clus && vn->c_idx <= ci) { c = vn->c_clus; k = vn->c_idx; }
    else { c = vn->first; k = 0; }
    if (!clus_ok(fs, c)) return -1;
    while (k < ci) {
        uint32_t next;
        if (fat_next(fs, c, &next) < 0) return -1;
        if (!next) return -1;              /* shorter than its length */
        c = next;
        k++;
    }
    vn->c_idx = k;
    vn->c_clus = c;
    *out = c;
    return 0;
}

/* Sector and offset of entry `idx` of directory `d`: 0, 1 past the end, -1. */
static int dir_pos(exfat_fs_t *fs, xf_vnode_t *d, uint32_t idx, uint32_t *sec, uint32_t *off) {
    uint64_t b = (uint64_t)idx * 32;
    if (b >= d->size || b >= XF_MAX_DIRBYTES) return 1;
    uint32_t c;
    int r = node_clus(fs, d, (uint32_t)(b >> fs->cshift), &c);
    if (r) return r;
    uint32_t in = (uint32_t)(b & (fs->cb - 1));
    *sec = clus_sec(fs, c) + in / fs->bps;
    *off = in % fs->bps;
    return 0;
}

static int dir_get(exfat_fs_t *fs, xf_vnode_t *d, uint32_t idx, uint8_t *e) {
    uint32_t sec, off;
    int r = dir_pos(fs, d, idx, &sec, &off);
    if (r) return r;
    uint8_t *p = bc_get(fs, sec, 1, NULL);
    if (!p) return -1;
    memcpy(e, p + off, 32);
    return 0;
}

static int dir_put(exfat_fs_t *fs, xf_vnode_t *d, uint32_t idx, const uint8_t *e) {
    uint32_t sec, off, slot;
    if (dir_pos(fs, d, idx, &sec, &off) != 0) return -1;
    uint8_t *p = bc_get(fs, sec, 1, &slot);
    if (!p) return -1;
    memcpy(p + off, e, 32);
    bc_mark(fs, slot);
    return 0;
}

typedef struct {
    uint32_t idx;                       /* the file entry */
    uint32_t nent;                      /* entries in the set (1 + SecondaryCount) */
    uint8_t  set[(XF_MAX_SEC + 1) * 32];
    uint16_t u[NAME_UNITS];             /* the name as stored */
    uint32_t nu;
    char     name[NAME_UNITS * 3 + 1];  /* UTF-8 */
} dent_t;

/* Is the entry set in o->set[0 .. o->nent) well formed?  Fills the name. */
static int set_valid(const exfat_fs_t *fs, dent_t *o) {
    const uint8_t *f = o->set, *s = o->set + 32;
    if (o->nent < 3 || o->nent > XF_MAX_SEC + 1) return 0;
    if (rd16(f + 2) != set_checksum(o->set, o->nent)) return 0;
    if (s[0] != ET_STREAM) return 0;
    uint32_t nl = s[3];
    uint32_t need = (nl + 14) / 15;
    if (nl == 0 || need > o->nent - 2) return 0;
    uint32_t k = 0;
    for (uint32_t i = 0; i < need; i++) {
        const uint8_t *e = o->set + (2 + i) * 32;
        if (e[0] != ET_NAME) return 0;
        for (uint32_t j = 0; j < 15 && k < nl && k < NAME_UNITS; j++)
            o->u[k++] = rd16(e + 2 + j * 2);
    }
    o->nu = k;
    uint32_t len = 0;
    o->name[0] = '\0';
    for (uint32_t i = 0; i < k; i++) {
        uint32_t c = o->u[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < k &&
            o->u[i + 1] >= 0xDC00 && o->u[i + 1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (o->u[i + 1] - 0xDC00);
            i++;
        } else if (c >= 0xD800 && c <= 0xDFFF) {
            c = 0xFFFD;
        }
        if (c == '/' || c == 0) c = '_';
        if (utf8_put(o->name, &len, sizeof(o->name), c) < 0) return 0;
    }
    /* The VFS holds names of up to 255 bytes. */
    if (len > 255) return 0;
    (void)fs;
    return 1;
}

/* The next valid file entry set at or after *idx; *idx moves past it.
 * 0 found, 1 end of directory, -1 error. */
static int dir_iter(exfat_fs_t *fs, xf_vnode_t *d, uint32_t *idx, dent_t *o) {
    uint8_t e[32];
    for (;;) {
        int r = dir_get(fs, d, *idx, e);
        if (r) return r < 0 ? -1 : 1;
        if (e[0] == ET_EOD) return 1;
        uint32_t i = (*idx)++;
        if (e[0] != ET_FILE) continue;     /* deleted, other primaries, stray secondaries */
        uint32_t sc = e[1];
        if (sc < 2 || sc > XF_MAX_SEC) continue;
        memcpy(o->set, e, 32);
        int ok = 1;
        for (uint32_t k = 1; k <= sc; k++) {
            r = dir_get(fs, d, i + k, o->set + k * 32);
            if (r < 0) return -1;
            if (r || !(o->set[k * 32] & ET_INUSE) || !(o->set[k * 32] & 0x40)) { ok = 0; break; }
        }
        if (!ok) continue;
        o->idx = i;
        o->nent = sc + 1;
        if (!set_valid(fs, o)) {
            if (fs->io_errors++ < 8)
                printk("[EXFAT] %s: ignoring a malformed entry set (run fsck.exfat)\n",
                       fs->bp->name);
            continue;
        }
        *idx = i + sc + 1;
        return 0;
    }
}

static int name_match(const exfat_fs_t *fs, const dent_t *de, const uint16_t *u, uint32_t n) {
    if (de->nu != n) return 0;
    for (uint32_t i = 0; i < n; i++)
        if (upc(fs, de->u[i]) != upc(fs, u[i])) return 0;
    return 1;
}

/* Find `name` (case-insensitive through the up-case table) in `d`. */
static int dir_find(exfat_fs_t *fs, xf_vnode_t *d, const char *name, dent_t *o) {
    uint16_t u[NAME_UNITS];
    uint32_t n;
    if (name_to_u16(name, u, &n, 0) < 0) return E_NOENT;
    uint32_t idx = 0;
    for (;;) {
        int r = dir_iter(fs, d, &idx, o);
        if (r) return r < 0 ? E_IO : E_NOENT;
        if (name_match(fs, o, u, n)) return 0;
    }
}

/* ── Nodes ──────────────────────────────────────────────────────────────── */

static void node_hooks(xf_vnode_t *vn);
static int file_grow(exfat_fs_t *fs, xf_vnode_t *vn, uint32_t need);
static int node_store(xf_vnode_t *vn);

static uint32_t mode_of(const exfat_fs_t *fs, const xf_vnode_t *vn) {
    uint32_t m = 0777 & ~(vn->is_dir ? fs->dmask : fs->fmask);
    if (!vn->is_dir && (vn->attr & ATTR_RO)) m &= ~0222u;
    return m;
}

static uint32_t ino_of(uint64_t key) {
    return key ? (uint32_t)(key / 32) + 2 : 1;
}

static uint64_t vsize(const xf_vnode_t *vn) {
    return vn->size;
}

/* The largest file whose cluster count fits the 32-bit cluster numbers
 * (anything near it runs out of clusters first: -ENOSPC). */
static uint64_t xf_max_file(const exfat_fs_t *fs) {
    return (uint64_t)0xFFFFFFFFu << fs->cshift;
}

static void node_from_set(xf_vnode_t *vn, const uint8_t *set) {
    exfat_fs_t *fs = vn->fs;
    const uint8_t *s = set + 32;
    vn->nsec = set[1];
    vn->attr = rd16(set + 4);
    vn->is_dir = (vn->attr & ATTR_DIR) != 0;
    vn->first = rd32(s + 20);
    vn->nofat = (s[1] & SF_NOFAT) != 0;
    vn->size = rd64(s + 24);
    vn->valid = rd64(s + 8);
    vn->bad = 0;
    /* Lengths and clusters must fit the cluster heap. */
    if (vn->valid > vn->size) vn->valid = vn->size;
    if (vn->size == 0) {
        vn->first = 0;
    } else if (vn->size > ((uint64_t)fs->nclus << fs->cshift) || !clus_ok(fs, vn->first) ||
               (vn->nofat && clusters_for(fs, vn->size) > fs->nclus - (vn->first - 2)) ||
               (vn->is_dir && vn->size > XF_MAX_DIRBYTES) ||
               sys_overlap(fs, vn->first, vn->nofat ? clusters_for(fs, vn->size) : 1)) {
        vn->bad = 1;
        if (fs->io_errors++ < 8)
            printk("[EXFAT] %s: entry with impossible storage (cluster %u, %u MiB)\n",
                   fs->bp->name, (unsigned)vn->first,
                   (unsigned)(vn->size >> 20 > 0xFFFFFFFFull ? 0xFFFFFFFFu : vn->size >> 20));
    }
    vn->mtime = ts_to_unix(rd32(set + 12), set[21], set[23]);
    vn->atime = ts_to_unix(rd32(set + 16), 0, set[24]);
    vn->ctime = vn->mtime;
    vn->c_clus = 0;
    vn->r_valid = 0;
    vfs_node_t *n = &vn->vnode;
    n->flags = vn->is_dir ? VFS_FLAG_DIR : VFS_FLAG_FILE;
    n->size  = vsize(vn);
    n->mask  = mode_of(fs, vn);
    n->uid   = fs->uid;
    n->gid   = fs->gid;
    n->atime = vn->atime;
    n->mtime = vn->mtime;
    n->ctime = vn->ctime;
    n->inode = ino_of(vn->key);
    n->nlink = 1;
    n->dev   = fs->bp->rdev;
}

static uint32_t bucket(uint64_t key) {
    return (uint32_t)(key / 32) % XF_NODE_BUCKETS;
}

static void node_hash_del(exfat_fs_t *fs, xf_vnode_t *vn) {
    xf_vnode_t **pp = &fs->nodes[bucket(vn->key)];
    while (*pp && *pp != vn) pp = &(*pp)->hnext;
    if (*pp) *pp = vn->hnext;
    vn->hnext = NULL;
}

static void node_hash_add(exfat_fs_t *fs, xf_vnode_t *vn) {
    xf_vnode_t **b = &fs->nodes[bucket(vn->key)];
    vn->hnext = *b;
    *b = vn;
}

static uint64_t ent_key(const exfat_fs_t *fs, uint32_t sec, uint32_t off) {
    return (uint64_t)sec * fs->bps + off;
}

static xf_vnode_t *node_find(exfat_fs_t *fs, uint64_t key) {
    xf_vnode_t *vn = fs->nodes[bucket(key)];
    while (vn && vn->key != key) vn = vn->hnext;
    return vn;
}

/* The node for entry set `de` of directory `d`. */
static xf_vnode_t *node_get(exfat_fs_t *fs, xf_vnode_t *d, const dent_t *de) {
    uint32_t sec, off;
    if (dir_pos(fs, d, de->idx, &sec, &off) != 0) return NULL;
    uint64_t key = ent_key(fs, sec, off);
    xf_vnode_t *vn = node_find(fs, key);
    if (vn) return vn;
    vn = (xf_vnode_t *)kmalloc(sizeof(*vn));
    if (!vn) return NULL;
    memset(vn, 0, sizeof(*vn));
    vn->fs = fs;
    vn->key = key;
    vn->ent_idx = de->idx;
    vn->parent = d;
    node_from_set(vn, de->set);
    strncpy(vn->vnode.name, de->name, 255);
    node_hooks(vn);
    node_hash_add(fs, vn);
    return vn;
}

/* Fill the timestamp fields of file entry `f` (create too when `create`). */
static void set_times(uint8_t *f, uint32_t mtime, uint32_t atime, int create) {
    uint32_t cs;
    uint32_t now = xf_now(&cs);
    uint8_t ms10 = (uint8_t)((mtime & 1) * 100 + (mtime == now ? cs : 0));
    if (create) {
        wr32(f + 8, unix_to_ts(mtime));
        f[20] = ms10;
        f[22] = 0x80;
    }
    wr32(f + 12, unix_to_ts(mtime));
    f[21] = ms10;
    f[23] = 0x80;
    wr32(f + 16, unix_to_ts(atime));
    f[24] = 0x80;
}

/* Write the node's attributes, times, flags, clusters and lengths to its
 * entry set, with a new SetChecksum. */
static int node_store(xf_vnode_t *vn) {
    exfat_fs_t *fs = vn->fs;
    if (!vn->key || vn->deleted) return 0;
    xf_vnode_t *d = vn->parent;
    uint8_t set[(XF_MAX_SEC + 1) * 32];
    uint32_t nent = vn->nsec + 1;
    if (nent < 3 || nent > XF_MAX_SEC + 1) return E_IO;
    for (uint32_t i = 0; i < nent; i++)
        if (dir_get(fs, d, vn->ent_idx + i, set + i * 32) != 0) return E_IO;
    if (set[0] != ET_FILE || set[1] != vn->nsec || set[32] != ET_STREAM) return E_IO;
    wr16(set + 4, vn->attr);
    set_times(set, vn->mtime, vn->atime, 0);
    uint8_t *s = set + 32;
    if (!vn->bad) {
        s[1] = (uint8_t)(SF_ALLOC | (vn->first && vn->nofat ? SF_NOFAT : 0));
        wr32(s + 20, vn->first);
        wr64(s + 24, vn->size);
        wr64(s + 8, vn->valid);
    }
    wr16(set + 2, set_checksum(set, nent));
    for (uint32_t i = 0; i < 2; i++)
        if (dir_put(fs, d, vn->ent_idx + i, set + i * 32) < 0) return E_IO;
    vfs_node_t *n = &vn->vnode;
    n->size  = vsize(vn);
    n->mtime = vn->mtime;
    n->atime = vn->atime;
    n->ctime = vn->ctime;
    n->mask  = mode_of(fs, vn);
    return 0;
}

static void touch(xf_vnode_t *vn) {
    vn->mtime = vn->ctime = vn->atime = xf_now(NULL);
    if (!vn->is_dir) vn->attr |= ATTR_ARCH;
}

static int clus_zero(exfat_fs_t *fs, uint32_t c) {
    for (uint32_t s = 0; s < fs->spc; s++) {
        uint32_t slot;
        if (!bc_get(fs, clus_sec(fs, c) + s, 0, &slot)) return E_IO;
        bc_mark(fs, slot);
    }
    return 0;
}

/* Grow directory `d` by one zeroed cluster. */
static int dir_extend(exfat_fs_t *fs, xf_vnode_t *d) {
    if (d->size + fs->cb > XF_MAX_DIRBYTES) return E_NOSPC;
    uint32_t have = clusters_for(fs, d->size);
    int r = file_grow(fs, d, have + 1);
    if (r < 0) return r;
    d->size = (uint64_t)(have + 1) << fs->cshift;
    d->valid = d->size;
    uint32_t c;
    if (node_clus(fs, d, have, &c) != 0) return E_IO;
    if ((r = clus_zero(fs, c)) < 0) return r;
    d->vnode.size = vsize(d);
    return node_store(d);
}

/* `n` consecutive free entries in `d` (growing it when needed): the index of
 * the first, or a negative errno. */
static int64_t dir_slots(exfat_fs_t *fs, xf_vnode_t *d, uint32_t n) {
    uint32_t run = 0, run_start = 0;
    uint8_t e[32];
    for (uint32_t idx = 0;; idx++) {
        int r = dir_get(fs, d, idx, e);
        if (r < 0) return E_IO;
        if (r == 1) {
            int x = dir_extend(fs, d);
            if (x < 0) return x;
            idx--;
            continue;
        }
        if (!(e[0] & ET_INUSE)) {
            if (!run) run_start = idx;
            if (++run == n) return run_start;
        } else {
            run = 0;
        }
    }
}

/* Mark the `nent` entries from `first` of `d` deleted. */
static int dir_del(exfat_fs_t *fs, xf_vnode_t *d, uint32_t first, uint32_t nent) {
    uint8_t e[32];
    for (uint32_t i = first; i < first + nent; i++) {
        if (dir_get(fs, d, i, e) != 0) return E_IO;
        e[0] &= (uint8_t)~ET_INUSE;
        if (dir_put(fs, d, i, e) < 0) return E_IO;
    }
    return 0;
}

/* Build an entry set for `u[0..nu)` in `set`: file entry `f` (attributes and
 * times) and stream entry `s` (flags, clusters and lengths) are copied in
 * when given.  Returns the number of entries. */
static uint32_t set_build(exfat_fs_t *fs, uint8_t *set, const uint16_t *u, uint32_t nu,
                          const uint8_t *f, const uint8_t *s) {
    uint32_t nn = (nu + 14) / 15, nent = 2 + nn;
    memset(set, 0, nent * 32);
    if (f) memcpy(set, f, 32);
    set[0] = ET_FILE;
    set[1] = (uint8_t)(nent - 1);
    uint8_t *st = set + 32;
    if (s) memcpy(st, s, 32);
    st[0] = ET_STREAM;
    st[2] = 0;
    st[3] = (uint8_t)nu;
    wr16(st + 4, name_hash(fs, u, nu));
    for (uint32_t i = 0; i < nn; i++) {
        uint8_t *e = set + (2 + i) * 32;
        e[0] = ET_NAME;
        e[1] = 0;
        for (uint32_t j = 0; j < 15; j++) {
            uint32_t k = i * 15 + j;
            wr16(e + 2 + j * 2, k < nu ? u[k] : 0);
        }
    }
    wr16(set + 2, set_checksum(set, nent));
    return nent;
}

/* Write `set` (nent entries) into free slots of `d`; the first index. */
static int64_t dir_add(exfat_fs_t *fs, xf_vnode_t *d, const uint8_t *set, uint32_t nent) {
    int64_t at = dir_slots(fs, d, nent);
    if (at < 0) return at;
    /* Secondaries first, the file entry last: the set is only live once
     * it is complete. */
    for (uint32_t i = nent; i-- > 0;)
        if (dir_put(fs, d, (uint32_t)at + i, set + i * 32) < 0) return E_IO;
    return at;
}

/* ── File data ──────────────────────────────────────────────────────────── */

/* Turn a NoFatChain allocation of `have` clusters into a FAT chain. */
static int to_chain(exfat_fs_t *fs, xf_vnode_t *vn, uint32_t have) {
    for (uint32_t i = 0; i < have; i++)
        if (fat_wr(fs, vn->first + i, i + 1 < have ? vn->first + i + 1 : FAT_EOC) < 0)
            return E_IO;
    vn->nofat = 0;
    vn->c_clus = 0;
    return 0;
}

/* Keep the first `keep` clusters of the node's storage, free the rest. */
static int file_cut(exfat_fs_t *fs, xf_vnode_t *vn, uint32_t have, uint32_t keep) {
    vn->c_clus = 0;
    if (!vn->first || keep >= have) return 0;
    if (vn->nofat) {
        int r = bm_free_run(fs, vn->first + keep, have - keep);
        if (keep == 0) vn->first = 0;
        return r < 0 ? E_IO : 0;
    }
    if (keep == 0) {
        uint32_t f = vn->first;
        vn->first = 0;
        return chain_free(fs, f, have);
    }
    uint32_t c, next;
    if (node_clus(fs, vn, keep - 1, &c) != 0) return E_IO;
    if (fat_next(fs, c, &next) < 0) return E_IO;
    if (fat_wr(fs, c, FAT_EOC) < 0) return E_IO;
    vn->c_clus = 0;
    return next ? chain_free(fs, next, have - keep) : 0;
}

/* Make the storage `need` clusters long.  New clusters follow the last one
 * when they are free (the file stays NoFatChain); otherwise the file becomes
 * a FAT chain.  On failure the clusters added here are given back.  The
 * caller sets the new size. */
static int file_grow(exfat_fs_t *fs, xf_vnode_t *vn, uint32_t need) {
    if (vn->bad) return E_IO;
    uint32_t have = vn->first ? clusters_for(fs, vn->size) : 0;
    if (need <= have) return 0;
    uint32_t start = have, tail = 0;
    uint64_t old_size = vn->size;
    if (have) {
        if (vn->nofat) tail = vn->first + have - 1;
        else if (node_clus(fs, vn, have - 1, &tail) != 0) return E_IO;
    }
    int r = 0;
    while (have < need) {
        uint32_t c;
        if (have && vn->nofat && clus_ok(fs, tail + 1) && bm_get(fs, tail + 1) == 0) {
            if (bm_set(fs, tail + 1, 1) < 0) { r = E_IO; break; }
            c = tail + 1;
            bc_drop(fs, clus_sec(fs, c), fs->spc);
        } else {
            r = clus_alloc(fs, tail ? tail + 1 : 0, &c);
            if (r < 0) break;
            if (!have) {
                vn->first = c;
                vn->nofat = 1;
            } else {
                if (vn->nofat && (r = to_chain(fs, vn, have)) < 0) { bm_set(fs, c, 0); break; }
                if (fat_wr(fs, tail, c) < 0 || fat_wr(fs, c, FAT_EOC) < 0) {
                    r = E_IO;
                    bm_set(fs, c, 0);
                    break;
                }
            }
        }
        if (!vn->nofat) { vn->c_idx = have; vn->c_clus = c; }
        tail = c;
        have++;
        /* node_clus bounds by size: count what is allocated so far. */
        vn->size = (uint64_t)have << fs->cshift;
    }
    if (r < 0) {
        file_cut(fs, vn, have, start);
        vn->size = old_size;
        return r;
    }
    vn->size = old_size;
    return 0;
}

/* Move `len` bytes at offset `off` between `buf` and the device (`buf` NULL
 * with `wr` writes zeros).  The storage must cover the range. */
static int file_io_n(exfat_fs_t *fs, xf_vnode_t *vn, uint64_t off, uint32_t len,
                     uint8_t *buf, int wr, uint32_t *donep) {
    uint32_t done = 0;
    *donep = 0;
    while (done < len) {
        uint64_t pos = off + done;
        uint32_t ci = (uint32_t)(pos >> fs->cshift), co = (uint32_t)(pos & (fs->cb - 1)), c;
        if (node_clus(fs, vn, ci, &c) != 0) return E_IO;
        uint32_t run = 1, want = len - done;
        while ((uint64_t)run * fs->cb - co < want && run < 256) {
            if (vn->nofat) {
                if ((uint64_t)ci + run >= clusters_for(fs, vn->size)) break;
            } else {
                uint32_t next;
                if (fat_next(fs, c + run - 1, &next) < 0) return E_IO;
                if (next != c + run) break;
            }
            run++;
        }
        if (run > 1 && !vn->nofat) { vn->c_idx = ci + run - 1; vn->c_clus = c + run - 1; }
        uint64_t avail = (uint64_t)run * fs->cb - co;
        uint32_t n = avail < want ? (uint32_t)avail : want;
        uint32_t sec = clus_sec(fs, c) + co / fs->bps, so = co % fs->bps;
        if (so == 0 && n >= fs->bps) {
            uint32_t k = n / fs->bps;
            n = k * fs->bps;
            if (!wr) {
                if (dev_io(fs, sec, k, buf + done, 0) < 0) return E_IO;
            } else if (buf) {
                if (dev_io(fs, sec, k, buf + done, 1) < 0) return E_IO;
            } else {
                uint32_t zs = XF_ZBUF / fs->bps;
                for (uint32_t i = 0; i < k; i += zs)
                    if (dev_io(fs, sec + i, k - i < zs ? k - i : zs, fs->zbuf, 1) < 0) return E_IO;
            }
        } else {
            if (n > fs->bps - so) n = fs->bps - so;
            if (dev_io(fs, sec, 1, fs->sbuf, 0) < 0) return E_IO;
            if (!wr) {
                memcpy(buf + done, fs->sbuf + so, n);
            } else {
                if (buf) memcpy(fs->sbuf + so, buf + done, n);
                else     memset(fs->sbuf + so, 0, n);
                if (dev_io(fs, sec, 1, fs->sbuf, 1) < 0) return E_IO;
            }
        }
        done += n;
        *donep = done;
    }
    return 0;
}

/* Zeros over [from, to) of the file, in chunks (the range can pass 4 GiB). */
static int file_zero(exfat_fs_t *fs, xf_vnode_t *vn, uint64_t from, uint64_t to) {
    while (from < to) {
        uint64_t n = to - from;
        if (n > (1u << 30)) n = 1u << 30;
        uint32_t done;
        int r = file_io_n(fs, vn, from, (uint32_t)n, NULL, 1, &done);
        if (r < 0) return r;
        from += n;
    }
    return 0;
}

static uint32_t xf_read(vfs_node_t *n, uint64_t off, uint32_t len, uint8_t *buf) {
    xf_vnode_t *vn = (xf_vnode_t *)n;
    exfat_fs_t *fs = vn->fs;
    fs_lock(fs);
    uint32_t got = 0;
    if (off < vn->size && !vn->bad) {
        if (len > vn->size - off) len = (uint32_t)(vn->size - off);
        /* Past ValidDataLength the file reads as zeros. */
        uint32_t disk = off >= vn->valid ? 0 : (vn->valid - off < len ? (uint32_t)(vn->valid - off) : len);
        file_io_n(fs, vn, off, disk, buf, 0, &got);
        if (got == disk && disk < len) {
            memset(buf + disk, 0, len - disk);
            got = len;
        }
    }
    fs_unlock(fs);
    return got;
}

static uint32_t xf_write(vfs_node_t *n, uint64_t off, uint32_t len, const uint8_t *buf) {
    xf_vnode_t *vn = (xf_vnode_t *)n;
    exfat_fs_t *fs = vn->fs;
    if (len == 0) return 0;
    uint64_t max = xf_max_file(fs);
    if (off >= max) return VFS_WRITE_EFBIG;
    if (len > max - off) len = (uint32_t)(max - off);
    fs_lock(fs);
    int r = 0;
    uint64_t end = (uint64_t)off + len;
    if (fs->ro) r = E_ROFS;
    else if (vn->bad) r = E_IO;
    int grown = 0;
    if (!r && end > vn->size) {
        r = file_grow(fs, vn, clusters_for(fs, end));
        if (!r) { vn->size = end; grown = 1; }
    }
    if (!r && off > vn->valid)
        r = file_zero(fs, vn, vn->valid, off);
    if (!r)
        r = file_io_n(fs, vn, off, len, (uint8_t *)buf, 1, &(uint32_t){0});
    if (!r) {
        if (end > vn->valid) vn->valid = end;
        touch(vn);
        r = node_store(vn);
    } else if (grown) {
        /* The clusters stay with the file (past ValidDataLength, reading as
         * zeros) rather than being lost to the bitmap. */
        node_store(vn);
    }
    if (fs_unlock(fs) < 0 && !r) r = E_IO;
    return r < 0 ? (uint32_t)r : len;
}

static int xf_truncate(vfs_node_t *n, uint64_t size) {
    xf_vnode_t *vn = (xf_vnode_t *)n;
    exfat_fs_t *fs = vn->fs;
    if (vn->is_dir) return E_ISDIR;
    if (size > xf_max_file(fs)) return -27;            /* -EFBIG */
    fs_lock(fs);
    int r = 0;
    if (fs->ro) r = E_ROFS;
    else if (vn->bad) r = E_IO;
    else if (size > vn->size) {
        /* The new part is past ValidDataLength: it reads as zeros without
         * being written. */
        r = file_grow(fs, vn, clusters_for(fs, size));
        if (!r) vn->size = size;
    } else if (size < vn->size) {
        /* Even when freeing fails part way the file is cut: the clusters
         * left marked are only lost space. */
        r = file_cut(fs, vn, clusters_for(fs, vn->size), clusters_for(fs, size));
        vn->size = size;
        if (vn->valid > size) vn->valid = size;
        if (!vn->size) vn->first = 0;
    }
    if (!r) {
        touch(vn);
        r = node_store(vn);
    }
    if (fs_unlock(fs) < 0 && !r) r = E_IO;
    return r;
}

/* ── Directory operations ───────────────────────────────────────────────── */

static vfs_node_t *xf_finddir(vfs_node_t *dir, const char *name) {
    xf_vnode_t *d = (xf_vnode_t *)dir;
    exfat_fs_t *fs = d->fs;
    if (strcmp(name, ".") == 0) return dir;
    if (strcmp(name, "..") == 0) return &d->parent->vnode;
    if (d->deleted) return NULL;
    fs_lock(fs);
    dent_t *de = (dent_t *)kmalloc(sizeof(dent_t));
    xf_vnode_t *vn = NULL;
    if (de && dir_find(fs, d, name, de) == 0)
        vn = node_get(fs, d, de);
    if (de) kfree(de);
    fs_unlock(fs);
    return vn ? &vn->vnode : NULL;
}

static int xf_readdir(vfs_node_t *dir, uint32_t req, vfs_dirent_t *out) {
    xf_vnode_t *d = (xf_vnode_t *)dir;
    exfat_fs_t *fs = d->fs;
    /* exFAT directories have no "." and ".." entries on disk. */
    if (req < 2) {
        out->ino = req ? d->parent->vnode.inode : d->vnode.inode;
        out->type = VFS_FLAG_DIR;
        strncpy(out->name, req ? ".." : ".", 255);
        return 0;
    }
    if (d->deleted) return -1;
    fs_lock(fs);
    dent_t *de = (dent_t *)kmalloc(sizeof(dent_t));
    int rc = -1;
    if (!de) goto out;
    /* The cursor holds the slot just past the set last returned; a deleted
     * set only loses its InUse bits, so `rm -r` sees every entry. */
    uint32_t i, pos;
    if (d->r_valid && d->r_idx <= req) { i = d->r_idx; pos = d->r_pos; }
    else if (d->r_valid && d->r_idx == req + 1) { i = req; pos = d->r_prev; }
    else { i = 2; pos = 0; }
    for (;;) {
        uint32_t at = pos;
        int r = dir_iter(fs, d, &pos, de);
        if (r) { d->r_valid = 0; break; }
        if (i == req) {
            d->r_idx = i + 1;
            d->r_pos = pos;
            d->r_prev = at;
            d->r_valid = 1;
            uint32_t sec, off;
            out->ino = dir_pos(fs, d, de->idx, &sec, &off) == 0
                     ? ino_of(ent_key(fs, sec, off)) : 0;
            out->type = (rd16(de->set + 4) & ATTR_DIR) ? VFS_FLAG_DIR : VFS_FLAG_FILE;
            strncpy(out->name, de->name, 255);
            out->name[255] = '\0';
            rc = 0;
            break;
        }
        i++;
    }
    kfree(de);
out:
    fs_unlock(fs);
    return rc;
}

static int xf_create(vfs_node_t *dir, const char *name, uint32_t flags) {
    xf_vnode_t *d = (xf_vnode_t *)dir;
    exfat_fs_t *fs = d->fs;
    if (flags != VFS_FLAG_FILE && flags != VFS_FLAG_DIR) return E_PERM;
    uint16_t u[NAME_UNITS];
    uint32_t nu;
    int r = name_to_u16(name, u, &nu, 1);
    if (r < 0) return r;
    fs_lock(fs);
    uint32_t clus = 0;
    dent_t *de = (dent_t *)kmalloc(sizeof(dent_t));
    if (!de) { r = E_NOMEM; goto out; }
    if (fs->ro) { r = E_ROFS; goto out; }
    if (d->deleted) { r = E_NOENT; goto out; }
    r = dir_find(fs, d, name, de);
    if (r == 0) { r = E_EXIST; goto out; }
    if (r != E_NOENT) goto out;
    uint8_t f[32], s[32];
    memset(f, 0, 32);
    memset(s, 0, 32);
    uint32_t t = xf_now(NULL);
    wr16(f + 4, flags == VFS_FLAG_DIR ? ATTR_DIR : ATTR_ARCH);
    set_times(f, t, t, 1);
    s[1] = SF_ALLOC;
    if (flags == VFS_FLAG_DIR) {
        r = clus_alloc(fs, 0, &clus);
        if (r < 0) goto out;
        if ((r = clus_zero(fs, clus)) < 0) goto undo;
        s[1] |= SF_NOFAT;
        wr32(s + 20, clus);
        wr64(s + 8, fs->cb);
        wr64(s + 24, fs->cb);
    }
    uint32_t nent = set_build(fs, de->set, u, nu, f, s);
    int64_t at = dir_add(fs, d, de->set, nent);
    if (at < 0) { r = (int)at; goto undo; }
    d->mtime = d->ctime = t;
    node_store(d);
    r = 0;
    goto out;
undo:
    if (clus) bm_set(fs, clus, 0);
out:
    if (de) kfree(de);
    if (fs_unlock(fs) < 0 && !r) r = E_IO;
    return r;
}

/* Free a node's storage. */
static void node_free_storage(exfat_fs_t *fs, xf_vnode_t *vn) {
    if (vn->first && !vn->bad) file_cut(fs, vn, clusters_for(fs, vn->size), 0);
    vn->first = 0;
}

/* An unlinked node: out of the hash; its clusters go once nothing has it
 * open. */
static void node_orphan(exfat_fs_t *fs, xf_vnode_t *vn) {
    node_hash_del(fs, vn);
    vn->deleted = 1;
    vn->hnext = fs->dead;
    fs->dead = vn;
    if (vn->refs == 0) node_free_storage(fs, vn);
}

static int dir_is_empty(exfat_fs_t *fs, xf_vnode_t *d) {
    dent_t *de = (dent_t *)kmalloc(sizeof(dent_t));
    if (!de) return E_NOMEM;
    uint32_t idx = 0;
    int r = dir_iter(fs, d, &idx, de);
    kfree(de);
    return r < 0 ? E_IO : r == 1;
}

static int xf_unlink(vfs_node_t *dir, const char *name) {
    xf_vnode_t *d = (xf_vnode_t *)dir;
    exfat_fs_t *fs = d->fs;
    fs_lock(fs);
    int r;
    dent_t *de = (dent_t *)kmalloc(sizeof(dent_t));
    if (!de) { r = E_NOMEM; goto out; }
    if (fs->ro) { r = E_ROFS; goto out; }
    if ((r = dir_find(fs, d, name, de)) < 0) goto out;
    xf_vnode_t *vn = node_get(fs, d, de);
    if (!vn) { r = E_IO; goto out; }
    if (vn->is_dir && !vn->bad && (r = dir_is_empty(fs, vn)) <= 0) {
        if (r == 0) r = E_NOTEMPTY;
        goto out;
    }
    if ((r = dir_del(fs, d, de->idx, de->nent)) < 0) goto out;
    node_orphan(fs, vn);
    d->mtime = d->ctime = xf_now(NULL);
    node_store(d);
    r = 0;
out:
    if (de) kfree(de);
    if (fs_unlock(fs) < 0 && !r) r = E_IO;
    return r;
}

/* Re-key `vn` to the entry set at index `idx` of `d`. */
static int node_move(exfat_fs_t *fs, xf_vnode_t *vn, xf_vnode_t *d, uint32_t idx,
                     uint32_t nent, const char *name) {
    uint32_t sec, off;
    if (dir_pos(fs, d, idx, &sec, &off) != 0) return E_IO;
    node_hash_del(fs, vn);
    vn->key = ent_key(fs, sec, off);
    vn->ent_idx = idx;
    vn->nsec = nent - 1;
    vn->parent = d;
    vn->vnode.inode = ino_of(vn->key);
    strncpy(vn->vnode.name, name, 255);
    vn->vnode.name[255] = '\0';
    node_hash_add(fs, vn);
    return 0;
}

static int xf_rename(vfs_node_t *odir, const char *oname, vfs_node_t *ndir, const char *nname) {
    xf_vnode_t *od = (xf_vnode_t *)odir, *nd = (xf_vnode_t *)ndir;
    exfat_fs_t *fs = od->fs;
    /* Another exFAT volume shares this rename_fn: never treat its nodes as
     * ours (its entries would be read with this volume's geometry). */
    if (nd->fs != fs) return -18;                        /* -EXDEV */
    uint16_t u[NAME_UNITS];
    uint32_t nu;
    int r = name_to_u16(nname, u, &nu, 1);
    if (r < 0) return r;
    fs_lock(fs);
    dent_t *src = (dent_t *)kmalloc(sizeof(dent_t));
    dent_t *dst = (dent_t *)kmalloc(sizeof(dent_t));
    if (!src || !dst) { r = E_NOMEM; goto out; }
    if (fs->ro) { r = E_ROFS; goto out; }
    if (od->deleted || nd->deleted) { r = E_NOENT; goto out; }
    if ((r = dir_find(fs, od, oname, src)) < 0) goto out;
    xf_vnode_t *vn = node_get(fs, od, src);
    if (!vn) { r = E_IO; goto out; }
    /* A directory cannot move below itself. */
    if (vn->is_dir)
        for (xf_vnode_t *a = nd; ; a = a->parent) {
            if (a == vn) { r = E_INVAL; goto out; }
            if (a->key == 0) break;
        }
    r = dir_find(fs, nd, nname, dst);
    if (r != 0 && r != E_NOENT) goto out;
    int exists = r == 0;
    if (exists && nd == od && dst->idx == src->idx) {
        /* The same entry set: only the spelling may change. */
        if (src->nu == nu && memcmp(src->u, u, nu * 2) == 0) { r = 0; goto out; }
        exists = 0;
    }
    xf_vnode_t *victim = NULL;
    if (exists) {
        victim = node_get(fs, nd, dst);
        if (!victim) { r = E_IO; goto out; }
        if (vn->is_dir && !victim->is_dir) { r = E_NOTDIR; goto out; }
        if (!vn->is_dir && victim->is_dir) { r = E_ISDIR; goto out; }
        if (victim->is_dir && (r = dir_is_empty(fs, victim)) <= 0) {
            if (r == 0) r = E_NOTEMPTY;
            goto out;
        }
    }
    /* The moved object's set under its new name, from its current entries
     * (dst->set is free for it once the victim's entries are read). */
    uint8_t f[32], s[32];
    memcpy(f, src->set, 32);
    memcpy(s, src->set + 32, 32);
    uint32_t vidx = exists ? dst->idx : 0, vnent = exists ? dst->nent : 0;
    uint32_t nent = set_build(fs, dst->set, u, nu, f, s);
    /* The new set goes in first (nothing has changed if there is no room),
     * then the victim's and the old entries are deleted; all of it under the
     * lock and written out together when the operation ends. */
    int64_t at = dir_add(fs, nd, dst->set, nent);
    if (at < 0) { r = (int)at; goto out; }
    if (exists) {
        if ((r = dir_del(fs, nd, vidx, vnent)) < 0) goto out;
        node_orphan(fs, victim);           /* frees the victim's clusters */
    }
    if ((r = dir_del(fs, od, src->idx, src->nent)) < 0) goto out;
    if ((r = node_move(fs, vn, nd, (uint32_t)at, nent, nname)) < 0) goto out;
    vn->ctime = xf_now(NULL);
    od->mtime = od->ctime = nd->mtime = nd->ctime = vn->ctime;
    node_store(od);
    if (nd != od) node_store(nd);
    r = 0;
out:
    if (src) kfree(src);
    if (dst) kfree(dst);
    if (fs_unlock(fs) < 0 && !r) r = E_IO;
    return r;
}

/* chmod: only the write bits mean something (the read-only attribute). */
static int xf_setattr(vfs_node_t *n, uint32_t mode, uint32_t uid, uint32_t gid) {
    (void)uid; (void)gid;
    xf_vnode_t *vn = (xf_vnode_t *)n;
    exfat_fs_t *fs = vn->fs;
    fs_lock(fs);
    int r = 0;
    if (!fs->ro && vn->key && !vn->is_dir) {
        uint16_t a = (mode & 0222) ? (uint16_t)(vn->attr & ~ATTR_RO) : (uint16_t)(vn->attr | ATTR_RO);
        if (a != vn->attr) {
            vn->attr = a;
            vn->ctime = xf_now(NULL);
            r = node_store(vn);
        }
    }
    n->mask = mode_of(fs, vn);
    n->uid = fs->uid;
    n->gid = fs->gid;
    if (fs_unlock(fs) < 0 && !r) r = E_IO;
    return r;
}

static int xf_settimes(vfs_node_t *n, uint32_t atime, uint32_t mtime) {
    xf_vnode_t *vn = (xf_vnode_t *)n;
    exfat_fs_t *fs = vn->fs;
    fs_lock(fs);
    int r = 0;
    if (fs->ro) r = E_ROFS;
    else {
        vn->atime = atime;
        vn->mtime = mtime;
        vn->ctime = xf_now(NULL);
        r = node_store(vn);
    }
    if (fs_unlock(fs) < 0 && !r) r = E_IO;
    return r;
}

static void xf_retain(vfs_node_t *n) {
    xf_vnode_t *vn = (xf_vnode_t *)n;
    preempt_disable();
    vn->refs++;
    vn->fs->open_refs++;
    preempt_enable();
}

static void xf_close(vfs_node_t *n) {
    xf_vnode_t *vn = (xf_vnode_t *)n;
    exfat_fs_t *fs = vn->fs;
    int last;
    preempt_disable();
    if (vn->refs > 0) vn->refs--;
    if (fs->open_refs > 0) fs->open_refs--;
    last = vn->refs == 0 && vn->deleted && vn->first;
    preempt_enable();
    if (last) {
        /* The last descriptor of an unlinked file: now its clusters go. */
        fs_lock(fs);
        if (vn->refs == 0 && vn->first && !fs->ro) node_free_storage(fs, vn);
        fs_unlock(fs);
    }
}

static int xf_symlink(vfs_node_t *d, const char *n, const char *t) {
    (void)d; (void)n; (void)t;
    return E_PERM;
}

static void node_hooks(xf_vnode_t *vn) {
    vfs_node_t *n = &vn->vnode;
    n->setattr_fn  = xf_setattr;
    n->settimes_fn = xf_settimes;
    n->retain_fn   = xf_retain;
    n->close_fn    = xf_close;
    if (vn->is_dir) {
        n->finddir_fn = xf_finddir;
        n->readdir_fn = xf_readdir;
        n->create_fn  = xf_create;
        n->unlink_fn  = xf_unlink;
        n->rename_fn  = xf_rename;
        n->symlink_fn = xf_symlink;
    } else {
        n->read_fn     = xf_read;
        n->write_fn    = xf_write;
        n->truncate_fn = xf_truncate;
    }
}

/* ── Mount ──────────────────────────────────────────────────────────────── */

static void free_nodes(xf_vnode_t *vn) {
    while (vn) {
        xf_vnode_t *next = vn->hnext;
        kfree(vn);
        vn = next;
    }
}

static void xf_free_fs(exfat_fs_t *fs) {
    for (int i = 0; i < XF_NODE_BUCKETS; i++) free_nodes(fs->nodes[i]);
    free_nodes(fs->dead);
    if (fs->root)    kfree(fs->root);           /* never in the hash */
    if (fs->bc)      kfree(fs->bc);
    if (fs->bc_data) kfree(fs->bc_data);
    if (fs->sbuf)    kfree(fs->sbuf);
    if (fs->zbuf)    kfree(fs->zbuf);
    if (fs->upcase)  kfree(fs->upcase);
    kfree(fs);
}

int exfat_busy(void *p) {
    return ((exfat_fs_t *)p)->open_refs > 0;
}

static int count_free(exfat_fs_t *fs) {
    uint32_t used = 0;
    for (uint32_t byte = 0; byte < (fs->nclus + 7) / 8; byte++) {
        uint8_t *p = bm_byte(fs, 2 + byte * 8, NULL);
        if (!p) return -1;
        uint8_t v = *p;
        if (byte == fs->nclus / 8 && (fs->nclus & 7)) v &= (uint8_t)((1u << (fs->nclus & 7)) - 1);
        used += (uint32_t)__builtin_popcount(v);
    }
    fs->free_count = fs->nclus - (used > fs->nclus ? fs->nclus : used);
    return 0;
}

void exfat_release(void *p) {
    exfat_fs_t *fs = (exfat_fs_t *)p;
    fs_lock(fs);
    if (!fs->ro && bc_flush(fs) == 0 && !fs->was_dirty) set_volflags(fs, 0);
    fs->ro = 1;
    fs_unlock(fs);
    printk("[EXFAT] %s: unmounted%s\n", fs->bp->name,
           fs->io_errors ? " (there were errors)" : "");
    xf_free_fs(fs);
}

int exfat_set_ro(void *p, int ro) {
    exfat_fs_t *fs = (exfat_fs_t *)p;
    int r = 0;
    fs_lock(fs);
    if (ro && !fs->ro) {
        r = bc_flush(fs);
        if (r == 0 && !fs->was_dirty) r = set_volflags(fs, 0);
        fs->ro = 1;
    } else if (!ro && fs->ro) {
        if (fs->ro_only) r = -1;
        else {
            fs->ro = 0;
            r = set_volflags(fs, 1);
        }
    }
    fs_unlock(fs);
    if (fs->ro_only && !ro) return E_ROFS;
    return r < 0 ? E_IO : 0;
}

int exfat_statfs(void *p, vfs_statfs_t *out) {
    exfat_fs_t *fs = (exfat_fs_t *)p;
    fs_lock(fs);
    int r = 0;
    if (fs->free_count == 0xFFFFFFFFu && fs->bm_clus && count_free(fs) < 0) r = E_IO;
    out->type    = 0x2011BAB0;                     /* EXFAT_SUPER_MAGIC */
    out->bsize   = fs->cb;
    out->blocks  = fs->nclus;
    out->bfree   = fs->free_count == 0xFFFFFFFFu ? 0 : fs->free_count;
    out->files   = 0;
    out->ffree   = 0;
    out->namelen = 255;
    fs_unlock(fs);
    return r;
}

/* Check the boot region whose boot sector is at 512-byte sector `first`:
 * signature, sector size `shift` (0: any) and the checksum sector.  `bs` gets
 * the boot sector.  1 valid, 0 not, -1 I/O error. */
static int boot_region_ok(blkpart_t *bp, uint32_t first, uint32_t shift,
                          uint8_t *bs, uint8_t *tmp) {
    if (blkpart_read(bp, first, 1, bs) < 0) return -1;
    if (memcmp(bs + 3, "EXFAT   ", 8) != 0 || bs[510] != 0x55 || bs[511] != 0xAA) return 0;
    if (bs[108] < 9 || bs[108] > 12 || (shift && bs[108] != shift)) return 0;
    uint32_t s512 = 1u << (bs[108] - 9);
    uint32_t sum = 0;
    for (uint32_t s = 0; s < 11; s++) {
        for (uint32_t k = 0; k < s512; k++) {
            if (blkpart_read(bp, first + s * s512 + k, 1, tmp) < 0) return -1;
            sum = boot_checksum_add(sum, tmp, 512, s == 0 && k == 0);
        }
    }
    for (uint32_t k = 0; k < s512; k++) {
        if (blkpart_read(bp, first + 11 * s512 + k, 1, tmp) < 0) return -1;
        for (uint32_t i = 0; i < 512; i += 4)
            if (rd32(tmp + i) != sum) return 0;
    }
    return 1;
}

/* Load the up-case table at cluster `c` (`len` bytes, checksum `sum`) into
 * fs->upcase; 0, or -1 when it is unusable (the built-in mapping is used). */
static int load_upcase(exfat_fs_t *fs, uint32_t c, uint64_t len, uint32_t sum) {
    if (!clus_ok(fs, c) || len < 2 || len > 2u * 65536u * 2u) return -1;
    for (uint32_t i = 0; i < 65536; i++) fs->upcase[i] = (uint16_t)i;
    uint32_t got = 0, csum = 0, idx = 0, clusters = 0;
    int run = 0;                        /* 1: the next unit is an identity-run length */
    uint8_t carry[2];
    int have_carry = 0;
    while (got < len) {
        if (++clusters > fs->nclus) return -1;
        for (uint32_t s = 0; s < fs->spc && got < len; s++) {
            if (dev_io(fs, clus_sec(fs, c) + s, 1, fs->sbuf, 0) < 0) return -1;
            uint32_t n = len - got < fs->bps ? (uint32_t)(len - got) : fs->bps;
            csum = boot_checksum_add(csum, fs->sbuf, n, 0);
            for (uint32_t i = 0; i < n; i++) {
                carry[have_carry++] = fs->sbuf[i];
                if (have_carry < 2) continue;
                have_carry = 0;
                uint16_t v = rd16(carry);
                if (run) {
                    idx += v;
                    run = 0;
                } else if (v == 0xFFFF) {
                    run = 1;
                } else if (idx < 65536) {
                    fs->upcase[idx++] = v;
                }
            }
            got += n;
        }
        if (got < len) {
            uint32_t next;
            if (fat_next(fs, c, &next) < 0 || !next) return -1;
            c = next;
        }
    }
    return csum == sum ? 0 : -1;
}

int exfat_mount_dev(blkpart_t *bp, int ro, const vfat_opts_t *o,
                    vfs_node_t **root_out, exfat_fs_t **fs_out) {
    uint8_t *bs = (uint8_t *)kmalloc(512), *tmp = (uint8_t *)kmalloc(512);
    exfat_fs_t *fs = NULL;
    int rc = E_INVAL;
    if (!bs || !tmp) { rc = E_NOMEM; goto fail; }
    if (bp->nsect < 64) goto fail;
    int from_backup = 0;
    int ok = boot_region_ok(bp, 0, 0, bs, tmp);
    if (ok < 0) { rc = E_IO; goto fail; }
    if (!ok) {
        if (blkpart_read(bp, 0, 1, bs) < 0) { rc = E_IO; goto fail; }
        if (memcmp(bs + 3, "EXFAT   ", 8) != 0) goto fail;   /* not exFAT: quietly */
        /* The backup region is sectors 12..23, in the volume's sector size. */
        for (uint32_t sh = 9; sh <= 12 && ok <= 0; sh++)
            if ((12u << (sh - 9)) + (12u << (sh - 9)) <= bp->nsect)
                ok = boot_region_ok(bp, 12u << (sh - 9), sh, bs, tmp);
        if (ok <= 0) {
            printk("[EXFAT] %s: boot region checksum mismatch (main and backup)\n", bp->name);
            goto fail;
        }
        printk("[EXFAT] %s: main boot region invalid, using the backup read-only\n", bp->name);
        from_backup = 1;
        if (!ro) { rc = E_ROFS; goto fail; }
    }
    uint32_t bshift = bs[108], cshift_s = bs[109];
    for (int i = 11; i < 64; i++)
        if (bs[i]) { printk("[EXFAT] %s: MustBeZero field is not zero\n", bp->name); goto fail; }
    if (bshift < 9 || bshift > 12 || bshift + cshift_s > 25 || rd16(bs + 104) >> 8 != 1 ||
        (bs[110] != 1 && bs[110] != 2)) {
        printk("[EXFAT] %s: unsupported geometry or revision\n", bp->name);
        goto fail;
    }
    fs = (exfat_fs_t *)kmalloc(sizeof(exfat_fs_t));
    if (!fs) { rc = E_NOMEM; goto fail; }
    memset(fs, 0, sizeof(*fs));
    fs->bp = bp;
    fs->bps = 1u << bshift;
    fs->s512 = fs->bps / 512;
    fs->spc = 1u << cshift_s;
    fs->cshift = bshift + cshift_s;
    fs->cb = 1u << fs->cshift;
    uint64_t vlen = rd64(bs + 72);
    uint32_t fat_off = rd32(bs + 80), fat_len = rd32(bs + 84);
    uint32_t heap = rd32(bs + 88), ncl = rd32(bs + 92);
    uint32_t nfats = bs[110];
    uint16_t vflags = rd16(bs + 106);
    if (vlen > bp->nsect / fs->s512 || vlen < 24) {
        printk("[EXFAT] %s: filesystem is larger than its partition\n", bp->name);
        goto fail;
    }
    fs->total = (uint32_t)vlen;
    /* FAT(s) after the boot regions, heap after the FAT(s), clusters inside
     * the volume, a FAT entry for every cluster. */
    if (fat_off < 24 || fat_len == 0 || (uint64_t)fat_off + (uint64_t)fat_len * nfats > heap ||
        heap >= fs->total || ncl == 0 || ncl > 0xFFFFFFF5u - 2 ||
        (uint64_t)heap + ((uint64_t)ncl << cshift_s) > fs->total ||
        ((uint64_t)ncl + 2) * 4 > (uint64_t)fat_len * fs->bps) {
        printk("[EXFAT] %s: inconsistent boot sector\n", bp->name);
        goto fail;
    }
    fs->nclus = ncl;
    fs->heap = heap;
    fs->fat_len = fat_len;
    fs->fat_start = fat_off + ((nfats == 2 && (vflags & 1)) ? fat_len : 0);
    fs->root_clus = rd32(bs + 96);
    if (!clus_ok(fs, fs->root_clus)) {
        printk("[EXFAT] %s: bad root directory cluster\n", bp->name);
        goto fail;
    }
    /* Two FATs (TexFAT), or geometry from the backup boot region (VolumeDirty
     * would be written into the broken main boot sector): never read-write,
     * not even by remount. */
    fs->ro_only = nfats != 1 || from_backup;
    fs->was_dirty = (vflags & VF_DIRTY) != 0;
    fs->free_count = 0xFFFFFFFFu;
    fs->next_free = 2;
    fs->uid = o->uid;
    fs->gid = o->gid;
    fs->dmask = o->dmask;
    fs->fmask = o->fmask;
    fs->ro = 1;                               /* until set up */
    if (!ro && fs->ro_only) {
        printk("[EXFAT] %s: two FATs (TexFAT): read-only\n", bp->name);
        rc = E_ROFS;
        goto fail;
    }

    uint32_t slots = XF_CACHE_BYTES / fs->bps, sets = 1;
    while (sets * 2 * XF_CACHE_WAYS <= slots) sets *= 2;
    fs->bc_sets = sets;
    slots = sets * XF_CACHE_WAYS;
    fs->bc      = (bc_meta_t *)kmalloc(slots * sizeof(bc_meta_t));
    fs->bc_data = (uint8_t *)kmalloc(slots * fs->bps);
    fs->sbuf    = (uint8_t *)kmalloc(fs->bps);
    fs->zbuf    = (uint8_t *)kmalloc(XF_ZBUF);
    fs->upcase  = (uint16_t *)kmalloc(65536 * sizeof(uint16_t));
    if (!fs->bc || !fs->bc_data || !fs->sbuf || !fs->zbuf || !fs->upcase) { rc = E_NOMEM; goto fail; }
    for (uint32_t i = 0; i < slots; i++) { fs->bc[i].sec = ~0u; fs->bc[i].age = 0; fs->bc[i].dirty = 0; }
    memset(fs->zbuf, 0, XF_ZBUF);

    /* The root directory: a FAT chain; its length is the chain's. */
    xf_vnode_t *r = (xf_vnode_t *)kmalloc(sizeof(*r));
    if (!r) { rc = E_NOMEM; goto fail; }
    memset(r, 0, sizeof(*r));
    fs->root = r;
    r->fs = fs;
    r->parent = r;
    r->is_dir = 1;
    r->attr = ATTR_DIR;
    r->first = fs->root_clus;
    {
        uint32_t c = fs->root_clus, n = 1, max = XF_MAX_DIRBYTES >> fs->cshift;
        for (;;) {
            uint32_t next;
            if (fat_next(fs, c, &next) < 0) { rc = E_INVAL; goto fail; }
            if (!next) break;
            if (++n > max || n > fs->nclus) {
                printk("[EXFAT] %s: root directory chain too long or looping\n", bp->name);
                goto fail;
            }
            c = next;
        }
        r->size = r->valid = (uint64_t)n << fs->cshift;
        fs->sys_lo[2] = fs->root_clus;
        fs->sys_n[2] = 1;
    }

    /* Critical primaries of the root: the allocation bitmap and the up-case
     * table. */
    int have_bm = 0, have_uc = 0;
    {
        uint8_t e[32];
        uint32_t want_bm = (nfats == 2 && (vflags & 1)) ? 1 : 0;
        for (uint32_t i = 0;; i++) {
            int x = dir_get(fs, r, i, e);
            if (x < 0) { rc = E_IO; goto fail; }
            if (x || e[0] == ET_EOD) break;
            if (e[0] == ET_BITMAP && (e[1] & 1) == want_bm && !have_bm) {
                uint32_t c = rd32(e + 20);
                uint64_t len = rd64(e + 24);
                uint32_t need_clus;
                if (!clus_ok(fs, c) || len < (fs->nclus + 7) / 8 || len > (1ull << 32)) {
                    printk("[EXFAT] %s: bad allocation bitmap entry\n", bp->name);
                    goto fail;
                }
                need_clus = clusters_for(fs, (fs->nclus + 7) / 8);
                if (need_clus > fs->nclus - (c - 2)) goto fail;
                /* The bitmap must be contiguous (as every formatter makes
                 * it); bm_byte() addresses it from its first cluster. */
                for (uint32_t k = 0, cc = c; k + 1 < need_clus; k++, cc++) {
                    uint32_t next;
                    if (fat_rd(fs, cc, &next) < 0) { rc = E_IO; goto fail; }
                    if (next != cc + 1) {
                        printk("[EXFAT] %s: fragmented allocation bitmap is not supported\n",
                               bp->name);
                        goto fail;
                    }
                }
                fs->bm_clus = c;
                fs->bm_bytes = (fs->nclus + 7) / 8;
                fs->sys_lo[0] = c;
                fs->sys_n[0] = need_clus;
                have_bm = 1;
            } else if (e[0] == ET_UPCASE && !have_uc) {
                have_uc = 1;
                fs->sys_lo[1] = rd32(e + 20);
                fs->sys_n[1] = clus_ok(fs, fs->sys_lo[1]) ? 1 : 0;
                if (load_upcase(fs, rd32(e + 20), rd64(e + 24), rd32(e + 4)) < 0) {
                    printk("[EXFAT] %s: up-case table unusable; using the built-in one\n",
                           bp->name);
                    have_uc = 2;
                }
            }
        }
    }
    if (!have_bm) {
        printk("[EXFAT] %s: no allocation bitmap\n", bp->name);
        goto fail;
    }
    if (have_uc != 1) {
        /* Names written with another mapping would get hashes other
         * implementations do not expect: read-only. */
        for (uint32_t i = 0; i < 65536; i++) fs->upcase[i] = (uint16_t)uc_default(i);
        fs->ro_only = 1;                    /* remount,rw is refused too */
        if (!ro) {
            printk("[EXFAT] %s: no usable up-case table: read-only\n", bp->name);
            rc = E_ROFS;
            goto fail;
        }
    }

    vfs_node_t *n = &r->vnode;
    strncpy(n->name, "/", 255);
    n->flags = VFS_FLAG_DIR;
    n->mask = mode_of(fs, r);
    n->uid = fs->uid;
    n->gid = fs->gid;
    n->inode = 1;
    n->nlink = 1;
    n->dev = bp->rdev;
    n->size = vsize(r);
    node_hooks(r);

    if (fs->was_dirty)
        printk("[EXFAT] %s: warning: not cleanly unmounted; run fsck.exfat\n", bp->name);
    if (!ro) {
        fs->ro = 0;
        if (set_volflags(fs, 1) < 0) { rc = E_IO; goto fail; }
    }
    printk("[EXFAT] %s: %u-byte clusters, %u clusters, mounted %s\n",
           bp->name, (unsigned)fs->cb, (unsigned)fs->nclus,
           ro ? "read-only" : "read-write");
    kfree(bs);
    kfree(tmp);
    *root_out = n;
    *fs_out = fs;
    return 0;

fail:
    if (fs) xf_free_fs(fs);
    if (bs) kfree(bs);
    if (tmp) kfree(tmp);
    return rc;
}
