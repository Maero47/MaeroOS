/*
 * tar — ustar archive create/extract/list
 * Usage: tar -c -f ARCHIVE FILE...   (create)
 *        tar -x -f ARCHIVE           (extract)
 *        tar -t -f ARCHIVE           (list)
 *        tar -xf ARCHIVE, tar -tf ARCHIVE (compact)
 */
#include "../include/unistd.h"
#include "../include/stdio.h"
#include "../include/string.h"
#include "../include/stdlib.h"
#include "../include/sys/stat.h"

/* ustar header — 512 bytes */
typedef struct {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];   /* octal ASCII */
    char mtime[12];
    char checksum[8];
    char typeflag;   /* '0'=file, '5'=dir, '2'=symlink */
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char pad[12];
} __attribute__((packed)) tar_hdr_t;

static long octal_decode(const char *s, int len) {
    long v = 0;
    for (int i = 0; i < len && s[i] >= '0' && s[i] <= '7'; i++)
        v = v * 8 + (s[i] - '0');
    return v;
}

static void octal_encode(char *s, int len, long v) {
    s[--len] = '\0';
    s[--len] = ' ';
    while (len-- > 0) {
        s[len] = '0' + (v & 7);
        v >>= 3;
    }
}

static unsigned compute_checksum(tar_hdr_t *h) {
    memset(h->checksum, ' ', 8);
    unsigned sum = 0;
    unsigned char *p = (unsigned char *)h;
    for (int i = 0; i < 512; i++) sum += p[i];
    return sum;
}

/* ── Extract ─────────────────────────────────────────────────────────────── */
static int do_extract(int afd, int verbose) {
    tar_hdr_t hdr;
    char block[512];
    while (1) {
        int n = (int)read(afd, &hdr, 512);
        if (n < 512) break;

        /* End-of-archive: two zero blocks */
        int zero = 1;
        for (int i = 0; i < 512; i++) if (((char *)&hdr)[i]) { zero = 0; break; }
        if (zero) break;

        /* Verify magic */
        if (strncmp(hdr.magic, "ustar", 5) != 0) break;

        long size = octal_decode(hdr.size, 12);
        char fullname[256];
        if (hdr.prefix[0]) {
            snprintf(fullname, sizeof(fullname), "%s/%s", hdr.prefix, hdr.name);
        } else {
            strncpy(fullname, hdr.name, 255);
        }
        fullname[255] = '\0';
        /* Strip leading ./ */
        const char *fname = fullname;
        if (fname[0] == '.' && fname[1] == '/') fname += 2;

        if (verbose) printf("%s\n", fname);

        if (hdr.typeflag == '5' || (hdr.typeflag == 0 && size == 0 && fname[strlen(fname)-1] == '/')) {
            /* Directory */
            mkdir(fname, 0755);
        } else if (hdr.typeflag == '2') {
            /* Symlink */
            symlink(hdr.linkname, fname);
        } else {
            /* Regular file */
            int ofd = open(fname, 0x241);  /* O_WRONLY|O_CREAT|O_TRUNC */
            long remaining = size;
            while (remaining > 0) {
                int to_read = (int)(remaining > 512 ? 512 : remaining);
                read(afd, block, 512);  /* always read full block */
                if (ofd >= 0) write(ofd, block, (unsigned)to_read);
                remaining -= to_read;
            }
            if (ofd >= 0) close(ofd);
            /* skip padding */
            long pad = (512 - (size % 512)) % 512;
            while (pad > 0) { read(afd, block, (unsigned)(pad > 512 ? 512 : pad)); pad -= 512; }
        }
    }
    return 0;
}

/* ── List ─────────────────────────────────────────────────────────────────── */
static int do_list(int afd) {
    tar_hdr_t hdr;
    char block[512];
    while (1) {
        if (read(afd, &hdr, 512) < 512) break;
        int zero = 1;
        for (int i = 0; i < 512; i++) if (((char *)&hdr)[i]) { zero = 0; break; }
        if (zero) break;
        if (strncmp(hdr.magic, "ustar", 5) != 0) break;
        long size = octal_decode(hdr.size, 12);
        printf("%s\n", hdr.name);
        long blks = (size + 511) / 512;
        for (long i = 0; i < blks; i++) read(afd, block, 512);
    }
    return 0;
}

/* ── Create ─────────────────────────────────────────────────────────────────── */
static void append_file(int afd, const char *path) {
    struct stat st;
    if (stat(path, &st) < 0) { printf("tar: %s: not found\n", path); return; }

    tar_hdr_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    strncpy(hdr.name, path, 99);
    octal_encode(hdr.mode,  8, 0644);
    octal_encode(hdr.uid,   8, 0);
    octal_encode(hdr.gid,   8, 0);
    octal_encode(hdr.size, 12, st.st_size);
    octal_encode(hdr.mtime,12, 0);
    hdr.typeflag = '0';
    memcpy(hdr.magic, "ustar", 5);
    hdr.version[0] = '0'; hdr.version[1] = '0';
    unsigned csum = compute_checksum(&hdr);
    snprintf(hdr.checksum, 8, "%06o", csum);

    write(afd, &hdr, 512);

    int ifd = open(path, 0);
    if (ifd < 0) { write(afd, &hdr, 512); return; }  /* write empty block */
    char block[512];
    long written = 0;
    int n;
    while ((n = (int)read(ifd, block, 512)) > 0) {
        if (n < 512) memset(block + n, 0, (size_t)(512 - n));
        write(afd, block, 512);
        written += n;
    }
    close(ifd);
}

static int do_create(int afd, char **files, int nfiles) {
    for (int i = 0; i < nfiles; i++) append_file(afd, files[i]);
    /* Two 512-byte zero blocks = EOF marker */
    char zero[512]; memset(zero, 0, 512);
    write(afd, zero, 512);
    write(afd, zero, 512);
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        write(2, "usage: tar [-c|-x|-t] [-f ARCHIVE] [FILE...]\n", 46);
        return 1;
    }

    int create = 0, extract = 0, list = 0, verbose = 0;
    const char *archive = NULL;
    char **files = NULL;
    int nfiles = 0;

    /* Parse flags */
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            const char *p = argv[i] + 1;
            while (*p) {
                if (*p == 'c') create = 1;
                else if (*p == 'x') extract = 1;
                else if (*p == 't') list = 1;
                else if (*p == 'v') verbose = 1;
                else if (*p == 'f' && i + 1 < argc) { archive = argv[++i]; break; }
                p++;
            }
        } else if (!archive) {
            archive = argv[i];
        } else {
            if (!files) files = &argv[i];
            nfiles = argc - i;
            break;
        }
    }

    if (!archive) { write(2, "tar: missing archive name\n", 26); return 1; }

    int afd;
    if (create) {
        afd = open(archive, 0x241);  /* O_WRONLY|O_CREAT|O_TRUNC */
        if (afd < 0) { printf("tar: cannot create %s\n", archive); return 1; }
        int r = do_create(afd, files, nfiles);
        close(afd);
        return r;
    } else {
        afd = open(archive, 0);
        if (afd < 0) { printf("tar: %s: not found\n", archive); return 1; }
        int r;
        if (extract) r = do_extract(afd, verbose);
        else if (list) r = do_list(afd);
        else { write(2, "tar: specify -c, -x, or -t\n", 27); r = 1; }
        close(afd);
        return r;
    }
}
