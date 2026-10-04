/*
 * P53 an unlinked file held open stays intact however many files are open.
 *
 * Linux: the inode of a file that is unlinked while open lives on until the
 * last descriptor closes; its blocks are never handed to another file in
 * the meantime, whatever else the process (or the system) has open.
 *
 * MaeroOS: fs/ext2.c counted open inodes in a 128-slot table per
 * filesystem.  The 129th open inode went untracked, so unlinking it freed
 * the inode and its blocks at once; the descriptor then read (and wrote)
 * whatever file was given those blocks next -- another user's, too.
 *
 * Runs in the directory given as the argument (smoke-abi passes /disk, the
 * ext2 volume), or in /tmp without one.
 */
#define PROBE_NAME "p53_ext2_open_many"
#include "probe.h"
#include <sys/stat.h>

#define NHOLD  160
#define FSIZE  (64 * 1024)

static char buf[FSIZE], chk[FSIZE];

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "/tmp";
    probe_watchdog(120);
    int pid = (int)getpid();
    char path[256];
    static int hold[NHOLD];

    /* Fill the old table: NHOLD distinct inodes held open. */
    for (int i = 0; i < NHOLD; i++) {
        snprintf(path, sizeof path, "%s/p53h.%d.%d", dir, pid, i);
        hold[i] = open(path, O_CREAT | O_RDWR | O_TRUNC, 0600);
        if (hold[i] < 0)
            probe_fail("open %s: %s", path, strerror(errno));
        unlink(path);
    }

    /* The victim: written, opened, unlinked. */
    for (int i = 0; i < FSIZE; i++) buf[i] = (char)('A' + i % 23);
    snprintf(path, sizeof path, "%s/p53v.%d", dir, pid);
    int v = open(path, O_CREAT | O_RDWR | O_TRUNC, 0600);
    if (v < 0 || write(v, buf, FSIZE) != FSIZE)
        probe_fail("write victim %s: %s", path, strerror(errno));
    fsync(v);
    if (unlink(path) != 0)
        probe_fail("unlink %s: %s", path, strerror(errno));

    /* Other files take whatever blocks and inodes are free now. */
    memset(chk, 'z', FSIZE);
    for (int i = 0; i < 8; i++) {
        snprintf(path, sizeof path, "%s/p53o.%d.%d", dir, pid, i);
        int o = open(path, O_CREAT | O_RDWR | O_TRUNC, 0600);
        if (o < 0 || write(o, chk, FSIZE) != FSIZE)
            probe_fail("write %s: %s", path, strerror(errno));
        fsync(o);
        close(o);
    }

    struct stat st;
    if (fstat(v, &st) != 0)
        probe_fail("fstat victim: %s", strerror(errno));
    if (st.st_size != FSIZE)
        probe_fail("victim size %ld, want %d", (long)st.st_size, FSIZE);
    memset(chk, 0, FSIZE);
    if (pread(v, chk, FSIZE, 0) != FSIZE)
        probe_fail("read victim: %s", strerror(errno));
    if (memcmp(chk, buf, FSIZE) != 0) {
        int at = 0;
        while (at < FSIZE && chk[at] == buf[at]) at++;
        probe_fail("unlinked open file changed at byte %d ('%c', want '%c'):"
                   " its blocks went to another file", at, chk[at], buf[at]);
    }

    close(v);
    for (int i = 0; i < NHOLD; i++) close(hold[i]);
    for (int i = 0; i < 8; i++) {
        snprintf(path, sizeof path, "%s/p53o.%d.%d", dir, pid, i);
        unlink(path);
    }
    probe_pass();
}
