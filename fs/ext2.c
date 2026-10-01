#include "ext2.h"
#include "vfs.h"
#include "../drivers/blkdev.h"
#include "../mm/heap.h"
#include "../mm/pmm.h"
#include <kernel/config.h>
#include "../lib/string.h"
#include "../kernel/printk.h"
#include "../arch/i686/cpu/pit.h"
#include "../drivers/rtc.h"
#include "../proc/scheduler.h"
#include <stdint.h>
#include <stddef.h>
#include <kernel/kprof.h>

/* ── ext2 on-disk structures ──────────────────────────────────────────────── */

typedef struct {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count;
    uint32_t s_r_blocks_count;
    uint32_t s_free_blocks_count;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;   /* 1 if block_size=1024, else 0 */
    uint32_t s_log_block_size;     /* block_size = 1024 << s_log_block_size */
    uint32_t s_log_frag_size;
    uint32_t s_blocks_per_group;
    uint32_t s_frags_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;              /* 0xEF53 */
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    /* EXT2_DYNAMIC_REV */
    uint32_t s_first_ino;
    uint16_t s_inode_size;
    /* remaining fields not accessed — no pad needed */
} __attribute__((packed)) ext2_sb_t;

typedef struct {
    uint32_t bg_block_bitmap;
    uint32_t bg_inode_bitmap;
    uint32_t bg_inode_table;
    uint16_t bg_free_blocks_count;
    uint16_t bg_free_inodes_count;
    uint16_t bg_used_dirs_count;
    uint16_t bg_pad;
    uint8_t  bg_reserved[12];
} __attribute__((packed)) ext2_bgd_t;

typedef struct {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks;     /* 512-byte units */
    uint32_t i_flags;
    uint32_t i_osd1;
    uint32_t i_block[15];  /* 12 direct + 1 ind + 1 dind + 1 tind */
    uint32_t i_generation;
    uint32_t i_file_acl;
    uint32_t i_size_high;
    uint32_t i_faddr;
    uint8_t  i_osd2[12];
} __attribute__((packed)) ext2_inode_t;

typedef struct {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
    char     name[];       /* NOT null-terminated */
} __attribute__((packed)) ext2_dirent_t;

/* i_mode type bits */
#define EXT2_S_IFMT   0xF000
#define EXT2_S_IFREG  0x8000
#define EXT2_S_IFDIR  0x4000
#define EXT2_S_IFLNK  0xA000
#define EXT2_S_IFSOCK 0xC000
#define EXT2_FT_SOCK  6       /* dirent file_type of a socket */
#define EXT2_FT_SYMLINK 7     /* dirent file_type of a symlink */
/* A target shorter than i_block (60 bytes) is stored in i_block itself. */
#define EXT2_FAST_LINK_MAX (15 * 4)

/* ── Filesystem state ─────────────────────────────────────────────────────── */

typedef struct {
    uint32_t lba_offset;         /* start of ext2 partition on disk */
    uint32_t block_size;         /* bytes per block */
    uint32_t sectors_per_block;
    uint32_t inodes_per_group;
    uint32_t blocks_per_group;
    uint32_t inode_size;
    uint32_t first_data_block;   /* s_first_data_block */
    uint32_t inodes_count;
    uint32_t blocks_count;
    uint32_t first_ino;
} ext2_state_t;


/* ── Block cache ──────────────────────────────────────────────────────────────
 * Every block read that misses costs one ATA PIO transaction, and under KVM
 * every port access in that transaction is a VM exit into QEMU (~2 us).  The
 * cache this replaced held 8 blocks, i.e. less than one page fault's worth: a
 * 4 KiB fault on libxul.so reads the group descriptor, the inode block, up to
 * three indirect blocks and four data blocks, so the metadata was evicted by
 * the data of the very fault that needed it and re-read from disk every time.
 *
 * Set-associative (bucket = block number masked to the set count, LRU within the
 * set), so a large cache costs no more per lookup than the old linear scan over
 * 8 slots.  Sized at mount from a share of the free physical memory, so it is
 * large on the 2 GiB machine that runs the browser and small on a 128 MiB one.
 *
 * The block buffers come out of ONE slab allocation rather than one kmalloc per
 * slot.  kmalloc is a first-fit walk of a single free list (mm/heap.c), and the
 * filesystem and the page-fault path both allocate on every call, so putting
 * tens of thousands of 1 KiB blocks on that list makes every later allocation
 * walk past them: measured, a 32 MiB cache built from per-slot allocations
 * quadrupled page-fault time (5.6 s -> 20.5 s over a Firefox startup) and lost
 * far more than the extra cache hits gained.  One slab costs one list entry. */
/* Most blocks that a run of consecutive file blocks can carry in one ATA
 * transaction.  128 KiB is 256 sectors, past what one command can express;
 * 64 blocks keeps a transaction's interrupts-off window near 100 us. */
#define EXT2_READ_CLUSTER  64
/* Blocks one cache miss may fetch, past what the request needs, when they
 * follow it on disk and in the file and are not cached yet.  With PIO every
 * extra sector cost ~17 us, so fetching what nobody asked for was a loss (see
 * docs/perf/firefox-startup.md); with bus-master DMA a read's cost is nearly
 * all per command (~75 us), so the neighbours of a miss are nearly free and
 * the next miss next to them is not. */
#define EXT2_READAHEAD     32

#define EXT2_CACHE_WAYS     4

/* The cache is bounded by three separate things, and it has to consult all of
 * them because they run out independently.
 *
 * PHYSICAL MEMORY: a share of what is free at mount.  The cache is never handed
 * back, so this is what keeps a small machine honest -- and it is what governs
 * there: a 128 MiB guest lands on 8 MiB of buffers and a 512 MiB one on 32 MiB.
 *
 * HEAP ADDRESS SPACE: whatever is left after reserving a fixed margin for
 * everything else.  The kernel heap is a fixed 256 MiB span
 * (HEAP_START..HEAP_MAX in kernel/config.h) that every other kernel allocation
 * also comes out of and that nothing ever releases, so it -- not physical
 * memory -- is the resource that binds on the 2 GiB machine, and sizing against
 * free RAM alone never looked at it.
 *
 * The margin is absolute rather than a share, because what matters to the rest
 * of the kernel is how many bytes it can still get, not what fraction of the
 * window some other subsystem took.  Measured (the kprof heap line): everything
 * in the kernel other than this cache uses 5.5 MiB of heap to Firefox's first
 * paint, and the heap only grows, so that is a peak and not an average.  The
 * margin is 64 MiB, an order of magnitude above the measured demand.
 *
 * A CEILING: what the workload is worth.  Measured at six sizes with
 * `make smoke-firefox` (docs/perf/firefox-startup.md): 4, 8 and 16 MiB of
 * buffers are indistinguishable from each other at 41.3 s, 32 MiB gives 40.8 s,
 * 64 MiB 38.0 s and 128 MiB 35.4 s.  There is no knee below 128 MiB -- a
 * startup touches ~92 700 distinct 1 KiB blocks and cycles over them, so a
 * cache that holds part of the working set holds almost none of the value.
 * 128 MiB is where it fits: 94 370 blocks fetched against 92 640 distinct, so
 * 98 % of the fetches are first-time reads and there is nothing left to win.
 * A budget of 160 MiB is what yields exactly those 32768 sets (128 MiB of
 * buffers plus 2 MiB of slot descriptors); the next power of two would need
 * 273 MiB and buy nothing. */
#define EXT2_CACHE_MEM_SHARE  8                      /* one eighth of free RAM */
#define EXT2_CACHE_HEAP_KEEP  (64u * 1024u * 1024u)  /* heap left for the rest */
#define EXT2_CACHE_MIN_SETS   8
#define EXT2_CACHE_MAX_BYTES  (160u * 1024u * 1024u)
#define EXT2_CACHE_DEV_BYTES  (2u * 1024u * 1024u)   /* mount(2) instances */


typedef struct {
    uint32_t blk;
    uint32_t age;
    int valid;
    uint8_t *data;
} ext2_cache_entry_t;

#define EXT2_NODE_BUCKETS 256

#define EXT2_OPEN_MAX 128
typedef struct {
    uint32_t ino;        /* 0 = free slot */
    int      refs;
    int      orphan;     /* name is gone; release when refs reaches 0 */
} ext2_open_t;

/* Per-node private data */
typedef struct ext2_priv {
    uint32_t          ino;
    ext2_fs_t        *fs;        /* the instance the inode belongs to */
    struct ext2_priv *hnext;     /* node-cache hash chain, see ext2_make_node */
} ext2_priv_t;

/* One mounted filesystem.  /disk is one of these (fs->boot), and mount(2)
 * makes more; everything the driver keeps between calls lives here, so
 * instances share nothing but the code. */
struct ext2_fs {
    ext2_state_t        st;
    int                 boot;        /* the /disk instance: blk_read/blk_write */
    int                 disk;        /* drivers/blkdev.c disk index otherwise */
    uint32_t            nsect;       /* device/partition length, sectors */
    uint32_t            rdev;        /* st_dev of its nodes (0 for /disk) */
    char                name[16];    /* "sdb2", for messages */
    int                 ro;          /* read-only: nothing is written */
    int                 rw_ok;       /* features allow read-write */
    int                 was_clean;   /* s_state VALID when mounted */
    int                 open_refs;   /* descriptors, mappings: umount EBUSY */
    uint32_t            incompat, ro_compat, compat;
    uint8_t            *cache_slab;  /* one allocation for all buffers */
    uint32_t            cache_setmask; /* sets in use, minus one */
    /* The slot array is allocated at mount alongside the slab, so the cache
     * size is decided by the machine rather than reserved in the BSS. */
    ext2_cache_entry_t *cache;
    uint8_t            *ra_buf;      /* EXT2_READAHEAD blocks, for read-ahead */
    uint32_t            cache_slots;
    uint32_t            cache_age;
    int                 cache_ready;
    uint8_t            *seen;        /* bit per block: fetched at least once */
    uint32_t            seen_blocks;
    ext2_open_t         open[EXT2_OPEN_MAX];
    ext2_priv_t        *nodes[EXT2_NODE_BUCKETS];
};

static inline ext2_cache_entry_t *ext2_cache_set(ext2_fs_t *fs, uint32_t blk) {
    return &fs->cache[(blk & fs->cache_setmask) * EXT2_CACHE_WAYS];
}

/* The slot holding `blk`, or NULL. */
static ext2_cache_entry_t *ext2_cache_find(ext2_fs_t *fs, uint32_t blk) {
    ext2_cache_entry_t *set = ext2_cache_set(fs, blk);
    for (int i = 0; i < EXT2_CACHE_WAYS; i++)
        if (set[i].valid && set[i].blk == blk) return &set[i];
    return (ext2_cache_entry_t *)0;
}

/* A slot in blk's set to (re)use: its own slot, then a free one, then the LRU.
 * Returns NULL if the buffer could not be allocated (the cache then just
 * misses, which is correct, only slower). */
/* The slot in blk's set to (re)use: its own, then a free one, then the LRU.
 * Every slot already owns its slab buffer, so this cannot fail. */
static ext2_cache_entry_t *ext2_cache_claim(ext2_fs_t *fs, uint32_t blk) {
    ext2_cache_entry_t *set = ext2_cache_set(fs, blk);
    ext2_cache_entry_t *lru = &set[0];
    for (int i = 0; i < EXT2_CACHE_WAYS; i++) {
        if (set[i].valid && set[i].blk == blk) return &set[i];   /* already ours */
        if (!set[i].valid) return &set[i];
        if (set[i].age < lru->age) lru = &set[i];
    }
    return lru;
}


static vfs_node_t *ext2_finddir(vfs_node_t *dir, const char *name);
static int ext2_readdir(vfs_node_t *dir, uint32_t req_idx,
                         vfs_dirent_t *out);
static int ext2_create(vfs_node_t *dir, const char *name, uint32_t flags);
static int ext2_truncate(vfs_node_t *node, uint32_t new_size);
static int ext2_setattr(vfs_node_t *node, uint32_t mode, uint32_t uid,
                        uint32_t gid);
static int ext2_settimes(vfs_node_t *node, uint32_t atime, uint32_t mtime);
static int ext2_link(vfs_node_t *dir, const char *name, vfs_node_t *target);
static int ext2_unlink(vfs_node_t *dir, const char *name);
static int ext2_symlink(vfs_node_t *dir, const char *name, const char *target);
static int ext2_rename(vfs_node_t *old_dir, const char *old_name,
                       vfs_node_t *new_dir, const char *new_name);
static int ext2_free_block(ext2_fs_t *fs, uint32_t blk);

/* ── Block I/O ────────────────────────────────────────────────────────────── */

/* Sectors `lba`.. of the instance's disk (`lba` already includes lba_offset).
 * /disk keeps blk_read/blk_write, the primary master's fast path; a mount(2)
 * instance addresses its disk through the table, kept inside its partition.
 * A read-only instance never writes, whatever asks it to. */
static int ext2_dev_read(ext2_fs_t *fs, uint32_t lba, uint8_t count, void *buf) {
    if (fs->boot) return blk_read(lba, count, buf);
    uint32_t n = count ? count : 256u;
    if (lba < fs->st.lba_offset || lba - fs->st.lba_offset >= fs->nsect ||
        n > fs->nsect - (lba - fs->st.lba_offset))
        return -1;
    return blk_disk_read(fs->disk, lba, n, buf);
}

static int ext2_dev_write(ext2_fs_t *fs, uint32_t lba, uint8_t count, const void *buf) {
    if (fs->ro) return -1;
    if (fs->boot) return blk_write(lba, count, buf);
    uint32_t n = count ? count : 256u;
    if (lba < fs->st.lba_offset || lba - fs->st.lba_offset >= fs->nsect ||
        n > fs->nsect - (lba - fs->st.lba_offset))
        return -1;
    return blk_disk_write(fs->disk, lba, n, buf);
}

/* Wall-clock seconds: the RTC's boot time plus the uptime.  Files unpacked
 * from archives (apk, tar) keep their real mtimes, so new files must not be
 * stamped with the seconds since boot or they would look decades older. */
static uint32_t ext2_now(void) {
    return rtc_boot_epoch() + pit_ticks() / 100U;
}

/* Read-volume accounting.  The question this profiling round asks is whether
 * the disk time is spent fetching data the kernel has never seen or re-fetching
 * blocks it read and then evicted, and the bucket totals cannot tell those
 * apart.  One bit per block of the volume answers it exactly: every block that
 * comes off the platter is counted, and the ones whose bit was still clear are
 * counted again as distinct.  128 KiB for the 1 GiB image, one bit test per
 * block fetched. */

static void ext2_seen_init(ext2_fs_t *fs) {
    fs->seen = (uint8_t *)0;
    fs->seen_blocks = 0;
    if (!fs->st.blocks_count || fs->st.blocks_count > (1u << 24)) return;
    uint32_t bytes = (fs->st.blocks_count + 7u) / 8u;
    fs->seen = (uint8_t *)kmalloc(bytes);
    if (!fs->seen) return;
    memset(fs->seen, 0, bytes);
    fs->seen_blocks = fs->st.blocks_count;
}

/* Count `n` blocks starting at `blk` as fetched from disk. */
static void ext2_account_fetch(ext2_fs_t *fs, uint32_t blk, uint32_t n) {
    kprof_add(KPE_EXT2_DISK, n);
    if (!fs->seen) return;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t b = blk + i;
        if (b >= fs->seen_blocks) return;
        if (!(fs->seen[b >> 3] & (uint8_t)(1u << (b & 7u)))) {
            fs->seen[b >> 3] |= (uint8_t)(1u << (b & 7u));
            kprof_count(KPE_EXT2_DISTINCT);
        }
    }
}

static int ext2_raw_read_block(ext2_fs_t *fs, uint32_t blk, void *buf) {
    /* Block numbers come from inodes and indirect blocks on disk; one past the
     * filesystem would read, or below write, some other part of the drive (the
     * LBA product can even wrap back onto the partition table). */
    if (blk >= fs->st.blocks_count) return -1;
    ext2_account_fetch(fs, blk, 1);
    uint32_t lba = fs->st.lba_offset + blk * fs->st.sectors_per_block;
    /* Read sectors_per_block sectors; handle block sizes > 255*512 by looping */
    if (fs->st.sectors_per_block <= 255) {
        return ext2_dev_read(fs, lba, (uint8_t)fs->st.sectors_per_block, buf);
    }
    /* Large blocks: read in 128-sector (64 KiB) chunks */
    uint8_t *p = (uint8_t *)buf;
    uint32_t rem = fs->st.sectors_per_block;
    while (rem > 0) {
        uint8_t n = (rem > 128) ? 128 : (uint8_t)rem;
        if (ext2_dev_read(fs, lba, n, p) < 0) return -1;
        lba += n; p += n * 512; rem -= n;
    }
    return 0;
}

/* Read `n` physically consecutive blocks in ONE ATA transaction.  A transaction
 * costs ~12 port accesses before the first sector moves, so reading the four
 * 1 KiB blocks of a page fault separately paid that fixed cost four times.
 * `n` is bounded by the caller (EXT2_READ_CLUSTER), and the sector count of one
 * ATA command by 255. */
static int ext2_raw_read_blocks(ext2_fs_t *fs, uint32_t blk, uint32_t n, void *buf) {
    if (blk >= fs->st.blocks_count || n > fs->st.blocks_count - blk) return -1;
    ext2_account_fetch(fs, blk, n);
    uint32_t lba  = fs->st.lba_offset + blk * fs->st.sectors_per_block;
    uint32_t rem  = n * fs->st.sectors_per_block;
    uint8_t *p    = (uint8_t *)buf;
    while (rem > 0) {
        uint8_t k = (rem > 128) ? 128 : (uint8_t)rem;
        if (ext2_dev_read(fs, lba, k, p) < 0) return -1;
        lba += k; p += (uint32_t)k * 512; rem -= k;
    }
    return 0;
}

static int ext2_raw_write_block(ext2_fs_t *fs, uint32_t blk, const void *buf) {
    if (blk >= fs->st.blocks_count) return -1;   /* see ext2_raw_read_block */
    uint32_t lba = fs->st.lba_offset + blk * fs->st.sectors_per_block;
    if (fs->st.sectors_per_block <= 255) {
        return ext2_dev_write(fs, lba, (uint8_t)fs->st.sectors_per_block, buf);
    }

    const uint8_t *p = (const uint8_t *)buf;
    uint32_t rem = fs->st.sectors_per_block;
    while (rem > 0) {
        uint8_t n = (rem > 128) ? 128 : (uint8_t)rem;
        if (ext2_dev_write(fs, lba, n, p) < 0) return -1;
        lba += n;
        p += n * 512;
        rem -= n;
    }
    return 0;
}

static void ext2_cache_init(ext2_fs_t *fs) {
    fs->cache_ready = 0;
    fs->cache_age = 1;
    if (fs->cache_slab) { kfree(fs->cache_slab); fs->cache_slab = (uint8_t *)0; }
    if (fs->cache) { kfree(fs->cache); fs->cache = (ext2_cache_entry_t *)0; }
    fs->cache_slots = 0;

    /* The budget is the smallest of the three bounds above.  Sets are a power
     * of two so the bucket index stays a mask, and a set costs its buffers AND
     * its slot descriptors -- both come out of the same heap. */
    /* Frames * (PAGE_SIZE / SHARE) rather than (frames * PAGE_SIZE) / SHARE:
     * the kernel links no libgcc 64-bit division helpers, and the share divides
     * the page size exactly. */
    uint32_t budget = pmm_free_frames() * (PAGE_SIZE / EXT2_CACHE_MEM_SHARE);
    size_t   room   = heap_headroom();
    uint32_t heap   = room > EXT2_CACHE_HEAP_KEEP
                      ? (uint32_t)(room - EXT2_CACHE_HEAP_KEEP) : 0u;
    if (budget > heap)                 budget = heap;
    if (budget > EXT2_CACHE_MAX_BYTES) budget = EXT2_CACHE_MAX_BYTES;
    /* /disk is what the system runs from and gets the share above; a disk
     * mounted with mount(2) gets a small fixed cache, given back at umount. */
    if (!fs->boot && budget > EXT2_CACHE_DEV_BYTES) budget = EXT2_CACHE_DEV_BYTES;

    uint32_t per_set = EXT2_CACHE_WAYS *
                       (fs->st.block_size + (uint32_t)sizeof(ext2_cache_entry_t));
    uint32_t want    = budget / per_set;
    uint32_t sets    = EXT2_CACHE_MIN_SETS;
    while (sets * 2u <= want) sets *= 2u;

    /* One slab for every buffer, one array for every slot.  If they will not
     * fit, halve and retry rather than fall back to per-slot allocations, which
     * would flood the heap's free list (see the note above).
     *
     * kmalloc_try, not kmalloc: plain kmalloc cannot fail for a request this
     * size -- it grows the heap, and running out of heap address space or of
     * physical frames halts the machine instead of returning NULL.  This loop
     * used to be written against kmalloc and was therefore unreachable: it read
     * as a safety valve while the only two outcomes were success and a wedged
     * kernel. */
    while (sets >= EXT2_CACHE_MIN_SETS) {
        uint32_t slots = sets * EXT2_CACHE_WAYS;
        fs->cache = (ext2_cache_entry_t *)kmalloc_try(slots * sizeof(*fs->cache));
        if (fs->cache) {
            fs->cache_slab = (uint8_t *)kmalloc_try(slots * fs->st.block_size);
            if (fs->cache_slab) { fs->cache_slots = slots; break; }
            kfree(fs->cache);
            fs->cache = (ext2_cache_entry_t *)0;
        }
        printk("[EXT2] block cache: %u sets did not fit, halving\n",
               (unsigned)sets);
        sets /= 2u;
    }
    if (!fs->cache_slab) {
        printk("[EXT2] block cache unavailable (no memory)\n");
        fs->cache_ready = 0;
        return;
    }

    fs->cache_setmask = sets - 1u;
    for (uint32_t i = 0; i < fs->cache_slots; i++) {
        fs->cache[i].blk = 0;
        fs->cache[i].age = 0;
        fs->cache[i].valid = 0;
        fs->cache[i].data = fs->cache_slab + (size_t)i * fs->st.block_size;
    }
    printk("[EXT2] block cache %u KiB (%u sets x %u ways of %u B) + %u KiB slots"
           "; budget %u KiB, heap headroom now %u KiB\n",
           (unsigned)(sets * EXT2_CACHE_WAYS * fs->st.block_size / 1024u),
           (unsigned)sets, (unsigned)EXT2_CACHE_WAYS,
           (unsigned)fs->st.block_size,
           (unsigned)(fs->cache_slots * sizeof(*fs->cache) / 1024u),
           (unsigned)(budget / 1024u),
           (unsigned)(heap_headroom() / 1024u));
    fs->ra_buf = (uint8_t *)kmalloc((size_t)EXT2_READAHEAD * fs->st.block_size);
    fs->cache_ready = 1;
}

static void ext2_cache_insert(ext2_fs_t *fs, uint32_t blk, const void *buf);

/* Copy `blk` out of the cache if it is there.  1 on a hit, 0 on a miss. */
static int ext2_cache_lookup(ext2_fs_t *fs, uint32_t blk, void *buf) {
    int hit = 0;
    kprof_count(KPE_EXT2_BLK);
    preempt_disable();
    if (fs->cache_ready) {
        ext2_cache_entry_t *e = ext2_cache_find(fs, blk);
        if (e) {
            memcpy(buf, e->data, fs->st.block_size);
            e->age = fs->cache_age++;
            hit = 1;
        }
    }
    preempt_enable();
    kprof_count(hit ? KPE_EXT2_HIT : KPE_EXT2_MISS);
    return hit;
}

static int ext2_read_block(ext2_fs_t *fs, uint32_t blk, void *buf) {
    /* The block cache (fs->cache) is shared mutable state.  Without this guard a
     * directory lookup preempted mid-memcpy (copying a cached block into buf)
     * can have its source slot evicted+overwritten by another thread's ext2
     * read, so it resumes copying a DIFFERENT block's bytes → corrupt directory
     * data → spurious ENOENT on a file that exists (the intermittent
     * "/disk/shell not found" boot flake and flaky smoke-disk).  Serialize the
     * whole cache access (the raw ATA read already runs with IRQs off).
     *
     * CACHE INVARIANT: the guard must span the raw read AND the insert that
     * publishes it, never just the insert.  A writer running in between would
     * make the disk newer than what we are about to publish, and our insert
     * would then leave the cache permanently stale for that block.  The
     * clustered read in ext2_read_node holds the same invariant. */
    kprof_count(KPE_EXT2_BLK);
    preempt_disable();
    if (fs->cache_ready) {
        ext2_cache_entry_t *hit = ext2_cache_find(fs, blk);
        if (hit) {
            memcpy(buf, hit->data, fs->st.block_size);
            hit->age = fs->cache_age++;
            preempt_enable();
            kprof_count(KPE_EXT2_HIT);
            return 0;
        }
    }

    kprof_count(KPE_EXT2_MISS);
    if (ext2_raw_read_block(fs, blk, buf) < 0) {
        preempt_enable();
        return -1;
    }
    ext2_cache_insert(fs, blk, buf);
    preempt_enable();
    return 0;
}

/* ── Reading a few bytes out of a block ──────────────────────────────────────
 * A block group descriptor is 32 bytes, an inode 128 or 256, an indirect
 * pointer 4.  Reading any of them through ext2_read_block cost a kmalloc, a
 * whole-block memcpy out of the cache and a kfree, to then use a fraction of
 * the result: measured over a Firefox startup, 3.0 s in ext2_read_inode and
 * 3.0 s in the indirect-block walk, on a machine where the whole disk cost
 * 11 s.  These two helpers read the bytes straight out of the cache slot.
 *
 * The slot pointer NEVER escapes: ext2_cache_get returns it with the cache's
 * preempt guard held and the caller must copy what it needs and release
 * immediately.  Holding two slots at once would be a use-after-free -- the
 * second fetch can evict the first when both land in the same set. */
static ext2_cache_entry_t *ext2_cache_get(ext2_fs_t *fs, uint32_t blk) {
    kprof_count(KPE_EXT2_BLK);
    preempt_disable();                     /* released by ext2_cache_put */
    if (!fs->cache_ready) return (ext2_cache_entry_t *)0;

    ext2_cache_entry_t *e = ext2_cache_find(fs, blk);
    if (e) {
        e->age = fs->cache_age++;
        kprof_count(KPE_EXT2_HIT);
        return e;
    }

    kprof_count(KPE_EXT2_MISS);
    ext2_cache_entry_t *slot = ext2_cache_claim(fs, blk);
    if (!slot || !slot->data) return (ext2_cache_entry_t *)0;
    /* Invalidate before the transfer: a failed read must not leave the slot
     * claiming to hold `blk` with the evicted block's bytes still in it. */
    slot->valid = 0;
    if (ext2_raw_read_block(fs, blk, slot->data) < 0) return (ext2_cache_entry_t *)0;
    slot->blk = blk;
    slot->age = fs->cache_age++;
    slot->valid = 1;
    return slot;
}

static void ext2_cache_put(ext2_fs_t *fs) { (void)fs; preempt_enable(); }

/* Copy `len` bytes at offset `off` inside block `blk`.  0 on success. */
static int ext2_read_block_part(ext2_fs_t *fs, uint32_t blk, uint32_t off, uint32_t len,
                                void *out) {
    if (off + len > fs->st.block_size) return -1;
    ext2_cache_entry_t *e = ext2_cache_get(fs, blk);
    if (!e) {
        ext2_cache_put(fs);
        /* No cache (or it could not be fetched into one): fall back to the
         * copying path, which owns its own buffer. */
        uint8_t *tmp = (uint8_t *)kmalloc(fs->st.block_size);
        if (!tmp) return -1;
        if (ext2_read_block(fs, blk, tmp) < 0) { kfree(tmp); return -1; }
        memcpy(out, tmp + off, len);
        kfree(tmp);
        return 0;
    }
    memcpy(out, e->data + off, len);
    ext2_cache_put(fs);
    return 0;
}

/* The `idx`-th 32-bit pointer stored in indirect block `blk`, or 0. */
static uint32_t ext2_ind_word(ext2_fs_t *fs, uint32_t blk, uint32_t idx) {
    uint32_t v = 0;
    if (!blk) return 0;
    if (idx >= fs->st.block_size / 4) return 0;
    if (ext2_read_block_part(fs, blk, idx * 4u, 4u, &v) < 0) return 0;
    return v;
}

/* Publish `buf` as the cached contents of `blk`.  Caller holds preempt_disable
 * (the slot buffers are shared mutable state). */
static void ext2_cache_insert(ext2_fs_t *fs, uint32_t blk, const void *buf) {
    if (!fs->cache_ready) return;
    ext2_cache_entry_t *slot = ext2_cache_claim(fs, blk);
    if (!slot || !slot->data) return;
    memcpy(slot->data, buf, fs->st.block_size);
    slot->blk = blk;
    slot->age = fs->cache_age++;
    slot->valid = 1;
}

static int ext2_write_block(ext2_fs_t *fs, uint32_t blk, const void *buf) {
    if (ext2_raw_write_block(fs, blk, buf) < 0)
        return -1;

    preempt_disable();   /* same shared-cache hazard as ext2_read_block */
    ext2_cache_insert(fs, blk, buf);
    preempt_enable();
    return 0;
}

static int ext2_update_super_free_counts(ext2_fs_t *fs, int block_delta, int inode_delta) {
    uint8_t sb_buf[2048];
    if (ext2_dev_read(fs, fs->st.lba_offset + 2, 4, sb_buf) < 0)
        return -1;
    ext2_sb_t *sb = (ext2_sb_t *)sb_buf;
    if (sb->s_magic != 0xEF53)
        return -1;
    if (block_delta < 0)
        sb->s_free_blocks_count -= (uint32_t)(-block_delta);
    else
        sb->s_free_blocks_count += (uint32_t)block_delta;
    if (inode_delta < 0)
        sb->s_free_inodes_count -= (uint32_t)(-inode_delta);
    else
        sb->s_free_inodes_count += (uint32_t)inode_delta;
    /* The superblock's 1024 bytes only: with 1 KiB blocks the next two
     * sectors are the group descriptors, which may have changed meanwhile. */
    return ext2_dev_write(fs, fs->st.lba_offset + 2, 2, sb_buf);
}

/* ── Inode reading ────────────────────────────────────────────────────────── */

static int ext2_read_bgd(ext2_fs_t *fs, uint32_t grp, ext2_bgd_t *out) {
    /* The block-group-descriptor table can span MULTIPLE blocks: one block holds
     * only block_size/32 descriptors (32 for 1024-byte blocks).  Read the block
     * that actually CONTAINS group `grp`, not always the first — indexing the
     * first block by grp*32 overruns the buffer for grp >= per_block (read past
     * the kmalloc'd block → not-present fault). */
    uint32_t per_block = fs->st.block_size / sizeof(ext2_bgd_t);
    uint32_t bgd_blk   = fs->st.first_data_block + 1 + (per_block ? grp / per_block : 0);
    uint32_t idx       = per_block ? grp % per_block : grp;
    return ext2_read_block_part(fs, bgd_blk, idx * sizeof(ext2_bgd_t),
                                sizeof(ext2_bgd_t), out);
}

static int ext2_write_bgd(ext2_fs_t *fs, uint32_t grp, const ext2_bgd_t *in) {
    /* Same multi-block bgd-table handling as ext2_read_bgd. */
    uint32_t per_block = fs->st.block_size / sizeof(ext2_bgd_t);
    uint32_t bgd_blk   = fs->st.first_data_block + 1 + (per_block ? grp / per_block : 0);
    uint32_t idx       = per_block ? grp % per_block : grp;
    uint8_t *bgd_block = (uint8_t *)kmalloc(fs->st.block_size);
    if (!bgd_block) return -1;
    if (ext2_read_block(fs, bgd_blk, bgd_block) < 0) {
        kfree(bgd_block);
        return -1;
    }
    memcpy(bgd_block + idx * sizeof(ext2_bgd_t), in, sizeof(ext2_bgd_t));
    int r = ext2_write_block(fs, bgd_blk, bgd_block);
    kfree(bgd_block);
    return r;
}

static int ext2_read_inode(ext2_fs_t *fs, uint32_t ino, ext2_inode_t *out) {
    /* Inode numbers come from directory entries on disk. */
    if (ino == 0 || ino > fs->st.inodes_count) return -1;
    uint32_t grp = (ino - 1) / fs->st.inodes_per_group;
    uint32_t idx = (ino - 1) % fs->st.inodes_per_group;

    ext2_bgd_t bgd;
    if (ext2_read_bgd(fs, grp, &bgd) < 0) return -1;

    /* Read inode from inode table */
    uint32_t inodes_per_block = fs->st.block_size / fs->st.inode_size;
    uint32_t blk = bgd.bg_inode_table + idx / inodes_per_block;
    uint32_t off = (idx % inodes_per_block) * fs->st.inode_size;

    return ext2_read_block_part(fs, blk, off, sizeof(ext2_inode_t), out);
}

static int ext2_write_inode(ext2_fs_t *fs, uint32_t ino, const ext2_inode_t *in) {
    if (ino == 0 || ino > fs->st.inodes_count) return -1;
    uint32_t grp = (ino - 1) / fs->st.inodes_per_group;
    uint32_t idx = (ino - 1) % fs->st.inodes_per_group;

    ext2_bgd_t bgd;
    if (ext2_read_bgd(fs, grp, &bgd) < 0) return -1;

    uint32_t inodes_per_block = fs->st.block_size / fs->st.inode_size;
    uint32_t blk = bgd.bg_inode_table + idx / inodes_per_block;
    uint32_t off = (idx % inodes_per_block) * fs->st.inode_size;

    uint8_t *blk_buf = (uint8_t *)kmalloc(fs->st.block_size);
    if (!blk_buf) return -1;
    if (ext2_read_block(fs, blk, blk_buf) < 0) {
        kfree(blk_buf);
        return -1;
    }
    memcpy(blk_buf + off, in, sizeof(ext2_inode_t));
    int r = ext2_write_block(fs, blk, blk_buf);
    kfree(blk_buf);
    return r;
}

static int ext2_bitmap_test(uint8_t *bitmap, uint32_t bit) {
    return (bitmap[bit / 8] & (1U << (bit % 8))) != 0;
}

static void ext2_bitmap_set(uint8_t *bitmap, uint32_t bit) {
    bitmap[bit / 8] |= (uint8_t)(1U << (bit % 8));
}

static void ext2_bitmap_clear(uint8_t *bitmap, uint32_t bit) {
    bitmap[bit / 8] &= (uint8_t)~(1U << (bit % 8));
}

static uint32_t ext2_group_count(ext2_fs_t *fs) {
    return (fs->st.blocks_count + fs->st.blocks_per_group - 1) /
           fs->st.blocks_per_group;
}

static uint32_t ext2_alloc_inode(ext2_fs_t *fs) {
    uint32_t groups = ext2_group_count(fs);
    uint8_t *bitmap = (uint8_t *)kmalloc(fs->st.block_size);
    if (!bitmap) return 0;

    for (uint32_t grp = 0; grp < groups; grp++) {
        ext2_bgd_t bgd;
        if (ext2_read_bgd(fs, grp, &bgd) < 0) continue;
        if (ext2_read_block(fs, bgd.bg_inode_bitmap, bitmap) < 0) continue;

        for (uint32_t i = 0; i < fs->st.inodes_per_group; i++) {
            uint32_t ino = grp * fs->st.inodes_per_group + i + 1;
            if (ino < fs->st.first_ino || ino > fs->st.inodes_count) continue;
            if (ext2_bitmap_test(bitmap, i)) continue;

            ext2_bitmap_set(bitmap, i);
            if (ext2_write_block(fs, bgd.bg_inode_bitmap, bitmap) < 0) {
                kfree(bitmap);
                return 0;
            }
            if (bgd.bg_free_inodes_count) bgd.bg_free_inodes_count--;
            ext2_write_bgd(fs, grp, &bgd);
            ext2_update_super_free_counts(fs, 0, -1);
            kfree(bitmap);
            return ino;
        }
    }

    kfree(bitmap);
    return 0;
}

static uint32_t ext2_alloc_block(ext2_fs_t *fs) {
    uint32_t groups = ext2_group_count(fs);
    uint8_t *bitmap = (uint8_t *)kmalloc(fs->st.block_size);
    uint8_t *zero = (uint8_t *)kmalloc(fs->st.block_size);
    if (!bitmap || !zero) {
        if (bitmap) kfree(bitmap);
        if (zero) kfree(zero);
        return 0;
    }
    memset(zero, 0, fs->st.block_size);

    for (uint32_t grp = 0; grp < groups; grp++) {
        ext2_bgd_t bgd;
        if (ext2_read_bgd(fs, grp, &bgd) < 0) continue;
        if (ext2_read_block(fs, bgd.bg_block_bitmap, bitmap) < 0) continue;

        for (uint32_t i = 0; i < fs->st.blocks_per_group; i++) {
            uint32_t blk = fs->st.first_data_block +
                           grp * fs->st.blocks_per_group + i;
            if (blk < fs->st.first_data_block || blk >= fs->st.blocks_count)
                continue;
            if (ext2_bitmap_test(bitmap, i)) continue;

            ext2_bitmap_set(bitmap, i);
            if (ext2_write_block(fs, bgd.bg_block_bitmap, bitmap) < 0) {
                kfree(bitmap);
                kfree(zero);
                return 0;
            }
            if (bgd.bg_free_blocks_count) bgd.bg_free_blocks_count--;
            ext2_write_bgd(fs, grp, &bgd);
            ext2_update_super_free_counts(fs, -1, 0);
            if (ext2_write_block(fs, blk, zero) < 0) {
                kfree(bitmap);
                kfree(zero);
                ext2_free_block(fs, blk);
                return 0;
            }
            kfree(bitmap);
            kfree(zero);
            return blk;
        }
    }

    kfree(bitmap);
    kfree(zero);
    return 0;
}

static int ext2_free_inode(ext2_fs_t *fs, uint32_t ino) {
    if (ino < fs->st.first_ino || ino > fs->st.inodes_count) return -1;
    uint32_t grp = (ino - 1) / fs->st.inodes_per_group;
    uint32_t idx = (ino - 1) % fs->st.inodes_per_group;

    ext2_bgd_t bgd;
    if (ext2_read_bgd(fs, grp, &bgd) < 0) return -1;

    uint8_t *bitmap = (uint8_t *)kmalloc(fs->st.block_size);
    if (!bitmap) return -1;
    if (ext2_read_block(fs, bgd.bg_inode_bitmap, bitmap) < 0) {
        kfree(bitmap);
        return -1;
    }
    if (ext2_bitmap_test(bitmap, idx)) {
        ext2_bitmap_clear(bitmap, idx);
        if (ext2_write_block(fs, bgd.bg_inode_bitmap, bitmap) < 0) {
            kfree(bitmap);
            return -1;
        }
        bgd.bg_free_inodes_count++;
        ext2_write_bgd(fs, grp, &bgd);
        ext2_update_super_free_counts(fs, 0, 1);
    }
    kfree(bitmap);
    return 0;
}

static int ext2_free_block(ext2_fs_t *fs, uint32_t blk) {
    if (blk < fs->st.first_data_block || blk >= fs->st.blocks_count)
        return -1;
    uint32_t grp = (blk - fs->st.first_data_block) / fs->st.blocks_per_group;
    uint32_t idx = (blk - fs->st.first_data_block) % fs->st.blocks_per_group;

    ext2_bgd_t bgd;
    if (ext2_read_bgd(fs, grp, &bgd) < 0) return -1;

    uint8_t *bitmap = (uint8_t *)kmalloc(fs->st.block_size);
    if (!bitmap) return -1;
    if (ext2_read_block(fs, bgd.bg_block_bitmap, bitmap) < 0) {
        kfree(bitmap);
        return -1;
    }
    if (ext2_bitmap_test(bitmap, idx)) {
        ext2_bitmap_clear(bitmap, idx);
        if (ext2_write_block(fs, bgd.bg_block_bitmap, bitmap) < 0) {
            kfree(bitmap);
            return -1;
        }
        bgd.bg_free_blocks_count++;
        ext2_write_bgd(fs, grp, &bgd);
        ext2_update_super_free_counts(fs, 1, 0);
    }
    kfree(bitmap);
    return 0;
}

/* Live free-space numbers for statfs().  The superblock counters are updated by
 * ext2_alloc_block/ext2_free_block (via ext2_update_super_free_counts), so what
 * df reports is what dumpe2fs reports. */
int ext2_statfs_fs(ext2_fs_t *fs, uint32_t *block_size, uint32_t *blocks,
                   uint32_t *bfree, uint32_t *inodes, uint32_t *ifree) {
    if (!fs) return -1;
    uint8_t sb_buf[2048];
    if (ext2_dev_read(fs, fs->st.lba_offset + 2, 4, sb_buf) < 0) return -1;
    ext2_sb_t *sb = (ext2_sb_t *)sb_buf;
    if (sb->s_magic != 0xEF53) return -1;
    if (block_size) *block_size = fs->st.block_size;
    if (blocks)     *blocks     = sb->s_blocks_count;
    if (bfree)      *bfree      = sb->s_free_blocks_count;
    if (inodes)     *inodes     = sb->s_inodes_count;
    if (ifree)      *ifree      = sb->s_free_inodes_count;
    return 0;
}

/* Linux ext2_inode_is_fast_symlink(): a symlink whose only block, if any, is
 * its extended-attribute block keeps the target in i_block, which then holds
 * text rather than block numbers. */
static int ext2_is_fast_symlink(ext2_fs_t *fs, const ext2_inode_t *ino) {
    if ((ino->i_mode & EXT2_S_IFMT) != EXT2_S_IFLNK) return 0;
    uint32_t ea = ino->i_file_acl ? fs->st.sectors_per_block : 0;
    return ino->i_blocks == ea;
}

/* ── Resolve an indirect block pointer ───────────────────────────────────── */

/* Returns the physical block number for file-block index `idx`.
 *
 * Each indirect level is one 4-byte read out of the block cache
 * (ext2_ind_word), so resolving a block in the doubly-indirect range costs two
 * cache lookups and eight bytes of copying.  The previous version kept a
 * per-call cache of whole indirect blocks because reaching one cost a kmalloc
 * and a 1 KiB memcpy; now that reaching one is a lookup, the cache saved less
 * than it cost to fill, and the block cache is the only cache in the path. */
static uint32_t ext2_file_blk(ext2_fs_t *fs, ext2_inode_t *ino, uint32_t idx) {
    uint32_t ppb = fs->st.block_size / 4;

    if (idx < 12) return ino->i_block[idx];
    idx -= 12;

    if (idx < ppb)                                    /* singly */
        return ext2_ind_word(fs, ino->i_block[12], idx);
    idx -= ppb;

    if (idx < ppb * ppb) {                            /* doubly */
        uint32_t ind = ext2_ind_word(fs, ino->i_block[13], idx / ppb);
        return ext2_ind_word(fs, ind, idx % ppb);
    }
    idx -= ppb * ppb;

    if (idx < ppb * ppb * ppb) {                      /* triply */
        uint32_t per2 = ppb * ppb;
        uint32_t dind = ext2_ind_word(fs, ino->i_block[14], idx / per2);
        uint32_t ind  = ext2_ind_word(fs, dind, (idx % per2) / ppb);
        return ext2_ind_word(fs, ind, idx % ppb);
    }
    return 0;
}

static uint32_t ext2_file_blk_alloc(ext2_fs_t *fs, ext2_inode_t *ino, uint32_t idx) {
    uint32_t ptrs_per_blk = fs->st.block_size / 4;
    if (idx < 12) {
        if (!ino->i_block[idx]) {
            uint32_t blk = ext2_alloc_block(fs);
            if (!blk) return 0;
            ino->i_block[idx] = blk;
            ino->i_blocks += fs->st.sectors_per_block;
        }
        return ino->i_block[idx];
    }

    idx -= 12;

    /* ── Doubly indirect range: idx >= ptrs_per_blk ─────────────────────
     * Layout: i_block[13] → table of pointer-blocks → data blocks.
     * Allocate each missing level on the way down. */
    if (idx >= ptrs_per_blk) {
        uint32_t d_idx, i_idx, ind_blk, blk;
        uint32_t *tbl;

        idx -= ptrs_per_blk;
        if (idx >= ptrs_per_blk * ptrs_per_blk) return 0;  /* > ~256MB: no */
        d_idx = idx / ptrs_per_blk;
        i_idx = idx % ptrs_per_blk;

        tbl = (uint32_t *)kmalloc(fs->st.block_size);
        if (!tbl) return 0;

        /* level 0: the double-indirect block itself */
        if (!ino->i_block[13]) {
            uint32_t nb = ext2_alloc_block(fs);
            if (!nb) { kfree(tbl); return 0; }
            memset(tbl, 0, fs->st.block_size);
            if (ext2_write_block(fs, nb, tbl) < 0) {
                ext2_free_block(fs, nb);
                kfree(tbl);
                return 0;
            }
            ino->i_block[13] = nb;
            ino->i_blocks += fs->st.sectors_per_block;
        }
        if (ext2_read_block(fs, ino->i_block[13], tbl) < 0) {
            kfree(tbl);
            return 0;
        }

        /* level 1: the indirect block for this slice */
        ind_blk = tbl[d_idx];
        if (!ind_blk) {
            uint32_t *zero = (uint32_t *)kmalloc(fs->st.block_size);
            if (!zero) { kfree(tbl); return 0; }
            ind_blk = ext2_alloc_block(fs);
            if (!ind_blk) { kfree(zero); kfree(tbl); return 0; }
            memset(zero, 0, fs->st.block_size);
            if (ext2_write_block(fs, ind_blk, zero) < 0) {
                ext2_free_block(fs, ind_blk);
                kfree(zero);
                kfree(tbl);
                return 0;
            }
            kfree(zero);
            tbl[d_idx] = ind_blk;
            ino->i_blocks += fs->st.sectors_per_block;
            if (ext2_write_block(fs, ino->i_block[13], tbl) < 0) {
                kfree(tbl);
                return 0;
            }
        }

        /* level 2: the data block */
        if (ext2_read_block(fs, ind_blk, tbl) < 0) {
            kfree(tbl);
            return 0;
        }
        blk = tbl[i_idx];
        if (!blk) {
            blk = ext2_alloc_block(fs);
            if (!blk) { kfree(tbl); return 0; }
            tbl[i_idx] = blk;
            ino->i_blocks += fs->st.sectors_per_block;
            if (ext2_write_block(fs, ind_blk, tbl) < 0) {
                kfree(tbl);
                return 0;
            }
        }
        kfree(tbl);
        return blk;
    }

    uint32_t *ind = (uint32_t *)kmalloc(fs->st.block_size);
    if (!ind) return 0;

    int created_ind = 0;
    if (!ino->i_block[12]) {
        uint32_t ind_blk = ext2_alloc_block(fs);
        if (!ind_blk) {
            kfree(ind);
            return 0;
        }
        ino->i_block[12] = ind_blk;
        ino->i_blocks += fs->st.sectors_per_block;
        created_ind = 1;
        memset(ind, 0, fs->st.block_size);
        if (ext2_write_block(fs, ino->i_block[12], ind) < 0) {
            ext2_free_block(fs, ino->i_block[12]);
            ino->i_block[12] = 0;
            if (ino->i_blocks >= fs->st.sectors_per_block)
                ino->i_blocks -= fs->st.sectors_per_block;
            kfree(ind);
            return 0;
        }
    } else if (ext2_read_block(fs, ino->i_block[12], ind) < 0) {
        kfree(ind);
        return 0;
    }

    if (!ind[idx]) {
        uint32_t blk = ext2_alloc_block(fs);
        if (!blk) {
            if (created_ind) {
                ext2_free_block(fs, ino->i_block[12]);
                ino->i_block[12] = 0;
                if (ino->i_blocks >= fs->st.sectors_per_block)
                    ino->i_blocks -= fs->st.sectors_per_block;
            }
            kfree(ind);
            return 0;
        }
        ind[idx] = blk;
        ino->i_blocks += fs->st.sectors_per_block;
        if (ext2_write_block(fs, ino->i_block[12], ind) < 0) {
            ext2_free_block(fs, blk);
            if (ino->i_blocks >= fs->st.sectors_per_block)
                ino->i_blocks -= fs->st.sectors_per_block;
            ind[idx] = 0;
            if (created_ind) {
                ext2_free_block(fs, ino->i_block[12]);
                ino->i_block[12] = 0;
                if (ino->i_blocks >= fs->st.sectors_per_block)
                    ino->i_blocks -= fs->st.sectors_per_block;
            }
            kfree(ind);
            return 0;
        }
    }
    uint32_t blk = ind[idx];
    kfree(ind);
    return blk;
}

/* ── VFS read_fn for ext2 file nodes ─────────────────────────────────────── */

static uint32_t ext2_read_node(vfs_node_t *node, uint32_t offset,
                                uint32_t size, uint8_t *buf) {
    if (!node->private) return 0;
    ext2_priv_t *priv = (ext2_priv_t *)node->private;
    ext2_fs_t *fs = priv->fs;

    ext2_inode_t inode;
    uint64_t kp_i = kprof_probe_begin();
    int inode_err = ext2_read_inode(fs, priv->ino, &inode) < 0;
    kprof_probe_end(KPP_E2_INODE, kp_i);
    if (inode_err) return 0;

    if (offset >= inode.i_size) return 0;
    if (size > inode.i_size - offset) size = inode.i_size - offset;

    if (ext2_is_fast_symlink(fs, &inode)) {
        if (inode.i_size > EXT2_FAST_LINK_MAX) return 0;
        memcpy(buf, (const uint8_t *)inode.i_block + offset, size);
        return size;
    }

    uint32_t blk_size = fs->st.block_size;
    uint32_t done = 0;
    /* The bounce buffer is only needed for a partial block; a page fault, which
     * is nearly every read here, is whole-block aligned and never touches it.
     * Allocating it unconditionally cost a kmalloc+kfree pair per read. */
    uint8_t *blk_buf = (uint8_t *)0;

    while (done < size) {
        uint32_t file_off   = offset + done;
        uint32_t blk_idx    = file_off / blk_size;
        uint32_t blk_off    = file_off % blk_size;
        uint32_t to_copy    = blk_size - blk_off;
        if (to_copy > size - done) to_copy = size - done;

        uint64_t kp_b = kprof_probe_begin();
        uint32_t blk_num = ext2_file_blk(fs, &inode, blk_idx);
        kprof_probe_end(KPP_E2_BMAP, kp_b);
        if (blk_num == 0) {
            memset(buf + done, 0, to_copy);
            done += to_copy;
            continue;
        }

        /* Whole-block aligned copy: read straight into the destination,
         * skipping the bounce buffer (the common case for mmap/page reads).
         * On a cache miss, extend the read over as many physically consecutive
         * blocks as the request still needs, so a 4 KiB page fault on a
         * contiguous file costs one ATA transaction instead of four. */
        if (blk_off == 0 && to_copy == blk_size) {
            uint64_t kp_c = kprof_probe_begin();
            int miss = !ext2_cache_lookup(fs, blk_num, buf + done);
            kprof_probe_end(KPP_E2_COPY, kp_c);
            if (miss) {
                /* Cluster only into a KERNEL destination.  ata_read transfers
                 * with interrupts disabled straight into the caller's buffer,
                 * and sys_read hands us the user pointer unbounced: a longer
                 * transaction into user memory would widen the window in which
                 * a demand-paged destination page faults in the middle of a
                 * live PIO transfer.  The mmap page-fill path, which is where
                 * nearly all of the reads are, fills a kernel temp mapping. */
                uint32_t run = 1;
                while ((uintptr_t)(buf + done) >= KERNEL_VMA &&
                       run < EXT2_READ_CLUSTER &&
                       done + (run + 1) * blk_size <= size &&
                       ext2_file_blk(fs, &inode, blk_idx + run) == blk_num + run)
                    run++;
                /* Read-ahead: the same command continues over the following
                 * blocks of the file while they are consecutive on disk and
                 * not cached, into fs->ra_buf, and they go to the cache only. */
                uint32_t total = run;
                uint32_t nblk = (inode.i_size + blk_size - 1) / blk_size;
                if ((uintptr_t)(buf + done) >= KERNEL_VMA && fs->ra_buf &&
                    run < EXT2_READAHEAD) {
                    while (total < EXT2_READAHEAD && blk_idx + total < nblk &&
                           ext2_file_blk(fs, &inode, blk_idx + total) == blk_num + total &&
                           !ext2_cache_find(fs, blk_num + total))
                        total++;
                }
                if (total > run) {
                    int failed;
                    preempt_disable();       /* same invariant as below */
                    failed = ext2_raw_read_blocks(fs, blk_num, total, fs->ra_buf) < 0;
                    if (!failed) {
                        memcpy(buf + done, fs->ra_buf, run * blk_size);
                        for (uint32_t r = 0; r < total; r++)
                            ext2_cache_insert(fs, blk_num + r, fs->ra_buf + r * blk_size);
                    }
                    preempt_enable();
                    if (failed) break;
                    kprof_add(KPE_EXT2_BLK, run);
                    kprof_add(KPE_EXT2_MISS, run);
                    kprof_add(KPE_EXT2_RA, total - run);
                    done += run * blk_size;
                    continue;
                }
                if (run > 1) {
                    /* CACHE INVARIANT: the raw read and the inserts that
                     * publish it MUST be one non-preemptible section, exactly
                     * as in ext2_read_block.  Otherwise a writer that runs in
                     * between puts fresh contents in the cache and on the disk,
                     * and this thread then republishes the copy it read BEFORE
                     * that write - leaving the cache permanently disagreeing
                     * with the disk for those blocks.  That is silent data
                     * corruption, not a stale-performance problem.
                     *
                     * Holding the guard across the transfer is nearly free
                     * here: ata_read already runs the whole transfer with
                     * interrupts disabled (one cluster is at most 128 sectors,
                     * a single ATA command), so preemption was impossible for
                     * the expensive part anyway.  The guard only adds the gaps
                     * around it - which are precisely the gaps the race needs.
                     * That is why this is a wider guard rather than a
                     * generation stamp on each block: same outcome, no new
                     * state, and it matches the single-block path. */
                    int failed;
                    preempt_disable();
                    failed = ext2_raw_read_blocks(fs, blk_num, run, buf + done) < 0;
                    if (!failed)
                        for (uint32_t r = 0; r < run; r++)
                            ext2_cache_insert(fs, blk_num + r,
                                              buf + done + r * blk_size);
                    preempt_enable();
                    if (failed) break;
                    kprof_add(KPE_EXT2_BLK, run);
                    kprof_add(KPE_EXT2_MISS, run);
                    done += run * blk_size;
                    continue;
                }
                if (ext2_read_block(fs, blk_num, buf + done) < 0) break;
            }
        } else {
            if (!blk_buf) {
                uint64_t kp_a = kprof_probe_begin();
                blk_buf = (uint8_t *)kmalloc(blk_size);
                kprof_probe_end(KPP_E2_ALLOC, kp_a);
                if (!blk_buf) break;
            }
            if (ext2_read_block(fs, blk_num, blk_buf) < 0) break;
            memcpy(buf + done, blk_buf + blk_off, to_copy);
        }
        done += to_copy;
    }
    if (blk_buf) {
        uint64_t kp_f = kprof_probe_begin();
        kfree(blk_buf);
        kprof_probe_end(KPP_E2_ALLOC, kp_f);
    }
    /* noatime: do NOT write the inode back on read.  Firefox's startup is
     * enormously read-heavy (libxul + hundreds of chrome/config files, many
     * small reads); an atime write-back per read turned every read into a
     * read+write, and that write volume against the ATA PIO disk under SMP load
     * was corrupting on-disk directory/inode structure → spurious ENOENT on
     * firefox-bin that poisoned every watchdog attempt after the first few
     * (observed 2026-07-04: 17/20 attempts died at exec once the corruption set
     * in).  atime is meaningless for this OS; Linux's own perf guidance is
     * noatime.  Keep the in-core node atime fresh without touching disk. */
    if (done) node->atime = ext2_now();
    return done;
}

/* ── VFS write_fn for ext2 file nodes ────────────────────────────────────── */

static uint32_t ext2_write_node(vfs_node_t *node, uint32_t offset,
                                 uint32_t size, const uint8_t *buf) {
    if (!node->private) return 0;
    ext2_priv_t *priv = (ext2_priv_t *)node->private;
    ext2_fs_t *fs = priv->fs;

    ext2_inode_t inode;
    if (ext2_read_inode(fs, priv->ino, &inode) < 0) return 0;
    if ((inode.i_mode & EXT2_S_IFMT) != EXT2_S_IFREG) return 0;

    uint32_t end = offset + size;
    if (end < offset) return 0;

    uint32_t blk_size = fs->st.block_size;
    uint32_t done = 0;
    uint8_t *blk_buf = (uint8_t *)kmalloc(blk_size);
    /* Out of kernel memory, not out of disk: say so.  A plain 0 here means
     * "wrote nothing" to a caller that cannot distinguish it from progress,
     * and a libc write loop spins on it.  The full-disk case below is
     * different - it breaks after a partial write and returns a short count,
     * which write(2) callers already handle. */
    if (!blk_buf) return VFS_WRITE_ENOMEM;

    while (done < size) {
        uint32_t file_off = offset + done;
        uint32_t blk_idx = file_off / blk_size;
        uint32_t blk_off = file_off % blk_size;
        uint32_t to_copy = blk_size - blk_off;
        if (to_copy > size - done) to_copy = size - done;

        uint32_t blk_num = ext2_file_blk_alloc(fs, &inode, blk_idx);
        if (blk_num == 0) break;

        if (ext2_read_block(fs, blk_num, blk_buf) < 0) break;
        memcpy(blk_buf + blk_off, buf + done, to_copy);
        if (ext2_write_block(fs, blk_num, blk_buf) < 0) break;
        done += to_copy;
    }

    kfree(blk_buf);
    if (done && offset + done > inode.i_size) {
        inode.i_size = offset + done;
        node->size = inode.i_size;
    }
    if (done) {
        inode.i_mtime = ext2_now();
        inode.i_ctime = inode.i_mtime;
        node->mtime = inode.i_mtime;
        node->ctime = inode.i_ctime;
        ext2_write_inode(fs, priv->ino, &inode);
    }
    return done;
}

static uint16_t ext2_dir_rec_len(uint8_t name_len) {
    return (uint16_t)((8U + name_len + 3U) & ~3U);
}

/* A directory entry at `off` in a `bs`-byte block is safe to use: its header
 * and name lie inside the block and rec_len steps to a sane next entry.  The
 * walkers used to trust rec_len and name_len as read from disk, so a corrupt
 * entry sent memcmp/memcpy, or the entry ext2_add_dirent writes, past the end
 * of the block buffer. */
static int ext2_de_ok(const uint8_t *blk, uint32_t off, uint32_t bs) {
    if (bs < 8 || off > bs - 8) return 0;
    const ext2_dirent_t *de = (const ext2_dirent_t *)(blk + off);
    if (de->rec_len < 8 || (de->rec_len & 3) || de->rec_len > bs - off) return 0;
    if (8u + de->name_len > de->rec_len) return 0;
    return 1;
}

/* A directory with an htree index (dir_index: mke2fs, e2fsck -D and Linux
 * build them) stays a valid linear directory -- the index hides in entries
 * with inode 0 -- but an entry added or removed here is not in the index, so
 * the index has to go: clear its flag, as Linux's ext2 driver does, and the
 * directory is read linearly from then on.  The caller writes the inode. */
#define EXT2_INDEX_FL 0x00001000u
static void ext2_dir_unindex(ext2_inode_t *dir_inode) {
    dir_inode->i_flags &= ~EXT2_INDEX_FL;
}

static int ext2_add_dirent(ext2_fs_t *fs, uint32_t dir_ino, ext2_inode_t *dir_inode,
                            uint32_t child_ino, const char *name,
                            uint8_t file_type) {
    uint32_t name_len = strlen(name);
    if (name_len == 0 || name_len > 255) return -1;
    ext2_dir_unindex(dir_inode);

    uint16_t need = ext2_dir_rec_len((uint8_t)name_len);
    uint8_t *blk_buf = (uint8_t *)kmalloc(fs->st.block_size);
    if (!blk_buf) return -1;

    uint32_t ptrs_per_blk = fs->st.block_size / 4;
    for (uint32_t blk_idx = 0; blk_idx < 12 + ptrs_per_blk; blk_idx++) {
        uint32_t blk_num = ext2_file_blk(fs, dir_inode, blk_idx);
        if (!blk_num) {
            blk_num = ext2_file_blk_alloc(fs, dir_inode, blk_idx);
            if (!blk_num) break;
            dir_inode->i_size += fs->st.block_size;
            memset(blk_buf, 0, fs->st.block_size);
            ext2_dirent_t *de = (ext2_dirent_t *)blk_buf;
            de->inode = child_ino;
            de->rec_len = (uint16_t)fs->st.block_size;
            de->name_len = (uint8_t)name_len;
            de->file_type = file_type;
            memcpy(de->name, name, name_len);
            int r = ext2_write_block(fs, blk_num, blk_buf);
            if (r == 0) ext2_write_inode(fs, dir_ino, dir_inode);
            kfree(blk_buf);
            return r;
        }

        if (ext2_read_block(fs, blk_num, blk_buf) < 0) break;
        uint32_t offset = 0;
        while (offset < fs->st.block_size) {
            ext2_dirent_t *de = (ext2_dirent_t *)(blk_buf + offset);
            if (!ext2_de_ok(blk_buf, offset, fs->st.block_size)) break;

            uint16_t actual = de->inode ? ext2_dir_rec_len(de->name_len) : 0;
            if (!de->inode && de->rec_len >= need) {
                de->inode = child_ino;
                de->name_len = (uint8_t)name_len;
                de->file_type = file_type;
                memcpy(de->name, name, name_len);
                int r = ext2_write_block(fs, blk_num, blk_buf);
                kfree(blk_buf);
                return r;
            }

            if (de->inode && de->rec_len >= actual + need) {
                uint16_t old_len = de->rec_len;
                de->rec_len = actual;
                ext2_dirent_t *new_de =
                    (ext2_dirent_t *)(blk_buf + offset + actual);
                new_de->inode = child_ino;
                new_de->rec_len = old_len - actual;
                new_de->name_len = (uint8_t)name_len;
                new_de->file_type = file_type;
                memcpy(new_de->name, name, name_len);
                int r = ext2_write_block(fs, blk_num, blk_buf);
                kfree(blk_buf);
                return r;
            }
            offset += de->rec_len;
        }
    }

    kfree(blk_buf);
    return -1;
}

static int ext2_remove_dirent(ext2_fs_t *fs, ext2_inode_t *dir_inode, const char *name,
                              uint32_t *removed_ino) {
    uint32_t name_len = strlen(name);
    if (name_len == 0 || name_len > 255) return -1;
    ext2_dir_unindex(dir_inode);

    uint8_t *blk_buf = (uint8_t *)kmalloc(fs->st.block_size);
    if (!blk_buf) return -1;

    uint32_t max_blocks = dir_inode->i_size / fs->st.block_size +
                          (dir_inode->i_size % fs->st.block_size != 0);
    for (uint32_t blk_idx = 0; blk_idx < max_blocks; blk_idx++) {
        uint32_t blk_num = ext2_file_blk(fs, dir_inode, blk_idx);
        if (!blk_num) continue;
        if (ext2_read_block(fs, blk_num, blk_buf) < 0) break;

        uint32_t offset = 0;
        ext2_dirent_t *prev = NULL;
        while (offset < fs->st.block_size) {
            ext2_dirent_t *de = (ext2_dirent_t *)(blk_buf + offset);
            if (!ext2_de_ok(blk_buf, offset, fs->st.block_size)) break;

            if (de->inode && de->name_len == (uint8_t)name_len &&
                memcmp(de->name, name, name_len) == 0) {
                if (removed_ino) *removed_ino = de->inode;
                if (prev) {
                    prev->rec_len += de->rec_len;
                } else {
                    de->inode = 0;
                }
                int r = ext2_write_block(fs, blk_num, blk_buf);
                kfree(blk_buf);
                return r;
            }
            prev = de;
            offset += de->rec_len;
        }
    }

    kfree(blk_buf);
    return -1;
}

static int ext2_dir_is_empty(ext2_fs_t *fs, ext2_inode_t *inode) {
    uint8_t *blk_buf = (uint8_t *)kmalloc(fs->st.block_size);
    if (!blk_buf) return 0;

    uint32_t max_blocks = inode->i_size / fs->st.block_size +
                          (inode->i_size % fs->st.block_size != 0);
    for (uint32_t blk_idx = 0; blk_idx < max_blocks; blk_idx++) {
        uint32_t blk_num = ext2_file_blk(fs, inode, blk_idx);
        if (!blk_num) continue;
        if (ext2_read_block(fs, blk_num, blk_buf) < 0) {
            kfree(blk_buf);
            return 0;
        }

        uint32_t offset = 0;
        while (offset < fs->st.block_size) {
            ext2_dirent_t *de = (ext2_dirent_t *)(blk_buf + offset);
            if (!ext2_de_ok(blk_buf, offset, fs->st.block_size)) break;
            if (de->inode) {
                int dot = (de->name_len == 1 && de->name[0] == '.');
                int dotdot = (de->name_len == 2 &&
                              de->name[0] == '.' && de->name[1] == '.');
                if (!dot && !dotdot) {
                    kfree(blk_buf);
                    return 0;
                }
            }
            offset += de->rec_len;
        }
    }

    kfree(blk_buf);
    return 1;
}

static int ext2_init_dir_block(ext2_fs_t *fs, uint32_t blk, uint32_t self_ino,
                               uint32_t parent_ino) {
    uint8_t *buf = (uint8_t *)kmalloc(fs->st.block_size);
    if (!buf) return -1;
    memset(buf, 0, fs->st.block_size);

    ext2_dirent_t *dot = (ext2_dirent_t *)buf;
    dot->inode = self_ino;
    dot->rec_len = ext2_dir_rec_len(1);
    dot->name_len = 1;
    dot->file_type = 2;
    dot->name[0] = '.';

    ext2_dirent_t *dotdot = (ext2_dirent_t *)(buf + dot->rec_len);
    dotdot->inode = parent_ino;
    dotdot->rec_len = (uint16_t)(fs->st.block_size - dot->rec_len);
    dotdot->name_len = 2;
    dotdot->file_type = 2;
    dotdot->name[0] = '.';
    dotdot->name[1] = '.';

    int r = ext2_write_block(fs, blk, buf);
    kfree(buf);
    return r;
}

/*
 * Free the blocks of one indirect subtree that lie at or after file-block index
 * `from`, and the indirect blocks that become empty as a result.
 *
 *   blk    the indirect block being walked
 *   level  how many pointer levels it holds: 1 = pointers to data blocks,
 *          2 = pointers to singly-indirect blocks, 3 = to doubly-indirect
 *   base   the file-block index that its first entry maps
 *   bufs   one scratch block per level, so a recursive call cannot clobber its
 *          caller's table.  Allocated once by ext2_free_blocks_from(fs).
 *
 * Returns 1 if the subtree ended up completely empty, in which case `blk`
 * itself has been freed and the caller must clear its pointer.
 */
static int ext2_free_subtree(ext2_fs_t *fs, uint32_t blk, int level, uint32_t base,
                             uint32_t from, uint32_t **bufs, uint32_t *freed) {
    uint32_t n = fs->st.block_size / 4;
    uint32_t span = 1;                       /* file blocks one entry covers */
    for (int l = 1; l < level; l++) span *= n;

    uint32_t *tbl = bufs[level - 1];
    if (!tbl || ext2_read_block(fs, blk, tbl) < 0)
        return 0;                            /* cannot read it: leave it alone */

    int dirty = 0, any = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (!tbl[i]) continue;
        uint32_t idx = base + i * span;
        if (idx + span <= from) { any = 1; continue; }   /* wholly kept */
        if (level > 1) {
            if (ext2_free_subtree(fs, tbl[i], level - 1, idx, from, bufs, freed)) {
                tbl[i] = 0;
                dirty = 1;
            } else {
                any = 1;                     /* partially kept */
            }
        } else {
            ext2_free_block(fs, tbl[i]);         /* span is 1, so idx >= from */
            (*freed)++;
            tbl[i] = 0;
            dirty = 1;
        }
    }

    if (!any) {                              /* nothing left: drop the table */
        ext2_free_block(fs, blk);
        (*freed)++;
        return 1;
    }
    if (dirty) ext2_write_block(fs, blk, tbl);
    return 0;
}

/*
 * Release every block of `inode` from file-block index `from` onward, including
 * the indirect blocks that stop being needed.  from == 0 empties the file.
 *
 * The driver reads all three indirect levels (ext2_file_blk) and allocates two
 * of them (ext2_file_blk_alloc stops at doubly indirect, ~256 MiB with 1 KiB
 * blocks), but the free path used to stop after the singly-indirect chain.
 * Everything a file held beyond ~268 KiB was therefore lost on unlink: the
 * bitmap bits stayed set with nothing referencing them, so the space could
 * never be reused and only fsck could recover it.  Walk all three levels, so
 * that guest-written files (doubly) and host-written ones such as libxul on the
 * Firefox image (triply) are both released completely.
 */
static void ext2_free_blocks_from(ext2_fs_t *fs, ext2_inode_t *inode, uint32_t from) {
    if (!inode) return;
    /* A fast symlink's i_block is its target text, not block numbers. */
    if (ext2_is_fast_symlink(fs, inode)) {
        if (from == 0) memset(inode->i_block, 0, sizeof(inode->i_block));
        return;
    }
    uint32_t n = fs->st.block_size / 4;
    uint32_t freed = 0;
    uint32_t *bufs[3];
    for (int i = 0; i < 3; i++) bufs[i] = (uint32_t *)kmalloc(fs->st.block_size);

    for (uint32_t i = 0; i < 12; i++) {
        if (i >= from && inode->i_block[i]) {
            ext2_free_block(fs, inode->i_block[i]);
            freed++;
            inode->i_block[i] = 0;
        }
    }

    uint32_t base = 12, span = 1;
    for (int lvl = 1; lvl <= 3; lvl++) {
        span *= n;                           /* file blocks this tree covers */
        uint32_t slot = 11 + (uint32_t)lvl;  /* i_block[12], [13], [14] */
        if (inode->i_block[slot] && base + span > from) {
            if (ext2_free_subtree(fs, inode->i_block[slot], lvl, base, from,
                                  bufs, &freed))
                inode->i_block[slot] = 0;
        }
        base += span;
    }

    for (int i = 0; i < 3; i++) if (bufs[i]) kfree(bufs[i]);

    /* i_blocks counts 512-byte sectors, data and indirect blocks alike. */
    uint32_t sectors = freed * fs->st.sectors_per_block;
    inode->i_blocks = (inode->i_blocks > sectors) ? inode->i_blocks - sectors : 0;
    if (from == 0) inode->i_blocks = 0;
}

/* Drop the inode's hold on its extended-attribute block (ext3 and Linux ext2
 * write them; mke2fs -d can too): free it with its last user, else count one
 * user fewer.  An attribute block this driver does not recognise is left
 * alone rather than freed. */
#define EXT2_XATTR_MAGIC 0xEA020000u
static void ext2_put_xattr_block(ext2_fs_t *fs, uint32_t blk) {
    uint32_t hdr[2];                         /* h_magic, h_refcount */
    if (ext2_read_block_part(fs, blk, 0, sizeof(hdr), hdr) < 0) return;
    if (hdr[0] != EXT2_XATTR_MAGIC) return;
    if (hdr[1] > 1) {
        uint8_t *buf = (uint8_t *)kmalloc(fs->st.block_size);
        if (!buf) return;
        if (ext2_read_block(fs, blk, buf) == 0) {
            uint32_t refs = hdr[1] - 1;
            memcpy(buf + 4, &refs, 4);
            ext2_write_block(fs, blk, buf);
        }
        kfree(buf);
    } else {
        ext2_free_block(fs, blk);
    }
}

/* Everything an inode being deleted owns: its blocks and its attribute block. */
static void ext2_free_inode_blocks(ext2_fs_t *fs, ext2_inode_t *inode) {
    int fast = ext2_is_fast_symlink(fs, inode);
    ext2_free_blocks_from(fs, inode, 0);
    if (inode->i_file_acl) {
        ext2_put_xattr_block(fs, inode->i_file_acl);
        inode->i_file_acl = 0;
    }
    if (fast) inode->i_blocks = 0;
}

/* ── Open-inode table (deferred release, Unix unlink-while-open) ───────────
 *
 * POSIX: unlink() removes the NAME.  The inode and its blocks live until the
 * last descriptor and the last mapping referring to them are gone.  tmpfs was
 * fixed for this; ext2 was not - ext2_unlink() freed the blocks and the inode
 * immediately, so a process still holding the file read an inode marked free
 * whose blocks the allocator was free to hand to somebody else.  That is worse
 * than a leak: it is cross-file corruption, and "write a temp file, unlink it,
 * keep using the fd" is an ordinary thing for a program to do.
 *
 * ext2 builds a FRESH vfs_node_t on every lookup, so node identity cannot carry
 * the count the way tmpfs's does.  Key it on the inode number instead: every
 * long-lived holder of any node for that inode counts once, through the
 * retain_fn/close_fn that vfs_retain()/vfs_close() already drive for open
 * descriptors, inherited fd tables, file-backed VMAs and the shared-mapping
 * registry.
 *
 * An orphan whose last reference is dropped by a reboot rather than a close
 * still leaks, exactly as it would on any filesystem without an on-disk orphan
 * list; recovering that needs fsck.  Table exhaustion degrades to the old
 * behaviour (immediate release) rather than to a dangling inode. */


static ext2_open_t *ext2_open_find(ext2_fs_t *fs, uint32_t ino) {
    for (int i = 0; i < EXT2_OPEN_MAX; i++)
        if (fs->open[i].ino == ino) return &fs->open[i];
    return (ext2_open_t *)0;
}

/* bg_used_dirs_count of the group holding `ino`, changed by `delta`: mkdir
 * and the release of a directory inode keep it what e2fsck counts. */
static void ext2_count_dir(ext2_fs_t *fs, uint32_t ino, int delta) {
    if (ino == 0 || ino > fs->st.inodes_count) return;
    uint32_t grp = (ino - 1) / fs->st.inodes_per_group;
    ext2_bgd_t bgd;
    if (ext2_read_bgd(fs, grp, &bgd) < 0) return;
    if (delta < 0 && bgd.bg_used_dirs_count == 0) return;
    bgd.bg_used_dirs_count = (uint16_t)(bgd.bg_used_dirs_count + delta);
    ext2_write_bgd(fs, grp, &bgd);
}

/* Drop an orphaned inode for good: its blocks, then the inode itself. */
static void ext2_release_orphan(ext2_fs_t *fs, uint32_t ino) {
    ext2_inode_t victim;
    if (ext2_read_inode(fs, ino, &victim) == 0) {
        if ((victim.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR) ext2_count_dir(fs, ino, -1);
        ext2_free_inode_blocks(fs, &victim);
        victim.i_size = 0;
        victim.i_dtime = ext2_now();
        ext2_write_inode(fs, ino, &victim);
    }
    ext2_free_inode(fs, ino);
}

static void ext2_retain_node(vfs_node_t *node) {
    if (!node || !node->private) return;
    ext2_fs_t *fs = ((ext2_priv_t *)node->private)->fs;
    uint32_t ino = ((ext2_priv_t *)node->private)->ino;
    preempt_disable();
    ext2_open_t *e = ext2_open_find(fs, ino);
    if (!e) {
        e = ext2_open_find(fs, 0);
        if (e) { e->ino = ino; e->refs = 0; e->orphan = 0; }
    }
    if (e) e->refs++;
    fs->open_refs++;
    preempt_enable();
}

static void ext2_close_node(vfs_node_t *node) {
    if (!node || !node->private) return;
    ext2_fs_t *fs = ((ext2_priv_t *)node->private)->fs;
    uint32_t ino = ((ext2_priv_t *)node->private)->ino;
    int release = 0;
    preempt_disable();
    ext2_open_t *e = ext2_open_find(fs, ino);
    if (e && --e->refs <= 0) {
        release = e->orphan;
        e->ino = 0; e->refs = 0; e->orphan = 0;
    }
    if (fs->open_refs > 0) fs->open_refs--;
    preempt_enable();
    if (release) ext2_release_orphan(fs, ino);
}

/* An open directory only keeps its instance mounted (umount is -EBUSY);
 * directories are never orphaned, so the inode table is not involved. */
static void ext2_retain_dir(vfs_node_t *node) {
    if (!node || !node->private) return;
    ext2_fs_t *fs = ((ext2_priv_t *)node->private)->fs;
    preempt_disable();
    fs->open_refs++;
    preempt_enable();
}

static void ext2_close_dir(vfs_node_t *node) {
    if (!node || !node->private) return;
    ext2_fs_t *fs = ((ext2_priv_t *)node->private)->fs;
    preempt_disable();
    if (fs->open_refs > 0) fs->open_refs--;
    preempt_enable();
}

/* ── Node cache: one vfs_node_t per inode ────────────────────────────────────
 *
 * Every lookup used to kmalloc a fresh node (and its private data) and nothing
 * ever freed it: vfs_open() hands out lookup results without taking a
 * reference, and stat(), access(), each directory crossed by a path walk and
 * ext2_create()'s own existence check simply dropped theirs.  A `find /` or a
 * shell loop calling stat() grew the kernel heap without bound.
 *
 * Those same callers are also why an idle node can never be freed: they keep
 * the bare pointer for as long as they like, a preemptible kernel lets anyone
 * else run in between, and nothing tells ext2 when they are done.  So a node is
 * created once per inode number and then reused by every later lookup of that
 * inode, forever.  The memory is bounded by the number of inodes on the disk
 * rather than by the number of lookups, and no pointer handed out is ever
 * freed underneath its holder.  Descriptor and mapping lifetimes are still
 * tracked by the open-inode table above.
 *
 * Each lookup refreshes the node from the on-disk inode, because the number
 * may have been freed and handed to a new file of a different type since the
 * node was last used. */

typedef struct {
    vfs_node_t  vnode;
    ext2_priv_t priv;            /* vnode.private points here */
} ext2_vnode_t;


/* Copy the on-disk state into `node` and install the operations that fit the
 * inode's type. */
static void ext2_fill_node(vfs_node_t *node, const ext2_inode_t *inode) {
    node->size  = inode->i_size;
    node->mask  = inode->i_mode & 0xFFF;
    node->uid   = inode->i_uid;
    node->gid   = inode->i_gid;
    node->atime = inode->i_atime;
    node->mtime = inode->i_mtime;
    node->ctime = inode->i_ctime;
    node->nlink = inode->i_links_count;

    uint16_t type = inode->i_mode & EXT2_S_IFMT;

    if (type == EXT2_S_IFDIR) {
        node->flags       = VFS_FLAG_DIR;
        node->finddir_fn  = ext2_finddir;
        node->readdir_fn  = ext2_readdir;
        node->create_fn   = ext2_create;
        node->unlink_fn   = ext2_unlink;
        node->symlink_fn  = ext2_symlink;
        node->rename_fn   = ext2_rename;
        node->read_fn     = NULL;
        node->write_fn    = NULL;
        node->truncate_fn = NULL;
        node->retain_fn   = ext2_retain_dir;
        node->close_fn    = ext2_close_dir;
    } else if (type == EXT2_S_IFLNK) {
        /* read_fn yields the target (fast or slow); the VFS resolver and
         * readlink() read it that way.  The target is never rewritten. */
        node->flags       = VFS_FLAG_SYMLINK;
        node->finddir_fn  = NULL;
        node->readdir_fn  = NULL;
        node->create_fn   = NULL;
        node->unlink_fn   = NULL;
        node->symlink_fn  = NULL;
        node->rename_fn   = NULL;
        node->read_fn     = ext2_read_node;
        node->write_fn    = NULL;
        node->truncate_fn = NULL;
        node->retain_fn   = ext2_retain_node;
        node->close_fn    = ext2_close_node;
    } else {
        /* A socket inode (bind()) has no data: nothing to read or write. */
        int sock = (type == EXT2_S_IFSOCK);
        node->flags       = sock ? VFS_FLAG_SOCK : VFS_FLAG_FILE;
        node->finddir_fn  = NULL;
        node->readdir_fn  = NULL;
        node->create_fn   = NULL;
        node->unlink_fn   = NULL;
        node->symlink_fn  = NULL;
        node->rename_fn   = NULL;
        node->read_fn     = sock ? NULL : ext2_read_node;
        node->write_fn    = sock ? NULL : ext2_write_node;
        node->truncate_fn = sock ? NULL : ext2_truncate;
        /* Reference tracking so unlink can defer the release; see the
         * open-inode table above. */
        node->retain_fn   = ext2_retain_node;
        node->close_fn    = ext2_close_node;
    }

    node->setattr_fn = ext2_setattr;
    node->settimes_fn = ext2_settimes;
    node->link_fn = ext2_link;
}

/* Return the node for inode `ino_num`, found as `name`, creating it on the
 * first lookup. */
static vfs_node_t *ext2_make_node(ext2_fs_t *fs, uint32_t ino_num, const char *name,
                                   uint8_t ftype) {
    (void)ftype;
    if (ino_num == 0 || ino_num > fs->st.inodes_count) return NULL;

    /* Read inode to get size and mode */
    ext2_inode_t inode;
    if (ext2_read_inode(fs, ino_num, &inode) < 0) return NULL;

    /* Find-or-insert with preemption off so two racing lookups of one inode
     * cannot both insert a node for it. */
    preempt_disable();
    ext2_priv_t **bucket = &fs->nodes[ino_num % EXT2_NODE_BUCKETS];
    ext2_vnode_t *en = NULL;
    for (ext2_priv_t *p = *bucket; p; p = p->hnext) {
        if (p->ino == ino_num) {
            en = (ext2_vnode_t *)((uint8_t *)p - offsetof(ext2_vnode_t, priv));
            break;
        }
    }
    if (!en) {
        en = (ext2_vnode_t *)kmalloc(sizeof(ext2_vnode_t));
        if (!en) { preempt_enable(); return NULL; }
        memset(en, 0, sizeof(ext2_vnode_t));
        en->vnode.inode   = ino_num;
        en->vnode.private = &en->priv;
        en->priv.ino      = ino_num;
        en->priv.fs       = fs;
        en->vnode.dev     = fs->rdev;
        en->priv.hnext    = *bucket;
        *bucket = &en->priv;
    }
    vfs_node_t *node = &en->vnode;
    /* "." and ".." name the directory by a relation, not by its name. */
    if (!en->vnode.name[0] ||
        (strcmp(name, ".") != 0 && strcmp(name, "..") != 0)) {
        strncpy(node->name, name, 255);
        node->name[255] = '\0';
    }
    ext2_fill_node(node, &inode);
    preempt_enable();

    return node;
}

static int ext2_create(vfs_node_t *dir, const char *name, uint32_t flags) {
    if (!dir || !dir->private || !name) return -1;
    ext2_fs_t *fs = ((ext2_priv_t *)dir->private)->fs;
    if (flags != VFS_FLAG_FILE && flags != VFS_FLAG_DIR && flags != VFS_FLAG_SOCK)
        return -22;                                                     /* -EINVAL */
    if (ext2_finddir(dir, name)) return -17;   /* -EEXIST (callers treat EEXIST as
                                                * "already there" = OK; other errno
                                                * is fatal — see tmpfs_create note) */

    ext2_priv_t *dpriv = (ext2_priv_t *)dir->private;
    ext2_inode_t dir_inode;
    if (ext2_read_inode(fs, dpriv->ino, &dir_inode) < 0) return -1;
    if ((dir_inode.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return -1;

    uint32_t ino = ext2_alloc_inode(fs);
    if (!ino) return -1;

    ext2_inode_t inode;
    memset(&inode, 0, sizeof(inode));
    inode.i_uid = 0;
    inode.i_gid = 0;
    uint32_t now = ext2_now();
    inode.i_atime = now;
    inode.i_ctime = now;
    inode.i_mtime = now;

    if (flags == VFS_FLAG_DIR) {
        uint32_t blk = ext2_alloc_block(fs);
        if (!blk) {
            ext2_free_inode(fs, ino);
            return -1;
        }
        if (ext2_init_dir_block(fs, blk, ino, dpriv->ino) < 0) {
            ext2_free_block(fs, blk);
            ext2_free_inode(fs, ino);
            return -1;
        }
        inode.i_mode = EXT2_S_IFDIR | 0755;
        inode.i_size = fs->st.block_size;
        inode.i_links_count = 2;
        inode.i_blocks = fs->st.sectors_per_block;
        inode.i_block[0] = blk;
    } else {
        inode.i_mode = (flags == VFS_FLAG_SOCK ? EXT2_S_IFSOCK : EXT2_S_IFREG) | 0644;
        inode.i_size = 0;
        inode.i_links_count = 1;
        inode.i_blocks = 0;
    }

    if (ext2_write_inode(fs, ino, &inode) < 0) {
        ext2_free_inode_blocks(fs, &inode);
        ext2_free_inode(fs, ino);
        return -1;
    }

    uint8_t ftype = (flags == VFS_FLAG_DIR) ? 2
                  : (flags == VFS_FLAG_SOCK) ? EXT2_FT_SOCK : 1;
    if (ext2_add_dirent(fs, dpriv->ino, &dir_inode, ino, name, ftype) < 0) {
        ext2_free_inode_blocks(fs, &inode);
        memset(&inode, 0, sizeof(inode));
        ext2_write_inode(fs, ino, &inode);
        ext2_free_inode(fs, ino);
        return -1;
    }
    dir_inode.i_mtime = now;
    dir_inode.i_ctime = now;
    if (flags == VFS_FLAG_DIR) {
        dir_inode.i_links_count++;
        ext2_count_dir(fs, ino, 1);
    }
    ext2_write_inode(fs, dpriv->ino, &dir_inode);
    return 0;
}

/* symlink(2): a target shorter than 60 bytes lives in i_block (a fast
 * symlink, i_blocks 0, as mke2fs/debugfs and Linux write it); a longer one
 * gets one data block.  Owner and mode are set by the caller (vfs_setattr). */
static int ext2_symlink(vfs_node_t *dir, const char *name, const char *target) {
    if (!dir || !dir->private || !name || !target) return -22;
    ext2_fs_t *fs = ((ext2_priv_t *)dir->private)->fs;
    uint32_t tlen = strlen(target);
    if (tlen == 0) return -2;                                   /* -ENOENT */
    if (tlen >= fs->st.block_size) return -36;                 /* -ENAMETOOLONG */
    uint32_t nlen = strlen(name);
    if (nlen == 0) return -22;
    if (nlen > 255) return -36;
    if (ext2_finddir(dir, name)) return -17;                    /* -EEXIST */

    ext2_priv_t *dpriv = (ext2_priv_t *)dir->private;
    ext2_inode_t dir_inode;
    if (ext2_read_inode(fs, dpriv->ino, &dir_inode) < 0) return -5;
    if ((dir_inode.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return -20;

    uint32_t ino = ext2_alloc_inode(fs);
    if (!ino) return -28;                                       /* -ENOSPC */

    ext2_inode_t inode;
    memset(&inode, 0, sizeof(inode));
    uint32_t now = ext2_now();
    inode.i_atime = inode.i_ctime = inode.i_mtime = now;
    inode.i_mode = EXT2_S_IFLNK | 0777;
    inode.i_links_count = 1;
    inode.i_size = tlen;

    if (tlen < EXT2_FAST_LINK_MAX) {
        memcpy(inode.i_block, target, tlen);
    } else {
        uint8_t *buf = (uint8_t *)kmalloc(fs->st.block_size);
        if (!buf) { ext2_free_inode(fs, ino); return -12; }         /* -ENOMEM */
        uint32_t blk = ext2_alloc_block(fs);
        if (!blk) { kfree(buf); ext2_free_inode(fs, ino); return -28; }
        memset(buf, 0, fs->st.block_size);
        memcpy(buf, target, tlen);
        int w = ext2_write_block(fs, blk, buf);
        kfree(buf);
        if (w < 0) {
            ext2_free_block(fs, blk);
            ext2_free_inode(fs, ino);
            return -5;
        }
        inode.i_block[0] = blk;
        inode.i_blocks = fs->st.sectors_per_block;
    }

    if (ext2_write_inode(fs, ino, &inode) < 0) {
        ext2_free_inode_blocks(fs, &inode);
        ext2_free_inode(fs, ino);
        return -5;
    }
    if (ext2_add_dirent(fs, dpriv->ino, &dir_inode, ino, name, EXT2_FT_SYMLINK) < 0) {
        ext2_free_inode_blocks(fs, &inode);
        memset(&inode, 0, sizeof(inode));
        ext2_write_inode(fs, ino, &inode);
        ext2_free_inode(fs, ino);
        return -28;
    }
    dir_inode.i_mtime = now;
    dir_inode.i_ctime = now;
    ext2_write_inode(fs, dpriv->ino, &dir_inode);
    return 0;
}

/* The last name of `ino` is gone (victim->i_links_count is 0).  Free it now,
 * or — while a descriptor or a mapping still holds it — mark it orphaned so
 * ext2_close_node() frees it when the last one closes. */
static void ext2_put_unlinked(ext2_fs_t *fs, uint32_t ino, ext2_inode_t *victim, uint32_t now) {
    preempt_disable();
    ext2_open_t *e = ext2_open_find(fs, ino);
    int in_use = (e && e->refs > 0);
    if (in_use) e->orphan = 1;
    preempt_enable();

    if (in_use) {
        /* Name gone, data still reachable through the open descriptors. */
        ext2_write_inode(fs, ino, victim);
    } else {
        if ((victim->i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR) ext2_count_dir(fs, ino, -1);
        ext2_free_inode_blocks(fs, victim);
        victim->i_dtime = now;
        victim->i_size = 0;
        ext2_write_inode(fs, ino, victim);
        ext2_free_inode(fs, ino);
    }
}

static int ext2_unlink(vfs_node_t *dir, const char *name) {
    if (!dir || !dir->private || !name) return -1;
    ext2_fs_t *fs = ((ext2_priv_t *)dir->private)->fs;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return -1;

    ext2_priv_t *dpriv = (ext2_priv_t *)dir->private;
    ext2_inode_t dir_inode;
    if (ext2_read_inode(fs, dpriv->ino, &dir_inode) < 0) return -1;
    if ((dir_inode.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return -1;

    vfs_node_t *victim_node = ext2_finddir(dir, name);
    if (!victim_node || !victim_node->private) return -1;
    uint32_t victim_ino = ((ext2_priv_t *)victim_node->private)->ino;

    ext2_inode_t victim;
    if (ext2_read_inode(fs, victim_ino, &victim) < 0) return -1;
    uint16_t type = victim.i_mode & EXT2_S_IFMT;

    if (type == EXT2_S_IFDIR && !ext2_dir_is_empty(fs, &victim))
        return -1;

    if (type != EXT2_S_IFREG && type != EXT2_S_IFDIR && type != EXT2_S_IFSOCK &&
        type != EXT2_S_IFLNK)
        return -1;

    if (ext2_remove_dirent(fs, &dir_inode, name, NULL) < 0) return -1;

    uint32_t now = ext2_now();

    /* Drop one link.  The inode only dies when the last name for it is gone -
     * and even then not while a descriptor or a mapping still holds it, which
     * is what the open-inode table records.  (link() is not implemented today,
     * so i_links_count is 1 for a regular file and 2 for a directory, but doing
     * this by the count rather than by assumption keeps unlink correct if hard
     * links ever arrive.) */
    if (victim.i_links_count > 0) victim.i_links_count--;
    /* A directory also loses the link its own "." entry held (Linux
     * ext2_rmdir: i_size = 0 and a second inode_dec_link_count).  Leaving
     * that one in place kept the removed directory's inode and blocks
     * allocated for good. */
    if (type == EXT2_S_IFDIR) victim.i_links_count = 0;
    victim.i_ctime = now;

    if (victim.i_links_count > 0)            /* another name still refers to it */
        ext2_write_inode(fs, victim_ino, &victim);
    else
        ext2_put_unlinked(fs, victim_ino, &victim, now);

    dir_inode.i_mtime = now;
    dir_inode.i_ctime = now;
    if (type == EXT2_S_IFDIR && dir_inode.i_links_count > 0) {
        dir_inode.i_links_count--;
    }
    ext2_write_inode(fs, dpriv->ino, &dir_inode);
    return 0;
}

/* Look `name` up in a directory inode: its inode number (0 if absent) and,
 * through *ftype, the entry's file-type byte. */
static uint32_t ext2_dir_lookup(ext2_fs_t *fs, ext2_inode_t *dir_inode, const char *name,
                                uint8_t *ftype) {
    uint32_t name_len = strlen(name);
    if (name_len == 0 || name_len > 255) return 0;
    uint8_t *blk_buf = (uint8_t *)kmalloc(fs->st.block_size);
    if (!blk_buf) return 0;
    uint32_t found = 0;
    for (uint32_t pos = 0; pos < dir_inode->i_size && !found;
         pos += fs->st.block_size) {
        uint32_t blk_num = ext2_file_blk(fs, dir_inode, pos / fs->st.block_size);
        if (!blk_num) continue;
        if (ext2_read_block(fs, blk_num, blk_buf) < 0) break;
        for (uint32_t off = 0; off < fs->st.block_size; ) {
            ext2_dirent_t *de = (ext2_dirent_t *)(blk_buf + off);
            if (!ext2_de_ok(blk_buf, off, fs->st.block_size)) break;
            if (de->inode && de->name_len == (uint8_t)name_len &&
                memcmp(de->name, name, name_len) == 0) {
                found = de->inode;
                if (ftype) *ftype = de->file_type;
                break;
            }
            off += de->rec_len;
        }
    }
    kfree(blk_buf);
    return found;
}

/* Point the existing entry `name` at inode `ino` (type byte `ftype`).  The
 * entry is rewritten in place with a single block write, which is what makes
 * replacing a rename target atomic: the name refers to the old inode before
 * the write and to the new one after it, and never to nothing. */
static int ext2_set_dirent(ext2_fs_t *fs, ext2_inode_t *dir_inode, const char *name,
                           uint32_t ino, uint8_t ftype) {
    uint32_t name_len = strlen(name);
    if (name_len == 0 || name_len > 255) return -1;
    ext2_dir_unindex(dir_inode);
    uint8_t *blk_buf = (uint8_t *)kmalloc(fs->st.block_size);
    if (!blk_buf) return -1;
    for (uint32_t pos = 0; pos < dir_inode->i_size; pos += fs->st.block_size) {
        uint32_t blk_num = ext2_file_blk(fs, dir_inode, pos / fs->st.block_size);
        if (!blk_num) continue;
        if (ext2_read_block(fs, blk_num, blk_buf) < 0) break;
        for (uint32_t off = 0; off < fs->st.block_size; ) {
            ext2_dirent_t *de = (ext2_dirent_t *)(blk_buf + off);
            if (!ext2_de_ok(blk_buf, off, fs->st.block_size)) break;
            if (de->inode && de->name_len == (uint8_t)name_len &&
                memcmp(de->name, name, name_len) == 0) {
                de->inode = ino;
                de->file_type = ftype;
                int r = ext2_write_block(fs, blk_num, blk_buf);
                kfree(blk_buf);
                return r;
            }
            off += de->rec_len;
        }
    }
    kfree(blk_buf);
    return -1;
}

/* rename_fn (Linux ext2_rename).  Order of the on-disk updates:
 *   1. new_name → source inode (rewritten in place if it exists, else added),
 *   2. old_name removed,
 *   3. a moved directory's ".." and the parents' link counts fixed,
 *   4. the replaced inode loses its link (freed, or orphaned while open).
 * A crash between 1 and 2 leaves the object under both names, never under
 * neither.  The source inode is not touched beyond its ctime, so it keeps its
 * mode, owner and data. */
static int ext2_rename(vfs_node_t *old_dir, const char *old_name,
                       vfs_node_t *new_dir, const char *new_name) {
    if (!old_dir || !new_dir || !old_dir->private ||
        !new_dir->private || !old_name || !new_name)
        return -1;
    ext2_fs_t *fs = ((ext2_priv_t *)old_dir->private)->fs;
    if (((ext2_priv_t *)new_dir->private)->fs != fs) return -18;   /* -EXDEV */
    if (strcmp(old_name, ".") == 0 || strcmp(old_name, "..") == 0 ||
        strcmp(new_name, ".") == 0 || strcmp(new_name, "..") == 0)
        return -22;                                          /* -EINVAL */
    if (strlen(new_name) > 255) return -36;                  /* -ENAMETOOLONG */

    uint32_t o_ino = ((ext2_priv_t *)old_dir->private)->ino;
    uint32_t n_ino = ((ext2_priv_t *)new_dir->private)->ino;
    int same_dir = (o_ino == n_ino);
    ext2_inode_t odir, ndir_store;
    ext2_inode_t *ndir = same_dir ? &odir : &ndir_store;
    if (ext2_read_inode(fs, o_ino, &odir) < 0) return -5;       /* -EIO */
    if (!same_dir && ext2_read_inode(fs, n_ino, ndir) < 0) return -5;
    if ((odir.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR ||
        (ndir->i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR)
        return -20;                                          /* -ENOTDIR */

    uint8_t src_ft = 0, dst_ft = 0;
    uint32_t src_ino = ext2_dir_lookup(fs, &odir, old_name, &src_ft);
    if (!src_ino) return -2;                                 /* -ENOENT */
    uint32_t dst_ino = ext2_dir_lookup(fs, ndir, new_name, &dst_ft);
    if (dst_ino == src_ino) return 0;        /* same object: nothing to do */

    ext2_inode_t src, dst;
    if (ext2_read_inode(fs, src_ino, &src) < 0) return -5;
    int src_is_dir = ((src.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR);
    int dst_is_dir = 0;
    if (dst_ino) {
        if (ext2_read_inode(fs, dst_ino, &dst) < 0) return -5;
        dst_is_dir = ((dst.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR);
        if (src_is_dir && !dst_is_dir) return -20;           /* -ENOTDIR */
        if (!src_is_dir && dst_is_dir) return -21;           /* -EISDIR */
        if (dst_is_dir && !ext2_dir_is_empty(fs, &dst)) return -39;  /* -ENOTEMPTY */
    }

    /* A directory cannot move below itself: walk up from the new parent. */
    if (src_is_dir && !same_dir) {
        uint32_t cur = n_ino;
        for (int depth = 0; depth < 256 && cur != 2; depth++) {
            if (cur == src_ino) return -22;                  /* -EINVAL */
            ext2_inode_t ci;
            if (ext2_read_inode(fs, cur, &ci) < 0) return -5;
            uint32_t up = ext2_dir_lookup(fs, &ci, "..", (uint8_t *)0);
            if (!up || up == cur) break;
            cur = up;
        }
    }

    /* 1. The new name. */
    if (dst_ino) {
        if (ext2_set_dirent(fs, ndir, new_name, src_ino, src_ft) < 0) return -5;
    } else {
        if (ext2_add_dirent(fs, n_ino, ndir, src_ino, new_name, src_ft) < 0)
            return -28;                                      /* -ENOSPC */
    }

    /* 2. The old name.  In the same directory this re-reads nothing: odir is
     * the only copy of the inode and add_dirent kept it current. */
    if (ext2_remove_dirent(fs, &odir, old_name, (uint32_t *)0) < 0)
        printk("[ext2] rename: '%s' vanished from its directory\n", old_name);

    uint32_t now = ext2_now();

    /* 3. A directory that changed parent: its ".." and both link counts. */
    if (src_is_dir && !same_dir) {
        ext2_set_dirent(fs, &src, "..", n_ino, 2);
        if (odir.i_links_count > 0) odir.i_links_count--;
        ndir->i_links_count++;
    }
    src.i_ctime = now;
    ext2_write_inode(fs, src_ino, &src);

    /* 4. The replaced object.  An (empty) directory loses both its names — the
     * entry and its own "." — and its ".." no longer counts on the parent. */
    if (dst_ino) {
        if (dst_is_dir) {
            dst.i_links_count = 0;
            if (ndir->i_links_count > 0) ndir->i_links_count--;
        } else if (dst.i_links_count > 0) {
            dst.i_links_count--;
        }
        dst.i_ctime = now;
        if (dst.i_links_count > 0)
            ext2_write_inode(fs, dst_ino, &dst);
        else
            ext2_put_unlinked(fs, dst_ino, &dst, now);
    }

    odir.i_mtime = odir.i_ctime = now;
    ext2_write_inode(fs, o_ino, &odir);
    if (!same_dir) {
        ndir->i_mtime = ndir->i_ctime = now;
        ext2_write_inode(fs, n_ino, ndir);
    }
    return 0;
}

/* Persist chmod/chown to the on-disk inode (Phase 24). */
static int ext2_setattr(vfs_node_t *node, uint32_t mode, uint32_t uid,
                        uint32_t gid) {
    if (!node || !node->private) return -1;
    ext2_priv_t *priv = (ext2_priv_t *)node->private;
    ext2_fs_t *fs = priv->fs;
    ext2_inode_t inode;
    if (ext2_read_inode(fs, priv->ino, &inode) < 0) return -1;
    inode.i_mode = (uint16_t)((inode.i_mode & EXT2_S_IFMT) | (mode & 0xFFF));
    inode.i_uid  = (uint16_t)uid;
    inode.i_gid  = (uint16_t)gid;
    return ext2_write_inode(fs, priv->ino, &inode);
}

/* Persist utimensat()'s atime/mtime; ctime becomes now, as on Linux. */
static int ext2_settimes(vfs_node_t *node, uint32_t atime, uint32_t mtime) {
    if (!node || !node->private) return -1;
    ext2_priv_t *priv = (ext2_priv_t *)node->private;
    ext2_fs_t *fs = priv->fs;
    ext2_inode_t inode;
    if (ext2_read_inode(fs, priv->ino, &inode) < 0) return -1;
    inode.i_atime = atime;
    inode.i_mtime = mtime;
    inode.i_ctime = ext2_now();
    node->ctime = inode.i_ctime;
    return ext2_write_inode(fs, priv->ino, &inode);
}

/* link(2): one more directory entry for an existing inode.  Directories
 * cannot be linked (-EPERM, as on Linux). */
static int ext2_link(vfs_node_t *dir, const char *name, vfs_node_t *target) {
    if (!dir || !dir->private || !target || !target->private)
        return -22;
    ext2_fs_t *fs = ((ext2_priv_t *)dir->private)->fs;
    if (((ext2_priv_t *)target->private)->fs != fs) return -18;    /* -EXDEV */
    if (ext2_finddir(dir, name)) return -17;                    /* -EEXIST */
    ext2_priv_t *dpriv = (ext2_priv_t *)dir->private;
    ext2_priv_t *tpriv = (ext2_priv_t *)target->private;
    ext2_inode_t dir_inode, inode;
    if (ext2_read_inode(fs, dpriv->ino, &dir_inode) < 0) return -5;
    if ((dir_inode.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return -20;
    if (ext2_read_inode(fs, tpriv->ino, &inode) < 0) return -5;
    uint16_t type = inode.i_mode & EXT2_S_IFMT;
    if (type == EXT2_S_IFDIR) return -1;                        /* -EPERM */
    if (inode.i_links_count == 0) return -2;                    /* -ENOENT */
    if (inode.i_links_count >= 65000) return -31;               /* -EMLINK */
    uint8_t ftype = type == EXT2_S_IFREG  ? 1 : type == EXT2_S_IFLNK ? 7
                  : type == EXT2_S_IFSOCK ? EXT2_FT_SOCK
                  : type == 0x2000 ? 3 : type == 0x6000 ? 4 : type == 0x1000 ? 5 : 0;
    if (ext2_add_dirent(fs, dpriv->ino, &dir_inode, tpriv->ino, name, ftype) < 0)
        return -28;                                             /* -ENOSPC */
    uint32_t now = ext2_now();
    inode.i_links_count++;
    inode.i_ctime = now;
    if (ext2_write_inode(fs, tpriv->ino, &inode) < 0) return -5;
    target->nlink = inode.i_links_count;
    target->ctime = now;
    dir_inode.i_mtime = dir_inode.i_ctime = now;
    ext2_write_inode(fs, dpriv->ino, &dir_inode);
    return 0;
}

static int ext2_truncate(vfs_node_t *node, uint32_t new_size) {
    if (!node || !node->private) return -1;
    ext2_priv_t *priv = (ext2_priv_t *)node->private;
    ext2_fs_t *fs = priv->fs;

    ext2_inode_t inode;
    if (ext2_read_inode(fs, priv->ino, &inode) < 0) return -1;
    if ((inode.i_mode & EXT2_S_IFMT) != EXT2_S_IFREG) return -1;

    /* Rounded up without forming size + bs - 1, which wraps for sizes near
     * 4 GiB: ftruncate(fd, -1) used to compute new_blocks = 0, free every
     * block and leave i_size at 0xFFFFFFFF. */
    uint32_t bs = fs->st.block_size;
    uint32_t old_blocks = inode.i_size / bs + (inode.i_size % bs != 0);
    uint32_t new_blocks = new_size / bs + (new_size % bs != 0);
    /* The limit belongs to GROWING only, and it is what ext2_file_blk_alloc can
     * actually reach (direct + singly + doubly indirect).  Applying it to every
     * call also refused to SHRINK a file bigger than that - ftruncate() on a
     * multi-megabyte file returned -1 instead of releasing the tail. */
    uint32_t ppb = fs->st.block_size / 4;
    uint32_t max_blocks = 12 + ppb + ppb * ppb;
    if (new_blocks > old_blocks && new_blocks > max_blocks) return -1;

    if (new_blocks > old_blocks) {
        uint32_t i;
        for (i = old_blocks; i < new_blocks; i++) {
            if (!ext2_file_blk_alloc(fs, &inode, i)) {
                ext2_free_blocks_from(fs, &inode, old_blocks);   /* undo this call */
                return -1;
            }
        }
    } else if (new_blocks < old_blocks) {
        /* Free the tail, including any indirect blocks it leaves empty.  This
         * used to go through a per-index helper that silently did nothing
         * beyond the singly-indirect range, so shrinking a large file leaked
         * exactly like unlink did. */
        ext2_free_blocks_from(fs, &inode, new_blocks);
    }
    inode.i_size = new_size;
    inode.i_mtime = ext2_now();
    inode.i_ctime = inode.i_mtime;
    node->size = new_size;
    node->mtime = inode.i_mtime;
    node->ctime = inode.i_ctime;
    return ext2_write_inode(fs, priv->ino, &inode);
}

/* ── VFS finddir_fn for ext2 directory nodes ─────────────────────────────── */

static vfs_node_t *ext2_finddir(vfs_node_t *dir, const char *name) {
    if (!dir->private) return NULL;
    ext2_priv_t *priv = (ext2_priv_t *)dir->private;
    ext2_fs_t *fs = priv->fs;

    ext2_inode_t inode;
    if (ext2_read_inode(fs, priv->ino, &inode) < 0) return NULL;
    if ((inode.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return NULL;

    uint32_t blk_size = fs->st.block_size;
    uint32_t name_len = strlen(name);
    uint8_t *blk_buf  = (uint8_t *)kmalloc(blk_size);
    if (!blk_buf) return NULL;

    /* Count blocks, not bytes: a byte position wraps (and the loop never ends)
     * for a corrupt i_size near 4 GiB. */
    uint32_t nblk = inode.i_size / blk_size + (inode.i_size % blk_size != 0);
    for (uint32_t blk_idx = 0; blk_idx < nblk; blk_idx++) {
        uint32_t blk_num = ext2_file_blk(fs, &inode, blk_idx);
        if (!blk_num) continue;
        if (ext2_read_block(fs, blk_num, blk_buf) < 0) break;

        uint32_t offset = 0;
        while (offset < blk_size) {
            ext2_dirent_t *de = (ext2_dirent_t *)(blk_buf + offset);
            if (!ext2_de_ok(blk_buf, offset, fs->st.block_size)) break;

            if (de->inode && de->name_len == (uint8_t)name_len &&
                memcmp(de->name, name, name_len) == 0) {
                char tmp[256];
                memcpy(tmp, de->name, de->name_len);
                tmp[de->name_len] = '\0';
                uint32_t child_ino = de->inode;
                kfree(blk_buf);
                return ext2_make_node(fs, child_ino, tmp, de->file_type);
            }
            offset += de->rec_len;
        }
    }
    kfree(blk_buf);
    return NULL;
}

/* ── VFS readdir_fn for ext2 directory nodes ─────────────────────────────── */

static int ext2_readdir(vfs_node_t *dir, uint32_t req_idx,
                         vfs_dirent_t *out) {
    if (!dir->private) return -1;
    ext2_priv_t *priv = (ext2_priv_t *)dir->private;
    ext2_fs_t *fs = priv->fs;

    ext2_inode_t inode;
    if (ext2_read_inode(fs, priv->ino, &inode) < 0) return -1;

    uint32_t blk_size = fs->st.block_size;
    uint8_t *blk_buf  = (uint8_t *)kmalloc(blk_size);
    if (!blk_buf) return -1;

    uint32_t cur_idx = 0;

    /* Count blocks, not bytes: a byte position wraps (and the loop never ends)
     * for a corrupt i_size near 4 GiB. */
    uint32_t nblk = inode.i_size / blk_size + (inode.i_size % blk_size != 0);
    for (uint32_t blk_idx = 0; blk_idx < nblk; blk_idx++) {
        uint32_t blk_num = ext2_file_blk(fs, &inode, blk_idx);
        if (!blk_num) continue;
        if (ext2_read_block(fs, blk_num, blk_buf) < 0) break;

        uint32_t offset = 0;
        while (offset < blk_size) {
            ext2_dirent_t *de = (ext2_dirent_t *)(blk_buf + offset);
            if (!ext2_de_ok(blk_buf, offset, fs->st.block_size)) break;
            if (de->inode) {
                if (cur_idx == req_idx) {
                    out->ino  = de->inode;
                    out->type = (de->file_type == 2) ? VFS_FLAG_DIR
                              : (de->file_type == EXT2_FT_SOCK) ? VFS_FLAG_SOCK
                              : (de->file_type == EXT2_FT_SYMLINK) ? VFS_FLAG_SYMLINK
                              : VFS_FLAG_FILE;
                    memcpy(out->name, de->name, de->name_len);
                    out->name[de->name_len] = '\0';
                    kfree(blk_buf);
                    return 0;
                }
                cur_idx++;
            }
            offset += de->rec_len;
        }
    }
    kfree(blk_buf);
    return -1;
}

/* ── Mount ────────────────────────────────────────────────────────────────── */

/* Superblock fields past ext2_sb_t, by byte offset. */
#define EXT2_SB_COMPAT     0x5C
#define EXT2_SB_INCOMPAT   0x60
#define EXT2_SB_RO_COMPAT  0x64

#define EXT2_VALID_FS              0x0001u   /* s_state: cleanly unmounted */
#define EXT2_INCOMPAT_FILETYPE     0x0002u
#define EXT2_INCOMPAT_RECOVER      0x0004u   /* ext3/4 journal needs replay */
#define EXT2_RO_COMPAT_SPARSE_SUPER 0x0001u
#define EXT2_RO_COMPAT_LARGE_FILE  0x0002u
#define EXT2_RO_COMPAT_BTREE_DIR   0x0004u
#define EXT2_RO_COMPAT_DIR_NLINK   0x0020u

/* What this driver understands.  Any other incompatible feature (extents,
 * 64bit, flex_bg, meta_bg, inline data, ...) changes the layout it reads, so
 * such a filesystem is the read-only ext4 driver's.  A read-only-compatible
 * feature it does not know (metadata_csum, gdt_csum, huge_file, extra_isize,
 * ...) can still be read but not written without breaking what the feature
 * promises, so the instance stays read-only.  A journal (has_journal, an
 * ext3) is fine to write around while it is empty, as Linux's ext2 driver
 * does; one that needs recovery is not. */
#define EXT2_INCOMPAT_OK   EXT2_INCOMPAT_FILETYPE
#define EXT2_RO_COMPAT_RW  (EXT2_RO_COMPAT_SPARSE_SUPER | EXT2_RO_COMPAT_LARGE_FILE | \
                            EXT2_RO_COMPAT_BTREE_DIR | EXT2_RO_COMPAT_DIR_NLINK)

static ext2_fs_t *g_boot_fs;          /* the /disk instance, for ext2_statfs */

int ext2_statfs(uint32_t *block_size, uint32_t *blocks, uint32_t *bfree,
                uint32_t *inodes, uint32_t *ifree) {
    return ext2_statfs_fs(g_boot_fs, block_size, blocks, bfree, inodes, ifree);
}

static void ext2_free_fs(ext2_fs_t *fs) {
    for (int i = 0; i < EXT2_NODE_BUCKETS; i++) {
        ext2_priv_t *p = fs->nodes[i];
        while (p) {
            ext2_priv_t *next = p->hnext;
            kfree((uint8_t *)p - offsetof(ext2_vnode_t, priv));
            p = next;
        }
    }
    if (fs->cache_slab) kfree(fs->cache_slab);
    if (fs->cache)      kfree(fs->cache);
    if (fs->ra_buf)     kfree(fs->ra_buf);
    if (fs->seen)       kfree(fs->seen);
    kfree(fs);
}

/* Record a read-write mount in the superblock (mount count, mount time, and
 * "not clean" until it is unmounted), or its end (write time, and clean again
 * if it was clean when mounted).  e2fsck then knows a filesystem whose
 * machine stopped while it was mounted needs checking. */
static int ext2_sb_mark(ext2_fs_t *fs, int mounting) {
    uint8_t sb_buf[2048];
    if (ext2_dev_read(fs, fs->st.lba_offset + 2, 4, sb_buf) < 0) return -1;
    ext2_sb_t *sb = (ext2_sb_t *)sb_buf;
    if (sb->s_magic != 0xEF53) return -1;
    if (mounting) {
        sb->s_mtime = ext2_now();
        sb->s_mnt_count++;
        sb->s_state &= (uint16_t)~EXT2_VALID_FS;
    } else {
        sb->s_wtime = ext2_now();
        if (fs->was_clean) sb->s_state |= EXT2_VALID_FS;
    }
    return ext2_dev_write(fs, fs->st.lba_offset + 2, 2, sb_buf);   /* see above */
}

/* Check the superblock in `sb_buf` against the device and fill fs->st.
 * 0, or -EINVAL with the reason printed. */
static int ext2_load_super(ext2_fs_t *fs, const uint8_t *sb_buf, uint32_t lba_offset,
                           uint32_t nsect) {
    const ext2_sb_t *sb = (const ext2_sb_t *)sb_buf;
    if (sb->s_magic != 0xEF53) {
        printk("[EXT2]  Bad magic (0x%04x), not ext2.\n", (unsigned)sb->s_magic);
        return -22;
    }

    /* Everything below divides by, shifts by or sizes buffers from these
     * fields, and the bitmap code assumes one group's bitmap fits one block.
     * Refuse a superblock that breaks any of it instead of dividing by zero at
     * the first inode read or overrunning a bitmap buffer on the first
     * allocation.  Block sizes above 4 KiB are rejected too: a 64 KiB block
     * does not fit the 16-bit rec_len a fresh directory block is given. */
    {
        uint32_t bs  = 1024U << (sb->s_log_block_size <= 2 ? sb->s_log_block_size : 0);
        uint32_t isz = (sb->s_rev_level >= 1) ? sb->s_inode_size : 128;
        if (sb->s_log_block_size > 2 ||
            !sb->s_inodes_per_group || sb->s_inodes_per_group > bs * 8 ||
            !sb->s_blocks_per_group || sb->s_blocks_per_group > bs * 8 ||
            isz < 128 || isz > bs || (isz & (isz - 1)) ||
            !sb->s_inodes_count || !sb->s_blocks_count) {
            printk("[EXT2]  Unsupported or corrupt superblock geometry, "
                   "not mounting.\n");
            return -22;
        }
    }

    {
        uint64_t need = (uint64_t)sb->s_blocks_count << (sb->s_log_block_size + 1);
        if (need > nsect) {
            printk("[EXT2]  Superblock claims %u blocks (%u MiB) but the device "
                   "has %u MiB; not mounting.\n", (unsigned)sb->s_blocks_count,
                   (unsigned)(need / 2048u), (unsigned)(nsect / 2048u));
            return -22;
        }
    }

    fs->st.lba_offset      = lba_offset;
    fs->st.block_size      = 1024U << sb->s_log_block_size;
    fs->st.sectors_per_block = fs->st.block_size / 512;
    fs->st.inodes_per_group  = sb->s_inodes_per_group;
    fs->st.blocks_per_group  = sb->s_blocks_per_group;
    fs->st.first_data_block  = sb->s_first_data_block;
    fs->st.inodes_count      = sb->s_inodes_count;
    fs->st.blocks_count      = sb->s_blocks_count;
    fs->st.first_ino         = (sb->s_rev_level >= 1) ? sb->s_first_ino : 11;
    fs->st.inode_size        = (sb->s_rev_level >= 1) ? sb->s_inode_size : 128;
    fs->nsect                = nsect;
    if (sb->s_rev_level >= 1) {
        memcpy(&fs->compat,    sb_buf + EXT2_SB_COMPAT, 4);
        memcpy(&fs->incompat,  sb_buf + EXT2_SB_INCOMPAT, 4);
        memcpy(&fs->ro_compat, sb_buf + EXT2_SB_RO_COMPAT, 4);
    }
    fs->was_clean = (sb->s_state & EXT2_VALID_FS) != 0;
    return 0;
}

/* The root node, or NULL. */
static vfs_node_t *ext2_root(ext2_fs_t *fs) {
    vfs_node_t *root = ext2_make_node(fs, 2, "", 2);
    if (!root || root->flags != VFS_FLAG_DIR) return NULL;
    root->finddir_fn = ext2_finddir;
    root->readdir_fn = ext2_readdir;
    return root;
}

vfs_node_t *ext2_mount(uint32_t lba_offset, uint32_t nsect) {
    if (!blk_present()) {
        printk("[EXT2]  No disk, skipping mount.\n");
        return NULL;
    }
    ext2_fs_t *fs = (ext2_fs_t *)kmalloc(sizeof(ext2_fs_t));
    if (!fs) return NULL;
    memset(fs, 0, sizeof(*fs));
    fs->boot  = 1;
    fs->disk  = -1;
    fs->rw_ok = 1;
    strncpy(fs->name, "disk", sizeof(fs->name) - 1);

    /* Read superblock: always at byte 1024 = sector 2 offset 0 */
    uint8_t sb_buf[2048];  /* 4 sectors, safely covers ext2_sb_t */
    if (ext2_dev_read(fs, lba_offset + 2, 4, sb_buf) < 0) {
        printk("[EXT2]  Cannot read superblock.\n");
        kfree(fs);
        return NULL;
    }
    if (ext2_load_super(fs, sb_buf, lba_offset, nsect) < 0) {
        kfree(fs);
        return NULL;
    }

    ext2_cache_init(fs);
    ext2_seen_init(fs);

    printk("[EXT2]  Mounted: block_size=%u  inodes=%u  inode_size=%u cache=%s\n",
           (unsigned)fs->st.block_size,
           (unsigned)fs->st.inodes_count,
           (unsigned)fs->st.inode_size,
           fs->cache_ready ? "on" : "off");

    /* Build VFS node for root (inode 2) */
    vfs_node_t *root = ext2_root(fs);
    if (!root) { ext2_free_fs(fs); return NULL; }
    g_boot_fs = fs;
    return root;
}

int ext2_mount_dev(blkpart_t *bp, int ro, vfs_node_t **root_out, ext2_fs_t **fs_out) {
    ext2_fs_t *fs = (ext2_fs_t *)kmalloc(sizeof(ext2_fs_t));
    if (!fs) return -12;
    memset(fs, 0, sizeof(*fs));
    fs->disk = bp->dev;
    fs->rdev = bp->rdev;
    fs->ro   = 1;                     /* until the checks below pass */
    strncpy(fs->name, bp->name, sizeof(fs->name) - 1);
    fs->st.lba_offset = bp->start;
    fs->nsect = bp->nsect;

    uint8_t sb_buf[2048];
    int rc = -22;
    if (bp->nsect < 6 || ext2_dev_read(fs, bp->start + 2, 4, sb_buf) < 0) {
        rc = -5;
        goto fail;
    }
    if (((ext2_sb_t *)sb_buf)->s_magic != 0xEF53) goto fail;  /* quietly: not ours */
    if (ext2_load_super(fs, sb_buf, bp->start, bp->nsect) < 0) goto fail;

    uint32_t bad = fs->incompat & ~EXT2_INCOMPAT_OK;
    if (bad) {
        /* Not a layout this driver reads; the ext4 driver may (read-only). */
        printk("[EXT2]  %s: incompatible features 0x%x%s; not for the ext2 driver\n",
               fs->name, (unsigned)bad,
               (bad & EXT2_INCOMPAT_RECOVER) ? " (journal needs recovery)" : "");
        rc = (bad & EXT2_INCOMPAT_RECOVER) ? -117 : -95;   /* -EUCLEAN / -EOPNOTSUPP */
        goto fail;
    }
    fs->rw_ok = !(fs->ro_compat & ~EXT2_RO_COMPAT_RW);
    if (!ro && !fs->rw_ok) {
        printk("[EXT2]  %s: read-only-compatible features 0x%x; read-only only\n",
               fs->name, (unsigned)(fs->ro_compat & ~EXT2_RO_COMPAT_RW));
        rc = -30;                                           /* -EROFS */
        goto fail;
    }
    if (!fs->was_clean)
        printk("[EXT2]  %s: warning: not cleanly unmounted, run e2fsck\n", fs->name);

    ext2_cache_init(fs);
    vfs_node_t *root = ext2_root(fs);
    if (!root) {
        printk("[EXT2]  %s: root inode unreadable\n", fs->name);
        goto fail;
    }
    if (!ro) {
        fs->ro = 0;
        if (ext2_sb_mark(fs, 1) < 0) { rc = -5; goto fail; }
    }
    printk("[EXT2]  %s: mounted %s (block %u, %u inodes%s)\n", fs->name,
           ro ? "read-only" : "read-write", (unsigned)fs->st.block_size,
           (unsigned)fs->st.inodes_count,
           (fs->compat & 0x4u) ? ", journal not used" : "");
    *root_out = root;
    *fs_out = fs;
    return 0;

fail:
    ext2_free_fs(fs);
    return rc;
}

int ext2_busy(void *p) {
    return ((ext2_fs_t *)p)->open_refs > 0;
}

int ext2_set_ro(void *p, int ro) {
    ext2_fs_t *fs = (ext2_fs_t *)p;
    if (ro == fs->ro) return 0;
    if (ro) {
        /* Every write is synchronous (ext2_write_block goes to the disk, and
         * the disk drivers flush), so going read-only only has to close the
         * superblock's record of the mount. */
        ext2_sb_mark(fs, 0);
        fs->ro = 1;
        printk("[EXT2]  %s: now read-only\n", fs->name);
        return 0;
    }
    if (!fs->rw_ok) return -30;                             /* -EROFS */
    fs->ro = 0;
    if (ext2_sb_mark(fs, 1) < 0) { fs->ro = 1; return -5; }
    printk("[EXT2]  %s: now read-write\n", fs->name);
    return 0;
}

void ext2_release(void *p) {
    ext2_fs_t *fs = (ext2_fs_t *)p;
    if (!fs->ro) ext2_sb_mark(fs, 0);
    printk("[EXT2]  %s: unmounted\n", fs->name);
    ext2_free_fs(fs);
}
