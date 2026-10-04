/*
 * maeros-install: build a populated ext2 filesystem in one pass, the way
 * `mke2fs -d` does.  The source tree is walked first (ext2_scan) so that
 * every inode number is known before any directory is written; then file
 * data, directories and their block maps go to the disk in allocation order,
 * and the inode tables, bitmaps, group descriptors and superblocks last.
 *
 * Layout: revision 1, 1 KiB blocks (what `make disk` and fs/ext2.c use),
 * 8192 blocks per group, sparse_super backups (groups 0, 1 and powers of 3,
 * 5 and 7), the filetype directory feature.  Two flavours:
 *
 *   ext4 (the default): 256-byte inodes with extra_isize and creation times,
 *     extent trees (depth 0 to 2), flex_bg (bitmaps and inode tables of 16
 *     groups packed into the first), metadata_csum (crc32c of the
 *     superblock, descriptors, bitmaps, inodes, extent blocks, directory
 *     leaves and htree index blocks), an internal jbd2 journal (inode 8,
 *     empty), dir_index with every directory of more than one block written
 *     as an htree (half-MD4, sorted by hash, one or two index levels), the
 *     orphan file (inode 12), huge_file, dir_nlink, large_file;
 *   ext2 (--ext2): 128-byte inodes and classic direct/indirect block maps.
 *
 * Written from the on-disk format in the Linux kernel's
 * Documentation/filesystems/ext4/ and journal.rst (documentation, not code)
 * and Dave Poirier's "The Second Extended File System" (nongnu.org/ext2-doc);
 * the directory hash and checksum routines are the ones fs/ext2.c uses.
 */
#include "install.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

#define BS          1024u          /* block size */
#define BPG         8192u          /* blocks per group (one bitmap block) */
#define ISZ         g_isz          /* inode size: 128 (ext2) or 256 (ext4) */
#define ADDR_PER    (BS / 4)       /* block numbers per map block */
#define ROOT_INO    2u
#define LPF_INO     11u            /* lost+found */
#define FIRST_INO   11u
#define LPF_BLOCKS  12u            /* lost+found gets room for e2fsck */
#define JNL_INO     8u             /* ext4: the journal */
#define ORPHAN_INO  12u            /* ext4: the orphan file */
#define ORPHAN_BLKS 16u
#define FLEX        16u            /* ext4: groups per flex group */
#define DIR_TAIL    12u            /* ext4: checksum entry ending a leaf */
#define EXTENTS_FL  0x00080000u
#define INDEX_FL    0x00001000u
#define EXT_MAX_LEN 32768u
#define EXT_PER     ((BS - 12 - 4) / 12)    /* extents per tree block (84) */
#define ROOT_LIMIT  ((BS - 0x20 - 8) / 8)   /* htree root entries (123) */
#define NODE_LIMIT  ((BS - 8 - 8) / 8)      /* htree interior entries (126) */

static int      g_ext4;
static uint32_t g_isz = 128;
static uint32_t g_seed;            /* metadata_csum seed: crc32c(~0, uuid) */
static uint32_t g_hseed[4];        /* directory hash seed */

typedef struct node node_t;
struct node {
    char      *path;               /* source path; NULL for lost+found */
    char      *name;
    uint32_t   mode, uid, gid, atime, mtime, ctime, rdev;
    uint64_t   size;
    uint64_t   sdev, sino;         /* the source file as the scan saw it */
    uint32_t   ino;
    uint32_t   nlink;              /* names that point at the inode */
    node_t    *link_of;            /* a later name of an earlier inode */
    node_t    *parent;
    node_t   **kids;
    int        nkids, capkids;
    char      *target;             /* symlink */
};

/* ── The scan ────────────────────────────────────────────────────────────── */

static node_t   *g_root;
static uint32_t  g_next_ino = FIRST_INO + 1;
static uint64_t  g_need_blocks;    /* data + map blocks for files and dirs */
static uint64_t  g_files, g_bytes;
static const char **g_skip;
static int       g_nskip;

/* Hard links: source (dev, ino) of every multiply-linked file seen so far. */
typedef struct { uint64_t dev, ino; node_t *n; } hl_t;
static hl_t *g_hl;
static int   g_nhl, g_caphl;

static uint64_t map_blocks(uint64_t nb) {
    /* Index blocks a file of `nb` data blocks needs. */
    uint64_t m = 0;
    if (nb <= 12) return 0;
    nb -= 12;
    m += 1;                                                /* single */
    if (nb <= ADDR_PER) return m;
    nb -= ADDR_PER;
    uint64_t d = nb < (uint64_t)ADDR_PER * ADDR_PER ? nb : (uint64_t)ADDR_PER * ADDR_PER;
    m += 1 + (d + ADDR_PER - 1) / ADDR_PER;                /* double */
    if (nb <= (uint64_t)ADDR_PER * ADDR_PER) return m;
    nb -= (uint64_t)ADDR_PER * ADDR_PER;
    uint64_t t2 = (nb + ADDR_PER - 1) / ADDR_PER;          /* singles under triple */
    m += 1 + (t2 + ADDR_PER - 1) / ADDR_PER + t2;          /* triple */
    return m;
}

static uint32_t rec_len(size_t namelen) { return (uint32_t)((8 + namelen + 3) & ~3u); }

/* Directory size in blocks: entries are packed in order and never span a
 * block, exactly as write_dir() lays them out. */
static uint64_t dir_blocks(const node_t *d) {
    uint64_t blocks = 1;
    uint32_t cap = BS - (g_ext4 ? DIR_TAIL : 0);
    uint32_t used = rec_len(1) + rec_len(2);
    for (int i = 0; i < d->nkids; i++) {
        uint32_t r = rec_len(strlen(d->kids[i]->name));
        if (used + r > cap) { blocks++; used = 0; }
        used += r;
    }
    /* An htree: leaves sorted by hash pack a little less tightly, plus the
     * root and interior blocks (an estimate for the size check only). */
    if (g_ext4 && blocks > 1 && d->ino != LPF_INO) blocks += blocks / 4 + 2 + blocks / NODE_LIMIT;
    if (d->ino == LPF_INO && blocks < LPF_BLOCKS) blocks = LPF_BLOCKS;
    return blocks;
}

/* ext4 mode (the default) or ext2; before ext2_scan(). */
void ext2_set_ext4(int on) {
    g_ext4 = on;
    g_isz = on ? 256 : 128;
    g_next_ino = FIRST_INO + (on ? 2 : 1);        /* 12 is the orphan file */
}

/* ── Checksums and the directory hash (as fs/ext2.c computes them) ───────── */

static uint32_t g_crc32c[256];

static uint32_t crc32c(uint32_t crc, const void *data, size_t n) {
    if (!g_crc32c[1]) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c >> 1) ^ ((c & 1) ? 0x82F63B78u : 0);
            g_crc32c[i] = c;
        }
    }
    const uint8_t *p = (const uint8_t *)data;
    while (n--) crc = g_crc32c[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return crc;
}

/* Per-inode seed: crc32c(fs seed, ino, generation 0). */
static uint32_t iseed(uint32_t ino) {
    uint8_t le[4];
    put32(le, ino);
    uint32_t c = crc32c(g_seed, le, 4);
    put32(le, 0);
    return crc32c(c, le, 4);
}

static void dx_str2hashbuf(const char *msg, uint32_t len, uint32_t *buf, int num) {
    uint32_t pad = len | (len << 8);
    pad |= pad << 16;
    uint32_t val = pad;
    if (len > (uint32_t)num * 4u) len = (uint32_t)num * 4u;
    for (uint32_t i = 0; i < len; i++) {
        int c = (int)(signed char)msg[i];                 /* signed_directory_hash */
        val = (uint32_t)c + (val << 8);
        if ((i & 3) == 3) { *buf++ = val; val = pad; num--; }
    }
    if (--num >= 0) *buf++ = val;
    while (--num >= 0) *buf++ = pad;
}

static uint32_t rol32(uint32_t x, uint32_t s) { return (x << s) | (x >> (32 - s)); }

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
            v[t] = rol32(v[t] + f + in[order[r][j]] + konst[r], shifts[r][j & 3]);
        }
    }
    for (int i = 0; i < 4; i++) h[i] += v[i];
}

/* half-MD4 with the filesystem's seed (s_def_hash_version 1). */
static uint32_t dx_hash(const char *name, uint32_t len) {
    uint32_t h[4] = { g_hseed[0], g_hseed[1], g_hseed[2], g_hseed[3] }, in[8];
    for (uint32_t p = 0; ; p += 32) {
        dx_str2hashbuf(name + p, len - p, in, 8);
        dx_half_md4(h, in);
        if (len - p <= 32) break;
    }
    uint32_t out = h[1] & ~1u;
    if (out == 0xFFFFFFFEu) out = 0xFFFFFFFCu;
    return out;
}

static void add_kid(node_t *dir, node_t *k) {
    if (dir->nkids == dir->capkids) {
        dir->capkids = dir->capkids ? dir->capkids * 2 : 8;
        node_t **nk = xmalloc(sizeof(node_t *) * (size_t)dir->capkids);
        if (dir->nkids) memcpy(nk, dir->kids, sizeof(node_t *) * (size_t)dir->nkids);
        free(dir->kids);
        dir->kids = nk;
    }
    dir->kids[dir->nkids++] = k;
    k->parent = dir;
}

static int skipped(const char *path) {
    for (int i = 0; i < g_nskip; i++)
        if (strcmp(path, g_skip[i]) == 0) return 1;
    return 0;
}

static node_t *new_node(const char *path, const char *name, const struct stat *st) {
    node_t *n = xcalloc(1, sizeof(*n));
    n->path  = path ? xstrdup(path) : NULL;
    n->name  = xstrdup(name);
    n->mode  = st->st_mode & 0xFFFF;
    n->uid   = (uint32_t)st->st_uid;
    n->gid   = (uint32_t)st->st_gid;
    n->atime = (uint32_t)st->st_atime;
    n->mtime = (uint32_t)st->st_mtime;
    n->ctime = (uint32_t)st->st_ctime;
    n->size  = (uint64_t)st->st_size;
    n->sdev  = (uint64_t)st->st_dev;
    n->sino  = (uint64_t)st->st_ino;
    n->nlink = 1;
    return n;
}

static void scan_dir(node_t *dir) {
    DIR *d = opendir(dir->path);
    if (!d) { fprintf(stderr, "maeros-install: cannot read %s, copied empty\n", dir->path); return; }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const char *nm = de->d_name;
        if (strcmp(nm, ".") == 0 || strcmp(nm, "..") == 0) continue;
        if (dir == g_root && strcmp(nm, "lost+found") == 0) continue;
        if (strlen(nm) > 255) continue;
        size_t pl = strlen(dir->path) + strlen(nm) + 2;
        char *p = xmalloc(pl);
        snprintf(p, pl, "%s/%s", strcmp(dir->path, "/") == 0 ? "" : dir->path, nm);
        struct stat st;
        if (lstat(p, &st) < 0) {
            fprintf(stderr, "maeros-install: cannot stat %s, skipped\n", p);
            free(p);
            continue;
        }
        node_t *n = new_node(p, nm, &st);
        if (!S_ISDIR(st.st_mode) && st.st_nlink > 1) {
            for (int i = 0; i < g_nhl; i++)
                if (g_hl[i].dev == (uint64_t)st.st_dev && g_hl[i].ino == (uint64_t)st.st_ino) {
                    n->link_of = g_hl[i].n;
                    n->ino = g_hl[i].n->ino;
                    g_hl[i].n->nlink++;
                    break;
                }
        }
        add_kid(dir, n);
        if (n->link_of) { free(p); continue; }
        n->ino = g_next_ino++;
        if (!S_ISDIR(st.st_mode) && st.st_nlink > 1) {
            if (g_nhl == g_caphl) {
                g_caphl = g_caphl ? g_caphl * 2 : 64;
                hl_t *nh = xmalloc(sizeof(hl_t) * (size_t)g_caphl);
                if (g_nhl) memcpy(nh, g_hl, sizeof(hl_t) * (size_t)g_nhl);
                free(g_hl);
                g_hl = nh;
            }
            g_hl[g_nhl].dev = (uint64_t)st.st_dev;
            g_hl[g_nhl].ino = (uint64_t)st.st_ino;
            g_hl[g_nhl].n = n;
            g_nhl++;
        }
        if (S_ISREG(st.st_mode)) {
            uint64_t nb = (n->size + BS - 1) / BS;
            g_need_blocks += nb + map_blocks(nb);
            g_files++;
            g_bytes += n->size;
        } else if (S_ISLNK(st.st_mode)) {
            char buf[4096];
            int l = (int)readlink(p, buf, sizeof(buf) - 1);
            if (l < 0) l = 0;
            buf[l] = '\0';
            n->target = xstrdup(buf);
            n->size = (uint64_t)l;
            if (l >= 60) g_need_blocks += (l + BS - 1) / BS;
        } else if (S_ISCHR(st.st_mode) || S_ISBLK(st.st_mode)) {
            n->rdev = (uint32_t)st.st_rdev;
        } else if (S_ISDIR(st.st_mode)) {
            if (!skipped(p)) scan_dir(n);
        }
        free(p);
    }
    closedir(d);
    uint64_t db = dir_blocks(dir);
    g_need_blocks += db + map_blocks(db);
}

void ext2_scan(const char *srcdir, const char **skip, int nskip,
               uint64_t *blocks, uint32_t *inodes) {
    struct stat st;
    if (stat(srcdir, &st) < 0 || !S_ISDIR(st.st_mode))
        die("%s is not a directory", srcdir);
    g_skip = skip;
    g_nskip = nskip;
    g_root = new_node(srcdir, "", &st);
    g_root->ino = ROOT_INO;
    /* lost+found first, as mke2fs makes it: inode 11, root-only. */
    struct stat lst = st;
    lst.st_mode = S_IFDIR | 0700;
    lst.st_uid = lst.st_gid = 0;
    node_t *lpf = new_node(NULL, "lost+found", &lst);
    lpf->ino = LPF_INO;
    add_kid(g_root, lpf);
    scan_dir(g_root);
    g_need_blocks += LPF_BLOCKS;
    if (g_ext4) g_need_blocks += 16384 + ORPHAN_BLKS;    /* the largest journal */
    *blocks = g_need_blocks;
    *inodes = g_next_ino - 1;
    printf("  %llu files, %llu MiB, %u inodes\n", (unsigned long long)g_files,
           (unsigned long long)(g_bytes >> 20), (unsigned)(g_next_ino - 1));
}

/* ── Geometry and allocation ─────────────────────────────────────────────── */

static uint64_t  g_base;           /* byte offset of block 0 on the disk */
static uint32_t  g_blocks, g_groups, g_ipg, g_itb, g_gdtb, g_inodes;
static uint8_t  *g_itab;           /* inodes 1..g_maxino */
static uint32_t  g_maxino;
static uint32_t  g_cursor;         /* next block to allocate */
static uint16_t *g_dirs;           /* directories per group */
static uint64_t  g_written;


static int has_super(uint32_t g) {
    if (g <= 1) return 1;
    for (uint32_t b = 3; b <= 7; b += 2) {
        uint32_t p = b;
        while (p < g) p *= b;
        if (p == g) return 1;
    }
    return 0;
}

static uint32_t group_first(uint32_t g) { return 1 + g * BPG; }
static uint32_t super_len(uint32_t g)   { return has_super(g) ? 1 + g_gdtb : 0; }
/* ext4: groups in the flex group led by `g` (a multiple of FLEX). */
static uint32_t flex_count(uint32_t g) { return g_groups - g < FLEX ? g_groups - g : FLEX; }
/* Metadata at the start of group g: superblock and descriptor backups, then
 * its own bitmaps and inode table (ext2), or those of its whole flex group
 * when it leads one (ext4). */
static uint32_t group_meta(uint32_t g) {
    if (!g_ext4) return super_len(g) + 2 + g_itb;
    return super_len(g) + (g % FLEX ? 0 : flex_count(g) * (2 + g_itb));
}
static uint32_t block_bitmap_of(uint32_t g) {
    if (!g_ext4) return group_first(g) + super_len(g);
    uint32_t f = g - g % FLEX;
    return group_first(f) + super_len(f) + (g - f);
}
static uint32_t inode_bitmap_of(uint32_t g) {
    if (!g_ext4) return block_bitmap_of(g) + 1;
    uint32_t f = g - g % FLEX;
    return group_first(f) + super_len(f) + flex_count(f) + (g - f);
}
static uint32_t inode_table_of(uint32_t g) {
    if (!g_ext4) return block_bitmap_of(g) + 2;
    uint32_t f = g - g % FLEX;
    return group_first(f) + super_len(f) + 2 * flex_count(f) + (g - f) * g_itb;
}
static uint32_t group_len(uint32_t g) {
    uint32_t f = group_first(g);
    return g_blocks - f < BPG ? g_blocks - f : BPG;
}

/* Blocks are handed out in order, skipping each group's metadata, so no
 * bitmap is kept while copying: a block is in use iff it is metadata or lies
 * below the cursor, and inode n is in use iff n <= g_maxino.  (A whole-disk
 * bitmap would be 128 MiB for the largest root.) */
static uint32_t alloc_block(void) {
    for (;;) {
        if (g_cursor >= g_blocks) die("the root partition is full");
        uint32_t g = (g_cursor - 1) / BPG, off = (g_cursor - 1) % BPG;
        if (off >= group_meta(g)) break;
        g_cursor = group_first(g) + group_meta(g);
    }
    return g_cursor++;
}

/* ── Sequential writer: consecutive blocks go out in one write ───────────── */

#define RUN_MAX 256u                /* blocks per write (256 KiB) */
static uint8_t  *g_run;
static uint32_t  g_run_start, g_run_len;

static void run_flush(void) {
    if (!g_run_len) return;
    dev_write(g_base + (uint64_t)g_run_start * BS, g_run, (size_t)g_run_len * BS);
    g_written += g_run_len;
    g_run_len = 0;
}

static void put_block(uint32_t blk, const void *data) {
    if (g_run_len && (blk != g_run_start + g_run_len || g_run_len == RUN_MAX))
        run_flush();
    if (!g_run_len) g_run_start = blk;
    memcpy(g_run + (size_t)g_run_len * BS, data, BS);
    g_run_len++;
}

/* ── Inodes ──────────────────────────────────────────────────────────────── */

static uint8_t *inode_at(uint32_t ino) { return g_itab + (size_t)(ino - 1) * ISZ; }

static void inode_fill(const node_t *n, uint32_t links) {
    uint8_t *p = inode_at(n->ino);
    if (g_ext4) {
        put16(p + 0x80, 32);                           /* i_extra_isize */
        put32(p + 0x90, n->ctime);                     /* i_crtime */
    }
    put16(p + 0, n->mode);
    put16(p + 2, n->uid & 0xFFFF);
    put32(p + 4, (uint32_t)n->size);
    put32(p + 8, n->atime);
    put32(p + 12, n->ctime);
    put32(p + 16, n->mtime);
    put16(p + 24, n->gid & 0xFFFF);
    put16(p + 26, links);
    put32(p + 108, (uint32_t)(n->size >> 32));         /* i_size_high */
    put16(p + 120, n->uid >> 16);
    put16(p + 122, n->gid >> 16);
}

/* Block map of one file being written: data blocks are allocated in order,
 * each map block just before the first data block it points to (as Linux
 * does), and the map blocks are written once the file is complete. */
typedef struct { uint32_t blk; uint32_t e[ADDR_PER]; } mapblk_t;
typedef struct { uint32_t lblk, len, pblk; } ext_t;
typedef struct {
    uint32_t  direct[12];
    mapblk_t *ind, *dind, *tind;
    mapblk_t **dkids;               /* singles under dind, ADDR_PER */
    mapblk_t **tkids;               /* doubles under tind */
    mapblk_t ***tgrand;             /* singles under each of them */
    uint64_t  idx;
    uint32_t  nblocks;              /* data + map, for i_blocks */
    ext_t    *ext;                  /* ext4: the extents so far */
    int       next, capext;
} fmap_t;

/* ext4: block `pblk` is the file's next one. */
static void fmap_record(fmap_t *m, uint32_t pblk) {
    uint32_t l = (uint32_t)m->idx++;
    ext_t *x = m->next ? &m->ext[m->next - 1] : NULL;
    if (x && x->pblk + x->len == pblk && x->lblk + x->len == l && x->len < EXT_MAX_LEN) {
        x->len++;
    } else {
        if (m->next == m->capext) {
            m->capext = m->capext ? m->capext * 2 : 16;
            ext_t *ne = xmalloc(sizeof(ext_t) * (size_t)m->capext);
            if (m->next) memcpy(ne, m->ext, sizeof(ext_t) * (size_t)m->next);
            free(m->ext);
            m->ext = ne;
        }
        m->ext[m->next].lblk = l;
        m->ext[m->next].len = 1;
        m->ext[m->next].pblk = pblk;
        m->next++;
    }
    m->nblocks++;
}

static mapblk_t *new_map(fmap_t *m) {
    mapblk_t *b = xcalloc(1, sizeof(*b));
    b->blk = alloc_block();
    m->nblocks++;
    return b;
}

static void fmap_add(fmap_t *m, const void *data) {
    if (g_ext4) {
        uint32_t b = alloc_block();
        fmap_record(m, b);
        put_block(b, data);
        return;
    }
    uint64_t i = m->idx++;
    uint32_t *slot;
    if (i < 12) {
        slot = &m->direct[i];
    } else if ((i -= 12) < ADDR_PER) {
        if (!m->ind) m->ind = new_map(m);
        slot = &m->ind->e[i];
    } else if ((i -= ADDR_PER) < (uint64_t)ADDR_PER * ADDR_PER) {
        if (!m->dind) {
            m->dind = new_map(m);
            m->dkids = xcalloc(ADDR_PER, sizeof(mapblk_t *));
        }
        uint32_t a = (uint32_t)(i / ADDR_PER);
        if (!m->dkids[a]) { m->dkids[a] = new_map(m); m->dind->e[a] = m->dkids[a]->blk; }
        slot = &m->dkids[a]->e[i % ADDR_PER];
    } else {
        i -= (uint64_t)ADDR_PER * ADDR_PER;
        if (i >= (uint64_t)ADDR_PER * ADDR_PER * ADDR_PER) die("file too large for ext2");
        if (!m->tind) {
            m->tind = new_map(m);
            m->tkids = xcalloc(ADDR_PER, sizeof(mapblk_t *));
            m->tgrand = xcalloc(ADDR_PER, sizeof(mapblk_t **));
        }
        uint32_t a = (uint32_t)(i / ((uint64_t)ADDR_PER * ADDR_PER));
        uint32_t b = (uint32_t)((i / ADDR_PER) % ADDR_PER);
        if (!m->tkids[a]) {
            m->tkids[a] = new_map(m);
            m->tind->e[a] = m->tkids[a]->blk;
            m->tgrand[a] = xcalloc(ADDR_PER, sizeof(mapblk_t *));
        }
        if (!m->tgrand[a][b]) {
            m->tgrand[a][b] = new_map(m);
            m->tkids[a]->e[b] = m->tgrand[a][b]->blk;
        }
        slot = &m->tgrand[a][b]->e[i % ADDR_PER];
    }
    *slot = alloc_block();
    m->nblocks++;
    put_block(*slot, data);
}

static void map_out(mapblk_t *b) {
    uint8_t buf[BS];
    for (uint32_t i = 0; i < ADDR_PER; i++) put32(buf + i * 4, b->e[i]);
    put_block(b->blk, buf);
    free(b);
}

static void ext_hdr(uint8_t *h, uint32_t entries, uint32_t max, uint32_t depth) {
    put16(h, 0xF30A);
    put16(h + 2, entries);
    put16(h + 4, max);
    put16(h + 6, depth);
    put32(h + 8, 0);
}

static void ext_leaf(uint8_t *e, const ext_t *x) {
    put32(e, x->lblk);
    put16(e + 4, x->len);
    put16(e + 6, 0);
    put32(e + 8, x->pblk);
}

static void ext_index(uint8_t *e, uint32_t lblk, uint32_t blk) {
    put32(e, lblk);
    put32(e + 4, blk);
    put32(e + 8, 0);
}

/* A tree block with its checksum (after eh_max entries). */
static void ext_block_out(uint32_t blk, uint8_t *b, uint32_t ino) {
    put32(b + 12 + 12 * EXT_PER, crc32c(iseed(ino), b, 12 + 12 * EXT_PER));
    put_block(blk, b);
}

/* ext4: the extent tree into i_block (up to 4 extents), or one or two
 * levels of tree blocks below it. */
static void ext_finish(fmap_t *m, uint32_t ino) {
    uint8_t *p = inode_at(ino), *root = p + 40;
    uint8_t b[BS];
    int n = m->next;
    put32(p + 32, get32(p + 32) | EXTENTS_FL);
    memset(root, 0, 60);
    if (n <= 4) {
        ext_hdr(root, (uint32_t)n, 4, 0);
        for (int i = 0; i < n; i++) ext_leaf(root + 12 + 12 * i, &m->ext[i]);
    } else {
        int leaves = (n + (int)EXT_PER - 1) / (int)EXT_PER;
        int idxb = leaves <= 4 ? 0 : (leaves + (int)EXT_PER - 1) / (int)EXT_PER;
        if (idxb > 4) die("a file with %d extents is too fragmented", n);
        uint32_t *lblk = xmalloc(sizeof(uint32_t) * (size_t)leaves);
        uint32_t *lfirst = xmalloc(sizeof(uint32_t) * (size_t)leaves);
        for (int j = 0; j < leaves; j++) {
            int a = j * (int)EXT_PER, z = a + (int)EXT_PER < n ? a + (int)EXT_PER : n;
            lblk[j] = alloc_block();
            m->nblocks++;
            lfirst[j] = m->ext[a].lblk;
            memset(b, 0, BS);
            ext_hdr(b, (uint32_t)(z - a), EXT_PER, 0);
            for (int i = a; i < z; i++) ext_leaf(b + 12 + 12 * (i - a), &m->ext[i]);
            ext_block_out(lblk[j], b, ino);
        }
        if (!idxb) {
            ext_hdr(root, (uint32_t)leaves, 4, 1);
            for (int j = 0; j < leaves; j++) ext_index(root + 12 + 12 * j, lfirst[j], lblk[j]);
        } else {
            ext_hdr(root, (uint32_t)idxb, 4, 2);
            for (int k = 0; k < idxb; k++) {
                int a = k * (int)EXT_PER, z = a + (int)EXT_PER < leaves ? a + (int)EXT_PER : leaves;
                uint32_t ib = alloc_block();
                m->nblocks++;
                memset(b, 0, BS);
                ext_hdr(b, (uint32_t)(z - a), EXT_PER, 1);
                for (int j = a; j < z; j++) ext_index(b + 12 + 12 * (j - a), lfirst[j], lblk[j]);
                ext_block_out(ib, b, ino);
                ext_index(root + 12 + 12 * k, lfirst[a], ib);
            }
        }
        free(lblk);
        free(lfirst);
    }
    put32(p + 28, m->nblocks * (BS / 512));
    free(m->ext);
    m->ext = NULL;
}

/* Write the map blocks, set i_block[] and i_blocks, free the map. */
static void fmap_finish(fmap_t *m, uint32_t ino) {
    if (g_ext4) { ext_finish(m, ino); return; }
    uint8_t *p = inode_at(ino);
    for (int i = 0; i < 12; i++) put32(p + 40 + i * 4, m->direct[i]);
    if (m->ind) { put32(p + 40 + 12 * 4, m->ind->blk); map_out(m->ind); }
    if (m->dind) {
        put32(p + 40 + 13 * 4, m->dind->blk);
        for (uint32_t a = 0; a < ADDR_PER; a++) if (m->dkids[a]) map_out(m->dkids[a]);
        map_out(m->dind);
        free(m->dkids);
    }
    if (m->tind) {
        put32(p + 40 + 14 * 4, m->tind->blk);
        for (uint32_t a = 0; a < ADDR_PER; a++) {
            if (!m->tkids[a]) continue;
            for (uint32_t b = 0; b < ADDR_PER; b++)
                if (m->tgrand[a][b]) map_out(m->tgrand[a][b]);
            free(m->tgrand[a]);
            map_out(m->tkids[a]);
        }
        map_out(m->tind);
        free(m->tkids);
        free(m->tgrand);
    }
    put32(p + 28, m->nblocks * (BS / 512));
}

/* ── Writing the tree ────────────────────────────────────────────────────── */

static uint8_t *g_io;               /* file read buffer */
#define IO_SIZE (256u * 1024u)
static uint64_t g_copied;

/* Open the regular file the scan recorded, or return -1 if `path` is now
 * something else.  The copy gets the scanned owner and mode, so between the
 * scan and the copy a user who owns a directory on the way must not be able
 * to swap their file for a link to (or another name of) a file they cannot
 * read, e.g. /disk/etc/shadow: no link is followed, and the file opened must
 * be the same inode on the same device.  O_NONBLOCK keeps a FIFO put in its
 * place from blocking the open. */
static int open_scanned(const node_t *n) {
    int fd = open(n->path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) ||
        (uint64_t)st.st_dev != n->sdev || (uint64_t)st.st_ino != n->sino) {
        close(fd);
        return -1;
    }
    return fd;
}

static void write_file(node_t *n) {
    fmap_t m;
    memset(&m, 0, sizeof(m));
    int fd = open_scanned(n);
    if (fd < 0) {
        /* Gone or replaced since the scan: install it empty. */
        fprintf(stderr, "maeros-install: %s changed since the scan, copied empty\n", n->path);
        g_copied += n->size;
        n->size = 0;
        fmap_finish(&m, n->ino);
        return;
    }
    uint64_t left = n->size;
    while (left) {
        size_t want = left < IO_SIZE ? (size_t)left : IO_SIZE;
        size_t got = 0;
        while (got < want) {
            int r = (int)read(fd, g_io + got, (int)(want - got));
            if (r <= 0) {
                /* Shrunk since the scan (a log being rotated): pad with zeros
                 * so the inode keeps the size the scan recorded. */
                fprintf(stderr, "maeros-install: %s: short read, zero-filled\n", n->path);
                memset(g_io + got, 0, want - got);
                got = want;
                break;
            }
            got += (size_t)r;
        }
        size_t nb = (got + BS - 1) / BS;
        if (got % BS) memset(g_io + got, 0, nb * BS - got);
        for (size_t b = 0; b < nb; b++) fmap_add(&m, g_io + b * BS);
        left -= got;
        g_copied += got;
        progress("copying files", g_copied, g_bytes);
    }
    close(fd);
    fmap_finish(&m, n->ino);
}

typedef struct { const char *name; uint32_t nlen, ino, type, hash; } dent_t;

static int dent_cmp(const void *a, const void *b) {
    uint32_t x = ((const dent_t *)a)->hash, y = ((const dent_t *)b)->hash;
    return x < y ? -1 : x > y;
}

static void put_dirent(uint8_t *at, const dent_t *e, uint32_t rl) {
    put32(at, e->ino);
    put16(at + 4, rl);
    at[6] = (uint8_t)e->nlen;
    at[7] = (uint8_t)e->type;
    memcpy(at + 8, e->name, e->nlen);
}

/* ext4: the 12-byte entry closing a leaf, with the leaf's checksum. */
static void dir_tail(uint8_t *blk, uint32_t ino) {
    if (!g_ext4) return;
    uint8_t *t = blk + BS - DIR_TAIL;
    memset(t, 0, DIR_TAIL);
    put16(t + 4, DIR_TAIL);
    t[7] = 0xDE;
    put32(t + 8, crc32c(iseed(ino), blk, BS - DIR_TAIL));
}

/* Entries packed in order into leaf blocks of `cap` bytes; returns the
 * number of blocks, which go to fmap_add() when `m` is given. */
static uint64_t dir_linear(const node_t *d, const dent_t *e, int n, fmap_t *m) {
    uint32_t cap = BS - (g_ext4 ? DIR_TAIL : 0), used = 0, last = 0;
    uint8_t blk[BS];
    uint64_t nblocks = 0;
    memset(blk, 0, sizeof(blk));
    for (int i = 0; i < n; i++) {
        uint32_t r = rec_len(e[i].nlen);
        if (used + r > cap) {
            put16(blk + last + 4, cap - last);
            dir_tail(blk, d->ino);
            fmap_add(m, blk); nblocks++;
            memset(blk, 0, sizeof(blk)); used = 0;
        }
        put_dirent(blk + used, &e[i], r);
        last = used; used += r;
    }
    put16(blk + last + 4, cap - last);
    dir_tail(blk, d->ino);
    fmap_add(m, blk); nblocks++;
    /* Empty blocks (lost+found's spare room): one unused entry each. */
    uint64_t want = d->ino == LPF_INO ? LPF_BLOCKS : 0;
    while (nblocks < want) {
        memset(blk, 0, sizeof(blk));
        put16(blk + 4, cap);
        dir_tail(blk, d->ino);
        fmap_add(m, blk); nblocks++;
    }
    return nblocks;
}

/* The checksum tail of an htree root or interior block. */
static void dx_tail(uint8_t *b, uint32_t count_off, uint32_t ino) {
    uint32_t limit = get32(b + count_off) & 0xFFFF, count = get32(b + count_off) >> 16;
    uint8_t *t = b + count_off + limit * 8;
    uint32_t c = crc32c(iseed(ino), b, count_off + count * 8);
    memset(t, 0, 8);
    c = crc32c(c, t, 8);
    put32(t + 4, c);
}

/* ext4: an htree directory.  The names (all but "." and "..") sorted by
 * hash fill leaves in order; the root (block 0, holding "." and "..")
 * indexes them directly, or through interior blocks 1..N when there are
 * more than ROOT_LIMIT leaves.  An index entry whose leaf starts with the
 * same hash the previous leaf ends with has the low bit set (a collision
 * run continues there).  0 when the directory is too large for two levels
 * (it is then written linearly). */
static int dir_htree(const node_t *d, dent_t *e, int n, fmap_t *m, uint64_t *nblocks) {
    uint32_t cap = BS - DIR_TAIL;
    dent_t *k = e + 2;
    int nk = n - 2;
    for (int i = 0; i < nk; i++) k[i].hash = dx_hash(k[i].name, k[i].nlen);
    qsort(k, (size_t)nk, sizeof(dent_t), dent_cmp);
    int *lstart = xmalloc(sizeof(int) * (size_t)(nk + 1));
    int L = 0;
    uint32_t used = cap;
    for (int i = 0; i < nk; i++) {
        uint32_t r = rec_len(k[i].nlen);
        if (used + r > cap) { lstart[L++] = i; used = 0; }
        used += r;
    }
    lstart[L] = nk;
    int levels = L > (int)ROOT_LIMIT;
    int N = levels ? (L + (int)NODE_LIMIT - 1) / (int)NODE_LIMIT : 0;
    if (N > (int)ROOT_LIMIT) { free(lstart); return 0; }
    int per = N ? (L + N - 1) / N : L;
    uint32_t total = 1 + (uint32_t)N + (uint32_t)L;
    uint8_t *img = xcalloc(total, BS);
    /* Index hash of leaf j (the first is implicit). */
#define LHASH(j) (k[lstart[j]].hash | ((j) && k[lstart[j]].hash == k[lstart[j] - 1].hash ? 1u : 0u))
    for (int j = 0; j < L; j++) {
        uint8_t *b = img + (size_t)(1 + N + j) * BS;
        uint32_t off = 0, last = 0;
        for (int i = lstart[j]; i < lstart[j + 1]; i++) {
            uint32_t r = rec_len(k[i].nlen);
            put_dirent(b + off, &k[i], r);
            last = off; off += r;
        }
        put16(b + last + 4, cap - last);
        dir_tail(b, d->ino);
    }
    uint8_t *r = img;
    put_dirent(r, &e[0], 12);
    put_dirent(r + 12, &e[1], BS - 12);
    r[0x1C] = 1;                                      /* half-MD4 */
    r[0x1D] = 8;                                      /* info_length */
    r[0x1E] = (uint8_t)levels;
    if (!levels) {
        put32(r + 0x20, ROOT_LIMIT | ((uint32_t)L << 16));
        put32(r + 0x24, 1);
        for (int j = 1; j < L; j++) {
            put32(r + 0x20 + 8 * j, LHASH(j));
            put32(r + 0x24 + 8 * j, 1 + (uint32_t)j);
        }
    } else {
        put32(r + 0x20, ROOT_LIMIT | ((uint32_t)N << 16));
        for (int t = 0; t < N; t++) {
            int a = t * per, z = a + per < L ? a + per : L;
            if (t) put32(r + 0x20 + 8 * t, LHASH(a));
            put32(r + 0x24 + 8 * t, 1 + (uint32_t)t);
            uint8_t *nb = img + (size_t)(1 + t) * BS;
            put16(nb + 4, BS);                         /* an empty entry over all */
            put32(nb + 8, NODE_LIMIT | ((uint32_t)(z - a) << 16));
            put32(nb + 12, 1 + (uint32_t)N + (uint32_t)a);
            for (int j = a + 1; j < z; j++) {
                put32(nb + 8 + 8 * (j - a), LHASH(j));
                put32(nb + 12 + 8 * (j - a), 1 + (uint32_t)N + (uint32_t)j);
            }
            dx_tail(nb, 8, d->ino);
        }
    }
#undef LHASH
    dx_tail(r, 0x20, d->ino);
    for (uint32_t i = 0; i < total; i++) fmap_add(m, img + (size_t)i * BS);
    free(img);
    free(lstart);
    *nblocks = total;
    return 1;
}

static void write_dir(node_t *d) {
    fmap_t m;
    memset(&m, 0, sizeof(m));
    int n = d->nkids + 2;
    dent_t *e = xcalloc((size_t)n, sizeof(dent_t));
    e[0].name = "."; e[0].nlen = 1; e[0].ino = d->ino; e[0].type = 2;
    e[1].name = ".."; e[1].nlen = 2; e[1].ino = d->parent ? d->parent->ino : ROOT_INO; e[1].type = 2;
    for (int i = 0; i < d->nkids; i++) {
        node_t *k = d->kids[i];
        uint32_t mode = (k->link_of ? k->link_of : k)->mode;
        e[i + 2].name = k->name;
        e[i + 2].nlen = (uint32_t)strlen(k->name);
        e[i + 2].ino = k->ino;
        e[i + 2].type = S_ISREG(mode) ? 1 : S_ISDIR(mode) ? 2 : S_ISCHR(mode) ? 3 :
                        S_ISBLK(mode) ? 4 : S_ISFIFO(mode) ? 5 : S_ISSOCK(mode) ? 6 :
                        S_ISLNK(mode) ? 7 : 0;
    }
    uint64_t nblocks = 0;
    /* More than one leaf: an htree (ext4), as Linux makes a directory
     * indexed once it outgrows its first block.  lost+found stays linear. */
    uint32_t cap = BS - DIR_TAIL, used = 0;
    int multi = 0;
    for (int i = 0; i < n && !multi; i++) {
        used += rec_len(e[i].nlen);
        if (used > cap) multi = 1;
    }
    if (g_ext4 && multi && d->ino != LPF_INO && dir_htree(d, e, n, &m, &nblocks)) {
        uint8_t *p = inode_at(d->ino);
        put32(p + 32, get32(p + 32) | INDEX_FL);
    } else {
        nblocks = dir_linear(d, e, n, &m);
    }
    free(e);
    d->size = nblocks * BS;
    fmap_finish(&m, d->ino);
}

static void write_tree(node_t *d) {
    uint32_t subdirs = 0;
    for (int i = 0; i < d->nkids; i++) {
        node_t *k = d->kids[i];
        if (k->link_of) continue;
        if (S_ISDIR(k->mode)) {
            subdirs++;
            write_tree(k);
        } else {
            if (S_ISREG(k->mode)) {
                write_file(k);
            } else if (S_ISLNK(k->mode)) {
                if (k->size < 60) {
                    memcpy(inode_at(k->ino) + 40, k->target, (size_t)k->size);
                } else {
                    fmap_t m;
                    memset(&m, 0, sizeof(m));
                    uint8_t blk[BS];
                    for (uint64_t off = 0; off < k->size; off += BS) {
                        memset(blk, 0, BS);
                        uint64_t n = k->size - off < BS ? k->size - off : BS;
                        memcpy(blk, k->target + off, (size_t)n);
                        fmap_add(&m, blk);
                    }
                    fmap_finish(&m, k->ino);
                }
            } else if (S_ISCHR(k->mode) || S_ISBLK(k->mode)) {
                uint32_t maj = (k->rdev >> 8) & 0xFFF, min = (k->rdev & 0xFF) | ((k->rdev >> 12) & 0xFFF00);
                uint8_t *p = inode_at(k->ino);
                if (maj < 256 && min < 256) put32(p + 40, (maj << 8) | min);
                else put32(p + 44, (min & 0xFF) | (maj << 8) | ((min & ~0xFFu) << 12));
                k->size = 0;
            } else {
                k->size = 0;
            }
            inode_fill(k, k->nlink);
        }
    }
    write_dir(d);
    inode_fill(d, 2 + subdirs);
    g_dirs[(d->ino - 1) / g_ipg]++;
}

/* ── Metadata ────────────────────────────────────────────────────────────── */

static void write_super(uint32_t g, const uint8_t *sb, const uint8_t *gdt) {
    uint8_t blk[BS];
    memcpy(blk, sb, BS);
    put16(blk + 90, g);                                    /* s_block_group_nr */
    if (g_ext4) put32(blk + 0x3FC, crc32c(~0u, blk, 0x3FC));
    dev_write(g_base + (uint64_t)group_first(g) * BS, blk, BS);
    dev_write(g_base + (uint64_t)(group_first(g) + 1) * BS, gdt, (size_t)g_gdtb * BS);
}

uint64_t ext2_max_sectors(void) {
    return (uint64_t)EXT2_MAX_BLOCKS * (BS / 512);
}

/* Geometry for a filesystem of `nsect` sectors; fills the g_* globals. */
static void geometry(uint64_t nsect) {
    uint64_t nb = nsect / 2;
    if (nb > EXT2_MAX_BLOCKS) nb = EXT2_MAX_BLOCKS;
    g_blocks = (uint32_t)nb;
    g_groups = (uint32_t)(((uint64_t)g_blocks - 1 + BPG - 1) / BPG);
    /* One inode per 16 KiB (mke2fs's ratio for large filesystems keeps the
     * inode tables, all of which are written, near 1% of the disk); at least
     * enough for the copy and some room to grow. */
    uint64_t want = (uint64_t)g_blocks / 16;
    if (want < (uint64_t)g_next_ino + 1024) want = (uint64_t)g_next_ino + 1024;
    for (;;) {
        g_ipg = (uint32_t)((want + g_groups - 1) / g_groups);
        g_ipg = (g_ipg + 7) & ~7u;                         /* whole itable blocks */
        if (g_ipg > BPG) g_ipg = BPG;
        g_itb  = g_ipg * ISZ / BS;
        g_gdtb = (g_groups * 32 + BS - 1) / BS;
        /* A last group too small for its own metadata is dropped. */
        uint32_t last = g_groups - 1;
        if (g_groups > 1 && group_len(last) < group_meta(last) + 16) {
            g_blocks = group_first(last);
            g_groups--;
            continue;
        }
        break;
    }
    g_inodes = g_ipg * g_groups;
    if (g_inodes < g_next_ino) die("the root partition is too small for the files");
    /* Group 0 holds the superblock, the whole descriptor table, its bitmaps
     * and inode table; past that the data and group 1's backup superblock
     * would land on it.  EXT2_MAX_BLOCKS keeps this far from happening. */
    if (group_meta(0) + 64 > BPG) die("internal: ext2 group 0 metadata does not fit");
}

void ext2_geometry(uint64_t nsect, uint64_t *blocks, uint32_t *groups,
                   uint32_t *inodes, uint32_t *meta0) {
    geometry(nsect);
    *blocks = g_blocks;
    *groups = g_groups;
    *inodes = g_inodes;
    *meta0 = group_meta(0);
}

static void be32w(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static void special_inode(uint32_t ino, uint32_t mode, uint64_t size) {
    uint8_t *p = inode_at(ino);
    uint32_t now = now32();
    put16(p + 0, mode);
    put32(p + 4, (uint32_t)size);
    put32(p + 8, now);
    put32(p + 12, now);
    put32(p + 16, now);
    put16(p + 26, 1);
    put16(p + 0x80, 32);
    put32(p + 0x90, now);
}

/* ext4: the journal (inode 8: an empty jbd2 v2 log, blocks zeroed) and the
 * orphan file (inode 12: empty blocks closed by their magic and checksum),
 * first on the disk.  Returns the journal's length in blocks. */
static uint32_t journal_and_orphans(const uint8_t uuid[16]) {
    uint32_t jb = g_blocks < 32768 ? 1024 : g_blocks < 262144 ? 4096 :
                  g_blocks < 1048576 ? 8192 : 16384;
    fmap_t m;
    memset(&m, 0, sizeof(m));
    uint8_t b[BS];
    for (uint32_t i = 0; i < jb; i++) {
        memset(b, 0, BS);
        if (i == 0) {
            be32w(b + 0x00, 0xC03B3998u);                 /* magic */
            be32w(b + 0x04, 4);                           /* superblock v2 */
            be32w(b + 0x0C, BS);                          /* s_blocksize */
            be32w(b + 0x10, jb);                          /* s_maxlen */
            be32w(b + 0x14, 1);                           /* s_first */
            be32w(b + 0x18, 1);                           /* s_sequence */
            memcpy(b + 0x30, uuid, 16);
            be32w(b + 0x40, 1);                           /* s_nr_users */
        }
        fmap_add(&m, b);
        if ((i & 255) == 255) progress("journal", i + 1, jb);
    }
    special_inode(JNL_INO, 0x8180, (uint64_t)jb * BS);
    fmap_finish(&m, JNL_INO);

    memset(&m, 0, sizeof(m));
    for (uint32_t i = 0; i < ORPHAN_BLKS; i++) {
        uint32_t blk = alloc_block();
        uint8_t le[8];
        memset(b, 0, BS);
        put32(b + BS - 8, 0x0B10CA04u);                  /* ob_magic */
        put64(le, blk);
        put32(b + BS - 4, crc32c(crc32c(iseed(ORPHAN_INO), le, 8), b, BS - 8));
        fmap_record(&m, blk);
        put_block(blk, b);
    }
    special_inode(ORPHAN_INO, 0x8180, (uint64_t)ORPHAN_BLKS * BS);
    fmap_finish(&m, ORPHAN_INO);
    printf("\n");
    return jb;
}

/* ext4: an inode's checksum (as fs/ext2.c x4_inode_csum), all-zero ones
 * (never used) left alone. */
static void inode_csum(uint32_t ino) {
    uint8_t *p = inode_at(ino);
    uint32_t i;
    for (i = 0; i < ISZ && !p[i]; i++) ;
    if (i == ISZ) return;
    put16(p + 0x7C, 0);
    put16(p + 0x82, 0);
    uint32_t c = crc32c(iseed(ino), p, ISZ);
    put16(p + 0x7C, c & 0xFFFF);
    put16(p + 0x82, c >> 16);
}

void ext2_build(uint64_t start, uint64_t nsect, const char *label,
                const uint8_t uuid[16]) {
    g_base = start * 512;
    geometry(nsect);

    g_maxino = g_next_ino - 1;
    g_itab = xcalloc(g_maxino, ISZ);
    g_dirs = xcalloc(g_groups, sizeof(uint16_t));
    g_run  = xmalloc(RUN_MAX * BS);
    g_io   = xmalloc(IO_SIZE);

    g_cursor = 1;

    printf("  %s: %u blocks of 1 KiB, %u groups, %u inodes\n", g_ext4 ? "ext4" : "ext2",
           (unsigned)g_blocks, (unsigned)g_groups, (unsigned)g_inodes);
    uint32_t jblocks = 0;
    if (g_ext4) {
        g_seed = crc32c(~0u, uuid, 16);
        uint8_t hs[16];
        random_bytes(hs, sizeof(hs));
        for (int i = 0; i < 4; i++) g_hseed[i] = get32(hs + 4 * i);
        jblocks = journal_and_orphans(uuid);
    }
    write_tree(g_root);
    run_flush();
    progress("copying files", g_bytes, g_bytes);
    printf("\n");

    /* Group descriptors and bitmaps. */
    uint8_t *gdt = xcalloc(g_gdtb, BS);
    uint8_t blk[BS];
    uint32_t free_blocks = 0, free_inodes = 0;
    if (g_ext4)
        for (uint32_t i = 1; i <= g_maxino; i++) inode_csum(i);
    for (uint32_t g = 0; g < g_groups; g++) {
        uint32_t f = group_first(g), len = group_len(g);
        uint32_t bb = block_bitmap_of(g), ib = inode_bitmap_of(g), it = inode_table_of(g);
        uint32_t fb = 0, fi = 0, bcs, ics;
        memset(blk, 0xFF, BS);
        for (uint32_t i = 0; i < len; i++) {
            if (i >= group_meta(g) && f + i >= g_cursor) {
                blk[i >> 3] &= (uint8_t)~(1u << (i & 7));
                fb++;
            }
        }
        bcs = crc32c(g_seed, blk, BPG / 8);
        dev_write(g_base + (uint64_t)bb * BS, blk, BS);
        memset(blk, 0xFF, BS);
        for (uint32_t i = 0; i < g_ipg; i++) {
            if (g * g_ipg + i + 1 > g_maxino) {
                blk[i >> 3] &= (uint8_t)~(1u << (i & 7));
                fi++;
            }
        }
        ics = crc32c(g_seed, blk, g_ipg / 8);
        dev_write(g_base + (uint64_t)ib * BS, blk, BS);
        /* Inode table: the inodes we filled, zeros after them. */
        uint64_t toff = g_base + (uint64_t)it * BS;
        uint32_t first = g * g_ipg + 1;
        uint32_t have = first <= g_maxino ? g_maxino - first + 1 : 0;
        if (have > g_ipg) have = g_ipg;
        if (have) dev_write(toff, inode_at(first), (size_t)have * ISZ);
        for (uint32_t i = have; i < g_ipg; ) {
            uint32_t n = g_ipg - i < RUN_MAX * BS / ISZ ? g_ipg - i : RUN_MAX * BS / ISZ;
            memset(g_run, 0, (size_t)n * ISZ);
            dev_write(toff + (uint64_t)i * ISZ, g_run, (size_t)n * ISZ);
            i += n;
        }
        uint8_t *d = gdt + g * 32;
        put32(d + 0, bb);
        put32(d + 4, ib);
        put32(d + 8, it);
        put16(d + 12, fb);
        put16(d + 14, fi);
        put16(d + 16, g_dirs[g]);
        if (g_ext4) {
            uint8_t le[4];
            put16(d + 18, 0x0004);                         /* bg_flags: ITABLE_ZEROED */
            put16(d + 24, bcs & 0xFFFF);                   /* block bitmap checksum */
            put16(d + 26, ics & 0xFFFF);                   /* inode bitmap checksum */
            put32(le, g);
            put16(d + 30, crc32c(crc32c(g_seed, le, 4), d, 32) & 0xFFFF);  /* field is 0 here */
        }
        free_blocks += fb;
        free_inodes += fi;
        progress("writing inode tables", g + 1, g_groups);
    }
    printf("\n");

    uint8_t sb[BS];
    memset(sb, 0, sizeof(sb));
    uint32_t now = now32();
    put32(sb + 0, g_inodes);
    put32(sb + 4, g_blocks);
    put32(sb + 8, g_blocks / 20);                          /* 5% reserved for root */
    put32(sb + 12, free_blocks);
    put32(sb + 16, free_inodes);
    put32(sb + 20, 1);                                     /* s_first_data_block */
    put32(sb + 24, 0);                                     /* 1 KiB blocks */
    put32(sb + 28, 0);
    put32(sb + 32, BPG);
    put32(sb + 36, BPG);                                   /* frags per group */
    put32(sb + 40, g_ipg);
    put32(sb + 48, now);                                   /* s_wtime */
    put16(sb + 54, 0xFFFF);                                /* s_max_mnt_count: -1 */
    put16(sb + 56, 0xEF53);
    put16(sb + 58, 1);                                     /* s_state: clean */
    put16(sb + 60, 1);                                     /* s_errors: continue */
    put32(sb + 64, now);                                   /* s_lastcheck */
    put32(sb + 76, 1);                                     /* s_rev_level: dynamic */
    put32(sb + 84, FIRST_INO);
    put16(sb + 88, ISZ);
    put32(sb + 92, 0);                                     /* compat */
    put32(sb + 96, 0x0002);                                /* incompat: filetype */
    put32(sb + 100, 0x0001 | 0x0002);                      /* ro_compat: sparse_super, large_file */
    memcpy(sb + 104, uuid, 16);
    strncpy((char *)sb + 120, label, 16);
    if (g_ext4) {
        uint8_t *ji = inode_at(JNL_INO);
        put32(sb + 0x5C, 0x0004 | 0x0020 | 0x1000);        /* has_journal, dir_index, orphan_file */
        put32(sb + 0x60, 0x0002 | 0x0040 | 0x0200);        /* filetype, extent, flex_bg */
        put32(sb + 0x64, 0x0001 | 0x0002 | 0x0008 | 0x0020 | 0x0040 | 0x0400);
                       /* sparse_super, large_file, huge_file, dir_nlink, extra_isize, metadata_csum */
        put32(sb + 0xE0, JNL_INO);                         /* s_journal_inum */
        for (int i = 0; i < 4; i++) put32(sb + 0xEC + 4 * i, g_hseed[i]);
        sb[0xFC] = 1;                                      /* s_def_hash_version: half-MD4 */
        sb[0xFD] = 1;                                      /* s_jnl_backup_type: inode blocks */
        put32(sb + 0x108, now);                            /* s_mkfs_time */
        memcpy(sb + 0x10C, ji + 40, 60);                   /* s_jnl_blocks: i_block, */
        put32(sb + 0x148, 0);                              /* i_size_high, */
        put32(sb + 0x14C, jblocks * BS);                   /* i_size */
        put16(sb + 0x15C, 32);                             /* s_min_extra_isize */
        put16(sb + 0x15E, 32);                             /* s_want_extra_isize */
        put32(sb + 0x160, 0x0001);                         /* s_flags: signed directory hash */
        sb[0x174] = 4;                                     /* s_log_groups_per_flex: 16 */
        sb[0x175] = 1;                                     /* s_checksum_type: crc32c */
        put32(sb + 0x280, ORPHAN_INO);                     /* s_orphan_file_inum */
    }
    for (uint32_t g = 0; g < g_groups; g++)
        if (has_super(g)) write_super(g, sb, gdt);
    memset(blk, 0, BS);
    dev_write(g_base, blk, BS);                            /* block 0 */
    free(gdt);
}
