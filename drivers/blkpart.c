#include "blkpart.h"
#include "blkdev.h"
#include "../mm/heap.h"
#include "../lib/string.h"
#include "../lib/printf.h"
#include "../kernel/printk.h"
#include "../proc/scheduler.h"
#include <stddef.h>

/*
 * Partition tables: MBR (four primary entries, logical partitions chained
 * through extended boot records) and GPT (UEFI spec, chapter 5: header at
 * LBA 1 with a CRC32 over itself and over the entry array, a backup header in
 * the last sector).  Written from the specifications; nothing here is copied.
 */

static blkpart_t *g_parts[BLKPART_MAX];
static uint32_t   g_nparts;

/* ── Raw sector I/O ─────────────────────────────────────────────────────── */

int blkpart_read(blkpart_t *bp, uint32_t sector, uint32_t count, void *buf) {
    if (!bp || bp->gone || !count || count > 128) return -1;
    if (sector >= bp->nsect || count > bp->nsect - sector) return -1;
    return blk_disk_read(bp->dev, bp->start + sector, count, buf);
}

int blkpart_write(blkpart_t *bp, uint32_t sector, uint32_t count, const void *buf) {
    if (!bp || bp->gone || !count || count > 128) return -1;
    if (sector >= bp->nsect || count > bp->nsect - sector) return -1;
    return blk_disk_write(bp->dev, bp->start + sector, count, buf);
}

/* ── Byte-granular access, used by the /dev node and by pread64/pwrite64 ──── */

/* vfs_mount_any_source callback: `source` is /dev/<a partition or disk> on
 * disk *(int *)arg. */
static int source_on_disk(const char *source, void *arg) {
    if (strncmp(source, "/dev/", 5) != 0) return 0;
    blkpart_t *p = blkpart_find(source + 5);
    return p && p->dev == *(const int *)arg;
}

int blkpart_rw(blkpart_t *bp, uint64_t off, uint8_t *buf, uint32_t len, int write) {
    if (!bp) return -19;                                   /* -ENODEV */
    uint64_t size = (uint64_t)bp->nsect * 512u;
    if (len == 0) return 0;
    if (off >= size) return write ? -28 : 0;               /* -ENOSPC / EOF */
    if ((uint64_t)len > size - off) len = (uint32_t)(size - off);
    /* The disk ext2 has mounted at /disk, and any disk with a mounted
     * partition (vfat, ext4; a USB stick too), is never written behind the
     * filesystem's back. */
    if (write && (blk_disk_busy(bp->dev) ||
                  vfs_mount_any_source(source_on_disk, &bp->dev)))
        return -16;                                        /* -EBUSY */
    uint8_t *sec = NULL;
    uint32_t done = 0;
    int err = 0;
    while (done < len) {
        uint64_t pos = off + done;
        uint32_t in  = (uint32_t)(pos & 511u);
        uint32_t lba = (uint32_t)(pos >> 9);
        if (!in && len - done >= 512u) {
            /* Whole sectors straight to or from the caller's buffer. */
            uint32_t n = (len - done) >> 9;
            if (n > 128) n = 128;
            int r = write ? blkpart_write(bp, lba, n, buf + done)
                          : blkpart_read(bp, lba, n, buf + done);
            if (r < 0) { err = -5; break; }                /* -EIO */
            done += n << 9;
            continue;
        }
        /* A partial sector: read it, patch it, write it back. */
        if (!sec && !(sec = (uint8_t *)kmalloc(512))) { err = -12; break; }
        uint32_t n = 512u - in;
        if (n > len - done) n = len - done;
        if (blkpart_read(bp, lba, 1, sec) < 0) { err = -5; break; }
        if (write) {
            memcpy(sec + in, buf + done, n);
            if (blkpart_write(bp, lba, 1, sec) < 0) { err = -5; break; }
        } else {
            memcpy(buf + done, sec + in, n);
        }
        done += n;
    }
    if (sec) kfree(sec);
    return done ? (int)done : err;
}

/* ── /dev node: raw access (root only: mode 0660, root:root) ─────────────── */

static uint32_t blkpart_node_read(vfs_node_t *node, uint32_t off, uint32_t len,
                                  uint8_t *buf) {
    int r = blkpart_rw((blkpart_t *)node->private, off, buf, len, 0);
    return r < 0 ? 0 : (uint32_t)r;
}

static uint32_t blkpart_node_write(vfs_node_t *node, uint32_t off, uint32_t len,
                                   const uint8_t *buf) {
    return (uint32_t)blkpart_rw((blkpart_t *)node->private, off, (uint8_t *)buf,
                                len, 1);
}

/* BLKGETSIZE (sectors, unsigned long) and BLKGETSIZE64 (bytes, u64), as on
 * Linux: the exact size.  For a whole disk that is the drive's real size,
 * which for an IDE disk past 128 GiB is more than the LBA28 part this kernel
 * reads and writes (/proc/partitions shows that part). */
static int blkpart_node_ioctl(vfs_node_t *node, uint32_t req, void *arg) {
    blkpart_t *bp = (blkpart_t *)node->private;
    uint64_t sect = bp->partno ? bp->nsect : blk_disk_capacity(bp->dev);
    if (req == BLKGETSIZE) {
        *(uint32_t *)arg = sect > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)sect;
        return 0;
    }
    if (req == BLKGETSIZE64) {
        uint64_t bytes = sect * 512u;
        memcpy(arg, &bytes, sizeof(bytes));
        return 0;
    }
    return -25;                                            /* -ENOTTY */
}

static blkpart_t *blkpart_add(int dev, int partno, uint32_t start, uint32_t nsect,
                              const uint8_t *guid) {
    if (g_nparts >= BLKPART_MAX || !nsect) return NULL;
    uint32_t disk = blk_disk_sectors(dev);
    const char *dname = blk_disk_devname(dev);
    /* Linux names: hdb1, sda1, but nvme0n1p1 after a name ending in a digit. */
    size_t dl = strlen(dname);
    const char *sep = (dl && dname[dl - 1] >= '0' && dname[dl - 1] <= '9') ? "p" : "";
    if (start >= disk || nsect > disk - start) {
        printk("[PART] %s%s%d: extends past the end of the disk, ignored\n",
               dname, sep, partno);
        return NULL;
    }
    blkpart_t *bp = (blkpart_t *)kmalloc(sizeof(blkpart_t));
    if (!bp) return NULL;
    memset(bp, 0, sizeof(*bp));
    if (partno)
        snprintf(bp->name, sizeof(bp->name), "%s%s%d", dname, sep, partno);
    else
        snprintf(bp->name, sizeof(bp->name), "%s", dname);
    bp->dev    = dev;
    bp->start  = start;
    bp->nsect  = nsect;
    bp->partno = partno;
    if (guid) { memcpy(bp->guid, guid, 16); bp->has_guid = 1; }
    bp->rdev   = blk_disk_rdev(dev, partno);
    strncpy(bp->node.name, bp->name, 255);
    bp->node.flags   = VFS_FLAG_BLKDEV;
    bp->node.inode   = 0x30000000u + bp->rdev;
    bp->node.mask    = 0660;
    bp->node.size    = nsect >= 0x800000u ? 0xFFFFFFFFu : nsect * 512u;
    bp->node.rdev    = bp->rdev;
    bp->node.read_fn = blkpart_node_read;
    bp->node.write_fn = blkpart_node_write;
    bp->node.ioctl_fn = blkpart_node_ioctl;
    bp->node.private = bp;
    preempt_disable();
    g_parts[g_nparts++] = bp;
    preempt_enable();
    return bp;
}

/* ── GPT ─────────────────────────────────────────────────────────────────── */

static uint32_t crc32_ieee(uint32_t crc, const uint8_t *p, uint32_t n) {
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
static uint64_t rd64(const uint8_t *p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

/* Validate the GPT header at `lba` and register its partitions.  Returns the
 * number registered, or -1 when the header (or its entry array) is invalid. */
static int gpt_scan(int dev, uint32_t lba) {
    uint8_t *hdr = (uint8_t *)kmalloc(512);
    if (!hdr) return -1;
    int found = -1;
    uint8_t *ents = NULL;
    if (blk_disk_read(dev, lba, 1, hdr) < 0 || memcmp(hdr, "EFI PART", 8) != 0)
        goto out;
    uint32_t hsize = rd32(hdr + 12);
    if (hsize < 92 || hsize > 512) goto out;
    uint32_t want = rd32(hdr + 16);
    hdr[16] = hdr[17] = hdr[18] = hdr[19] = 0;
    if (crc32_ieee(0, hdr, hsize) != want) {
        printk("[PART] %s: GPT header at LBA %u fails its CRC\n",
               blk_disk_devname(dev), (unsigned)lba);
        goto out;
    }
    uint64_t ent_lba = rd64(hdr + 72);
    uint32_t nent    = rd32(hdr + 80);
    uint32_t esize   = rd32(hdr + 84);
    uint32_t ecrc    = rd32(hdr + 88);
    if (esize < 128 || esize > 512 || (esize & 7) || nent == 0 || nent > 1024 ||
        ent_lba >= blk_disk_sectors(dev))
        goto out;
    uint32_t bytes = nent * esize;
    uint32_t nsec  = (bytes + 511) / 512;
    ents = (uint8_t *)kmalloc(nsec * 512);
    if (!ents) goto out;
    for (uint32_t s = 0; s < nsec; ) {
        uint32_t k = nsec - s > 64 ? 64 : nsec - s;
        if (blk_disk_read(dev, (uint32_t)ent_lba + s, k, ents + s * 512) < 0)
            goto out;
        s += k;
    }
    if (crc32_ieee(0, ents, bytes) != ecrc) {
        printk("[PART] %s: GPT entry array fails its CRC\n", blk_disk_devname(dev));
        goto out;
    }
    found = 0;
    for (uint32_t i = 0; i < nent; i++) {
        const uint8_t *e = ents + i * esize;
        int used = 0;
        for (int k = 0; k < 16; k++) used |= e[k];
        if (!used) continue;
        uint64_t first = rd64(e + 32), last = rd64(e + 40);
        if (last < first || last >= 0xFFFFFFFFull) {
            printk("[PART] %s%u: beyond 2 TiB or malformed, ignored\n",
                   blk_disk_devname(dev), (unsigned)(i + 1));
            continue;
        }
        if (i + 1 > 99) break;
        if (blkpart_add(dev, (int)(i + 1), (uint32_t)first,
                        (uint32_t)(last - first + 1), e + 16))
            found++;
    }
out:
    if (ents) kfree(ents);
    kfree(hdr);
    return found;
}

/* ── MBR ─────────────────────────────────────────────────────────────────── */

static int mbr_is_extended(uint8_t type) {
    return type == 0x05 || type == 0x0F || type == 0x85;
}

/* Walk the chain of extended boot records starting at `ext_start` (the
 * extended partition): each EBR holds one logical partition, relative to the
 * EBR itself, and a link to the next EBR, relative to the extended partition. */
static void mbr_scan_logical(int dev, uint32_t ext_start, uint32_t ext_len,
                             uint8_t *sec) {
    uint32_t ebr = ext_start;
    int partno = 5;
    for (int hops = 0; hops < 64 && partno < 64; hops++) {
        if (blk_disk_read(dev, ebr, 1, sec) < 0) return;
        if (sec[510] != 0x55 || sec[511] != 0xAA) return;
        const uint8_t *e0 = sec + 446, *e1 = sec + 462;
        if (e0[4] && rd32(e0 + 12))
            blkpart_add(dev, partno++, ebr + rd32(e0 + 8), rd32(e0 + 12), NULL);
        if (!mbr_is_extended(e1[4]) || !rd32(e1 + 12)) return;
        uint32_t next = ext_start + rd32(e1 + 8);
        if (next <= ebr || next >= ext_start + ext_len) return;   /* no loops */
        ebr = next;
    }
}

/* A FAT or exFAT boot sector: a disk formatted without a partition table
 * (a "superfloppy", common on USB sticks).  Its boot code would otherwise be
 * read as partition entries. */
static int is_fat_vbr(const uint8_t *sec) {
    if (memcmp(sec + 3, "EXFAT   ", 8) == 0) return 1;
    if (sec[0] != 0xEB && sec[0] != 0xE9) return 0;
    uint32_t bps = (uint32_t)sec[11] | ((uint32_t)sec[12] << 8);
    if (bps != 512 && bps != 1024 && bps != 2048 && bps != 4096) return 0;
    if (!sec[13] || (sec[13] & (sec[13] - 1)) || !sec[16]) return 0;
    return memcmp(sec + 54, "FAT", 3) == 0 || memcmp(sec + 82, "FAT32", 5) == 0;
}

static void scan_disk(int dev) {
    uint32_t n = blk_disk_sectors(dev);
    if (!blkpart_add(dev, 0, 0, n, NULL)) return;
    uint8_t *sec = (uint8_t *)kmalloc(512);
    if (!sec) return;
    if (blk_disk_read(dev, 0, 1, sec) < 0 || sec[510] != 0x55 || sec[511] != 0xAA)
        goto out;
    if (is_fat_vbr(sec)) {
        printk("[PART] %s: FAT filesystem on the whole disk, no partition table\n",
               blk_disk_devname(dev));
        goto out;
    }
    /* A protective MBR (type 0xEE) means GPT. */
    for (int i = 0; i < 4; i++) {
        if (sec[446 + i * 16 + 4] == 0xEE) {
            if (gpt_scan(dev, 1) < 0 && gpt_scan(dev, n - 1) < 0)
                printk("[PART] %s: protective MBR but no valid GPT\n",
                       blk_disk_devname(dev));
            goto out;
        }
    }
    /* A filesystem starting at sector 0 (no table) still ends in 0x55AA when
     * it is FAT; ext2/3/4 never does, since its superblock is at byte 1024.
     * Only accept entries whose status byte is 0x00 or 0x80. */
    for (int i = 0; i < 4; i++)
        if (sec[446 + i * 16] & 0x7F) goto out;
    uint32_t ext_start = 0, ext_len = 0;
    for (int i = 0; i < 4; i++) {
        const uint8_t *e = sec + 446 + i * 16;
        uint32_t start = rd32(e + 8), len = rd32(e + 12);
        if (!e[4] || !len) continue;
        if (mbr_is_extended(e[4])) {
            if (!ext_start) { ext_start = start; ext_len = len; }
            continue;
        }
        blkpart_add(dev, i + 1, start, len, NULL);
    }
    if (ext_start)
        mbr_scan_logical(dev, ext_start, ext_len, sec);
out:
    kfree(sec);
}

static void print_parts(uint32_t from) {
    for (uint32_t i = from; i < g_nparts; i++)
        if (g_parts[i]->partno)
            printk("[PART] %s: start %u, %u sectors\n", g_parts[i]->name,
                   (unsigned)g_parts[i]->start, (unsigned)g_parts[i]->nsect);
}

void blkpart_init(void) {
    for (int dev = 0; dev < blk_disk_count(); dev++)
        scan_disk(dev);
    print_parts(0);
}

void blkpart_scan(int dev) {
    uint32_t from = g_nparts;
    scan_disk(dev);
    print_parts(from);
}

void blkpart_drop(int dev) {
    preempt_disable();
    uint32_t k = 0;
    for (uint32_t i = 0; i < g_nparts; i++) {
        if (g_parts[i]->dev == dev) g_parts[i]->gone = 1;
        else g_parts[k++] = g_parts[i];
    }
    g_nparts = k;
    preempt_enable();
}

blkpart_t *blkpart_find(const char *name) {
    for (uint32_t i = 0; i < g_nparts; i++)
        if (strcmp(g_parts[i]->name, name) == 0) return g_parts[i];
    return NULL;
}

/* GPT GUIDs are printed with their first three fields little-endian:
 * bytes 3 2 1 0 - 5 4 - 7 6 - 8 9 - 10..15. */
void blkpart_guid_str(const uint8_t *g, char *out) {
    static const uint8_t order[16] = { 3, 2, 1, 0, 5, 4, 7, 6, 8, 9,
                                       10, 11, 12, 13, 14, 15 };
    static const char hex[] = "0123456789abcdef";
    char *p = out;
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) *p++ = '-';
        *p++ = hex[g[order[i]] >> 4];
        *p++ = hex[g[order[i]] & 15];
    }
    *p = '\0';
}

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

blkpart_t *blkpart_find_partuuid(const char *uuid) {
    char s[37];
    for (uint32_t i = 0; i < g_nparts; i++) {
        if (!g_parts[i]->has_guid) continue;
        blkpart_guid_str(g_parts[i]->guid, s);
        int k = 0;
        while (s[k] && lower((unsigned char)uuid[k]) == s[k]) k++;
        if (!s[k] && !uuid[k]) return g_parts[i];
    }
    return NULL;
}

blkpart_t *blkpart_get(uint32_t idx) {
    return idx < g_nparts ? g_parts[idx] : NULL;
}

blkpart_t *blkpart_from_node(vfs_node_t *node) {
    if (!node || node->flags != VFS_FLAG_BLKDEV) return NULL;
    for (uint32_t i = 0; i < g_nparts; i++)
        if (&g_parts[i]->node == node) return g_parts[i];
    return NULL;
}

uint32_t blkpart_format(char *buf, uint32_t size) {
    uint32_t pos = 0;
    if (!size) return 0;
    pos += (uint32_t)snprintf(buf, size, "major minor  #blocks  name\n\n");
    for (uint32_t i = 0; i < g_nparts && pos < size; i++) {
        blkpart_t *bp = g_parts[i];
        pos += (uint32_t)snprintf(buf + pos, size - pos, "%4u  %7u %10u %s\n",
                                  (unsigned)(bp->rdev >> 8),
                                  (unsigned)(bp->rdev & 0xFF),
                                  (unsigned)(bp->nsect / 2), bp->name);
    }
    if (pos > size) pos = size;
    return pos;
}
