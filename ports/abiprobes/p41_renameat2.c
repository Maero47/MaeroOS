/*
 * P41 renameat2(2) with flags 0 and RENAME_NOREPLACE.
 *
 * Linux: flags 0 is renameat; RENAME_NOREPLACE fails with EEXIST when the
 * target exists and leaves both names alone, and renames otherwise.
 *
 * MaeroOS: renameat2 (353) was missing; busybox 1.37 in Alpine 3.24 calls
 * it (udhcpc's script moving resolv.conf into place logged
 * "[SYSCALL] unimplemented 353").
 */
#define PROBE_NAME "p41_renameat2"
#include "probe.h"
#include <sys/stat.h>

#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE 1
#endif

static void touch(const char *p, const char *text)
{
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0 || write(fd, text, strlen(text)) != (ssize_t)strlen(text))
        probe_fail("create %s: %s", p, strerror(errno));
    close(fd);
}

static int rn2(const char *a, const char *b, unsigned flags)
{
    return (int)syscall(SYS_renameat2, AT_FDCWD, a, AT_FDCWD, b, flags);
}

int main(int argc, char **argv)
{
    probe_watchdog(60);
    const char *dir = argc > 1 ? argv[1] : "/tmp";
    char a[256], b[256], c[256];
    snprintf(a, sizeof a, "%s/p41_a.%d", dir, (int)getpid());
    snprintf(b, sizeof b, "%s/p41_b.%d", dir, (int)getpid());
    snprintf(c, sizeof c, "%s/p41_c.%d", dir, (int)getpid());
    touch(a, "a");
    touch(b, "b");
    if (rn2(a, b, RENAME_NOREPLACE) == 0 || errno != EEXIST)
        probe_fail("RENAME_NOREPLACE onto an existing name: want EEXIST, got %s",
                   strerror(errno));
    struct stat st;
    if (stat(a, &st) != 0 || stat(b, &st) != 0 || st.st_size != 1)
        probe_fail("a refused RENAME_NOREPLACE changed the names");
    if (rn2(a, c, RENAME_NOREPLACE) != 0)
        probe_fail("RENAME_NOREPLACE to a free name: %s", strerror(errno));
    if (stat(a, &st) == 0 || stat(c, &st) != 0)
        probe_fail("RENAME_NOREPLACE did not move the file");
    if (rn2(c, b, 0) != 0)
        probe_fail("renameat2 with flags 0 over an existing name: %s", strerror(errno));
    if (stat(c, &st) == 0 || stat(b, &st) != 0)
        probe_fail("flags 0 did not replace the target");
    if (rn2(b, c, 0x80) == 0 || errno != EINVAL)
        probe_fail("an unknown flag: want EINVAL");
    unlink(b);
    probe_pass();
}
