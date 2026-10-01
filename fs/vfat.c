/*
 * FAT12/16/32 with long file names (VFAT), read-write (see vfat.h).
 *
 * Written for MaeroOS from Microsoft's "FAT: General Overview of On-Disk
 * Format" (fatgen103, version 1.03): BPB fields, the FAT type by cluster
 * count, FSInfo, the long-name entries and their checksum, and the basis-name
 * and numeric-tail rules for short names.  The boot sector's dirty flag (bit 0
 * of BS_Reserved1) is the one Linux and dosfstools use.  No code was copied
 * from FatFs, Linux or any other implementation.
 *
 * Layout of the driver:
 *   - a small write-back cache of sectors for metadata (FAT, directories,
 *     boot sector, FSInfo), flushed when each operation ends, so every
 *     system call leaves the volume consistent;
 *   - file data moves straight between the caller's buffer and the device,
 *     in runs of contiguous clusters;
 *   - one node per directory entry, keyed by the entry's byte position on
 *     the volume, kept until umount (the mount table matches mountpoints by
 *     node identity), re-keyed when a rename moves the entry;
 *   - one sleeping lock per instance serialises every operation.
 */
#include "vfat.h"
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
#define ATTR_VOLID  0x08
#define ATTR_DIR    0x10
#define ATTR_ARCH   0x20
#define ATTR_LFN    0x0F

#define NT_LOWER_BASE 0x08
#define NT_LOWER_EXT  0x10

#define VFAT_NODE_BUCKETS 256
#define VFAT_CACHE_BYTES  (256u * 1024u)
#define VFAT_CACHE_WAYS   4
#define VFAT_MAX_DIRENTS  65536u        /* a directory is at most 2 MiB */
#define VFAT_ZBUF         8192u

typedef struct vfat_vnode vfat_vnode_t;

typedef struct {
    uint32_t sec;                       /* ~0 = empty */
    uint32_t age;
    uint8_t  dirty;
} bc_meta_t;

struct vfat_fs {
    blkpart_t *bp;
    volatile int lock;
    int        ro;
    int        type;                    /* 12, 16, 32 */
    uint32_t   bps, s512, spc, cb;      /* bytes/sector, 512s/sector, sectors and bytes/cluster */
    uint32_t   reserved, nfats, fatsz;  /* FAT sectors */
    uint32_t   fat_start;               /* active (or first, when mirrored) FAT */
    int        mirror;
    uint32_t   root_ents, root_sec, root_secs;
    uint32_t   data_sec, total;
    uint32_t   nclus;                   /* data clusters: 2 .. nclus+1 */
    uint32_t   eoc, eoc_min, bad;
    uint32_t   root_clus;
    uint32_t   fsinfo_sec;              /* 0 = none */
    uint32_t   free_count;              /* 0xFFFFFFFF = unknown */
    uint32_t   next_free;
    int        fsinfo_dirty;
    int        was_dirty;               /* dirty when mounted: leave it so */
    uint32_t   dirty_off;               /* BS_Reserved1 in the boot sector */
    uint32_t   uid, gid, dmask, fmask;
    /* metadata cache */
    uint32_t   bc_sets;
    bc_meta_t *bc;
    uint8_t   *bc_data;
    uint32_t   bc_clock;
    uint8_t   *sbuf;                    /* one sector, for partial writes */
    uint8_t   *zbuf;                    /* zeros */
    vfat_vnode_t *nodes[VFAT_NODE_BUCKETS];
    vfat_vnode_t *dead;                 /* unlinked nodes, freed at umount */
    vfat_vnode_t *root;
    int        open_refs;
    uint32_t   io_errors;
};

struct vfat_vnode {
    vfs_node_t    vnode;                /* first: the VFS hands us &vnode */
    vfat_fs_t    *fs;
    vfat_vnode_t *hnext;
    vfat_vnode_t *parent;               /* containing directory (root: itself) */
    uint64_t      key;                  /* byte position of the short entry; 0 = root */
    uint32_t      ent_sec, ent_off;     /* the short entry */
    uint32_t      first;                /* first cluster, 0 = none */
    uint32_t      size;
    uint8_t       attr;
    int           is_dir;
    int           fixed_root;           /* FAT12/16 root directory region */
    int           refs;
    int           deleted;
    uint32_t      atime, mtime, ctime;
    /* chain cursor: cluster number c_clus is the c_idx-th of the chain */
    uint32_t      c_idx, c_clus;
    /* readdir cursor: VFS index r_idx is the first live entry at or after
     * slot r_pos; index r_idx-1 was found searching from slot r_prev */
    uint32_t      r_idx, r_pos, r_prev;
    int           r_valid;
};

/* ── Little-endian helpers ──────────────────────────────────────────────── */

static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
static inline void wr16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* ── Locking ────────────────────────────────────────────────────────────── */

static int bc_flush(vfat_fs_t *fs);
static void fsinfo_store(vfat_fs_t *fs);

static void fs_lock(vfat_fs_t *fs) {
    while (__sync_lock_test_and_set(&fs->lock, 1)) {
        if (current_proc) yield();
    }
}

/* End of an operation: everything it changed goes to the device. */
static int fs_unlock(vfat_fs_t *fs) {
    int rc = 0;
    if (!fs->ro) {
        fsinfo_store(fs);
        rc = bc_flush(fs);
    }
    __sync_lock_release(&fs->lock);
    return rc;
}

/* ── Time ───────────────────────────────────────────────────────────────── */

static uint32_t vfat_now(void) {
    return rtc_boot_epoch() + pit_ticks() / 100U;
}

/* Days since 1970-01-01 <-> civil date (proleptic Gregorian). */
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

/* FAT timestamps have no time zone; they are taken as UTC here. */
static void unix_to_fat(uint32_t t, uint16_t *date, uint16_t *tm) {
    int y; unsigned mo, d;
    civil_from_days((int32_t)(t / 86400u), &y, &mo, &d);
    uint32_t s = t % 86400u;
    if (y < 1980) { y = 1980; mo = 1; d = 1; s = 0; }
    if (y > 2107) { y = 2107; mo = 12; d = 31; s = 86399; }
    *date = (uint16_t)(((y - 1980) << 9) | (mo << 5) | d);
    *tm   = (uint16_t)(((s / 3600) << 11) | (((s / 60) % 60) << 5) | ((s % 60) / 2));
}

static uint32_t fat_to_unix(uint16_t date, uint16_t tm) {
    unsigned d = date & 31, mo = (date >> 5) & 15;
    int y = 1980 + (date >> 9);
    if (d == 0 || mo == 0 || mo > 12) return 315532800u;        /* 1980-01-01 */
    uint32_t s = ((tm >> 11) & 31) * 3600u + ((tm >> 5) & 63) * 60u + (tm & 31) * 2u;
    return (uint32_t)days_from_civil(y, mo, d) * 86400u + s;
}

/* ── Device I/O ─────────────────────────────────────────────────────────── */

/* `n` whole sectors (of bps bytes) at `sec` of the volume. */
static int dev_io(vfat_fs_t *fs, uint32_t sec, uint32_t n, void *buf, int wr) {
    if (sec >= fs->total || n > fs->total - sec) return -1;
    uint64_t s = (uint64_t)sec * fs->s512, cnt = (uint64_t)n * fs->s512;
    uint8_t *p = (uint8_t *)buf;
    while (cnt) {
        uint32_t k = cnt > 128 ? 128 : (uint32_t)cnt;
        int r = wr ? blkpart_write(fs->bp, (uint32_t)s, k, p)
                   : blkpart_read(fs->bp, (uint32_t)s, k, p);
        if (r < 0) {
            if (fs->io_errors++ < 8)
                printk("[VFAT] %s: %s error at sector %u\n", fs->bp->name,
                       wr ? "write" : "read", (unsigned)s);
            return -1;
        }
        s += k; cnt -= k; p += k * 512u;
    }
    return 0;
}

/* ── Metadata cache ─────────────────────────────────────────────────────── */

static int in_fat(const vfat_fs_t *fs, uint32_t sec) {
    return sec >= fs->fat_start && sec < fs->fat_start + fs->fatsz;
}

static int bc_writeback(vfat_fs_t *fs, uint32_t w) {
    bc_meta_t *m = &fs->bc[w];
    uint8_t *data = fs->bc_data + (uint64_t)w * fs->bps;
    int rc = 0;
    if (in_fat(fs, m->sec) && fs->mirror) {
        uint32_t rel = m->sec - fs->fat_start;
        for (uint32_t i = 0; i < fs->nfats; i++)
            if (dev_io(fs, fs->reserved + i * fs->fatsz + rel, 1, data, 1) < 0) rc = -1;
    } else {
        rc = dev_io(fs, m->sec, 1, data, 1);
    }
    if (rc == 0) m->dirty = 0;
    return rc;
}

/* The cached copy of sector `sec`, read in when `load` (else left as it was,
 * for a caller that overwrites all of it).  *slot gets the entry for
 * bc_mark().  The pointer is good until the next bc_get(). */
static uint8_t *bc_get(vfat_fs_t *fs, uint32_t sec, int load, uint32_t *slot) {
    uint32_t set = sec & (fs->bc_sets - 1u);
    uint32_t base = set * VFAT_CACHE_WAYS, victim = base;
    for (uint32_t w = base; w < base + VFAT_CACHE_WAYS; w++) {
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

static void bc_mark(vfat_fs_t *fs, uint32_t slot) {
    fs->bc[slot].dirty = 1;
}

static int bc_flush(vfat_fs_t *fs) {
    int rc = 0;
    uint32_t n = fs->bc_sets * VFAT_CACHE_WAYS;
    for (uint32_t w = 0; w < n; w++)
        if (fs->bc[w].sec != ~0u && fs->bc[w].dirty && bc_writeback(fs, w) < 0)
            rc = -1;
    return rc;
}

/* Forget sectors [sec, sec+n) without writing them: a cluster that is freed
 * or handed to a file's data, which bypasses the cache. */
static void bc_drop(vfat_fs_t *fs, uint32_t sec, uint32_t n) {
    for (uint32_t s = sec; s < sec + n; s++) {
        uint32_t base = (s & (fs->bc_sets - 1u)) * VFAT_CACHE_WAYS;
        for (uint32_t w = base; w < base + VFAT_CACHE_WAYS; w++)
            if (fs->bc[w].sec == s) { fs->bc[w].sec = ~0u; fs->bc[w].dirty = 0; fs->bc[w].age = 0; }
    }
}

/* ── FAT ────────────────────────────────────────────────────────────────── */

static uint32_t clus_sec(const vfat_fs_t *fs, uint32_t c) {
    return fs->data_sec + (c - 2) * fs->spc;
}

static int clus_ok(const vfat_fs_t *fs, uint32_t c) {
    return c >= 2 && c < fs->nclus + 2;
}

/* Byte `off` of the active FAT through the cache. */
static uint8_t *fat_byte(vfat_fs_t *fs, uint32_t off, uint32_t *slot) {
    uint8_t *p = bc_get(fs, fs->fat_start + off / fs->bps, 1, slot);
    return p ? p + off % fs->bps : NULL;
}

static int fat_rd(vfat_fs_t *fs, uint32_t c, uint32_t *v) {
    if (c >= fs->nclus + 2) return -1;
    uint8_t *p;
    if (fs->type == 32) {
        if (!(p = fat_byte(fs, c * 4, NULL))) return -1;
        *v = rd32(p) & 0x0FFFFFFFu;
    } else if (fs->type == 16) {
        if (!(p = fat_byte(fs, c * 2, NULL))) return -1;
        *v = rd16(p);
    } else {
        uint32_t off = c + c / 2;
        if (!(p = fat_byte(fs, off, NULL))) return -1;
        uint32_t lo = *p;
        if (!(p = fat_byte(fs, off + 1, NULL))) return -1;
        uint32_t x = lo | ((uint32_t)*p << 8);
        *v = (c & 1) ? x >> 4 : x & 0xFFFu;
    }
    return 0;
}

static int fat_wr(vfat_fs_t *fs, uint32_t c, uint32_t v) {
    if (c >= fs->nclus + 2) return -1;
    uint32_t slot;
    uint8_t *p;
    if (fs->type == 32) {
        if (!(p = fat_byte(fs, c * 4, &slot))) return -1;
        wr32(p, (rd32(p) & 0xF0000000u) | (v & 0x0FFFFFFFu));   /* top 4 bits are reserved */
        bc_mark(fs, slot);
    } else if (fs->type == 16) {
        if (!(p = fat_byte(fs, c * 2, &slot))) return -1;
        wr16(p, v);
        bc_mark(fs, slot);
    } else {
        uint32_t off = c + c / 2;
        v &= 0xFFFu;
        if (!(p = fat_byte(fs, off, &slot))) return -1;
        if (c & 1) *p = (uint8_t)((*p & 0x0F) | ((v & 0x0F) << 4));
        else       *p = (uint8_t)v;
        bc_mark(fs, slot);
        if (!(p = fat_byte(fs, off + 1, &slot))) return -1;
        if (c & 1) *p = (uint8_t)(v >> 4);
        else       *p = (uint8_t)((*p & 0xF0) | (v >> 8));
        bc_mark(fs, slot);
    }
    return 0;
}

/* The cluster after `c`: 0 at the end of the chain; -1 for an I/O error or a
 * link to a free, reserved or bad cluster (a corrupt chain). */
static int fat_next(vfat_fs_t *fs, uint32_t c, uint32_t *next) {
    uint32_t v;
    if (fat_rd(fs, c, &v) < 0) return -1;
    if (v >= fs->eoc_min) { *next = 0; return 0; }
    if (!clus_ok(fs, v)) {
        if (fs->io_errors++ < 8)
            printk("[VFAT] %s: cluster %u links to %u: corrupt chain (run fsck.vfat)\n",
                   fs->bp->name, (unsigned)c, (unsigned)v);
        return -1;
    }
    *next = v;
    return 0;
}

static void free_count_add(vfat_fs_t *fs, int32_t d) {
    if (fs->free_count != 0xFFFFFFFFu) fs->free_count = (uint32_t)((int32_t)fs->free_count + d);
    fs->fsinfo_dirty = 1;
}

/* Allocate one free cluster, mark it the end of a chain and link `prev` (if
 * any) to it. */
static int clus_alloc(vfat_fs_t *fs, uint32_t prev, uint32_t *out) {
    uint32_t start = fs->next_free;
    if (!clus_ok(fs, start)) start = 2;
    for (uint32_t i = 0; i < fs->nclus; i++) {
        uint32_t c = 2 + (start - 2 + 1 + i) % fs->nclus;
        uint32_t v;
        if (fat_rd(fs, c, &v) < 0) return E_IO;
        if (v != 0) continue;
        if (fat_wr(fs, c, fs->eoc) < 0) return E_IO;
        if (prev && fat_wr(fs, prev, c) < 0) return E_IO;
        free_count_add(fs, -1);
        fs->next_free = c;
        bc_drop(fs, clus_sec(fs, c), fs->spc);
        *out = c;
        return 0;
    }
    return E_NOSPC;
}

static int chain_free(vfat_fs_t *fs, uint32_t c) {
    for (uint32_t n = 0; clus_ok(fs, c) && n <= fs->nclus; n++) {
        uint32_t next;
        if (fat_next(fs, c, &next) < 0) {
            fat_wr(fs, c, 0);
            free_count_add(fs, 1);
            return E_IO;
        }
        if (fat_wr(fs, c, 0) < 0) return E_IO;
        free_count_add(fs, 1);
        bc_drop(fs, clus_sec(fs, c), fs->spc);
        c = next;
    }
    return 0;
}

static void fsinfo_store(vfat_fs_t *fs) {
    if (!fs->fsinfo_dirty || !fs->fsinfo_sec) return;
    uint32_t slot;
    uint8_t *p = bc_get(fs, fs->fsinfo_sec, 1, &slot);
    if (!p) return;
    wr32(p + 488, fs->free_count);
    wr32(p + 492, fs->next_free);
    bc_mark(fs, slot);
    fs->fsinfo_dirty = 0;
}

/* Boot sector flag and, on FAT16/32, the clean bit in FAT[1]. */
static int set_dirty(vfat_fs_t *fs, int dirty) {
    uint32_t slot;
    uint8_t *p = bc_get(fs, 0, 1, &slot);
    if (!p) return -1;
    if (dirty) p[fs->dirty_off] |= 1; else p[fs->dirty_off] &= (uint8_t)~1u;
    bc_mark(fs, slot);
    if (fs->type != 12) {
        uint32_t v, bit = fs->type == 32 ? 0x08000000u : 0x8000u;
        if (fat_rd(fs, 1, &v) == 0) {
            v = dirty ? (v & ~bit) : (v | bit);
            fat_wr(fs, 1, v);
        }
    }
    return bc_flush(fs);
}

/* ── Unicode ────────────────────────────────────────────────────────────── */

static const uint16_t cp437_hi[128] = {
    0x00C7,0x00FC,0x00E9,0x00E2,0x00E4,0x00E0,0x00E5,0x00E7,0x00EA,0x00EB,0x00E8,0x00EF,0x00EE,0x00EC,0x00C4,0x00C5,
    0x00C9,0x00E6,0x00C6,0x00F4,0x00F6,0x00F2,0x00FB,0x00F9,0x00FF,0x00D6,0x00DC,0x00A2,0x00A3,0x00A5,0x20A7,0x0192,
    0x00E1,0x00ED,0x00F3,0x00FA,0x00F1,0x00D1,0x00AA,0x00BA,0x00BF,0x2310,0x00AC,0x00BD,0x00BC,0x00A1,0x00AB,0x00BB,
    0x2591,0x2592,0x2593,0x2502,0x2524,0x2561,0x2562,0x2556,0x2555,0x2563,0x2551,0x2557,0x255D,0x255C,0x255B,0x2510,
    0x2514,0x2534,0x252C,0x251C,0x2500,0x253C,0x255E,0x255F,0x255A,0x2554,0x2569,0x2566,0x2560,0x2550,0x256C,0x2567,
    0x2568,0x2564,0x2565,0x2559,0x2558,0x2552,0x2553,0x256B,0x256A,0x2518,0x250C,0x2588,0x2584,0x258C,0x2590,0x2580,
    0x03B1,0x00DF,0x0393,0x03C0,0x03A3,0x03C3,0x00B5,0x03C4,0x03A6,0x0398,0x03A9,0x03B4,0x221E,0x03C6,0x03B5,0x2229,
    0x2261,0x00B1,0x2265,0x2264,0x2320,0x2321,0x00F7,0x2248,0x00B0,0x2219,0x00B7,0x221A,0x207F,0x00B2,0x25A0,0x00A0,
};

/* Simple upper-case mapping for name comparison (FAT names are
 * case-insensitive).  Turkish dotted/dotless i (U+0130, U+0131) map to
 * themselves, so "ı" and "I", "i" and "İ" stay distinct names. */
static uint32_t uc_upper(uint32_t c) {
    if (c < 0x80) return (c >= 'a' && c <= 'z') ? c - 32 : c;
    if (c < 0x100) {
        if ((c >= 0xE0 && c <= 0xFE && c != 0xF7)) return c - 32;
        if (c == 0xFF) return 0x178;
        return c;
    }
    if (c < 0x180) {
        if (c == 0x130 || c == 0x131 || c == 0x138 || c == 0x149 || c == 0x178) return c;
        if (c == 0x17F) return 'S';
        if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E))
            return (c & 1) ? c : c - 1;                     /* odd = upper */
        return (c & 1) ? c - 1 : c;                         /* even = upper */
    }
    if (c >= 0x3B1 && c <= 0x3C9 && c != 0x3C2) return c - 32;
    if (c >= 0x430 && c <= 0x44F) return c - 32;
    if (c >= 0x450 && c <= 0x45F) return c - 80;
    return c;
}

/* Next code point of NUL-terminated UTF-8 at *p; 0 at the end, -1 (and
 * stops) on a malformed sequence. */
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

/* Append code point `c` to `out` (capacity `cap` including the NUL). */
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

/* Case-insensitive equality of two UTF-8 names. */
static int name_eq(const char *a, const char *b) {
    for (;;) {
        int32_t x = utf8_next(&a), y = utf8_next(&b);
        if (x < 0 || y < 0) return 0;
        if (uc_upper((uint32_t)x) != uc_upper((uint32_t)y)) return 0;
        if (!x) return 1;
    }
}

/* ── Names ──────────────────────────────────────────────────────────────── */

static uint8_t lfn_sum(const uint8_t *n11) {
    uint8_t s = 0;
    for (int i = 0; i < 11; i++) s = (uint8_t)(((s & 1) << 7) + (s >> 1) + n11[i]);
    return s;
}

/* The 8.3 name of a short entry as UTF-8 ("README.TXT", or lower case as the
 * NT flags say). */
static void short_to_utf8(const uint8_t *e, char *out, uint32_t cap) {
    uint32_t len = 0;
    out[0] = '\0';
    int bl = 8, el = 3;
    while (bl > 0 && e[bl - 1] == ' ') bl--;
    while (el > 0 && e[8 + el - 1] == ' ') el--;
    for (int i = 0; i < bl + el; i++) {
        if (i == bl) utf8_put(out, &len, cap, '.');
        int j = i < bl ? i : 8 + (i - bl);
        uint32_t c = e[j];
        if (j == 0 && c == 0x05) c = 0xE5;
        if (c >= 0x80) c = cp437_hi[c - 0x80];
        else if (c >= 'A' && c <= 'Z' &&
                 (e[12] & (i < bl ? NT_LOWER_BASE : NT_LOWER_EXT))) c += 32;
        utf8_put(out, &len, cap, c);
    }
}

/* Characters allowed in a short name (besides upper-case letters and digits). */
static int sfn_char_ok(uint32_t c) {
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return 1;
    return c && strchr("$%'-_@~`!(){}^#&", (int)c) != NULL;
}

/* Long-name rules: no control characters, none of "*:<>?\| or /. */
static int lfn_char_ok(uint32_t c) {
    return c >= 0x20 && !(c < 0x80 && strchr("\"*/:<>?\\|", (int)c));
}

#define NAME_MAX_UNITS 255

/* Check `name` and turn it into UTF-16 (`u`, at most 255 units) and code
 * points (`cp`).  Trailing dots are dropped, as Windows and Linux do.
 * Returns the number of code points, or a negative errno. */
static int name_parse(const char *name, uint16_t *u, uint32_t *nu, uint32_t *cp) {
    uint32_t n = 0, k = 0;
    const char *p = name;
    for (;;) {
        int32_t c = utf8_next(&p);
        if (c < 0) return E_INVAL;
        if (!c) break;
        if (!lfn_char_ok((uint32_t)c)) return E_INVAL;
        if (n >= NAME_MAX_UNITS) return E_NAMETOOLONG;
        cp[n++] = (uint32_t)c;
    }
    while (n && cp[n - 1] == '.') n--;
    if (n == 0) return E_INVAL;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t c = cp[i];
        if (c >= 0x10000) {
            if (k + 2 > NAME_MAX_UNITS) return E_NAMETOOLONG;
            c -= 0x10000;
            u[k++] = (uint16_t)(0xD800 | (c >> 10));
            u[k++] = (uint16_t)(0xDC00 | (c & 0x3FF));
        } else {
            if (k + 1 > NAME_MAX_UNITS) return E_NAMETOOLONG;
            u[k++] = (uint16_t)c;
        }
    }
    *nu = k;
    return (int)n;
}

/* ── Directory entries ──────────────────────────────────────────────────── */

/* Sector and offset of entry `idx` of directory `d`.  0, 1 when the
 * directory's storage ends before it, -1 on an error. */
static int dir_pos(vfat_fs_t *fs, vfat_vnode_t *d, uint32_t idx, uint32_t *sec, uint32_t *off) {
    if (d->fixed_root) {
        if (idx >= fs->root_ents) return 1;
        uint32_t b = idx * 32;
        *sec = fs->root_sec + b / fs->bps;
        *off = b % fs->bps;
        return 0;
    }
    if (idx >= VFAT_MAX_DIRENTS) return 1;
    uint32_t epc = fs->cb / 32, ci = idx / epc, c, k;
    if (d->c_clus && d->c_idx <= ci) { c = d->c_clus; k = d->c_idx; }
    else { c = d->first; k = 0; }
    if (!clus_ok(fs, c)) return -1;
    while (k < ci) {
        uint32_t next;
        if (fat_next(fs, c, &next) < 0) return -1;
        if (!next) { d->c_idx = k; d->c_clus = c; return 1; }
        c = next;
        k++;
    }
    d->c_idx = k;
    d->c_clus = c;
    uint32_t b = (idx % epc) * 32;
    *sec = clus_sec(fs, c) + b / fs->bps;
    *off = b % fs->bps;
    return 0;
}

static int dir_get(vfat_fs_t *fs, vfat_vnode_t *d, uint32_t idx, uint8_t *e) {
    uint32_t sec, off;
    int r = dir_pos(fs, d, idx, &sec, &off);
    if (r) return r;
    uint8_t *p = bc_get(fs, sec, 1, NULL);
    if (!p) return -1;
    memcpy(e, p + off, 32);
    return 0;
}

static int dir_put(vfat_fs_t *fs, vfat_vnode_t *d, uint32_t idx, const uint8_t *e) {
    uint32_t sec, off, slot;
    int r = dir_pos(fs, d, idx, &sec, &off);
    if (r) return -1;
    uint8_t *p = bc_get(fs, sec, 1, &slot);
    if (!p) return -1;
    memcpy(p + off, e, 32);
    bc_mark(fs, slot);
    return 0;
}

typedef struct {
    uint32_t first_idx;          /* first entry of the set (an LFN entry or the short one) */
    uint32_t sfn_idx;            /* the short entry */
    uint8_t  e[32];
    char     name[256];          /* UTF-8: the long name, else the short one */
    char     sname[40];          /* the 8.3 name, UTF-8 */
} dent_t;

/* The next live entry at or after *idx (volume labels and deleted entries
 * skipped; "." and ".." included); *idx moves past it.  0 found, 1 end of
 * directory, -1 error. */
static int dir_iter(vfat_fs_t *fs, vfat_vnode_t *d, uint32_t *idx, dent_t *o) {
    uint16_t lfn[260];
    int have = 0, left = 0;
    uint8_t sum = 0;
    uint32_t start = 0, nparts = 0;
    uint8_t e[32];
    for (;;) {
        int r = dir_get(fs, d, *idx, e);
        if (r) return r < 0 ? -1 : 1;
        uint32_t i = (*idx)++;
        if (e[0] == 0x00) { (*idx)--; return 1; }
        if (e[0] == 0xE5) { have = 0; continue; }
        if ((e[11] & 0x3F) == ATTR_LFN) {
            uint32_t ord = e[0] & 0x1F;
            if (e[0] & 0x40) {
                if (ord == 0 || ord > 20) { have = 0; continue; }
                have = 1; left = (int)ord; sum = e[13]; start = i; nparts = ord;
                for (int k = 0; k < 260; k++) lfn[k] = 0xFFFF;
            } else if (!have || (int)ord != left || e[13] != sum) {
                have = 0;
                continue;
            }
            static const uint8_t at[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
            for (int k = 0; k < 13; k++) lfn[(ord - 1) * 13 + k] = rd16(e + at[k]);
            left--;
            continue;
        }
        if (e[11] & ATTR_VOLID) { have = 0; continue; }
        o->sfn_idx = i;
        o->first_idx = i;
        memcpy(o->e, e, 32);
        short_to_utf8(e, o->sname, sizeof(o->sname));
        int used = 0;
        if (have && left == 0 && sum == lfn_sum(e)) {
            uint32_t len = 0, n = nparts * 13;
            int ok = 1;
            o->name[0] = '\0';
            for (uint32_t k = 0; k < n && ok; k++) {
                uint32_t c = lfn[k];
                if (c == 0x0000 || c == 0xFFFF) break;
                if (c >= 0xD800 && c <= 0xDBFF && k + 1 < n &&
                    lfn[k + 1] >= 0xDC00 && lfn[k + 1] <= 0xDFFF) {
                    c = 0x10000 + ((c - 0xD800) << 10) + (lfn[k + 1] - 0xDC00);
                    k++;
                } else if (c >= 0xD800 && c <= 0xDFFF) {
                    c = 0xFFFD;
                }
                if (c == '/' || c == 0) c = '_';
                if (utf8_put(o->name, &len, sizeof(o->name), c) < 0) ok = 0;
            }
            if (ok && len) { used = 1; o->first_idx = start; }
        }
        if (!used) {
            strncpy(o->name, o->sname, sizeof(o->name) - 1);
            o->name[sizeof(o->name) - 1] = '\0';
        }
        return 0;
    }
}

static int is_dot(const dent_t *de) {
    return de->e[0] == '.' && (de->e[1] == ' ' || (de->e[1] == '.' && de->e[2] == ' '));
}

/* Find `name` (long or short form, case-insensitive) in `d`.  0, -ENOENT or
 * -EIO.  Dot entries are never matched. */
static int dir_find(vfat_fs_t *fs, vfat_vnode_t *d, const char *name, dent_t *o) {
    uint32_t idx = 0;
    for (;;) {
        int r = dir_iter(fs, d, &idx, o);
        if (r) return r < 0 ? E_IO : E_NOENT;
        if (is_dot(o)) continue;
        if (name_eq(o->name, name) || name_eq(o->sname, name)) return 0;
    }
}

/* Is the 11-byte short name `n11` taken in `d` (ignoring entry `skip`)? */
static int dir_short_taken(vfat_fs_t *fs, vfat_vnode_t *d, const uint8_t *n11, uint32_t skip) {
    uint8_t e[32];
    for (uint32_t i = 0;; i++) {
        int r = dir_get(fs, d, i, e);
        if (r) return r < 0 ? -1 : 0;
        if (e[0] == 0) return 0;
        if (e[0] == 0xE5 || (e[11] & 0x3F) == ATTR_LFN || (e[11] & ATTR_VOLID)) continue;
        if (i != skip && memcmp(e, n11, 11) == 0) return 1;
    }
}

/* Short name for `cp[0..n)` in `d` (fatgen103, "Basis-Name Generation" and
 * "Numeric-Tail Generation").  Sets *need_lfn when the long name must be
 * stored as well, *nt to the NT case flags when it need not. */
static int make_short(vfat_fs_t *fs, vfat_vnode_t *d, const uint32_t *cp, uint32_t n,
                      uint32_t skip, uint8_t *out, int *need_lfn, uint8_t *nt) {
    memset(out, ' ', 11);
    *nt = 0;
    /* Already a valid 8.3 name? */
    uint32_t dot = n;
    int dots = 0, ok = 1;
    for (uint32_t i = 0; i < n; i++)
        if (cp[i] == '.') { dot = i; dots++; }
    uint32_t bl = dot, el = dot < n ? n - dot - 1 : 0;
    if (dots > 1 || bl == 0 || bl > 8 || el > 3 || (dot < n && el == 0)) ok = 0;
    int bup = 0, blo = 0, eup = 0, elo = 0;
    for (uint32_t i = 0; ok && i < n; i++) {
        if (i == dot) continue;
        uint32_t c = cp[i];
        if (c >= 0x80 || !sfn_char_ok(uc_upper(c))) { ok = 0; break; }
        if (c >= 'a' && c <= 'z') { if (i < dot) blo = 1; else elo = 1; }
        if (c >= 'A' && c <= 'Z') { if (i < dot) bup = 1; else eup = 1; }
    }
    if (ok) {
        for (uint32_t i = 0; i < bl; i++) out[i] = (uint8_t)uc_upper(cp[i]);
        for (uint32_t i = 0; i < el; i++) out[8 + i] = (uint8_t)uc_upper(cp[dot + 1 + i]);
        *need_lfn = (bup && blo) || (eup && elo);
        if (!*need_lfn) *nt = (uint8_t)((blo ? NT_LOWER_BASE : 0) | (elo ? NT_LOWER_EXT : 0));
        int t = dir_short_taken(fs, d, out, skip);
        if (t < 0) return E_IO;
        if (!t) return 0;
    }
    /* Basis name: upper case, invalid characters to '_', spaces and dots
     * dropped, at most 8 + 3. */
    *need_lfn = 1;
    *nt = 0;
    uint8_t base[8], ext[3];
    uint32_t nb = 0, ne = 0, lead = 0;
    while (lead < n && (cp[lead] == '.' || cp[lead] == ' ')) lead++;
    uint32_t last = n;
    for (uint32_t i = lead; i < n; i++) if (cp[i] == '.') last = i;
    for (uint32_t i = lead; i < last && nb < 8; i++) {
        uint32_t c = cp[i];
        if (c == ' ' || c == '.') continue;
        c = uc_upper(c);
        base[nb++] = (uint8_t)((c < 0x80 && sfn_char_ok(c)) ? c : '_');
    }
    for (uint32_t i = last + 1; i < n && ne < 3; i++) {
        uint32_t c = cp[i];
        if (c == ' ') continue;
        c = uc_upper(c);
        ext[ne++] = (uint8_t)((c < 0x80 && sfn_char_ok(c)) ? c : '_');
    }
    if (nb == 0) base[nb++] = '_';
    for (uint32_t num = 1; num < 1000000; num++) {
        char tail[8];
        uint32_t tl = 0, x = num;
        char digits[7];
        uint32_t nd = 0;
        while (x) { digits[nd++] = (char)('0' + x % 10); x /= 10; }
        tail[tl++] = '~';
        while (nd) tail[tl++] = digits[--nd];
        uint32_t keep = nb + tl > 8 ? 8 - tl : nb;
        memset(out, ' ', 11);
        memcpy(out, base, keep);
        memcpy(out + keep, tail, tl);
        memcpy(out + 8, ext, ne);
        int t = dir_short_taken(fs, d, out, skip);
        if (t < 0) return E_IO;
        if (!t) return 0;
    }
    return E_EXIST;
}

/* Find `n` consecutive free entries in `d`, growing it by a cluster when
 * needed.  Returns the first index or a negative errno. */
static int dir_extend(vfat_fs_t *fs, vfat_vnode_t *d);

static int64_t dir_slots(vfat_fs_t *fs, vfat_vnode_t *d, uint32_t n) {
    uint32_t run = 0, run_start = 0;
    uint8_t e[32];
    for (uint32_t idx = 0;; idx++) {
        int r = dir_get(fs, d, idx, e);
        if (r < 0) return E_IO;
        if (r == 1) {
            if (d->fixed_root || idx + n - run > VFAT_MAX_DIRENTS) return E_NOSPC;
            int x = dir_extend(fs, d);
            if (x < 0) return x;
            idx--;
            continue;
        }
        if (e[0] == 0x00 || e[0] == 0xE5) {
            if (!run) run_start = idx;
            if (++run == n) return run_start;
        } else {
            run = 0;
        }
    }
}

static int clus_zero(vfat_fs_t *fs, uint32_t c) {
    for (uint32_t s = 0; s < fs->spc; s++) {
        uint32_t slot;
        if (!bc_get(fs, clus_sec(fs, c) + s, 0, &slot)) return E_IO;
        bc_mark(fs, slot);
    }
    return 0;
}

static int dir_extend(vfat_fs_t *fs, vfat_vnode_t *d) {
    uint32_t c = d->first, n = 0, next, nc;
    if (d->c_clus) { c = d->c_clus; }
    for (;;) {
        if (fat_next(fs, c, &next) < 0) return E_IO;
        if (!next) break;
        c = next;
        if (++n > fs->nclus) return E_IO;
    }
    int r = clus_alloc(fs, c, &nc);
    if (r < 0) return r;
    return clus_zero(fs, nc);
}

/* Write the entries for a new name: LFN entries (when needed) and the short
 * entry `se` (whose name bytes 0..10 are filled in here).  Returns the short
 * entry's index or a negative errno. */
static int64_t dir_add(vfat_fs_t *fs, vfat_vnode_t *d, const char *name, uint8_t *se,
                       uint32_t skip) {
    uint16_t u[NAME_MAX_UNITS + 1];
    uint32_t cp[NAME_MAX_UNITS + 1], nu;
    int n = name_parse(name, u, &nu, cp);
    if (n < 0) return n;
    int need_lfn;
    uint8_t nt;
    int r = make_short(fs, d, cp, (uint32_t)n, skip, se, &need_lfn, &nt);
    if (r < 0) return r;
    se[12] = nt;
    uint32_t nl = need_lfn ? (nu + 12) / 13 : 0;
    int64_t first = dir_slots(fs, d, nl + 1);
    if (first < 0) return first;
    uint8_t sum = lfn_sum(se);
    static const uint8_t at[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
    for (uint32_t k = nl; k >= 1; k--) {
        uint8_t e[32];
        memset(e, 0, 32);
        e[0] = (uint8_t)(k | (k == nl ? 0x40 : 0));
        e[11] = ATTR_LFN;
        e[13] = sum;
        for (uint32_t j = 0; j < 13; j++) {
            uint32_t pos = (k - 1) * 13 + j;
            uint16_t ch = pos < nu ? u[pos] : pos == nu ? 0x0000 : 0xFFFF;
            wr16(e + at[j], ch);
        }
        if (dir_put(fs, d, (uint32_t)first + (nl - k), e) < 0) return E_IO;
    }
    if (dir_put(fs, d, (uint32_t)first + nl, se) < 0) return E_IO;
    return first + nl;
}

/* Mark entries first..last of `d` deleted. */
static int dir_del(vfat_fs_t *fs, vfat_vnode_t *d, uint32_t first, uint32_t last) {
    uint8_t e[32];
    for (uint32_t i = first; i <= last; i++) {
        if (dir_get(fs, d, i, e) != 0) return E_IO;
        e[0] = 0xE5;
        if (dir_put(fs, d, i, e) < 0) return E_IO;
    }
    return 0;
}

/* ── Nodes ──────────────────────────────────────────────────────────────── */

static void node_hooks(vfat_vnode_t *vn);

static uint32_t mode_of(const vfat_fs_t *fs, const vfat_vnode_t *vn) {
    uint32_t m = 0777 & ~(vn->is_dir ? fs->dmask : fs->fmask);
    if (!vn->is_dir && (vn->attr & ATTR_RO)) m &= ~0222u;
    return m;
}

static uint32_t ino_of(uint64_t key) {
    return key ? (uint32_t)(key / 32) + 2 : 1;
}

static void node_from_entry(vfat_vnode_t *vn, const uint8_t *e) {
    vfat_fs_t *fs = vn->fs;
    vn->attr = e[11];
    vn->is_dir = (e[11] & ATTR_DIR) != 0;
    vn->first = rd16(e + 26) | (fs->type == 32 ? (uint32_t)rd16(e + 20) << 16 : 0);
    if (!clus_ok(fs, vn->first)) vn->first = 0;
    vn->size = vn->is_dir ? 0 : rd32(e + 28);
    vn->mtime = fat_to_unix(rd16(e + 24), rd16(e + 22));
    vn->atime = fat_to_unix(rd16(e + 18), 0);
    if (vn->atime < vn->mtime && vn->atime / 86400 == vn->mtime / 86400) vn->atime = vn->mtime;
    vn->ctime = vn->mtime;
    vn->c_clus = 0;
    vn->r_valid = 0;
    vfs_node_t *n = &vn->vnode;
    n->flags = vn->is_dir ? VFS_FLAG_DIR : VFS_FLAG_FILE;
    n->size  = vn->is_dir ? fs->cb : vn->size;
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

static void node_hash_del(vfat_fs_t *fs, vfat_vnode_t *vn) {
    vfat_vnode_t **pp = &fs->nodes[(uint32_t)(vn->key / 32) % VFAT_NODE_BUCKETS];
    while (*pp && *pp != vn) pp = &(*pp)->hnext;
    if (*pp) *pp = vn->hnext;
    vn->hnext = NULL;
}

static void node_hash_add(vfat_fs_t *fs, vfat_vnode_t *vn) {
    vfat_vnode_t **b = &fs->nodes[(uint32_t)(vn->key / 32) % VFAT_NODE_BUCKETS];
    vn->hnext = *b;
    *b = vn;
}

static uint64_t ent_key(const vfat_fs_t *fs, uint32_t sec, uint32_t off) {
    return (uint64_t)sec * fs->bps + off;
}

static vfat_vnode_t *node_find(vfat_fs_t *fs, uint64_t key) {
    vfat_vnode_t *vn = fs->nodes[(uint32_t)(key / 32) % VFAT_NODE_BUCKETS];
    while (vn && vn->key != key) vn = vn->hnext;
    return vn;
}

/* The node for entry `de` of directory `d`. */
static vfat_vnode_t *node_get(vfat_fs_t *fs, vfat_vnode_t *d, const dent_t *de) {
    uint32_t sec, off;
    if (dir_pos(fs, d, de->sfn_idx, &sec, &off) != 0) return NULL;
    uint64_t key = ent_key(fs, sec, off);
    vfat_vnode_t *vn = node_find(fs, key);
    if (vn) return vn;
    vn = (vfat_vnode_t *)kmalloc(sizeof(*vn));
    if (!vn) return NULL;
    memset(vn, 0, sizeof(*vn));
    vn->fs = fs;
    vn->key = key;
    vn->ent_sec = sec;
    vn->ent_off = off;
    vn->parent = d;
    node_from_entry(vn, de->e);
    strncpy(vn->vnode.name, de->name, 255);
    node_hooks(vn);
    node_hash_add(fs, vn);
    return vn;
}

/* Write the node's size, first cluster, attributes and times to its entry. */
static int node_store(vfat_vnode_t *vn) {
    vfat_fs_t *fs = vn->fs;
    if (!vn->key || vn->deleted) return 0;
    uint32_t slot;
    uint8_t *p = bc_get(fs, vn->ent_sec, 1, &slot);
    if (!p) return E_IO;
    uint8_t *e = p + vn->ent_off;
    e[11] = vn->attr;
    wr16(e + 26, vn->first & 0xFFFF);
    wr16(e + 20, fs->type == 32 ? vn->first >> 16 : 0);
    wr32(e + 28, vn->is_dir ? 0 : vn->size);
    uint16_t dt, tm;
    unix_to_fat(vn->mtime, &dt, &tm);
    wr16(e + 22, tm);
    wr16(e + 24, dt);
    unix_to_fat(vn->atime, &dt, &tm);
    wr16(e + 18, dt);
    bc_mark(fs, slot);
    vfs_node_t *n = &vn->vnode;
    n->size  = vn->is_dir ? fs->cb : vn->size;
    n->mtime = vn->mtime;
    n->atime = vn->atime;
    n->ctime = vn->ctime;
    n->mask  = mode_of(fs, vn);
    return 0;
}

static void touch(vfat_vnode_t *vn) {
    vn->mtime = vn->ctime = vn->atime = vfat_now();
    vn->attr |= vn->is_dir ? 0 : ATTR_ARCH;
}

/* ── File data ──────────────────────────────────────────────────────────── */

/* The ci-th cluster of the node's chain.  0, 1 past the end, -1 error. */
static int file_clus(vfat_fs_t *fs, vfat_vnode_t *vn, uint32_t ci, uint32_t *out) {
    uint32_t c, k;
    if (vn->c_clus && vn->c_idx <= ci) { c = vn->c_clus; k = vn->c_idx; }
    else { c = vn->first; k = 0; }
    if (!c) return 1;
    if (!clus_ok(fs, c)) return -1;
    while (k < ci) {
        uint32_t next;
        if (fat_next(fs, c, &next) < 0) return -1;
        if (!next) return 1;
        c = next;
        k++;
    }
    vn->c_idx = k;
    vn->c_clus = c;
    *out = c;
    return 0;
}

/* Make the chain at least `need` clusters long.  On failure the clusters
 * added here are given back. */
static int file_grow(vfat_fs_t *fs, vfat_vnode_t *vn, uint32_t need) {
    if (need == 0) return 0;
    uint32_t tail = 0, have = 0;
    if (vn->first) {
        uint32_t c, k;
        if (vn->c_clus) { c = vn->c_clus; k = vn->c_idx; } else { c = vn->first; k = 0; }
        for (;;) {
            uint32_t next;
            if (fat_next(fs, c, &next) < 0) return E_IO;
            if (!next) break;
            c = next;
            if (++k > fs->nclus) return E_IO;
        }
        tail = c;
        have = k + 1;
        vn->c_idx = k;
        vn->c_clus = c;
    }
    uint32_t old_tail = tail, old_first = vn->first;
    while (have < need) {
        uint32_t c;
        int r = clus_alloc(fs, tail, &c);
        if (r < 0) {
            /* Undo: cut the chain back where it was. */
            if (old_tail) {
                uint32_t next;
                if (fat_next(fs, old_tail, &next) == 0 && next) chain_free(fs, next);
                fat_wr(fs, old_tail, fs->eoc);
            } else if (vn->first && !old_first) {
                chain_free(fs, vn->first);
                vn->first = 0;
            }
            vn->c_clus = 0;
            return r;
        }
        if (!vn->first) vn->first = c;
        tail = c;
        vn->c_idx = have;
        vn->c_clus = c;
        have++;
    }
    return 0;
}

/* Move `len` bytes at file offset `off` between `buf` and the device.
 * `buf` NULL with `wr` writes zeros.  0 or -EIO. */
static int file_io_n(vfat_fs_t *fs, vfat_vnode_t *vn, uint32_t off, uint32_t len,
                     uint8_t *buf, int wr, uint32_t *donep) {
    uint32_t done = 0;
    *donep = 0;
    while (done < len) {
        uint32_t pos = off + done, ci = pos / fs->cb, co = pos % fs->cb, c;
        if (file_clus(fs, vn, ci, &c) != 0) return E_IO;
        /* Extend over physically contiguous clusters. */
        uint32_t run = 1, want = len - done;
        while ((uint64_t)run * fs->cb - co < want && run < 256) {
            uint32_t next;
            if (fat_next(fs, c + run - 1, &next) < 0) return E_IO;
            if (next != c + run) break;
            run++;
        }
        if (run > 1) { vn->c_idx = ci + run - 1; vn->c_clus = c + run - 1; }
        uint32_t avail = run * fs->cb - co;
        uint32_t n = avail < want ? avail : want;
        uint32_t sec = clus_sec(fs, c) + co / fs->bps, so = co % fs->bps;
        if (so == 0 && n >= fs->bps) {
            uint32_t k = n / fs->bps;
            n = k * fs->bps;
            if (!wr) {
                if (dev_io(fs, sec, k, buf + done, 0) < 0) return E_IO;
            } else if (buf) {
                if (dev_io(fs, sec, k, buf + done, 1) < 0) return E_IO;
            } else {
                uint32_t zs = VFAT_ZBUF / fs->bps;
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

static int file_io(vfat_fs_t *fs, vfat_vnode_t *vn, uint32_t off, uint32_t len,
                   uint8_t *buf, int wr) {
    uint32_t done;
    return file_io_n(fs, vn, off, len, buf, wr, &done);
}

static uint32_t nclus_for(const vfat_fs_t *fs, uint32_t size) {
    return (uint32_t)(((uint64_t)size + fs->cb - 1) / fs->cb);
}

/* Grow the file to `size` bytes, the new part reading as zeros. */
static int file_extend(vfat_fs_t *fs, vfat_vnode_t *vn, uint32_t size) {
    if (size <= vn->size) return 0;
    int r = file_grow(fs, vn, nclus_for(fs, size));
    if (r < 0) return r;
    r = file_io(fs, vn, vn->size, size - vn->size, NULL, 1);
    if (r < 0) return r;
    vn->size = size;
    return 0;
}

static int file_shrink(vfat_fs_t *fs, vfat_vnode_t *vn, uint32_t size) {
    uint32_t keep = nclus_for(fs, size);
    vn->c_clus = 0;
    if (keep == 0) {
        if (vn->first) {
            uint32_t f = vn->first;
            vn->first = 0;
            if (chain_free(fs, f) < 0) return E_IO;
        }
    } else {
        uint32_t c, next;
        if (file_clus(fs, vn, keep - 1, &c) == 0) {
            if (fat_next(fs, c, &next) < 0) return E_IO;
            if (next) {
                if (fat_wr(fs, c, fs->eoc) < 0) return E_IO;
                if (chain_free(fs, next) < 0) return E_IO;
            }
        }
        vn->c_clus = 0;
    }
    vn->size = size;
    return 0;
}

static uint32_t vfat_read(vfs_node_t *n, uint32_t off, uint32_t len, uint8_t *buf) {
    vfat_vnode_t *vn = (vfat_vnode_t *)n;
    vfat_fs_t *fs = vn->fs;
    fs_lock(fs);
    uint32_t got = 0;
    if (off < vn->size) {
        /* An I/O error ends the read early (as fs/ext4.c does): callers
         * other than read(2) take the result as a length. */
        if (len > vn->size - off) len = vn->size - off;
        file_io_n(fs, vn, off, len, buf, 0, &got);
    }
    fs_unlock(fs);
    return got;
}

static uint32_t vfat_write(vfs_node_t *n, uint32_t off, uint32_t len, const uint8_t *buf) {
    vfat_vnode_t *vn = (vfat_vnode_t *)n;
    vfat_fs_t *fs = vn->fs;
    if (len == 0) return 0;
    if (off == 0xFFFFFFFFu) return VFS_WRITE_EFBIG;
    if (len > 0xFFFFFFFFu - off) len = 0xFFFFFFFFu - off;
    fs_lock(fs);
    int r = 0;
    if (fs->ro) r = E_ROFS;
    if (!r && off + len > vn->size)
        r = file_grow(fs, vn, nclus_for(fs, off + len));
    if (!r && off > vn->size)
        r = file_io(fs, vn, vn->size, off - vn->size, NULL, 1);
    if (!r)
        r = file_io(fs, vn, off, len, (uint8_t *)buf, 1);
    if (!r) {
        if (off + len > vn->size) vn->size = off + len;
        touch(vn);
        r = node_store(vn);
    }
    if (fs_unlock(fs) < 0 && !r) r = E_IO;
    return r < 0 ? (uint32_t)r : len;
}

static int vfat_truncate(vfs_node_t *n, uint32_t size) {
    vfat_vnode_t *vn = (vfat_vnode_t *)n;
    vfat_fs_t *fs = vn->fs;
    if (vn->is_dir) return E_ISDIR;
    fs_lock(fs);
    int r = 0;
    if (fs->ro) r = E_ROFS;
    else if (size > vn->size) r = file_extend(fs, vn, size);
    else if (size < vn->size) r = file_shrink(fs, vn, size);
    if (!r) {
        touch(vn);
        r = node_store(vn);
    }
    if (fs_unlock(fs) < 0 && !r) r = E_IO;
    return r;
}

/* ── Directory operations ───────────────────────────────────────────────── */

static vfs_node_t *vfat_finddir(vfs_node_t *dir, const char *name) {
    vfat_vnode_t *d = (vfat_vnode_t *)dir;
    vfat_fs_t *fs = d->fs;
    if (strcmp(name, ".") == 0) return dir;
    if (strcmp(name, "..") == 0) return &d->parent->vnode;
    if (d->deleted) return NULL;
    char key[256];
    strncpy(key, name, 255);
    key[255] = '\0';
    size_t l = strlen(key);
    while (l && key[l - 1] == '.') key[--l] = '\0';
    if (!l) return NULL;
    fs_lock(fs);
    dent_t *de = (dent_t *)kmalloc(sizeof(dent_t));
    vfat_vnode_t *vn = NULL;
    if (de && dir_find(fs, d, key, de) == 0)
        vn = node_get(fs, d, de);
    if (de) kfree(de);
    fs_unlock(fs);
    return vn ? &vn->vnode : NULL;
}

static int vfat_readdir(vfs_node_t *dir, uint32_t req, vfs_dirent_t *out) {
    vfat_vnode_t *d = (vfat_vnode_t *)dir;
    vfat_fs_t *fs = d->fs;
    uint32_t base = 0;
    if (d->key == 0) {
        /* The root directory has no "." and ".." entries on disk. */
        if (req < 2) {
            out->ino = req ? d->parent->vnode.inode : d->vnode.inode;
            out->type = VFS_FLAG_DIR;
            strncpy(out->name, req ? ".." : ".", 255);
            return 0;
        }
        base = 2;
    }
    if (d->deleted) return -1;
    fs_lock(fs);
    dent_t *de = (dent_t *)kmalloc(sizeof(dent_t));
    int rc = -1;
    if (!de) goto out;
    /* The cursor holds the slot just past the entry last returned.  Slots
     * never move in FAT (a deleted entry only gets a mark), so `rm -r`,
     * which unlinks entries between getdents calls, still sees every one. */
    uint32_t i, pos;
    if (d->r_valid && d->r_idx <= req) { i = d->r_idx; pos = d->r_pos; }
    else if (d->r_valid && d->r_idx == req + 1) { i = req; pos = d->r_prev; }
    else { i = base; pos = 0; }
    for (;;) {
        uint32_t at = pos;
        int r = dir_iter(fs, d, &pos, de);
        if (r) { d->r_valid = 0; break; }
        if (i == req) {
            /* getdents asks again for an entry that did not fit its buffer:
             * r_prev is where that one was looked for. */
            d->r_idx = i + 1;
            d->r_pos = pos;
            d->r_prev = at;
            d->r_valid = 1;
            uint32_t sec, off;
            if (is_dot(de)) {
                vfat_vnode_t *t = de->e[1] == '.' ? d->parent : d;
                out->ino = t->vnode.inode;
            } else {
                out->ino = dir_pos(fs, d, de->sfn_idx, &sec, &off) == 0
                         ? ino_of(ent_key(fs, sec, off)) : 0;
            }
            out->type = (de->e[11] & ATTR_DIR) ? VFS_FLAG_DIR : VFS_FLAG_FILE;
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

static void fill_short(uint8_t *e, uint8_t attr, uint32_t clus, uint32_t t) {
    memset(e, 0, 32);
    e[11] = attr;
    uint16_t dt, tm;
    unix_to_fat(t, &dt, &tm);
    e[13] = (uint8_t)((t & 1) ? 100 : 0);           /* tenths: the odd second */
    wr16(e + 14, tm);
    wr16(e + 16, dt);
    wr16(e + 18, dt);
    wr16(e + 20, clus >> 16);
    wr16(e + 22, tm);
    wr16(e + 24, dt);
    wr16(e + 26, clus & 0xFFFF);
}

static int vfat_create(vfs_node_t *dir, const char *name, uint32_t flags) {
    vfat_vnode_t *d = (vfat_vnode_t *)dir;
    vfat_fs_t *fs = d->fs;
    if (flags != VFS_FLAG_FILE && flags != VFS_FLAG_DIR) return E_PERM;
    fs_lock(fs);
    int r = 0;
    uint32_t clus = 0;
    dent_t *de = (dent_t *)kmalloc(sizeof(dent_t));
    if (!de) { r = E_NOMEM; goto out; }
    if (fs->ro) { r = E_ROFS; goto out; }
    if (d->deleted) { r = E_NOENT; goto out; }
    r = dir_find(fs, d, name, de);
    if (r == 0) { r = E_EXIST; goto out; }
    if (r != E_NOENT) goto out;
    uint32_t t = vfat_now();
    if (flags == VFS_FLAG_DIR) {
        r = clus_alloc(fs, 0, &clus);
        if (r < 0) goto out;
        if ((r = clus_zero(fs, clus)) < 0) goto undo;
        uint8_t e[32];
        uint32_t up = d->key == 0 ? 0 : d->first;    /* ".." of a child of the root is 0 */
        fill_short(e, ATTR_DIR, clus, t);
        memcpy(e, ".          ", 11);
        uint32_t slot;
        uint8_t *p = bc_get(fs, clus_sec(fs, clus), 1, &slot);
        if (!p) { r = E_IO; goto undo; }
        memcpy(p, e, 32);
        fill_short(e, ATTR_DIR, up, t);
        memcpy(e, "..         ", 11);
        memcpy(p + 32, e, 32);
        bc_mark(fs, slot);
    }
    uint8_t se[32];
    fill_short(se, flags == VFS_FLAG_DIR ? ATTR_DIR : ATTR_ARCH, clus, t);
    int64_t at = dir_add(fs, d, name, se, ~0u);
    if (at < 0) { r = (int)at; goto undo; }
    r = 0;
    goto out;
undo:
    if (clus) chain_free(fs, clus);
out:
    if (de) kfree(de);
    if (fs_unlock(fs) < 0 && !r) r = E_IO;
    return r;
}

/* An unlinked node: out of the hash; its clusters go once nothing has it
 * open. */
static void node_orphan(vfat_fs_t *fs, vfat_vnode_t *vn) {
    node_hash_del(fs, vn);
    vn->deleted = 1;
    vn->hnext = fs->dead;
    fs->dead = vn;
    if (vn->refs == 0 && vn->first) {
        chain_free(fs, vn->first);
        vn->first = 0;
    }
}

static int dir_is_empty(vfat_fs_t *fs, vfat_vnode_t *d) {
    dent_t *de = (dent_t *)kmalloc(sizeof(dent_t));
    if (!de) return E_NOMEM;
    uint32_t idx = 0;
    int r;
    for (;;) {
        r = dir_iter(fs, d, &idx, de);
        if (r) { r = r < 0 ? E_IO : 1; break; }
        if (!is_dot(de)) { r = 0; break; }
    }
    kfree(de);
    return r;
}

static int vfat_unlink(vfs_node_t *dir, const char *name) {
    vfat_vnode_t *d = (vfat_vnode_t *)dir;
    vfat_fs_t *fs = d->fs;
    fs_lock(fs);
    int r;
    dent_t *de = (dent_t *)kmalloc(sizeof(dent_t));
    if (!de) { r = E_NOMEM; goto out; }
    if (fs->ro) { r = E_ROFS; goto out; }
    if ((r = dir_find(fs, d, name, de)) < 0) goto out;
    vfat_vnode_t *vn = node_get(fs, d, de);
    if (!vn) { r = E_IO; goto out; }
    if (vn->is_dir && (r = dir_is_empty(fs, vn)) <= 0) {
        if (r == 0) r = E_NOTEMPTY;
        goto out;
    }
    if ((r = dir_del(fs, d, de->first_idx, de->sfn_idx)) < 0) goto out;
    node_orphan(fs, vn);
    d->mtime = d->ctime = vfat_now();
    node_store(d);
    r = 0;
out:
    if (de) kfree(de);
    if (fs_unlock(fs) < 0 && !r) r = E_IO;
    return r;
}

/* Re-key `vn` to the short entry at index `idx` of `d`. */
static int node_move(vfat_fs_t *fs, vfat_vnode_t *vn, vfat_vnode_t *d, uint32_t idx,
                     const char *name) {
    uint32_t sec, off;
    if (dir_pos(fs, d, idx, &sec, &off) != 0) return E_IO;
    node_hash_del(fs, vn);
    vn->key = ent_key(fs, sec, off);
    vn->ent_sec = sec;
    vn->ent_off = off;
    vn->parent = d;
    vn->vnode.inode = ino_of(vn->key);
    strncpy(vn->vnode.name, name, 255);
    vn->vnode.name[255] = '\0';
    node_hash_add(fs, vn);
    return 0;
}

static int vfat_rename(vfs_node_t *odir, const char *oname, vfs_node_t *ndir, const char *nname) {
    vfat_vnode_t *od = (vfat_vnode_t *)odir, *nd = (vfat_vnode_t *)ndir;
    vfat_fs_t *fs = od->fs;
    fs_lock(fs);
    int r;
    dent_t *src = (dent_t *)kmalloc(sizeof(dent_t));
    dent_t *dst = (dent_t *)kmalloc(sizeof(dent_t));
    if (!src || !dst) { r = E_NOMEM; goto out; }
    if (fs->ro) { r = E_ROFS; goto out; }
    if (od->deleted || nd->deleted) { r = E_NOENT; goto out; }
    if ((r = dir_find(fs, od, oname, src)) < 0) goto out;
    vfat_vnode_t *vn = node_get(fs, od, src);
    if (!vn) { r = E_IO; goto out; }
    /* A directory cannot move below itself. */
    if (vn->is_dir)
        for (vfat_vnode_t *a = nd; ; a = a->parent) {
            if (a == vn) { r = E_INVAL; goto out; }
            if (a->key == 0) break;
        }
    {
        uint16_t u[NAME_MAX_UNITS + 1];
        uint32_t cp[NAME_MAX_UNITS + 1], nu;
        int pn = name_parse(nname, u, &nu, cp);
        if (pn < 0) { r = pn; goto out; }
    }
    r = dir_find(fs, nd, nname, dst);
    if (r != 0 && r != E_NOENT) goto out;
    int exists = r == 0;
    if (exists && nd == od && dst->sfn_idx == src->sfn_idx) {
        /* The same entry: only the spelling may change. */
        if (strcmp(src->name, nname) == 0) { r = 0; goto out; }
        exists = 0;
    }
    uint8_t se[32];
    memcpy(se, src->e, 32);
    if (exists) {
        vfat_vnode_t *victim = node_get(fs, nd, dst);
        if (!victim) { r = E_IO; goto out; }
        if (vn->is_dir && !victim->is_dir) { r = E_NOTDIR; goto out; }
        if (!vn->is_dir && victim->is_dir) { r = E_ISDIR; goto out; }
        if (victim->is_dir && (r = dir_is_empty(fs, victim)) <= 0) {
            if (r == 0) r = E_NOTEMPTY;
            goto out;
        }
        /* Point the existing entry at the moved object (one sector write:
         * the name never disappears), then drop the old entries. */
        uint8_t e[32];
        if (dir_get(fs, nd, dst->sfn_idx, e) != 0) { r = E_IO; goto out; }
        memcpy(e + 11, se + 11, 21);
        e[12] = dst->e[12];
        if (dir_put(fs, nd, dst->sfn_idx, e) < 0) { r = E_IO; goto out; }
        node_orphan(fs, victim);           /* frees the victim's own clusters */
        if ((r = dir_del(fs, od, src->first_idx, src->sfn_idx)) < 0) goto out;
        if ((r = node_move(fs, vn, nd, dst->sfn_idx, dst->name)) < 0) goto out;
    } else {
        int64_t at = dir_add(fs, nd, nname, se, nd == od ? src->sfn_idx : ~0u);
        if (at < 0) { r = (int)at; goto out; }
        if ((r = dir_del(fs, od, src->first_idx, src->sfn_idx)) < 0) goto out;
        if ((r = node_move(fs, vn, nd, (uint32_t)at, nname)) < 0) goto out;
    }
    /* A directory that changed parents: its ".." follows. */
    if (vn->is_dir && nd != od) {
        uint8_t e[32];
        if (dir_get(fs, vn, 1, e) == 0 && e[0] == '.' && e[1] == '.') {
            uint32_t up = nd->key == 0 ? 0 : nd->first;
            wr16(e + 26, up & 0xFFFF);
            wr16(e + 20, fs->type == 32 ? up >> 16 : 0);
            dir_put(fs, vn, 1, e);
        }
    }
    vn->ctime = vfat_now();
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

/* chmod: only the write bits mean something (the read-only attribute);
 * owner and the other bits come from the mount options. */
static int vfat_setattr(vfs_node_t *n, uint32_t mode, uint32_t uid, uint32_t gid) {
    (void)uid; (void)gid;
    vfat_vnode_t *vn = (vfat_vnode_t *)n;
    vfat_fs_t *fs = vn->fs;
    fs_lock(fs);
    int r = 0;
    if (!fs->ro && vn->key && !vn->is_dir) {
        uint8_t a = (mode & 0222) ? (uint8_t)(vn->attr & ~ATTR_RO) : (uint8_t)(vn->attr | ATTR_RO);
        if (a != vn->attr) {
            vn->attr = a;
            vn->ctime = vfat_now();
            r = node_store(vn);
        }
    }
    n->mask = mode_of(fs, vn);
    n->uid = fs->uid;
    n->gid = fs->gid;
    if (fs_unlock(fs) < 0 && !r) r = E_IO;
    return r;
}

static int vfat_settimes(vfs_node_t *n, uint32_t atime, uint32_t mtime) {
    vfat_vnode_t *vn = (vfat_vnode_t *)n;
    vfat_fs_t *fs = vn->fs;
    fs_lock(fs);
    int r = 0;
    if (fs->ro) r = E_ROFS;
    else {
        vn->atime = atime;
        vn->mtime = mtime;
        vn->ctime = vfat_now();
        r = node_store(vn);
    }
    if (fs_unlock(fs) < 0 && !r) r = E_IO;
    return r;
}

static void vfat_retain(vfs_node_t *n) {
    vfat_vnode_t *vn = (vfat_vnode_t *)n;
    preempt_disable();
    vn->refs++;
    vn->fs->open_refs++;
    preempt_enable();
}

static void vfat_close(vfs_node_t *n) {
    vfat_vnode_t *vn = (vfat_vnode_t *)n;
    vfat_fs_t *fs = vn->fs;
    int last;
    preempt_disable();
    if (vn->refs > 0) vn->refs--;
    if (fs->open_refs > 0) fs->open_refs--;
    last = vn->refs == 0 && vn->deleted && vn->first;
    preempt_enable();
    if (last) {
        /* The last descriptor of an unlinked file: now its clusters go. */
        fs_lock(fs);
        if (vn->refs == 0 && vn->first && !fs->ro) {
            chain_free(fs, vn->first);
            vn->first = 0;
        }
        fs_unlock(fs);
    }
}

static void node_hooks(vfat_vnode_t *vn) {
    vfs_node_t *n = &vn->vnode;
    n->setattr_fn  = vfat_setattr;
    n->settimes_fn = vfat_settimes;
    n->retain_fn   = vfat_retain;
    n->close_fn    = vfat_close;
    if (vn->is_dir) {
        n->finddir_fn = vfat_finddir;
        n->readdir_fn = vfat_readdir;
        n->create_fn  = vfat_create;
        n->unlink_fn  = vfat_unlink;
        n->rename_fn  = vfat_rename;
    } else {
        n->read_fn     = vfat_read;
        n->write_fn    = vfat_write;
        n->truncate_fn = vfat_truncate;
    }
}

/* ── Mount ──────────────────────────────────────────────────────────────── */

static uint32_t parse_num(const char *s, int base) {
    uint32_t v = 0;
    for (; *s; s++) {
        uint32_t d;
        if (*s >= '0' && *s <= '9') d = (uint32_t)(*s - '0');
        else break;
        if ((int)d >= base) break;
        v = v * (uint32_t)base + d;
    }
    return v;
}

void vfat_parse_opts(const char *data, vfat_opts_t *o) {
    o->uid = o->gid = 0;
    o->dmask = o->fmask = 022;
    if (!data) return;
    char opt[64];
    while (*data) {
        uint32_t n = 0;
        while (*data && *data != ',') {
            if (n < sizeof(opt) - 1) opt[n++] = *data;
            data++;
        }
        opt[n] = '\0';
        if (*data == ',') data++;
        if (strncmp(opt, "uid=", 4) == 0)        o->uid = parse_num(opt + 4, 10);
        else if (strncmp(opt, "gid=", 4) == 0)   o->gid = parse_num(opt + 4, 10);
        else if (strncmp(opt, "umask=", 6) == 0) o->dmask = o->fmask = parse_num(opt + 6, 8) & 0777;
        else if (strncmp(opt, "dmask=", 6) == 0) o->dmask = parse_num(opt + 6, 8) & 0777;
        else if (strncmp(opt, "fmask=", 6) == 0) o->fmask = parse_num(opt + 6, 8) & 0777;
    }
}

static void free_nodes(vfat_vnode_t *vn) {
    while (vn) {
        vfat_vnode_t *next = vn->hnext;
        kfree(vn);
        vn = next;
    }
}

static void vfat_free_fs(vfat_fs_t *fs) {
    for (int i = 0; i < VFAT_NODE_BUCKETS; i++) free_nodes(fs->nodes[i]);
    free_nodes(fs->dead);
    if (fs->root)    kfree(fs->root);           /* never in the hash */
    if (fs->bc)      kfree(fs->bc);
    if (fs->bc_data) kfree(fs->bc_data);
    if (fs->sbuf)    kfree(fs->sbuf);
    if (fs->zbuf)    kfree(fs->zbuf);
    kfree(fs);
}

int vfat_busy(void *p) {
    return ((vfat_fs_t *)p)->open_refs > 0;
}

void vfat_release(void *p) {
    vfat_fs_t *fs = (vfat_fs_t *)p;
    fs_lock(fs);
    if (!fs->ro) {
        fsinfo_store(fs);
        if (bc_flush(fs) == 0 && !fs->was_dirty) set_dirty(fs, 0);
    }
    fs->ro = 1;
    fs_unlock(fs);
    printk("[VFAT] %s: unmounted%s\n", fs->bp->name,
           fs->io_errors ? " (there were I/O errors)" : "");
    vfat_free_fs(fs);
}

int vfat_set_ro(void *p, int ro) {
    vfat_fs_t *fs = (vfat_fs_t *)p;
    int r = 0;
    fs_lock(fs);
    if (ro && !fs->ro) {
        fsinfo_store(fs);
        r = bc_flush(fs);
        if (r == 0 && !fs->was_dirty) r = set_dirty(fs, 0);
        fs->ro = 1;
    } else if (!ro && fs->ro) {
        fs->ro = 0;
        r = set_dirty(fs, 1);
    }
    fs_unlock(fs);
    return r < 0 ? E_IO : 0;
}

/* Free clusters by scanning the FAT, when FSInfo has no valid count (FAT12
 * and FAT16 have no FSInfo at all).  Done once; allocation keeps it. */
static int count_free(vfat_fs_t *fs) {
    uint32_t n = 0;
    for (uint32_t c = 2; c < fs->nclus + 2; c++) {
        uint32_t v;
        if (fat_rd(fs, c, &v) < 0) return -1;
        if (v == 0) n++;
    }
    fs->free_count = n;
    if (fs->fsinfo_sec && !fs->ro) fs->fsinfo_dirty = 1;
    return 0;
}

int vfat_statfs(void *p, vfs_statfs_t *out) {
    vfat_fs_t *fs = (vfat_fs_t *)p;
    fs_lock(fs);
    int r = 0;
    if (fs->free_count == 0xFFFFFFFFu && count_free(fs) < 0) r = E_IO;
    out->type    = 0x4d44;                         /* MSDOS_SUPER_MAGIC */
    out->bsize   = fs->cb;
    out->blocks  = fs->nclus;
    out->bfree   = fs->free_count == 0xFFFFFFFFu ? 0 : fs->free_count;
    out->files   = 0;
    out->ffree   = 0;
    out->namelen = 255;
    fs_unlock(fs);
    return r;
}

int vfat_mount_dev(blkpart_t *bp, int ro, const vfat_opts_t *o,
                   vfs_node_t **root_out, vfat_fs_t **fs_out) {
    uint8_t *bs = (uint8_t *)kmalloc(512);
    if (!bs) return E_NOMEM;
    vfat_fs_t *fs = NULL;
    int rc = E_INVAL;
    if (blkpart_read(bp, 0, 1, bs) < 0) { rc = E_IO; goto fail; }
    if (memcmp(bs + 3, "EXFAT   ", 8) == 0) {
        printk("[VFAT] %s: exFAT is not supported (only FAT12/16/32)\n", bp->name);
        goto fail;
    }
    uint32_t bps = rd16(bs + 11), spc = bs[13];
    if ((bs[0] != 0xEB && bs[0] != 0xE9) ||
        (bps != 512 && bps != 1024 && bps != 2048 && bps != 4096) ||
        spc == 0 || (spc & (spc - 1)) || bps * spc > 65536u ||
        rd16(bs + 14) == 0 || bs[16] == 0) {
        printk("[VFAT] %s: no FAT boot sector\n", bp->name);
        goto fail;
    }
    fs = (vfat_fs_t *)kmalloc(sizeof(vfat_fs_t));
    if (!fs) { rc = E_NOMEM; goto fail; }
    memset(fs, 0, sizeof(*fs));
    fs->bp = bp;
    fs->bps = bps;
    fs->s512 = bps / 512;
    fs->spc = spc;
    fs->cb = bps * spc;
    fs->reserved = rd16(bs + 14);
    fs->nfats = bs[16];
    fs->root_ents = rd16(bs + 17);
    uint32_t fatsz16 = rd16(bs + 22);
    fs->fatsz = fatsz16 ? fatsz16 : rd32(bs + 36);
    fs->total = rd16(bs + 19) ? rd16(bs + 19) : rd32(bs + 32);
    fs->root_secs = (fs->root_ents * 32 + bps - 1) / bps;
    fs->root_sec = fs->reserved + fs->nfats * fs->fatsz;
    fs->data_sec = fs->root_sec + fs->root_secs;
    if (!fs->fatsz || fs->data_sec >= fs->total) {
        printk("[VFAT] %s: inconsistent BPB\n", bp->name);
        goto fail;
    }
    fs->nclus = (fs->total - fs->data_sec) / spc;
    if (!fatsz16) fs->type = 32;
    else fs->type = fs->nclus < 4085 ? 12 : 16;
    if (fs->type == 32 && (fs->root_ents || rd16(bs + 42) != 0)) {
        printk("[VFAT] %s: unsupported FAT32 version\n", bp->name);
        goto fail;
    }
    if (fs->type != 32 && fs->root_ents == 0) goto fail;
    /* The FAT must have an entry for every cluster. */
    uint64_t cap = (uint64_t)fs->fatsz * bps * 8 / (uint32_t)fs->type;
    if (cap < 2) goto fail;
    if (fs->nclus > cap - 2) fs->nclus = (uint32_t)(cap - 2);
    if (fs->nclus == 0) goto fail;
    if ((uint64_t)fs->total * fs->s512 > bp->nsect) {
        printk("[VFAT] %s: filesystem is larger than its partition\n", bp->name);
        goto fail;
    }
    if (fs->type == 12) { fs->eoc = 0xFFF; fs->eoc_min = 0xFF8; fs->bad = 0xFF7; }
    else if (fs->type == 16) { fs->eoc = 0xFFFF; fs->eoc_min = 0xFFF8; fs->bad = 0xFFF7; }
    else { fs->eoc = 0x0FFFFFFF; fs->eoc_min = 0x0FFFFFF8; fs->bad = 0x0FFFFFF7; }
    fs->fat_start = fs->reserved;
    fs->mirror = 1;
    fs->free_count = 0xFFFFFFFFu;
    fs->next_free = 2;
    fs->dirty_off = fs->type == 32 ? 0x41 : 0x25;
    if (fs->type == 32) {
        uint32_t ext = rd16(bs + 40);
        if (ext & 0x80) {
            fs->mirror = 0;
            if ((ext & 0xF) >= fs->nfats) goto fail;
            fs->fat_start = fs->reserved + (ext & 0xF) * fs->fatsz;
        }
        fs->root_clus = rd32(bs + 44);
        fs->fsinfo_sec = rd16(bs + 48);
        if (!clus_ok(fs, fs->root_clus)) {
            printk("[VFAT] %s: bad root cluster\n", bp->name);
            goto fail;
        }
    }
    fs->uid = o->uid;
    fs->gid = o->gid;
    fs->dmask = o->dmask;
    fs->fmask = o->fmask;
    fs->ro = 1;                               /* until set up */

    /* Metadata cache */
    uint32_t slots = VFAT_CACHE_BYTES / bps, sets = 1;
    while (sets * 2 * VFAT_CACHE_WAYS <= slots) sets *= 2;
    fs->bc_sets = sets;
    slots = sets * VFAT_CACHE_WAYS;
    fs->bc      = (bc_meta_t *)kmalloc(slots * sizeof(bc_meta_t));
    fs->bc_data = (uint8_t *)kmalloc(slots * bps);
    fs->sbuf    = (uint8_t *)kmalloc(bps);
    fs->zbuf    = (uint8_t *)kmalloc(VFAT_ZBUF);
    if (!fs->bc || !fs->bc_data || !fs->sbuf || !fs->zbuf) { rc = E_NOMEM; goto fail; }
    for (uint32_t i = 0; i < slots; i++) { fs->bc[i].sec = ~0u; fs->bc[i].age = 0; fs->bc[i].dirty = 0; }
    memset(fs->zbuf, 0, VFAT_ZBUF);

    if (fs->fsinfo_sec) {
        uint8_t *fi;
        if (fs->fsinfo_sec >= fs->reserved || !(fi = bc_get(fs, fs->fsinfo_sec, 1, NULL)) ||
            rd32(fi) != 0x41615252u || rd32(fi + 484) != 0x61417272u) {
            fs->fsinfo_sec = 0;
        } else {
            fs->free_count = rd32(fi + 488);
            fs->next_free = rd32(fi + 492);
            if (fs->free_count > fs->nclus) fs->free_count = 0xFFFFFFFFu;
        }
    }
    if (!clus_ok(fs, fs->next_free)) fs->next_free = 2;

    uint8_t *b0 = bc_get(fs, 0, 1, NULL);
    if (!b0) { rc = E_IO; goto fail; }
    fs->was_dirty = b0[fs->dirty_off] & 1;

    vfat_vnode_t *r = (vfat_vnode_t *)kmalloc(sizeof(*r));
    if (!r) { rc = E_NOMEM; goto fail; }
    memset(r, 0, sizeof(*r));
    r->fs = fs;
    r->parent = r;
    r->is_dir = 1;
    r->attr = ATTR_DIR;
    r->fixed_root = fs->type != 32;
    r->first = fs->type == 32 ? fs->root_clus : 0;
    fs->root = r;
    vfs_node_t *n = &r->vnode;
    strncpy(n->name, "/", 255);
    n->flags = VFS_FLAG_DIR;
    n->mask = mode_of(fs, r);
    n->uid = fs->uid;
    n->gid = fs->gid;
    n->inode = 1;
    n->nlink = 1;
    n->dev = bp->rdev;
    n->size = fs->type == 32 ? fs->cb : fs->root_ents * 32;
    node_hooks(r);
    {
        /* The root's times: those of the volume label, if there is one. */
        uint8_t e[32];
        for (uint32_t i = 0; i < 64 && dir_get(fs, r, i, e) == 0 && e[0]; i++)
            if (e[0] != 0xE5 && (e[11] & 0x3F) != ATTR_LFN && (e[11] & ATTR_VOLID)) {
                r->mtime = fat_to_unix(rd16(e + 24), rd16(e + 22));
                break;
            }
        n->mtime = n->atime = n->ctime = r->mtime;
        r->c_clus = 0;
    }

    if (fs->was_dirty)
        printk("[VFAT] %s: warning: not cleanly unmounted; run fsck.vfat\n", bp->name);
    if (!ro) {
        fs->ro = 0;
        if (set_dirty(fs, 1) < 0) { rc = E_IO; goto fail; }
    }
    printk("[VFAT] %s: FAT%d, %u-byte clusters, %u clusters, mounted %s\n",
           bp->name, fs->type, (unsigned)fs->cb, (unsigned)fs->nclus,
           ro ? "read-only" : "read-write");
    kfree(bs);
    *root_out = n;
    *fs_out = fs;
    return 0;

fail:
    if (fs) vfat_free_fs(fs);
    kfree(bs);
    return rc;
}
