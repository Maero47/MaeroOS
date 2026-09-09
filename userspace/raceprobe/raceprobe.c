/* raceprobe — does a clustered ext2 read ever publish stale blocks?
 *
 * The clustered multi-block read in ext2_read_node fills the caller's buffer
 * from the disk and then inserts those blocks into the block cache.  If a
 * writer runs between the read and the insert, the writer's fresh contents
 * reach both the disk and the cache, and the reader then republishes the copy
 * it took BEFORE that write.  The cache is left permanently disagreeing with
 * the disk for those blocks, which every later reader sees — silent corruption,
 * not a stale-performance problem.
 *
 * The probe drives exactly that shape:
 *
 *   reader   mmap()s the whole file PRIVATE and touches every page.  Each fault
 *            fills a KERNEL temp mapping, which is the only destination the
 *            clustered path accepts, so every miss is a clustered read.
 *   writer   rewrites whole 1 KiB blocks of the same file with an increasing
 *            generation number, and remembers the last one it wrote.
 *
 * The file is deliberately several times larger than the block cache so the
 * reader keeps missing (a hit never reaches the clustered path).  After both
 * threads stop, every block is read back through a fresh open and compared with
 * the last generation actually written.  read() consults the same cache, so a
 * poisoned entry shows up as a mismatch.  A clean run proves nothing on its own
 * — the point is the comparison against the unfixed kernel, which fails.
 *
 * Each block holds (magic, block index, generation) repeated, so a stale block,
 * a torn block and a misdirected block are told apart.
 *
 * usage: raceprobe [file_kib] [reader_passes]
 */
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define BLK        1024u
#define MAGIC      0x52434501u          /* "RCE\1" */
#define MAX_BLOCKS 16384u               /* 16 MiB ceiling */

static const char *PATH = "/disk/raceprobe.bin";

static unsigned n_blocks   = 12u * 1024u;   /* 12 MiB */
static unsigned passes     = 12u;
static unsigned last_gen[MAX_BLOCKS];       /* last generation written, per block */
static volatile int reader_done;
static unsigned writes_done;

/* Stamp a block buffer with (magic, index, generation), repeated. */
static void stamp(unsigned *b, unsigned idx, unsigned gen) {
    for (unsigned i = 0; i < BLK / 4; i += 4) {
        b[i + 0] = MAGIC;
        b[i + 1] = idx;
        b[i + 2] = gen;
        b[i + 3] = idx ^ gen;
    }
}

/* 0 = matches (idx,gen); otherwise a description of how it differs. */
static const char *check(const unsigned *b, unsigned idx, unsigned gen,
                         unsigned *saw_gen) {
    *saw_gen = b[2];
    if (b[0] != MAGIC)   return "not a stamped block";
    if (b[1] != idx)     return "wrong block index (misdirected read)";
    if (b[3] != (b[1] ^ b[2])) return "torn block";
    for (unsigned i = 4; i < BLK / 4; i += 4)
        if (b[i + 2] != b[2]) return "torn block (generation varies inside it)";
    if (b[2] != gen)     return "STALE";
    return 0;
}

static void *writer(void *arg) {
    static unsigned buf[BLK / 4];
    unsigned gen = 1;
    int fd = *(int *)arg;

    while (!reader_done) {
        for (unsigned i = 0; i < n_blocks && !reader_done; i++) {
            stamp(buf, i, gen);
            if (lseek(fd, (int)(i * BLK), 0) < 0) continue;
            if (write(fd, buf, BLK) == (int)BLK) {
                last_gen[i] = gen;
                writes_done++;
            }
            /* Give the reader the CPU back promptly: the window we are hunting
             * is only a few microseconds wide inside each clustered read. */
            sched_yield();
        }
        gen++;
    }
    return 0;
}

int main(int argc, char **argv) {
    static unsigned buf[BLK / 4];
    unsigned i, p;

    if (argc > 1) {
        unsigned kib = 0;
        for (const char *s = argv[1]; *s >= '0' && *s <= '9'; s++)
            kib = kib * 10 + (unsigned)(*s - '0');
        if (kib && kib <= MAX_BLOCKS) n_blocks = kib;
    }
    if (argc > 2) {
        unsigned n = 0;
        for (const char *s = argv[2]; *s >= '0' && *s <= '9'; s++)
            n = n * 10 + (unsigned)(*s - '0');
        if (n) passes = n;
    }

    printf("raceprobe: file=%u KiB (%u blocks), reader passes=%u\n",
           n_blocks, n_blocks, passes);

    /* Lay the file down, every block at generation 0. */
    int fd = open(PATH, O_RDWR | O_CREAT | O_TRUNC);
    if (fd < 0) { printf("raceprobe: FAIL cannot create %s\n", PATH); return 1; }
    for (i = 0; i < n_blocks; i++) {
        stamp(buf, i, 0);
        if (write(fd, buf, BLK) != (int)BLK) {
            printf("raceprobe: FAIL short write laying down block %u\n", i);
            close(fd);
            return 1;
        }
        last_gen[i] = 0;
    }
    close(fd);

    int wfd = open(PATH, O_RDWR);
    if (wfd < 0) { printf("raceprobe: FAIL cannot reopen for writing\n"); return 1; }

    /* Self-test the write path from the main thread first, so a silent writer
     * thread is never mistaken for a clean run. */
    stamp(buf, 0, 0);
    if (lseek(wfd, 0, 0) != 0 || write(wfd, buf, BLK) != (int)BLK) {
        printf("raceprobe: FAIL write path does not work at all\n");
        return 1;
    }

    pthread_t th;
    if (pthread_create(&th, 0, writer, &wfd) != 0) {
        printf("raceprobe: FAIL cannot start writer thread\n");
        return 1;
    }

    /* Reader: mmap the whole file and touch every page, repeatedly.  Each pass
     * uses a fresh mapping so the pages fault in again. */
    unsigned long touched = 0;
    for (p = 0; p < passes; p++) {
        int rfd = open(PATH, O_RDONLY);
        if (rfd < 0) { printf("raceprobe: FAIL cannot open for reading\n"); break; }
        unsigned len = n_blocks * BLK;
        volatile unsigned char *m =
            (volatile unsigned char *)mmap(0, len, PROT_READ, MAP_PRIVATE, rfd, 0);
        if (m == (void *)-1 || !m) {
            printf("raceprobe: FAIL mmap of %u bytes\n", len);
            close(rfd);
            break;
        }
        for (unsigned off = 0; off < len; off += 4096) {
            (void)m[off];
            touched++;
            /* Hand the CPU over regularly.  Without this the writer starves:
             * the reader spends nearly all its time in the page-fault handler,
             * and scheduler_tick only preempts a thread it interrupts in USER
             * mode, so a fault-bound thread is effectively non-preemptible. */
            if ((touched & 7u) == 0) sched_yield();
        }
        munmap((void *)m, len);
        close(rfd);
    }
    reader_done = 1;
    pthread_join(th, 0);
    close(wfd);

    printf("raceprobe: %lu pages faulted, %u block writes\n", touched, writes_done);

    /* Verify through a fresh open: every block must carry the last generation
     * the writer recorded for it. */
    fd = open(PATH, O_RDONLY);
    if (fd < 0) { printf("raceprobe: FAIL cannot reopen to verify\n"); return 1; }
    unsigned bad = 0, first_bad = 0;
    const char *first_why = 0;
    unsigned first_want = 0, first_saw = 0;
    for (i = 0; i < n_blocks; i++) {
        if (read(fd, buf, BLK) != (int)BLK) {
            printf("raceprobe: FAIL short read verifying block %u\n", i);
            close(fd);
            return 1;
        }
        unsigned saw = 0;
        const char *why = check(buf, i, last_gen[i], &saw);
        if (why) {
            if (!bad) {
                first_bad = i; first_why = why;
                first_want = last_gen[i]; first_saw = saw;
            }
            bad++;
        }
    }
    close(fd);
    unlink(PATH);

    if (bad) {
        printf("raceprobe: FAIL %u of %u blocks wrong; first block %u: %s "
               "(wrote gen %u, read gen %u)\n",
               bad, n_blocks, first_bad, first_why, first_want, first_saw);
        return 1;
    }
    printf("raceprobe: OK all %u blocks match the last write\n", n_blocks);
    return 0;
}
