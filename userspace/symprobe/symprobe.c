/*
 * symprobe — ext2 symlink regression probe (run on /disk).
 *
 *   - fast (target < 60 bytes, kept in i_block) and slow (target in a data
 *     block) symlinks: readlink returns the target, open follows it, lstat
 *     reports S_IFLNK with st_size == target length, stat the target;
 *   - relative and absolute targets, a link in the middle of a path, a
 *     dangling link, a two-link loop (ELOOP), EEXIST, ENAMETOOLONG;
 *   - getdents reports DT_LNK;
 *   - unlink removes the link, not the target, and gives a slow link's block
 *     back (statfs free-block count);
 *   - leaves /disk/sp-keep-fast and /disk/sp-keep-slow behind so the host can
 *     check them with debugfs after the guest shuts down.
 *
 * Prints "symprobe ok" when everything holds, "symprobe FAIL: ..." otherwise.
 */
#include "../include/dirent.h"
#include "../include/errno.h"
#include "../include/fcntl.h"
#include "../include/stdio.h"
#include "../include/string.h"
#include "../include/syscall.h"
#include "../include/sys/stat.h"
#include "../include/unistd.h"

#define DIR_  "/disk/sp"
#define TGT   DIR_ "/target.txt"
#define BODY  "symtarget\n"

static int fails;

static void fail(const char *what) {
    printf("symprobe FAIL: %s (errno %d)\n", what, errno);
    fails++;
}

static int write_file(const char *path, const char *text) {
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) return -1;
    int n = (int)strlen(text);
    int w = write(fd, text, n);
    close(fd);
    return w == n ? 0 : -1;
}

/* open `path` (following links) and compare its contents with `want`. */
static int reads_as(const char *path, const char *want) {
    char buf[64];
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n < 0) return 0;
    buf[n] = '\0';
    return strcmp(buf, want) == 0;
}

/* readlink(path) == target, lstat is S_IFLNK of the target's length, and
 * opening it reads `body` (NULL: a dangling link, stat must fail ENOENT). */
static void check_link(const char *what, const char *path, const char *target,
                       const char *body) {
    char buf[512];
    char msg[128];
    int tlen = (int)strlen(target);

    int n = readlink(path, buf, sizeof(buf));
    if (n != tlen || memcmp(buf, target, (unsigned)tlen) != 0) {
        snprintf(msg, sizeof(msg), "%s: readlink (%d)", what, n);
        fail(msg);
    }
    struct stat st;
    if (lstat(path, &st) < 0) {
        snprintf(msg, sizeof(msg), "%s: lstat", what);
        fail(msg);
    } else {
        if (!S_ISLNK(st.st_mode)) {
            snprintf(msg, sizeof(msg), "%s: lstat mode %o", what, st.st_mode);
            fail(msg);
        }
        if (st.st_size != tlen) {
            snprintf(msg, sizeof(msg), "%s: lstat size %d != %d", what,
                     (int)st.st_size, tlen);
            fail(msg);
        }
    }
    if (body) {
        if (!reads_as(path, body)) {
            snprintf(msg, sizeof(msg), "%s: open through link", what);
            fail(msg);
        }
        if (stat(path, &st) < 0 || !S_ISREG(st.st_mode) ||
            st.st_size != (long long)strlen(body)) {
            snprintf(msg, sizeof(msg), "%s: stat through link", what);
            fail(msg);
        }
    } else {
        errno = 0;
        if (stat(path, &st) == 0 || errno != ENOENT) {
            snprintf(msg, sizeof(msg), "%s: dangling stat", what);
            fail(msg);
        }
    }
}

/* statfs64 (268): f_type, f_bsize, then 64-bit f_blocks, f_bfree, ... */
static long free_blocks(void) {
    struct {
        unsigned int f_type, f_bsize;
        unsigned long long f_blocks, f_bfree, f_bavail, f_files, f_ffree;
        unsigned int f_fsid[2], f_namelen, f_frsize, f_flags, f_spare[4];
    } sf;
    if (syscall3(268, (int)"/disk", (int)sizeof(sf), (int)&sf) < 0) return -1;
    return (long)sf.f_bfree;
}

static const char *names[] = {
    "fast", "fastabs", "slow", "slowrel", "dangle", "loopa", "loopb",
    "toolong", "blockfree", "target.txt",
};

static void cleanup(void) {
    char p[128];
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        snprintf(p, sizeof(p), DIR_ "/%s", names[i]);
        unlink(p);
    }
    unlink("/disk/spdir");
    rmdir(DIR_);
}

int main(void) {
    char slow[256], slowrel[256], p[128];

    cleanup();
    if (mkdir(DIR_, 0755) < 0) { fail("mkdir " DIR_); return 1; }
    if (write_file(TGT, BODY) < 0) { fail("write target"); return 1; }

    /* Fast symlinks: relative and absolute. */
    if (symlink("target.txt", DIR_ "/fast") < 0) fail("symlink fast");
    check_link("fast", DIR_ "/fast", "target.txt", BODY);
    if (symlink(TGT, DIR_ "/fastabs") < 0) fail("symlink fastabs");
    check_link("fastabs", DIR_ "/fastabs", TGT, BODY);

    /* Slow symlinks (>= 60 bytes): absolute and relative. */
    strcpy(slow, DIR_);
    for (int i = 0; i < 60; i++) strcat(slow, "/.");
    strcat(slow, "/target.txt");
    if (symlink(slow, DIR_ "/slow") < 0) fail("symlink slow");
    check_link("slow", DIR_ "/slow", slow, BODY);
    slowrel[0] = '\0';
    for (int i = 0; i < 50; i++) strcat(slowrel, "./");
    strcat(slowrel, "target.txt");
    if (symlink(slowrel, DIR_ "/slowrel") < 0) fail("symlink slowrel");
    check_link("slowrel", DIR_ "/slowrel", slowrel, BODY);

    /* A link to a directory, crossed in the middle of a path. */
    if (symlink("sp", "/disk/spdir") < 0) fail("symlink spdir");
    if (!reads_as("/disk/spdir/fast", BODY)) fail("open through dir link");

    /* Dangling link. */
    if (symlink("nowhere", DIR_ "/dangle") < 0) fail("symlink dangle");
    check_link("dangle", DIR_ "/dangle", "nowhere", NULL);

    /* A loop fails with ELOOP. */
    if (symlink("loopb", DIR_ "/loopa") < 0) fail("symlink loopa");
    if (symlink("loopa", DIR_ "/loopb") < 0) fail("symlink loopb");
    errno = 0;
    int fd = open(DIR_ "/loopa", O_RDONLY);
    if (fd >= 0 || errno != ELOOP) {
        fail("loop open not ELOOP");
        if (fd >= 0) close(fd);
    }

    /* An existing name is EEXIST; a target of a block or more ENAMETOOLONG. */
    errno = 0;
    if (symlink("x", DIR_ "/fast") == 0 || errno != EEXIST)
        fail("symlink over existing not EEXIST");
    if (!reads_as(DIR_ "/fast", BODY)) fail("fast changed by EEXIST");
    {
        static char huge[1100];
        memset(huge, 'a', sizeof(huge) - 1);
        huge[sizeof(huge) - 1] = '\0';
        errno = 0;
        if (symlink(huge, DIR_ "/toolong") == 0 || errno != 36 /* ENAMETOOLONG */)
            fail("huge target not ENAMETOOLONG");
    }

    /* getdents: DT_LNK for a link, DT_REG for the target. */
    {
        int saw_lnk = 0, saw_reg = 0;
        DIR *d = opendir(DIR_);
        if (!d) fail("opendir");
        else {
            struct dirent *de;
            while ((de = readdir(d)) != NULL) {
                if (strcmp(de->d_name, "slow") == 0 && de->d_type == DT_LNK) saw_lnk = 1;
                if (strcmp(de->d_name, "target.txt") == 0 && de->d_type == DT_REG) saw_reg = 1;
            }
            closedir(d);
            if (!saw_lnk) fail("getdents d_type of link");
            if (!saw_reg) fail("getdents d_type of file");
        }
    }

    /* A slow link holds exactly one block, and unlink gives it back; a fast
     * one holds none. */
    long b0 = free_blocks();
    if (symlink(slow, DIR_ "/blockfree") < 0) fail("symlink blockfree");
    long b1 = free_blocks();
    unlink(DIR_ "/blockfree");
    long b2 = free_blocks();
    if (b0 < 0 || b1 != b0 - 1 || b2 != b0) {
        snprintf(p, sizeof(p), "slow link blocks %ld/%ld/%ld", b0, b1, b2);
        fail(p);
    }
    if (symlink("target.txt", DIR_ "/blockfree") < 0) fail("symlink fast blockfree");
    if (free_blocks() != b0) fail("fast link allocated a block");
    unlink(DIR_ "/blockfree");

    /* unlink removes the link, never the target. */
    if (unlink(DIR_ "/fast") < 0) fail("unlink fast");
    if (unlink(DIR_ "/slow") < 0) fail("unlink slow");
    errno = 0;
    if (readlink(DIR_ "/fast", p, sizeof(p)) >= 0 || errno != ENOENT)
        fail("fast link survived unlink");
    errno = 0;
    struct stat st;
    if (lstat(DIR_ "/slow", &st) == 0 || errno != ENOENT)
        fail("slow link survived unlink");
    if (!reads_as(TGT, BODY)) fail("target lost with its link");
    if (!reads_as(DIR_ "/fastabs", BODY)) fail("other link lost");
    /* A link to a directory is unlinked as a link, not rmdir'ed. */
    if (unlink("/disk/spdir") < 0) fail("unlink dir link");
    if (!reads_as(TGT, BODY)) fail("dir gone with its link");

    /* Links that must survive a reboot (the smoke checks them on the host). */
    unlink("/disk/sp-keep-fast");
    unlink("/disk/sp-keep-slow");
    if (symlink("hello.txt", "/disk/sp-keep-fast") < 0) fail("symlink keep-fast");
    char keep[256];
    strcpy(keep, "/disk");
    for (int i = 0; i < 40; i++) strcat(keep, "/.");
    strcat(keep, "/hello.txt");
    if (symlink(keep, "/disk/sp-keep-slow") < 0) fail("symlink keep-slow");
    {
        char got[256];
        int n = readlink("/disk/sp-keep-slow", got, sizeof(got));
        if (n != (int)strlen(keep) || memcmp(got, keep, (unsigned)n) != 0)
            fail("readlink keep-slow");
        int kfd = open("/disk/sp-keep-slow", O_RDONLY);
        if (kfd < 0) fail("open keep-slow");
        else close(kfd);
    }

    cleanup();
    if (fails) {
        printf("symprobe: %d failure(s)\n", fails);
        return 1;
    }
    puts("symprobe ok");
    return 0;
}
