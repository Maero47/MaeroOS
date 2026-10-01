/*
 * P39 splice(2) between pipes: either it moves the bytes or it says EINVAL.
 *
 * Linux: splice(pipe_r, NULL, pipe2_w, NULL, n, 0) moves the bytes.  A
 * kernel or filesystem that cannot splice a given pair answers EINVAL, on
 * which callers fall back to read/write.
 *
 * MaeroOS: splice (313) was missing; coreutils 9.8+ `cat` (Alpine 3.24)
 * splices whenever a pipe is involved, and every `... | cat` logged
 * "[SYSCALL] unimplemented 313".  MaeroOS answers EINVAL; the bytes must
 * still be in the source pipe afterwards.
 */
#define PROBE_NAME "p39_splice"
#include "probe.h"

int main(void)
{
    probe_watchdog(60);
    int a[2], b[2];
    if (pipe(a) != 0 || pipe(b) != 0)
        probe_fail("pipe: %s", strerror(errno));
    if (write(a[1], "spliced", 7) != 7)
        probe_fail("write: %s", strerror(errno));
    ssize_t n = splice(a[0], NULL, b[1], NULL, 64, 0);
    char buf[16] = {0};
    if (n == 7) {
        if (read(b[0], buf, sizeof buf - 1) != 7 || strcmp(buf, "spliced") != 0)
            probe_fail("splice moved 7 bytes but the target pipe holds \"%s\"", buf);
        probe_info("splice is implemented");
    } else if (n == -1 && errno == EINVAL) {
        if (read(a[0], buf, sizeof buf - 1) != 7 || strcmp(buf, "spliced") != 0)
            probe_fail("splice failed with EINVAL but consumed the source");
        probe_info("splice answers EINVAL (callers fall back to read/write)");
    } else {
        probe_fail("splice = %zd (%s), want 7 or EINVAL", n, strerror(errno));
    }
    probe_pass();
}
