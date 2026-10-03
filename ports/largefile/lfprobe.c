/*
 * lfprobe - 64-bit file offsets end to end (tools/smoke_largefile.py).
 *
 *   lfprobe big  DIR   a file past 4 GiB and 8 GiB on DIR (ext4, exFAT)
 *   lfprobe efbig DIR  a filesystem whose files stop at 4 GiB - 1 (vfat)
 *
 * Static musl binary: off_t is 64-bit, open() passes O_LARGEFILE, lseek()
 * is _llseek, pread/pwrite are pread64/pwrite64, ftruncate is ftruncate64,
 * fstat is fstat64, F_SETLK is F_SETLK64, mmap is mmap2.  The 32-bit off_t
 * ABI (lseek 19, open without O_LARGEFILE, stat 106/fstat 108, sendfile 187)
 * is reached with raw syscalls, as a binary built with a 32-bit off_t would.
 * The same binary runs on a Linux host as the reference.
 *
 * Final state of DIR/lf.bin after "big" (checked again by the host):
 *   size 9 GiB; at 4 GiB - 100 the first 50 bytes of marker 1; at
 *   8 GiB + 12345 marker 3 (1000 bytes); zeroes everywhere else.
 * marker(k)[i] = (i * 31 + k * 17 + (i >> 8)) & 0xFF.
 *
 * Prints "info ..." lines and one final "PASS lfprobe" or "FAIL lfprobe: ...".
 */
#define _GNU_SOURCE 1
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef SEEK_DATA
#define SEEK_DATA 3
#define SEEK_HOLE 4
#endif
#ifndef SYS_statx
#define SYS_statx 383
#endif

#define GiB (1LL << 30)
#define M1_OFF (4 * GiB - 100)
#define M1_LEN 200
#define M2_OFF (8 * GiB + 12345)
#define M2_LEN 1000
#define FINAL  (9 * GiB)

static void fail(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
static void fail(const char *fmt, ...) {
    va_list ap;
    printf("FAIL lfprobe: ");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf(" (errno %d %s)\n", errno, strerror(errno));
    fflush(stdout);
    _exit(1);
}

static void info(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void info(const char *fmt, ...) {
    va_list ap;
    printf("info lfprobe: ");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

static void marker(int k, unsigned char *b, int n) {
    for (int i = 0; i < n; i++) b[i] = (unsigned char)((i * 31 + k * 17 + (i >> 8)) & 0xFF);
}

static void check_bytes(int fd, long long off, const unsigned char *want, int n, const char *what) {
    unsigned char got[4096];
    if (n > (int)sizeof got) fail("check_bytes: %d too long", n);
    ssize_t r = pread(fd, got, n, off);
    if (r != n) fail("%s: pread(%d at %lld) = %zd", what, n, off, r);
    if (memcmp(got, want, n)) fail("%s: wrong bytes at %lld", what, off);
}

static void check_zero(int fd, long long off, int n, const char *what) {
    static const unsigned char z[4096];
    check_bytes(fd, off, z, n, what);
}

static long long fsize(int fd) {
    struct stat st;
    if (fstat(fd, &st)) fail("fstat");
    return (long long)st.st_size;
}

/* The 32-bit off_t ABI on a file past 2 GiB: EOVERFLOW, never a wrapped value. */
static void check_off32(int fd, const char *path) {
    /* An x86_64 kernel running this i386 binary returns the truncated
     * offset from compat lseek (its off_t is 64-bit); an i386 kernel
     * checks it (Linux ksys_lseek). */
    struct utsname u;
    int compat = uname(&u) == 0 && strcmp(u.machine, "x86_64") == 0;
    long r;
    if (compat) {
        info("x86_64 kernel: compat lseek(2) does not report EOVERFLOW; not checked");
        goto open32;
    }
    if (lseek(fd, 0, SEEK_SET) != 0) fail("lseek to 0");
    errno = 0;
    r = syscall(SYS_lseek, fd, 0L, SEEK_END);
    if (r != -1 || errno != EOVERFLOW) fail("lseek(2) 32-bit SEEK_END = %ld, want EOVERFLOW", r);
    /* Linux ksys_lseek moves the position first and then finds that the
     * result does not fit. */
    if (lseek(fd, 0, SEEK_CUR) != fsize(fd)) fail("offset after the EOVERFLOW lseek");
    errno = 0;
    r = syscall(SYS_lseek, fd, 0x7FFFFFFFL, SEEK_SET);
    if (r != 0x7FFFFFFFL) fail("lseek(2) to 2 GiB - 1 = %ld", r);
    errno = 0;
    r = syscall(SYS_lseek, fd, 1L, SEEK_CUR);
    if (r != -1 || errno != EOVERFLOW) fail("lseek(2) to 2 GiB = %ld, want EOVERFLOW", r);
    info("lseek(2) with a 32-bit off_t: EOVERFLOW past 2 GiB - 1");
open32:

    errno = 0;
    r = syscall(SYS_open, path, O_RDONLY);
    if (r != -1 || errno != EOVERFLOW) fail("open without O_LARGEFILE = %ld, want EOVERFLOW", r);
    unsigned char st32[64];
    errno = 0;
    r = syscall(SYS_fstat, fd, st32);
    if (r != -1 || errno != EOVERFLOW) fail("fstat(108) = %ld, want EOVERFLOW", r);
    errno = 0;
    r = syscall(SYS_stat, path, st32);
    if (r != -1 || errno != EOVERFLOW) fail("stat(106) = %ld, want EOVERFLOW", r);
    info("open without O_LARGEFILE, stat(106), fstat(108): EOVERFLOW");
}

static void check_locks(int fd) {
    struct flock fl = { .l_type = F_WRLCK, .l_whence = SEEK_SET,
                        .l_start = 5 * GiB, .l_len = 4096 };
    if (fcntl(fd, F_SETLK, &fl)) fail("F_SETLK64 at 5 GiB");
    pid_t pid = fork();
    if (pid < 0) fail("fork");
    if (pid == 0) {
        struct flock c = { .l_type = F_WRLCK, .l_whence = SEEK_SET,
                           .l_start = 5 * GiB + 100, .l_len = 10 };
        if (fcntl(fd, F_SETLK, &c) == 0 || (errno != EAGAIN && errno != EACCES)) _exit(10);
        c.l_start = 5 * GiB + 100;
        if (fcntl(fd, F_GETLK, &c)) _exit(11);
        if (c.l_type != F_WRLCK || c.l_start != 5 * GiB || c.l_len != 4096 || c.l_pid != getppid())
            _exit(12);
        c.l_type = F_WRLCK; c.l_start = 5 * GiB + 4096; c.l_len = 4096;   /* adjacent */
        if (fcntl(fd, F_SETLK, &c)) _exit(13);
        c.l_start = 1 * GiB; c.l_len = 4096;          /* 5 GiB mod 4 GiB: no conflict */
        if (fcntl(fd, F_SETLK, &c)) _exit(14);
        /* The 32-bit struct flock cannot describe the conflicting lock. */
        struct { short type, whence; long start, len; int pid; } s = { F_WRLCK, SEEK_SET, 0, 0, 0 };
        errno = 0;
        s.start = 0; s.len = 0;                        /* whole file: hits 5 GiB */
        if (syscall(SYS_fcntl, fd, 5 /* F_GETLK */, &s) != -1 || errno != EOVERFLOW) _exit(15);
        _exit(0);
    }
    int st;
    if (waitpid(pid, &st, 0) != pid) fail("waitpid");
    if (!WIFEXITED(st) || WEXITSTATUS(st)) fail("lock child exit status %d", WEXITSTATUS(st));
    fl.l_type = F_UNLCK;
    if (fcntl(fd, F_SETLK, &fl)) fail("F_UNLCK");
    info("F_SETLK64/F_GETLK64 at 5 GiB: conflicts, adjacent and 1 GiB ranges right; F_GETLK EOVERFLOW");
}

static void check_mmap(int fd) {
    long long off = M2_OFF & ~4095LL;
    unsigned char *p = mmap(NULL, 8192, PROT_READ, MAP_PRIVATE, fd, off);
    if (p == MAP_FAILED) fail("mmap at %lld", off);
    unsigned char m[M2_LEN];
    marker(2, m, M2_LEN);
    if (memcmp(p + (M2_OFF - off), m, M2_LEN)) fail("mmap at 8 GiB: wrong bytes");
    munmap(p, 8192);
    info("mmap2 with a page offset past 8 GiB reads marker 2");
}

static void check_copy(const char *dir, int fd) {
    char path[512];
    snprintf(path, sizeof path, "%s/lfcopy.bin", dir);
    int out = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (out < 0) fail("open %s", path);
    loff_t in_off = M2_OFF, out_off = 4 * GiB + 1;
    ssize_t r = copy_file_range(fd, &in_off, out, &out_off, M2_LEN, 0);
    if (r != M2_LEN) fail("copy_file_range = %zd", r);
    if (in_off != M2_OFF + M2_LEN || out_off != 4 * GiB + 1 + M2_LEN)
        fail("copy_file_range offsets %lld %lld", (long long)in_off, (long long)out_off);
    unsigned char m[M2_LEN];
    marker(2, m, M2_LEN);
    check_bytes(out, 4 * GiB + 1, m, M2_LEN, "copy_file_range result");
    off_t so = M2_OFF;
    if (lseek(out, 0, SEEK_SET) != 0) fail("lseek out");
    r = sendfile(out, fd, &so, 100);                   /* sendfile64 */
    if (r != 100 || so != M2_OFF + 100) fail("sendfile64 = %zd, offset %lld", r, (long long)so);
    check_bytes(out, 0, m, 100, "sendfile64 result");
    long so32 = 0x7FFFFF00L;
    errno = 0;
    r = syscall(SYS_sendfile, out, fd, &so32, 0x200L);
    if (r != 0xFF || so32 != 0x7FFFFFFFL)
        fail("sendfile(187) near 2 GiB = %zd, offset %ld (want 255 and 2 GiB - 1)", r, so32);
    errno = 0;
    r = syscall(SYS_sendfile, out, fd, &so32, 1L);
    if (r != -1 || errno != EOVERFLOW) fail("sendfile(187) at 2 GiB - 1 = %zd, want EOVERFLOW", r);
    close(out);
    if (unlink(path)) fail("unlink %s", path);
    info("copy_file_range and sendfile64 past 4 GiB; sendfile(187) stops at 2 GiB - 1");
}

static void big(const char *dir) {
    char path[512];
    snprintf(path, sizeof path, "%s/lf.bin", dir);
    int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (fd < 0) fail("open %s", path);
    unsigned char m1[M1_LEN], m2[M2_LEN], m3[M2_LEN];
    marker(1, m1, M1_LEN);
    marker(2, m2, M2_LEN);
    marker(3, m3, M2_LEN);

    /* Marker 1 straddles 4 GiB, through lseek + write. */
    if (lseek(fd, M1_OFF, SEEK_SET) != M1_OFF) fail("lseek to 4 GiB - 100");
    if (write(fd, m1, M1_LEN) != M1_LEN) fail("write across 4 GiB");
    if (lseek(fd, 0, SEEK_CUR) != M1_OFF + M1_LEN) fail("offset after the write");
    /* Marker 2 past 8 GiB, through pwrite64. */
    if (pwrite(fd, m2, M2_LEN, M2_OFF) != M2_LEN) fail("pwrite at 8 GiB");
    if (fsync(fd)) fail("fsync");
    long long sz = fsize(fd);
    if (sz != M2_OFF + M2_LEN) fail("fstat64 size %lld, want %lld", sz, M2_OFF + M2_LEN);
    struct stat st;
    if (stat(path, &st) || st.st_size != M2_OFF + M2_LEN) fail("stat64 size %lld", (long long)st.st_size);
    unsigned char sx[256];                    /* struct statx: stx_size at 40 */
    unsigned long long sxsize;
    if (syscall(SYS_statx, AT_FDCWD, path, 0, 0x200 /* STATX_SIZE */, sx)) fail("statx");
    memcpy(&sxsize, sx + 40, 8);
    if ((long long)sxsize != M2_OFF + M2_LEN) fail("statx size %llu", sxsize);
    if (lseek(fd, 0, SEEK_END) != M2_OFF + M2_LEN) fail("SEEK_END");
    info("markers written at 4 GiB - 100 (straddling) and 8 GiB + 12345; stat64/statx size %lld", sz);

    /* Read back, through lseek + read and pread64; the hole reads as zeroes. */
    unsigned char b[M2_LEN];
    if (lseek(fd, M1_OFF, SEEK_SET) != M1_OFF || read(fd, b, M1_LEN) != M1_LEN ||
        memcmp(b, m1, M1_LEN))
        fail("read back marker 1");
    check_bytes(fd, M2_OFF, m2, M2_LEN, "marker 2");
    check_zero(fd, 6 * GiB, 4096, "the hole at 6 GiB");
    check_zero(fd, 4 * GiB + 100, 4096, "past marker 1");
    if (pread(fd, b, 10, M2_OFF + M2_LEN) != 0) fail("read at EOF past 8 GiB");
    if (lseek(fd, 3, SEEK_DATA) != 3 || lseek(fd, 3, SEEK_HOLE) != M2_OFF + M2_LEN) {
        /* Filesystems may report real holes; only check what is always true. */
        long long d = lseek(fd, M2_OFF, SEEK_DATA);
        if (d < 0 || d > M2_OFF) fail("SEEK_DATA at 8 GiB = %lld", d);
    }
    info("both markers read back; the hole reads as zeroes");

    check_off32(fd, path);
    check_locks(fd);
    check_mmap(fd);
    check_copy(dir, fd);

    /* Truncate down across 4 GiB, then up past 8 GiB. */
    if (ftruncate(fd, 4 * GiB - 50)) fail("ftruncate down to 4 GiB - 50");
    if (fsize(fd) != 4 * GiB - 50) fail("size after shrinking %lld", fsize(fd));
    check_bytes(fd, M1_OFF, m1, 50, "marker 1 head after shrinking");
    if (pread(fd, b, 100, M1_OFF) != 50) fail("read across the new EOF");
    if (ftruncate(fd, FINAL)) fail("ftruncate up to 9 GiB");
    if (fsize(fd) != FINAL) fail("size after growing %lld", fsize(fd));
    check_zero(fd, M1_OFF + 50, M1_LEN - 50, "marker 1 tail after regrowing");
    check_zero(fd, M2_OFF, M2_LEN, "marker 2 after regrowing");
    check_zero(fd, FINAL - 4096, 4096, "the end after regrowing");
    info("ftruncate64 down to 4 GiB - 50 and up to 9 GiB: old bytes gone, zeroes read");

    if (pwrite(fd, m3, M2_LEN, M2_OFF) != M2_LEN) fail("pwrite marker 3");
    check_bytes(fd, M2_OFF, m3, M2_LEN, "marker 3");
    if (fsize(fd) != FINAL) fail("size changed by marker 3: %lld", fsize(fd));
    if (fsync(fd)) fail("fsync");
    close(fd);

    /* A fresh descriptor sees the same. */
    fd = open(path, O_RDONLY);
    if (fd < 0) fail("reopen");
    check_bytes(fd, M1_OFF, m1, 50, "marker 1 head after reopening");
    check_bytes(fd, M2_OFF, m3, M2_LEN, "marker 3 after reopening");
    if (fsize(fd) != FINAL) fail("size after reopening");
    close(fd);
    info("final: 9 GiB, marker 1 head at 4 GiB - 100, marker 3 at 8 GiB + 12345");
}

static void efbig(const char *dir) {
    char path[512];
    snprintf(path, sizeof path, "%s/efbig.bin", dir);
    int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (fd < 0) fail("open %s", path);
    if (write(fd, "small\n", 6) != 6) fail("small write");
    static const long long bad[] = { 4 * GiB - 1, 4 * GiB, 5 * GiB, 9 * GiB };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        errno = 0;
        if (pwrite(fd, "x", 1, bad[i]) != -1 || errno != EFBIG)
            fail("pwrite at %lld: want EFBIG", bad[i]);
        if (bad[i] > 4 * GiB - 1) {
            errno = 0;
            if (ftruncate(fd, bad[i]) != -1 || errno != EFBIG)
                fail("ftruncate to %lld: want EFBIG", bad[i]);
        }
    }
    if (fsize(fd) != 6) fail("size changed to %lld", fsize(fd));
    close(fd);
    info("writes at and past 4 GiB - 1 and truncates past it: EFBIG; the file is untouched");
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc != 3) {
        fprintf(stderr, "usage: lfprobe big|efbig DIR\n");
        return 2;
    }
    if (!strcmp(argv[1], "big")) big(argv[2]);
    else if (!strcmp(argv[1], "efbig")) efbig(argv[2]);
    else fail("unknown mode %s", argv[1]);
    printf("PASS lfprobe\n");
    return 0;
}
