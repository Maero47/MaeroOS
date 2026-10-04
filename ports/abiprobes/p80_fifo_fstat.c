/*
 * P80 fstat of an open named FIFO is the FIFO's inode.
 *
 * Linux: fstat on a descriptor of a named FIFO reports the node's owner,
 * mode, st_ino and st_dev, the same as stat() of its name; only pipe(2) ends
 * are pipefs inodes.
 *
 * MaeroOS before: every pipe descriptor reported uid 0, gid 0, mode 0666 and
 * st_ino 0, so a program checking who owns the FIFO it opened (libwm) could
 * not trust fstat.
 */
#define PROBE_NAME "p80_fifo_fstat"
#include "probe.h"
#include <sys/stat.h>

int main(void)
{
    probe_watchdog(60);
    char path[64];
    snprintf(path, sizeof path, "/tmp/p80-fifo-%d", (int)getpid());
    unlink(path);
    if (mkfifo(path, 0640) != 0) probe_fail("mkfifo: %s", strerror(errno));
    if (getuid() == 0 && chown(path, 1234, 5678) != 0)
        probe_fail("chown: %s", strerror(errno));

    struct stat ns, fs;
    if (stat(path, &ns) != 0) probe_fail("stat: %s", strerror(errno));
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) probe_fail("open: %s", strerror(errno));
    if (fstat(fd, &fs) != 0) probe_fail("fstat: %s", strerror(errno));
    unlink(path);
    probe_info("stat: ino %lu dev %lu mode %o uid %u gid %u",
               (unsigned long)ns.st_ino, (unsigned long)ns.st_dev,
               (unsigned)ns.st_mode, (unsigned)ns.st_uid, (unsigned)ns.st_gid);
    probe_info("fstat: ino %lu dev %lu mode %o uid %u gid %u",
               (unsigned long)fs.st_ino, (unsigned long)fs.st_dev,
               (unsigned)fs.st_mode, (unsigned)fs.st_uid, (unsigned)fs.st_gid);
    if (!S_ISFIFO(fs.st_mode)) probe_fail("fstat mode %o is not a FIFO", (unsigned)fs.st_mode);
    if ((fs.st_mode & 07777) != 0640)
        probe_fail("fstat permissions %o, the FIFO's are 640", (unsigned)(fs.st_mode & 07777));
    if (fs.st_uid != ns.st_uid || fs.st_gid != ns.st_gid)
        probe_fail("fstat owner %u:%u, the FIFO's is %u:%u", (unsigned)fs.st_uid,
                   (unsigned)fs.st_gid, (unsigned)ns.st_uid, (unsigned)ns.st_gid);
    if (fs.st_ino != ns.st_ino || fs.st_dev != ns.st_dev)
        probe_fail("fstat ino/dev %lu/%lu, the FIFO's are %lu/%lu",
                   (unsigned long)fs.st_ino, (unsigned long)fs.st_dev,
                   (unsigned long)ns.st_ino, (unsigned long)ns.st_dev);

    int p[2];
    if (pipe(p) != 0) probe_fail("pipe: %s", strerror(errno));
    if (fstat(p[0], &fs) != 0) probe_fail("fstat pipe: %s", strerror(errno));
    if (!S_ISFIFO(fs.st_mode)) probe_fail("a pipe(2) end is not S_IFIFO (%o)", (unsigned)fs.st_mode);
    probe_pass();
}
