/*
 * P57 Only /dev/fb0 itself maps the framebuffer.
 *
 * A regular file that happens to be called fb0 maps its own bytes.  On
 * MaeroOS, /dev/fb0 opened read-only cannot be mapped writable, even
 * MAP_PRIVATE: its pages are the real screen whatever the flags say.
 *
 * MaeroOS before: mmap matched the node NAME "fb0", so /tmp/fb0 mapped the
 * physical framebuffer (bypassing /dev/fb0's mode), and a MAP_PRIVATE|
 * PROT_WRITE view of a read-only /dev/fb0 descriptor wrote the screen.
 */
#define PROBE_NAME "p57_fb0_identity"
#include "probe.h"
#include <sys/mman.h>
#include <sys/utsname.h>

int main(void)
{
    probe_watchdog(60);
    const char *path = "/tmp/fb0";
    unlink(path);
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) probe_fail("create %s: %s", path, strerror(errno));
    char page[4096];
    for (unsigned i = 0; i < sizeof page; i++) page[i] = (char)(i * 7 + 3);
    if (write(fd, page, sizeof page) != (ssize_t)sizeof page) probe_fail("write: %s", strerror(errno));
    char *m = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) probe_fail("mmap %s: %s", path, strerror(errno));
    if (memcmp(m, page, sizeof page) != 0)
        probe_fail("mapping a regular file named fb0 does not show its contents");
    munmap(m, 4096);
    close(fd);
    unlink(path);

    struct utsname u;
    if (uname(&u) == 0 && strstr(u.release, "maeros")) {
        int ro = open("/dev/fb0", O_RDONLY);
        if (ro < 0) {
            probe_info("/dev/fb0: %s (writable-view check skipped)", strerror(errno));
        } else {
            void *w = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE, ro, 0);
            if (w != MAP_FAILED)
                probe_fail("a read-only /dev/fb0 descriptor gave a writable mapping");
            if (errno != EACCES)
                probe_fail("writable view of read-only /dev/fb0: %s, want EACCES", strerror(errno));
            close(ro);
        }
    }
    probe_pass();
}
