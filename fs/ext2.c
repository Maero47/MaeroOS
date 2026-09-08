#include "ext2.h"
#include "vfs.h"
#include "../drivers/ata.h"
#include "../mm/heap.h"
#include "../mm/pmm.h"
#include <kernel/config.h>
#include "../lib/string.h"
#include "../kernel/printk.h"
#include "../arch/i686/cpu/pit.h"
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

/* Only one ext2 instance for now */
static ext2_state_t g_state;
static int g_mounted = 0;

/* ── Block cache ──────────────────────────────────────────────────────────────
 * Every block read that misses costs one ATA PIO transaction, and under KVM
 * every port access in that transaction is a VM exit into QEMU (~2 us).  The
 * cache this replaced held 8 blocks, i.e. less than one page fault's worth: a
 * 4 KiB fault on libxul.so reads the group descriptor, the inode block, up to
 * three indirect blocks and four data blocks, so the metadata was evicted by
 * the data of the very fault that needed it and re-read from disk every time.
 *
 * Set-associative (bucket = block number mod EXT2_CACHE_BUCKETS, LRU within the
 * bucket), so a large cache costs no more per lookup than the old linear scan
 * over 8 slots.  Slot buffers are allocated on first use and the total is
 * capped at a share of the free physical memory measured when the filesystem is
 * mounted, so the cache is large on the 2 GiB machine that runs the browser and
 * stays small on a 128 MiB one.  Once the budget is reached a claim recycles a
 * slot that already owns a buffer instead of allocating another. */
/* Most blocks that a run of consecutive file blocks can carry in one ATA
 * transaction.  128 KiB is 256 sectors, past what one command can express;
 * 64 blocks keeps a transaction's interrupts-off window near 100 us. */
#define EXT2_READ_CLUSTER  64

#define EXT2_CACHE_BUCKETS 8192                 /* power of two */
#define EXT2_CACHE_WAYS    4
#define EXT2_CACHE_SLOTS   (EXT2_CACHE_BUCKETS * EXT2_CACHE_WAYS)

/* Share of free physical memory the cache may hold, and a floor so that a tiny
 * machine still gets a useful one. */
#define EXT2_CACHE_MEM_SHARE  8                 /* one eighth of free RAM */
#define EXT2_CACHE_MIN_BYTES  (256u * 1024u)

static uint32_t g_cache_budget;                 /* bytes the cache may allocate */
static uint32_t g_cache_bytes;                  /* bytes it has allocated       */

typedef struct {
    uint32_t blk;
    uint32_t age;
    int valid;
    uint8_t *data;
} ext2_cache_entry_t;

static ext2_cache_entry_t g_cache[EXT2_CACHE_SLOTS];
static uint32_t g_cache_age = 1;
static int g_cache_ready = 0;

static inline ext2_cache_entry_t *ext2_cache_set(uint32_t blk) {
    return &g_cache[(blk & (EXT2_CACHE_BUCKETS - 1)) * EXT2_CACHE_WAYS];
}

/* The slot holding `blk`, or NULL. */
static ext2_cache_entry_t *ext2_cache_find(uint32_t blk) {
    ext2_cache_entry_t *set = ext2_cache_set(blk);
    for (int i = 0; i < EXT2_CACHE_WAYS; i++)
        if (set[i].valid && set[i].blk == blk) return &set[i];
    return (ext2_cache_entry_t *)0;
}

/* A slot in blk's set to (re)use: its own slot, then a free one, then the LRU.
 * Returns NULL if the buffer could not be allocated (the cache then just
 * misses, which is correct, only slower). */
static ext2_cache_entry_t *ext2_cache_claim(uint32_t blk) {
    ext2_cache_entry_t *set = ext2_cache_set(blk);
    ext2_cache_entry_t *empty = (ext2_cache_entry_t *)0;   /* no buffer yet */
    ext2_cache_entry_t *lru   = (ext2_cache_entry_t *)0;   /* LRU that has one */
    for (int i = 0; i < EXT2_CACHE_WAYS; i++) {
        if (set[i].valid && set[i].blk == blk) return &set[i];   /* already ours */
        if (set[i].data) {
            if (!lru || set[i].age < lru->age) lru = &set[i];
        } else if (!empty) {
            empty = &set[i];
        }
    }
    /* Take an empty slot while there is budget to back it; past the budget,
     * recycle a slot that already owns a buffer rather than growing. */
    int can_grow = empty && g_cache_bytes + g_state.block_size <= g_cache_budget;
    ext2_cache_entry_t *slot = can_grow ? empty : (lru ? lru : empty);
    if (!slot) return (ext2_cache_entry_t *)0;
    if (!slot->data) {
        if (g_cache_bytes + g_state.block_size > g_cache_budget)
            return (ext2_cache_entry_t *)0;
        slot->data = (uint8_t *)kmalloc(g_state.block_size);
        if (!slot->data) { slot->valid = 0; return (ext2_cache_entry_t *)0; }
        g_cache_bytes += g_state.block_size;
    }
    return slot;
}

/* Per-node private data */
typedef struct { uint32_t ino; } ext2_priv_t;

static vfs_node_t *ext2_finddir(vfs_node_t *dir, const char *name);
static int ext2_readdir(vfs_node_t *dir, uint32_t req_idx,
                         vfs_dirent_t *out);
static int ext2_create(vfs_node_t *dir, const char *name, uint32_t flags);
static int ext2_truncate(vfs_node_t *node, uint32_t new_size);
static int ext2_setattr(vfs_node_t *node, uint32_t mode, uint32_t uid,
                        uint32_t gid);
static int ext2_unlink(vfs_node_t *dir, const char *name);
static int ext2_free_block(uint32_t blk);

/* ── Block I/O ────────────────────────────────────────────────────────────── */

static uint32_t ext2_now(void) {
    return pit_ticks() / 100U;
}

static int ext2_raw_read_block(uint32_t blk, void *buf) {
    uint32_t lba = g_state.lba_offset + blk * g_state.sectors_per_block;
    /* Read sectors_per_block sectors; handle block sizes > 255*512 by looping */
    if (g_state.sectors_per_block <= 255) {
        return ata_read(lba, (uint8_t)g_state.sectors_per_block, buf);
    }
    /* Large blocks: read in 128-sector (64 KiB) chunks */
    uint8_t *p = (uint8_t *)buf;
    uint32_t rem = g_state.sectors_per_block;
    while (rem > 0) {
        uint8_t n = (rem > 128) ? 128 : (uint8_t)rem;
        if (ata_read(lba, n, p) < 0) return -1;
        lba += n; p += n * 512; rem -= n;
    }
    return 0;
}

/* Read `n` physically consecutive blocks in ONE ATA transaction.  A transaction
 * costs ~12 port accesses before the first sector moves, so reading the four
 * 1 KiB blocks of a page fault separately paid that fixed cost four times.
 * `n` is bounded by the caller (EXT2_READ_CLUSTER), and the sector count of one
 * ATA command by 255. */
static int ext2_raw_read_blocks(uint32_t blk, uint32_t n, void *buf) {
    uint32_t lba  = g_state.lba_offset + blk * g_state.sectors_per_block;
    uint32_t rem  = n * g_state.sectors_per_block;
    uint8_t *p    = (uint8_t *)buf;
    while (rem > 0) {
        uint8_t k = (rem > 128) ? 128 : (uint8_t)rem;
        if (ata_read(lba, k, p) < 0) return -1;
        lba += k; p += (uint32_t)k * 512; rem -= k;
    }
    return 0;
}

static int ext2_raw_write_block(uint32_t blk, const void *buf) {
    uint32_t lba = g_state.lba_offset + blk * g_state.sectors_per_block;
    if (g_state.sectors_per_block <= 255) {
        return ata_write(lba, (uint8_t)g_state.sectors_per_block, buf);
    }

    const uint8_t *p = (const uint8_t *)buf;
    uint32_t rem = g_state.sectors_per_block;
    while (rem > 0) {
        uint8_t n = (rem > 128) ? 128 : (uint8_t)rem;
        if (ata_write(lba, n, p) < 0) return -1;
        lba += n;
        p += n * 512;
        rem -= n;
    }
    return 0;
}

static void ext2_cache_init(void) {
    g_cache_ready = 0;
    g_cache_age = 1;
    for (uint32_t i = 0; i < EXT2_CACHE_SLOTS; i++) {
        g_cache[i].blk = 0;
        g_cache[i].age = 0;
        g_cache[i].valid = 0;
        /* Buffers are allocated on demand in ext2_cache_claim(); a remount
         * keeps the ones already allocated, and g_cache_bytes still counts
         * them, so the budget is not double-spent. */
    }
    uint64_t share = ((uint64_t)pmm_free_frames() * PAGE_SIZE) / EXT2_CACHE_MEM_SHARE;
    uint64_t cap   = (uint64_t)EXT2_CACHE_SLOTS * g_state.block_size;
    if (share > cap) share = cap;
    if (share < EXT2_CACHE_MIN_BYTES) share = EXT2_CACHE_MIN_BYTES;
    g_cache_budget = (uint32_t)share;
    printk("[EXT2] block cache up to %u KiB (%u slots of %u B)\n",
           (unsigned)(g_cache_budget / 1024u), (unsigned)EXT2_CACHE_SLOTS,
           (unsigned)g_state.block_size);
    g_cache_ready = 1;
}

static void ext2_cache_insert(uint32_t blk, const void *buf);

/* Copy `blk` out of the cache if it is there.  1 on a hit, 0 on a miss. */
static int ext2_cache_lookup(uint32_t blk, void *buf) {
    int hit = 0;
    kprof_count(KPE_EXT2_BLK);
    preempt_disable();
    if (g_cache_ready) {
        ext2_cache_entry_t *e = ext2_cache_find(blk);
        if (e) {
            memcpy(buf, e->data, g_state.block_size);
            e->age = g_cache_age++;
            hit = 1;
        }
    }
    preempt_enable();
    kprof_count(hit ? KPE_EXT2_HIT : KPE_EXT2_MISS);
    return hit;
}

static int ext2_read_block(uint32_t blk, void *buf) {
    /* The block cache (g_cache) is shared mutable state.  Without this guard a
     * directory lookup preempted mid-memcpy (copying a cached block into buf)
     * can have its source slot evicted+overwritten by another thread's ext2
     * read, so it resumes copying a DIFFERENT block's bytes → corrupt directory
     * data → spurious ENOENT on a file that exists (the intermittent
     * "/disk/shell not found" boot flake and flaky smoke-disk).  Serialize the
     * whole cache access (the raw ATA read already runs with IRQs off). */
    kprof_count(KPE_EXT2_BLK);
    preempt_disable();
    if (g_cache_ready) {
        ext2_cache_entry_t *hit = ext2_cache_find(blk);
        if (hit) {
            memcpy(buf, hit->data, g_state.block_size);
            hit->age = g_cache_age++;
            preempt_enable();
            kprof_count(KPE_EXT2_HIT);
            return 0;
        }
    }

    kprof_count(KPE_EXT2_MISS);
    if (ext2_raw_read_block(blk, buf) < 0) {
        preempt_enable();
        return -1;
    }
    ext2_cache_insert(blk, buf);
    preempt_enable();
    return 0;
}

/* Publish `buf` as the cached contents of `blk`.  Caller holds preempt_disable
 * (the slot buffers are shared mutable state). */
static void ext2_cache_insert(uint32_t blk, const void *buf) {
    if (!g_cache_ready) return;
    ext2_cache_entry_t *slot = ext2_cache_claim(blk);
    if (!slot) return;
    memcpy(slot->data, buf, g_state.block_size);
    slot->blk = blk;
    slot->age = g_cache_age++;
    slot->valid = 1;
}

static int ext2_write_block(uint32_t blk, const void *buf) {
    if (ext2_raw_write_block(blk, buf) < 0)
        return -1;

    preempt_disable();   /* same shared-cache hazard as ext2_read_block */
    ext2_cache_insert(blk, buf);
    preempt_enable();
    return 0;
}

static int ext2_update_super_free_counts(int block_delta, int inode_delta) {
    uint8_t sb_buf[2048];
    if (ata_read(g_state.lba_offset + 2, 4, sb_buf) < 0)
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
    return ata_write(g_state.lba_offset + 2, 4, sb_buf);
}

/* ── Inode reading ────────────────────────────────────────────────────────── */

static int ext2_read_bgd(uint32_t grp, ext2_bgd_t *out) {
    /* The block-group-descriptor table can span MULTIPLE blocks: one block holds
     * only block_size/32 descriptors (32 for 1024-byte blocks).  Read the block
     * that actually CONTAINS group `grp`, not always the first — indexing the
     * first block by grp*32 overruns the buffer for grp >= per_block (read past
     * the kmalloc'd block → not-present fault). */
    uint32_t per_block = g_state.block_size / sizeof(ext2_bgd_t);
    uint32_t bgd_blk   = g_state.first_data_block + 1 + (per_block ? grp / per_block : 0);
    uint32_t idx       = per_block ? grp % per_block : grp;
    uint8_t *bgd_block = (uint8_t *)kmalloc(g_state.block_size);
    if (!bgd_block) return -1;
    if (ext2_read_block(bgd_blk, bgd_block) < 0) {
        kfree(bgd_block);
        return -1;
    }
    memcpy(out, bgd_block + idx * sizeof(ext2_bgd_t), sizeof(ext2_bgd_t));
    kfree(bgd_block);
    return 0;
}

static int ext2_write_bgd(uint32_t grp, const ext2_bgd_t *in) {
    /* Same multi-block bgd-table handling as ext2_read_bgd. */
    uint32_t per_block = g_state.block_size / sizeof(ext2_bgd_t);
    uint32_t bgd_blk   = g_state.first_data_block + 1 + (per_block ? grp / per_block : 0);
    uint32_t idx       = per_block ? grp % per_block : grp;
    uint8_t *bgd_block = (uint8_t *)kmalloc(g_state.block_size);
    if (!bgd_block) return -1;
    if (ext2_read_block(bgd_blk, bgd_block) < 0) {
        kfree(bgd_block);
        return -1;
    }
    memcpy(bgd_block + idx * sizeof(ext2_bgd_t), in, sizeof(ext2_bgd_t));
    int r = ext2_write_block(bgd_blk, bgd_block);
    kfree(bgd_block);
    return r;
}

static int ext2_read_inode(uint32_t ino, ext2_inode_t *out) {
    uint32_t grp = (ino - 1) / g_state.inodes_per_group;
    uint32_t idx = (ino - 1) % g_state.inodes_per_group;

    ext2_bgd_t bgd;
    if (ext2_read_bgd(grp, &bgd) < 0) return -1;

    /* Read inode from inode table */
    uint32_t inodes_per_block = g_state.block_size / g_state.inode_size;
    uint32_t blk = bgd.bg_inode_table + idx / inodes_per_block;
    uint32_t off = (idx % inodes_per_block) * g_state.inode_size;

    uint8_t *blk_buf = (uint8_t *)kmalloc(g_state.block_size);
    if (!blk_buf) return -1;
    if (ext2_read_block(blk, blk_buf) < 0) { kfree(blk_buf); return -1; }
    memcpy(out, blk_buf + off, sizeof(ext2_inode_t));
    kfree(blk_buf);
    return 0;
}

static int ext2_write_inode(uint32_t ino, const ext2_inode_t *in) {
    uint32_t grp = (ino - 1) / g_state.inodes_per_group;
    uint32_t idx = (ino - 1) % g_state.inodes_per_group;

    ext2_bgd_t bgd;
    if (ext2_read_bgd(grp, &bgd) < 0) return -1;

    uint32_t inodes_per_block = g_state.block_size / g_state.inode_size;
    uint32_t blk = bgd.bg_inode_table + idx / inodes_per_block;
    uint32_t off = (idx % inodes_per_block) * g_state.inode_size;

    uint8_t *blk_buf = (uint8_t *)kmalloc(g_state.block_size);
    if (!blk_buf) return -1;
    if (ext2_read_block(blk, blk_buf) < 0) {
        kfree(blk_buf);
        return -1;
    }
    memcpy(blk_buf + off, in, sizeof(ext2_inode_t));
    int r = ext2_write_block(blk, blk_buf);
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

static uint32_t ext2_group_count(void) {
    return (g_state.blocks_count + g_state.blocks_per_group - 1) /
           g_state.blocks_per_group;
}

static uint32_t ext2_alloc_inode(void) {
    uint32_t groups = ext2_group_count();
    uint8_t *bitmap = (uint8_t *)kmalloc(g_state.block_size);
    if (!bitmap) return 0;

    for (uint32_t grp = 0; grp < groups; grp++) {
        ext2_bgd_t bgd;
        if (ext2_read_bgd(grp, &bgd) < 0) continue;
        if (ext2_read_block(bgd.bg_inode_bitmap, bitmap) < 0) continue;

        for (uint32_t i = 0; i < g_state.inodes_per_group; i++) {
            uint32_t ino = grp * g_state.inodes_per_group + i + 1;
            if (ino < g_state.first_ino || ino > g_state.inodes_count) continue;
            if (ext2_bitmap_test(bitmap, i)) continue;

            ext2_bitmap_set(bitmap, i);
            if (ext2_write_block(bgd.bg_inode_bitmap, bitmap) < 0) {
                kfree(bitmap);
                return 0;
            }
            if (bgd.bg_free_inodes_count) bgd.bg_free_inodes_count--;
            ext2_write_bgd(grp, &bgd);
            ext2_update_super_free_counts(0, -1);
            kfree(bitmap);
            return ino;
        }
    }

    kfree(bitmap);
    return 0;
}

static uint32_t ext2_alloc_block(void) {
    uint32_t groups = ext2_group_count();
    uint8_t *bitmap = (uint8_t *)kmalloc(g_state.block_size);
    uint8_t *zero = (uint8_t *)kmalloc(g_state.block_size);
    if (!bitmap || !zero) {
        if (bitmap) kfree(bitmap);
        if (zero) kfree(zero);
        return 0;
    }
    memset(zero, 0, g_state.block_size);

    for (uint32_t grp = 0; grp < groups; grp++) {
        ext2_bgd_t bgd;
        if (ext2_read_bgd(grp, &bgd) < 0) continue;
        if (ext2_read_block(bgd.bg_block_bitmap, bitmap) < 0) continue;

        for (uint32_t i = 0; i < g_state.blocks_per_group; i++) {
            uint32_t blk = g_state.first_data_block +
                           grp * g_state.blocks_per_group + i;
            if (blk < g_state.first_data_block || blk >= g_state.blocks_count)
                continue;
            if (ext2_bitmap_test(bitmap, i)) continue;

            ext2_bitmap_set(bitmap, i);
            if (ext2_write_block(bgd.bg_block_bitmap, bitmap) < 0) {
                kfree(bitmap);
                kfree(zero);
                return 0;
            }
            if (bgd.bg_free_blocks_count) bgd.bg_free_blocks_count--;
            ext2_write_bgd(grp, &bgd);
            ext2_update_super_free_counts(-1, 0);
            if (ext2_write_block(blk, zero) < 0) {
                kfree(bitmap);
                kfree(zero);
                ext2_free_block(blk);
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

static int ext2_free_inode(uint32_t ino) {
    if (ino < g_state.first_ino || ino > g_state.inodes_count) return -1;
    uint32_t grp = (ino - 1) / g_state.inodes_per_group;
    uint32_t idx = (ino - 1) % g_state.inodes_per_group;

    ext2_bgd_t bgd;
    if (ext2_read_bgd(grp, &bgd) < 0) return -1;

    uint8_t *bitmap = (uint8_t *)kmalloc(g_state.block_size);
    if (!bitmap) return -1;
    if (ext2_read_block(bgd.bg_inode_bitmap, bitmap) < 0) {
        kfree(bitmap);
        return -1;
    }
    if (ext2_bitmap_test(bitmap, idx)) {
        ext2_bitmap_clear(bitmap, idx);
        if (ext2_write_block(bgd.bg_inode_bitmap, bitmap) < 0) {
            kfree(bitmap);
            return -1;
        }
        bgd.bg_free_inodes_count++;
        ext2_write_bgd(grp, &bgd);
        ext2_update_super_free_counts(0, 1);
    }
    kfree(bitmap);
    return 0;
}

static int ext2_free_block(uint32_t blk) {
    if (blk < g_state.first_data_block || blk >= g_state.blocks_count)
        return -1;
    uint32_t grp = (blk - g_state.first_data_block) / g_state.blocks_per_group;
    uint32_t idx = (blk - g_state.first_data_block) % g_state.blocks_per_group;

    ext2_bgd_t bgd;
    if (ext2_read_bgd(grp, &bgd) < 0) return -1;

    uint8_t *bitmap = (uint8_t *)kmalloc(g_state.block_size);
    if (!bitmap) return -1;
    if (ext2_read_block(bgd.bg_block_bitmap, bitmap) < 0) {
        kfree(bitmap);
        return -1;
    }
    if (ext2_bitmap_test(bitmap, idx)) {
        ext2_bitmap_clear(bitmap, idx);
        if (ext2_write_block(bgd.bg_block_bitmap, bitmap) < 0) {
            kfree(bitmap);
            return -1;
        }
        bgd.bg_free_blocks_count++;
        ext2_write_bgd(grp, &bgd);
        ext2_update_super_free_counts(1, 0);
    }
    kfree(bitmap);
    return 0;
}

/* ── Resolve an indirect block pointer ───────────────────────────────────── */

/* Reentrant cache of recently-read indirect blocks, one slot per level
 * (0=singly, 1=doubly, 2=triply).  Lives on the caller's stack so concurrent
 * reads never share it.  Sequential reads of a large file (libxul.so = 175 MiB)
 * otherwise re-read up to 3 indirect blocks per 1 KiB data block from the PIO
 * disk — hundreds of thousands of redundant reads.  With the cache the doubly/
 * triply blocks are read once per 64 MiB and the singly once per 256 KiB. */
typedef struct {
    uint32_t blk[3];      /* cached disk block number, 0 = empty */
    uint32_t *buf[3];     /* cached block contents (block_size bytes) */
} ext2_indcache_t;

static uint32_t *ext2_ind_get(ext2_indcache_t *c, int lvl, uint32_t blk) {
    if (blk == 0) return 0;
    if (c->blk[lvl] == blk && c->buf[lvl]) return c->buf[lvl];
    if (!c->buf[lvl]) {
        c->buf[lvl] = (uint32_t *)kmalloc(g_state.block_size);
        if (!c->buf[lvl]) return 0;
    }
    if (ext2_read_block(blk, c->buf[lvl]) < 0) { c->blk[lvl] = 0; return 0; }
    c->blk[lvl] = blk;
    return c->buf[lvl];
}

/* Cached variant: resolves file-block `idx` reusing already-read indirect
 * blocks from `c`.  Equivalent to ext2_file_blk but fast for sequential scans. */
static uint32_t ext2_file_blk_cached(ext2_inode_t *ino, uint32_t idx,
                                     ext2_indcache_t *c) {
    uint32_t ppb = g_state.block_size / 4;
    if (idx < 12) return ino->i_block[idx];
    idx -= 12;
    if (idx < ppb) {                                  /* singly */
        uint32_t *ind = ext2_ind_get(c, 0, ino->i_block[12]);
        return ind ? ind[idx] : 0;
    }
    idx -= ppb;
    if (idx < ppb * ppb) {                            /* doubly */
        uint32_t *dind = ext2_ind_get(c, 1, ino->i_block[13]);
        if (!dind) return 0;
        uint32_t *ind = ext2_ind_get(c, 0, dind[idx / ppb]);
        return ind ? ind[idx % ppb] : 0;
    }
    idx -= ppb * ppb;
    if (idx < ppb * ppb * ppb) {                      /* triply */
        uint32_t per2 = ppb * ppb;
        uint32_t *tind = ext2_ind_get(c, 2, ino->i_block[14]);
        if (!tind) return 0;
        uint32_t *dind = ext2_ind_get(c, 1, tind[idx / per2]);
        if (!dind) return 0;
        uint32_t *ind = ext2_ind_get(c, 0, dind[(idx % per2) / ppb]);
        return ind ? ind[idx % ppb] : 0;
    }
    return 0;
}

static void ext2_indcache_free(ext2_indcache_t *c) {
    for (int i = 0; i < 3; i++) if (c->buf[i]) kfree(c->buf[i]);
}

/* Returns the physical block number for file-block index `idx` */
static uint32_t ext2_file_blk(ext2_inode_t *ino, uint32_t idx) {
    uint32_t ptrs_per_blk = g_state.block_size / 4;

    if (idx < 12)
        return ino->i_block[idx];

    idx -= 12;

    /* Singly indirect */
    if (idx < ptrs_per_blk) {
        if (!ino->i_block[12]) return 0;
        uint32_t *ind = (uint32_t *)kmalloc(g_state.block_size);
        if (!ind) return 0;
        if (ext2_read_block(ino->i_block[12], ind) < 0) {
            kfree(ind);
            return 0;
        }
        uint32_t blk = ind[idx];
        kfree(ind);
        return blk;
    }
    idx -= ptrs_per_blk;

    /* Doubly indirect */
    if (idx < ptrs_per_blk * ptrs_per_blk) {
        if (!ino->i_block[13]) return 0;
        uint32_t *dind = (uint32_t *)kmalloc(g_state.block_size);
        if (!dind) return 0;
        if (ext2_read_block(ino->i_block[13], dind) < 0) {
            kfree(dind);
            return 0;
        }
        uint32_t ind_blk = dind[idx / ptrs_per_blk];
        kfree(dind);
        if (!ind_blk) return 0;

        uint32_t *ind = (uint32_t *)kmalloc(g_state.block_size);
        if (!ind) return 0;
        if (ext2_read_block(ind_blk, ind) < 0) {
            kfree(ind);
            return 0;
        }
        uint32_t blk = ind[idx % ptrs_per_blk];
        kfree(ind);
        return blk;
    }
    idx -= ptrs_per_blk * ptrs_per_blk;

    /* Triply indirect.  With 1 KiB blocks the doubly-indirect range only covers
     * ~64 MiB, so large files (libxul.so is 175 MiB) MUST use this path — without
     * it, reads past 64 MiB return zeros and silently corrupt the file. */
    if (idx < ptrs_per_blk * ptrs_per_blk * ptrs_per_blk) {
        if (!ino->i_block[14]) return 0;
        uint32_t per2 = ptrs_per_blk * ptrs_per_blk;

        uint32_t *tind = (uint32_t *)kmalloc(g_state.block_size);
        if (!tind) return 0;
        if (ext2_read_block(ino->i_block[14], tind) < 0) { kfree(tind); return 0; }
        uint32_t dind_blk = tind[idx / per2];
        kfree(tind);
        if (!dind_blk) return 0;

        uint32_t *dind = (uint32_t *)kmalloc(g_state.block_size);
        if (!dind) return 0;
        if (ext2_read_block(dind_blk, dind) < 0) { kfree(dind); return 0; }
        uint32_t ind_blk = dind[(idx % per2) / ptrs_per_blk];
        kfree(dind);
        if (!ind_blk) return 0;

        uint32_t *ind = (uint32_t *)kmalloc(g_state.block_size);
        if (!ind) return 0;
        if (ext2_read_block(ind_blk, ind) < 0) { kfree(ind); return 0; }
        uint32_t blk = ind[idx % ptrs_per_blk];
        kfree(ind);
        return blk;
    }

    return 0;   /* beyond triply-indirect range (> 16 GiB with 1 KiB blocks) */
}

static uint32_t ext2_file_blk_alloc(ext2_inode_t *ino, uint32_t idx) {
    uint32_t ptrs_per_blk = g_state.block_size / 4;
    if (idx < 12) {
        if (!ino->i_block[idx]) {
            uint32_t blk = ext2_alloc_block();
            if (!blk) return 0;
            ino->i_block[idx] = blk;
            ino->i_blocks += g_state.sectors_per_block;
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

        tbl = (uint32_t *)kmalloc(g_state.block_size);
        if (!tbl) return 0;

        /* level 0: the double-indirect block itself */
        if (!ino->i_block[13]) {
            uint32_t nb = ext2_alloc_block();
            if (!nb) { kfree(tbl); return 0; }
            memset(tbl, 0, g_state.block_size);
            if (ext2_write_block(nb, tbl) < 0) {
                ext2_free_block(nb);
                kfree(tbl);
                return 0;
            }
            ino->i_block[13] = nb;
            ino->i_blocks += g_state.sectors_per_block;
        }
        if (ext2_read_block(ino->i_block[13], tbl) < 0) {
            kfree(tbl);
            return 0;
        }

        /* level 1: the indirect block for this slice */
        ind_blk = tbl[d_idx];
        if (!ind_blk) {
            uint32_t *zero = (uint32_t *)kmalloc(g_state.block_size);
            if (!zero) { kfree(tbl); return 0; }
            ind_blk = ext2_alloc_block();
            if (!ind_blk) { kfree(zero); kfree(tbl); return 0; }
            memset(zero, 0, g_state.block_size);
            if (ext2_write_block(ind_blk, zero) < 0) {
                ext2_free_block(ind_blk);
                kfree(zero);
                kfree(tbl);
                return 0;
            }
            kfree(zero);
            tbl[d_idx] = ind_blk;
            ino->i_blocks += g_state.sectors_per_block;
            if (ext2_write_block(ino->i_block[13], tbl) < 0) {
                kfree(tbl);
                return 0;
            }
        }

        /* level 2: the data block */
        if (ext2_read_block(ind_blk, tbl) < 0) {
            kfree(tbl);
            return 0;
        }
        blk = tbl[i_idx];
        if (!blk) {
            blk = ext2_alloc_block();
            if (!blk) { kfree(tbl); return 0; }
            tbl[i_idx] = blk;
            ino->i_blocks += g_state.sectors_per_block;
            if (ext2_write_block(ind_blk, tbl) < 0) {
                kfree(tbl);
                return 0;
            }
        }
        kfree(tbl);
        return blk;
    }

    uint32_t *ind = (uint32_t *)kmalloc(g_state.block_size);
    if (!ind) return 0;

    int created_ind = 0;
    if (!ino->i_block[12]) {
        uint32_t ind_blk = ext2_alloc_block();
        if (!ind_blk) {
            kfree(ind);
            return 0;
        }
        ino->i_block[12] = ind_blk;
        ino->i_blocks += g_state.sectors_per_block;
        created_ind = 1;
        memset(ind, 0, g_state.block_size);
        if (ext2_write_block(ino->i_block[12], ind) < 0) {
            ext2_free_block(ino->i_block[12]);
            ino->i_block[12] = 0;
            if (ino->i_blocks >= g_state.sectors_per_block)
                ino->i_blocks -= g_state.sectors_per_block;
            kfree(ind);
            return 0;
        }
    } else if (ext2_read_block(ino->i_block[12], ind) < 0) {
        kfree(ind);
        return 0;
    }

    if (!ind[idx]) {
        uint32_t blk = ext2_alloc_block();
        if (!blk) {
            if (created_ind) {
                ext2_free_block(ino->i_block[12]);
                ino->i_block[12] = 0;
                if (ino->i_blocks >= g_state.sectors_per_block)
                    ino->i_blocks -= g_state.sectors_per_block;
            }
            kfree(ind);
            return 0;
        }
        ind[idx] = blk;
        ino->i_blocks += g_state.sectors_per_block;
        if (ext2_write_block(ino->i_block[12], ind) < 0) {
            ext2_free_block(blk);
            if (ino->i_blocks >= g_state.sectors_per_block)
                ino->i_blocks -= g_state.sectors_per_block;
            ind[idx] = 0;
            if (created_ind) {
                ext2_free_block(ino->i_block[12]);
                ino->i_block[12] = 0;
                if (ino->i_blocks >= g_state.sectors_per_block)
                    ino->i_blocks -= g_state.sectors_per_block;
            }
            kfree(ind);
            return 0;
        }
    }
    uint32_t blk = ind[idx];
    kfree(ind);
    return blk;
}

static int ext2_file_blk_free(ext2_inode_t *ino, uint32_t idx) {
    uint32_t ptrs_per_blk = g_state.block_size / 4;

    if (idx < 12) {
        if (ino->i_block[idx]) {
            ext2_free_block(ino->i_block[idx]);
            ino->i_block[idx] = 0;
            if (ino->i_blocks >= g_state.sectors_per_block)
                ino->i_blocks -= g_state.sectors_per_block;
        }
        return 0;
    }

    idx -= 12;
    if (idx >= ptrs_per_blk || !ino->i_block[12])
        return 0;

    uint32_t *ind = (uint32_t *)kmalloc(g_state.block_size);
    if (!ind) return -1;
    if (ext2_read_block(ino->i_block[12], ind) < 0) {
        kfree(ind);
        return -1;
    }

    if (ind[idx]) {
        ext2_free_block(ind[idx]);
        ind[idx] = 0;
        if (ino->i_blocks >= g_state.sectors_per_block)
            ino->i_blocks -= g_state.sectors_per_block;
    }

    int any = 0;
    for (uint32_t i = 0; i < ptrs_per_blk; i++) {
        if (ind[i]) {
            any = 1;
            break;
        }
    }

    if (any) {
        int r = ext2_write_block(ino->i_block[12], ind);
        kfree(ind);
        return r;
    }

    kfree(ind);
    ext2_free_block(ino->i_block[12]);
    ino->i_block[12] = 0;
    if (ino->i_blocks >= g_state.sectors_per_block)
        ino->i_blocks -= g_state.sectors_per_block;
    return 0;
}

/* ── VFS read_fn for ext2 file nodes ─────────────────────────────────────── */

static uint32_t ext2_read_node(vfs_node_t *node, uint32_t offset,
                                uint32_t size, uint8_t *buf) {
    if (!g_mounted || !node->private) return 0;
    ext2_priv_t *priv = (ext2_priv_t *)node->private;

    ext2_inode_t inode;
    if (ext2_read_inode(priv->ino, &inode) < 0) return 0;

    if (offset >= inode.i_size) return 0;
    if (offset + size > inode.i_size) size = inode.i_size - offset;

    uint32_t blk_size = g_state.block_size;
    uint32_t done = 0;
    uint8_t *blk_buf = (uint8_t *)kmalloc(blk_size);
    if (!blk_buf) return 0;
    ext2_indcache_t ic = {{0,0,0},{0,0,0}};

    while (done < size) {
        uint32_t file_off   = offset + done;
        uint32_t blk_idx    = file_off / blk_size;
        uint32_t blk_off    = file_off % blk_size;
        uint32_t to_copy    = blk_size - blk_off;
        if (to_copy > size - done) to_copy = size - done;

        uint32_t blk_num = ext2_file_blk_cached(&inode, blk_idx, &ic);
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
            if (!ext2_cache_lookup(blk_num, buf + done)) {
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
                       ext2_file_blk_cached(&inode, blk_idx + run, &ic)
                           == blk_num + run)
                    run++;
                if (run > 1) {
                    if (ext2_raw_read_blocks(blk_num, run, buf + done) < 0) break;
                    preempt_disable();
                    for (uint32_t r = 0; r < run; r++)
                        ext2_cache_insert(blk_num + r, buf + done + r * blk_size);
                    preempt_enable();
                    kprof_add(KPE_EXT2_BLK, run);
                    kprof_add(KPE_EXT2_MISS, run);
                    done += run * blk_size;
                    continue;
                }
                if (ext2_read_block(blk_num, buf + done) < 0) break;
            }
        } else {
            if (ext2_read_block(blk_num, blk_buf) < 0) break;
            memcpy(buf + done, blk_buf + blk_off, to_copy);
        }
        done += to_copy;
    }
    kfree(blk_buf);
    ext2_indcache_free(&ic);
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
    if (!g_mounted || !node->private) return 0;
    ext2_priv_t *priv = (ext2_priv_t *)node->private;

    ext2_inode_t inode;
    if (ext2_read_inode(priv->ino, &inode) < 0) return 0;
    if ((inode.i_mode & EXT2_S_IFMT) != EXT2_S_IFREG) return 0;

    uint32_t end = offset + size;
    if (end < offset) return 0;

    uint32_t blk_size = g_state.block_size;
    uint32_t done = 0;
    uint8_t *blk_buf = (uint8_t *)kmalloc(blk_size);
    if (!blk_buf) return 0;

    while (done < size) {
        uint32_t file_off = offset + done;
        uint32_t blk_idx = file_off / blk_size;
        uint32_t blk_off = file_off % blk_size;
        uint32_t to_copy = blk_size - blk_off;
        if (to_copy > size - done) to_copy = size - done;

        uint32_t blk_num = ext2_file_blk_alloc(&inode, blk_idx);
        if (blk_num == 0) break;

        if (ext2_read_block(blk_num, blk_buf) < 0) break;
        memcpy(blk_buf + blk_off, buf + done, to_copy);
        if (ext2_write_block(blk_num, blk_buf) < 0) break;
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
        ext2_write_inode(priv->ino, &inode);
    }
    return done;
}

static uint16_t ext2_dir_rec_len(uint8_t name_len) {
    return (uint16_t)((8U + name_len + 3U) & ~3U);
}

static int ext2_add_dirent(uint32_t dir_ino, ext2_inode_t *dir_inode,
                            uint32_t child_ino, const char *name,
                            uint8_t file_type) {
    uint32_t name_len = strlen(name);
    if (name_len == 0 || name_len > 255) return -1;

    uint16_t need = ext2_dir_rec_len((uint8_t)name_len);
    uint8_t *blk_buf = (uint8_t *)kmalloc(g_state.block_size);
    if (!blk_buf) return -1;

    uint32_t ptrs_per_blk = g_state.block_size / 4;
    for (uint32_t blk_idx = 0; blk_idx < 12 + ptrs_per_blk; blk_idx++) {
        uint32_t blk_num = ext2_file_blk(dir_inode, blk_idx);
        if (!blk_num) {
            blk_num = ext2_file_blk_alloc(dir_inode, blk_idx);
            if (!blk_num) break;
            dir_inode->i_size += g_state.block_size;
            memset(blk_buf, 0, g_state.block_size);
            ext2_dirent_t *de = (ext2_dirent_t *)blk_buf;
            de->inode = child_ino;
            de->rec_len = (uint16_t)g_state.block_size;
            de->name_len = (uint8_t)name_len;
            de->file_type = file_type;
            memcpy(de->name, name, name_len);
            int r = ext2_write_block(blk_num, blk_buf);
            if (r == 0) ext2_write_inode(dir_ino, dir_inode);
            kfree(blk_buf);
            return r;
        }

        if (ext2_read_block(blk_num, blk_buf) < 0) break;
        uint32_t offset = 0;
        while (offset < g_state.block_size) {
            ext2_dirent_t *de = (ext2_dirent_t *)(blk_buf + offset);
            if (de->rec_len == 0) break;

            uint16_t actual = de->inode ? ext2_dir_rec_len(de->name_len) : 0;
            if (!de->inode && de->rec_len >= need) {
                de->inode = child_ino;
                de->name_len = (uint8_t)name_len;
                de->file_type = file_type;
                memcpy(de->name, name, name_len);
                int r = ext2_write_block(blk_num, blk_buf);
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
                int r = ext2_write_block(blk_num, blk_buf);
                kfree(blk_buf);
                return r;
            }
            offset += de->rec_len;
        }
    }

    kfree(blk_buf);
    return -1;
}

static int ext2_remove_dirent(ext2_inode_t *dir_inode, const char *name,
                              uint32_t *removed_ino) {
    uint32_t name_len = strlen(name);
    if (name_len == 0 || name_len > 255) return -1;

    uint8_t *blk_buf = (uint8_t *)kmalloc(g_state.block_size);
    if (!blk_buf) return -1;

    uint32_t max_blocks = (dir_inode->i_size + g_state.block_size - 1) /
                          g_state.block_size;
    for (uint32_t blk_idx = 0; blk_idx < max_blocks; blk_idx++) {
        uint32_t blk_num = ext2_file_blk(dir_inode, blk_idx);
        if (!blk_num) continue;
        if (ext2_read_block(blk_num, blk_buf) < 0) break;

        uint32_t offset = 0;
        ext2_dirent_t *prev = NULL;
        while (offset < g_state.block_size) {
            ext2_dirent_t *de = (ext2_dirent_t *)(blk_buf + offset);
            if (de->rec_len == 0) break;

            if (de->inode && de->name_len == (uint8_t)name_len &&
                memcmp(de->name, name, name_len) == 0) {
                if (removed_ino) *removed_ino = de->inode;
                if (prev) {
                    prev->rec_len += de->rec_len;
                } else {
                    de->inode = 0;
                }
                int r = ext2_write_block(blk_num, blk_buf);
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

static int ext2_dir_is_empty(ext2_inode_t *inode) {
    uint8_t *blk_buf = (uint8_t *)kmalloc(g_state.block_size);
    if (!blk_buf) return 0;

    uint32_t max_blocks = (inode->i_size + g_state.block_size - 1) /
                          g_state.block_size;
    for (uint32_t blk_idx = 0; blk_idx < max_blocks; blk_idx++) {
        uint32_t blk_num = ext2_file_blk(inode, blk_idx);
        if (!blk_num) continue;
        if (ext2_read_block(blk_num, blk_buf) < 0) {
            kfree(blk_buf);
            return 0;
        }

        uint32_t offset = 0;
        while (offset < g_state.block_size) {
            ext2_dirent_t *de = (ext2_dirent_t *)(blk_buf + offset);
            if (de->rec_len == 0) break;
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

static int ext2_init_dir_block(uint32_t blk, uint32_t self_ino,
                               uint32_t parent_ino) {
    uint8_t *buf = (uint8_t *)kmalloc(g_state.block_size);
    if (!buf) return -1;
    memset(buf, 0, g_state.block_size);

    ext2_dirent_t *dot = (ext2_dirent_t *)buf;
    dot->inode = self_ino;
    dot->rec_len = ext2_dir_rec_len(1);
    dot->name_len = 1;
    dot->file_type = 2;
    dot->name[0] = '.';

    ext2_dirent_t *dotdot = (ext2_dirent_t *)(buf + dot->rec_len);
    dotdot->inode = parent_ino;
    dotdot->rec_len = (uint16_t)(g_state.block_size - dot->rec_len);
    dotdot->name_len = 2;
    dotdot->file_type = 2;
    dotdot->name[0] = '.';
    dotdot->name[1] = '.';

    int r = ext2_write_block(blk, buf);
    kfree(buf);
    return r;
}

static void ext2_free_inode_blocks(ext2_inode_t *inode) {
    if (!inode) return;
    for (uint32_t i = 0; i < 12; i++) {
        if (inode->i_block[i]) {
            ext2_free_block(inode->i_block[i]);
            inode->i_block[i] = 0;
        }
    }

    if (inode->i_block[12]) {
        uint32_t *ind = (uint32_t *)kmalloc(g_state.block_size);
        if (ind && ext2_read_block(inode->i_block[12], ind) == 0) {
            uint32_t ptrs_per_blk = g_state.block_size / 4;
            for (uint32_t i = 0; i < ptrs_per_blk; i++) {
                if (ind[i])
                    ext2_free_block(ind[i]);
            }
        }
        if (ind) kfree(ind);
        ext2_free_block(inode->i_block[12]);
        inode->i_block[12] = 0;
    }
    inode->i_blocks = 0;
}

/* ── Helper: build a vfs_node_t from an ext2 directory entry ─────────────── */

static vfs_node_t *ext2_make_node(uint32_t ino_num, const char *name,
                                   uint8_t ftype) {
    vfs_node_t *node = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
    if (!node) return NULL;
    memset(node, 0, sizeof(vfs_node_t));

    strncpy(node->name, name, 255);
    node->name[255] = '\0';
    node->inode     = ino_num;

    /* Read inode to get size and mode */
    ext2_inode_t inode;
    if (ext2_read_inode(ino_num, &inode) < 0) {
        kfree(node);
        return NULL;
    }
    node->size  = inode.i_size;
    node->mask  = inode.i_mode & 0xFFF;
    node->uid   = inode.i_uid;
    node->gid   = inode.i_gid;
    node->atime = inode.i_atime;
    node->mtime = inode.i_mtime;
    node->ctime = inode.i_ctime;

    ext2_priv_t *priv = (ext2_priv_t *)kmalloc(sizeof(ext2_priv_t));
    if (!priv) { kfree(node); return NULL; }
    priv->ino     = ino_num;
    node->private = priv;

    uint16_t type = inode.i_mode & EXT2_S_IFMT;
    (void)ftype;

    if (type == EXT2_S_IFDIR) {
        node->flags      = VFS_FLAG_DIR;
        node->finddir_fn = ext2_finddir;
        node->readdir_fn = ext2_readdir;
        node->create_fn  = ext2_create;
        node->unlink_fn  = ext2_unlink;
    } else {
        node->flags    = VFS_FLAG_FILE;
        node->read_fn  = ext2_read_node;
        node->write_fn = ext2_write_node;
        node->truncate_fn = ext2_truncate;
    }

    node->setattr_fn = ext2_setattr;

    return node;
}

static int ext2_create(vfs_node_t *dir, const char *name, uint32_t flags) {
    if (!g_mounted || !dir || !dir->private || !name) return -1;
    if (flags != VFS_FLAG_FILE && flags != VFS_FLAG_DIR) return -22;   /* -EINVAL */
    if (ext2_finddir(dir, name)) return -17;   /* -EEXIST (callers treat EEXIST as
                                                * "already there" = OK; other errno
                                                * is fatal — see tmpfs_create note) */

    ext2_priv_t *dpriv = (ext2_priv_t *)dir->private;
    ext2_inode_t dir_inode;
    if (ext2_read_inode(dpriv->ino, &dir_inode) < 0) return -1;
    if ((dir_inode.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return -1;

    uint32_t ino = ext2_alloc_inode();
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
        uint32_t blk = ext2_alloc_block();
        if (!blk) {
            ext2_free_inode(ino);
            return -1;
        }
        if (ext2_init_dir_block(blk, ino, dpriv->ino) < 0) {
            ext2_free_block(blk);
            ext2_free_inode(ino);
            return -1;
        }
        inode.i_mode = EXT2_S_IFDIR | 0755;
        inode.i_size = g_state.block_size;
        inode.i_links_count = 2;
        inode.i_blocks = g_state.sectors_per_block;
        inode.i_block[0] = blk;
    } else {
        inode.i_mode = EXT2_S_IFREG | 0644;
        inode.i_size = 0;
        inode.i_links_count = 1;
        inode.i_blocks = 0;
    }

    if (ext2_write_inode(ino, &inode) < 0) {
        ext2_free_inode_blocks(&inode);
        ext2_free_inode(ino);
        return -1;
    }

    uint8_t ftype = (flags == VFS_FLAG_DIR) ? 2 : 1;
    if (ext2_add_dirent(dpriv->ino, &dir_inode, ino, name, ftype) < 0) {
        ext2_free_inode_blocks(&inode);
        memset(&inode, 0, sizeof(inode));
        ext2_write_inode(ino, &inode);
        ext2_free_inode(ino);
        return -1;
    }
    dir_inode.i_mtime = now;
    dir_inode.i_ctime = now;
    if (flags == VFS_FLAG_DIR) {
        dir_inode.i_links_count++;
    }
    ext2_write_inode(dpriv->ino, &dir_inode);
    return 0;
}

static int ext2_unlink(vfs_node_t *dir, const char *name) {
    if (!g_mounted || !dir || !dir->private || !name) return -1;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return -1;

    ext2_priv_t *dpriv = (ext2_priv_t *)dir->private;
    ext2_inode_t dir_inode;
    if (ext2_read_inode(dpriv->ino, &dir_inode) < 0) return -1;
    if ((dir_inode.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return -1;

    vfs_node_t *victim_node = ext2_finddir(dir, name);
    if (!victim_node || !victim_node->private) return -1;
    uint32_t victim_ino = ((ext2_priv_t *)victim_node->private)->ino;

    ext2_inode_t victim;
    if (ext2_read_inode(victim_ino, &victim) < 0) return -1;
    uint16_t type = victim.i_mode & EXT2_S_IFMT;

    if (type == EXT2_S_IFDIR && !ext2_dir_is_empty(&victim))
        return -1;

    if (type != EXT2_S_IFREG && type != EXT2_S_IFDIR)
        return -1;

    if (ext2_remove_dirent(&dir_inode, name, NULL) < 0) return -1;

    uint32_t now = ext2_now();
    ext2_free_inode_blocks(&victim);
    victim.i_dtime = now;
    victim.i_links_count = 0;
    victim.i_size = 0;
    victim.i_ctime = now;
    ext2_write_inode(victim_ino, &victim);
    ext2_free_inode(victim_ino);

    dir_inode.i_mtime = now;
    dir_inode.i_ctime = now;
    if (type == EXT2_S_IFDIR && dir_inode.i_links_count > 0) {
        dir_inode.i_links_count--;
    }
    ext2_write_inode(dpriv->ino, &dir_inode);
    return 0;
}

/* Persist chmod/chown to the on-disk inode (Phase 24). */
static int ext2_setattr(vfs_node_t *node, uint32_t mode, uint32_t uid,
                        uint32_t gid) {
    if (!g_mounted || !node || !node->private) return -1;
    ext2_priv_t *priv = (ext2_priv_t *)node->private;
    ext2_inode_t inode;
    if (ext2_read_inode(priv->ino, &inode) < 0) return -1;
    inode.i_mode = (uint16_t)((inode.i_mode & EXT2_S_IFMT) | (mode & 0xFFF));
    inode.i_uid  = (uint16_t)uid;
    inode.i_gid  = (uint16_t)gid;
    return ext2_write_inode(priv->ino, &inode);
}

static int ext2_truncate(vfs_node_t *node, uint32_t new_size) {
    if (!g_mounted || !node || !node->private) return -1;
    ext2_priv_t *priv = (ext2_priv_t *)node->private;

    ext2_inode_t inode;
    if (ext2_read_inode(priv->ino, &inode) < 0) return -1;
    if ((inode.i_mode & EXT2_S_IFMT) != EXT2_S_IFREG) return -1;

    uint32_t old_blocks = (inode.i_size + g_state.block_size - 1) /
                          g_state.block_size;
    uint32_t new_blocks = (new_size + g_state.block_size - 1) /
                          g_state.block_size;
    uint32_t max_blocks = 12 + (g_state.block_size / 4);
    if (new_blocks > max_blocks) return -1;

    if (new_blocks > old_blocks) {
        uint32_t i;
        for (i = old_blocks; i < new_blocks; i++) {
            if (!ext2_file_blk_alloc(&inode, i)) {
                while (i > old_blocks) {
                    i--;
                    ext2_file_blk_free(&inode, i);
                }
                return -1;
            }
        }
    } else if (new_blocks < old_blocks) {
        for (uint32_t i = old_blocks; i > new_blocks; i--)
            ext2_file_blk_free(&inode, i - 1);
    }
    inode.i_size = new_size;
    inode.i_mtime = ext2_now();
    inode.i_ctime = inode.i_mtime;
    node->size = new_size;
    node->mtime = inode.i_mtime;
    node->ctime = inode.i_ctime;
    return ext2_write_inode(priv->ino, &inode);
}

/* ── VFS finddir_fn for ext2 directory nodes ─────────────────────────────── */

static vfs_node_t *ext2_finddir(vfs_node_t *dir, const char *name) {
    if (!g_mounted || !dir->private) return NULL;
    ext2_priv_t *priv = (ext2_priv_t *)dir->private;

    ext2_inode_t inode;
    if (ext2_read_inode(priv->ino, &inode) < 0) return NULL;
    if ((inode.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return NULL;

    uint32_t blk_size = g_state.block_size;
    uint32_t name_len = strlen(name);
    uint8_t *blk_buf  = (uint8_t *)kmalloc(blk_size);
    if (!blk_buf) return NULL;

    for (uint32_t pos = 0; pos < inode.i_size; pos += blk_size) {
        uint32_t blk_idx = pos / blk_size;
        uint32_t blk_num = ext2_file_blk(&inode, blk_idx);
        if (!blk_num) continue;
        if (ext2_read_block(blk_num, blk_buf) < 0) break;

        uint32_t offset = 0;
        while (offset < blk_size) {
            ext2_dirent_t *de = (ext2_dirent_t *)(blk_buf + offset);
            if (de->rec_len == 0) break;

            if (de->inode && de->name_len == (uint8_t)name_len &&
                memcmp(de->name, name, name_len) == 0) {
                char tmp[256];
                memcpy(tmp, de->name, de->name_len);
                tmp[de->name_len] = '\0';
                uint32_t child_ino = de->inode;
                kfree(blk_buf);
                return ext2_make_node(child_ino, tmp, de->file_type);
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
    if (!g_mounted || !dir->private) return -1;
    ext2_priv_t *priv = (ext2_priv_t *)dir->private;

    ext2_inode_t inode;
    if (ext2_read_inode(priv->ino, &inode) < 0) return -1;

    uint32_t blk_size = g_state.block_size;
    uint8_t *blk_buf  = (uint8_t *)kmalloc(blk_size);
    if (!blk_buf) return -1;

    uint32_t cur_idx = 0;

    for (uint32_t pos = 0; pos < inode.i_size; pos += blk_size) {
        uint32_t blk_idx = pos / blk_size;
        uint32_t blk_num = ext2_file_blk(&inode, blk_idx);
        if (!blk_num) continue;
        if (ext2_read_block(blk_num, blk_buf) < 0) break;

        uint32_t offset = 0;
        while (offset < blk_size) {
            ext2_dirent_t *de = (ext2_dirent_t *)(blk_buf + offset);
            if (de->rec_len == 0) break;
            if (de->inode) {
                if (cur_idx == req_idx) {
                    out->ino  = de->inode;
                    out->type = (de->file_type == 2) ? VFS_FLAG_DIR : VFS_FLAG_FILE;
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

vfs_node_t *ext2_mount(uint32_t lba_offset) {
    if (!ata_present()) {
        printk("[EXT2]  No ATA drive, skipping mount.\n");
        return NULL;
    }

    /* Read superblock: always at byte 1024 = sector 2 offset 0 */
    uint8_t sb_buf[2048];  /* 4 sectors, safely covers ext2_sb_t */
    if (ata_read(lba_offset + 2, 4, sb_buf) < 0) {
        printk("[EXT2]  Cannot read superblock.\n");
        return NULL;
    }
    ext2_sb_t *sb = (ext2_sb_t *)sb_buf;

    if (sb->s_magic != 0xEF53) {
        printk("[EXT2]  Bad magic (0x%04x), not ext2.\n", (unsigned)sb->s_magic);
        return NULL;
    }

    g_state.lba_offset      = lba_offset;
    g_state.block_size      = 1024U << sb->s_log_block_size;
    g_state.sectors_per_block = g_state.block_size / 512;
    g_state.inodes_per_group  = sb->s_inodes_per_group;
    g_state.blocks_per_group  = sb->s_blocks_per_group;
    g_state.first_data_block  = sb->s_first_data_block;
    g_state.inodes_count      = sb->s_inodes_count;
    g_state.blocks_count      = sb->s_blocks_count;
    g_state.first_ino         = (sb->s_rev_level >= 1) ? sb->s_first_ino : 11;
    g_state.inode_size        = (sb->s_rev_level >= 1) ? sb->s_inode_size : 128;

    ext2_cache_init();
    g_mounted = 1;

    printk("[EXT2]  Mounted: block_size=%u  inodes=%u  inode_size=%u cache=%s\n",
           (unsigned)g_state.block_size,
           (unsigned)g_state.inodes_count,
           (unsigned)g_state.inode_size,
           g_cache_ready ? "on" : "off");

    /* Build VFS node for root (inode 2) */
    vfs_node_t *root = ext2_make_node(2, "", 2);
    if (!root) { g_mounted = 0; return NULL; }
    root->finddir_fn = ext2_finddir;
    root->readdir_fn = ext2_readdir;
    return root;
}
