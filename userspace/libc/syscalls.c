#include "../include/syscall.h"
#include "../include/unistd.h"
#include "../include/fcntl.h"
/* struct timeval defined in unistd.h */
#include "../include/signal.h"
#include "../include/sys/stat.h"
#include "../include/sys/utsname.h"
#include <stddef.h>
#include <stdint.h>

/* errno — set to positive error code on syscall failure */
int errno = 0;

/* Convert a negative kernel return value to -1 and set errno. */
static inline int __chkerr(int ret) {
    if (ret < 0) { errno = -ret; return -1; }
    errno = 0;
    return ret;
}

int read(int fd, void *buf, int n) {
    return __chkerr(syscall3(3, fd, (int)buf, n));
}

int write(int fd, const void *buf, int n) {
    return __chkerr(syscall3(4, fd, (int)buf, n));
}

int open(const char *path, int flags, ...) {
    return __chkerr(syscall2(5, (int)path, flags));
}

int close(int fd) {
    return __chkerr(syscall1(6, fd));
}

int waitpid(int pid, int *status, int options) {
    return __chkerr(syscall3(7, pid, (int)status, options));
}

int execve(const char *path, char *const argv[], char *const envp[]) {
    return __chkerr(syscall3(11, (int)path, (int)argv, (int)envp));
}

int chdir(const char *path) {
    return __chkerr(syscall1(12, (int)path));
}

int lseek(int fd, int offset, int whence) {
    return __chkerr(syscall3(19, fd, offset, whence));
}

int getpid(void) {
    return syscall0(20);
}

int access(const char *path, int mode) {
    return __chkerr(syscall2(33, (int)path, mode));
}

void *brk(void *addr) {
    return (void *)syscall1(45, (int)addr);
}

int fork(void) {
    return __chkerr(syscall0(2));
}

void exit(int status) {
    syscall1(252, status);   /* exit_group */
    syscall1(1,   status);   /* exit fallback */
    for (;;);
}

int kill(int pid, int sig) {
    return __chkerr(syscall2(37, pid, sig));
}

int pipe(int fd[2]) {
    return __chkerr(syscall1(42, (int)fd));
}

sighandler_t signal(int signum, sighandler_t handler) {
    int ret = syscall2(48, signum, (int)handler);
    if (ret < 0) { errno = -ret; return (sighandler_t)-1; }
    return (sighandler_t)(uintptr_t)ret;
}

int dup2(int oldfd, int newfd) {
    return __chkerr(syscall2(63, oldfd, newfd));
}

int stat(const char *path, struct stat *buf) {
    return __chkerr(syscall2(195, (int)path, (int)buf));
}

int fstat(int fd, struct stat *buf) {
    return __chkerr(syscall2(197, fd, (int)buf));
}

int lstat(const char *path, struct stat *buf) {
    return __chkerr(syscall2(196, (int)path, (int)buf));
}

int getdents64(int fd, void *buf, int count) {
    return __chkerr(syscall3(220, fd, (int)buf, count));
}

int getcwd_syscall(char *buf, int size) {
    return __chkerr(syscall2(183, (int)buf, size));
}

int mkdir(const char *path, int mode) {
    return __chkerr(syscall2(39, (int)path, mode));
}

int unlink(const char *path) {
    return __chkerr(syscall1(10, (int)path));
}

int dup(int fd) {
    return __chkerr(syscall1(41, fd));
}

int truncate(const char *path, int length) {
    return __chkerr(syscall2(92, (int)path, length));
}

int ftruncate(int fd, int length) {
    return __chkerr(syscall2(93, fd, length));
}

int getppid(void) {
    return syscall0(64);
}

int fcntl(int fd, int cmd, ...) {
    int arg = 0;
    if (cmd == F_SETFD || cmd == F_SETFL || cmd == F_DUPFD ||
        cmd == F_DUPFD_CLOEXEC || cmd == F_GETLK || cmd == F_SETLK ||
        cmd == F_SETLKW) {
        __builtin_va_list ap;
        __builtin_va_start(ap, cmd);
        arg = __builtin_va_arg(ap, int);
        __builtin_va_end(ap);
    }
    return __chkerr(syscall3(55, fd, cmd, arg));
}

int usleep(unsigned int usec) {
    struct timespec ts = { (long)(usec / 1000000U),
                           (long)(usec % 1000000U) * 1000L };
    return nanosleep(&ts, 0);
}

int sched_yield(void) {
    return syscall0(159);
}

int setpgid(int pid, int pgid) {
    return __chkerr(syscall2(57, pid, pgid));
}

int getpgrp(void) {
    return syscall0(65);
}

int setsid(void) {
    return __chkerr(syscall0(66));
}

int uname(struct utsname *buf) {
    return __chkerr(syscall1(122, (int)buf));
}

int openat(int dirfd, const char *path, int flags) {
    return __chkerr(syscall3(295, dirfd, (int)path, flags));
}

int nanosleep(const struct timespec *req, struct timespec *rem) {
    return __chkerr(syscall2(162, (int)req, (int)rem));
}

int ioctl(int fd, unsigned long req, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, req);
    void *arg = __builtin_va_arg(ap, void *);
    __builtin_va_end(ap);
    return __chkerr(syscall3(54, fd, (int)req, (int)arg));
}

int rename(const char *old, const char *new) {
    return __chkerr(syscall2(38, (int)old, (int)new));
}

int gettimeofday(struct timeval *tv, void *tz) {
    return __chkerr(syscall2(78, (int)tv, (int)tz));
}

int getrandom(void *buf, unsigned int buflen, unsigned int flags) {
    return __chkerr(syscall3(355, (int)buf, (int)buflen, (int)flags));
}

int getentropy(void *buf, size_t buflen) {
    if (buflen > 256) {
        errno = 5;
        return -1;
    }
    return getrandom(buf, (unsigned int)buflen, 0) == (int)buflen ? 0 : -1;
}

int symlink(const char *target, const char *path) {
    return __chkerr(syscall2(83, (int)target, (int)path));
}

int readlink(const char *path, char *buf, int bufsiz) {
    return __chkerr(syscall3(85, (int)path, (int)buf, bufsiz));
}

int mknod(const char *path, int mode, int dev) {
    return __chkerr(syscall3(14, (int)path, mode, dev));
}

int getuid(void)  { return syscall0(24); }
int getgid(void)  { return syscall0(47); }
int geteuid(void) { return syscall0(49); }
int getegid(void) { return syscall0(50); }

int mkfifo(const char *path, int mode) {
    return mknod(path, 0x1000 | (mode & 0777), 0);
}
