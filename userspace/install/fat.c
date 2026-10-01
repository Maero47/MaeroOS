/*
 * maeros-install: format the EFI System Partition as FAT32 and write the
 * boot files into it.  Every file and directory gets one contiguous run of
 * clusters, allocated in order; the FATs are written last.  Names that are
 * not plain upper-case 8.3 get long-name (VFAT) entries, so UEFI firmware and
 * Limine both find "limine-bios.sys" and "BOOTX64.EFI" by their real names.
 *
 * Written from Microsoft's "FAT: General Overview of On-Disk Format"
 * (fatgen103, the FAT32 BPB, FSInfo sector and long-name entries) and the UEFI
 * specification's section on the EFI System Partition.
 */
#include "install.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#define RESERVED   32u                 /* sectors before the first FAT */
#define NFATS      2u

typedef struct fent fent_t;
struct fent {
    char         name[64];
    int          is_dir;
    inst_file_t *file;
    uint32_t     clus, nclus;
    uint8_t      short_name[11];
    fent_t      *kids[16];
    int          nkids;
    fent_t      *parent;
};

static uint64_t  g_base;               /* byte offset of the partition */
static uint32_t  g_spc, g_cs;          /* sectors per cluster, cluster bytes */
static uint32_t  g_fatsz, g_nclus, g_next = 2;
static uint32_t *g_fat;
static uint32_t  g_dosdate, g_dostime;

static uint64_t clus_off(uint32_t c) {
    return g_base + ((uint64_t)RESERVED + (uint64_t)NFATS * g_fatsz) * 512 +
           (uint64_t)(c - 2) * g_cs;
}

static uint32_t alloc_run(uint32_t n) {
    if (!n) return 0;
    if (g_next + n > g_nclus + 2) die("the EFI system partition is full");
    uint32_t first = g_next;
    for (uint32_t i = 0; i < n; i++)
        g_fat[first + i] = i + 1 < n ? first + i + 1 : 0x0FFFFFFF;
    g_next += n;
    return first;
}

static fent_t *child(fent_t *dir, const char *name, int is_dir) {
    for (int i = 0; i < dir->nkids; i++)
        if (strcmp(dir->kids[i]->name, name) == 0) return dir->kids[i];
    if (dir->nkids == 16) die("too many ESP entries in one directory");
    fent_t *e = xcalloc(1, sizeof(*e));
    snprintf(e->name, sizeof(e->name), "%s", name);
    e->is_dir = is_dir;
    e->parent = dir;
    dir->kids[dir->nkids++] = e;
    return e;
}

/* ── Names ───────────────────────────────────────────────────────────────── */

static int valid83(const char *n, uint8_t out[11]) {
    memset(out, ' ', 11);
    const char *dot = strchr(n, '.');
    size_t bl = dot ? (size_t)(dot - n) : strlen(n);
    size_t el = dot ? strlen(dot + 1) : 0;
    if (bl == 0 || bl > 8 || el > 3 || (dot && strchr(dot + 1, '.'))) return 0;
    for (size_t i = 0; i < bl; i++) {
        char c = n[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return 0;
        out[i] = (uint8_t)c;
    }
    for (size_t i = 0; i < el; i++) {
        char c = dot[1 + i];
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return 0;
        out[8 + i] = (uint8_t)c;
    }
    return 1;
}

static uint8_t sfn_char(char c) {
    if (c >= 'a' && c <= 'z') return (uint8_t)(c - 32);
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-') return (uint8_t)c;
    return '_';
}

/* Number of 32-byte slots the entry needs (long-name slots + the 8.3 one),
 * and its 8.3 name ("LIMINE~1SYS" style when a long name is needed). */
static int name_slots(fent_t *e, int seq) {
    if (valid83(e->name, e->short_name)) return 1;
    memset(e->short_name, ' ', 11);
    const char *dot = strrchr(e->name, '.');
    if (dot == e->name) dot = NULL;
    size_t bl = dot ? (size_t)(dot - e->name) : strlen(e->name);
    int k = 0;
    for (size_t i = 0; i < bl && k < 6; i++)
        if (e->name[i] != '.' && e->name[i] != ' ') e->short_name[k++] = sfn_char(e->name[i]);
    if (!k) e->short_name[k++] = '_';
    e->short_name[k++] = '~';
    e->short_name[k] = (uint8_t)('0' + seq);
    if (dot)
        for (int i = 0; i < 3 && dot[1 + i]; i++) e->short_name[8 + i] = sfn_char(dot[1 + i]);
    return 1 + (int)((strlen(e->name) + 12) / 13);
}

static uint8_t sfn_sum(const uint8_t *s) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) sum = (uint8_t)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + s[i]);
    return sum;
}

static void dirent83(uint8_t *d, const uint8_t name[11], uint8_t attr, uint32_t clus,
                     uint32_t size) {
    memset(d, 0, 32);
    memcpy(d, name, 11);
    d[11] = attr;
    put16(d + 14, g_dostime); put16(d + 16, g_dosdate);    /* created */
    put16(d + 18, g_dosdate);                              /* accessed */
    put16(d + 20, clus >> 16);
    put16(d + 22, g_dostime); put16(d + 24, g_dosdate);    /* written */
    put16(d + 26, clus & 0xFFFF);
    put32(d + 28, size);
}

static void lfn_slots(uint8_t *d, const fent_t *e, int nl) {
    static const int pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
    uint8_t sum = sfn_sum(e->short_name);
    size_t len = strlen(e->name);
    for (int s = 0; s < nl; s++) {
        int ord = nl - s;                                  /* last part first */
        uint8_t *p = d + s * 32;
        memset(p, 0, 32);
        p[0] = (uint8_t)(ord | (s == 0 ? 0x40 : 0));
        p[11] = 0x0F;
        p[13] = sum;
        for (int i = 0; i < 13; i++) {
            size_t ci = (size_t)(ord - 1) * 13 + (size_t)i;
            uint16_t ch = ci < len ? (uint8_t)e->name[ci] : ci == len ? 0 : 0xFFFF;
            put16(p + pos[i], ch);
        }
    }
}

/* ── Layout and writing ──────────────────────────────────────────────────── */

static uint32_t dir_slots(fent_t *d) {
    uint32_t n = d->parent ? 2 : 1;                        /* . and .., or the label */
    for (int i = 0; i < d->nkids; i++) n += (uint32_t)name_slots(d->kids[i], i + 1);
    return n;
}

static void assign(fent_t *d) {
    d->nclus = (dir_slots(d) * 32 + g_cs - 1) / g_cs;
    d->clus = alloc_run(d->nclus);
    for (int i = 0; i < d->nkids; i++) {
        fent_t *e = d->kids[i];
        if (e->is_dir) {
            assign(e);
        } else {
            e->nclus = (uint32_t)((e->file->len + g_cs - 1) / g_cs);
            e->clus = alloc_run(e->nclus);
        }
    }
}

static void write_dir(fent_t *d, const char *label) {
    size_t bytes = (size_t)d->nclus * g_cs;
    uint8_t *buf = xcalloc(bytes, 1);
    uint8_t *p = buf;
    if (d->parent) {
        uint8_t dot[11], dotdot[11];
        memset(dot, ' ', 11); dot[0] = '.';
        memset(dotdot, ' ', 11); dotdot[0] = dotdot[1] = '.';
        dirent83(p, dot, 0x10, d->clus, 0); p += 32;
        dirent83(p, dotdot, 0x10, d->parent->parent ? d->parent->clus : 0, 0); p += 32;
    } else {
        uint8_t vl[11];
        memset(vl, ' ', 11);
        memcpy(vl, label, strlen(label) < 11 ? strlen(label) : 11);
        dirent83(p, vl, 0x08, 0, 0); p += 32;
    }
    for (int i = 0; i < d->nkids; i++) {
        fent_t *e = d->kids[i];
        int slots = name_slots(e, i + 1);
        if (slots > 1) { lfn_slots(p, e, slots - 1); p += (slots - 1) * 32; }
        dirent83(p, e->short_name, e->is_dir ? 0x10 : 0x20, e->clus,
                 e->is_dir ? 0 : (uint32_t)e->file->len);
        p += 32;
    }
    dev_write(clus_off(d->clus), buf, bytes);
    free(buf);
    for (int i = 0; i < d->nkids; i++)
        if (d->kids[i]->is_dir) write_dir(d->kids[i], label);
}

static void write_files(fent_t *d, uint64_t *done, uint64_t total) {
    for (int i = 0; i < d->nkids; i++) {
        fent_t *e = d->kids[i];
        if (e->is_dir) { write_files(e, done, total); continue; }
        inst_file_t *f = e->file;
        uint64_t off = clus_off(e->clus);
        if (f->data) {
            dev_write(off, f->data, (size_t)f->len);
            *done += f->len;
            continue;
        }
        int fd = open(f->src, O_RDONLY);
        if (fd < 0) die("cannot open %s", f->src);
        size_t bs = 256 * 1024;
        uint8_t *buf = xmalloc(bs);
        uint64_t left = f->len;
        while (left) {
            size_t want = left < bs ? (size_t)left : bs;
            int r = (int)read(fd, buf, (int)want);
            if (r <= 0) die("read error on %s", f->src);
            dev_write(off, buf, (size_t)r);
            off += (uint64_t)r;
            left -= (uint64_t)r;
            *done += (uint64_t)r;
            progress("writing the EFI system partition", *done, total);
        }
        free(buf);
        close(fd);
    }
}

void fat32_build(uint64_t start, uint64_t nsect, const char *label,
                 inst_file_t *files, int nfiles) {
    if (nsect > 0xFFFFFFFFull) nsect = 0xFFFFFFFFull;
    g_base = start * 512;
    /* The largest cluster that still leaves the 65525 clusters FAT32 needs. */
    for (g_spc = 8; g_spc > 1; g_spc /= 2)
        if ((nsect - RESERVED) / g_spc > 66000) break;
    g_cs = g_spc * 512;
    uint32_t total = (uint32_t)nsect;
    g_fatsz = 1;
    for (int it = 0; it < 8; it++) {
        g_nclus = (total - RESERVED - NFATS * g_fatsz) / g_spc;
        g_fatsz = ((g_nclus + 2) * 4 + 511) / 512;
    }
    g_nclus = (total - RESERVED - NFATS * g_fatsz) / g_spc;
    if (g_nclus < 65525) die("the EFI system partition is too small for FAT32");
    g_fat = xcalloc((size_t)g_fatsz * 128, 4);
    g_fat[0] = 0x0FFFFFF8;
    g_fat[1] = 0x0FFFFFFF;

    /* A fixed timestamp keeps the image reproducible from the same inputs. */
    uint32_t t = now32();
    uint32_t days = t / 86400, secs = t % 86400;
    uint32_t y = 1970;
    for (;;) {
        uint32_t yd = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 366 : 365;
        if (days < yd) break;
        days -= yd; y++;
    }
    static const uint8_t mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    uint32_t mo = 0;
    for (; mo < 12; mo++) {
        uint32_t md = mdays[mo] + (mo == 1 && y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
        if (days < md) break;
        days -= md;
    }
    if (y < 1980) y = 1980;
    g_dosdate = ((y - 1980) << 9) | ((mo + 1) << 5) | (days + 1);
    g_dostime = ((secs / 3600) << 11) | (((secs / 60) % 60) << 5) | ((secs % 60) / 2);

    /* The tree. */
    fent_t root;
    memset(&root, 0, sizeof(root));
    uint64_t total_bytes = 0;
    for (int i = 0; i < nfiles; i++) {
        char path[256];
        snprintf(path, sizeof(path), "%s", files[i].dst);
        fent_t *d = &root;
        char *s = path + 1, *slash;
        while ((slash = strchr(s, '/')) != NULL) {
            *slash = '\0';
            d = child(d, s, 1);
            s = slash + 1;
        }
        fent_t *e = child(d, s, 0);
        e->file = &files[i];
        total_bytes += files[i].len;
    }
    assign(&root);
    if (root.clus != 2) die("internal: the FAT32 root is not cluster 2");

    /* Zero the reserved area and both FATs' worth of space is overwritten
     * below; the reserved sectors are written whole. */
    uint8_t *res = xcalloc(RESERVED, 512);
    uint8_t *bs = res;
    bs[0] = 0xEB; bs[1] = 0x58; bs[2] = 0x90;
    memcpy(bs + 3, "MAEROS  ", 8);
    put16(bs + 11, 512);
    bs[13] = (uint8_t)g_spc;
    put16(bs + 14, RESERVED);
    bs[16] = NFATS;
    bs[21] = 0xF8;
    put16(bs + 24, 63);
    put16(bs + 26, 255);
    put32(bs + 28, (uint32_t)start);
    put32(bs + 32, total);
    put32(bs + 36, g_fatsz);
    put32(bs + 44, 2);                                     /* root cluster */
    put16(bs + 48, 1);                                     /* FSInfo */
    put16(bs + 50, 6);                                     /* backup boot sector */
    bs[64] = 0x80;
    bs[66] = 0x29;
    uint32_t volid;
    random_bytes(&volid, sizeof(volid));
    put32(bs + 67, volid);
    memset(bs + 71, ' ', 11);
    memcpy(bs + 71, label, strlen(label) < 11 ? strlen(label) : 11);
    memcpy(bs + 82, "FAT32   ", 8);
    bs[510] = 0x55; bs[511] = 0xAA;
    uint8_t *fsi = res + 512;
    put32(fsi + 0, 0x41615252);
    put32(fsi + 484, 0x61417272);
    put32(fsi + 488, g_nclus - (g_next - 2));              /* free clusters */
    put32(fsi + 492, g_next);                              /* next free */
    put32(fsi + 508, 0xAA550000);
    memcpy(res + 6 * 512, res, 1024);                      /* backups at 6, 7 */
    dev_write(g_base, res, RESERVED * 512);
    free(res);

    uint64_t done = 0;
    write_files(&root, &done, total_bytes);
    progress("writing the EFI system partition", total_bytes, total_bytes);
    printf("\n");
    write_dir(&root, label);
    for (uint32_t f = 0; f < NFATS; f++)
        dev_write(g_base + ((uint64_t)RESERVED + (uint64_t)f * g_fatsz) * 512,
                  g_fat, (size_t)g_fatsz * 512);
    free(g_fat);
}
