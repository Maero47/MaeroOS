/*
 * P21 unlink-while-open - a file that is unlinked while a descriptor is still
 * open must keep working through that descriptor.
 *
 * Linux: unlink() removes the NAME.  The inode survives until the last
 * descriptor (and the last mapping) referring to it is gone, so reads, writes,
 * lseek and fstat through an already-open fd behave exactly as before, two
 * descriptors opened before the unlink still share one inode, and the path
 * itself is immediately gone (ENOENT).  Every program that writes a temporary
 * file and unlinks it straight away depends on this; so does the usual
 * "write X.tmp, rename over X" pattern when something still has X open.
 *
 * MaeroOS: tmpfs_unlink() used to kfree() the node unconditionally, so the
 * descriptor was left pointing at freed memory.  Reads returned recycled heap
 * content and the eventual close() called node->close_fn through whatever had
 * landed in the freed block - an intermittent kernel panic during process
 * teardown (jumping to 0x74206e69, the ASCII "in t" of some other file's data).
 *
 * /tmp is the tmpfs mount on MaeroOS and a tmpfs on the Linux reference host,
 * so the same binary exercises the same kind of filesystem on both.
 */
#define PROBE_NAME "p21_unlink_open"
#include "probe.h"
#include <sys/stat.h>

static const char msg[] = "hello unlinked world";

/*
 * Force the kernel to recycle whatever the unlink released.  A use-after-free
 * is invisible while the freed block still happens to hold its old contents,
 * so the probe would pass on a broken kernel unless it makes the allocator
 * hand that block out again first.  Creating tmpfs files is the exact same
 * allocation size class as the node that unlink() frees, and each new node is
 * zeroed, so a stale node reused this way reports size 0 and a NULL data
 * pointer - which the reads below then catch.  The descriptors stay open so
 * the churn nodes cannot themselves be recycled.
 */
#define CHURN 200
#define BIG   (64 * 1024)      /* big enough that the kernel heap has few free
                                * blocks this large, so a fresh request of the
                                * same size lands on the one unlink just
                                * released */
static int churn_fd[CHURN];
static char churn_path[CHURN][160];
static char pattern_a[BIG], pattern_b[BIG], readback[BIG];

static void churn_alloc(const char *tag)
{
    for (int i = 0; i < CHURN; i++) {
        snprintf(churn_path[i], sizeof churn_path[i],
                 "/tmp/p21.%s.%d.%d", tag, (int)getpid(), i);
        unlink(churn_path[i]);
        int f = open(churn_path[i], O_CREAT | O_RDWR | O_TRUNC, 0600);
        if (f < 0)
            continue;
        /* Same size class as the node unlink() frees, and (for the first one)
         * as its data buffer: whichever the allocator hands back, the recycled
         * bytes are pattern_b or a freshly zeroed node, and the checks below
         * notice.  The descriptor is closed immediately but the file stays
         * linked, so the node keeps its allocation without costing an fd. */
        (void)!write(f, pattern_b, i == 0 ? BIG : 26);
        close(f);
        churn_fd[i] = -1;
    }
}

static void churn_free(void)
{
    for (int i = 0; i < CHURN; i++) {
        if (churn_fd[i] >= 0) { close(churn_fd[i]); churn_fd[i] = -1; }
        unlink(churn_path[i]);
    }
}

int main(void)
{
    probe_watchdog(60);
    char path[128], buf[128];
    struct stat st;

    for (int i = 0; i < BIG; i++) {
        pattern_a[i] = (char)('A' + (i % 23));
        pattern_b[i] = (char)('a' + (i % 19));
    }

    snprintf(path, sizeof path, "/tmp/p21.open.%d", (int)getpid());
    unlink(path);

    /* ── U1: read back through the fd after the name is gone ────────────── */
    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0600);
    if (fd < 0)
        probe_fail("open(%s): %s", path, strerror(errno));
    if (write(fd, msg, sizeof msg - 1) != (ssize_t)(sizeof msg - 1))
        probe_fail("write: %s", strerror(errno));
    if (write(fd, pattern_a, BIG) != (ssize_t)BIG)
        probe_fail("write of %d bytes: %s", BIG, strerror(errno));

    if (unlink(path) != 0)
        probe_fail("unlink(%s): %s", path, strerror(errno));

    churn_alloc("churn");   /* make the allocator reuse anything unlink freed */

    /* The name must be gone immediately. */
    if (open(path, O_RDONLY) >= 0)
        probe_fail("the path still resolves after unlink");
    if (errno != ENOENT)
        probe_fail("open after unlink gave %s, expected ENOENT", strerror(errno));

    memset(buf, 0, sizeof buf);
    if (lseek(fd, 0, SEEK_SET) != 0)
        probe_fail("lseek after unlink: %s", strerror(errno));
    ssize_t got = read(fd, buf, sizeof msg - 1);
    if (got != (ssize_t)(sizeof msg - 1))
        probe_fail("read after unlink returned %ld, expected %ld",
                   (long)got, (long)(sizeof msg - 1));
    if (memcmp(buf, msg, sizeof msg - 1) != 0)
        probe_fail("read after unlink returned %.20s, expected %.20s", buf, msg);
    got = read(fd, readback, BIG);
    if (got != (ssize_t)BIG)
        probe_fail("bulk read after unlink returned %ld, expected %d",
                   (long)got, BIG);
    for (int i = 0; i < BIG; i++)
        if (readback[i] != pattern_a[i])
            probe_fail("byte %d of the unlinked file is 0x%02x, expected 0x%02x "
                       "(the node or its data was recycled)",
                       i, (unsigned char)readback[i],
                       (unsigned char)pattern_a[i]);
    probe_info("%d bytes survive unlink and read back through the open fd",
               BIG + (int)sizeof msg - 1);

    /* ── U2: the fd is still fully writable ─────────────────────────────── */
    if (lseek(fd, 0, SEEK_END) != (off_t)(sizeof msg - 1 + BIG))
        probe_fail("lseek(SEEK_END) after unlink: %s", strerror(errno));
    if (write(fd, "!", 1) != 1)
        probe_fail("write after unlink: %s", strerror(errno));
    if (fstat(fd, &st) != 0)
        probe_fail("fstat after unlink: %s", strerror(errno));
    if (st.st_size != (off_t)(sizeof msg + BIG))
        probe_fail("fstat size after unlink is %lld, expected %lld",
                   (long long)st.st_size, (long long)(sizeof msg + BIG));
    probe_info("writes and fstat still work on the unlinked file (size %lld)",
               (long long)st.st_size);

    if (close(fd) != 0)
        probe_fail("close of the unlinked fd: %s", strerror(errno));
    churn_free();
    probe_info("closing the last fd of an unlinked file is clean");

    /* ── U3: two descriptors opened before the unlink share one inode ───── */
    snprintf(path, sizeof path, "/tmp/p21.share.%d", (int)getpid());
    unlink(path);
    int a = open(path, O_CREAT | O_RDWR | O_TRUNC, 0600);
    if (a < 0)
        probe_fail("open(a): %s", strerror(errno));
    int b = open(path, O_RDONLY);
    if (b < 0) {
        close(a); unlink(path);
        probe_fail("second open: %s", strerror(errno));
    }
    if (unlink(path) != 0) {
        close(a); close(b);
        probe_fail("unlink(%s): %s", path, strerror(errno));
    }
    churn_alloc("churn2");
    if (write(a, msg, sizeof msg - 1) != (ssize_t)(sizeof msg - 1)) {
        close(a); close(b);
        probe_fail("write through fd a after unlink: %s", strerror(errno));
    }
    memset(buf, 0, sizeof buf);
    got = read(b, buf, sizeof buf - 1);
    if (got != (ssize_t)(sizeof msg - 1)) {
        close(a); close(b);
        probe_fail("read through fd b returned %ld, expected %ld",
                   (long)got, (long)(sizeof msg - 1));
    }
    if (memcmp(buf, msg, sizeof msg - 1) != 0) {
        close(a); close(b);
        probe_fail("fd b saw %.20s, expected %.20s", buf, msg);
    }
    /* Closing one descriptor must not disturb the other. */
    if (close(a) != 0) { close(b); probe_fail("close(a): %s", strerror(errno)); }
    if (fstat(b, &st) != 0) {
        close(b);
        probe_fail("fstat(b) after close(a): %s", strerror(errno));
    }
    if (st.st_size != (off_t)(sizeof msg - 1)) {
        close(b);
        probe_fail("fd b size after close(a) is %lld, expected %lld",
                   (long long)st.st_size, (long long)(sizeof msg - 1));
    }
    if (close(b) != 0)
        probe_fail("close(b): %s", strerror(errno));
    churn_free();
    probe_info("two fds opened before unlink share one inode to the last close");

    /* ── U4: the name is reusable and the old content is gone ───────────── */
    fd = open(path, O_CREAT | O_RDWR | O_EXCL, 0600);
    if (fd < 0)
        probe_fail("re-create %s after unlink: %s", path, strerror(errno));
    if (fstat(fd, &st) != 0 || st.st_size != 0) {
        close(fd); unlink(path);
        probe_fail("re-created file is %lld bytes, expected 0",
                   (long long)st.st_size);
    }
    close(fd);
    unlink(path);
    probe_info("the name is free again and the new file starts empty");

    /* ── U5: a directory with children cannot be removed ────────────────── */
    char dir[128], inner[192];
    snprintf(dir, sizeof dir, "/tmp/p21.dir.%d", (int)getpid());
    snprintf(inner, sizeof inner, "%s/child", dir);
    if (mkdir(dir, 0700) != 0)
        probe_fail("mkdir(%s): %s", dir, strerror(errno));
    fd = open(inner, O_CREAT | O_RDWR, 0600);
    if (fd < 0) {
        rmdir(dir);
        probe_fail("open(%s): %s", inner, strerror(errno));
    }
    close(fd);
    if (rmdir(dir) == 0) {
        probe_fail("rmdir removed a non-empty directory");
    }
    if (errno != ENOTEMPTY && errno != EEXIST)
        probe_info("rmdir of a non-empty directory gave %s (Linux: ENOTEMPTY)",
                   strerror(errno));
    if (unlink(inner) != 0)
        probe_fail("unlink(%s): %s", inner, strerror(errno));
    if (rmdir(dir) != 0)
        probe_fail("rmdir(%s) after emptying it: %s", dir, strerror(errno));
    probe_info("a non-empty directory is refused, an emptied one is removed");

    probe_pass();
}
