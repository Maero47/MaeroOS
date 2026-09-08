/*
 * P7 ftruncate64 - tests the missing syscalls 194 (ftruncate64),
 * 193 (truncate64), 297 (mknodat) and 40 (rmdir) from audit section 4.
 *
 * Linux: on i386 a 64-bit-offset ftruncate() is syscall 194 (glibc
 * sysdeps/unix/sysv/linux/ftruncate64.c; musl src/unistd/ftruncate.c uses
 * SYS_ftruncate64 whenever it exists), truncate() is 193, mkfifo()/mknod()
 * are mknodat(AT_FDCWD, ...) (glibc io/mknod.c, musl src/stat/mkfifo.c ->
 * mknod -> SYS_mknodat), rmdir() is 40.  All succeed on tmpfs and memfd.
 *
 * MaeroOS (audit): 194, 193, 297 and 40 are absent from syscall_dispatch
 * and return ENOSYS.
 */
#define PROBE_NAME "p07_ftruncate64"
#include "probe.h"
#include <sys/mman.h>
#include <sys/stat.h>

int main(void)
{
    probe_watchdog(60);
    char path[128];
    struct stat st;

    int fd = memfd_create("p07", 0);
    if (fd < 0)
        probe_fail("memfd_create: %s", strerror(errno));
    if (ftruncate(fd, 1 << 20) != 0)
        probe_fail("ftruncate(memfd, 1 MiB): %s", strerror(errno));
    if (fstat(fd, &st) != 0)
        probe_fail("fstat: %s", strerror(errno));
    if (st.st_size != (1 << 20))
        probe_fail("size after ftruncate is %lld, expected %d", (long long)st.st_size, 1 << 20);
    if (pwrite(fd, "abc", 3, 4096) != 3)
        probe_fail("pwrite: %s", strerror(errno));
    if (ftruncate(fd, 100) != 0)
        probe_fail("ftruncate(memfd, 100): %s", strerror(errno));
    off_t end = lseek(fd, 0, SEEK_END);
    if (end != 100)
        probe_fail("size after shrinking ftruncate is %lld, expected 100", (long long)end);
    close(fd);
    probe_info("ftruncate64 grows to 1 MiB and shrinks to 100 bytes");

    snprintf(path, sizeof path, "/tmp/p07.file.%d", (int)getpid());
    fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0600);
    if (fd < 0)
        probe_fail("open(%s): %s", path, strerror(errno));
    close(fd);
    if (truncate(path, 4096) != 0) {
        int e = errno;
        unlink(path);
        probe_fail("truncate(%s, 4096): %s", path, strerror(e));
    }
    if (stat(path, &st) != 0 || st.st_size != 4096) {
        unlink(path);
        probe_fail("size after truncate64 is %lld, expected 4096", (long long)st.st_size);
    }
    unlink(path);
    probe_info("truncate64 on a /tmp file works");

    snprintf(path, sizeof path, "/tmp/p07.fifo.%d", (int)getpid());
    unlink(path);
    if (mkfifo(path, 0600) != 0)
        probe_fail("mkfifo(%s): %s", path, strerror(errno));
    if (stat(path, &st) != 0) {
        unlink(path);
        probe_fail("stat(fifo): %s", strerror(errno));
    }
    if (!S_ISFIFO(st.st_mode)) {
        unlink(path);
        probe_fail("mkfifo created mode 0%o, not a FIFO", (unsigned)st.st_mode);
    }
    if (unlink(path) != 0)
        probe_fail("unlink(fifo): %s", strerror(errno));
    probe_info("mknodat creates a FIFO");

    snprintf(path, sizeof path, "/tmp/p07.dir.%d", (int)getpid());
    if (mkdir(path, 0700) != 0)
        probe_fail("mkdir(%s): %s", path, strerror(errno));
    if (rmdir(path) != 0)
        probe_fail("rmdir(%s): %s", path, strerror(errno));
    if (stat(path, &st) == 0 || errno != ENOENT)
        probe_fail("directory still exists after rmdir (errno %s)", strerror(errno));
    probe_info("rmdir removes a directory");

    probe_pass();
}
