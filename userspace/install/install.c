/*
 * maeros-install — install the running MaeroOS onto a disk.
 *
 *   maeros-install                       interactive: pick a disk, confirm
 *   maeros-install [-y] /dev/sdb         non-interactive with -y
 *   maeros-install -l                    list the disks
 *
 * The disk gets a GPT with three partitions:
 *
 *   1  BIOS boot (1 MiB)       Limine's BIOS stage 2 (limine bios-install)
 *   2  EFI System (FAT32)      /EFI/BOOT/BOOTX64.EFI, BOOTIA32.EFI,
 *                              /boot/limine/{limine.conf,limine-bios.sys},
 *                              /boot/kernel.elf, /boot/initrd.tar
 *   3  MaeroOS root (ext2)     a copy of /disk
 *
 * and Limine's BIOS stage 1 in the protective MBR, so it boots under legacy
 * BIOS and UEFI (x86_64 and IA32) alike.  limine.conf passes
 * root=PARTUUID=<partition 3> on the kernel command line, and the kernel
 * mounts that partition at /disk (kernel/main.c).
 *
 * Sources: the running kernel and Limine files from /boot (the live ISO's
 * initrd carries them, see the limine-iso target), the initrd itself from
 * /dev/initrd, the root tree from /disk.  Each can be overridden (--boot-dir,
 * --initrd, --source), which is also how the same program runs on a Linux
 * host against an image file.
 *
 * The BIOS stage install follows limine.c's bios_install() (Limine,
 * BSD-2-Clause, https://github.com/limine-bootloader/limine): stage 1 is the
 * first sector of limine-bios-hdd.bin, written over the MBR boot code with the
 * partition table and timestamp kept; the rest goes to the BIOS boot
 * partition, whose byte offset is stored at MBR offset 0x1A4.
 */
#include "install.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#ifdef __linux__
#include <sys/random.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#else
#include <syscall.h>
#define BLKGETSIZE64 0x80041272u           /* <linux/fs.h>, i386: u64 bytes */
#endif

#define MiB (1024u * 1024u / 512u)          /* sectors */

static int      g_fd = -1;
static uint64_t g_disk_sect;
static int      g_tty;

/* ── Helpers ─────────────────────────────────────────────────────────────── */

void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "\nmaeros-install: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
    __builtin_unreachable();
}

void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) die("out of memory (%lu bytes)", (unsigned long)n);
    return p;
}

void *xcalloc(size_t n, size_t m) {
    void *p = calloc(n ? n : 1, m ? m : 1);
    if (!p) die("out of memory (%lu x %lu bytes)", (unsigned long)n, (unsigned long)m);
    return p;
}

char *xstrdup(const char *s) {
    size_t l = strlen(s) + 1;
    char *d = xmalloc(l);
    memcpy(d, s, l);
    return d;
}

void random_bytes(void *buf, size_t n) {
    if (getrandom(buf, (unsigned)n, 0) != (int)n) die("getrandom failed");
}

uint32_t now32(void) { return (uint32_t)time(NULL); }

void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }
void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }
uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void progress(const char *what, uint64_t done, uint64_t total) {
    static int last = -1;
    static const char *last_what;
    int pct = total ? (int)(done * 100 / total) : 100;
    if (what == last_what && pct == last) return;
    if (!g_tty && what == last_what && pct / 10 == last / 10 && pct != 100) return;
    last = pct;
    last_what = what;
    printf(g_tty ? "\r  %s: %3d%%" : "  %s: %d%%\n", what, pct);
    fflush(stdout);
}

/* ── The device ──────────────────────────────────────────────────────────── */

void dev_write(uint64_t off, const void *buf, size_t len) {
    const uint8_t *p = buf;
    while (len) {
        size_t n = len > (1u << 20) ? (1u << 20) : len;
#ifdef __linux__
        long r = (long)pwrite(g_fd, p, n, (off_t)off);
#else
        /* pwrite64 with the full 64-bit offset (lo, hi). */
        long r = syscall(181, g_fd, p, (long)n, (long)(uint32_t)off, (long)(uint32_t)(off >> 32));
#endif
        if (r <= 0) die("write error at byte %llu of the target disk (%ld)",
                        (unsigned long long)off, r);
        p += r; off += (uint64_t)r; len -= (size_t)r;
    }
}

void dev_read(uint64_t off, void *buf, size_t len) {
    uint8_t *p = buf;
    while (len) {
#ifdef __linux__
        long r = (long)pread(g_fd, p, len, (off_t)off);
#else
        long r = syscall(180, g_fd, p, (long)len, (long)(uint32_t)off, (long)(uint32_t)(off >> 32));
#endif
        if (r <= 0) die("read error at byte %llu of the target disk", (unsigned long long)off);
        p += r; off += (uint64_t)r; len -= (size_t)r;
    }
}

/* ── Disks: /proc/partitions and /proc/mounts ────────────────────────────── */

typedef struct { char name[16]; uint64_t sectors; int in_use; } disk_t;

static int is_whole_disk(const char *n) {
    size_t l = strlen(n);
    if ((n[0] == 'h' || n[0] == 's') && n[1] == 'd' && l == 3) return 1;
    if (strncmp(n, "nvme", 4) == 0 && !strchr(n + 4, 'p')) return 1;
    return 0;
}

/* Whether `dev` (a /dev path from /proc/mounts) lives on disk `disk`. */
static int on_disk(const char *dev, const char *disk) {
    if (strncmp(dev, "/dev/", 5) != 0) return 0;
    dev += 5;
    size_t l = strlen(disk);
    if (strncmp(dev, disk, l) != 0) return 0;
    const char *rest = dev + l;
    if (*rest == 'p') rest++;
    for (; *rest; rest++) if (*rest < '0' || *rest > '9') return 0;
    return 1;
}

static int list_disks(disk_t *out, int max) {
    FILE *f = fopen("/proc/partitions", "r");
    if (!f) return 0;
    char line[160];
    int n = 0;
    while (fgets(line, sizeof(line), f) && n < max) {
        unsigned maj, min;
        unsigned long long kb;
        char name[32];
        if (sscanf(line, "%u %u %llu %31s", &maj, &min, &kb, name) != 4) continue;
        if (!is_whole_disk(name)) continue;
        snprintf(out[n].name, sizeof(out[n].name), "%s", name);
        out[n].sectors = kb * 2;
        out[n].in_use = 0;
        n++;
    }
    fclose(f);
    f = fopen("/proc/mounts", "r");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            char dev[64];
            if (sscanf(line, "%63s", dev) != 1) continue;
            for (int i = 0; i < n; i++)
                if (on_disk(dev, out[i].name)) out[i].in_use = 1;
        }
        fclose(f);
    }
    return n;
}

static void print_disks(disk_t *d, int n) {
    if (!n) { printf("No disks found.\n"); return; }
    for (int i = 0; i < n; i++)
        printf("  /dev/%-10s %8llu MiB%s\n", d[i].name,
               (unsigned long long)(d[i].sectors / MiB),
               d[i].in_use ? "   (in use)" : "");
}

/* ── GPT ─────────────────────────────────────────────────────────────────── */

static uint32_t crc32(const uint8_t *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

/* GUID text "aabbccdd-eeff-gghh-..." to its on-disk byte order. */
static void guid_parse(const char *s, uint8_t g[16]) {
    static const int order[16] = { 3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15 };
    uint8_t b[16];
    int k = 0;
    for (const char *p = s; *p && k < 16; ) {
        if (*p == '-') { p++; continue; }
        unsigned v;
        sscanf(p, "%2x", &v);
        b[k++] = (uint8_t)v;
        p += 2;
    }
    for (int i = 0; i < 16; i++) g[order[i]] = b[i];
}

static void guid_str(const uint8_t g[16], char out[37]) {
    static const int order[16] = { 3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15 };
    char *p = out;
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) *p++ = '-';
        p += sprintf(p, "%02x", g[order[i]]);
    }
    *p = '\0';
}

static void guid_random(uint8_t g[16]) {
    random_bytes(g, 16);
    g[7] = (uint8_t)((g[7] & 0x0F) | 0x40);                /* version 4 */
    g[8] = (uint8_t)((g[8] & 0x3F) | 0x80);                /* RFC 4122 variant */
}

typedef struct { const char *type; const char *name; uint64_t first, last; uint8_t guid[16]; } gpart_t;

static void put_utf16(uint8_t *p, const char *s, int max) {
    for (int i = 0; i < max && s[i]; i++) put16(p + i * 2, (uint8_t)s[i]);
}

static void write_gpt(gpart_t *parts, int nparts) {
    uint64_t n = g_disk_sect;
    /* Protective MBR: one 0xEE entry over the whole disk. */
    uint8_t mbr[512];
    memset(mbr, 0, sizeof(mbr));
    uint8_t *e = mbr + 446;
    e[1] = 0x00; e[2] = 0x02; e[3] = 0x00;                 /* CHS 0/0/2 */
    e[4] = 0xEE;
    e[5] = 0xFF; e[6] = 0xFF; e[7] = 0xFF;
    put32(e + 8, 1);
    put32(e + 12, n - 1 > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)(n - 1));
    mbr[510] = 0x55; mbr[511] = 0xAA;
    dev_write(0, mbr, 512);

    uint8_t *ents = xcalloc(128, 128);                     /* 32 sectors */
    for (int i = 0; i < nparts; i++) {
        uint8_t *pe = ents + i * 128;
        guid_parse(parts[i].type, pe);
        memcpy(pe + 16, parts[i].guid, 16);
        put64(pe + 32, parts[i].first);
        put64(pe + 40, parts[i].last);
        put_utf16(pe + 56, parts[i].name, 36);
    }
    uint32_t ecrc = crc32(ents, 128 * 128);
    uint8_t disk_guid[16];
    guid_random(disk_guid);
    for (int backup = 0; backup < 2; backup++) {
        uint8_t h[512];
        memset(h, 0, sizeof(h));
        memcpy(h, "EFI PART", 8);
        put32(h + 8, 0x00010000);
        put32(h + 12, 92);
        put64(h + 24, backup ? n - 1 : 1);                 /* this header */
        put64(h + 32, backup ? 1 : n - 1);                 /* the other one */
        put64(h + 40, 34);                                 /* first usable */
        put64(h + 48, n - 34);                             /* last usable */
        memcpy(h + 56, disk_guid, 16);
        put64(h + 72, backup ? n - 33 : 2);                /* entry array */
        put32(h + 80, 128);
        put32(h + 84, 128);
        put32(h + 88, ecrc);
        put32(h + 16, crc32(h, 92));
        dev_write((backup ? n - 33 : 2) * 512, ents, 128 * 128);
        dev_write((backup ? n - 1 : 1) * 512, h, 512);
    }
    free(ents);
}

/* ── Limine ──────────────────────────────────────────────────────────────── */

static uint8_t *slurp(const char *path, uint64_t *len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) die("cannot open %s", path);
    struct stat st;
    if (fstat(fd, &st) < 0) die("cannot stat %s", path);
    uint8_t *buf = xmalloc((size_t)st.st_size + 1);
    size_t got = 0;
    while (got < (size_t)st.st_size) {
        int r = (int)read(fd, buf + got, (int)((size_t)st.st_size - got));
        if (r <= 0) die("read error on %s", path);
        got += (size_t)r;
    }
    close(fd);
    buf[got] = '\0';
    *len = got;
    return buf;
}

static void limine_bios_install(const char *hdd_bin, uint64_t stage2_sector) {
    uint64_t len;
    uint8_t *img = slurp(hdd_bin, &len);
    if (len < 1024 || img[510] != 0x55 || img[511] != 0xAA)
        die("%s is not limine-bios-hdd.bin", hdd_bin);
    if (len - 512 > 1024 * 1024) die("Limine's stage 2 does not fit the BIOS boot partition");
    uint8_t orig[512], mbr[512];
    dev_read(0, orig, 512);
    memcpy(mbr, img, 512);
    memcpy(mbr + 218, orig + 218, 6);                      /* timestamp */
    memcpy(mbr + 440, orig + 440, 70);                     /* signature + table */
    put64(mbr + 0x1A4, stage2_sector * 512);
    dev_write(stage2_sector * 512, img + 512, (size_t)(len - 512));
    dev_write(0, mbr, 512);
    free(img);
}

/* ── Main ────────────────────────────────────────────────────────────────── */

static void usage(void) {
    printf("usage: maeros-install [-y] [-l] [-n] [--source DIR] [--boot-dir DIR]\n"
           "                      [--initrd FILE] [--esp-mib N] [/dev/DISK]\n"
           "  Installs the running system onto DISK (GPT: BIOS boot, EFI system,\n"
           "  ext2 root), bootable under BIOS and UEFI.  Everything on DISK is lost.\n"
           "  -y  do not ask for confirmation    -l  list the disks and exit\n"
           "  -n  dry run: print the layout, write nothing\n");
}

static int ask(const char *q, char *buf, int len) {
    printf("%s", q);
    fflush(stdout);
    if (!fgets(buf, len, stdin)) return -1;
    buf[strcspn(buf, "\r\n")] = '\0';
    return 0;
}

int main(int argc, char **argv) {
    const char *source = "/disk", *boot = "/boot", *initrd = "/dev/initrd";
    const char *target = NULL;
    int yes = 0, list = 0, dry = 0;
    uint64_t esp_mib = 0;
    g_tty = isatty(1);
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-y") == 0 || strcmp(argv[i], "--yes") == 0) yes = 1;
        else if (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--list") == 0) list = 1;
        else if (strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "--dry-run") == 0) dry = 1;
        else if (strcmp(argv[i], "--source") == 0 && i + 1 < argc) source = argv[++i];
        else if (strcmp(argv[i], "--boot-dir") == 0 && i + 1 < argc) boot = argv[++i];
        else if (strcmp(argv[i], "--initrd") == 0 && i + 1 < argc) initrd = argv[++i];
        else if (strcmp(argv[i], "--esp-mib") == 0 && i + 1 < argc) esp_mib = strtoull(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) { usage(); return 0; }
        else if (argv[i][0] != '-' && !target) target = argv[i];
        else { usage(); return 2; }
    }

    disk_t disks[20];
    int ndisks = list_disks(disks, 20);
    if (list) { print_disks(disks, ndisks); return 0; }

    char line[64];
    if (!target) {
        if (yes) die("-y needs a target disk");
        printf("MaeroOS installer\n\nDisks:\n");
        print_disks(disks, ndisks);
        if (ask("\nInstall to which disk (e.g. sdb, empty to cancel)? ", line, sizeof(line)) < 0 || !line[0])
            return 1;
        static char path[80];
        snprintf(path, sizeof(path), "%s%s", strncmp(line, "/dev/", 5) ? "/dev/" : "", line);
        target = path;
    }

    /* The target and its size. */
    g_fd = open(target, O_RDWR);
    if (g_fd < 0) die("cannot open %s for writing (are you root?)", target);
    struct stat st;
    if (fstat(g_fd, &st) < 0) die("cannot stat %s", target);
    const char *base = strrchr(target, '/') ? strrchr(target, '/') + 1 : target;
    if (S_ISREG(st.st_mode)) {
        g_disk_sect = (uint64_t)st.st_size / 512;
    } else {
        for (int i = 0; i < ndisks; i++)
            if (strcmp(disks[i].name, base) == 0) {
                if (disks[i].in_use) die("%s holds a mounted filesystem; pick another disk", target);
                g_disk_sect = disks[i].sectors;
            }
        if (!g_disk_sect) die("%s is not a whole disk (see maeros-install -l)", target);
        /* /proc/partitions counts 1 KiB blocks, which loses an odd last
         * sector, and shows only what the kernel can address.  BLKGETSIZE64
         * is the drive's exact size: the backup GPT goes in its last sector,
         * so the two must agree. */
        uint64_t bytes = 0;
        if (ioctl(g_fd, BLKGETSIZE64, &bytes) < 0 || bytes < 512)
            die("cannot get the size of %s (BLKGETSIZE64)", target);
        uint64_t exact = bytes / 512;
        if (exact / 2 != g_disk_sect / 2)
            die("%s has %llu MiB, but MaeroOS can address only its first %llu MiB "
                "(an IDE disk past 128 GiB: LBA28); the backup GPT would not be at "
                "its end", target, (unsigned long long)(exact / MiB),
                (unsigned long long)(g_disk_sect / MiB));
        g_disk_sect = exact;
        /* The block layer counts sectors in 32 bits and saturates: a disk
         * that shows 2 TiB may be larger, so its last sector (the backup GPT)
         * is unknown. */
        if (g_disk_sect >= 0xFFFFFFFEull)
            die("%s is 2 TiB or larger; MaeroOS addresses disks up to 2 TiB only", target);
    }

    /* What goes on the ESP. */
    char p_kernel[256], p_sys[256], p_hdd[256], p_x64[256], p_ia32[256], p_lic[256];
    snprintf(p_kernel, sizeof(p_kernel), "%s/kernel.elf", boot);
    snprintf(p_sys, sizeof(p_sys), "%s/limine/limine-bios.sys", boot);
    snprintf(p_hdd, sizeof(p_hdd), "%s/limine/limine-bios-hdd.bin", boot);
    snprintf(p_x64, sizeof(p_x64), "%s/limine/BOOTX64.EFI", boot);
    snprintf(p_ia32, sizeof(p_ia32), "%s/limine/BOOTIA32.EFI", boot);
    snprintf(p_lic, sizeof(p_lic), "%s/limine/LICENSE", boot);
    static char conf[1024];
    inst_file_t files[] = {
        { "/EFI/BOOT/BOOTX64.EFI", p_x64, NULL, 0 },
        { "/EFI/BOOT/BOOTIA32.EFI", p_ia32, NULL, 0 },
        { "/boot/limine/limine.conf", NULL, (const uint8_t *)conf, 0 },
        { "/boot/limine/limine-bios.sys", p_sys, NULL, 0 },
        { "/boot/limine/LICENSE", p_lic, NULL, 0 },
        { "/boot/kernel.elf", p_kernel, NULL, 0 },
        { "/boot/initrd.tar", initrd, NULL, 0 },
    };
    int nfiles = (int)(sizeof(files) / sizeof(files[0]));
    uint64_t esp_bytes = 0;
    for (int i = 0; i < nfiles; i++) {
        if (!files[i].src) continue;
        struct stat fs;
        if (stat(files[i].src, &fs) < 0 || fs.st_size == 0)
            die("%s is missing: this system was not booted from the Limine live image "
                "(make limine-iso), or pass --boot-dir/--initrd", files[i].src);
        files[i].len = (uint64_t)fs.st_size;
        esp_bytes += files[i].len;
    }
    struct stat hs;
    if (stat(p_hdd, &hs) < 0) die("%s is missing", p_hdd);

    /* Layout: BIOS boot 1 MiB at 1 MiB, ESP after it, root to the end. */
    if (!esp_mib) {
        esp_mib = esp_bytes / (1024 * 1024) * 2 + 64;      /* room for an update */
        if (esp_mib < 128) esp_mib = 128;
    }
    printf("Scanning %s...\n", source);
    const char *skip[16];
    int nskip = 0;
    FILE *mf = fopen("/proc/mounts", "r");
    static char mnt[16][128];
    if (mf) {
        /* Mounts below the source (proc in a chroot, say) are copied as
         * empty directories. */
        char l[256], dev[128], dir[128];
        size_t sl = strlen(source);
        while (fgets(l, sizeof(l), mf) && nskip < 16)
            if (sscanf(l, "%127s %127s", dev, dir) == 2 && strncmp(dir, source, sl) == 0 &&
                dir[sl] == '/') {
                snprintf(mnt[nskip], sizeof(mnt[nskip]), "%s", dir);
                skip[nskip] = mnt[nskip];
                nskip++;
            }
        fclose(mf);
    }
    uint64_t need_blocks;
    uint32_t need_inodes;
    ext2_scan(source, skip, nskip, &need_blocks, &need_inodes);

    gpart_t parts[3] = {
        { "21686148-6449-6e6f-744e-656564454649", "BIOS boot", 1 * MiB, 2 * MiB - 1, {0} },
        { "c12a7328-f81f-11d2-ba4b-00a0c93ec93b", "EFI system", 2 * MiB, 0, {0} },
        { "0fc63daf-8483-4772-8e79-3d69d8477de4", "MaeroOS root", 0, 0, {0} },
    };
    parts[1].last = parts[1].first + esp_mib * MiB - 1;
    parts[2].first = parts[1].last + 1;
    parts[2].last = g_disk_sect >= 34 ? ((g_disk_sect - 34 + 1) & ~(uint64_t)2047) - 1 : 0;
    /* The root is at most ext2_max_sectors() (1 TiB); the rest of a bigger
     * disk is left unpartitioned. */
    if (parts[2].last >= parts[2].first &&
        parts[2].last - parts[2].first + 1 > ext2_max_sectors())
        parts[2].last = parts[2].first + ext2_max_sectors() - 1;
    /* Metadata is under 4% of the root; 32 MiB of slack on top. */
    uint64_t root_need = need_blocks * 2 + need_blocks / 12 + (uint64_t)need_inodes / 4 + 32 * MiB;
    if (parts[2].last <= parts[2].first || parts[2].last - parts[2].first + 1 < root_need)
        die("%s is too small: it needs at least %llu MiB", target,
            (unsigned long long)((parts[2].first + root_need + 34) / MiB + 1));
    for (int i = 0; i < 3; i++) guid_random(parts[i].guid);
    char root_uuid[37];
    guid_str(parts[2].guid, root_uuid);

    printf("\nTarget %s, %llu MiB:\n", target, (unsigned long long)(g_disk_sect / MiB));
    for (int i = 0; i < 3; i++)
        printf("  partition %d  %-13s %8llu MiB\n", i + 1, parts[i].name,
               (unsigned long long)((parts[i].last - parts[i].first + 1) / MiB));
    uint64_t unpart = g_disk_sect - 34 - parts[2].last;
    if (unpart >= MiB)
        printf("  (%llu MiB at the end left unpartitioned)\n", (unsigned long long)(unpart / MiB));
    if (dry) {
        uint64_t b; uint32_t g, in, m0;
        ext2_geometry(parts[2].last - parts[2].first + 1, &b, &g, &in, &m0);
        printf("ext2: %llu blocks, %u groups, %u inodes, group 0 metadata %u of 8192 blocks\n"
               "backup GPT at LBA %llu\nDry run: nothing written.\n",
               (unsigned long long)b, (unsigned)g, (unsigned)in, (unsigned)m0,
               (unsigned long long)(g_disk_sect - 1));
        return 0;
    }
    if (!yes) {
        if (ask("\nEVERYTHING on this disk will be erased. Type \"yes\" to continue: ",
                line, sizeof(line)) < 0 || strcmp(line, "yes") != 0) {
            printf("Cancelled.\n");
            return 1;
        }
    }

    printf("\n[1/4] Partitioning (GPT)\n");
    write_gpt(parts, 3);

    printf("[2/4] EFI system partition (FAT32)\n");
    snprintf(conf, sizeof(conf),
             "# Written by maeros-install.  Limine boots the kernel through\n"
             "# Multiboot 2 from BIOS and UEFI alike; root= is the ext2 root.\n"
             "timeout: 3\n"
             "serial: yes\n"
             "\n"
             "/MaeroOS\n"
             "    protocol: multiboot2\n"
             "    path: boot():/boot/kernel.elf\n"
             "    cmdline: root=PARTUUID=%s\n"
             "    module_path: boot():/boot/initrd.tar\n"
             "    module_string: initrd\n"
             "    resolution: 1920x1080x32\n", root_uuid);
    files[2].len = strlen(conf);
    fat32_build(parts[1].first, parts[1].last - parts[1].first + 1, "MAEROS ESP", files, nfiles);

    printf("[3/4] Root filesystem (ext2), copying %s\n", source);
    uint8_t fsuuid[16];
    guid_random(fsuuid);
    ext2_build(parts[2].first, parts[2].last - parts[2].first + 1, "maeros-root", fsuuid);

    printf("[4/4] Limine BIOS boot code\n");
    limine_bios_install(p_hdd, parts[0].first);

    fsync(g_fd);
    close(g_fd);
    sync();
    printf("\nDone.  %s boots MaeroOS with root=PARTUUID=%s\n"
           "Remove the live medium and reboot.\n", target, root_uuid);
    return 0;
}
