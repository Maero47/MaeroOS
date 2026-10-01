/*
 * maeros-install: shared declarations.
 *
 * The installer is plain POSIX C so the same sources also build on a Linux
 * host (`cc -O2 userspace/install/[a-z]*.c`), where it writes a disk image file;
 * tools/smoke_install.py does not need that, but it is how the on-disk
 * formats were checked against e2fsck and fsck.fat while writing them.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

/* ── The target device ───────────────────────────────────────────────────── */

/* Absolute byte offsets on the target disk.  Exit with a message on error. */
void dev_write(uint64_t off, const void *buf, size_t len);
void dev_read(uint64_t off, void *buf, size_t len);

/* ── Helpers (install.c) ─────────────────────────────────────────────────── */

void   die(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
void  *xmalloc(size_t n);
void  *xcalloc(size_t n, size_t m);
char  *xstrdup(const char *s);
void   random_bytes(void *buf, size_t n);
uint32_t now32(void);
void   put16(uint8_t *p, uint32_t v);
void   put32(uint8_t *p, uint32_t v);
void   put64(uint8_t *p, uint64_t v);
uint32_t get32(const uint8_t *p);
/* Progress line on stdout, rewritten in place ("\r") when it is a terminal. */
void   progress(const char *what, uint64_t done, uint64_t total);

/* A file the installer copies: from `src` (a path), or `data`/`len` in memory. */
typedef struct {
    const char    *dst;        /* "/EFI/BOOT/BOOTX64.EFI" */
    const char    *src;
    const uint8_t *data;
    uint64_t       len;        /* filled in from stat() for `src` */
} inst_file_t;

/* ── FAT32 (fat.c) ───────────────────────────────────────────────────────── */

/* Format the partition [start, start + nsect) (512-byte sectors) as FAT32
 * labelled `label` and write `files` into it, creating their directories. */
void fat32_build(uint64_t start, uint64_t nsect, const char *label,
                 inst_file_t *files, int nfiles);

/* ── ext2 (ext2.c) ───────────────────────────────────────────────────────── */

/* Walk `srcdir` and report how many 1 KiB blocks and inodes a copy needs. */
void ext2_scan(const char *srcdir, const char **skip, int nskip,
               uint64_t *blocks, uint32_t *inodes);
/* Make an ext2 filesystem (1 KiB blocks, sparse_super, filetype) on
 * [start, start + nsect) holding a copy of the tree ext2_scan() walked. */
void ext2_build(uint64_t start, uint64_t nsect, const char *label,
                const uint8_t uuid[16]);
