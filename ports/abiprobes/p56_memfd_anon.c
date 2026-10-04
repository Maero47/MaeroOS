/*
 * P56 memfd_create makes an anonymous file, not a name in /tmp.
 *
 * readlink(/proc/self/fd/N) of a memfd is "/memfd:<name> (deleted)", no
 * directory entry appears anywhere, and reopening /proc/self/fd/N gives the
 * same file (Firefox's shared-memory Freeze reopens it read-only and maps it
 * MAP_SHARED: both views must show the same bytes).
 *
 * MaeroOS before: a memfd was /tmp/.memfd-<counter>, created with O_CREAT|
 * O_TRUNC and a followed final symlink: a name another user could predict
 * and plant (a symlink, or a world-readable file) before root's next call.
 */
#define PROBE_NAME "p56_memfd_anon"
#include "probe.h"
#include <dirent.h>
#include <sys/mman.h>

int main(void)
{
    probe_watchdog(60);
    int fd = memfd_create("p56", 0);
    if (fd < 0) probe_fail("memfd_create: %s", strerror(errno));

    char link[64], target[256];
    snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
    ssize_t n = readlink(link, target, sizeof target - 1);
    if (n < 0) probe_fail("readlink %s: %s", link, strerror(errno));
    target[n] = 0;
    probe_info("memfd link: %s", target);
    if (strncmp(target, "/memfd:p56", 10) != 0)
        probe_fail("memfd is reachable by name: link is '%s'", target);

    DIR *d = opendir("/tmp");
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)))
            if (strncmp(e->d_name, ".memfd-", 7) == 0)
                probe_fail("/tmp/%s exists: memfds are named files", e->d_name);
        closedir(d);
    }

    if (ftruncate(fd, 4096) != 0) probe_fail("ftruncate: %s", strerror(errno));
    char *w = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (w == MAP_FAILED) probe_fail("mmap: %s", strerror(errno));
    memcpy(w, "p56-shared", 11);

    int ro = open(link, O_RDONLY);
    if (ro < 0) probe_fail("reopen %s: %s", link, strerror(errno));
    char *r = mmap(NULL, 4096, PROT_READ, MAP_SHARED, ro, 0);
    if (r == MAP_FAILED) probe_fail("mmap of the reopened memfd: %s", strerror(errno));
    if (memcmp(r, "p56-shared", 11) != 0)
        probe_fail("the reopened memfd does not share the original's pages");
    char buf[11];
    if (pread(ro, buf, sizeof buf, 0) != (ssize_t)sizeof buf || memcmp(buf, "p56-shared", 11))
        probe_fail("read() of the reopened memfd differs from the mapping");
    w[0] = 'P';
    if (r[0] != 'P') probe_fail("a store through one mapping is not seen in the other");
    probe_pass();
}
