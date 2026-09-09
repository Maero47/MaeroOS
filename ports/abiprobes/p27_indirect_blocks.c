/*
 * P27 a file's bytes must come back unchanged from every indirect level, and
 * read() and mmap() must agree about them.
 *
 * Linux: what a file returns does not depend on how far into the file the byte
 * is, nor on whether it is reached through read() or through a mapping.  That
 * is unremarkable there, and exactly the property an ext2 driver has to earn:
 * a block index below 12 is in the inode, the next 256 (with 1 KiB blocks) are
 * behind one indirect block, the next 65 536 behind two, and the rest behind
 * three.  Each level is a different piece of arithmetic and a different block
 * to fetch, and getting one of them wrong returns somebody else's data or
 * zeroes rather than an error.
 *
 * MaeroOS: this exists because the indirect walk was rewritten.  It used to
 * read each whole indirect block into a kmalloc'd buffer through the block
 * cache and index the copy; it now reads the single 32-bit pointer it wants
 * straight out of the cache slot.  That is a change to the code that decides
 * which disk block holds a given file offset -- the one piece of the driver
 * whose failure mode is silent wrong data rather than an error, which is why
 * it is worth a probe rather than a benchmark.
 *
 * The file is 3 MiB, which with 1 KiB blocks puts its last bytes about
 * 2.7 MiB into the doubly-indirect range, so the direct, singly-indirect and
 * doubly-indirect paths are all exercised.  (The triply-indirect path needs a
 * file past ~64 MiB, too slow to write over PIO in a probe; Firefox's own
 * libxul.so is 175 MiB and exercises it on every startup.)  Every 4 KiB page
 * carries its own offset in every word, so a block resolved to the wrong place
 * is reported as the offset it actually came from rather than as a mismatch.
 *
 * On the Linux reference host this is an ordinary file and the probe is only
 * checking that the expectation itself is right.
 */
#define PROBE_NAME "p27_indirect_blocks"
#include "probe.h"
#include <sys/mman.h>

#define FILE_BYTES (3u * 1024u * 1024u)
#define CHUNK      4096u

static char dirbuf[128];
static char path[192];

static void pick_dir(void)
{
    static const char *cands[] = { "/disk", "/tmp", "." };
    for (unsigned i = 0; i < sizeof(cands) / sizeof(cands[0]); i++) {
        snprintf(path, sizeof(path), "%s/p27probe.tmp", cands[i]);
        int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            close(fd);
            unlink(path);
            snprintf(dirbuf, sizeof(dirbuf), "%s", cands[i]);
            return;
        }
    }
    probe_skip("no writable directory among /disk, /tmp, .");
}

/* Every 4-byte word of the page at `off` holds `off`, so a page that turns up
 * at the wrong offset names its true home. */
static void fill(uint32_t *page, uint32_t off)
{
    for (unsigned i = 0; i < CHUNK / 4; i++)
        page[i] = off;
}

static void check(const uint32_t *page, uint32_t off, const char *how)
{
    for (unsigned i = 0; i < CHUNK / 4; i++)
        if (page[i] != off)
            probe_fail("%s: word %u of the page at offset %u holds %u -- that "
                       "page belongs at offset %u, so the block map resolved "
                       "this offset to the wrong disk block",
                       how, i, off, page[i], page[i]);
}

int main(void)
{
    static uint32_t page[CHUNK / 4];
    probe_watchdog(240);
    pick_dir();
    snprintf(path, sizeof(path), "%s/p27probe.tmp", dirbuf);
    probe_info("dir=%s file=%u KiB", dirbuf, FILE_BYTES / 1024);

    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        probe_fail("cannot create %s: %s", path, strerror(errno));

    for (uint32_t off = 0; off < FILE_BYTES; off += CHUNK) {
        fill(page, off);
        ssize_t n = write(fd, page, CHUNK);
        if (n != (ssize_t)CHUNK) {
            unlink(path);
            probe_fail("write at offset %u returned %ld: %s",
                       off, (long)n, strerror(errno));
        }
    }
    close(fd);

    /* 1. sequential read() through a fresh descriptor. */
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        unlink(path);
        probe_fail("cannot reopen %s: %s", path, strerror(errno));
    }
    for (uint32_t off = 0; off < FILE_BYTES; off += CHUNK) {
        ssize_t n = read(fd, page, CHUNK);
        if (n != (ssize_t)CHUNK) {
            close(fd);
            unlink(path);
            probe_fail("read at offset %u returned %ld: %s",
                       off, (long)n, strerror(errno));
        }
        check(page, off, "read");
    }

    /* 2. the same file backwards through pread(), so the block map is asked
     *    for offsets in an order no readahead can have prepared. */
    for (uint32_t off = FILE_BYTES; off > 0; off -= CHUNK) {
        uint32_t at = off - CHUNK;
        ssize_t n = pread(fd, page, CHUNK, (off_t)at);
        if (n != (ssize_t)CHUNK) {
            close(fd);
            unlink(path);
            probe_fail("pread at offset %u returned %ld: %s",
                       at, (long)n, strerror(errno));
        }
        check(page, at, "pread");
    }

    /* 3. the same bytes through a private mapping, which reaches them by page
     *    fault rather than by read(). */
    void *m = mmap(NULL, FILE_BYTES, PROT_READ, MAP_PRIVATE, fd, 0);
    if (m == MAP_FAILED) {
        close(fd);
        unlink(path);
        probe_fail("mmap of %u bytes failed: %s", FILE_BYTES, strerror(errno));
    }
    for (uint32_t off = 0; off < FILE_BYTES; off += CHUNK)
        check((const uint32_t *)((const char *)m + off), off, "mmap");

    munmap(m, FILE_BYTES);
    close(fd);
    if (unlink(path) != 0)
        probe_fail("unlink failed: %s", strerror(errno));

    probe_info("%u KiB verified through read, pread and mmap",
               FILE_BYTES / 1024);
    probe_pass();
}
