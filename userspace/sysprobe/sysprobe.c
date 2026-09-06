#include "../include/errno.h"
#include "../include/fcntl.h"
#include "../include/stdio.h"
#include "../include/string.h"
#include "../include/syscall.h"
#include "../include/sys/stat.h"
#include "../include/unistd.h"

#define AT_FDCWD (-100)

static int expect_efault(const char *name, int ret) {
    if (ret == -1 && errno == 14)
        return 0;
    printf("sysprobe: %s failed ret=%d errno=%d\n", name, ret, errno);
    return 1;
}

static int expect_raw(const char *name, int got, int want) {
    if (got == want)
        return 0;
    printf("sysprobe: %s failed got=%d want=%d\n", name, got, want);
    return 1;
}

int main(void) {
    int failed = 0;
    char *bad = (char *)0x40000000;
    int p[2];
    char ch = 0;

    failed |= expect_efault("write bad buf", write(1, bad, 4));
    failed |= expect_efault("open bad path", open(bad, O_RDONLY));
    failed |= expect_raw("pipe bad fds", syscall1(42, (int)bad), -14);
    failed |= expect_raw("uname bad buf", syscall1(122, (int)bad), -14);
    failed |= expect_raw("getcwd bad buf", syscall2(183, (int)bad, 16), -14);
    failed |= expect_raw("poll bad fds", syscall3(168, (int)bad, 1, 0), -14);
    failed |= expect_raw("writev bad iov", syscall3(146, 1, (int)bad, 1), -14);
    failed |= expect_raw("access invalid mode",
                         syscall2(33, (int)"/", 8), -22);
    failed |= expect_raw("setrlimit bad ptr",
                         syscall2(75, 0, (int)bad), -14);
    failed |= expect_raw("mprotect unaligned",
                         syscall3(125, 0x08048001, 4096, 1), -22);
    failed |= expect_raw("madvise bad range",
                         syscall3(219, (int)bad, 4096, 0), -12);
    failed |= expect_raw("flock bad fd", syscall2(143, -1, 2), -9);
    if (syscall1(45, 0xBFFC0000) != -12) {
        printf("sysprobe: brk stack boundary failed\n");
        failed = 1;
    }

    int rootfd = open("/", O_RDONLY);
    if (rootfd >= 0) {
        failed |= expect_raw("getdents64 bad buf",
                             syscall3(220, rootfd, (int)bad, 128), -14);
        close(rootfd);
    } else {
        printf("sysprobe: open root failed errno=%d\n", errno);
        failed = 1;
    }

    if (pipe(p) < 0) {
        printf("sysprobe: pipe failed errno=%d\n", errno);
        failed = 1;
    } else {
        int w2 = dup(p[1]);
        if (w2 < 0) {
            printf("sysprobe: dup pipe write failed errno=%d\n", errno);
            failed = 1;
        } else {
            close(p[1]);
            if (write(w2, "x", 1) != 1) {
                printf("sysprobe: write duplicated pipe failed errno=%d\n", errno);
                failed = 1;
            }
            close(w2);
            if (read(p[0], &ch, 1) != 1 || ch != 'x') {
                printf("sysprobe: read duplicated pipe data failed errno=%d ch=%c\n",
                       errno, ch);
                failed = 1;
            }
            if (read(p[0], &ch, 1) != 0) {
                printf("sysprobe: pipe eof failed errno=%d\n", errno);
                failed = 1;
            }
        }
        close(p[0]);
    }

    failed |= expect_raw("openat bad path",
                         syscall3(295, AT_FDCWD, (int)bad, O_RDONLY), -14);
    failed |= expect_raw("chmod bad path", syscall2(15, (int)bad, 0644), -14);

    int tmpfd = open("/tmp", O_RDONLY);
    if (tmpfd < 0) {
        printf("sysprobe: open /tmp failed errno=%d\n", errno);
        failed = 1;
    } else {
        syscall3(301, tmpfd, (int)"spdir/file", 0);
        syscall3(301, tmpfd, (int)"spdir", 0x200);

        int r = syscall3(296, tmpfd, (int)"spdir", 0755);
        failed |= expect_raw("mkdirat relative", r, 0);

        int fd = syscall3(295, tmpfd, (int)"spdir/file", O_CREAT | O_RDWR);
        if (fd < 0) {
            printf("sysprobe: openat relative create failed ret=%d\n", fd);
            failed = 1;
        } else {
            if (write(fd, "ok", 2) != 2) {
                printf("sysprobe: openat write failed errno=%d\n", errno);
                failed = 1;
            }
            close(fd);
        }

        struct stat st;
        r = syscall4(300, tmpfd, (int)"spdir/file", (int)&st, 0);
        failed |= expect_raw("fstatat64 relative", r, 0);

        r = syscall4(307, tmpfd, (int)"spdir/file", 0, 0);
        failed |= expect_raw("faccessat relative", r, 0);

        /* fchmodat is now implemented (Phase 24): root chmods its own file. */
        r = syscall4(306, tmpfd, (int)"spdir/file", 0600, 0);
        failed |= expect_raw("fchmodat relative", r, 0);

        int flags = fcntl(tmpfd, F_GETFL);
        if (flags < 0) {
            printf("sysprobe: fcntl F_GETFL failed errno=%d\n", errno);
            failed = 1;
        } else if (fcntl(tmpfd, F_SETFL, flags | O_NONBLOCK) < 0) {
            printf("sysprobe: fcntl F_SETFL failed errno=%d\n", errno);
            failed = 1;
        } else {
            int newflags = fcntl(tmpfd, F_GETFL);
            if ((newflags & O_NONBLOCK) == 0) {
                printf("sysprobe: fcntl F_SETFL did not persist flags=%d\n",
                       newflags);
                failed = 1;
            }
        }
        struct flock fl;
        memset(&fl, 0, sizeof(fl));
        fl.l_type = F_WRLCK;
        r = fcntl(tmpfd, F_SETLK, &fl);
        if (r < 0) {
            printf("sysprobe: fcntl F_SETLK failed errno=%d\n", errno);
            failed = 1;
        }
        failed |= expect_raw("fcntl bad lock ptr",
                             syscall3(55, tmpfd, F_SETLK, (int)bad), -14);

        r = syscall4(302, tmpfd, (int)"spdir/file", tmpfd, (int)"spdir/renamed");
        failed |= expect_raw("renameat relative", r, 0);

        r = syscall3(295, tmpfd, (int)"spdir/renamed", O_RDONLY);
        if (r < 0) {
            printf("sysprobe: openat renamed failed ret=%d\n", r);
            failed = 1;
        } else {
            close(r);
        }

        symlink("target", "/tmp/splink");
        char linkbuf[16];
        r = syscall4(305, tmpfd, (int)"splink", (int)linkbuf, sizeof(linkbuf));
        if (r != 6 || memcmp(linkbuf, "target", 6) != 0) {
            printf("sysprobe: readlinkat failed ret=%d errno=%d\n", r, errno);
            failed = 1;
        }

        syscall3(301, tmpfd, (int)"splink", 0);
        syscall3(301, tmpfd, (int)"spdir/renamed", 0);
        syscall3(301, tmpfd, (int)"spdir", 0x200);
        close(tmpfd);
    }

    if (failed)
        return 1;

    printf("sysprobe ok\n");
    return 0;
}
