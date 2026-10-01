/*
 * P35 extended attributes: an unsupported attribute is an error a caller
 * understands, not ENOSYS.
 *
 * Linux: getxattr/lgetxattr/fgetxattr of an attribute a file does not have
 * is ENODATA, or EOPNOTSUPP on a filesystem without xattrs; a missing path
 * is ENOENT and a closed descriptor EBADF.
 *
 * MaeroOS: syscalls 226-237 were missing.  GNU ls (coreutils, built with
 * libacl) calls lgetxattr on every file of `ls -l` and logged
 * "[SYSCALL] unimplemented 230" for each one.
 */
#define PROBE_NAME "p35_xattr"
#include "probe.h"
#include <sys/xattr.h>

static int unsupported(int e)
{
    return e == ENODATA || e == EOPNOTSUPP;
}

int main(void)
{
    probe_watchdog(60);
    char buf[64];
    const char *path = "/tmp/p35.file";
    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0)
        probe_fail("create %s: %s", path, strerror(errno));
    if (getxattr(path, "user.p35", buf, sizeof buf) != -1 || !unsupported(errno))
        probe_fail("getxattr: %s, want ENODATA or EOPNOTSUPP", strerror(errno));
    if (lgetxattr(path, "user.p35", buf, sizeof buf) != -1 || !unsupported(errno))
        probe_fail("lgetxattr: %s, want ENODATA or EOPNOTSUPP", strerror(errno));
    if (fgetxattr(fd, "user.p35", buf, sizeof buf) != -1 || !unsupported(errno))
        probe_fail("fgetxattr: %s, want ENODATA or EOPNOTSUPP", strerror(errno));
    if (llistxattr(path, buf, sizeof buf) < 0 && errno != EOPNOTSUPP)
        probe_fail("llistxattr: %s", strerror(errno));
    if (lgetxattr("/p35-no-such-path", "user.p35", buf, sizeof buf) != -1 ||
        errno != ENOENT)
        probe_fail("lgetxattr(missing): %s, want ENOENT", strerror(errno));
    close(fd);
    if (fgetxattr(fd, "user.p35", buf, sizeof buf) != -1 || errno != EBADF)
        probe_fail("fgetxattr(closed fd): %s, want EBADF", strerror(errno));
    unlink(path);
    probe_pass();
}
