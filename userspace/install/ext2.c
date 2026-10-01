/*
 * maeros-install: build a populated ext2 filesystem in one pass, the way
 * `mke2fs -d` does.  The source tree is walked first (ext2_scan) so that
 * every inode number is known before any directory is written; then file
 * data, directories and their block maps go to the disk in allocation order,
 * and the inode tables, bitmaps, group descriptors and superblocks last.
 *
 * Layout: revision 1, 1 KiB blocks (what `make disk` and fs/ext2.c use),
 * 128-byte inodes, 8192 blocks per group, sparse_super backups (groups 0, 1
 * and powers of 3, 5 and 7), the filetype directory feature, classic
 * direct/indirect block maps.  Written from the on-disk format in the Linux
 * kernel's Documentation/filesystems/ext4/ (documentation, not code) and
 * Dave Poirier's "The Second Extended File System" (nongnu.org/ext2-doc).
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
#define ISZ         128u           /* inode size */
#define ADDR_PER    (BS / 4)       /* block numbers per map block */
#define ROOT_INO    2u
#define LPF_INO     11u            /* lost+found */
#define FIRST_INO   11u
#define LPF_BLOCKS  12u            /* lost+found gets room for e2fsck */

typedef struct node node_t;
struct node {
    char      *path;               /* source path; NULL for lost+found */
    char      *name;
    uint32_t   mode, uid, gid, atime, mtime, ctime, rdev;
    uint64_t   size;
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
    uint32_t used = rec_len(1) + rec_len(2);
    for (int i = 0; i < d->nkids; i++) {
        uint32_t r = rec_len(strlen(d->kids[i]->name));
        if (used + r > BS) { blocks++; used = 0; }
        used += r;
    }
    if (d->ino == LPF_INO && blocks < LPF_BLOCKS) blocks = LPF_BLOCKS;
    return blocks;
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
    *blocks = g_need_blocks;
    *inodes = g_next_ino - 1;
    printf("  %llu files, %llu MiB, %u inodes\n", (unsigned long long)g_files,
           (unsigned long long)(g_bytes >> 20), (unsigned)(g_next_ino - 1));
}

/* ── Geometry and allocation ─────────────────────────────────────────────── */

static uint64_t  g_base;           /* byte offset of block 0 on the disk */
static uint32_t  g_blocks, g_groups, g_ipg, g_itb, g_gdtb, g_inodes;
static uint8_t  *g_bmap;           /* whole-filesystem block bitmap */
static uint8_t  *g_imap;           /* whole-filesystem inode bitmap */
static uint8_t  *g_itab;           /* inodes 1..g_maxino */
static uint32_t  g_maxino;
static uint32_t  g_cursor;         /* next block to try */
static uint16_t *g_dirs;           /* directories per group */
static uint64_t  g_written;

static int bit(const uint8_t *m, uint32_t i) { return (m[i >> 3] >> (i & 7)) & 1; }
static void set(uint8_t *m, uint32_t i) { m[i >> 3] |= (uint8_t)(1u << (i & 7)); }

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
static uint32_t group_meta(uint32_t g)  { return (has_super(g) ? 1 + g_gdtb : 0) + 2 + g_itb; }
static uint32_t group_len(uint32_t g) {
    uint32_t f = group_first(g);
    return g_blocks - f < BPG ? g_blocks - f : BPG;
}

static uint32_t alloc_block(void) {
    while (g_cursor < g_blocks && bit(g_bmap, g_cursor)) g_cursor++;
    if (g_cursor >= g_blocks) die("the root partition is full");
    set(g_bmap, g_cursor);
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
    set(g_imap, n->ino - 1);
}

/* Block map of one file being written: data blocks are allocated in order,
 * each map block just before the first data block it points to (as Linux
 * does), and the map blocks are written once the file is complete. */
typedef struct { uint32_t blk; uint32_t e[ADDR_PER]; } mapblk_t;
typedef struct {
    uint32_t  direct[12];
    mapblk_t *ind, *dind, *tind;
    mapblk_t **dkids;               /* singles under dind, ADDR_PER */
    mapblk_t **tkids;               /* doubles under tind */
    mapblk_t ***tgrand;             /* singles under each of them */
    uint64_t  idx;
    uint32_t  nblocks;              /* data + map, for i_blocks */
} fmap_t;

static mapblk_t *new_map(fmap_t *m) {
    mapblk_t *b = xcalloc(1, sizeof(*b));
    b->blk = alloc_block();
    m->nblocks++;
    return b;
}

static void fmap_add(fmap_t *m, const void *data) {
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

/* Write the map blocks, set i_block[] and i_blocks, free the map. */
static void fmap_finish(fmap_t *m, uint32_t ino) {
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

static void write_file(node_t *n) {
    fmap_t m;
    memset(&m, 0, sizeof(m));
    int fd = open(n->path, O_RDONLY);
    if (fd < 0) die("cannot open %s", n->path);
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

static void write_dir(node_t *d) {
    fmap_t m;
    memset(&m, 0, sizeof(m));
    uint8_t blk[BS];
    uint32_t used = 0, last = 0;
    memset(blk, 0, sizeof(blk));
    uint64_t nblocks = 0;
    uint64_t want = dir_blocks(d);
#define DIRENT(ino_, name_, nl_, type_) do {                                   \
        uint32_t r_ = rec_len(nl_);                                            \
        if (used + r_ > BS) {                                                  \
            put16(blk + last + 4, BS - last);                                  \
            fmap_add(&m, blk); nblocks++;                                      \
            memset(blk, 0, sizeof(blk)); used = 0;                             \
        }                                                                      \
        put32(blk + used, (ino_)); put16(blk + used + 4, r_);                  \
        blk[used + 6] = (uint8_t)(nl_); blk[used + 7] = (uint8_t)(type_);      \
        memcpy(blk + used + 8, (name_), (nl_));                                \
        last = used; used += r_;                                               \
    } while (0)
    DIRENT(d->ino, ".", 1, 2);
    DIRENT(d->parent ? d->parent->ino : ROOT_INO, "..", 2, 2);
    for (int i = 0; i < d->nkids; i++) {
        node_t *k = d->kids[i];
        uint32_t mode = (k->link_of ? k->link_of : k)->mode;
        uint8_t t = S_ISREG(mode) ? 1 : S_ISDIR(mode) ? 2 : S_ISCHR(mode) ? 3 :
                    S_ISBLK(mode) ? 4 : S_ISFIFO(mode) ? 5 : S_ISSOCK(mode) ? 6 :
                    S_ISLNK(mode) ? 7 : 0;
        DIRENT(k->ino, k->name, strlen(k->name), t);
    }
#undef DIRENT
    put16(blk + last + 4, BS - last);
    fmap_add(&m, blk); nblocks++;
    /* Empty blocks (lost+found's spare room): one unused entry each. */
    while (nblocks < want) {
        memset(blk, 0, sizeof(blk));
        put16(blk + 4, BS);
        fmap_add(&m, blk); nblocks++;
    }
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
    dev_write(g_base + (uint64_t)group_first(g) * BS, blk, BS);
    dev_write(g_base + (uint64_t)(group_first(g) + 1) * BS, gdt, (size_t)g_gdtb * BS);
}

void ext2_build(uint64_t start, uint64_t nsect, const char *label,
                const uint8_t uuid[16]) {
    g_base = start * 512;
    uint64_t nb = nsect / 2;
    if (nb > 0xFFFFFFFFull) nb = 0xFFFFFFFFull;
    g_blocks = (uint32_t)nb;
    g_groups = (g_blocks - 1 + BPG - 1) / BPG;
    /* About one inode per 4 KiB, as mke2fs's default; at least enough for
     * the copy and some room to grow. */
    uint64_t want = (uint64_t)g_blocks / 4;
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

    g_bmap = xcalloc((g_blocks + 7) / 8 + BPG / 8, 1);
    g_imap = xcalloc((g_inodes + 7) / 8 + 1, 1);
    g_maxino = g_next_ino - 1;
    g_itab = xcalloc(g_maxino, ISZ);
    g_dirs = xcalloc(g_groups, sizeof(uint16_t));
    g_run  = xmalloc(RUN_MAX * BS);
    g_io   = xmalloc(IO_SIZE);

    set(g_bmap, 0);                                        /* the boot block */
    for (uint32_t g = 0; g < g_groups; g++) {
        uint32_t f = group_first(g), m = group_meta(g);
        for (uint32_t i = 0; i < m; i++) set(g_bmap, f + i);
    }
    for (uint32_t i = 0; i < FIRST_INO - 1; i++) set(g_imap, i);   /* 1..10 */
    g_cursor = 1;

    printf("  ext2: %u blocks of 1 KiB, %u groups, %u inodes\n",
           (unsigned)g_blocks, (unsigned)g_groups, (unsigned)g_inodes);
    write_tree(g_root);
    run_flush();
    progress("copying files", g_bytes, g_bytes);
    printf("\n");

    /* Group descriptors and bitmaps. */
    uint8_t *gdt = xcalloc(g_gdtb, BS);
    uint8_t blk[BS];
    uint32_t free_blocks = 0, free_inodes = 0;
    for (uint32_t g = 0; g < g_groups; g++) {
        uint32_t f = group_first(g), len = group_len(g);
        uint32_t bb = f + (has_super(g) ? 1 + g_gdtb : 0);
        uint32_t fb = 0, fi = 0;
        memset(blk, 0xFF, BS);
        for (uint32_t i = 0; i < len; i++) {
            if (!bit(g_bmap, f + i)) { blk[i >> 3] &= (uint8_t)~(1u << (i & 7)); fb++; }
        }
        dev_write(g_base + (uint64_t)bb * BS, blk, BS);
        memset(blk, 0xFF, BS);
        for (uint32_t i = 0; i < g_ipg; i++) {
            if (!bit(g_imap, g * g_ipg + i)) { blk[i >> 3] &= (uint8_t)~(1u << (i & 7)); fi++; }
        }
        dev_write(g_base + (uint64_t)(bb + 1) * BS, blk, BS);
        /* Inode table: the inodes we filled, zeros after them. */
        uint64_t toff = g_base + (uint64_t)(bb + 2) * BS;
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
        put32(d + 4, bb + 1);
        put32(d + 8, bb + 2);
        put16(d + 12, fb);
        put16(d + 14, fi);
        put16(d + 16, g_dirs[g]);
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
    for (uint32_t g = 0; g < g_groups; g++)
        if (has_super(g)) write_super(g, sb, gdt);
    memset(blk, 0, BS);
    dev_write(g_base, blk, BS);                            /* block 0 */
    free(gdt);
}
