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
#include "../proc/process.h"
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

/* A group descriptor: 32 bytes, or 64 with the ext4 64bit feature
 * (s_desc_size).  The high halves are only ever zero here, because a
 * filesystem of 2^32 blocks or more is refused at mount; they are kept as
 * read so a descriptor goes back to the disk unchanged apart from what the
 * driver means to change. */
typedef struct {
    uint32_t bg_block_bitmap;
    uint32_t bg_inode_bitmap;
    uint32_t bg_inode_table;
    uint16_t bg_free_blocks_count;
    uint16_t bg_free_inodes_count;
    uint16_t bg_used_dirs_count;
    uint16_t bg_flags;              /* EXT4_BG_* */
    uint32_t bg_exclude_bitmap;
    uint16_t bg_block_bitmap_csum;
    uint16_t bg_inode_bitmap_csum;
    uint16_t bg_itable_unused;
    uint16_t bg_checksum;
    uint32_t bg_block_bitmap_hi;
    uint32_t bg_inode_bitmap_hi;
    uint32_t bg_inode_table_hi;
    uint16_t bg_free_blocks_count_hi;
    uint16_t bg_free_inodes_count_hi;
    uint16_t bg_used_dirs_count_hi;
    uint16_t bg_itable_unused_hi;
    uint32_t bg_exclude_bitmap_hi;
    uint16_t bg_block_bitmap_csum_hi;
    uint16_t bg_inode_bitmap_csum_hi;
    uint32_t bg_reserved;
} __attribute__((packed)) ext2_bgd_t;

#define EXT4_BG_INODE_UNINIT 0x0001u   /* inode bitmap not initialised */
#define EXT4_BG_BLOCK_UNINIT 0x0002u   /* block bitmap not initialised */

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

    /* ext3/ext4 (see "ext4: checksums, uninitialised groups, extents" and
     * "jbd2" below).  x4 is set when any of it is in use: the superblock is
     * then kept in sb[] and written at the end of each operation (inside the
     * transaction when there is a journal) instead of being read, patched
     * and written back on every allocation. */
    int                 x4;
    int                 csum;        /* metadata_csum */
    int                 extents;     /* new files get extent trees */
    uint32_t            desc_size;   /* 32, or s_desc_size with 64bit */
    uint32_t            csum_seed;   /* crc32c(~0, uuid) or s_checksum_seed */
    uint32_t            groups;
    uint32_t            gdt_blocks;  /* group descriptor table, blocks */
    uint32_t            rsv_gdt;     /* s_reserved_gdt_blocks */
    uint16_t            extra_isize; /* i_extra_isize of new inodes */
    int                 sb_dirty;
    uint8_t             sb[1024];
    /* last extent mapping found, valid while xgen is unchanged and the
     * inode's i_block (the tree root) is the one it came from */
    uint32_t            xgen, xc_gen;
    uint32_t            xc_lblk, xc_len, xc_pblk;
    uint8_t             xc_root[60];
    struct ext2_jnl    *j;           /* the journal, when one is written */
    int                 crash_test;  /* mount -o x4crash, see ext2_test_crash */
    int                 crash_unlink; /* mount -o x4crashunlink */
    volatile int        lock;        /* ext2_lock: one changing operation at a time */
    int                 want_reclaim; /* ENOSPC with freed blocks awaiting commit */
    int                 no_step;     /* undoing: no step commits (ext2_jnl_due) */
    int                 nozero;      /* the block being allocated is about to be
                                      * written whole: no need to clear it */
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
static uint32_t ext2_write_node(vfs_node_t *node, uint32_t offset,
                                uint32_t size, const uint8_t *buf);
static const uint8_t *ext2_jt_find(ext2_fs_t *fs, uint32_t blk);
static void ext2_op_end(ext2_fs_t *fs);
static int ext2_jnl_reclaim(ext2_fs_t *fs);
static int ext2_jnl_commit(ext2_fs_t *fs);
static int ext2_jnl_due(ext2_fs_t *fs);
static void ext2_free_blocks_from(ext2_fs_t *fs, uint32_t ino, ext2_inode_t *inode,
                                  uint32_t from);
static void ext2_jnl_step(ext2_fs_t *fs);

/* Operations that change an ext3/ext4 instance run one at a time, and the
 * journal flusher runs between them: the running transaction is shared
 * state, and a commit must not catch an operation half done.  (A plain ext2
 * instance keeps the old, unlocked behaviour.) */
static int ext2_sleep_chan;
static void sleep_ticks(uint32_t ticks) {
    if (!current_proc) return;
    current_proc->wake_tick = pit_ticks() + (ticks ? ticks : 1);
    sleep_on(&ext2_sleep_chan);
}

static void ext2_lock(ext2_fs_t *fs) {
    if (!fs->x4) return;
    while (__sync_lock_test_and_set(&fs->lock, 1))
        sleep_ticks(1);
}
static void ext2_unlock(ext2_fs_t *fs) {
    if (!fs->x4) return;
    __sync_lock_release(&fs->lock);
}
static int ext2_jt_put(ext2_fs_t *fs, uint32_t blk, const void *buf);
static void ext2_jt_drop(ext2_fs_t *fs, uint32_t blk);

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
    const uint8_t *jd = fs->j ? ext2_jt_find(fs, blk) : (const uint8_t *)0;
    if (jd) {
        memcpy(buf, jd, fs->st.block_size);
        hit = 1;
    } else if (fs->cache_ready) {
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
    /* A block the running transaction changed: its new contents live in the
     * transaction until the commit writes them (the cache may have dropped
     * them, and the disk does not have them yet). */
    if (fs->j) {
        const uint8_t *jd = ext2_jt_find(fs, blk);
        if (jd) {
            memcpy(buf, jd, fs->st.block_size);
            preempt_enable();
            return 0;
        }
    }
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
    if (fs->j) {
        preempt_disable();
        const uint8_t *jd = ext2_jt_find(fs, blk);
        if (jd) memcpy(out, jd + off, len);
        preempt_enable();
        if (jd) return 0;
    }
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

/* Metadata: with a journal, into the running transaction (written to the
 * journal and then in place at the commit); otherwise straight to the disk. */
static int ext2_write_block(ext2_fs_t *fs, uint32_t blk, const void *buf) {
    if (fs->j) {
        if (fs->ro) return -1;
        if (ext2_jt_put(fs, blk, buf) < 0) return -1;
    } else if (ext2_raw_write_block(fs, blk, buf) < 0)
        return -1;

    preempt_disable();   /* same shared-cache hazard as ext2_read_block */
    ext2_cache_insert(fs, blk, buf);
    preempt_enable();
    return 0;
}

/* File data (and fresh zeroes): always straight to the disk.  In ordered
 * mode that is before the transaction that makes it reachable commits. */
static int ext2_write_data(ext2_fs_t *fs, uint32_t blk, const void *buf) {
    if (fs->j) ext2_jt_drop(fs, blk);
    if (ext2_raw_write_block(fs, blk, buf) < 0)
        return -1;
    preempt_disable();
    ext2_cache_insert(fs, blk, buf);
    preempt_enable();
    return 0;
}

/* ── ext4: checksums, uninitialised groups ───────────────────────────────────
 * Written from the on-disk format in the Linux kernel's
 * Documentation/filesystems/ext4/ (kernel.org, "Ext4 Disk Layout"); no code
 * was copied.  metadata_csum puts a crc32c (Castagnoli) of every metadata
 * structure into the structure or into its group descriptor, seeded with
 * crc32c(~0, uuid) (or s_checksum_seed), and with no final inversion. */
static uint32_t g_crc32c[256];
static int g_crc32c_ready;

static uint32_t crc32c(uint32_t crc, const void *data, uint32_t n) {
    if (!g_crc32c_ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c >> 1) ^ (0x82F63B78u & (0u - (c & 1u)));
            g_crc32c[i] = c;
        }
        g_crc32c_ready = 1;
    }
    const uint8_t *p = (const uint8_t *)data;
    while (n--)
        crc = g_crc32c[(crc ^ *p++) & 0xFFu] ^ (crc >> 8);
    return crc;
}

static inline uint16_t x4_rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t x4_rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
static inline void x4_wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void x4_wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* Superblock byte offsets used below. */
#define SB_FREE_BLOCKS   0x0C
#define SB_FREE_INODES   0x10
#define SB_MTIME         0x2C
#define SB_WTIME         0x30
#define SB_MNT_COUNT     0x34
#define SB_STATE         0x3A
#define SB_INCOMPAT      0x60
#define SB_UUID          0x68
#define SB_RSV_GDT       0xCE
#define SB_JOURNAL_INUM  0xE0
#define SB_DESC_SIZE     0xFE
#define SB_BLOCKS_HI     0x150
#define SB_FREE_HI       0x158
#define SB_WANT_EXTRA    0x15E
#define SB_CSUM_SEED     0x270
#define SB_CHECKSUM      0x3FC

/* The superblock in fs->sb, checksummed, straight to the disk (sectors 2-3
 * of the filesystem: only its own 1024 bytes). */
static int x4_sb_write(ext2_fs_t *fs) {
    if (fs->csum) x4_wr32(fs->sb + SB_CHECKSUM, crc32c(~0u, fs->sb, SB_CHECKSUM));
    fs->sb_dirty = 0;
    return ext2_dev_write(fs, fs->st.lba_offset + 2, 2, fs->sb);
}

/* Does group g carry a superblock backup (and so the descriptor table)? */
static int x4_has_super(ext2_fs_t *fs, uint32_t g) {
    if (g <= 1 || !(fs->ro_compat & 0x0001u)) return 1;     /* sparse_super */
    for (uint32_t b = 3; b <= 7; b += 2) {
        uint32_t x = g;
        while (x % b == 0) x /= b;
        if (x == 1) return 1;
    }
    return 0;
}

static uint16_t x4_gd_csum(ext2_fs_t *fs, uint32_t grp, const ext2_bgd_t *d) {
    uint8_t le[4];
    uint16_t zero = 0;
    x4_wr32(le, grp);
    uint32_t c = crc32c(fs->csum_seed, le, 4);
    c = crc32c(c, d, 0x1E);
    c = crc32c(c, &zero, 2);
    if (fs->desc_size > 0x20) c = crc32c(c, (const uint8_t *)d + 0x20, fs->desc_size - 0x20);
    return (uint16_t)c;
}

/* The checksum of a block or inode bitmap, into its descriptor. */
static void x4_bitmap_csum(ext2_fs_t *fs, ext2_bgd_t *d, int inode_bm, const uint8_t *bm) {
    if (!fs->csum) return;
    uint32_t n = inode_bm ? fs->st.inodes_per_group / 8 : fs->st.blocks_per_group / 8;
    uint32_t c = crc32c(fs->csum_seed, bm, n);
    if (inode_bm) {
        d->bg_inode_bitmap_csum = (uint16_t)c;
        if (fs->desc_size >= 64) d->bg_inode_bitmap_csum_hi = (uint16_t)(c >> 16);
    } else {
        d->bg_block_bitmap_csum = (uint16_t)c;
        if (fs->desc_size >= 64) d->bg_block_bitmap_csum_hi = (uint16_t)(c >> 16);
    }
}

/* Per-inode checksum seed: crc32c(fs seed, ino, generation). */
static uint32_t x4_iseed(ext2_fs_t *fs, uint32_t ino, uint32_t gen) {
    uint8_t le[4];
    x4_wr32(le, ino);
    uint32_t c = crc32c(fs->csum_seed, le, 4);
    x4_wr32(le, gen);
    return crc32c(c, le, 4);
}

/* Checksum the raw on-disk inode at `raw` (fs inode size) in place. */
static void x4_inode_csum(ext2_fs_t *fs, uint32_t ino, uint8_t *raw) {
    if (!fs->csum) return;
    uint32_t isz = fs->st.inode_size;
    uint16_t zero = 0;
    uint32_t c = x4_iseed(fs, ino, x4_rd32(raw + 0x64));
    c = crc32c(c, raw, 0x7C);
    c = crc32c(c, &zero, 2);
    c = crc32c(c, raw + 0x7E, 128 - 0x7E);
    int hi = 0;
    if (isz > 128) {
        uint32_t extra = x4_rd16(raw + 0x80);
        c = crc32c(c, raw + 128, 2);                 /* i_extra_isize */
        uint32_t off = 0x82;
        if (extra >= 4) { c = crc32c(c, &zero, 2); off += 2; hi = 1; }
        c = crc32c(c, raw + off, isz - off);
    }
    x4_wr16(raw + 0x7C, (uint16_t)c);
    if (hi) x4_wr16(raw + 0x82, (uint16_t)(c >> 16));
}

/* Directory leaf blocks end in a 12-byte fake entry holding their checksum. */
#define X4_DIR_TAIL 12u
static uint32_t ext2_dir_end(ext2_fs_t *fs) {
    return fs->st.block_size - (fs->csum ? X4_DIR_TAIL : 0u);
}

static void x4_dir_tail(ext2_fs_t *fs, uint32_t ino, uint32_t gen, uint8_t *blk) {
    if (!fs->csum) return;
    uint8_t *t = blk + fs->st.block_size - X4_DIR_TAIL;
    x4_wr32(t, 0);
    x4_wr16(t + 4, X4_DIR_TAIL);
    t[6] = 0;
    t[7] = 0xDE;
    x4_wr32(t + 8, crc32c(x4_iseed(fs, ino, gen), blk, fs->st.block_size - X4_DIR_TAIL));
}

/* Extent tree blocks: the checksum follows eh_max entries. */
static void x4_ext_tail(ext2_fs_t *fs, uint32_t ino, uint32_t gen, uint8_t *blk) {
    if (!fs->csum) return;
    uint32_t off = 12u + 12u * x4_rd16(blk + 4);
    if (off + 4 > fs->st.block_size) return;
    x4_wr32(blk + off, crc32c(x4_iseed(fs, ino, gen), blk, off));
}

static int ext2_update_super_free_counts(ext2_fs_t *fs, int block_delta, int inode_delta) {
    if (fs->x4) {
        uint32_t b = x4_rd32(fs->sb + SB_FREE_BLOCKS), i = x4_rd32(fs->sb + SB_FREE_INODES);
        x4_wr32(fs->sb + SB_FREE_BLOCKS, b + (uint32_t)block_delta);
        x4_wr32(fs->sb + SB_FREE_INODES, i + (uint32_t)inode_delta);
        fs->sb_dirty = 1;
        return 0;
    }
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
    uint32_t ds        = fs->desc_size ? fs->desc_size : 32u;
    uint32_t per_block = fs->st.block_size / ds;
    uint32_t bgd_blk   = fs->st.first_data_block + 1 + (per_block ? grp / per_block : 0);
    uint32_t idx       = per_block ? grp % per_block : grp;
    memset(out, 0, sizeof(*out));
    return ext2_read_block_part(fs, bgd_blk, idx * ds, ds < sizeof(*out) ? ds : sizeof(*out),
                                out);
}

static int ext2_write_bgd(ext2_fs_t *fs, uint32_t grp, const ext2_bgd_t *in) {
    /* Same multi-block bgd-table handling as ext2_read_bgd. */
    uint32_t ds        = fs->desc_size ? fs->desc_size : 32u;
    uint32_t per_block = fs->st.block_size / ds;
    uint32_t bgd_blk   = fs->st.first_data_block + 1 + (per_block ? grp / per_block : 0);
    uint32_t idx       = per_block ? grp % per_block : grp;
    uint8_t *bgd_block = (uint8_t *)kmalloc(fs->st.block_size);
    if (!bgd_block) return -1;
    if (ext2_read_block(fs, bgd_blk, bgd_block) < 0) {
        kfree(bgd_block);
        return -1;
    }
    ext2_bgd_t d = *in;
    if (fs->csum) d.bg_checksum = x4_gd_csum(fs, grp, &d);
    memcpy(bgd_block + idx * ds, &d, ds < sizeof(d) ? ds : sizeof(d));
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

/* `fresh`: a newly allocated inode, whose slot may hold anything (a deleted
 * inode's leftovers, or nothing initialised at all): the part past the first
 * 128 bytes is cleared and given i_extra_isize. */
static int ext2_write_inode_ex(ext2_fs_t *fs, uint32_t ino, const ext2_inode_t *in,
                               int fresh) {
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
    if (fresh && fs->st.inode_size > 128) {
        memset(blk_buf + off + 128, 0, fs->st.inode_size - 128);
        if (fs->x4) {
            x4_wr16(blk_buf + off + 0x80, fs->extra_isize);
            if (fs->extra_isize >= 0x18)                       /* i_crtime */
                x4_wr32(blk_buf + off + 0x90, in->i_ctime);
        }
    }
    x4_inode_csum(fs, ino, blk_buf + off);
    int r = ext2_write_block(fs, blk, blk_buf);
    kfree(blk_buf);
    return r;
}

static int ext2_write_inode(ext2_fs_t *fs, uint32_t ino, const ext2_inode_t *in) {
    return ext2_write_inode_ex(fs, ino, in, 0);
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

/* An uninitialised block bitmap (BLOCK_UNINIT) is implied by the layout:
 * the superblock backup and descriptor table of the group, if it has them,
 * and any group's bitmaps and inode table that lie in it (with flex_bg those
 * of other groups too).  Built here, then checked against the descriptor's
 * free count; a group whose implied bitmap does not agree with it is not
 * allocated from (-1). */
static int x4_init_block_bitmap(ext2_fs_t *fs, uint32_t grp, const ext2_bgd_t *d,
                                uint8_t *bm) {
    uint32_t bs = fs->st.block_size, bpg = fs->st.blocks_per_group;
    uint32_t start = fs->st.first_data_block + grp * bpg;
    uint32_t nblk = fs->st.blocks_count - start;
    if (nblk > bpg) nblk = bpg;
    uint32_t itb = (fs->st.inodes_per_group * fs->st.inode_size + bs - 1) / bs;
    memset(bm, 0, bs);
    if (x4_has_super(fs, grp))
        for (uint32_t i = 0; i < 1 + fs->gdt_blocks + fs->rsv_gdt && i < nblk; i++)
            ext2_bitmap_set(bm, i);
    for (uint32_t g = 0; g < fs->groups; g++) {
        ext2_bgd_t o;
        if (g == grp) o = *d;
        else if (ext2_read_bgd(fs, g, &o) < 0) return -1;
        uint32_t m[3] = { o.bg_block_bitmap, o.bg_inode_bitmap, o.bg_inode_table };
        uint32_t len[3] = { 1, 1, itb };
        for (int k = 0; k < 3; k++)
            for (uint32_t b = m[k]; b < m[k] + len[k]; b++)
                if (b >= start && b - start < nblk) ext2_bitmap_set(bm, b - start);
    }
    for (uint32_t i = nblk; i < bs * 8; i++) ext2_bitmap_set(bm, i);
    uint32_t nfree = 0;
    for (uint32_t i = 0; i < nblk; i++) if (!ext2_bitmap_test(bm, i)) nfree++;
    if (nfree != d->bg_free_blocks_count) {
        printk("[EXT2]  %s: group %u: uninitialised block bitmap would have %u free, "
               "descriptor says %u; not allocating there\n", fs->name, (unsigned)grp,
               (unsigned)nfree, (unsigned)d->bg_free_blocks_count);
        return -1;
    }
    return 0;
}

/* Group `grp`'s block or inode bitmap into `bm` (one block). */
static int ext2_get_bitmap(ext2_fs_t *fs, uint32_t grp, const ext2_bgd_t *d, int inode_bm,
                           uint8_t *bm) {
    if (fs->x4 && inode_bm && (d->bg_flags & EXT4_BG_INODE_UNINIT)) {
        memset(bm, 0, fs->st.block_size);
        for (uint32_t i = fs->st.inodes_per_group; i < fs->st.block_size * 8; i++)
            ext2_bitmap_set(bm, i);
        return 0;
    }
    if (fs->x4 && !inode_bm && (d->bg_flags & EXT4_BG_BLOCK_UNINIT))
        return x4_init_block_bitmap(fs, grp, d, bm);
    return ext2_read_block(fs, inode_bm ? d->bg_inode_bitmap : d->bg_block_bitmap, bm);
}

/* Write a bitmap back; the caller then writes the descriptor, which now holds
 * its checksum and no longer says it is uninitialised. */
static int ext2_put_bitmap(ext2_fs_t *fs, ext2_bgd_t *d, int inode_bm, const uint8_t *bm) {
    if (ext2_write_block(fs, inode_bm ? d->bg_inode_bitmap : d->bg_block_bitmap, bm) < 0)
        return -1;
    if (fs->x4) {
        d->bg_flags &= (uint16_t)~(inode_bm ? EXT4_BG_INODE_UNINIT : EXT4_BG_BLOCK_UNINIT);
        x4_bitmap_csum(fs, d, inode_bm, bm);
    }
    return 0;
}

static int ext2_jt_freed(ext2_fs_t *fs, uint32_t blk);
static void ext2_jt_freed_add(ext2_fs_t *fs, uint32_t blk, uint32_t n);

static uint32_t ext2_alloc_inode(ext2_fs_t *fs) {
    uint32_t groups = ext2_group_count(fs);
    uint8_t *bitmap = (uint8_t *)kmalloc(fs->st.block_size);
    if (!bitmap) return 0;

    for (uint32_t grp = 0; grp < groups; grp++) {
        ext2_bgd_t bgd;
        if (ext2_read_bgd(fs, grp, &bgd) < 0) continue;
        if (fs->x4 && !bgd.bg_free_inodes_count) continue;
        if (ext2_get_bitmap(fs, grp, &bgd, 1, bitmap) < 0) continue;

        for (uint32_t i = 0; i < fs->st.inodes_per_group; i++) {
            uint32_t ino = grp * fs->st.inodes_per_group + i + 1;
            if (ino < fs->st.first_ino || ino > fs->st.inodes_count) continue;
            if (ext2_bitmap_test(bitmap, i)) continue;

            ext2_bitmap_set(bitmap, i);
            if (ext2_put_bitmap(fs, &bgd, 1, bitmap) < 0) {
                kfree(bitmap);
                return 0;
            }
            if (bgd.bg_free_inodes_count) bgd.bg_free_inodes_count--;
            /* With group checksums, inodes past ipg - bg_itable_unused are
             * never looked at (e2fsck, Linux): move the mark past this one. */
            if (fs->csum && i >= fs->st.inodes_per_group - bgd.bg_itable_unused)
                bgd.bg_itable_unused = (uint16_t)(fs->st.inodes_per_group - i - 1);
            ext2_write_bgd(fs, grp, &bgd);
            ext2_update_super_free_counts(fs, 0, -1);
            kfree(bitmap);
            return ino;
        }
    }

    kfree(bitmap);
    return 0;
}

/* A free block, searched from `goal` onward (wrapping), so a file's blocks
 * follow each other on the disk.  `zero`: clear it on the disk too. */
static uint32_t ext2_alloc_block_goal_once(ext2_fs_t *fs, uint32_t goal, int zero);
static uint32_t ext2_alloc_block_goal(ext2_fs_t *fs, uint32_t goal, int zero) {
    uint32_t b = ext2_alloc_block_goal_once(fs, goal, zero);
    /* Full, apart from blocks the running transaction freed: they become
     * free once it commits, which can only happen between operations (a
     * commit here would make half of this one durable).  write(2) commits
     * and retries (ext2_write_node); anything else gets ENOSPC, like an
     * ext4 that runs out of retries. */
    if (!b && fs->j && ext2_jnl_reclaim(fs)) fs->want_reclaim = 1;
    return b;
}

static uint32_t ext2_alloc_block_goal_once(ext2_fs_t *fs, uint32_t goal, int zero) {
    uint32_t groups = ext2_group_count(fs);
    uint32_t bpg = fs->st.blocks_per_group;
    uint32_t sg = 0, sbit = 0;
    if (goal >= fs->st.first_data_block && goal < fs->st.blocks_count) {
        sg = (goal - fs->st.first_data_block) / bpg;
        sbit = (goal - fs->st.first_data_block) % bpg;
    }
    uint8_t *bitmap = (uint8_t *)kmalloc(fs->st.block_size);
    if (!bitmap) return 0;

    for (uint32_t k = 0; k <= groups; k++) {
        uint32_t grp = (sg + k) % groups;
        uint32_t from = (k == 0) ? sbit : 0;
        uint32_t to = (k == groups) ? sbit : bpg;    /* the goal group's head, last */
        if (from >= to) continue;
        ext2_bgd_t bgd;
        if (ext2_read_bgd(fs, grp, &bgd) < 0) continue;
        if (fs->x4 && !bgd.bg_free_blocks_count) continue;
        if (ext2_get_bitmap(fs, grp, &bgd, 0, bitmap) < 0) continue;

        for (uint32_t i = from; i < to; i++) {
            if (!(i & 7u) && i + 8 <= to && bitmap[i / 8] == 0xFF) { i += 7; continue; }
            uint32_t blk = fs->st.first_data_block + grp * bpg + i;
            if (blk >= fs->st.blocks_count) break;
            if (ext2_bitmap_test(bitmap, i)) continue;
            /* Freed by the running transaction: on the disk it may still be
             * what the last committed state points at. */
            if (fs->j && ext2_jt_freed(fs, blk)) continue;

            ext2_bitmap_set(bitmap, i);
            if (ext2_put_bitmap(fs, &bgd, 0, bitmap) < 0) {
                kfree(bitmap);
                return 0;
            }
            if (bgd.bg_free_blocks_count) bgd.bg_free_blocks_count--;
            ext2_write_bgd(fs, grp, &bgd);
            ext2_update_super_free_counts(fs, -1, 0);
            kfree(bitmap);
            if (zero) {
                uint8_t *z = (uint8_t *)kmalloc(fs->st.block_size);
                if (!z || (memset(z, 0, fs->st.block_size), ext2_write_data(fs, blk, z) < 0)) {
                    if (z) kfree(z);
                    ext2_free_block(fs, blk);
                    return 0;
                }
                kfree(z);
            }
            return blk;
        }
    }

    kfree(bitmap);
    return 0;
}

static uint32_t ext2_alloc_block(ext2_fs_t *fs) {
    return ext2_alloc_block_goal(fs, 0, 1);
}

static int ext2_free_inode(ext2_fs_t *fs, uint32_t ino) {
    if (ino < fs->st.first_ino || ino > fs->st.inodes_count) return -1;
    uint32_t grp = (ino - 1) / fs->st.inodes_per_group;
    uint32_t idx = (ino - 1) % fs->st.inodes_per_group;

    ext2_bgd_t bgd;
    if (ext2_read_bgd(fs, grp, &bgd) < 0) return -1;

    uint8_t *bitmap = (uint8_t *)kmalloc(fs->st.block_size);
    if (!bitmap) return -1;
    if (ext2_get_bitmap(fs, grp, &bgd, 1, bitmap) < 0) {
        kfree(bitmap);
        return -1;
    }
    if (ext2_bitmap_test(bitmap, idx)) {
        ext2_bitmap_clear(bitmap, idx);
        if (ext2_put_bitmap(fs, &bgd, 1, bitmap) < 0) {
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

/* Free `n` consecutive blocks from `blk`, one bitmap update per group. */
static int ext2_free_range(ext2_fs_t *fs, uint32_t blk, uint32_t n) {
    uint8_t *bitmap = (uint8_t *)kmalloc(fs->st.block_size);
    if (!bitmap) return -1;
    int rc = 0;
    while (n) {
        if (blk < fs->st.first_data_block || blk >= fs->st.blocks_count) { rc = -1; break; }
        uint32_t grp = (blk - fs->st.first_data_block) / fs->st.blocks_per_group;
        uint32_t idx = (blk - fs->st.first_data_block) % fs->st.blocks_per_group;
        uint32_t chunk = fs->st.blocks_per_group - idx;
        if (chunk > n) chunk = n;
        if (chunk > fs->st.blocks_count - blk) chunk = fs->st.blocks_count - blk;

        ext2_bgd_t bgd;
        if (ext2_read_bgd(fs, grp, &bgd) < 0 ||
            ext2_get_bitmap(fs, grp, &bgd, 0, bitmap) < 0) { rc = -1; break; }
        uint32_t cleared = 0;
        for (uint32_t k = 0; k < chunk; k++)
            if (ext2_bitmap_test(bitmap, idx + k)) {
                ext2_bitmap_clear(bitmap, idx + k);
                cleared++;
            }
        if (cleared) {
            if (ext2_put_bitmap(fs, &bgd, 0, bitmap) < 0) { rc = -1; break; }
            bgd.bg_free_blocks_count = (uint16_t)(bgd.bg_free_blocks_count + cleared);
            ext2_write_bgd(fs, grp, &bgd);
            ext2_update_super_free_counts(fs, (int)cleared, 0);
        }
        if (fs->j) ext2_jt_freed_add(fs, blk, chunk);
        blk += chunk;
        n -= chunk;
    }
    kfree(bitmap);
    return rc;
}

static int ext2_free_block(ext2_fs_t *fs, uint32_t blk) {
    return ext2_free_range(fs, blk, 1);
}

/* Live free-space numbers for statfs().  The superblock counters are updated by
 * ext2_alloc_block/ext2_free_block (via ext2_update_super_free_counts), so what
 * df reports is what dumpe2fs reports. */
int ext2_statfs_fs(ext2_fs_t *fs, uint32_t *block_size, uint32_t *blocks,
                   uint32_t *bfree, uint32_t *inodes, uint32_t *ifree) {
    if (!fs) return -1;
    uint8_t sb_buf[2048];
    if (fs->x4) memcpy(sb_buf, fs->sb, 1024);
    else if (ext2_dev_read(fs, fs->st.lba_offset + 2, 4, sb_buf) < 0) return -1;
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

/* ── ext4 extent trees ───────────────────────────────────────────────────────
 * i_block holds a 12-byte header and four 12-byte entries; deeper levels are
 * whole blocks (header, eh_max entries, then a crc32c tail with
 * metadata_csum).  Index entries point at the next level down, leaf entries
 * map up to 32768 blocks each (a length above that marks an unwritten extent,
 * which reads as zeroes).
 *
 * Lookups walk the tree.  Appending one block right after the last extent,
 * contiguous on the disk -- nearly every write -- just lengthens that extent
 * in place.  Anything else (a hole filled, an unwritten block written, a
 * truncate) collects the whole extent list, edits it and lays the tree out
 * again in as few levels as it needs, reusing the old tree blocks first. */
#define EXT4_EXTENTS_FL  0x00080000u
#define EXT4_EXT_MAGIC   0xF30Au
#define EXT4_EXT_MAX_LEN 32768u

typedef struct { uint32_t lblk, len, pblk; int unwritten; } x4_ext_t;

static int x4_is_ext(const ext2_inode_t *ino) {
    return (ino->i_flags & EXT4_EXTENTS_FL) != 0;
}

static void x4_ext_get(const uint8_t *e, x4_ext_t *x) {
    uint32_t len = x4_rd16(e + 4);
    x->lblk = x4_rd32(e);
    x->unwritten = len > EXT4_EXT_MAX_LEN;
    x->len = x->unwritten ? len - EXT4_EXT_MAX_LEN : len;
    x->pblk = x4_rd16(e + 6) ? 0u : x4_rd32(e + 8);     /* above 2^32: not ours */
}

static void x4_ext_put(uint8_t *e, const x4_ext_t *x) {
    x4_wr32(e, x->lblk);
    x4_wr16(e + 4, (uint16_t)(x->unwritten ? x->len + EXT4_EXT_MAX_LEN : x->len));
    x4_wr16(e + 6, 0);
    x4_wr32(e + 8, x->pblk);
}

static void x4_hdr_put(uint8_t *h, uint32_t entries, uint32_t max, uint32_t depth) {
    x4_wr16(h, EXT4_EXT_MAGIC);
    x4_wr16(h + 2, (uint16_t)entries);
    x4_wr16(h + 4, (uint16_t)max);
    x4_wr16(h + 6, (uint16_t)depth);
    x4_wr32(h + 8, 0);
}

static int x4_hdr_ok(const uint8_t *h, uint32_t room) {
    return x4_rd16(h) == EXT4_EXT_MAGIC && x4_rd16(h + 2) <= x4_rd16(h + 4) &&
           12u + 12u * x4_rd16(h + 4) <= room && x4_rd16(h + 6) <= 5;
}

/* The physical block of file block `lblk`, 0 for a hole or an unwritten
 * extent (*unwritten then says which; it may be NULL). */
static uint32_t x4_ext_map(ext2_fs_t *fs, const ext2_inode_t *ino, uint32_t lblk,
                           int *unwritten) {
    if (unwritten) *unwritten = 0;
    preempt_disable();
    if (fs->xc_gen == fs->xgen && fs->xc_len && lblk >= fs->xc_lblk &&
        lblk - fs->xc_lblk < fs->xc_len &&
        memcmp(fs->xc_root, ino->i_block, 60) == 0) {
        uint32_t p = fs->xc_pblk + (lblk - fs->xc_lblk);
        preempt_enable();
        return p;
    }
    preempt_enable();

    const uint8_t *node = (const uint8_t *)ino->i_block;
    uint32_t room = 60;
    uint8_t *buf = (uint8_t *)0;
    uint32_t res = 0;
    uint32_t gen = fs->xgen;
    for (int lvl = 0; lvl < 6; lvl++) {
        if (!x4_hdr_ok(node, room)) break;
        uint32_t n = x4_rd16(node + 2), depth = x4_rd16(node + 6);
        if (depth == 0) {
            for (uint32_t i = 0; i < n; i++) {
                x4_ext_t x;
                x4_ext_get(node + 12 + 12 * i, &x);
                if (lblk < x.lblk || lblk - x.lblk >= x.len) continue;
                if (x.unwritten) {
                    if (unwritten) *unwritten = 1;
                    break;
                }
                if (!x.pblk) break;
                res = x.pblk + (lblk - x.lblk);
                preempt_disable();
                if (gen == fs->xgen) {
                    fs->xc_gen = gen;
                    fs->xc_lblk = x.lblk;
                    fs->xc_len = x.len;
                    fs->xc_pblk = x.pblk;
                    memcpy(fs->xc_root, ino->i_block, 60);
                }
                preempt_enable();
                break;
            }
            break;
        }
        int k = -1;
        for (uint32_t i = 0; i < n; i++) {
            if (x4_rd32(node + 12 + 12 * i) <= lblk) k = (int)i;
            else break;
        }
        if (k < 0) break;
        const uint8_t *ix = node + 12 + 12 * (uint32_t)k;
        if (x4_rd16(ix + 8)) break;
        uint32_t child = x4_rd32(ix + 4);
        if (!buf && !(buf = (uint8_t *)kmalloc(fs->st.block_size))) break;
        if (ext2_read_block(fs, child, buf) < 0) break;
        node = buf;
        room = fs->st.block_size;
    }
    if (buf) kfree(buf);
    return res;
}

/* Every extent of the tree under `node` (depth from its header) appended to
 * *v, and every tree block below it to *tb. */
typedef struct {
    x4_ext_t *v;  uint32_t n, cap;
    uint32_t *tb; uint32_t ntb, tbcap;
} x4_list_t;

static int x4_list_add(x4_list_t *l, const x4_ext_t *x) {
    if (l->n == l->cap) {
        uint32_t nc = l->cap ? l->cap * 2 : 16;
        x4_ext_t *nv = (x4_ext_t *)kmalloc(nc * sizeof(*nv));
        if (!nv) return -1;
        if (l->v) { memcpy(nv, l->v, l->n * sizeof(*nv)); kfree(l->v); }
        l->v = nv;
        l->cap = nc;
    }
    l->v[l->n++] = *x;
    return 0;
}

static int x4_list_tb(x4_list_t *l, uint32_t b) {
    if (l->ntb == l->tbcap) {
        uint32_t nc = l->tbcap ? l->tbcap * 2 : 8;
        uint32_t *nt = (uint32_t *)kmalloc(nc * sizeof(*nt));
        if (!nt) return -1;
        if (l->tb) { memcpy(nt, l->tb, l->ntb * sizeof(*nt)); kfree(l->tb); }
        l->tb = nt;
        l->tbcap = nc;
    }
    l->tb[l->ntb++] = b;
    return 0;
}

static void x4_list_free(x4_list_t *l) {
    if (l->v) kfree(l->v);
    if (l->tb) kfree(l->tb);
    memset(l, 0, sizeof(*l));
}

static int x4_collect(ext2_fs_t *fs, const uint8_t *node, uint32_t room, int lvl,
                      x4_list_t *l) {
    if (lvl > 5 || !x4_hdr_ok(node, room)) return -1;
    uint32_t n = x4_rd16(node + 2), depth = x4_rd16(node + 6);
    if (depth == 0) {
        for (uint32_t i = 0; i < n; i++) {
            x4_ext_t x;
            x4_ext_get(node + 12 + 12 * i, &x);
            if (!x.len) continue;
            if (!x.pblk || x4_list_add(l, &x) < 0) return -1;
        }
        return 0;
    }
    uint8_t *buf = (uint8_t *)kmalloc(fs->st.block_size);
    if (!buf) return -1;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *ix = node + 12 + 12 * i;
        uint32_t child = x4_rd32(ix + 4);
        if (x4_rd16(ix + 8) || x4_list_tb(l, child) < 0 ||
            ext2_read_block(fs, child, buf) < 0 ||
            x4_collect(fs, buf, fs->st.block_size, lvl + 1, l) < 0) {
            kfree(buf);
            return -1;
        }
    }
    kfree(buf);
    return 0;
}

/* Lay out `l->v` (sorted, non-overlapping) as the inode's extent tree.  The
 * tree blocks in `l->tb` are reused first; what is left over is freed, what
 * is missing is allocated near `goal`.  i_blocks follows the tree blocks; the
 * caller writes the inode. */
static int x4_rebuild(ext2_fs_t *fs, uint32_t ino, ext2_inode_t *inode, x4_list_t *l,
                      uint32_t goal) {
    uint32_t bs = fs->st.block_size, per = (bs - 12) / 12;
    uint32_t n = l->n, depth, nleaf = 0, nidx = 0;
    if (n <= 4) depth = 0;
    else {
        nleaf = (n + per - 1) / per;
        if (nleaf <= 4) depth = 1;
        else {
            nidx = (nleaf + per - 1) / per;
            if (nidx > 4) return -1;            /* 4 * 340 * 340 extents: no */
            depth = 2;
        }
    }
    uint32_t need = nleaf + nidx;
    uint32_t *blks = (uint32_t *)0;
    if (need) {
        blks = (uint32_t *)kmalloc(need * sizeof(uint32_t));
        if (!blks) return -1;
        for (uint32_t i = 0; i < need; i++) {
            if (i < l->ntb) { blks[i] = l->tb[i]; continue; }
            blks[i] = ext2_alloc_block_goal(fs, goal, 0);
            if (!blks[i]) {
                for (uint32_t k = l->ntb; k < i; k++) ext2_free_block(fs, blks[k]);
                kfree(blks);
                return -1;
            }
            inode->i_blocks += fs->st.sectors_per_block;
        }
    }
    for (uint32_t i = need; i < l->ntb; i++) {
        ext2_free_block(fs, l->tb[i]);
        if (inode->i_blocks >= fs->st.sectors_per_block)
            inode->i_blocks -= fs->st.sectors_per_block;
    }

    uint8_t *root = (uint8_t *)inode->i_block;
    memset(root, 0, 60);
    int rc = 0;
    if (depth == 0) {
        x4_hdr_put(root, n, 4, 0);
        for (uint32_t i = 0; i < n; i++) x4_ext_put(root + 12 + 12 * i, &l->v[i]);
    } else {
        uint8_t *buf = (uint8_t *)kmalloc(bs);
        if (!buf) { kfree(blks); return -1; }
        /* leaves: blks[0 .. nleaf) */
        for (uint32_t j = 0; j < nleaf && rc == 0; j++) {
            uint32_t a = j * per, cnt = n - a < per ? n - a : per;
            memset(buf, 0, bs);
            x4_hdr_put(buf, cnt, per, 0);
            for (uint32_t i = 0; i < cnt; i++) x4_ext_put(buf + 12 + 12 * i, &l->v[a + i]);
            x4_ext_tail(fs, ino, inode->i_generation, buf);
            if (ext2_write_block(fs, blks[j], buf) < 0) rc = -1;
        }
        /* the level above the leaves: the root itself, or nidx blocks */
        uint32_t nup = depth == 1 ? 1 : nidx;
        for (uint32_t u = 0; u < nup && rc == 0; u++) {
            uint32_t a = u * per, cnt = nleaf - a < per ? nleaf - a : per;
            uint8_t *node = depth == 1 ? root : buf;
            if (depth == 2) {
                memset(buf, 0, bs);
                x4_hdr_put(buf, cnt, per, 1);
            } else {
                cnt = nleaf;
                x4_hdr_put(root, cnt, 4, 1);
            }
            for (uint32_t i = 0; i < cnt; i++) {
                uint8_t *ix = node + 12 + 12 * i;
                x4_wr32(ix, l->v[(a + i) * per].lblk);
                x4_wr32(ix + 4, blks[a + i]);
            }
            if (depth == 2) {
                x4_ext_tail(fs, ino, inode->i_generation, buf);
                if (ext2_write_block(fs, blks[nleaf + u], buf) < 0) rc = -1;
            }
        }
        if (depth == 2) {
            x4_hdr_put(root, nidx, 4, 2);
            for (uint32_t u = 0; u < nidx; u++) {
                uint8_t *ix = root + 12 + 12 * u;
                x4_wr32(ix, l->v[u * per * per].lblk);
                x4_wr32(ix + 4, blks[nleaf + u]);
            }
        }
        kfree(buf);
    }
    if (blks) kfree(blks);
    fs->xgen++;
    return rc;
}

/* Sort by logical block and merge neighbours that continue each other. */
static void x4_normalise(x4_list_t *l) {
    for (uint32_t i = 1; i < l->n; i++) {            /* insertion sort: nearly sorted */
        x4_ext_t t = l->v[i];
        uint32_t j = i;
        while (j && l->v[j - 1].lblk > t.lblk) { l->v[j] = l->v[j - 1]; j--; }
        l->v[j] = t;
    }
    uint32_t o = 0;
    for (uint32_t i = 0; i < l->n; i++) {
        x4_ext_t *p = o ? &l->v[o - 1] : (x4_ext_t *)0, *c = &l->v[i];
        uint32_t lim = c->unwritten ? EXT4_EXT_MAX_LEN - 1 : EXT4_EXT_MAX_LEN;
        if (p && p->unwritten == c->unwritten && p->lblk + p->len == c->lblk &&
            p->pblk + p->len == c->pblk && p->len + c->len <= lim) {
            p->len += c->len;
            continue;
        }
        l->v[o++] = *c;
    }
    l->n = o;
}

/* The block for file block `lblk`, allocating (and mapping) it if needed. */
static uint32_t x4_ext_alloc(ext2_fs_t *fs, uint32_t ino, ext2_inode_t *inode,
                             uint32_t lblk) {
    int unwritten = 0;
    uint32_t p = x4_ext_map(fs, inode, lblk, &unwritten);
    if (p) return p;

    uint8_t *root = (uint8_t *)inode->i_block;
    if (!x4_hdr_ok(root, 60)) return 0;

    /* The rightmost extent, and the leaf block holding it (0: the inode). */
    uint8_t *buf = (uint8_t *)kmalloc(fs->st.block_size);
    if (!buf) return 0;
    uint8_t *node = root;
    uint32_t leaf = 0;
    for (int lvl = 0; lvl < 6 && x4_rd16(node + 6) > 0; lvl++) {
        uint32_t n = x4_rd16(node + 2);
        if (!n) break;
        const uint8_t *ix = node + 12 + 12 * (n - 1);
        leaf = x4_rd32(ix + 4);
        if (ext2_read_block(fs, leaf, buf) < 0 || !x4_hdr_ok(buf, fs->st.block_size)) {
            kfree(buf);
            return 0;
        }
        node = buf;
    }
    x4_ext_t last;
    int have_last = 0;
    if (x4_rd16(node + 6) == 0 && x4_rd16(node + 2) > 0) {
        x4_ext_get(node + 12 + 12 * (x4_rd16(node + 2) - 1u), &last);
        have_last = last.pblk != 0;
    }

    uint32_t nb = 0;
    if (!unwritten) {
        uint32_t goal = have_last ? last.pblk + last.len + (lblk - (last.lblk + last.len)) : 0;
        if (have_last && lblk < last.lblk + last.len) goal = last.pblk;
        nb = ext2_alloc_block_goal(fs, goal, !fs->nozero);
        if (!nb) { kfree(buf); return 0; }
        inode->i_blocks += fs->st.sectors_per_block;
        /* Fast path: the block right after the last extent, on the disk too. */
        if (have_last && !last.unwritten && lblk == last.lblk + last.len &&
            nb == last.pblk + last.len && last.len < EXT4_EXT_MAX_LEN) {
            last.len++;
            x4_ext_put(node + 12 + 12 * (x4_rd16(node + 2) - 1u), &last);
            int rc = 0;
            if (leaf) {
                x4_ext_tail(fs, ino, inode->i_generation, buf);
                rc = ext2_write_block(fs, leaf, buf);
            }
            fs->xgen++;
            kfree(buf);
            if (rc < 0) return 0;
            return nb;
        }
    }
    kfree(buf);

    /* General path: edit the whole list. */
    x4_list_t l;
    memset(&l, 0, sizeof(l));
    if (x4_collect(fs, root, 60, 0, &l) < 0) goto fail;
    if (unwritten) {
        /* Split the unwritten extent around lblk; that block becomes written
         * (and zeroed: an unwritten block's contents are undefined). */
        for (uint32_t i = 0; i < l.n; i++) {
            x4_ext_t x = l.v[i];
            if (!x.unwritten || lblk < x.lblk || lblk - x.lblk >= x.len) continue;
            nb = x.pblk + (lblk - x.lblk);
            uint32_t before = lblk - x.lblk, after = x.len - before - 1;
            x4_ext_t mid = { lblk, 1, nb, 0 };
            if (before) {
                l.v[i].len = before;
                if (x4_list_add(&l, &mid) < 0) goto fail;
            } else {
                l.v[i] = mid;
            }
            if (after) {
                x4_ext_t tail = { lblk + 1, after, nb + 1, 1 };
                if (x4_list_add(&l, &tail) < 0) goto fail;
            }
            break;
        }
        if (!nb) goto fail;
        uint8_t *z = (uint8_t *)kmalloc(fs->st.block_size);
        if (!z) goto fail;
        memset(z, 0, fs->st.block_size);
        ext2_write_data(fs, nb, z);
        kfree(z);
    } else {
        x4_ext_t x = { lblk, 1, nb, 0 };
        if (x4_list_add(&l, &x) < 0) goto fail;
    }
    x4_normalise(&l);
    if (x4_rebuild(fs, ino, inode, &l, nb + 1) < 0) goto fail;
    x4_list_free(&l);
    return nb;

fail:
    x4_list_free(&l);
    if (nb && !unwritten) {
        ext2_free_block(fs, nb);
        inode->i_blocks -= fs->st.sectors_per_block;
    }
    return 0;
}

/* Release every block from file block `from` onward (unwritten ones too) and
 * the tree blocks that are no longer needed.  Returns the data blocks freed,
 * or -1 if the tree could not be read (then nothing was changed).
 *
 * Freed from the end backwards.  With a journal, once the transaction is big
 * enough, the tree is cut back to what is still kept, the inode written and
 * the step committed before going on (Linux's truncate restart): every
 * commit then holds a tree that references no block the same commit marks
 * free, so a crash in the middle leaves a shorter file, never a block that is
 * both free and in use. */
static int x4_ext_free_from(ext2_fs_t *fs, uint32_t ino, ext2_inode_t *inode, uint32_t from) {
    uint32_t freed = 0;
    for (int round = 0; ; round++) {
        x4_list_t l;
        memset(&l, 0, sizeof(l));
        if (x4_collect(fs, (const uint8_t *)inode->i_block, 60, 0, &l) < 0) {
            x4_list_free(&l);
            return round ? (int)freed : -1;
        }
        uint32_t i = l.n, now = 0;
        int stepped = 0;
        while (i > 0) {
            x4_ext_t *x = &l.v[i - 1];
            if (x->lblk + x->len <= from) break;
            if (x->lblk >= from) {
                ext2_free_range(fs, x->pblk, x->len);
                now += x->len;
                i--;
            } else {
                uint32_t keep = from - x->lblk;
                ext2_free_range(fs, x->pblk + keep, x->len - keep);
                now += x->len - keep;
                x->len = keep;
                break;
            }
            if (i > 0 && l.v[i - 1].lblk + l.v[i - 1].len > from && ext2_jnl_due(fs)) {
                stepped = 1;
                break;
            }
        }
        l.n = i;
        freed += now;
        uint32_t sectors = now * fs->st.sectors_per_block;
        inode->i_blocks = inode->i_blocks > sectors ? inode->i_blocks - sectors : 0;
        x4_rebuild(fs, ino, inode, &l, 0);
        x4_list_free(&l);
        if (!stepped) break;
        if (ext2_write_inode(fs, ino, inode) < 0) break;
        ext2_jnl_step(fs);
        if (fs->ro) break;
    }
    return (int)freed;
}

/* An empty extent tree, for a new file or directory. */
static void x4_ext_init(ext2_fs_t *fs, ext2_inode_t *inode) {
    if (!fs->extents) return;
    inode->i_flags |= EXT4_EXTENTS_FL;
    memset(inode->i_block, 0, sizeof(inode->i_block));
    x4_hdr_put((uint8_t *)inode->i_block, 0, 4, 0);
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

    if (x4_is_ext(ino)) return x4_ext_map(fs, ino, idx, (int *)0);
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

static uint32_t ext2_file_blk_alloc(ext2_fs_t *fs, uint32_t ino_num, ext2_inode_t *ino,
                                    uint32_t idx) {
    if (x4_is_ext(ino)) return x4_ext_alloc(fs, ino_num, ino, idx);
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

static uint32_t ext2_write_node_do(vfs_node_t *node, uint32_t offset,
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

        int whole = (blk_off == 0 && to_copy == blk_size);
        fs->nozero = whole;
        uint32_t blk_num = ext2_file_blk_alloc(fs, priv->ino, &inode, blk_idx);
        fs->nozero = 0;
        if (blk_num == 0) break;

        if (!whole && ext2_read_block(fs, blk_num, blk_buf) < 0) break;
        memcpy(blk_buf + blk_off, buf + done, to_copy);
        if (ext2_write_data(fs, blk_num, blk_buf) < 0) break;
        done += to_copy;
        /* A large write commits in steps, each a complete shorter write:
         * the inode says what has been written so far, then the commit. */
        if (done < size && ext2_jnl_due(fs)) {
            if (offset + done > inode.i_size) {
                inode.i_size = offset + done;
                node->size = inode.i_size;
            }
            if (ext2_write_inode(fs, priv->ino, &inode) < 0) break;
            ext2_jnl_step(fs);
        }
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

/* A directory block back to the disk, with its checksum tail. */
static void x4_dx_csum(ext2_fs_t *fs, uint32_t dir_ino, const ext2_inode_t *dir_inode,
                       uint8_t *buf, uint32_t count_off);
static int x4_dx_kind(ext2_fs_t *fs, const ext2_inode_t *dir_inode, const uint8_t *buf);

static int ext2_write_dirblk(ext2_fs_t *fs, uint32_t dir_ino, const ext2_inode_t *dir_inode,
                             uint32_t blk, uint8_t *buf) {
    if (fs->csum) {
        /* An htree root or interior block carries a dx tail instead. */
        int kind = x4_dx_kind(fs, dir_inode, buf);
        if (kind) {
            x4_dx_csum(fs, dir_ino, dir_inode, buf, kind == 1 ? 0x20u : 8u);
            return ext2_write_block(fs, blk, buf);
        }
        /* Only a block whose entries stop where the tail starts has one. */
        uint32_t off = 0, end = ext2_dir_end(fs);
        while (off < end && ext2_de_ok(buf, off, end))
            off += ((ext2_dirent_t *)(buf + off))->rec_len;
        if (off == end) x4_dir_tail(fs, dir_ino, dir_inode->i_generation, buf);
        else printk("[EXT2]  %s: dir %u block %u has no checksum tail\n", fs->name,
                    (unsigned)dir_ino, (unsigned)blk);
        return ext2_write_block(fs, blk, buf);
    }
    return ext2_write_block(fs, blk, buf);
}

/* With metadata_csum the index blocks have no room for a leaf's checksum
 * tail (their checksum is a dx tail instead), so dropping the index also
 * turns them into plain leaf blocks: the root keeps "." and "..", whose
 * rec_len now stops at the tail, and an interior node becomes one empty
 * entry.  Both are what a linear reader already saw in them. */
static void ext2_dir_unindex(ext2_fs_t *fs, uint32_t dir_ino, ext2_inode_t *dir_inode) {
    if (!(dir_inode->i_flags & EXT2_INDEX_FL)) return;
    dir_inode->i_flags &= ~EXT2_INDEX_FL;
    if (!fs->csum) return;
    uint32_t bs = fs->st.block_size, end = ext2_dir_end(fs);
    uint8_t *buf = (uint8_t *)kmalloc(bs);
    if (!buf) return;
    uint32_t nblk = dir_inode->i_size / bs;
    for (uint32_t i = 0; i < nblk; i++) {
        uint32_t blk = ext2_file_blk(fs, dir_inode, i);
        if (!blk || ext2_read_block(fs, blk, buf) < 0) continue;
        ext2_dirent_t *de = (ext2_dirent_t *)buf;
        if (i == 0) {
            if (de->rec_len != 12 || de->name_len != 1) continue;
            ext2_dirent_t *dd = (ext2_dirent_t *)(buf + 12);
            if (dd->name_len != 2 || dd->rec_len != bs - 12) continue;
            dd->rec_len = (uint16_t)(end - 12);
        } else {
            if (de->inode || de->rec_len != bs) continue;     /* a leaf already */
            de->rec_len = (uint16_t)end;
            de->name_len = 0;
            de->file_type = 0;
        }
        ext2_write_dirblk(fs, dir_ino, dir_inode, blk, buf);
    }
    kfree(buf);
}

/* ── htree insertion ──────────────────────────────────────────────────────────
 * An indexed directory (dir_index: e2fsck -D, Linux) keeps its index: the
 * new name goes into the leaf its hash selects; a full leaf is split at the
 * median hash into a new block at the end of the directory and the index
 * gains an entry for it (a full root first moves its entries down into a new
 * interior block, a full interior block splits into two).  Root and interior
 * blocks carry a dx tail checksum with metadata_csum.  Up to one interior
 * level is handled (what a directory of a few hundred thousand names needs);
 * past that, or for anything unexpected, the index is dropped as before and
 * the name is added linearly.  Hashes: legacy, half-MD4 and TEA as described
 * in the kernel's ext4 documentation (the same functions fs/ext4.c reads with). */
#define DX_HASH_LEGACY    0
#define DX_HASH_HALF_MD4  1
#define DX_HASH_TEA       2

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

static inline uint32_t dx_rol32(uint32_t x, uint32_t s) { return (x << s) | (x >> (32 - s)); }

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
            int t = (4 - (j & 3)) & 3;
            uint32_t x = v[(t + 1) & 3], y = v[(t + 2) & 3], z = v[(t + 3) & 3];
            uint32_t f = r == 0 ? (z ^ (x & (y ^ z)))
                       : r == 1 ? ((x & y) | (x & z) | (y & z))
                       : (x ^ y ^ z);
            v[t] = dx_rol32(v[t] + f + in[order[r][j]] + konst[r], shifts[r][j & 3]);
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

static int dx_hash(ext2_fs_t *fs, uint32_t version, const char *name, uint32_t len,
                   uint32_t *hash) {
    int is_unsigned = (x4_rd32(fs->sb + 0x160) & 0x2u) != 0;
    uint32_t h[4] = { 0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u };
    uint32_t seed[4], any = 0;
    for (int i = 0; i < 4; i++) { seed[i] = x4_rd32(fs->sb + 0xEC + 4 * i); any |= seed[i]; }
    if (any) for (int i = 0; i < 4; i++) h[i] = seed[i];
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
    if (out == 0xFFFFFFFEu) out = 0xFFFFFFFCu;
    *hash = out;
    return 0;
}

static uint32_t dx_root_limit(ext2_fs_t *fs) {
    return (fs->st.block_size - 0x20u - (fs->csum ? 8u : 0u)) / 8u;
}
static uint32_t dx_node_limit(ext2_fs_t *fs) {
    return (fs->st.block_size - 8u - (fs->csum ? 8u : 0u)) / 8u;
}

/* 1: an htree root, 2: an interior block, 0: a leaf (or not indexed). */
static int x4_dx_kind(ext2_fs_t *fs, const ext2_inode_t *dir_inode, const uint8_t *buf) {
    if (!(dir_inode->i_flags & EXT2_INDEX_FL)) return 0;
    uint32_t bs = fs->st.block_size;
    const ext2_dirent_t *d0 = (const ext2_dirent_t *)buf;
    if (d0->inode == 0 && d0->rec_len == bs && x4_rd16(buf + 8) == dx_node_limit(fs))
        return 2;
    const ext2_dirent_t *d1 = (const ext2_dirent_t *)(buf + 12);
    if (d0->rec_len == 12 && d0->name_len == 1 && d0->name[0] == '.' &&
        d1->rec_len == bs - 12 && d1->name_len == 2 && buf[0x1D] == 8 &&
        x4_rd16(buf + 0x20) == dx_root_limit(fs))
        return 1;
    return 0;
}

static void x4_dx_csum(ext2_fs_t *fs, uint32_t dir_ino, const ext2_inode_t *dir_inode,
                       uint8_t *buf, uint32_t count_off) {
    if (!fs->csum) return;
    uint32_t limit = x4_rd16(buf + count_off), count = x4_rd16(buf + count_off + 2);
    uint32_t tail = count_off + limit * 8u;
    if (count > limit || tail + 8 > fs->st.block_size) return;
    uint32_t c = crc32c(x4_iseed(fs, dir_ino, dir_inode->i_generation), buf,
                        count_off + count * 8u);
    x4_wr32(buf + tail, 0);                    /* dt_reserved */
    x4_wr32(buf + tail + 4, 0);                /* dt_checksum, as zero */
    c = crc32c(c, buf + tail, 8);
    x4_wr32(buf + tail + 4, c);
}

/* Put an entry into a leaf block if it has room: 1, else 0. */
static int x4_leaf_insert(ext2_fs_t *fs, uint8_t *b, uint32_t child, const char *name,
                          uint32_t nlen, uint8_t ftype) {
    uint32_t end = ext2_dir_end(fs), need = ext2_dir_rec_len((uint8_t)nlen);
    for (uint32_t off = 0; off < end; ) {
        if (!ext2_de_ok(b, off, end)) return 0;
        ext2_dirent_t *de = (ext2_dirent_t *)(b + off);
        uint32_t actual = de->inode ? ext2_dir_rec_len(de->name_len) : 0;
        if (de->rec_len >= actual + need) {
            ext2_dirent_t *nd = de;
            if (de->inode) {
                uint16_t old = de->rec_len;
                de->rec_len = (uint16_t)actual;
                nd = (ext2_dirent_t *)(b + off + actual);
                nd->rec_len = (uint16_t)(old - actual);
            }
            nd->inode = child;
            nd->name_len = (uint8_t)nlen;
            nd->file_type = ftype;
            memcpy(nd->name, name, nlen);
            return 1;
        }
        off += de->rec_len;
    }
    return 0;
}

/* Insert dx entry (hash, lblk) after position `at` of the dx block at buf. */
static void x4_dx_insert(uint8_t *buf, uint32_t count_off, uint32_t at, uint32_t hash,
                         uint32_t lblk) {
    uint32_t count = x4_rd16(buf + count_off + 2);
    uint8_t *e = buf + count_off;
    memmove(e + 8 * (at + 2), e + 8 * (at + 1), 8 * (count - at - 1));
    x4_wr32(e + 8 * (at + 1), hash);
    x4_wr32(e + 8 * (at + 1) + 4, lblk);
    x4_wr16(buf + count_off + 2, (uint16_t)(count + 1));
}

/* Last entry of a dx block whose hash is <= `hash`. */
static uint32_t x4_dx_find(const uint8_t *buf, uint32_t count_off, uint32_t hash) {
    uint32_t count = x4_rd16(buf + count_off + 2), at = 0;
    for (uint32_t i = 1; i < count; i++) {
        if (x4_rd32(buf + count_off + 8 * i) > hash) break;
        at = i;
    }
    return at;
}

/* A new block at the end of the directory: its logical number, 0 on failure
 * (block 0 is the root, never new). */
static uint32_t x4_dir_grow(ext2_fs_t *fs, uint32_t dir_ino, ext2_inode_t *d, uint32_t *pblk) {
    uint32_t l = d->i_size / fs->st.block_size;
    *pblk = ext2_file_blk_alloc(fs, dir_ino, d, l);
    if (!*pblk) return 0;
    d->i_size += fs->st.block_size;
    return l;
}

typedef struct { uint32_t hash, off; } x4_dxent_t;

/* 1: added under the index; 0: the index cannot take it (drop it and add
 * linearly); -1: an I/O or allocation failure. */
static int x4_dx_add(ext2_fs_t *fs, uint32_t dir_ino, ext2_inode_t *d, uint32_t child,
                     const char *name, uint32_t nlen, uint8_t ftype) {
    uint32_t bs = fs->st.block_size;
    uint32_t nblk = d->i_size / bs;
    uint8_t *root = (uint8_t *)kmalloc(bs), *node = (uint8_t *)kmalloc(bs);
    uint8_t *leaf = (uint8_t *)kmalloc(bs), *nleaf = (uint8_t *)kmalloc(bs);
    x4_dxent_t *ents = (x4_dxent_t *)kmalloc((bs / 12 + 1) * sizeof(x4_dxent_t));
    uint8_t *fresh = (uint8_t *)0, *xbuf = (uint8_t *)0;
    int rc = -1;
    uint32_t rblk = 0, nodeblk = 0, nodel = 0, levels, version, hash;
    if (!root || !node || !leaf || !nleaf || !ents) goto out;
    rc = 0;
    if (!(rblk = ext2_file_blk(fs, d, 0)) || ext2_read_block(fs, rblk, root) < 0) goto out;
    if (x4_dx_kind(fs, d, root) != 1 || x4_rd32(root + 0x18) != 0) goto out;
    version = root[0x1C];
    levels = root[0x1E];
    if (levels > 1 || version > DX_HASH_TEA) goto out;
    if (dx_hash(fs, version, name, nlen, &hash) < 0) goto out;
    uint32_t rcount = x4_rd16(root + 0x22);
    if (!rcount || rcount > dx_root_limit(fs)) goto out;

    /* Down to the leaf. */
    uint32_t rat = x4_dx_find(root, 0x20, hash), nat = 0, leafl;
    if (levels) {
        nodel = x4_rd32(root + 0x20 + 8 * rat + 4) & 0x0FFFFFFFu;
        if (nodel >= nblk || !(nodeblk = ext2_file_blk(fs, d, nodel)) ||
            ext2_read_block(fs, nodeblk, node) < 0 || x4_dx_kind(fs, d, node) != 2)
            goto out;
        uint32_t ncount = x4_rd16(node + 10);
        if (!ncount || ncount > dx_node_limit(fs)) goto out;
        nat = x4_dx_find(node, 8, hash);
        leafl = x4_rd32(node + 8 + 8 * nat + 4) & 0x0FFFFFFFu;
    } else {
        leafl = x4_rd32(root + 0x20 + 8 * rat + 4) & 0x0FFFFFFFu;
    }
    uint32_t leafblk;
    if (leafl == 0 || leafl >= nblk || !(leafblk = ext2_file_blk(fs, d, leafl)) ||
        ext2_read_block(fs, leafblk, leaf) < 0)
        goto out;

    if (x4_leaf_insert(fs, leaf, child, name, nlen, ftype)) {
        rc = ext2_write_dirblk(fs, dir_ino, d, leafblk, leaf) < 0 ? -1 : 1;
        goto out;
    }

    /* The leaf is full.  Plan the split first and allocate every block it
     * needs before any index block changes: running out of space then
     * leaves the tree exactly as it was.  The writes go new blocks first,
     * the root last; with a journal they all land in one transaction. */
    int root_full = !levels && x4_rd16(root + 0x22) >= x4_rd16(root + 0x20);
    int node_full = levels && x4_rd16(node + 10) >= x4_rd16(node + 8);
    if (node_full && x4_rd16(root + 0x22) >= x4_rd16(root + 0x20))
        goto out;                                  /* would need a third level */

    uint32_t n = 0, end = ext2_dir_end(fs);
    for (uint32_t off = 0; off < end && n < bs / 12; ) {
        if (!ext2_de_ok(leaf, off, end)) goto out;
        ext2_dirent_t *de = (ext2_dirent_t *)(leaf + off);
        if (de->inode) {
            if (dx_hash(fs, version, de->name, de->name_len, &ents[n].hash) < 0) goto out;
            ents[n++].off = off;
        }
        off += de->rec_len;
    }
    if (n < 2) goto out;
    for (uint32_t i = 1; i < n; i++) {               /* insertion sort by hash */
        x4_dxent_t t = ents[i];
        uint32_t j = i;
        while (j && ents[j - 1].hash > t.hash) { ents[j] = ents[j - 1]; j--; }
        ents[j] = t;
    }
    uint32_t m = n / 2;
    uint32_t hash2 = ents[m].hash;
    uint32_t cont = (ents[m - 1].hash == hash2) ? 1u : 0u;

    rc = -1;
    uint32_t old_size = d->i_size, xl = 0, xpb = 0, newpb = 0, newl;
    fresh = (uint8_t *)kmalloc(bs);
    if ((root_full || node_full) && !(xbuf = (uint8_t *)kmalloc(bs))) goto nospace;
    if (!fresh) goto nospace;
    if ((root_full || node_full) && !(xl = x4_dir_grow(fs, dir_ino, d, &xpb))) goto nospace;
    if (!(newl = x4_dir_grow(fs, dir_ino, d, &newpb))) goto nospace;

    /* Everything is allocated: the index changes, in memory. */
    uint8_t *parent;
    uint32_t poff, pat, pblk_w;
    if (root_full) {
        /* The root's entries move down into the new interior block. */
        memset(xbuf, 0, bs);
        ((ext2_dirent_t *)xbuf)->rec_len = (uint16_t)bs;
        uint32_t cnt = x4_rd16(root + 0x22);
        memcpy(xbuf + 8, root + 0x20, 8 * cnt);
        x4_wr16(xbuf + 8, (uint16_t)dx_node_limit(fs));
        x4_wr16(xbuf + 10, (uint16_t)cnt);
        x4_wr16(root + 0x22, 1);
        x4_wr32(root + 0x24, xl);
        root[0x1E] = 1;
        parent = xbuf; poff = 8; pat = rat; pblk_w = xpb;
    } else if (node_full) {
        /* The interior block splits; the root gains the new half. */
        uint32_t cnt = x4_rd16(node + 10), half = cnt / 2;
        memset(xbuf, 0, bs);
        ((ext2_dirent_t *)xbuf)->rec_len = (uint16_t)bs;
        memcpy(xbuf + 8, node + 8 + 8 * half, 8 * (cnt - half));
        x4_wr16(xbuf + 8, (uint16_t)dx_node_limit(fs));
        x4_wr16(xbuf + 10, (uint16_t)(cnt - half));
        uint32_t split_hash = x4_rd32(node + 8 + 8 * half);
        x4_wr16(node + 10, (uint16_t)half);
        x4_dx_insert(root, 0x20, rat, split_hash, xl);
        poff = 8;
        if (nat >= half) { parent = xbuf; pat = nat - half; pblk_w = xpb; }
        else { parent = node; pat = nat; pblk_w = nodeblk; }
    } else {
        parent = levels ? node : root;
        poff = levels ? 8u : 0x20u;
        pat = levels ? nat : rat;
        pblk_w = levels ? nodeblk : rblk;
    }
    (void)pblk_w;

    /* The leaf, split at the median hash: [0, m) stays, [m, n) moves. */
    memcpy(nleaf, leaf, bs);                         /* the old contents */
    uint8_t *outb[2] = { leaf, fresh };
    for (int side = 0; side < 2; side++) {
        uint8_t *bb = outb[side];
        memset(bb, 0, bs);
        uint32_t off = 0, last = 0;
        int any = 0;
        for (uint32_t i = side ? m : 0; i < (side ? n : m); i++) {
            const ext2_dirent_t *src = (const ext2_dirent_t *)(nleaf + ents[i].off);
            ext2_dirent_t *dst = (ext2_dirent_t *)(bb + off);
            uint32_t len = ext2_dir_rec_len(src->name_len);
            dst->inode = src->inode;
            dst->name_len = src->name_len;
            dst->file_type = src->file_type;
            memcpy(dst->name, src->name, src->name_len);
            dst->rec_len = (uint16_t)len;
            last = off;
            off += len;
            any = 1;
        }
        if (any) ((ext2_dirent_t *)(bb + last))->rec_len = (uint16_t)(end - last);
        else ((ext2_dirent_t *)bb)->rec_len = (uint16_t)end;
    }
    if (!x4_leaf_insert(fs, (hash >= hash2) ? fresh : leaf, child, name, nlen, ftype))
        goto nospace;                              /* cannot happen: half empty */
    x4_dx_insert(parent, poff, pat, hash2 | cont, newl);

    /* New blocks, then the changed old ones, the root last. */
    if (ext2_write_dirblk(fs, dir_ino, d, newpb, fresh) < 0) goto out;
    if (xbuf && ext2_write_dirblk(fs, dir_ino, d, xpb, xbuf) < 0) goto out;
    if (levels && (node_full || parent == node) &&
        ext2_write_dirblk(fs, dir_ino, d, nodeblk, node) < 0) goto out;
    if (ext2_write_dirblk(fs, dir_ino, d, leafblk, leaf) < 0) goto out;
    if ((root_full || node_full || !levels) &&
        ext2_write_dirblk(fs, dir_ino, d, rblk, root) < 0) goto out;
    rc = 1;
    goto out;

nospace:
    /* Give back what was allocated; nothing in the tree points at it. */
    if (d->i_size > old_size) {
        uint32_t keep = old_size / bs;
        d->i_size = old_size;
        fs->no_step = 1;
        ext2_free_blocks_from(fs, dir_ino, d, keep);
        fs->no_step = 0;
    }
    rc = -1;
out:
    if (fresh) kfree(fresh);
    if (xbuf) kfree(xbuf);
    if (root) kfree(root);
    if (node) kfree(node);
    if (leaf) kfree(leaf);
    if (nleaf) kfree(nleaf);
    if (ents) kfree(ents);
    return rc;
}

static int ext2_add_dirent(ext2_fs_t *fs, uint32_t dir_ino, ext2_inode_t *dir_inode,
                            uint32_t child_ino, const char *name,
                            uint8_t file_type) {
    uint32_t name_len = strlen(name);
    if (name_len == 0 || name_len > 255) return -1;
    if (fs->x4 && (dir_inode->i_flags & EXT2_INDEX_FL)) {
        int r = x4_dx_add(fs, dir_ino, dir_inode, child_ino, name, name_len, file_type);
        if (r == 1) return ext2_write_inode(fs, dir_ino, dir_inode);
        if (r < 0) return -1;
        printk("[EXT2]  %s: dir %u: htree cannot take '%s'; index dropped\n", fs->name,
               (unsigned)dir_ino, name);
    }
    ext2_dir_unindex(fs, dir_ino, dir_inode);

    uint16_t need = ext2_dir_rec_len((uint8_t)name_len);
    uint32_t end = ext2_dir_end(fs);
    uint8_t *blk_buf = (uint8_t *)kmalloc(fs->st.block_size);
    if (!blk_buf) return -1;

    uint32_t ptrs_per_blk = fs->st.block_size / 4;
    for (uint32_t blk_idx = 0; blk_idx < 12 + ptrs_per_blk; blk_idx++) {
        uint32_t blk_num = ext2_file_blk(fs, dir_inode, blk_idx);
        if (!blk_num) {
            blk_num = ext2_file_blk_alloc(fs, dir_ino, dir_inode, blk_idx);
            if (!blk_num) break;
            dir_inode->i_size += fs->st.block_size;
            memset(blk_buf, 0, fs->st.block_size);
            ext2_dirent_t *de = (ext2_dirent_t *)blk_buf;
            de->inode = child_ino;
            de->rec_len = (uint16_t)end;
            de->name_len = (uint8_t)name_len;
            de->file_type = file_type;
            memcpy(de->name, name, name_len);
            int r = ext2_write_dirblk(fs, dir_ino, dir_inode, blk_num, blk_buf);
            if (r == 0) ext2_write_inode(fs, dir_ino, dir_inode);
            kfree(blk_buf);
            return r;
        }

        if (ext2_read_block(fs, blk_num, blk_buf) < 0) break;
        uint32_t offset = 0;
        while (offset < end) {
            ext2_dirent_t *de = (ext2_dirent_t *)(blk_buf + offset);
            if (!ext2_de_ok(blk_buf, offset, end)) break;

            uint16_t actual = de->inode ? ext2_dir_rec_len(de->name_len) : 0;
            if (!de->inode && de->rec_len >= need) {
                de->inode = child_ino;
                de->name_len = (uint8_t)name_len;
                de->file_type = file_type;
                memcpy(de->name, name, name_len);
                int r = ext2_write_dirblk(fs, dir_ino, dir_inode, blk_num, blk_buf);
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
                int r = ext2_write_dirblk(fs, dir_ino, dir_inode, blk_num, blk_buf);
                kfree(blk_buf);
                return r;
            }
            offset += de->rec_len;
        }
    }

    kfree(blk_buf);
    return -1;
}

static int ext2_remove_dirent(ext2_fs_t *fs, uint32_t dir_ino, ext2_inode_t *dir_inode,
                              const char *name, uint32_t *removed_ino) {
    uint32_t name_len = strlen(name);
    if (name_len == 0 || name_len > 255) return -1;
    /* Taking a name out of a leaf leaves an htree index valid. */
    if (!fs->x4) ext2_dir_unindex(fs, dir_ino, dir_inode);
    uint32_t end = ext2_dir_end(fs);

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
        while (offset < end) {
            ext2_dirent_t *de = (ext2_dirent_t *)(blk_buf + offset);
            if (!ext2_de_ok(blk_buf, offset, end)) break;

            if (de->inode && de->name_len == (uint8_t)name_len &&
                memcmp(de->name, name, name_len) == 0) {
                if (removed_ino) *removed_ino = de->inode;
                if (prev) {
                    prev->rec_len += de->rec_len;
                } else {
                    de->inode = 0;
                }
                int r = ext2_write_dirblk(fs, dir_ino, dir_inode, blk_num, blk_buf);
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
                               const ext2_inode_t *self, uint32_t parent_ino) {
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
    dotdot->rec_len = (uint16_t)(ext2_dir_end(fs) - dot->rec_len);
    dotdot->name_len = 2;
    dotdot->file_type = 2;
    dotdot->name[0] = '.';
    dotdot->name[1] = '.';

    int r = ext2_write_dirblk(fs, self_ino, self, blk, buf);
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
static void ext2_free_blocks_from(ext2_fs_t *fs, uint32_t ino, ext2_inode_t *inode,
                                  uint32_t from) {
    if (!inode) return;
    /* A fast symlink's i_block is its target text, not block numbers. */
    if (ext2_is_fast_symlink(fs, inode)) {
        if (from == 0) memset(inode->i_block, 0, sizeof(inode->i_block));
        return;
    }
    if (x4_is_ext(inode)) {
        if (x4_ext_free_from(fs, ino, inode, from) >= 0 && from == 0)
            inode->i_blocks = 0;
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
static void ext2_free_inode_blocks(ext2_fs_t *fs, uint32_t ino, ext2_inode_t *inode) {
    int fast = ext2_is_fast_symlink(fs, inode);
    ext2_free_blocks_from(fs, ino, inode, 0);
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
        ext2_free_inode_blocks(fs, ino, &victim);
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
    preempt_enable();
    if (release) {
        ext2_lock(fs);
        ext2_release_orphan(fs, ino);
        ext2_op_end(fs);
        ext2_unlock(fs);
    }
    /* Only now: while open_refs counts this reference the instance is busy,
     * so a lazily detached one is not released (vfs.c detached_reap) under
     * the orphan release's disk I/O. */
    preempt_disable();
    if (fs->open_refs > 0) fs->open_refs--;
    preempt_enable();
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

/* i_generation of a new inode: NFS-style, it only has to differ from the
 * last inode that had the number, and it seeds the inode's checksums. */
static uint32_t ext2_new_generation(uint32_t ino) {
    static uint32_t ctr;
    return (ext2_now() * 2654435761u) ^ (ino << 8) ^ ++ctr;
}

static int ext2_create_do(vfs_node_t *dir, const char *name, uint32_t flags) {
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
    inode.i_generation = ext2_new_generation(ino);
    x4_ext_init(fs, &inode);

    if (flags == VFS_FLAG_DIR) {
        uint32_t blk = ext2_file_blk_alloc(fs, ino, &inode, 0);
        if (!blk) {
            ext2_free_inode(fs, ino);
            return -1;
        }
        if (ext2_init_dir_block(fs, blk, ino, &inode, dpriv->ino) < 0) {
            ext2_free_inode_blocks(fs, ino, &inode);
            ext2_free_inode(fs, ino);
            return -1;
        }
        inode.i_mode = EXT2_S_IFDIR | 0755;
        inode.i_size = fs->st.block_size;
        inode.i_links_count = 2;
    } else {
        inode.i_mode = (flags == VFS_FLAG_SOCK ? EXT2_S_IFSOCK : EXT2_S_IFREG) | 0644;
        inode.i_size = 0;
        inode.i_links_count = 1;
        inode.i_blocks = 0;
    }

    if (ext2_write_inode_ex(fs, ino, &inode, 1) < 0) {
        ext2_free_inode_blocks(fs, ino, &inode);
        ext2_free_inode(fs, ino);
        return -1;
    }

    uint8_t ftype = (flags == VFS_FLAG_DIR) ? 2
                  : (flags == VFS_FLAG_SOCK) ? EXT2_FT_SOCK : 1;
    if (ext2_add_dirent(fs, dpriv->ino, &dir_inode, ino, name, ftype) < 0) {
        ext2_free_inode_blocks(fs, ino, &inode);
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
static int ext2_symlink_do(vfs_node_t *dir, const char *name, const char *target) {
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
    inode.i_generation = ext2_new_generation(ino);

    if (tlen < EXT2_FAST_LINK_MAX) {
        memcpy(inode.i_block, target, tlen);
    } else {
        uint8_t *buf = (uint8_t *)kmalloc(fs->st.block_size);
        if (!buf) { ext2_free_inode(fs, ino); return -12; }         /* -ENOMEM */
        x4_ext_init(fs, &inode);
        uint32_t blk = ext2_file_blk_alloc(fs, ino, &inode, 0);
        if (!blk) { kfree(buf); ext2_free_inode(fs, ino); return -28; }
        memset(buf, 0, fs->st.block_size);
        memcpy(buf, target, tlen);
        int w = ext2_write_data(fs, blk, buf);
        kfree(buf);
        if (w < 0) {
            ext2_free_inode_blocks(fs, ino, &inode);
            ext2_free_inode(fs, ino);
            return -5;
        }
    }

    if (ext2_write_inode_ex(fs, ino, &inode, 1) < 0) {
        ext2_free_inode_blocks(fs, ino, &inode);
        ext2_free_inode(fs, ino);
        return -5;
    }
    if (ext2_add_dirent(fs, dpriv->ino, &dir_inode, ino, name, EXT2_FT_SYMLINK) < 0) {
        ext2_free_inode_blocks(fs, ino, &inode);
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
        ext2_free_inode_blocks(fs, ino, victim);
        victim->i_dtime = now;
        victim->i_size = 0;
        ext2_write_inode(fs, ino, victim);
        ext2_free_inode(fs, ino);
    }
}

static int ext2_unlink_do(vfs_node_t *dir, const char *name) {
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

    if (ext2_remove_dirent(fs, dpriv->ino, &dir_inode, name, NULL) < 0) return -1;

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
static int ext2_set_dirent(ext2_fs_t *fs, uint32_t dir_ino, ext2_inode_t *dir_inode,
                           const char *name, uint32_t ino, uint8_t ftype) {
    uint32_t name_len = strlen(name);
    if (name_len == 0 || name_len > 255) return -1;
    /* Rewriting an entry in place leaves an htree index valid. */
    if (!fs->x4) ext2_dir_unindex(fs, dir_ino, dir_inode);
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
                int r = ext2_write_dirblk(fs, dir_ino, dir_inode, blk_num, blk_buf);
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
static int ext2_rename_do(vfs_node_t *old_dir, const char *old_name,
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
        if (ext2_set_dirent(fs, n_ino, ndir, new_name, src_ino, src_ft) < 0) return -5;
    } else {
        if (ext2_add_dirent(fs, n_ino, ndir, src_ino, new_name, src_ft) < 0)
            return -28;                                      /* -ENOSPC */
    }

    /* 2. The old name.  In the same directory this re-reads nothing: odir is
     * the only copy of the inode and add_dirent kept it current. */
    if (ext2_remove_dirent(fs, o_ino, &odir, old_name, (uint32_t *)0) < 0)
        printk("[ext2] rename: '%s' vanished from its directory\n", old_name);

    uint32_t now = ext2_now();

    /* 3. A directory that changed parent: its ".." and both link counts. */
    if (src_is_dir && !same_dir) {
        ext2_set_dirent(fs, src_ino, &src, "..", n_ino, 2);
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
static int ext2_setattr_do(vfs_node_t *node, uint32_t mode, uint32_t uid,
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
static int ext2_settimes_do(vfs_node_t *node, uint32_t atime, uint32_t mtime) {
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
static int ext2_link_do(vfs_node_t *dir, const char *name, vfs_node_t *target) {
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

static int ext2_truncate_do(vfs_node_t *node, uint32_t new_size) {
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
    if (!x4_is_ext(&inode) && new_blocks > old_blocks && new_blocks > max_blocks) return -1;

    /* Growing an extent-mapped file leaves a hole, as Linux does. */
    if (new_blocks > old_blocks && !x4_is_ext(&inode)) {
        uint32_t i;
        for (i = old_blocks; i < new_blocks; i++) {
            if (!ext2_file_blk_alloc(fs, priv->ino, &inode, i)) {
                ext2_free_blocks_from(fs, priv->ino, &inode, old_blocks);   /* undo this call */
                return -1;
            }
        }
    } else if (new_blocks < old_blocks) {
        /* Free the tail, including any indirect blocks it leaves empty.  This
         * used to go through a per-index helper that silently did nothing
         * beyond the singly-indirect range, so shrinking a large file leaked
         * exactly like unlink did. */
        ext2_free_blocks_from(fs, priv->ino, &inode, new_blocks);
    }
    /* The rest of a block cut in the middle reads as zeroes if the file
     * grows over it again (and holds nothing of the old contents). */
    if (new_size < inode.i_size && (new_size % bs) && !ext2_is_fast_symlink(fs, &inode)) {
        uint32_t blk = ext2_file_blk(fs, &inode, new_size / bs);
        uint8_t *tb = blk ? (uint8_t *)kmalloc(bs) : (uint8_t *)0;
        if (tb && ext2_read_block(fs, blk, tb) == 0) {
            memset(tb + new_size % bs, 0, bs - new_size % bs);
            ext2_write_data(fs, blk, tb);
        }
        if (tb) kfree(tb);
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

/* ── jbd2 ─────────────────────────────────────────────────────────────────────
 * The journal of an ext3/ext4 (has_journal, an internal journal inode), as
 * described in the Linux kernel's Documentation/filesystems/ext4/journal.rst
 * (kernel.org); no code was copied.  Everything in it is big-endian.
 *
 * Writing: every metadata block an operation changes (bitmaps, descriptors,
 * inodes, directory and extent blocks, the superblock) goes into a running
 * transaction instead of to its place on the disk; file data goes to its
 * place at once (ordered mode).  When the operation ends -- or the
 * transaction is as large as the journal allows -- it commits: the journal
 * superblock is pointed at the log, a descriptor block and the new copies
 * are written into it, then the commit block; only then are the blocks
 * written in place, and the journal is marked empty again.  A crash before
 * the commit block leaves the old state, one after it a log that replays
 * (here, in Linux or e2fsck) to the new one.  While mounted read-write the
 * superblock carries needs_recovery, as Linux has it, so that a journal left
 * behind is replayed and not discarded.
 *
 * Replay, at a read-write mount whose journal is not empty: scan the log from
 * s_start for transactions closed by a valid commit block, collect revoke
 * records, then write every logged block not revoked by its own or a later
 * transaction to its place. */
#define JBD2_MAGIC          0xC03B3998u
#define JBD2_DESCRIPTOR     1u
#define JBD2_COMMIT         2u
#define JBD2_SB_V1          3u
#define JBD2_SB_V2          4u
#define JBD2_REVOKE         5u
#define JBD2_FLAG_ESCAPE    1u
#define JBD2_FLAG_SAME_UUID 2u
#define JBD2_FLAG_LAST_TAG  8u
#define JBD2_COMPAT_CHECKSUM 0x1u
#define JBD2_INCOMPAT_REVOKE 0x1u
#define JBD2_INCOMPAT_64BIT  0x2u
#define JBD2_INCOMPAT_ASYNC  0x4u
#define JBD2_INCOMPAT_CSUM2  0x8u
#define JBD2_INCOMPAT_CSUM3  0x10u
#define JBD2_INCOMPAT_FC     0x20u
#define JBD2_INCOMPAT_KNOWN  (JBD2_INCOMPAT_REVOKE | JBD2_INCOMPAT_64BIT | JBD2_INCOMPAT_ASYNC | \
                              JBD2_INCOMPAT_CSUM2 | JBD2_INCOMPAT_CSUM3 | JBD2_INCOMPAT_FC)
#define EXT2_JT_HASH        8192u           /* > 2 * the largest transaction */
#define EXT2_JT_MAX         4096u
#define EXT2_JT_SOFT        1024u

typedef struct ext2_jnl {
    uint32_t  len, first, seq;     /* s_maxlen, s_first, next transaction id */
    uint32_t *map;                 /* journal block -> filesystem block */
    uint32_t  compat, incompat;
    uint32_t  seed;                /* crc32c(~0, journal uuid) */
    uint8_t   uuid[16];
    uint32_t  tag_bytes;
    int       csum;                /* v2 or v3 checksums */
    uint32_t  cap;                 /* commit at the next safe point past this */
    uint32_t  hard;                /* most blocks one transaction can hold */
    uint32_t  n;
    uint32_t  tblk[EXT2_JT_MAX];
    uint8_t  *tdata[EXT2_JT_MAX];
    uint16_t  hash[EXT2_JT_HASH];  /* entry + 1, 0 = empty */
    uint32_t  nfreed, fcap;
    uint32_t *fstart, *flen;       /* grown as needed, emptied by each commit */
    int       busy;
    uint32_t  commits;
    uint32_t  last;                /* pit_ticks() of the last commit */
} ext2_jnl_t;

static inline uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static inline void wbe32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static inline uint32_t jt_slot(uint32_t blk) { return (blk * 2654435761u) >> 20; }

static const uint8_t *ext2_jt_find(ext2_fs_t *fs, uint32_t blk) {
    ext2_jnl_t *j = fs->j;
    if (!j || !j->n) return (const uint8_t *)0;
    for (uint32_t h = jt_slot(blk), k = 0; k < EXT2_JT_HASH; k++, h = (h + 1) & (EXT2_JT_HASH - 1)) {
        uint32_t e = j->hash[h];
        if (!e) return (const uint8_t *)0;
        if (j->tblk[e - 1] == blk) return j->tdata[e - 1];
    }
    return (const uint8_t *)0;
}

static void ext2_jt_drop(ext2_fs_t *fs, uint32_t blk) {
    /* A block that was metadata in this transaction and is now file data
     * (freed and reused cannot happen before the commit: see ext2_jt_freed):
     * keep the entry, but make it carry the data, so the checkpoint does not
     * write stale metadata over it. */
    ext2_jnl_t *j = fs->j;
    if (!j || !j->n) return;
    const uint8_t *d = ext2_jt_find(fs, blk);
    if (d) printk("[EXT2]  %s: journal: block %u written as data while logged\n",
                  fs->name, (unsigned)blk);
}

/* Commit so the blocks this transaction freed can be allocated: 1 if
 * there were any. */
static int ext2_jnl_reclaim(ext2_fs_t *fs) {
    return fs->j->nfreed != 0;
}

/* The journal cannot take what the running operation needs (or a commit
 * failed): like jbd2's abort, the instance stops writing.  What the journal
 * already holds is consistent; the running transaction is lost. */
static void ext2_jnl_abort(ext2_fs_t *fs, const char *why) {
    if (!fs->ro)
        printk("[EXT2]  %s: journal aborted (%s); read-only until remounted after "
               "e2fsck\n", fs->name, why);
    fs->ro = 1;
    fs->rw_ok = 0;
}

static int ext2_jt_freed(ext2_fs_t *fs, uint32_t blk) {
    ext2_jnl_t *j = fs->j;
    for (uint32_t i = 0; i < j->nfreed; i++)
        if (blk >= j->fstart[i] && blk - j->fstart[i] < j->flen[i]) return 1;
    return 0;
}

static int ext2_jnl_commit(ext2_fs_t *fs);

static void ext2_jt_freed_add(ext2_fs_t *fs, uint32_t blk, uint32_t n) {
    ext2_jnl_t *j = fs->j;
    if (j->busy) return;
    if (j->nfreed && j->fstart[j->nfreed - 1] + j->flen[j->nfreed - 1] == blk) {
        j->flen[j->nfreed - 1] += n;
        return;
    }
    if (j->nfreed == j->fcap) {
        /* Never a commit here: the operation freeing these is half done. */
        uint32_t nc = j->fcap ? j->fcap * 2 : 64;
        uint32_t *ns = (uint32_t *)kmalloc(nc * sizeof(uint32_t));
        uint32_t *nl = (uint32_t *)kmalloc(nc * sizeof(uint32_t));
        if (!ns || !nl) {
            if (ns) kfree(ns);
            if (nl) kfree(nl);
            ext2_jnl_abort(fs, "out of memory for freed blocks");
            return;
        }
        if (j->nfreed) {
            memcpy(ns, j->fstart, j->nfreed * sizeof(uint32_t));
            memcpy(nl, j->flen, j->nfreed * sizeof(uint32_t));
        }
        if (j->fstart) kfree(j->fstart);
        if (j->flen) kfree(j->flen);
        j->fstart = ns;
        j->flen = nl;
        j->fcap = nc;
    }
    j->fstart[j->nfreed] = blk;
    j->flen[j->nfreed] = n;
    j->nfreed++;
}

static int ext2_jt_put(ext2_fs_t *fs, uint32_t blk, const void *buf) {
    ext2_jnl_t *j = fs->j;
    uint8_t *d = (uint8_t *)ext2_jt_find(fs, blk);
    if (d) {
        memcpy(d, buf, fs->st.block_size);
        return 0;
    }
    /* Never a commit here, in the middle of an operation: operations that
     * can grow large commit at their own safe points (ext2_jnl_step). */
    if (j->n >= j->hard) {
        ext2_jnl_abort(fs, "operation larger than the journal");
        return -1;
    }
    d = (uint8_t *)kmalloc(fs->st.block_size);
    if (!d) {
        ext2_jnl_abort(fs, "out of memory");
        return -1;
    }
    memcpy(d, buf, fs->st.block_size);
    uint32_t e = j->n++;
    j->tblk[e] = blk;
    j->tdata[e] = d;
    uint32_t h = jt_slot(blk);
    while (j->hash[h]) h = (h + 1) & (EXT2_JT_HASH - 1);
    j->hash[h] = (uint16_t)(e + 1);
    return 0;
}

/* Journal block `jb` (index into the journal inode). */
static int ext2_jnl_read(ext2_fs_t *fs, uint32_t jb, void *buf) {
    if (jb >= fs->j->len || !fs->j->map[jb]) return -1;
    return ext2_raw_read_block(fs, fs->j->map[jb], buf);
}
static int ext2_jnl_write(ext2_fs_t *fs, uint32_t jb, const void *buf) {
    if (jb >= fs->j->len || !fs->j->map[jb]) return -1;
    return ext2_raw_write_block(fs, fs->j->map[jb], buf);
}

/* The journal superblock: s_start (0 = empty) and s_sequence. */
static int ext2_jnl_sb(ext2_fs_t *fs, uint32_t start, uint32_t seq) {
    uint8_t *b = (uint8_t *)kmalloc(fs->st.block_size);
    if (!b) return -1;
    int rc = ext2_jnl_read(fs, 0, b);
    if (rc == 0) {
        wbe32(b + 0x18, seq);
        wbe32(b + 0x1C, start);
        if (fs->j->csum) {
            wbe32(b + 0xFC, 0);
            wbe32(b + 0xFC, crc32c(~0u, b, 1024));
        }
        rc = ext2_jnl_write(fs, 0, b);
    }
    kfree(b);
    return rc;
}

static void ext2_jnl_block_tail(ext2_fs_t *fs, uint8_t *b) {
    if (!fs->j->csum) return;
    wbe32(b + fs->st.block_size - 4, 0);
    wbe32(b + fs->st.block_size - 4, crc32c(fs->j->seed, b, fs->st.block_size));
}

/* Write the running transaction to the journal, then in place. */
static int ext2_jnl_commit(ext2_fs_t *fs) {
    ext2_jnl_t *j = fs->j;
    if (!j || j->busy) return 0;
    if (!j->n) { j->nfreed = 0; return 0; }
    j->busy = 1;
    uint32_t bs = fs->st.block_size, seq = j->seq;
    uint8_t *desc = (uint8_t *)kmalloc(bs), *copy = (uint8_t *)kmalloc(bs);
    int rc = -1;
    if (!desc || !copy) goto out;

    if (ext2_jnl_sb(fs, j->first, seq) < 0) goto out;
    uint32_t pos = j->first;
    uint32_t limit = bs - (j->csum ? 4u : 0u);
    for (uint32_t i = 0; i < j->n; ) {
        memset(desc, 0, bs);
        wbe32(desc, JBD2_MAGIC);
        wbe32(desc + 4, JBD2_DESCRIPTOR);
        wbe32(desc + 8, seq);
        uint32_t off = 12, first = i, last_tag = 0;
        while (i < j->n && off + j->tag_bytes + (i == first ? 16u : 0u) <= limit) {
            uint8_t *t = desc + off;
            const uint8_t *data = j->tdata[i];
            uint32_t flags = (i == first) ? 0 : JBD2_FLAG_SAME_UUID;
            if (be32(data) == JBD2_MAGIC) flags |= JBD2_FLAG_ESCAPE;
            wbe32(t, j->tblk[i]);
            uint32_t tc = 0;
            if (j->csum) {
                uint8_t sq[4];
                wbe32(sq, seq);
                memcpy(copy, data, bs);
                if (flags & JBD2_FLAG_ESCAPE) wbe32(copy, 0);
                tc = crc32c(crc32c(j->seed, sq, 4), copy, bs);
            }
            if (j->incompat & JBD2_INCOMPAT_CSUM3) {
                wbe32(t + 4, flags);
                wbe32(t + 8, 0);                          /* blocknr_high */
                wbe32(t + 12, tc);
            } else {
                t[4] = (uint8_t)(tc >> 8); t[5] = (uint8_t)tc;   /* be16 checksum */
                t[6] = (uint8_t)(flags >> 8); t[7] = (uint8_t)flags;
                if (j->incompat & JBD2_INCOMPAT_64BIT) wbe32(t + 8, 0);
            }
            last_tag = off;
            off += j->tag_bytes;
            if (i == first) {
                memcpy(desc + off, j->uuid, 16);
                off += 16;
            }
            i++;
        }
        /* LAST_TAG on the last tag of this descriptor */
        if (j->incompat & JBD2_INCOMPAT_CSUM3) {
            wbe32(desc + last_tag + 4, be32(desc + last_tag + 4) | JBD2_FLAG_LAST_TAG);
        } else {
            desc[last_tag + 7] |= (uint8_t)JBD2_FLAG_LAST_TAG;
        }
        ext2_jnl_block_tail(fs, desc);
        if (ext2_jnl_write(fs, pos++, desc) < 0) goto out;
        for (uint32_t k = first; k < i; k++) {
            memcpy(copy, j->tdata[k], bs);
            if (be32(copy) == JBD2_MAGIC) wbe32(copy, 0);
            if (ext2_jnl_write(fs, pos++, copy) < 0) goto out;
        }
    }
    /* The commit block. */
    memset(desc, 0, bs);
    wbe32(desc, JBD2_MAGIC);
    wbe32(desc + 4, JBD2_COMMIT);
    wbe32(desc + 8, seq);
    uint32_t now = ext2_now();
    wbe32(desc + 0x30, 0);                     /* h_commit_sec, high half */
    wbe32(desc + 0x34, now);
    if (j->csum) wbe32(desc + 16, crc32c(j->seed, desc, bs));
    if (ext2_jnl_write(fs, pos++, desc) < 0) goto out;

    if (fs->crash_test == 2) {
        /* Test hook: the power goes now -- committed, nothing in place. */
        printk("[EXT2]  %s: x4crash: stopped after committing transaction %u "
               "(%u blocks), nothing written in place\n", fs->name, (unsigned)seq,
               (unsigned)j->n);
        fs->ro = 1;
        fs->crash_test = 3;
        rc = 0;
        goto out;
    }

    /* Checkpoint: everything in place, then the log is empty again. */
    for (uint32_t k = 0; k < j->n; k++)
        if (ext2_raw_write_block(fs, j->tblk[k], j->tdata[k]) < 0) goto out;
    j->seq = seq + 1;
    if (ext2_jnl_sb(fs, 0, j->seq) < 0) goto out;
    j->commits++;
    rc = 0;
out:
    j->last = pit_ticks();
    if (rc < 0) {
        printk("[EXT2]  %s: journal commit %u failed\n", fs->name, (unsigned)seq);
        ext2_jnl_abort(fs, "commit failed");
    }
    for (uint32_t k = 0; k < j->n; k++) kfree(j->tdata[k]);
    j->n = 0;
    j->nfreed = 0;
    memset(j->hash, 0, sizeof(j->hash));
    if (desc) kfree(desc);
    if (copy) kfree(copy);
    j->busy = 0;
    return rc;
}

/* The superblock into the transaction (or onto the disk) when it changed. */
static void x4_sb_sync(ext2_fs_t *fs) {
    if (!fs->x4 || !fs->sb_dirty || fs->ro) return;
    if (!fs->j) { x4_sb_write(fs); return; }
    if (fs->csum) x4_wr32(fs->sb + SB_CHECKSUM, crc32c(~0u, fs->sb, SB_CHECKSUM));
    uint32_t bs = fs->st.block_size;
    uint32_t blk = bs == 1024 ? 1 : 0, off = bs == 1024 ? 0 : 1024;
    uint8_t *b = (uint8_t *)kmalloc(bs);
    if (!b) return;
    if (ext2_read_block(fs, blk, b) == 0) {
        memcpy(b + off, fs->sb, 1024);
        if (ext2_write_block(fs, blk, b) == 0) fs->sb_dirty = 0;
    }
    kfree(b);
}

/* The end of one operation that may have changed the filesystem.  Without a
 * journal the superblock goes out now (everything else already has).  With
 * one, the transaction keeps collecting operations and commits once a second
 * (here, or from the flusher when nothing else happens), when it is full, at
 * sync/fsync and when the instance goes read-only: Linux's ordered mode with
 * a 1 s commit interval instead of 5. */
#define EXT2_COMMIT_TICKS 100u
static void ext2_op_end(ext2_fs_t *fs) {
    if (!fs->x4 || fs->ro) return;
    x4_sb_sync(fs);
    if (fs->j && (fs->crash_test == 2 || fs->j->n >= fs->j->cap ||
                  pit_ticks() - fs->j->last >= EXT2_COMMIT_TICKS))
        ext2_jnl_commit(fs);
}

/* Everything changed so far, committed and in place.  Caller holds the lock. */
static void ext2_sync_fs(ext2_fs_t *fs) {
    if (!fs->x4 || fs->ro) return;
    x4_sb_sync(fs);
    if (fs->j) ext2_jnl_commit(fs);
}

/* A safe point inside a large operation (one that commits in complete
 * steps): is the transaction big enough to commit here? */
static int ext2_jnl_due(ext2_fs_t *fs) {
    return fs->j && !fs->ro && !fs->no_step && !fs->j->busy && fs->j->n >= fs->j->cap;
}

static void ext2_jnl_step(ext2_fs_t *fs) {
    if (!ext2_jnl_due(fs)) return;
    x4_sb_sync(fs);
    ext2_jnl_commit(fs);
}

/* Journaled instances, for the flusher and sync(2). */
#define EXT2_JFS_MAX 16
static ext2_fs_t *g_jfs[EXT2_JFS_MAX];
static volatile int g_jfs_lock;
static int g_flusher_started;

static void jfs_lock(void) {
    while (__sync_lock_test_and_set(&g_jfs_lock, 1)) sleep_ticks(1);
}
static void jfs_unlock(void) { __sync_lock_release(&g_jfs_lock); }

/* sync(2), fsync(2), fdatasync(2): commit every journaled instance. */
void ext2_sync_all(void) {
    jfs_lock();
    for (int i = 0; i < EXT2_JFS_MAX; i++) {
        ext2_fs_t *fs = g_jfs[i];
        if (!fs) continue;
        ext2_lock(fs);
        ext2_sync_fs(fs);
        ext2_unlock(fs);
    }
    jfs_unlock();
}

/* kjournald: commits a transaction that has been open for a second with no
 * operation around to do it. */
static void ext2_flusher(void) {
    for (;;) {
        sleep_ticks(EXT2_COMMIT_TICKS);
        jfs_lock();
        for (int i = 0; i < EXT2_JFS_MAX; i++) {
            ext2_fs_t *fs = g_jfs[i];
            if (!fs || fs->ro || !fs->j || (!fs->j->n && !fs->sb_dirty)) continue;
            if (pit_ticks() - fs->j->last < EXT2_COMMIT_TICKS) continue;
            if (__sync_lock_test_and_set(&fs->lock, 1)) continue;   /* busy: later */
            ext2_sync_fs(fs);
            ext2_unlock(fs);
        }
        jfs_unlock();
    }
}

static void ext2_jfs_add(ext2_fs_t *fs) {
    if (!fs->j) return;
    jfs_lock();
    for (int i = 0; i < EXT2_JFS_MAX; i++)
        if (!g_jfs[i]) { g_jfs[i] = fs; break; }
    jfs_unlock();
    if (!g_flusher_started) {
        g_flusher_started = 1;
        proc_create_kthread(ext2_flusher, "kjournald");
    }
}

static void ext2_jfs_del(ext2_fs_t *fs) {
    jfs_lock();
    for (int i = 0; i < EXT2_JFS_MAX; i++)
        if (g_jfs[i] == fs) g_jfs[i] = (ext2_fs_t *)0;
    jfs_unlock();
}

/* Each journal block's place on the disk, from the journal inode. */
static int ext2_jnl_map(ext2_fs_t *fs, uint32_t jino, uint32_t len) {
    ext2_inode_t ji;
    if (ext2_read_inode(fs, jino, &ji) < 0) return -1;
    if (ji.i_size / fs->st.block_size < len) return -1;
    uint32_t *map = (uint32_t *)kmalloc(len * sizeof(uint32_t));
    if (!map) return -1;
    memset(map, 0, len * sizeof(uint32_t));
    if (x4_is_ext(&ji)) {
        x4_list_t l;
        memset(&l, 0, sizeof(l));
        if (x4_collect(fs, (const uint8_t *)ji.i_block, 60, 0, &l) < 0) {
            x4_list_free(&l);
            kfree(map);
            return -1;
        }
        for (uint32_t i = 0; i < l.n; i++)
            for (uint32_t k = 0; k < l.v[i].len; k++)
                if (l.v[i].lblk + k < len && !l.v[i].unwritten)
                    map[l.v[i].lblk + k] = l.v[i].pblk + k;
        x4_list_free(&l);
    } else {
        for (uint32_t i = 0; i < len; i++) map[i] = ext2_file_blk(fs, &ji, i);
    }
    for (uint32_t i = 0; i < len; i++)
        if (!map[i]) { kfree(map); return -1; }
    fs->j->map = map;
    return 0;
}

static void ext2_jnl_free(ext2_fs_t *fs) {
    if (!fs->j) return;
    for (uint32_t k = 0; k < fs->j->n; k++) kfree(fs->j->tdata[k]);
    if (fs->j->map) kfree(fs->j->map);
    if (fs->j->fstart) kfree(fs->j->fstart);
    if (fs->j->flen) kfree(fs->j->flen);
    kfree(fs->j);
    fs->j = (ext2_jnl_t *)0;
}

/* Bytes of one block tag. */
static uint32_t jnl_tag_bytes(uint32_t incompat) {
    if (incompat & JBD2_INCOMPAT_CSUM3) return 16;
    uint32_t sz = 12;
    if (incompat & JBD2_INCOMPAT_CSUM2) sz += 2;
    return (incompat & JBD2_INCOMPAT_64BIT) ? sz : sz - 4;
}

/* Open the journal: 0, or <0 when it cannot be used (and then the instance
 * cannot be written). */
static int ext2_jnl_open(ext2_fs_t *fs, uint32_t jino) {
    ext2_jnl_t *j = (ext2_jnl_t *)kmalloc(sizeof(ext2_jnl_t));
    if (!j) return -12;
    memset(j, 0, sizeof(*j));
    fs->j = j;
    uint8_t *b = (uint8_t *)kmalloc(fs->st.block_size);
    if (!b) { ext2_jnl_free(fs); return -12; }
    /* Block 0 of the journal first, to learn its length. */
    ext2_inode_t ji;
    uint32_t jb0;
    int rc = -22;
    if (ext2_read_inode(fs, jino, &ji) < 0 || !(jb0 = ext2_file_blk(fs, &ji, 0)) ||
        ext2_raw_read_block(fs, jb0, b) < 0)
        goto bad;
    uint32_t type = be32(b + 4);
    if (be32(b) != JBD2_MAGIC || (type != JBD2_SB_V1 && type != JBD2_SB_V2) ||
        be32(b + 0x0C) != fs->st.block_size) {
        printk("[EXT2]  %s: journal superblock not recognised\n", fs->name);
        goto bad;
    }
    j->len = be32(b + 0x10);
    j->first = be32(b + 0x14);
    j->seq = be32(b + 0x18);
    if (type == JBD2_SB_V2) {
        j->compat = be32(b + 0x24);
        j->incompat = be32(b + 0x28);
    }
    memcpy(j->uuid, b + 0x30, 16);
    if ((j->incompat & ~JBD2_INCOMPAT_KNOWN) || (j->compat & JBD2_COMPAT_CHECKSUM) ||
        ((j->incompat & JBD2_INCOMPAT_CSUM2) && (j->incompat & JBD2_INCOMPAT_CSUM3))) {
        printk("[EXT2]  %s: journal features 0x%x/0x%x not supported\n", fs->name,
               (unsigned)j->compat, (unsigned)j->incompat);
        rc = -95;
        goto bad;
    }
    j->csum = (j->incompat & (JBD2_INCOMPAT_CSUM2 | JBD2_INCOMPAT_CSUM3)) != 0;
    j->seed = crc32c(~0u, j->uuid, 16);
    j->tag_bytes = jnl_tag_bytes(j->incompat);
    if (j->csum) {
        uint32_t want = be32(b + 0xFC);
        wbe32(b + 0xFC, 0);
        if (crc32c(~0u, b, 1024) != want) {
            printk("[EXT2]  %s: journal superblock checksum mismatch\n", fs->name);
            goto bad;
        }
    }
    if (j->len < 64 || j->first == 0 || j->first >= j->len || j->len - j->first < 32) {
        printk("[EXT2]  %s: journal geometry impossible (len %u, first %u)\n", fs->name,
               (unsigned)j->len, (unsigned)j->first);
        goto bad;
    }
    if (ext2_jnl_map(fs, jino, j->len) < 0) {
        printk("[EXT2]  %s: journal inode unreadable\n", fs->name);
        goto bad;
    }
    /* Room for a transaction: its blocks, their descriptors, the commit. */
    {
        uint32_t room = j->len - j->first;
        uint32_t per = (fs->st.block_size - 12 - 4 - 16) / j->tag_bytes;
        uint32_t desc = (room + per - 1) / per;
        if (room < desc + 4 + 16) goto bad;
        uint32_t hard = room - desc - 4;        /* jsb slack, commit block */
        if (hard > EXT2_JT_MAX) hard = EXT2_JT_MAX;
        j->hard = hard;
        j->cap = hard / 2 < EXT2_JT_SOFT ? hard / 2 : EXT2_JT_SOFT;
    }
    kfree(b);
    return 0;
bad:
    kfree(b);
    ext2_jnl_free(fs);
    return rc;
}

/* Revoke records seen while scanning: block -> latest revoking transaction. */
typedef struct { uint32_t blk, seq; } jnl_revoke_t;

static int jnl_revoked(const jnl_revoke_t *r, uint32_t n, uint32_t blk, uint32_t seq) {
    for (uint32_t i = 0; i < n; i++)
        if (r[i].blk == blk && (int32_t)(r[i].seq - seq) >= 0) return 1;
    return 0;
}

/* Walk the log.  pass 0: find the end, collect revokes; pass 1: replay.
 * Returns the transaction id after the last complete one, or 0 on error. */
static uint32_t ext2_jnl_scan(ext2_fs_t *fs, uint32_t start, uint32_t seq, int pass,
                              uint32_t end_seq, jnl_revoke_t *rv, uint32_t *nrv,
                              uint32_t rvcap, uint32_t *replayed, int *rv_over,
                              uint32_t *rv_over_seq) {
    /* Revoke records count only once their transaction's commit block has
     * been read and checked: those of a torn last transaction are dropped
     * (back to `committed`) at the end of pass 0. */
    uint32_t committed = *nrv;
    ext2_jnl_t *j = fs->j;
    uint32_t bs = fs->st.block_size;
    uint8_t *b = (uint8_t *)kmalloc(bs), *d = (uint8_t *)kmalloc(bs);
    if (!b || !d) { if (b) kfree(b); if (d) kfree(d); return 0; }
    uint32_t pos = start;
#define JNEXT(p) ((p) + 1 >= j->len ? j->first : (p) + 1)
    for (uint32_t guard = 0; guard < j->len; guard++) {
        if (pass == 1 && seq == end_seq) break;
        if (ext2_jnl_read(fs, pos, b) < 0) break;
        if (be32(b) != JBD2_MAGIC || be32(b + 8) != seq) break;
        uint32_t type = be32(b + 4);
        if (type == JBD2_DESCRIPTOR) {
            if (j->csum) {
                uint32_t want = be32(b + bs - 4);
                wbe32(b + bs - 4, 0);
                if (crc32c(j->seed, b, bs) != want) break;
            }
            uint32_t limit = bs - (j->csum ? 4u : 0u);
            uint32_t off = 12;
            pos = JNEXT(pos);
            while (off + j->tag_bytes <= limit) {
                const uint8_t *t = b + off;
                uint32_t blk = be32(t), flags, hi = 0, tcs;
                if (j->incompat & JBD2_INCOMPAT_CSUM3) {
                    flags = be32(t + 4);
                    hi = be32(t + 8);
                    tcs = be32(t + 12);
                } else {
                    tcs = ((uint32_t)t[4] << 8) | t[5];
                    flags = ((uint32_t)t[6] << 8) | t[7];
                    if (j->incompat & JBD2_INCOMPAT_64BIT) hi = be32(t + 8);
                }
                if (pass == 1) {
                    if (ext2_jnl_read(fs, pos, d) < 0) break;
                    int ok = !hi && blk < fs->st.blocks_count &&
                             !jnl_revoked(rv, *nrv, blk, seq);
                    if (ok && j->csum) {
                        uint8_t sq[4];
                        wbe32(sq, seq);
                        uint32_t c = crc32c(crc32c(j->seed, sq, 4), d, bs);
                        if (!(j->incompat & JBD2_INCOMPAT_CSUM3)) c &= 0xFFFFu;
                        if (c != tcs) {
                            printk("[EXT2]  %s: journal: bad checksum for block %u in "
                                   "transaction %u, not replayed\n", fs->name,
                                   (unsigned)blk, (unsigned)seq);
                            ok = 0;
                        }
                    }
                    if (ok) {
                        if (flags & JBD2_FLAG_ESCAPE) wbe32(d, JBD2_MAGIC);
                        if (ext2_raw_write_block(fs, blk, d) < 0) break;
                        (*replayed)++;
                    }
                }
                pos = JNEXT(pos);
                off += j->tag_bytes;
                if (!(flags & JBD2_FLAG_SAME_UUID)) off += 16;
                if (flags & JBD2_FLAG_LAST_TAG) break;
            }
            continue;
        }
        if (type == JBD2_COMMIT) {
            if (pass == 0 && j->csum) {
                uint32_t want = be32(b + 16);
                wbe32(b + 16, 0);
                if (crc32c(j->seed, b, bs) != want) break;   /* torn: ends here */
            }
            if (pass == 0) committed = *nrv;
            seq++;
            pos = JNEXT(pos);
            continue;
        }
        if (type == JBD2_REVOKE) {
            if (pass == 0) {
                uint32_t cnt = be32(b + 12);
                uint32_t rsz = (j->incompat & JBD2_INCOMPAT_64BIT) ? 8 : 4;
                if (cnt > bs) cnt = bs;
                for (uint32_t o = 16; o + rsz <= cnt; o += rsz) {
                    uint32_t blk = rsz == 8 ? be32(b + o + 4) : be32(b + o);
                    if (rsz == 8 && be32(b + o)) continue;
                    if (*nrv == rvcap) {           /* never dropped: see recover */
                        if (!*rv_over) { *rv_over = 1; *rv_over_seq = seq; }
                        continue;
                    }
                    rv[*nrv].blk = blk;
                    rv[*nrv].seq = seq;
                    (*nrv)++;
                }
            }
            pos = JNEXT(pos);
            continue;
        }
        break;
    }
#undef JNEXT
    if (pass == 0) *nrv = committed;
    kfree(b);
    kfree(d);
    return seq;
}

/* Replay a journal that is not empty.  0 on success. */
static int ext2_jnl_recover(ext2_fs_t *fs) {
    ext2_jnl_t *j = fs->j;
    uint8_t *b = (uint8_t *)kmalloc(fs->st.block_size);
    if (!b) return -12;
    if (ext2_jnl_read(fs, 0, b) < 0) { kfree(b); return -5; }
    uint32_t start = be32(b + 0x1C), seq = be32(b + 0x18);
    kfree(b);
    if (!start) return 0;
    uint32_t rvcap = 16384, nrv = 0, replayed = 0, over_seq = 0;
    int over = 0;
    jnl_revoke_t *rv = (jnl_revoke_t *)kmalloc(rvcap * sizeof(*rv));
    if (!rv) return -12;
    uint32_t end = ext2_jnl_scan(fs, start, seq, 0, 0, rv, &nrv, rvcap, &replayed,
                                 &over, &over_seq);
    if (over && (int32_t)(over_seq - end) < 0) {
        /* A committed revoke that did not fit: replaying without it could
         * write a stale block over a reused one.  Refuse instead. */
        kfree(rv);
        printk("[EXT2]  %s: journal has more than %u revoke records; not replaying "
               "(run e2fsck)\n", fs->name, (unsigned)rvcap);
        return -117;
    }
    if (end != seq) ext2_jnl_scan(fs, start, seq, 1, end, rv, &nrv, rvcap, &replayed,
                                  &over, &over_seq);
    kfree(rv);
    printk("[EXT2]  %s: journal replayed: transactions %u..%u, %u blocks\n", fs->name,
           (unsigned)seq, (unsigned)(end - 1), (unsigned)replayed);
    j->seq = end;
    return ext2_jnl_sb(fs, 0, end);
}

/* Every operation that can change the filesystem ends with ext2_op_end. */
static int ext2_create(vfs_node_t *dir, const char *name, uint32_t flags) {
    ext2_fs_t *fs = (dir && dir->private) ? ((ext2_priv_t *)dir->private)->fs
                                        : (ext2_fs_t *)0;
    if (fs) ext2_lock(fs);
    int r = ext2_create_do(dir, name, flags);
    if (fs) {
        ext2_op_end(fs);
        ext2_unlock(fs);
    }
    return r;
}

static int ext2_symlink(vfs_node_t *dir, const char *name, const char *target) {
    ext2_fs_t *fs = (dir && dir->private) ? ((ext2_priv_t *)dir->private)->fs
                                        : (ext2_fs_t *)0;
    if (fs) ext2_lock(fs);
    int r = ext2_symlink_do(dir, name, target);
    if (fs) {
        ext2_op_end(fs);
        ext2_unlock(fs);
    }
    return r;
}

static int ext2_unlink(vfs_node_t *dir, const char *name) {
    ext2_fs_t *fs = (dir && dir->private) ? ((ext2_priv_t *)dir->private)->fs
                                        : (ext2_fs_t *)0;
    if (fs) ext2_lock(fs);
    if (fs && fs->crash_unlink) {
        fs->crash_unlink = 0;
        fs->crash_test = 2;
    }
    int r = ext2_unlink_do(dir, name);
    if (fs) {
        ext2_op_end(fs);
        ext2_unlock(fs);
    }
    return r;
}

static int ext2_rename(vfs_node_t *old_dir, const char *old_name,
                       vfs_node_t *new_dir, const char *new_name) {
    ext2_fs_t *fs = (old_dir && old_dir->private) ? ((ext2_priv_t *)old_dir->private)->fs
                                        : (ext2_fs_t *)0;
    if (fs) ext2_lock(fs);
    int r = ext2_rename_do(old_dir, old_name, new_dir, new_name);
    if (fs) {
        ext2_op_end(fs);
        ext2_unlock(fs);
    }
    return r;
}

static int ext2_setattr(vfs_node_t *node, uint32_t mode, uint32_t uid,
                        uint32_t gid) {
    ext2_fs_t *fs = (node && node->private) ? ((ext2_priv_t *)node->private)->fs
                                        : (ext2_fs_t *)0;
    if (fs) ext2_lock(fs);
    int r = ext2_setattr_do(node, mode, uid, gid);
    if (fs) {
        ext2_op_end(fs);
        ext2_unlock(fs);
    }
    return r;
}

static int ext2_settimes(vfs_node_t *node, uint32_t atime, uint32_t mtime) {
    ext2_fs_t *fs = (node && node->private) ? ((ext2_priv_t *)node->private)->fs
                                        : (ext2_fs_t *)0;
    if (fs) ext2_lock(fs);
    int r = ext2_settimes_do(node, atime, mtime);
    if (fs) {
        ext2_op_end(fs);
        ext2_unlock(fs);
    }
    return r;
}

static int ext2_link(vfs_node_t *dir, const char *name, vfs_node_t *target) {
    ext2_fs_t *fs = (dir && dir->private) ? ((ext2_priv_t *)dir->private)->fs
                                        : (ext2_fs_t *)0;
    if (fs) ext2_lock(fs);
    int r = ext2_link_do(dir, name, target);
    if (fs) {
        ext2_op_end(fs);
        ext2_unlock(fs);
    }
    return r;
}

static int ext2_truncate(vfs_node_t *node, uint32_t new_size) {
    ext2_fs_t *fs = (node && node->private) ? ((ext2_priv_t *)node->private)->fs
                                        : (ext2_fs_t *)0;
    if (fs) ext2_lock(fs);
    int r = ext2_truncate_do(node, new_size);
    if (fs) {
        ext2_op_end(fs);
        ext2_unlock(fs);
    }
    return r;
}

static uint32_t ext2_write_node(vfs_node_t *node, uint32_t offset,
                                uint32_t size, const uint8_t *buf) {
    ext2_fs_t *fs = (node && node->private) ? ((ext2_priv_t *)node->private)->fs
                                             : (ext2_fs_t *)0;
    if (fs) {
        ext2_lock(fs);
        fs->want_reclaim = 0;
    }
    uint32_t r = ext2_write_node_do(node, offset, size, buf);
    if (fs && fs->want_reclaim && r != VFS_WRITE_ENOMEM && r < size && !fs->ro) {
        /* Out of space but for blocks freed by the open transaction: the
         * write so far is a complete short write, so commit it and go on. */
        fs->want_reclaim = 0;
        ext2_sync_fs(fs);
        uint32_t r2 = ext2_write_node_do(node, offset + r, size - r, buf + r);
        if (r2 != VFS_WRITE_ENOMEM) r += r2;
    }
    if (fs) {
        if (fs->crash_test == 1 && r) fs->crash_test = 2;
        ext2_op_end(fs);
        ext2_unlock(fs);
    }
    return r;
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

/* ext4 (see the sections above).  Incompatible features this driver reads
 * and writes: */
#define EXT2_INCOMPAT_EXTENTS      0x0040u
#define EXT2_INCOMPAT_64BIT        0x0080u
#define EXT2_INCOMPAT_FLEX_BG      0x0200u
#define EXT2_INCOMPAT_CSUM_SEED    0x2000u
#define EXT2_INCOMPAT_LARGEDIR     0x4000u
#define X4_INCOMPAT_OK  (EXT2_INCOMPAT_FILETYPE | EXT2_INCOMPAT_RECOVER | EXT2_INCOMPAT_EXTENTS | \
                         EXT2_INCOMPAT_64BIT | EXT2_INCOMPAT_FLEX_BG | EXT2_INCOMPAT_CSUM_SEED | \
                         EXT2_INCOMPAT_LARGEDIR)
#define EXT2_RO_COMPAT_HUGE_FILE   0x0008u
#define EXT2_RO_COMPAT_GDT_CSUM    0x0010u
#define EXT2_RO_COMPAT_EXTRA_ISIZE 0x0040u
#define EXT2_RO_COMPAT_METADATA_CSUM 0x0400u
/* Read-only-compatible features kept up to date when writing.  Not here,
 * so read-only: quota and project (their inodes would go stale), bigalloc,
 * orphan_present (orphans waiting in the orphan file), verity, ... */
#define X4_RO_COMPAT_RW (EXT2_RO_COMPAT_RW | EXT2_RO_COMPAT_HUGE_FILE | EXT2_RO_COMPAT_GDT_CSUM | \
                         EXT2_RO_COMPAT_EXTRA_ISIZE | EXT2_RO_COMPAT_METADATA_CSUM)
/* Compatible features that still change where things are: sparse_super2
 * (backup groups), exclude_bitmap. */
#define X4_COMPAT_NOT_RW 0x0300u
#define SB_JNL_DEV_INCOMPAT 0x0008u
#define EXT2_COMPAT_HAS_JOURNAL 0x0004u

/* ext3/ext4 state of a mount(2) instance from its superblock: checksum
 * seed, descriptor size, the journal (replayed here when a read-write mount
 * finds it not empty).  0, or a negative errno. */
static int x4_setup(ext2_fs_t *fs, uint8_t *sb_buf, int ro) {
    uint32_t inc = fs->incompat;
    fs->x4 = (inc & ~(EXT2_INCOMPAT_FILETYPE | EXT2_INCOMPAT_RECOVER)) ||
             (fs->ro_compat & (EXT2_RO_COMPAT_HUGE_FILE | EXT2_RO_COMPAT_EXTRA_ISIZE |
                               EXT2_RO_COMPAT_METADATA_CSUM | EXT2_RO_COMPAT_GDT_CSUM)) ||
             (fs->compat & EXT2_COMPAT_HAS_JOURNAL);
    fs->desc_size = 32;
    if (!fs->x4) {
        if (inc & EXT2_INCOMPAT_RECOVER) return -117;
        return 0;
    }
    if (inc & EXT2_INCOMPAT_64BIT) {
        uint32_t ds = x4_rd16(sb_buf + SB_DESC_SIZE);
        if (ds < 32 || ds > 1024 || (ds & (ds - 1))) return -22;
        fs->desc_size = ds;
        if (ds > sizeof(ext2_bgd_t)) {
            /* Descriptors are read and checksummed through the 64-byte
             * ext2_bgd_t: a bigger one could be read, not written. */
            fs->rw_ok = 0;
            if (!ro) {
                printk("[EXT2]  %s: %u-byte group descriptors; read-only only\n",
                       fs->name, (unsigned)ds);
                return -30;
            }
        }
    }
    fs->csum = (fs->ro_compat & EXT2_RO_COMPAT_METADATA_CSUM) != 0;
    fs->extents = (inc & EXT2_INCOMPAT_EXTENTS) != 0;
    fs->csum_seed = (inc & EXT2_INCOMPAT_CSUM_SEED) ? x4_rd32(sb_buf + SB_CSUM_SEED)
                                                    : crc32c(~0u, sb_buf + SB_UUID, 16);
    fs->groups = ext2_group_count(fs);
    fs->gdt_blocks = (fs->groups * fs->desc_size + fs->st.block_size - 1) / fs->st.block_size;
    fs->rsv_gdt = x4_rd16(sb_buf + SB_RSV_GDT);
    if (fs->st.inode_size > 128) {
        uint32_t want = x4_rd16(sb_buf + SB_WANT_EXTRA), min = x4_rd16(sb_buf + 0x15C);
        if (want < min) want = min;
        if (!want) want = 32;
        if (want > fs->st.inode_size - 128) want = fs->st.inode_size - 128;
        fs->extra_isize = (uint16_t)(want & ~3u);
    }
    if (fs->csum && crc32c(~0u, sb_buf, SB_CHECKSUM) != x4_rd32(sb_buf + SB_CHECKSUM)) {
        printk("[EXT2]  %s: superblock checksum mismatch\n", fs->name);
        return -22;
    }

    if (fs->compat & EXT2_COMPAT_HAS_JOURNAL) {
        uint32_t jino = x4_rd32(sb_buf + SB_JOURNAL_INUM);
        if (!jino || (inc & SB_JNL_DEV_INCOMPAT)) {
            printk("[EXT2]  %s: external journal; not mounting here\n", fs->name);
            return -95;
        }
        int r = ext2_jnl_open(fs, jino);
        if (r < 0) {
            if (inc & EXT2_INCOMPAT_RECOVER) return -117;
            if (!ro) return -30;              /* unknown journal: never write */
        }
        if (fs->j) {
            uint8_t b[64];
            uint32_t start = 0;
            {
                uint8_t *jb = (uint8_t *)kmalloc(fs->st.block_size);
                if (!jb) return -12;
                if (ext2_jnl_read(fs, 0, jb) < 0) { kfree(jb); return -5; }
                memcpy(b, jb, sizeof(b));
                kfree(jb);
                start = be32(b + 0x1C);
            }
            if (start || (inc & EXT2_INCOMPAT_RECOVER)) {
                if (ro) {
                    printk("[EXT2]  %s: journal needs recovery; mount read-write to "
                           "replay it\n", fs->name);
                    return -117;
                }
                fs->ro = 0;
                r = ext2_jnl_recover(fs);
                if (r < 0) { fs->ro = 1; return r; }
                /* The replay may have rewritten the superblock itself. */
                if (ext2_dev_read(fs, fs->st.lba_offset + 2, 2, sb_buf) < 0) {
                    fs->ro = 1;
                    return -5;
                }
                fs->ro = 1;
                fs->was_clean = 1;           /* the replay made it consistent */
                x4_wr16(sb_buf + SB_STATE, (uint16_t)(x4_rd16(sb_buf + SB_STATE) | EXT2_VALID_FS));
            }
        }
    } else if (inc & EXT2_INCOMPAT_RECOVER) {
        return -117;
    }
    memcpy(fs->sb, sb_buf, 1024);
    x4_wr32(fs->sb + SB_INCOMPAT, x4_rd32(fs->sb + SB_INCOMPAT) & ~EXT2_INCOMPAT_RECOVER);
    fs->incompat &= ~EXT2_INCOMPAT_RECOVER;
    return 0;
}

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
    ext2_jnl_free(fs);
    kfree(fs);
}

/* Record a read-write mount in the superblock (mount count, mount time, and
 * "not clean" until it is unmounted), or its end (write time, and clean again
 * if it was clean when mounted).  e2fsck then knows a filesystem whose
 * machine stopped while it was mounted needs checking. */
static int ext2_sb_mark(ext2_fs_t *fs, int mounting) {
    if (fs->x4) {
        /* Whatever the last operation left is committed and in place first,
         * so the journal is empty when needs_recovery goes. */
        ext2_sync_fs(fs);
        uint16_t st = x4_rd16(fs->sb + SB_STATE);
        uint32_t inc = x4_rd32(fs->sb + SB_INCOMPAT);
        if (mounting) {
            x4_wr32(fs->sb + SB_MTIME, ext2_now());
            x4_wr16(fs->sb + SB_MNT_COUNT, (uint16_t)(x4_rd16(fs->sb + SB_MNT_COUNT) + 1));
            st &= (uint16_t)~EXT2_VALID_FS;
            if (fs->j) inc |= EXT2_INCOMPAT_RECOVER;
        } else {
            x4_wr32(fs->sb + SB_WTIME, ext2_now());
            if (fs->was_clean) st |= EXT2_VALID_FS;
            inc &= ~EXT2_INCOMPAT_RECOVER;
        }
        x4_wr16(fs->sb + SB_STATE, st);
        x4_wr32(fs->sb + SB_INCOMPAT, inc);
        return x4_sb_write(fs);
    }
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

    uint32_t bad = fs->incompat & ~X4_INCOMPAT_OK;
    if (x4_rd32(sb_buf + SB_BLOCKS_HI)) bad |= EXT2_INCOMPAT_64BIT;   /* > 2^32 blocks */
    if (bad) {
        /* Not a layout this driver reads; the ext4 driver may (read-only). */
        printk("[EXT2]  %s: incompatible features 0x%x%s; not for the ext2 driver\n",
               fs->name, (unsigned)bad,
               (bad & EXT2_INCOMPAT_RECOVER) ? " (journal needs recovery)" : "");
        rc = (bad & EXT2_INCOMPAT_RECOVER) ? -117 : -95;   /* -EUCLEAN / -EOPNOTSUPP */
        goto fail;
    }
    fs->rw_ok = !(fs->ro_compat & ~X4_RO_COMPAT_RW) &&
                !(fs->compat & X4_COMPAT_NOT_RW) &&
                /* gdt_csum's crc16 descriptors are not written here */
                !((fs->ro_compat & EXT2_RO_COMPAT_GDT_CSUM) &&
                  !(fs->ro_compat & EXT2_RO_COMPAT_METADATA_CSUM));
    if (!ro && !fs->rw_ok) {
        printk("[EXT2]  %s: features 0x%x/0x%x; read-only only\n",
               fs->name, (unsigned)(fs->compat & X4_COMPAT_NOT_RW),
               (unsigned)(fs->ro_compat & ~X4_RO_COMPAT_RW));
        rc = -30;                                           /* -EROFS */
        goto fail;
    }
    rc = x4_setup(fs, sb_buf, ro);
    if (rc < 0) goto fail;
    rc = -22;
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
    printk("[EXT2]  %s: mounted %s (block %u, %u inodes%s%s%s%s)\n", fs->name,
           ro ? "read-only" : "read-write", (unsigned)fs->st.block_size,
           (unsigned)fs->st.inodes_count,
           fs->j ? ", journal" : "", fs->extents ? ", extents" : "",
           fs->csum ? ", metadata_csum" : "",
           (fs->incompat & EXT2_INCOMPAT_64BIT) ? ", 64bit" : "");
    ext2_jfs_add(fs);
    *root_out = root;
    *fs_out = fs;
    return 0;

fail:
    ext2_free_fs(fs);
    return rc;
}

/* Test hook (mount -o x4crash, smoke-ext4rw): the commit that ends the first
 * write(2) on the instance stops short of the checkpoint, and nothing is
 * written after it, as if the machine had lost power right there.  The
 * journal is left for the next mount, or e2fsck, to replay. */
void ext2_test_crash(ext2_fs_t *fs) {
    if (fs->j) {
        fs->crash_test = 1;
        printk("[EXT2]  %s: x4crash armed\n", fs->name);
    }
}

/* More test hooks (smoke-ext4rw): x4smalltxn makes every transaction past
 * two blocks due at the next safe point, so writes and frees commit in many
 * steps; x4crashunlink loses power at the first commit inside the next
 * unlink(2), i.e. between two steps of freeing a file. */
void ext2_test_opt(ext2_fs_t *fs, const char *opt) {
    if (!fs->j) return;
    if (strcmp(opt, "x4smalltxn") == 0) {
        fs->j->cap = 2;
        printk("[EXT2]  %s: x4smalltxn: commits at 2 blocks\n", fs->name);
    } else if (strcmp(opt, "x4crashunlink") == 0) {
        fs->crash_unlink = 1;
        printk("[EXT2]  %s: x4crashunlink armed\n", fs->name);
    }
}

static int ext2_set_ro_locked(ext2_fs_t *fs, int ro);

int ext2_busy(void *p) {
    return ((ext2_fs_t *)p)->open_refs > 0;
}

int ext2_set_ro(void *p, int ro) {
    ext2_fs_t *fs = (ext2_fs_t *)p;
    ext2_lock(fs);
    int r = ext2_set_ro_locked(fs, ro);
    ext2_unlock(fs);
    return r;
}

static int ext2_set_ro_locked(ext2_fs_t *fs, int ro) {
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
    ext2_jfs_del(fs);
    ext2_lock(fs);
    if (!fs->ro) ext2_sb_mark(fs, 0);
    ext2_unlock(fs);
    if (fs->j) printk("[EXT2]  %s: unmounted (%u journal commits)\n", fs->name,
                      (unsigned)fs->j->commits);
    else printk("[EXT2]  %s: unmounted\n", fs->name);
    ext2_free_fs(fs);
}
