/*
 * P38 sync(2) and syncfs(2).
 *
 * Linux: sync() always succeeds; syncfs(fd) returns 0 for an open
 * descriptor and EBADF for a closed one.
 *
 * MaeroOS: both (36, 344) were missing (ENOSYS).  apk-tools 3 calls sync()
 * after committing an installation.  ext2 writes through its block cache,
 * so there is nothing to flush.
 */
#define PROBE_NAME "p38_sync"
#include "probe.h"

int main(void)
{
    probe_watchdog(60);
    errno = 0;
    sync();
    if (syscall(SYS_sync) != 0)
        probe_fail("sync: %s", strerror(errno));
    int fd = open("/", O_RDONLY);
    if (fd < 0)
        probe_fail("open /: %s", strerror(errno));
    if (syncfs(fd) != 0)
        probe_fail("syncfs(/): %s", strerror(errno));
    close(fd);
    if (syncfs(fd) != -1 || errno != EBADF)
        probe_fail("syncfs(closed fd): %s, want EBADF", strerror(errno));
    probe_pass();
}
