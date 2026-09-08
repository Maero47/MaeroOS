/*
 * p25_ptmx_lookup — a lookup of /dev/ptmx or /dev/pts/N must not consume or
 * mutate a pty.
 *
 * Review finding (fix round 1, .yonet/fix1.md): devfs allocated the master/
 * slave pair inside devdir_finddir(), which every path lookup reaches —
 * stat(), lstat(), access(), execve(), and open() itself before its permission
 * and descriptor checks.  The pair was only ever reclaimed from the close
 * paths (pty_master_close -> pty_maybe_free), so a lookup that never became an
 * open leaked the pair for the life of the boot.  With MAX_PTYS = 8, eight
 * stat("/dev/ptmx") calls from unprivileged code exhausted the table and every
 * later open("/dev/ptmx") failed.
 *
 * The same lookup also cleared the pair's slave_closed flag, so a bare
 * stat("/dev/pts/N") on a slave nobody had open stopped the master reporting
 * hangup, with no holder left to set the flag back.
 *
 * Linux behaviour this asserts (drivers/tty/pty.c):
 *   - /dev/ptmx is a cloning device: ptmx_open() allocates the pair, and it is
 *     reached only by open(2).  stat(2) goes through the devpts inode and
 *     allocates nothing, so any number of stats leave the pty count unchanged.
 *   - the slave's hangup state is a property of who has it open, not of who
 *     looked it up, so a stat() of the slave cannot change what the master's
 *     poll()/read() report.
 *
 * Both cases are checked without asserting a specific absolute hangup
 * behaviour for an unopened slave, because Linux and MaeroOS legitimately
 * differ there (Linux's master read blocks until the slave has been opened
 * once; MaeroOS reports EOF).  What is asserted is that the stat does not
 * *change* it — the same observation before and after — which is the actual
 * finding and is true on both.
 */
#define PROBE_NAME "p25_ptmx_lookup"
#include "probe.h"

#include <poll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <sys/wait.h>

#define STAT_ROUNDS 32

/* How the master looks right now: poll revents plus the result of a
 * non-blocking read.  Encoded so two observations can be compared exactly. */
struct master_state {
    int revents;
    int read_rc;
    int read_errno;
};

static void observe_master(int fd, struct master_state *out)
{
    struct pollfd pfd;
    char buf[64];

    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    out->revents = (poll(&pfd, 1, 0) < 0) ? -1 : pfd.revents;

    /* Only read when poll says there is something to collect.  An
     * unconditional read here hangs instead of failing on a kernel that does
     * not honour O_NONBLOCK on the pty master, which turns the case below into
     * a watchdog timeout rather than a verdict that says what went wrong. */
    if (out->revents & (POLLIN | POLLHUP | POLLERR)) {
        errno = 0;
        out->read_rc = (int)read(fd, buf, sizeof buf);
        out->read_errno = (out->read_rc < 0) ? errno : 0;
    } else {
        out->read_rc = -2;          /* not attempted */
        out->read_errno = 0;
    }
}

static int slave_number(int master)
{
    int n = -1;
    if (ioctl(master, TIOCGPTN, &n) < 0)
        return -1;
    return n;
}

/* Wait until fd has something to read, or the deadline passes. */
static int wait_readable(int fd, int ms)
{
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    return poll(&pfd, 1, ms) > 0 && (pfd.revents & POLLIN);
}

int main(void)
{
    probe_watchdog(60);

    /* ── A: stat("/dev/ptmx") must not consume a pty ─────────────────────── */
    struct stat st;

    if (stat("/dev/ptmx", &st) < 0)
        probe_skip("/dev/ptmx is not present (errno %d %s)",
                   errno, strerror(errno));

    for (int i = 0; i < STAT_ROUNDS; i++) {
        if (stat("/dev/ptmx", &st) < 0)
            probe_fail("A: stat(\"/dev/ptmx\") failed on round %d of %d "
                       "(errno %d %s) - the lookup is consuming ptys",
                       i + 1, STAT_ROUNDS, errno, strerror(errno));
        /* lstat and access reach the same lookup */
        if (lstat("/dev/ptmx", &st) < 0)
            probe_fail("A: lstat(\"/dev/ptmx\") failed on round %d (errno %d %s)",
                       i + 1, errno, strerror(errno));
        if (access("/dev/ptmx", R_OK | W_OK) < 0)
            probe_fail("A: access(\"/dev/ptmx\") failed on round %d (errno %d %s)",
                       i + 1, errno, strerror(errno));
    }
    probe_info("A: %d rounds of stat+lstat+access on /dev/ptmx all succeeded",
               STAT_ROUNDS);

    /* ── B: a pty is still available, and a full cycle works ─────────────── */
    int master = open("/dev/ptmx", O_RDWR | O_NONBLOCK);
    if (master < 0)
        probe_fail("B: open(\"/dev/ptmx\") failed after %d lookups "
                   "(errno %d %s) - the lookups leaked the pty table",
                   STAT_ROUNDS, errno, strerror(errno));

    int n = slave_number(master);
    if (n < 0)
        probe_fail("B: TIOCGPTN on the master failed (errno %d %s)",
                   errno, strerror(errno));
    probe_info("B: open(\"/dev/ptmx\") still works after the lookups, pts/%d", n);

    char spath[64];
    snprintf(spath, sizeof spath, "/dev/pts/%d", n);

    /* ── C: stat() of an unopened slave must not change the master ───────── */
    struct master_state before, after;
    observe_master(master, &before);

    if (stat(spath, &st) < 0)
        probe_fail("C: stat(\"%s\") on an unopened slave failed (errno %d %s)",
                   spath, errno, strerror(errno));
    if (access(spath, R_OK) < 0 && errno != EACCES)
        probe_fail("C: access(\"%s\") failed unexpectedly (errno %d %s)",
                   spath, errno, strerror(errno));

    observe_master(master, &after);

    if (before.revents != after.revents ||
        before.read_rc != after.read_rc ||
        before.read_errno != after.read_errno)
        probe_fail("C: stat(\"%s\") changed the master's state: "
                   "poll 0x%x->0x%x, read %d(errno %d)->%d(errno %d) - a lookup "
                   "must not clear the slave's hangup",
                   spath, before.revents, after.revents,
                   before.read_rc, before.read_errno,
                   after.read_rc, after.read_errno);
    probe_info("C: stat of the unopened %s left the master unchanged "
               "(poll 0x%x, read %d errno %d)",
               spath, before.revents, before.read_rc, before.read_errno);

    /* ── D: openpty-style round trip through the pair ────────────────────── */
    if (grantpt(master) < 0)
        probe_info("D: grantpt failed (errno %d %s), continuing",
                   errno, strerror(errno));
    if (unlockpt(master) < 0)
        probe_info("D: unlockpt failed (errno %d %s), continuing",
                   errno, strerror(errno));

    int slave = open(spath, O_RDWR | O_NOCTTY);
    if (slave < 0)
        probe_fail("D: open(\"%s\") failed (errno %d %s)",
                   spath, errno, strerror(errno));

    /* Raw mode on the slave so the kernel does not echo or line-buffer what we
     * write, which would make the byte counts below depend on termios. */
    struct termios tio;
    if (tcgetattr(slave, &tio) == 0) {
        tio.c_lflag &= (tcflag_t)~(ICANON | ECHO | ISIG);
        tio.c_iflag &= (tcflag_t)~(ICRNL | INLCR);
        tio.c_oflag &= (tcflag_t)~OPOST;
        tcsetattr(slave, TCSANOW, &tio);
    }

    static const char to_slave[] = "ping";
    if (write(master, to_slave, sizeof to_slave - 1) != (ssize_t)(sizeof to_slave - 1))
        probe_fail("D: write to the master failed (errno %d %s)",
                   errno, strerror(errno));
    if (!wait_readable(slave, 5000))
        probe_fail("D: the slave never became readable after a master write");
    char buf[64];
    ssize_t got = read(slave, buf, sizeof buf);
    if (got != (ssize_t)(sizeof to_slave - 1) || memcmp(buf, to_slave, (size_t)got) != 0)
        probe_fail("D: slave read got %zd bytes ('%.*s'), expected %zu ('%s')",
                   got, (int)(got > 0 ? got : 0), buf,
                   sizeof to_slave - 1, to_slave);

    static const char to_master[] = "pong";
    if (write(slave, to_master, sizeof to_master - 1) != (ssize_t)(sizeof to_master - 1))
        probe_fail("D: write to the slave failed (errno %d %s)",
                   errno, strerror(errno));
    if (!wait_readable(master, 5000))
        probe_fail("D: the master never became readable after a slave write");
    got = read(master, buf, sizeof buf);
    if (got != (ssize_t)(sizeof to_master - 1) || memcmp(buf, to_master, (size_t)got) != 0)
        probe_fail("D: master read got %zd bytes ('%.*s'), expected %zu ('%s')",
                   got, (int)(got > 0 ? got : 0), buf,
                   sizeof to_master - 1, to_master);
    probe_info("D: a full master<->slave round trip on pts/%d works", n);

    close(slave);
    close(master);

    /* ── E: the pair came back, and more lookups still cost nothing ──────── */
    for (int i = 0; i < STAT_ROUNDS; i++)
        if (stat("/dev/ptmx", &st) < 0)
            probe_fail("E: stat(\"/dev/ptmx\") failed on round %d after the "
                       "cycle (errno %d %s)", i + 1, errno, strerror(errno));

    /* Open more pairs than the table holds if lookups were consuming them:
     * MAX_PTYS is 8 on MaeroOS, so four simultaneous pairs interleaved with
     * lookups is a real test of both the leak and the reclaim. */
    int held[4];
    for (int i = 0; i < 4; i++) {
        for (int k = 0; k < 4; k++)
            if (stat("/dev/ptmx", &st) < 0)
                probe_fail("E: stat(\"/dev/ptmx\") failed while %d pairs were "
                           "open (errno %d %s)", i, errno, strerror(errno));
        held[i] = open("/dev/ptmx", O_RDWR | O_NONBLOCK);
        if (held[i] < 0)
            probe_fail("E: open(\"/dev/ptmx\") failed for pair %d of 4 "
                       "(errno %d %s) - lookups are still consuming ptys",
                       i + 1, errno, strerror(errno));
    }
    for (int i = 0; i < 4; i++)
        close(held[i]);

    /* Everything is closed again: one more open must succeed, proving the
     * pairs were reclaimed rather than merely not leaked by the lookups. */
    int again = open("/dev/ptmx", O_RDWR | O_NONBLOCK);
    if (again < 0)
        probe_fail("E: open(\"/dev/ptmx\") failed after closing four pairs "
                   "(errno %d %s) - closed pairs are not being reclaimed",
                   errno, strerror(errno));
    close(again);
    probe_info("E: four concurrent pairs, interleaved with %d more lookups, "
               "all opened and were reclaimed", 4 * 4 + STAT_ROUNDS);

    probe_pass();
}
