/*
 * P33 isatty() on pipes and files: terminal ioctls fail with ENOTTY.
 *
 * Linux: TIOCGWINSZ and TCGETS (musl's isatty() is TIOCGWINSZ) succeed only
 * on a terminal; on a pipe or a regular file they fail with ENOTTY.
 *
 * MaeroOS: both answered for every descriptor with the console's settings,
 * so every pipe was a tty: Alpine's `less` refused piped input ("Missing
 * filename") and git started a pager writing into a pipe.
 */
#define PROBE_NAME "p33_isatty"
#include "probe.h"
#include <sys/ioctl.h>
#include <termios.h>

static void not_tty(int fd, const char *what)
{
    struct winsize ws;
    struct termios t;
    if (isatty(fd))
        probe_fail("isatty(%s) is true", what);
    if (ioctl(fd, TIOCGWINSZ, &ws) != -1 || errno != ENOTTY)
        probe_fail("TIOCGWINSZ on %s: %s, want ENOTTY", what, strerror(errno));
    if (tcgetattr(fd, &t) != -1 || errno != ENOTTY)
        probe_fail("TCGETS on %s: %s, want ENOTTY", what, strerror(errno));
    if (tcsetattr(fd, TCSANOW, &t) != -1 || errno != ENOTTY)
        probe_fail("TCSETS on %s: %s, want ENOTTY", what, strerror(errno));
}

int main(void)
{
    probe_watchdog(60);
    int p[2];
    if (pipe(p) != 0)
        probe_fail("pipe: %s", strerror(errno));
    not_tty(p[0], "a pipe's read end");
    not_tty(p[1], "a pipe's write end");
    int fd = open("/tmp/p33.file", O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0)
        probe_fail("create /tmp/p33.file: %s", strerror(errno));
    not_tty(fd, "a regular file");
    close(fd);
    unlink("/tmp/p33.file");
    probe_pass();
}
