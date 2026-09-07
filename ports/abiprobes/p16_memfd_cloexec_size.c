/*
 * P16 memfd cloexec and size - tests C5, M5.
 *
 * Linux: memfd_create(MFD_CLOEXEC) sets FD_CLOEXEC (mm/memfd.c);
 * posix_fallocate() extends the memfd to the requested size and every page
 * of a MAP_SHARED mapping can be touched; a second 64 MiB memfd works the
 * same way (shmem pages are ordinary page-cache pages); write()/pwrite()
 * to the memfd is visible through an existing MAP_SHARED mapping and
 * stores through the mapping are visible to pread() (shared page cache).
 *
 * MaeroOS (audit): memfd_create ignores MFD_CLOEXEC (proc/syscall.c:
 * 4884-4902, C5); memfd frames come from a per-node registry and a write(2)
 * after mapping is not reflected in the frames (:2741-2851, fs/tmpfs.c:
 * 77-103, M5); a second 64 MiB memfd must not halt the machine.
 *
 * argv[1] optionally overrides the size in MiB (default 64).
 */
#define PROBE_NAME "p16_memfd_cloexec_size"
#include "probe.h"
#include <sys/mman.h>
#include <sys/stat.h>

static void fill_and_verify(int fd, size_t len, unsigned seed, const char *what)
{
    double t0 = now_ms();
    uint32_t *m = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED)
        probe_fail("%s: mmap(%zu MiB, MAP_SHARED): %s", what, len >> 20, strerror(errno));
    size_t words = len / sizeof(uint32_t);
    for (size_t i = 0; i < words; i += 1024)
        m[i] = (uint32_t)(i ^ seed);
    for (size_t i = 0; i < words; i += 1024)
        if (m[i] != (uint32_t)(i ^ seed))
            probe_fail("%s: page %zu reads 0x%08x, expected 0x%08x", what, i / 1024, m[i],
                       (uint32_t)(i ^ seed));
    munmap(m, len);
    probe_info("%s: %zu MiB fallocated, mapped, touched and verified in %.0f ms",
               what, len >> 20, now_ms() - t0);
}

int main(int argc, char **argv)
{
    probe_watchdog(120);
    size_t mib = 64;
    if (argc > 1)
        mib = (size_t)strtoul(argv[1], NULL, 10);
    size_t len = mib << 20;
    struct stat st;

    int fd = memfd_create("p16", MFD_CLOEXEC);
    if (fd < 0)
        probe_fail("memfd_create(MFD_CLOEXEC): %s", strerror(errno));
    int fl = fcntl(fd, F_GETFD);
    if (fl < 0 || !(fl & FD_CLOEXEC))
        probe_fail("memfd_create(MFD_CLOEXEC) did not set FD_CLOEXEC (F_GETFD = %d)", fl);
    int fd2 = memfd_create("p16b", 0);
    if (fd2 < 0)
        probe_fail("memfd_create: %s", strerror(errno));
    fl = fcntl(fd2, F_GETFD);
    if (fl < 0 || (fl & FD_CLOEXEC))
        probe_fail("memfd_create without MFD_CLOEXEC set FD_CLOEXEC (F_GETFD = %d)", fl);
    probe_info("MFD_CLOEXEC honoured");

    int rc = posix_fallocate(fd, 0, (off_t)len);
    if (rc != 0)
        probe_fail("posix_fallocate(%zu MiB): %s", mib, strerror(rc));
    if (fstat(fd, &st) != 0 || (size_t)st.st_size != len)
        probe_fail("size after posix_fallocate is %lld, expected %zu", (long long)st.st_size, len);
    fill_and_verify(fd, len, 0xA5A5A5A5u, "memfd #1");

    rc = posix_fallocate(fd2, 0, (off_t)len);
    if (rc != 0)
        probe_fail("posix_fallocate(second memfd, %zu MiB): %s", mib, strerror(rc));
    fill_and_verify(fd2, len, 0x5A5A5A5Au, "memfd #2");

    /* M5: write(2) after mapping must be visible through the mapping and
     * stores through the mapping visible to pread(2). */
    unsigned char *m = mmap(NULL, 2 * 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED)
        probe_fail("mmap(8 KiB of memfd): %s", strerror(errno));
    unsigned char before = m[4096];
    (void)before;
    if (pwrite(fd, "XYZ", 3, 4096) != 3)
        probe_fail("pwrite(memfd): %s", strerror(errno));
    if (memcmp(m + 4096, "XYZ", 3) != 0)
        probe_fail("pwrite after mmap is not visible through the MAP_SHARED mapping "
                   "(mapping shows %02x %02x %02x)", m[4096], m[4097], m[4098]);
    memcpy(m + 100, "hello", 5);
    char rb[8] = { 0 };
    if (pread(fd, rb, 5, 100) != 5)
        probe_fail("pread(memfd): %s", strerror(errno));
    if (memcmp(rb, "hello", 5) != 0)
        probe_fail("store through the mapping is not visible to pread (got '%.5s')", rb);
    munmap(m, 2 * 4096);
    probe_info("write(2) and mapping stores are coherent");

    close(fd);
    close(fd2);
    probe_pass();
}
