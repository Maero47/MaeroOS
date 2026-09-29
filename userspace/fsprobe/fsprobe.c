/*
 * fsprobe — regression probe for filesystem-layer memory-safety and hang bugs.
 *
 *   - tmpfs: a pwrite whose off+len wraps 32 bits must fail, not write outside
 *     the file body; ftruncate to 3 GiB must fail, not spin in the grow loop.
 *   - VFS: a symlink cycle must fail the open (ELOOP), not recurse the kernel
 *     stack away; a 30-link chain and a relative link must still resolve.
 *   - FIFO: open/close/reopen must keep using a live buffer (the buffer used to
 *     be freed on the last close while the node still pointed at it).
 *   - ext2: repeated lookups of one file must not grow the kernel heap.
 *
 * Prints "fsprobe ok" when everything holds, "fsprobe FAIL: ..." otherwise.
 */
#include "../include/errno.h"
#include "../include/fcntl.h"
#include "../include/stdio.h"
#include "../include/string.h"
#include "../include/syscall.h"
#include "../include/sys/stat.h"
#include "../include/unistd.h"

static int fails;

static void fail(const char *what) {
    printf("fsprobe FAIL: %s\n", what);
    fails++;
}

/* Raw pwrite64 (EAX=181): the libc here has no wrapper.  Returns -errno. */
static int raw_pwrite(int fd, const void *buf, unsigned len, unsigned off) {
    return syscall4(181, fd, (int)buf, (int)len, (int)off);
}

static void check_tmpfs(void) {
    int fd = open("/tmp/fsp.bin", O_CREAT | O_TRUNC | O_RDWR);
    if (fd < 0) { fail("open /tmp/fsp.bin"); return; }
    if (write(fd, "hello", 5) != 5) fail("tmpfs write");

    static char big[0x2000];
    memset(big, 'W', sizeof(big));
    int r = raw_pwrite(fd, big, 0x2000, 0xFFFFF000U);
    if (r >= 0) fail("pwrite at 0xFFFFF000 len 0x2000 succeeded");
    else printf("fsprobe tmpfs wrap pwrite rejected errno=%d\n", -r);

    /* ftruncate(93) takes the length as an unsigned 32-bit value. */
    r = syscall2(93, fd, (int)0xC0000000U);
    if (r >= 0) fail("ftruncate to 3 GiB succeeded");
    else printf("fsprobe tmpfs 3GiB ftruncate rejected errno=%d\n", -r);

    /* The file must be exactly what was written. */
    char back[16];
    memset(back, 0, sizeof(back));
    if (lseek(fd, 0, SEEK_END) != 5) fail("tmpfs size changed");
    lseek(fd, 0, SEEK_SET);
    if (read(fd, back, sizeof(back)) != 5 || memcmp(back, "hello", 5) != 0)
        fail("tmpfs content changed");
    close(fd);
}

static void check_symlinks(void) {
    unlink("/tmp/fsp.loop");
    unlink("/tmp/fsp.la");
    unlink("/tmp/fsp.lb");
    if (symlink("/tmp/fsp.loop", "/tmp/fsp.loop") < 0) { fail("symlink self"); return; }
    if (symlink("/tmp/fsp.lb", "/tmp/fsp.la") < 0 ||
        symlink("/tmp/fsp.la", "/tmp/fsp.lb") < 0) { fail("symlink pair"); return; }

    const char *loops[] = { "/tmp/fsp.loop", "/tmp/fsp.la", "/tmp/fsp.loop/x" };
    for (unsigned i = 0; i < sizeof(loops) / sizeof(loops[0]); i++) {
        errno = 0;
        int fd = open(loops[i], O_RDONLY);
        if (fd >= 0) { fail("open of a symlink loop succeeded"); close(fd); continue; }
        printf("fsprobe symlink loop %s rejected errno=%d%s\n", loops[i], errno,
               errno == ELOOP ? " (ELOOP)" : "");
        struct stat st;
        if (stat(loops[i], &st) == 0) fail("stat of a symlink loop succeeded");
    }

    /* A chain of 30 absolute links stays inside the 40-link budget. */
    char name[32], target[32];
    for (int i = 0; i < 30; i++) {
        snprintf(name, sizeof(name), "/tmp/fsp.c%d", i);
        if (i == 29) snprintf(target, sizeof(target), "/tmp/fsp.bin");
        else         snprintf(target, sizeof(target), "/tmp/fsp.c%d", i + 1);
        unlink(name);
        if (symlink(target, name) < 0) { fail("symlink chain"); return; }
    }
    char buf[8];
    int fd = open("/tmp/fsp.c0", O_RDONLY);
    if (fd < 0) fail("30-link chain did not resolve");
    else {
        if (read(fd, buf, 5) != 5 || memcmp(buf, "hello", 5) != 0)
            fail("30-link chain read");
        close(fd);
    }

    /* Relative target, resolved against the link's directory. */
    unlink("/tmp/fsp.rel");
    if (symlink("fsp.bin", "/tmp/fsp.rel") < 0) { fail("symlink relative"); return; }
    fd = open("/tmp/fsp.rel", O_RDONLY);
    if (fd < 0) fail("relative symlink did not resolve");
    else {
        if (read(fd, buf, 5) != 5 || memcmp(buf, "hello", 5) != 0)
            fail("relative symlink read");
        close(fd);
    }

    unlink("/tmp/fsp.loop");
    unlink("/tmp/fsp.la");
    unlink("/tmp/fsp.lb");
    unlink("/tmp/fsp.rel");
    for (int i = 0; i < 30; i++) {
        snprintf(name, sizeof(name), "/tmp/fsp.c%d", i);
        unlink(name);
    }
}

static void check_fifo(void) {
    unlink("/tmp/fsp.fifo");
    if (mkfifo("/tmp/fsp.fifo", 0666) < 0) { fail("mkfifo"); return; }

    static char fill[4000];
    memset(fill, 'Z', sizeof(fill));
    for (int round = 0; round < 5; round++) {
        int rfd = open("/tmp/fsp.fifo", O_RDONLY | O_NONBLOCK);
        int wfd = open("/tmp/fsp.fifo", O_WRONLY);
        if (rfd < 0 || wfd < 0) { fail("fifo open"); return; }
        char c = 0;
        if (write(wfd, "x", 1) != 1) fail("fifo write");
        if (read(rfd, &c, 1) != 1 || c != 'x') fail("fifo read");
        close(wfd);
        close(rfd);

        /* A heap block the size of a freed pipe buffer: if the FIFO still
         * pointed at freed memory, the next round's write would land in this
         * file's body. */
        int fd = open("/tmp/fsp.fill", O_CREAT | O_TRUNC | O_RDWR);
        if (fd < 0) { fail("fill open"); return; }
        if (write(fd, fill, sizeof(fill)) != (int)sizeof(fill)) fail("fill write");
        int wfd2 = open("/tmp/fsp.fifo", O_WRONLY);
        int rfd2 = open("/tmp/fsp.fifo", O_RDONLY | O_NONBLOCK);
        if (wfd2 >= 0) { write(wfd2, "yy", 2); close(wfd2); }
        if (rfd2 >= 0) close(rfd2);
        static char back[4000];
        lseek(fd, 0, SEEK_SET);
        if (read(fd, back, sizeof(back)) != (int)sizeof(back) ||
            memcmp(back, fill, sizeof(back)) != 0)
            fail("fifo write corrupted another file (use-after-free)");
        close(fd);
        unlink("/tmp/fsp.fill");
    }
    unlink("/tmp/fsp.fifo");
}

static long mem_free_kb(void) {
    char buf[256];
    int fd = open("/proc/meminfo", O_RDONLY);
    if (fd < 0) return -1;
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';
    char *p = strstr(buf, "MemFree:");
    if (!p) return -1;
    p += 8;
    while (*p == ' ') p++;
    long v = 0;
    while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
    return v;
}

static void check_ext2_lookup_leak(void) {
    struct stat st;
    if (stat("/disk/hello.txt", &st) < 0) {
        puts("fsprobe ext2 leak check skipped (no /disk)");
        return;
    }
    for (int i = 0; i < 200; i++) stat("/disk/hello.txt", &st);
    long before = mem_free_kb();
    for (int i = 0; i < 6000; i++) stat("/disk/hello.txt", &st);
    long after = mem_free_kb();
    if (before < 0 || after < 0) { fail("read /proc/meminfo"); return; }
    long grew = before - after;
    printf("fsprobe ext2 6000 lookups: free memory delta %ld kB\n", grew);
    /* One leaked node per lookup was ~0.4 KiB: 6000 lookups took ~2.4 MiB. */
    if (grew > 1024) fail("ext2 lookups leak kernel memory");
}

int main(void) {
    check_tmpfs();
    check_symlinks();
    check_fifo();
    check_ext2_lookup_leak();
    unlink("/tmp/fsp.bin");
    if (fails) {
        printf("fsprobe FAIL (%d)\n", fails);
        return 1;
    }
    puts("fsprobe ok");
    return 0;
}
