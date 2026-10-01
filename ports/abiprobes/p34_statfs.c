/*
 * P34 statfs(2): the filesystem type of /proc, and errors for bad arguments.
 *
 * Linux: statfs("/proc") reports PROC_SUPER_MAGIC (0x9fa0); a missing path
 * is ENOENT and fstatfs on a closed descriptor EBADF.
 *
 * MaeroOS: every statfs answered EXT2_SUPER_MAGIC for any path, existing or
 * not, and fstatfs ignored the descriptor.  apk statfs()es <root>/proc and,
 * not seeing procfs, tries to mount it itself (syscall 21).
 */
#define PROBE_NAME "p34_statfs"
#include "probe.h"
#include <sys/vfs.h>

int main(void)
{
    probe_watchdog(60);
    struct statfs sf;
    if (statfs("/proc", &sf) != 0)
        probe_fail("statfs(/proc): %s", strerror(errno));
    if (sf.f_type != 0x9fa0)
        probe_fail("statfs(/proc).f_type = %#lx, want 0x9fa0", (unsigned long)sf.f_type);
    if (statfs("/p34-no-such-path", &sf) != -1 || errno != ENOENT)
        probe_fail("statfs(missing): %s, want ENOENT", strerror(errno));
    int fd = open("/", O_RDONLY);
    if (fd < 0 || fstatfs(fd, &sf) != 0)
        probe_fail("fstatfs(/): %s", strerror(errno));
    close(fd);
    if (fstatfs(fd, &sf) != -1 || errno != EBADF)
        probe_fail("fstatfs(closed fd): %s, want EBADF", strerror(errno));
    probe_pass();
}
