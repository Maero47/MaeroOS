/*
 * P26 unlink and truncate must give the space back.
 *
 * Linux: removing the last link to a file returns its data blocks to the
 * filesystem, and so does truncating a file shorter.  The space comes back only
 * once the last descriptor is closed, though: a file that is unlinked while
 * still open keeps its blocks until then, which is what makes the "create a
 * temp file, unlink it immediately, keep writing through the fd" idiom safe.
 *
 * MaeroOS: ext2_free_inode_blocks() walked the direct blocks and the singly
 * indirect chain and stopped there, although the driver reads all three
 * indirect levels and allocates two of them.  Everything a file held past
 * ~268 KiB (with 1 KiB blocks) was therefore lost on unlink: the block bitmap
 * bits stayed set with nothing referencing them, so the space could never be
 * reused.  Measured before the fix, one create-and-delete cycle of a 12 MiB
 * file cost 12 074 blocks permanently, and repeated cycles filled the disk.
 * ext2_truncate() leaked the same way when shrinking, and refused outright to
 * shrink a file larger than the singly-indirect range.  Separately, ext2_unlink
 * released the inode and its blocks even while a descriptor still held them,
 * so the allocator could hand a live file's blocks to somebody else.
 *
 * The probe uses whichever directory it can write to - /disk (the ext2 volume)
 * on MaeroOS, /tmp on the Linux reference host.  The file is sized past the
 * singly-indirect range of a 1 KiB-block ext2 so the doubly-indirect path is
 * the one being exercised on MaeroOS; on the host it is an ordinary file and
 * only the semantics are being checked.
 */
#define PROBE_NAME "p26_unlink_frees_space"
#include "probe.h"
#include <sys/vfs.h>

#define CYCLES     10
#define FILE_BYTES (2u * 1024u * 1024u)     /* 2 MiB: past 12 + 256 blocks */
#define CHUNK      4096u

static char dirbuf[128];
static char path[192];

/* Free blocks, normalised to 1 KiB units so the tolerance means the same thing
 * whatever the filesystem's block size is. */
static unsigned long free_kib(void)
{
    struct statfs sf;
    if (statfs(dirbuf, &sf) != 0)
        probe_fail("statfs(%s) failed: %s", dirbuf, strerror(errno));
    if (sf.f_bsize == 0)
        probe_fail("statfs(%s) reports f_bsize 0", dirbuf);
    return (unsigned long)sf.f_bfree * (unsigned long)(sf.f_bsize / 1024 ?
                                                       sf.f_bsize / 1024 : 1);
}

static int make_file(const char *p, unsigned bytes)
{
    static char buf[CHUNK];
    unsigned done = 0;
    int fd = open(p, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;
    memset(buf, 0xA5, sizeof(buf));
    while (done < bytes) {
        unsigned want = bytes - done < CHUNK ? bytes - done : CHUNK;
        ssize_t n = write(fd, buf, want);
        if (n <= 0) {
            close(fd);
            return -1;
        }
        done += (unsigned)n;
    }
    return fd;
}

static void pick_dir(void)
{
    static const char *cands[] = { "/disk", "/tmp", "." };
    for (unsigned i = 0; i < sizeof(cands) / sizeof(cands[0]); i++) {
        snprintf(path, sizeof(path), "%s/p26probe.tmp", cands[i]);
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

int main(void)
{
    probe_watchdog(240);
    pick_dir();
    snprintf(path, sizeof(path), "%s/p26probe.tmp", dirbuf);

    unsigned long file_kib = FILE_BYTES / 1024;
    /* Generous: the point is to catch a leak of a whole file per cycle, which
     * is 10x this, not to audit metadata block-for-block.  On the host /tmp is
     * shared with whatever else is running, so it has to absorb that too. */
    unsigned long tol = file_kib / 2;

    unsigned long start = free_kib();
    probe_info("dir=%s free=%lu KiB, file=%lu KiB, %d cycles, tolerance %lu KiB",
               dirbuf, start, file_kib, CYCLES, tol);

    /* 1. create / write / close / unlink, repeatedly.  A per-cycle leak shows
     *    up multiplied by CYCLES. */
    for (int i = 0; i < CYCLES; i++) {
        int fd = make_file(path, FILE_BYTES);
        if (fd < 0)
            probe_fail("cycle %d: cannot write %u bytes to %s: %s",
                       i, FILE_BYTES, path, strerror(errno));
        close(fd);
        if (unlink(path) != 0)
            probe_fail("cycle %d: unlink failed: %s", i, strerror(errno));
    }
    unsigned long after = free_kib();
    if (start > after + tol)
        probe_fail("%d create/unlink cycles of a %lu KiB file lost %lu KiB "
                   "(free %lu -> %lu KiB); unlink is not freeing the file's "
                   "blocks", CYCLES, file_kib, start - after, start, after);
    probe_info("after %d cycles free=%lu KiB (drift %ld KiB)",
               CYCLES, after, (long)after - (long)start);

    /* 2. truncating a file shorter must return the tail. */
    {
        int fd = make_file(path, FILE_BYTES);
        if (fd < 0)
            probe_fail("truncate case: cannot create the file: %s", strerror(errno));
        unsigned long full = free_kib();
        if (ftruncate(fd, 0) != 0)
            probe_fail("ftruncate(0) on a %lu KiB file failed: %s",
                       file_kib, strerror(errno));
        unsigned long shrunk = free_kib();
        if (shrunk + tol < full + file_kib - tol)
            probe_fail("ftruncate(0) of a %lu KiB file returned only %ld KiB "
                       "(free %lu -> %lu KiB)", file_kib,
                       (long)shrunk - (long)full, full, shrunk);
        close(fd);
        unlink(path);
    }

    /* 3. unlink while open: the name goes at once, the blocks do not, and the
     *    data stays readable through the descriptor. */
    {
        int fd = make_file(path, FILE_BYTES);
        if (fd < 0)
            probe_fail("open case: cannot create the file: %s", strerror(errno));
        unsigned long held = free_kib();
        if (unlink(path) != 0)
            probe_fail("open case: unlink failed: %s", strerror(errno));
        if (access(path, F_OK) == 0)
            probe_fail("the name still resolves after unlink");

        unsigned long while_open = free_kib();
        if (while_open > held + tol)
            probe_fail("blocks were released while a descriptor still held the "
                       "file (free %lu -> %lu KiB, file %lu KiB)",
                       held, while_open, file_kib);

        /* and it is still readable */
        char c = 0;
        if (lseek(fd, (off_t)FILE_BYTES - 1, SEEK_SET) < 0 ||
            read(fd, &c, 1) != 1 || (unsigned char)c != 0xA5)
            probe_fail("the unlinked file is not readable through its open fd");

        close(fd);
        unsigned long closed = free_kib();
        if (closed + tol < while_open + file_kib - tol)
            probe_fail("closing the last descriptor of an unlinked %lu KiB file "
                       "returned only %ld KiB (free %lu -> %lu KiB)",
                       file_kib, (long)closed - (long)while_open,
                       while_open, closed);
        probe_info("open-then-unlink: held %lu KiB, returned on close (%lu KiB)",
                   while_open, closed);
    }

    probe_pass();
}
