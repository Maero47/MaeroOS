/*
 * P55 Old fstat on any descriptor; nothing maps below 64 KiB.
 *
 * The i386 fstat (syscall 108, struct stat with 16-bit ids) works on every
 * kind of descriptor: an eventfd, an epoll instance, a socket, a pipe.  And
 * an unprivileged MAP_FIXED mapping at address 0 (or anywhere under
 * vm.mmap_min_addr, 64 KiB here) is refused.
 *
 * MaeroOS before: syscall 108 read the vfs node of descriptors that have
 * none (a kernel panic), and MAP_FIXED at 0 succeeded.
 */
#define PROBE_NAME "p55_fstat_low_map"
#include "probe.h"
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>

/* struct stat as syscall 108 fills it on i386. */
struct old_kstat {
    unsigned short st_dev, pad1;
    unsigned long st_ino;
    unsigned short st_mode, st_nlink, st_uid, st_gid, st_rdev, pad2;
    unsigned long st_size, st_blksize, st_blocks;
    unsigned long times[6];
    unsigned long unused[2];
};

static void old_fstat(int fd, const char *what)
{
    struct old_kstat st;
    long r = syscall(108, fd, &st);
    if (r != 0) probe_fail("fstat (108) of %s: %s", what, strerror(errno));
}

int main(void)
{
    probe_watchdog(60);
    int efd = eventfd(0, 0), ep = epoll_create1(0);
    int sk = socket(AF_UNIX, SOCK_STREAM, 0), p[2];
    if (efd < 0 || ep < 0 || sk < 0 || pipe(p) != 0)
        probe_fail("creating descriptors: %s", strerror(errno));
    old_fstat(efd, "an eventfd");
    old_fstat(ep, "an epoll instance");
    old_fstat(sk, "a socket");
    old_fstat(p[0], "a pipe");

    /* As an unprivileged user (Linux lets CAP_SYS_RAWIO map low). */
    pid_t c = fork();
    if (c < 0) probe_fail("fork: %s", strerror(errno));
    if (c == 0) {
        if (geteuid() == 0 && (setgid(65534) != 0 || setuid(65534) != 0)) _exit(4);
        unsigned long lows[] = { 0, 0x1000, 0xF000 };
        for (unsigned i = 0; i < sizeof lows / sizeof lows[0]; i++) {
            void *m = mmap((void *)lows[i], 4096, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
            if (m != MAP_FAILED) _exit(10 + (int)i);
        }
        void *m = mmap((void *)0x10000, 4096, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        _exit(m == MAP_FAILED ? 5 : 0);
    }
    int st;
    if (waitpid(c, &st, 0) != c) probe_fail("waitpid: %s", strerror(errno));
    if (!WIFEXITED(st)) probe_fail("mapping child died: status %#x", st);
    switch (WEXITSTATUS(st)) {
    case 0: break;
    case 4: probe_fail("setuid(65534) failed");
    case 5: probe_fail("MAP_FIXED at 64 KiB (the floor itself) was refused");
    default: probe_fail("MAP_FIXED below 64 KiB succeeded (case %d)", WEXITSTATUS(st) - 10);
    }
    probe_pass();
}
