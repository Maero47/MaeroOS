/*
 * sysmiscprobe — regression probe for the syscall-layer sweep: long relative
 * paths (lstat/lstat64/readlink/symlink used to join cwd + path into a stack
 * buffer unchecked), symlink loops (-ELOOP), out-of-range offsets (lseek,
 * _llseek, ftruncate, pwrite64), the exact Linux getdents/getdents64 record
 * layouts, and waitpid(WUNTRACED) reporting a stop once.
 *
 * Every call is a raw int $0x80 so the syscall under test is the one reached
 * (a libc might route lstat through fstatat64, say), and results are compared
 * as raw negative errnos.  Prints "FAIL: ..." per failure and
 * "sysmiscprobe ok" only if there were none.
 */
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <syscall.h>

#define SYS_exit        1
#define SYS_fork        2
#define SYS_write       4
#define SYS_open        5
#define SYS_close       6
#define SYS_waitpid     7
#define SYS_unlink     10
#define SYS_chdir      12
#define SYS_lseek      19
#define SYS_getpid     20
#define SYS_access     33
#define SYS_kill       37
#define SYS_mkdir      39
#define SYS_rmdir      40
#define SYS_symlink    83
#define SYS_readlink   85
#define SYS_ftruncate  93
#define SYS_stat      106
#define SYS_lstat     107
#define SYS__llseek   140
#define SYS_getdents  141
#define SYS_pwrite64  181
#define SYS_getcwd    183
#define SYS_stat64    195
#define SYS_lstat64   196
#define SYS_getdents64 220

#define O_RDONLY  0
#define O_WRONLY  1
#define O_RDWR    2
#define O_CREAT   0100

#define E_NOENT        2
#define E_INVAL       22
#define E_FBIG        27
#define E_NAMETOOLONG 36
#define E_LOOP        40

static int fails;

static int sc5(int num, int a1, int a2, int a3, int a4, int a5) {
    int ret;
    __asm__ volatile("int $0x80"
        : "=a"(ret)
        : "0"(num), "b"(a1), "c"(a2), "d"(a3), "S"(a4), "D"(a5)
        : "memory");
    return ret;
}

#define FAILF(...) do { printf("FAIL: " __VA_ARGS__); fails++; } while (0)

static void expect(const char *what, int got, int want) {
    if (got != want)
        FAILF("%s: got %d, want %d\n", what, got, want);
}

static int bounded_len(const char *s, int max) {
    int n = 0;
    while (n < max && s[n]) n++;
    return n;
}

static void fill(char *buf, char c, int n) {
    memset(buf, c, (size_t)n);
    buf[n] = '\0';
}

/* ── 1. long relative paths ──────────────────────────────────────────────── */

#define DEEP_ROOT "/tmp/smp_deep"
#define DEEP_LEVELS 5
#define DEEP_COMP   46  /* "/tmp/smp_deep" + 5 x 47 = 248 bytes of cwd */

static void test_long_paths(void) {
    static char comp[DEEP_COMP + 1], name[251], two[301], cwd[300];
    static unsigned char st[256];

    syscall2(SYS_mkdir, (int)DEEP_ROOT, 0755);
    if (syscall1(SYS_chdir, (int)DEEP_ROOT) < 0) {
        FAILF("chdir %s\n", DEEP_ROOT);
        return;
    }
    fill(comp, 'd', DEEP_COMP);
    for (int i = 0; i < DEEP_LEVELS; i++) {
        syscall2(SYS_mkdir, (int)comp, 0755);
        if (syscall1(SYS_chdir, (int)comp) < 0) {
            FAILF("chdir level %d\n", i);
            syscall1(SYS_chdir, (int)"/");
            return;
        }
    }
    int cl = syscall2(SYS_getcwd, (int)cwd, sizeof(cwd));
    printf("sysmiscprobe: cwd length %d\n", cl > 0 ? cl - 1 : cl);

    /* cwd (~250) + '/' + 250 = ~501 bytes: under the 511-byte lookup limit,
     * so ENOENT is right; ENAMETOOLONG is tolerated.  The old code copied
     * this into a 256-byte stack buffer. */
    fill(name, 'n', 250);
    int r;
    r = syscall2(SYS_lstat, (int)name, (int)st);
    printf("sysmiscprobe: lstat long -> %d\n", r);
    if (r != -E_NOENT && r != -E_NAMETOOLONG) FAILF("lstat long: %d\n", r);
    r = syscall2(SYS_lstat64, (int)name, (int)st);
    printf("sysmiscprobe: lstat64 long -> %d\n", r);
    if (r != -E_NOENT && r != -E_NAMETOOLONG) FAILF("lstat64 long: %d\n", r);
    r = syscall3(SYS_readlink, (int)name, (int)st, sizeof(st));
    printf("sysmiscprobe: readlink long -> %d\n", r);
    if (r != -E_NOENT && r != -E_NAMETOOLONG) FAILF("readlink long: %d\n", r);

    /* Past 511 bytes it must be ENAMETOOLONG. */
    fill(two, 'x', 150);
    two[150] = '/';
    fill(two + 151, 'y', 149);
    expect("readlink >511", syscall3(SYS_readlink, (int)two, (int)st, sizeof(st)),
           -E_NAMETOOLONG);

    r = syscall2(SYS_symlink, (int)"tgt", (int)name);
    printf("sysmiscprobe: symlink long -> %d\n", r);
    if (r == 0) syscall1(SYS_unlink, (int)name);
    else if (r != -E_NAMETOOLONG) FAILF("symlink long: %d\n", r);

    /* A real file under a >256-byte absolute path must still be found. */
    int fd = syscall3(SYS_open, (int)name, O_WRONLY | O_CREAT, 0644);
    printf("sysmiscprobe: create long -> %d\n", fd < 0 ? fd : 0);
    if (fd >= 0) {
        syscall1(SYS_close, fd);
        expect("lstat64 existing long", syscall2(SYS_lstat64, (int)name, (int)st), 0);
        syscall1(SYS_unlink, (int)name);
    } else if (fd != -E_NAMETOOLONG) {
        FAILF("create long: %d\n", fd);
    }

    for (int i = 0; i < DEEP_LEVELS; i++) {
        syscall1(SYS_chdir, (int)"..");
        syscall1(SYS_rmdir, (int)comp);
    }
    syscall1(SYS_chdir, (int)"/");
    syscall1(SYS_rmdir, (int)DEEP_ROOT);
}

/* ── 2. symlink loops ────────────────────────────────────────────────────── */

static void test_loops(void) {
    static unsigned char st[256];
    const char *a = "/tmp/smp_loopA", *b = "/tmp/smp_loopB", *self = "/tmp/smp_self";
    syscall2(SYS_symlink, (int)b, (int)a);
    syscall2(SYS_symlink, (int)a, (int)b);
    syscall2(SYS_symlink, (int)"smp_self", (int)self);

    expect("open loop", syscall3(SYS_open, (int)a, O_RDONLY, 0), -E_LOOP);
    expect("open loop O_CREAT", syscall3(SYS_open, (int)a, O_WRONLY | O_CREAT, 0644),
           -E_LOOP);
    expect("stat loop", syscall2(SYS_stat, (int)a, (int)st), -E_LOOP);
    expect("stat64 loop", syscall2(SYS_stat64, (int)a, (int)st), -E_LOOP);
    expect("access loop", syscall2(SYS_access, (int)a, 0), -E_LOOP);
    expect("open self loop", syscall3(SYS_open, (int)self, O_RDONLY, 0), -E_LOOP);
    expect("open through loop", syscall3(SYS_open, (int)"/tmp/smp_loopA/x", O_RDONLY, 0),
           -E_LOOP);
    /* lstat does not follow: it must see the link itself. */
    int r = syscall2(SYS_lstat64, (int)a, (int)st);
    unsigned mode;
    memcpy(&mode, st + 16, 4);                      /* i386 stat64.st_mode */
    if (r != 0 || (mode & 0170000) != 0120000)
        FAILF("lstat64 loop link: r=%d mode=%o\n", r, mode);

    syscall1(SYS_unlink, (int)a);
    syscall1(SYS_unlink, (int)b);
    syscall1(SYS_unlink, (int)self);
}

/* ── 3. offsets ──────────────────────────────────────────────────────────── */

static void test_offsets(void) {
    const char *path = "/tmp/smp_seek";
    int fd = syscall3(SYS_open, (int)path, O_RDWR | O_CREAT, 0644);
    if (fd < 0) { FAILF("open %s: %d\n", path, fd); return; }
    syscall3(SYS_write, fd, (int)"0123456789", 10);
    syscall3(SYS_lseek, fd, 0, 0);

    expect("lseek -1 SET", syscall3(SYS_lseek, fd, -1, 0), -E_INVAL);
    expect("pos after SET", syscall3(SYS_lseek, fd, 0, 1), 0);
    expect("lseek -11 END", syscall3(SYS_lseek, fd, -11, 2), -E_INVAL);
    expect("lseek -1 CUR", syscall3(SYS_lseek, fd, -1, 1), -E_INVAL);
    expect("pos after CUR", syscall3(SYS_lseek, fd, 0, 1), 0);
    expect("lseek 5", syscall3(SYS_lseek, fd, 5, 0), 5);

    unsigned long long res = 0;
    expect("_llseek -1", sc5(SYS__llseek, fd, -1, -1, (int)&res, 0), -E_INVAL);
    expect("pos after _llseek", syscall3(SYS_lseek, fd, 0, 1), 5);
    expect("_llseek 7", sc5(SYS__llseek, fd, 0, 7, (int)&res, 0), 0);
    if (res != 7) FAILF("_llseek result %u\n", (unsigned)res);

    expect("ftruncate -1", syscall2(SYS_ftruncate, fd, -1), -E_INVAL);
    expect("pwrite64 >4GiB", sc5(SYS_pwrite64, fd, (int)"x", 1, 0, 1), -E_FBIG);
    /* tmpfs caps a file at 1 GiB: a write at the cap is EFBIG.  Bodies are
     * page frames, not kernel heap, so the old 256 MiB cap (the heap window)
     * is gone, and a sparse write there costs one frame. */
    expect("pwrite64 past old cap", sc5(SYS_pwrite64, fd, (int)"x", 1, 0x10000000, 0), 1);
    expect("pwrite64 tmpfs cap", sc5(SYS_pwrite64, fd, (int)"x", 1, 0x40000000, 0),
           -E_FBIG);

    syscall1(SYS_close, fd);
    syscall1(SYS_unlink, (int)path);
}

/* ── 4. getdents layouts ─────────────────────────────────────────────────── */

#define GD_DIR "/tmp/smp_gd"
#define GD_N   20

static char gd_names[GD_N][GD_N + 1];
static int  gd_seen[GD_N];

static void gd_note(const char *how, const char *name) {
    if (!strcmp(name, ".") || !strcmp(name, "..")) return;
    for (int i = 0; i < GD_N; i++)
        if (!strcmp(name, gd_names[i])) { gd_seen[i]++; return; }
    FAILF("%s: unexpected name '%s'\n", how, name);
}

static void gd_check(const char *how) {
    for (int i = 0; i < GD_N; i++) {
        if (gd_seen[i] != 1) FAILF("%s: name of length %d seen %d times\n",
                                   how, i + 1, gd_seen[i]);
        gd_seen[i] = 0;
    }
}

/* One pass of raw getdents64 with a `bufsz`-byte buffer. */
static void gd_pass64(int bufsz) {
    static char buf[4096];
    char how[32];
    snprintf(how, sizeof(how), "getdents64/%d", bufsz);
    int fd = syscall3(SYS_open, (int)GD_DIR, O_RDONLY, 0);
    if (fd < 0) { FAILF("%s: open %d\n", how, fd); return; }
    for (;;) {
        int n = syscall3(SYS_getdents64, fd, (int)buf, bufsz);
        if (n < 0) { FAILF("%s: %d\n", how, n); break; }
        if (n == 0) break;
        for (int off = 0; off < n; ) {
            unsigned short reclen;
            memcpy(&reclen, buf + off + 16, 2);
            unsigned char type = (unsigned char)buf[off + 18];
            const char *nm = buf + off + 19;
            int nlen = bounded_len(nm, reclen > 19 ? reclen - 19 : 0);
            int want = (19 + nlen + 1 + 7) & ~7;
            if (reclen != want || off + reclen > n || nm[nlen] != '\0')
                FAILF("%s: '%s' reclen %u want %d\n", how, nm, reclen, want);
            if (reclen == 0) { off = n; break; }
            if (strcmp(nm, ".") && strcmp(nm, "..") && type != DT_REG)
                FAILF("%s: '%s' d_type %u\n", how, nm, type);
            gd_note(how, nm);
            off += reclen;
        }
    }
    syscall1(SYS_close, fd);
    gd_check(how);
}

static void test_getdents(void) {
    static char path[64], buf[4096];
    syscall2(SYS_mkdir, (int)GD_DIR, 0755);
    for (int i = 0; i < GD_N; i++) {
        fill(gd_names[i], (char)('a' + i), i + 1);
        snprintf(path, sizeof(path), "%s/%s", GD_DIR, gd_names[i]);
        int fd = syscall3(SYS_open, (int)path, O_WRONLY | O_CREAT, 0644);
        if (fd < 0) FAILF("create %s: %d\n", path, fd);
        else syscall1(SYS_close, fd);
    }

    /* The libc readdir (getdents64 into a 4 KiB buffer, like musl's). */
    DIR *d = opendir(GD_DIR);
    struct dirent *e;
    if (!d) FAILF("opendir\n");
    while (d && (e = readdir(d))) gd_note("readdir", e->d_name);
    if (d) closedir(d);
    gd_check("readdir");

    gd_pass64(4096);
    gd_pass64(48);        /* fits one 40-byte record: exercises every boundary */

    int fd = syscall3(SYS_open, (int)GD_DIR, O_RDONLY, 0);
    expect("getdents64 tiny buffer", syscall3(SYS_getdents64, fd, (int)buf, 16), -E_INVAL);
    syscall1(SYS_close, fd);

    /* Legacy getdents: ino(4) off(4) reclen(2) name NUL ... type(last byte). */
    fd = syscall3(SYS_open, (int)GD_DIR, O_RDONLY, 0);
    for (;;) {
        int n = syscall3(SYS_getdents, fd, (int)buf, sizeof(buf));
        if (n < 0) { FAILF("getdents: %d\n", n); break; }
        if (n == 0) break;
        for (int off = 0; off < n; ) {
            unsigned short reclen;
            memcpy(&reclen, buf + off + 8, 2);
            const char *nm = buf + off + 10;
            int nlen = bounded_len(nm, reclen > 10 ? reclen - 10 : 0);
            int want = (10 + nlen + 2 + 3) & ~3;
            unsigned char type = (unsigned char)buf[off + reclen - 1];
            if (reclen != want || off + reclen > n || nm[nlen] != '\0')
                FAILF("getdents: '%s' reclen %u want %d\n", nm, reclen, want);
            if (reclen == 0) break;
            if (strcmp(nm, ".") && strcmp(nm, "..") && type != DT_REG)
                FAILF("getdents: '%s' d_type %u\n", nm, type);
            gd_note("getdents", nm);
            off += reclen;
        }
    }
    syscall1(SYS_close, fd);
    gd_check("getdents");

    for (int i = 0; i < GD_N; i++) {
        snprintf(path, sizeof(path), "%s/%s", GD_DIR, gd_names[i]);
        syscall1(SYS_unlink, (int)path);
    }
    syscall1(SYS_rmdir, (int)GD_DIR);
}

/* ── 5. waitpid(WUNTRACED) ───────────────────────────────────────────────── */

static void test_wuntraced(void) {
    int pid = syscall0(SYS_fork);
    if (pid < 0) { FAILF("fork %d\n", pid); return; }
    if (pid == 0) {
        syscall2(SYS_kill, syscall0(SYS_getpid), 19);   /* SIGSTOP */
        syscall1(SYS_exit, 7);
    }
    int st = 0;
    expect("waitpid stopped", syscall3(SYS_waitpid, pid, (int)&st, 2), pid);
    if (st != ((19 << 8) | 0x7f)) FAILF("stop status %#x\n", st);
    expect("waitpid stop again", syscall3(SYS_waitpid, pid, (int)&st, 2 | 1), 0);
    syscall2(SYS_kill, pid, 18);                        /* SIGCONT */
    expect("waitpid exit", syscall3(SYS_waitpid, pid, (int)&st, 0), pid);
    if (st != (7 << 8)) FAILF("exit status %#x\n", st);
}

int main(void) {
    test_long_paths();
    test_loops();
    test_offsets();
    test_getdents();
    test_wuntraced();
    if (fails) {
        printf("sysmiscprobe: %d failure(s)\n", fails);
        return 1;
    }
    printf("sysmiscprobe ok\n");
    return 0;
}
