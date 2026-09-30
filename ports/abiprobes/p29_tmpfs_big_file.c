/*
 * P29 a tmpfs file bigger than the kernel heap window, and its memory back.
 *
 * Linux: a tmpfs (shmem) file is page cache - its size is bounded by RAM and
 * the mount's size= limit, never by some kernel heap, and removing it gives
 * the pages back.  Holes read as zero, and bytes cut off by a shrinking
 * truncate read as zero after the file is extended again.
 *
 * MaeroOS: a tmpfs file body was one contiguous kmalloc() buffer that doubled
 * as the file grew, so every /tmp file lived in the 256 MiB kernel heap
 * window (0xD0000000-0xE0000000) and a growing file briefly needed its old
 * and its new buffer at once.  Firefox keeps its profile in /tmp; loading
 * one real web page filled the window ("[OOM] out of kernel heap address
 * space (0xe0000000)" hundreds of times per run) and from then on every
 * kmalloc in the kernel - socket buffers, pipes, exec - could fail.  Before
 * the fix this probe's write stopped with ENOMEM at 64 MiB (run after p28;
 * the next doubling, 128 MiB, no longer fit beside the old buffer in what
 * was left of the window), and the heap never shrinks, so the rest of the
 * boot was starved too.
 *
 * The file is FILE_MIB (> 256) MiB, each MiB stamped with its index so a
 * page mixed up with another shows, and it is created and unlinked three
 * times, which only fits a 1 GiB machine (smoke-abi's) if unlink gives the
 * pages back.  SKIPs when /proc/meminfo shows less than FILE_MIB + 100 MiB
 * free, since that is not what is being tested.
 */
#define PROBE_NAME "p29_tmpfs_big_file"
#include "probe.h"
#include <sys/stat.h>

#define FILE_MIB 288u
#define MIB      (1024u * 1024u)

static char buf[MIB];

/* MemFree in KiB from /proc/meminfo, or -1. */
static long mem_free_kib(void)
{
    char text[2048];
    int fd = open("/proc/meminfo", O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, text, sizeof(text) - 1);
    close(fd);
    if (n <= 0)
        return -1;
    text[n] = 0;
    char *p = strstr(text, "MemFree:");
    return p ? strtol(p + 8, NULL, 10) : -1;
}

static void stamp(unsigned mib)
{
    for (unsigned i = 0; i < MIB; i += 4) {
        uint32_t v = (mib << 20) ^ i ^ 0x5A5A0000u;
        memcpy(buf + i, &v, 4);
    }
}

static void check_holes_and_truncate(void)
{
    const char *path = "/tmp/p29_holes.tmp";
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        probe_fail("open %s: %s", path, strerror(errno));
    /* A byte at 3 MiB + 5 leaves a hole in front of it. */
    if (pwrite(fd, "X", 1, 3 * MIB + 5) != 1)
        probe_fail("pwrite past a hole: %s", strerror(errno));
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size != (off_t)(3 * MIB + 6))
        probe_fail("size after the hole write is %lld, want %u",
                   (long long)st.st_size, 3 * MIB + 6);
    char b[64];
    if (pread(fd, b, sizeof b, MIB + 100) != (ssize_t)sizeof b)
        probe_fail("pread in the hole: %s", strerror(errno));
    for (unsigned i = 0; i < sizeof b; i++)
        if (b[i])
            probe_fail("hole byte %u reads 0x%02x, want 0", i, (unsigned char)b[i]);

    /* Fill 10000 bytes, cut to 5000 (mid-page), grow to 12000: 5000..11999
     * must read back as zero, not as the bytes the truncate removed. */
    if (ftruncate(fd, 0) != 0)
        probe_fail("ftruncate 0: %s", strerror(errno));
    memset(buf, 'T', 10000);
    if (pwrite(fd, buf, 10000, 0) != 10000)
        probe_fail("pwrite 10000: %s", strerror(errno));
    if (ftruncate(fd, 5000) != 0 || ftruncate(fd, 12000) != 0)
        probe_fail("shrink/extend: %s", strerror(errno));
    if (pread(fd, buf, 12000, 0) != 12000)
        probe_fail("pread 12000: %s", strerror(errno));
    for (unsigned i = 0; i < 12000; i++) {
        char want = i < 5000 ? 'T' : 0;
        if (buf[i] != want)
            probe_fail("byte %u after shrink+extend is 0x%02x, want 0x%02x",
                       i, (unsigned char)buf[i], (unsigned char)want);
    }
    close(fd);
    unlink(path);
}

/* Write the whole file; `verify` stamps each MiB and reads a spread back. */
static void one_cycle(unsigned cycle, int verify)
{
    const char *path = "/tmp/p29_big.tmp";
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        probe_fail("cycle %u: open %s: %s", cycle, path, strerror(errno));
    for (unsigned m = 0; m < FILE_MIB; m++) {
        if (verify)
            stamp(m);
        ssize_t w = write(fd, buf, MIB);
        if (w != (ssize_t)MIB)
            probe_fail("cycle %u: write of MiB %u returned %zd (%s): the file "
                       "stopped at %u MiB", cycle, m, w,
                       w < 0 ? strerror(errno) : "short", m);
    }
    if (verify) {
        static char got[MIB];
        for (unsigned m = 0; m < FILE_MIB; m += 37) {
            unsigned mm = m + 37 >= FILE_MIB ? FILE_MIB - 1 : m;
            if (pread(fd, got, MIB, (off_t)mm * MIB) != (ssize_t)MIB)
                probe_fail("pread MiB %u: %s", mm, strerror(errno));
            stamp(mm);
            if (memcmp(got, buf, MIB) != 0)
                probe_fail("MiB %u reads back different bytes", mm);
        }
    }
    long with = mem_free_kib();
    close(fd);
    if (unlink(path) != 0)
        probe_fail("cycle %u: unlink: %s", cycle, strerror(errno));
    probe_info("cycle %u: %u MiB written%s, MemFree %ld KiB with it, %ld after unlink",
               cycle, FILE_MIB, verify ? " and read back" : "", with, mem_free_kib());
}

int main(void)
{
    probe_watchdog(240);
    check_holes_and_truncate();

    long before = mem_free_kib();
    if (before >= 0 && before < (long)(FILE_MIB + 100) * 1024)
        probe_skip("only %ld KiB free, the %u MiB file needs more", before, FILE_MIB);

    /* Three cycles are 864 MiB: more than a 1 GiB machine has free, so pages
     * an unlink failed to give back surface as ENOMEM in a later cycle. */
    one_cycle(1, 1);
    one_cycle(2, 0);
    one_cycle(3, 0);
    probe_pass();
}
