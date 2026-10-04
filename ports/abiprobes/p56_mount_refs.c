/*
 * P56 mounts keep the directories they use.
 *
 * Linux: a mount holds references on its mountpoint and on the directory it
 * shows.  The source directory of a bind mount can be removed (the mount
 * keeps showing it, empty); a mountpoint can be neither removed nor renamed
 * nor replaced by a rename (EBUSY); umount still works afterwards.
 *
 * MaeroOS: the mount table kept raw node pointers.  rmdir of a tmpfs bind
 * source freed the node the mount still showed, so the mount pointed into
 * freed (and then reused) kernel memory; renaming over an empty mountpoint
 * freed the mountpoint the same way.
 *
 * Runs as root on /tmp (tmpfs).
 */
#define PROBE_NAME "p56_mount_refs"
#include "probe.h"
#include <dirent.h>
#include <sys/mount.h>
#include <sys/stat.h>

#define NFILL 300

static int count_entries(const char *dir, char *first, size_t n)
{
    DIR *d = opendir(dir);
    if (!d) return -1;
    int c = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (!c) snprintf(first, n, "%s", e->d_name);
        c++;
    }
    closedir(d);
    return c;
}

int main(void)
{
    probe_watchdog(60);
    if (geteuid() != 0) probe_skip("needs root");
    int pid = (int)getpid();
    char src[96], dst[96], mk[128], other[96], path[160], first[256] = "";
    snprintf(src, sizeof src, "/tmp/p56s.%d", pid);
    snprintf(dst, sizeof dst, "/tmp/p56d.%d", pid);
    snprintf(other, sizeof other, "/tmp/p56o.%d", pid);
    snprintf(mk, sizeof mk, "%s/marker", src);

    if (mkdir(src, 0755) || mkdir(dst, 0755) || mkdir(other, 0755))
        probe_fail("mkdir: %s", strerror(errno));
    int fd = open(mk, O_CREAT | O_WRONLY, 0644);
    if (fd < 0) probe_fail("create %s: %s", mk, strerror(errno));
    close(fd);
    if (mount(src, dst, NULL, MS_BIND, NULL) != 0) {
        if (errno == EPERM) probe_skip("mount: EPERM");
        probe_fail("bind %s on %s: %s", src, dst, strerror(errno));
    }

    /* The mountpoint stays where it is. */
    if (rmdir(dst) == 0 || errno != EBUSY)
        probe_fail("rmdir of a mountpoint: %s, want EBUSY",
                   errno ? strerror(errno) : "removed");
    errno = 0;
    if (rename(other, dst) == 0 || errno != EBUSY)
        probe_fail("rename over a mountpoint: %s, want EBUSY",
                   errno ? strerror(errno) : "renamed");
    errno = 0;
    snprintf(path, sizeof path, "%s.moved", dst);
    if (rename(dst, path) == 0 || errno != EBUSY)
        probe_fail("rename of a mountpoint: %s, want EBUSY",
                   errno ? strerror(errno) : "renamed");

    /* The bind source goes away; the mount keeps (an empty) it. */
    if (unlink(mk) != 0 || rmdir(src) != 0)
        probe_fail("remove the bind source: %s", strerror(errno));
    for (int i = 0; i < NFILL; i++) {
        snprintf(path, sizeof path, "/tmp/p56f.%d.%d", pid, i);
        if (mkdir(path, 0755) != 0) probe_fail("mkdir %s: %s", path, strerror(errno));
        snprintf(path, sizeof path, "/tmp/p56f.%d.%d/inside", pid, i);
        fd = open(path, O_CREAT | O_WRONLY, 0644);
        if (fd >= 0) close(fd);
    }
    struct stat st;
    if (stat(dst, &st) != 0 || !S_ISDIR(st.st_mode))
        probe_fail("stat of the bind mount after its source went: %s, mode %o",
                   strerror(errno), (unsigned)st.st_mode);
    int n = count_entries(dst, first, sizeof first);
    if (n != 0)
        probe_fail("the bind mount of a removed directory lists %d entries "
                   "(first \"%s\"): it shows freed and reused memory", n, first);

    for (int i = 0; i < NFILL; i++) {
        snprintf(path, sizeof path, "/tmp/p56f.%d.%d/inside", pid, i);
        unlink(path);
        snprintf(path, sizeof path, "/tmp/p56f.%d.%d", pid, i);
        rmdir(path);
    }
    if (umount(dst) != 0) probe_fail("umount %s: %s", dst, strerror(errno));
    if (rmdir(dst) != 0) probe_fail("rmdir %s after umount: %s", dst, strerror(errno));
    rmdir(other);
    probe_pass();
}
