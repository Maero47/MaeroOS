/* POSIX and Linux interfaces on top of the kernel's i386 system calls:
 * the *at() family, scheduling and resource queries, wait4, sysconf,
 * setjmp/longjmp, a generic syscall(), environment editing, 64-bit and
 * floating-point string conversions, and the string/stdlib extras toybox
 * expects from a C library. */
#include "../include/errno.h"
#include "../include/fcntl.h"
#include "../include/limits.h"
#include "../include/pwd.h"
#include "../include/sched.h"
#include "../include/setjmp.h"
#include "../include/signal.h"
#include "../include/stdint.h"
#include "../include/stdio.h"
#include "../include/stdlib.h"
#include "../include/string.h"
#include "../include/strings.h"
#include "../include/dirent.h"
#include "../include/netdb.h"
#include "../include/wctype.h"
#include "../include/sys/file.h"
#include "../include/sys/mount.h"
#include "../include/sys/statvfs.h"
#include "../include/sys/klog.h"
#include "../include/sys/resource.h"
#include "../include/sys/stat.h"
#include "../include/sys/sysinfo.h"
#include "../include/sys/uio.h"
#include "../include/sys/utsname.h"
#include "../include/sys/wait.h"
#include "../include/syscall.h"
#include "../include/time.h"
#include "../include/unistd.h"

static int chkerr(int ret) {
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    return ret;
}

/* ── syscall(), setjmp/longjmp ──────────────────────────────────────────── */

/* long syscall(long num, a1..a6): the six arguments go in ebx, ecx, edx,
 * esi, edi, ebp; a result in [-4095, -1] is an error number. */
__asm__(
    ".text\n"
    ".globl syscall\n"
    ".type syscall, @function\n"
    "syscall:\n"
    "    push %ebp\n"
    "    push %edi\n"
    "    push %esi\n"
    "    push %ebx\n"
    "    mov 20(%esp), %eax\n"
    "    mov 24(%esp), %ebx\n"
    "    mov 28(%esp), %ecx\n"
    "    mov 32(%esp), %edx\n"
    "    mov 36(%esp), %esi\n"
    "    mov 40(%esp), %edi\n"
    "    mov 44(%esp), %ebp\n"
    "    int $0x80\n"
    "    pop %ebx\n"
    "    pop %esi\n"
    "    pop %edi\n"
    "    pop %ebp\n"
    "    cmp $-4095, %eax\n"
    "    jae 1f\n"
    "    ret\n"
    "1:  neg %eax\n"
    "    mov %eax, errno\n"
    "    mov $-1, %eax\n"
    "    ret\n"
    ".size syscall, .-syscall\n"

/* jmp_buf: [0] ebx [1] esi [2] edi [3] ebp [4] esp [5] eip
 *          [6] mask saved? [7] saved mask (sigsetjmp only) */
    ".globl setjmp\n"
    ".globl _setjmp\n"
    ".type setjmp, @function\n"
    "setjmp:\n"
    "_setjmp:\n"
    "    mov 4(%esp), %eax\n"
    "    mov %ebx, 0(%eax)\n"
    "    mov %esi, 4(%eax)\n"
    "    mov %edi, 8(%eax)\n"
    "    mov %ebp, 12(%eax)\n"
    "    lea 4(%esp), %ecx\n"
    "    mov %ecx, 16(%eax)\n"
    "    mov (%esp), %ecx\n"
    "    mov %ecx, 20(%eax)\n"
    "    xor %eax, %eax\n"
    "    ret\n"
    ".size setjmp, .-setjmp\n"

    ".globl longjmp\n"
    ".globl _longjmp\n"
    ".type longjmp, @function\n"
    "longjmp:\n"
    "_longjmp:\n"
    "    mov 4(%esp), %edx\n"
    "    mov 8(%esp), %eax\n"
    "    test %eax, %eax\n"
    "    jnz 1f\n"
    "    inc %eax\n"
    "1:  mov 0(%edx), %ebx\n"
    "    mov 4(%edx), %esi\n"
    "    mov 8(%edx), %edi\n"
    "    mov 12(%edx), %ebp\n"
    "    mov 16(%edx), %esp\n"
    "    jmp *20(%edx)\n"
    ".size longjmp, .-longjmp\n"

/* sigsetjmp(env, savemask): record the mask, then become setjmp with the
 * caller's own return address still on the stack. */
    ".globl sigsetjmp\n"
    ".globl __sigsetjmp\n"
    ".type sigsetjmp, @function\n"
    "sigsetjmp:\n"
    "__sigsetjmp:\n"
    "    mov 4(%esp), %ecx\n"
    "    mov 8(%esp), %edx\n"
    "    mov %edx, 24(%ecx)\n"
    "    test %edx, %edx\n"
    "    jz 1f\n"
    "    push %ecx\n"
    "    lea 28(%ecx), %eax\n"
    "    push %eax\n"
    "    push $0\n"
    "    push $0\n"
    "    call sigprocmask\n"
    "    add $12, %esp\n"
    "    pop %ecx\n"
    "1:  jmp setjmp\n"
    ".size sigsetjmp, .-sigsetjmp\n"
);

void siglongjmp(sigjmp_buf env, int val) {
    if (env[6]) sigprocmask(SIG_SETMASK, (sigset_t *)&env[7], 0);
    longjmp(env, val);
}

/* ── files and directories ──────────────────────────────────────────────── */

int readv(int fd, const struct iovec *iov, int iovcnt) {
    return chkerr(syscall3(145, fd, (int)iov, iovcnt));
}
int writev(int fd, const struct iovec *iov, int iovcnt) {
    return chkerr(syscall3(146, fd, (int)iov, iovcnt));
}
int lchown(const char *path, int owner, int group) {
    return chkerr(syscall3(198, (int)path, owner, group));   /* lchown32 */
}
int chown(const char *path, int owner, int group) {
    return chkerr(syscall3(212, (int)path, owner, group));   /* chown32 */
}
int fchownat(int dirfd, const char *path, int owner, int group, int flags) {
    return chkerr(syscall5(298, dirfd, (int)path, owner, group, flags));
}
int linkat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath,
           int flags) {
    (void)olddirfd; (void)oldpath; (void)newdirfd; (void)newpath; (void)flags;
    errno = EPERM;          /* no hard links: link(2) is refused the same way */
    return -1;
}
int symlinkat(const char *target, int newdirfd, const char *linkpath) {
    return chkerr(syscall3(304, (int)target, newdirfd, (int)linkpath));
}
int renameat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath) {
    return chkerr(syscall4(302, olddirfd, (int)oldpath, newdirfd, (int)newpath));
}
int mknodat(int dirfd, const char *path, int mode, int dev) {
    return chkerr(syscall4(297, dirfd, (int)path, mode, dev));
}
int fchdir(int fd) { return chkerr(syscall1(133, fd)); }
void sync(void) {}
int syncfs(int fd) { (void)fd; return 0; }
int fdatasync(int fd) { (void)fd; return 0; }
int pipe2(int fd[2], int flags) { return chkerr(syscall2(331, (int)fd, flags)); }
int dup3(int oldfd, int newfd, int flags) {
    return chkerr(syscall3(330, oldfd, newfd, flags));
}
ssize_t pread(int fd, void *buf, size_t count, off_t offset) {
    return chkerr(syscall5(180, fd, (int)buf, (int)count, offset, offset < 0 ? -1 : 0));
}
ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset) {
    return chkerr(syscall5(181, fd, (int)buf, (int)count, offset, offset < 0 ? -1 : 0));
}
int flock(int fd, int operation) { return chkerr(syscall2(143, fd, operation)); }

/* posix_fallocate/posix_fadvise return the error instead of setting errno. */
int posix_fallocate(int fd, off_t offset, off_t len) {
    if (offset < 0 || len <= 0) return EINVAL;
    long r = syscall(324, fd, 0, offset, 0, len, 0);
    return r < 0 ? errno : 0;
}
int posix_fadvise(int fd, off_t offset, off_t len, int advice) {
    (void)fd; (void)offset; (void)len; (void)advice;
    return 0;
}

/* A terminal is a character device that answers TCGETS.  The kernel answers
 * TCGETS for pipes and files too (with the console's settings), so the
 * device type decides. */
int isatty(int fd) {
    char termios[36];
    struct stat st;
    int r = syscall3(54, fd, 0x5401, (int)termios);
    if (r < 0) { errno = r == -EBADF ? EBADF : ENOTTY; return 0; }
    if (fstat(fd, &st)) return 0;
    if (!S_ISCHR(st.st_mode)) { errno = ENOTTY; return 0; }
    return 1;
}

/* Find the /dev (or /dev/pts) entry that is the same device as `fd`. */
static int tty_lookup(const char *dir, const struct stat *want, char *buf, size_t len) {
    DIR *d = opendir(dir);
    struct dirent *de;
    struct stat st;
    int found = 0;

    if (!d) return 0;
    while (!found && (de = readdir(d))) {
        size_t dl = strlen(dir), nl = strlen(de->d_name);
        if (de->d_name[0] == '.' || dl + 1 + nl + 1 > len) continue;
        memcpy(buf, dir, dl);
        buf[dl] = '/';
        memcpy(buf + dl + 1, de->d_name, nl + 1);
        if (!stat(buf, &st) && S_ISCHR(st.st_mode) && st.st_rdev == want->st_rdev &&
            st.st_ino == want->st_ino)
            found = 1;
    }
    closedir(d);
    return found;
}

int ttyname_r(int fd, char *buf, size_t buflen) {
    struct stat st;

    struct stat ln;
    char proc[32];
    int n;

    if (!isatty(fd)) return errno;
    if (fstat(fd, &st) || !S_ISCHR(st.st_mode)) return ENOTTY;
    /* The name the descriptor was opened by, if it still is that device. */
    snprintf(proc, sizeof(proc), "/proc/self/fd/%d", fd);
    n = readlink(proc, buf, (int)buflen - 1);
    if (n > 0 && buf[0] == '/') {
        buf[n] = 0;
        if (!stat(buf, &ln) && S_ISCHR(ln.st_mode) && ln.st_rdev == st.st_rdev)
            return 0;
    }
    if (tty_lookup("/dev/pts", &st, buf, buflen) || tty_lookup("/dev", &st, buf, buflen))
        return 0;
    return ENOTTY;
}

char *ttyname(int fd) {
    static char name[64];
    int r = ttyname_r(fd, name, sizeof(name));
    if (r) { errno = r; return 0; }
    return name;
}

long pathconf(const char *path, int name) {
    (void)path;
    switch (name) {
    case _PC_LINK_MAX:  return 1;
    case _PC_MAX_CANON:
    case _PC_MAX_INPUT: return 255;
    case _PC_NAME_MAX:  return NAME_MAX;
    case _PC_PATH_MAX:  return PATH_MAX;
    case _PC_PIPE_BUF:  return PIPE_BUF;
    }
    errno = EINVAL;
    return -1;
}
long fpathconf(int fd, int name) { (void)fd; return pathconf("/", name); }

char *mkdtemp(char *template) {
    static const char chars[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    size_t len = template ? strlen(template) : 0;
    char *x = template + len - 6;

    if (len < 6 || memcmp(x, "XXXXXX", 6)) { errno = EINVAL; return 0; }
    for (int tries = 0; tries < 100; tries++) {
        unsigned char rnd[6];
        if (getrandom(rnd, sizeof(rnd), 0) != (int)sizeof(rnd)) {
            unsigned v = (unsigned)getpid() * 2654435761u + (unsigned)tries;
            for (int i = 0; i < 6; i++) { rnd[i] = (unsigned char)(v >> 8); v = v * 1103515245u + 12345u; }
        }
        for (int i = 0; i < 6; i++) x[i] = chars[rnd[i] % (sizeof(chars) - 1)];
        if (!mkdir(template, 0700)) return template;
        if (errno != EEXIST) return 0;
    }
    errno = EEXIST;
    return 0;
}

/* ── processes ──────────────────────────────────────────────────────────── */

pid_t getsid(pid_t pid) { return chkerr(syscall1(147, pid)); }
pid_t getpgid(pid_t pid) { return chkerr(syscall1(132, pid)); }
int killpg(int pgrp, int sig) {
    if (pgrp <= 1) { errno = EINVAL; return -1; }
    return kill(-pgrp, sig);
}

/* The kernel has one priority for everyone: getpriority reports nice 0 and
 * setpriority accepts (and ignores) any value. */
int getpriority(int which, unsigned who) {
    int r = syscall2(96, which, (int)who);
    if (r < 0) { errno = -r; return -1; }
    return 20 - r;
}
int setpriority(int which, unsigned who, int prio) {
    return chkerr(syscall3(97, which, (int)who, prio));
}
int nice(int inc) {
    int cur = getpriority(PRIO_PROCESS, 0);
    if (setpriority(PRIO_PROCESS, 0, cur + inc)) return -1;
    return getpriority(PRIO_PROCESS, 0);
}

pid_t wait4(pid_t pid, int *status, int options, struct rusage *usage) {
    if (usage) memset(usage, 0, sizeof(*usage));
    return chkerr(syscall4(114, pid, (int)status, options, (int)usage));
}
pid_t wait3(int *status, int options, struct rusage *usage) {
    return wait4(-1, status, options, usage);
}
pid_t wait(int *status) { return waitpid(-1, status, 0); }

int getrlimit(int resource, struct rlimit *rlim) {
    return chkerr(syscall2(191, resource, (int)rlim));   /* ugetrlimit */
}
int setrlimit(int resource, const struct rlimit *rlim) {
    return chkerr(syscall2(75, resource, (int)rlim));
}
int prlimit(int pid, int resource, const struct rlimit *new_limit,
            struct rlimit *old_limit) {
    /* prlimit64 takes 64-bit limits; RLIM_INFINITY maps both ways. */
    unsigned long long knew[2], kold[2];
    if (new_limit)
        for (int i = 0; i < 2; i++) {
            rlim_t v = i ? new_limit->rlim_max : new_limit->rlim_cur;
            knew[i] = v == RLIM_INFINITY ? ~0ULL : v;
        }
    if (chkerr(syscall4(340, pid, resource, new_limit ? (int)knew : 0,
                        old_limit ? (int)kold : 0)) < 0)
        return -1;
    if (old_limit) {
        old_limit->rlim_cur = kold[0] > 0xffffffffULL ? RLIM_INFINITY : (rlim_t)kold[0];
        old_limit->rlim_max = kold[1] > 0xffffffffULL ? RLIM_INFINITY : (rlim_t)kold[1];
    }
    return 0;
}
int getrusage(int who, struct rusage *usage) {
    (void)who;
    memset(usage, 0, sizeof(*usage));
    return 0;
}

int sched_getaffinity(pid_t pid, size_t size, cpu_set_t *set) {
    memset(set, 0, size);
    int r = syscall3(242, pid, (int)size, (int)set);
    return r < 0 ? chkerr(r) : 0;
}
int sched_setaffinity(pid_t pid, size_t size, const cpu_set_t *set) {
    return chkerr(syscall3(241, pid, (int)size, (int)set));
}
int sched_get_priority_max(int policy) { return chkerr(syscall1(159, policy)); }
int sched_get_priority_min(int policy) { return chkerr(syscall1(160, policy)); }

int get_nprocs(void) {
    cpu_set_t set;
    if (sched_getaffinity(0, sizeof(set), &set)) return 1;
    int n = CPU_COUNT(&set);
    return n ? n : 1;
}
int get_nprocs_conf(void) { return get_nprocs(); }

int sysinfo(struct sysinfo *info) { return chkerr(syscall1(116, (int)info)); }

long sysconf(int name) {
    struct sysinfo si;
    switch (name) {
    case _SC_ARG_MAX:     return ARG_MAX;
    case _SC_CHILD_MAX:   return 256;
    case _SC_CLK_TCK:     return 100;
    case _SC_NGROUPS_MAX: return NGROUPS_MAX;
    case _SC_OPEN_MAX: {
        struct rlimit rl;
        if (!getrlimit(RLIMIT_NOFILE, &rl) && rl.rlim_cur != RLIM_INFINITY)
            return (long)rl.rlim_cur;
        return 256;
    }
    case _SC_PAGESIZE:    return 4096;
    case _SC_LINE_MAX:    return LINE_MAX;
    case _SC_HOST_NAME_MAX:  return HOST_NAME_MAX;
    case _SC_LOGIN_NAME_MAX: return LOGIN_NAME_MAX;
    case _SC_SYMLOOP_MAX: return SYMLOOP_MAX;
    case _SC_NPROCESSORS_CONF:
    case _SC_NPROCESSORS_ONLN: return get_nprocs();
    case _SC_PHYS_PAGES:
    case _SC_AVPHYS_PAGES:
        if (sysinfo(&si)) return -1;
        return (long)(((unsigned long long)(name == _SC_PHYS_PAGES ? si.totalram : si.freeram)
                       * si.mem_unit) / 4096);
    }
    errno = EINVAL;
    return -1;
}
int getpagesize(void) { return 4096; }

unsigned int sleep(unsigned int seconds) {
    struct timespec ts = { (long)seconds, 0 }, rem = { 0, 0 };
    if (nanosleep(&ts, &rem) && errno == EINTR)
        return (unsigned)rem.tv_sec + (rem.tv_nsec ? 1 : 0);
    return 0;
}

/* No interval timers: alarm() cannot schedule SIGALRM. */
unsigned int alarm(unsigned int seconds) { (void)seconds; return 0; }
int pause(void) {
    sigset_t mask;
    sigprocmask(SIG_BLOCK, 0, &mask);
    return sigsuspend(&mask);
}

int gethostname(char *name, size_t len) {
    struct utsname u;
    if (uname(&u)) return -1;
    size_t n = strlen(u.nodename);
    if (n + 1 > len) { errno = ENAMETOOLONG; return -1; }
    memcpy(name, u.nodename, n + 1);
    return 0;
}
int sethostname(const char *name, size_t len) {
    (void)name; (void)len;
    errno = EPERM;
    return -1;
}
long gethostid(void) { return 0x007f0100L; }   /* glibc's value for 127.0.1.1 */

char *getlogin(void) {
    char *s = getenv("LOGNAME");
    if (s) return s;
    struct passwd *pw = getpwuid(getuid());
    return pw ? pw->pw_name : 0;
}

/* The kernel log through /proc/kmsg: SYSLOG_ACTION_READ_ALL (3) and
 * SIZE_BUFFER (10); clearing it (4, 5) is not supported. */
int klogctl(int type, char *buf, int len) {
    if (type == 10) return 65536;
    if (type == 4 || type == 5) { errno = EPERM; return -1; }
    if (type != 2 && type != 3) return 0;
    /* Linux hands out "<pri>[seconds] text" lines; the kernel log here is
     * plain text, so each line gets that prefix (priority 6, no timestamp). */
    static const char prefix[] = "<6>[    0.000000] ";
    char raw[4096];
    int fd = open("/proc/kmsg", O_RDONLY), n = 0, r, bol = 1;
    if (fd < 0) return -1;
    while ((r = read(fd, raw, sizeof(raw))) > 0) {
        for (int i = 0; i < r; i++) {
            if (raw[i] == '\r') continue;
            if (bol) {
                if (n + (int)sizeof(prefix) - 1 >= len) goto full;
                memcpy(buf + n, prefix, sizeof(prefix) - 1);
                n += sizeof(prefix) - 1;
                bol = 0;
            }
            if (n >= len) goto full;
            buf[n++] = raw[i];
            if (raw[i] == '\n') bol = 1;
        }
    }
full:
    close(fd);
    return n;
}

/* ── exec ───────────────────────────────────────────────────────────────── */

int execvp(const char *file, char *const argv[]) {
    if (!*file) { errno = ENOENT; return -1; }
    if (!strchr(file, '/')) {
        const char *path = getenv("PATH");
        size_t fl = strlen(file);
        char buf[PATH_MAX];
        int saw_eacces = 0;

        if (!path) path = "/bin:/usr/bin:/sbin:/usr/sbin";
        while (*path) {
            const char *end = strchr(path, ':');
            size_t dl = end ? (size_t)(end - path) : strlen(path);
            if (dl + fl + 2 <= sizeof(buf)) {
                memcpy(buf, path, dl);
                if (!dl) buf[dl++] = '.';
                buf[dl] = '/';
                memcpy(buf + dl + 1, file, fl + 1);
                execve(buf, argv, environ);
                if (errno == EACCES) saw_eacces = 1;
            }
            path += dl + (end ? 1 : 0);
            if (!end) break;
        }
        /* The kernel resolves bare names itself as a last resort. */
        execve(file, argv, environ);
        if (saw_eacces) errno = EACCES;
        return -1;
    }
    return execve(file, argv, environ);
}

static int execl_common(const char *path, const char *arg, va_list ap, int search) {
    va_list aq;
    int n = 1;
    va_copy(aq, ap);
    while (va_arg(aq, char *)) n++;
    va_end(aq);
    char **argv = malloc((n + 1) * sizeof(char *));
    if (!argv) return -1;
    argv[0] = (char *)arg;
    for (int i = 1; i <= n; i++) argv[i] = va_arg(ap, char *);
    int r = search ? execvp(path, argv) : execve(path, argv, environ);
    free(argv);
    return r;
}
int execl(const char *path, const char *arg, ...) {
    va_list ap;
    va_start(ap, arg);
    int r = execl_common(path, arg, ap, 0);
    va_end(ap);
    return r;
}
int execlp(const char *file, const char *arg, ...) {
    va_list ap;
    va_start(ap, arg);
    int r = execl_common(file, arg, ap, 1);
    va_end(ap);
    return r;
}

/* ── environment ────────────────────────────────────────────────────────── */

/* environ starts as the kernel's array on the stack; the first change moves
 * it to the heap so it can grow. */
static char **env_owned;

static int env_find(const char *name, size_t nlen) {
    if (!environ) return -1;
    for (int i = 0; environ[i]; i++)
        if (!strncmp(environ[i], name, nlen) && environ[i][nlen] == '=') return i;
    return -1;
}

static int env_put(char *entry, size_t nlen, int overwrite) {
    int i = env_find(entry, nlen), n = 0;
    if (i >= 0) {
        if (overwrite) environ[i] = entry;
        return 0;
    }
    if (environ) while (environ[n]) n++;
    char **e = malloc((n + 2) * sizeof(char *));
    if (!e) { errno = ENOMEM; return -1; }
    if (n) memcpy(e, environ, n * sizeof(char *));
    e[n] = entry;
    e[n + 1] = 0;
    if (env_owned) free(env_owned);
    environ = env_owned = e;
    return 0;
}

int setenv(const char *name, const char *value, int overwrite) {
    size_t nlen = name ? strlen(name) : 0;
    if (!nlen || strchr(name, '=')) { errno = EINVAL; return -1; }
    if (!overwrite && env_find(name, nlen) >= 0) return 0;
    size_t vlen = strlen(value);
    char *entry = malloc(nlen + vlen + 2);
    if (!entry) { errno = ENOMEM; return -1; }
    memcpy(entry, name, nlen);
    entry[nlen] = '=';
    memcpy(entry + nlen + 1, value, vlen + 1);
    return env_put(entry, nlen, 1);
}

int putenv(char *string) {
    char *eq = strchr(string, '=');
    if (!eq) return unsetenv(string);
    return env_put(string, (size_t)(eq - string), 1);
}

int unsetenv(const char *name) {
    size_t nlen = name ? strlen(name) : 0;
    if (!nlen || strchr(name, '=')) { errno = EINVAL; return -1; }
    int i;
    while ((i = env_find(name, nlen)) >= 0)
        for (; environ[i]; i++) environ[i] = environ[i + 1];
    return 0;
}

int clearenv(void) {
    if (env_owned) free(env_owned);
    env_owned = 0;
    environ = 0;
    return 0;
}

/* ── stdlib ─────────────────────────────────────────────────────────────── */

long long llabs(long long v) { return v < 0 ? -v : v; }

static int digit_val(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return 99;
}

/* Shared by strtoll/strtoull: the magnitude, saturated at `limit`. */
static unsigned long long strto_u64(const char *s, char **endp, int base,
                                    int *neg, unsigned long long limit) {
    const char *p = s;
    unsigned long long v = 0;
    int any = 0, over = 0;

    *neg = 0;
    while (*p == ' ' || (*p >= '\t' && *p <= '\r')) p++;
    if (*p == '-' || *p == '+') *neg = *p++ == '-';
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')
        && digit_val(p[2]) < 16) {
        p += 2;
        base = 16;
    } else if (base == 0) base = *p == '0' ? 8 : 10;
    for (;; p++) {
        int d = digit_val((unsigned char)*p);
        if (d >= base) break;
        any = 1;
        if (v > (limit - (unsigned)d) / (unsigned)base) over = 1;
        else v = v * (unsigned)base + (unsigned)d;
    }
    if (endp) *endp = (char *)(any ? p : s);
    if (over) { errno = ERANGE; return limit; }
    return v;
}

long long strtoll(const char *s, char **endp, int base) {
    int neg;
    unsigned long long v = strto_u64(s, endp, base, &neg, (unsigned long long)LLONG_MAX + 1);
    if (!neg && v > (unsigned long long)LLONG_MAX) { errno = ERANGE; return LLONG_MAX; }
    return neg ? (long long)(0ULL - v) : (long long)v;
}

unsigned long long strtoull(const char *s, char **endp, int base) {
    int neg;
    unsigned long long v = strto_u64(s, endp, base, &neg, ULLONG_MAX);
    return neg ? 0ULL - v : v;
}

long long atoll(const char *s) { return strtoll(s, 0, 10); }

static long double pow10l_int(int e) {
    long double r = 1.0L, b = 10.0L;
    unsigned n = e < 0 ? -(unsigned)e : (unsigned)e;
    while (n) {
        if (n & 1) r *= b;
        b *= b;
        n >>= 1;
    }
    return e < 0 ? 1.0L / r : r;
}

static int lc_eq(const char *s, const char *word) {
    while (*word) if ((*s++ | 32) != *word++) return 0;
    return 1;
}

long double strtold(const char *s, char **endp) {
    const char *p = s;
    int neg = 0, any = 0, exp10 = 0;
    unsigned long long mant = 0;
    long double v;

    while (*p == ' ' || (*p >= '\t' && *p <= '\r')) p++;
    if (*p == '-' || *p == '+') neg = *p++ == '-';
    if (lc_eq(p, "inf")) {
        p += lc_eq(p, "infinity") ? 8 : 3;
        if (endp) *endp = (char *)p;
        return neg ? -__builtin_infl() : __builtin_infl();
    }
    if (lc_eq(p, "nan")) {
        if (endp) *endp = (char *)p + 3;
        return __builtin_nanl("");
    }
    if (p[0] == '0' && (p[1] | 32) == 'x' && digit_val(p[2]) < 16) {
        p += 2;
        v = 0;
        for (; digit_val(*p) < 16; p++) v = v * 16 + digit_val(*p);
        if (*p == '.') for (long double f = 1.0L / 16; digit_val(*++p) < 16; f /= 16)
            v += digit_val(*p) * f;
        if ((*p | 32) == 'p') {
            char *e;
            long x = strtol(p + 1, &e, 10);
            if (e != p + 1) {
                for (; x > 0; x--) v *= 2;
                for (; x < 0; x++) v /= 2;
                p = e;
            }
        }
        if (endp) *endp = (char *)p;
        return neg ? -v : v;
    }
    /* Up to 19 significant digits go into a 64-bit mantissa, then scale. */
    for (; *p >= '0' && *p <= '9'; p++, any = 1) {
        if (mant < 1000000000000000000ULL) mant = mant * 10 + (*p - '0');
        else exp10++;
    }
    if (*p == '.') {
        for (p++; *p >= '0' && *p <= '9'; p++, any = 1) {
            if (mant < 1000000000000000000ULL) { mant = mant * 10 + (*p - '0'); exp10--; }
        }
    }
    if (!any) {
        if (endp) *endp = (char *)s;
        return 0;
    }
    if ((*p | 32) == 'e') {
        const char *q = p + 1;
        int eneg = 0, e = 0;
        if (*q == '-' || *q == '+') eneg = *q++ == '-';
        if (*q >= '0' && *q <= '9') {
            for (; *q >= '0' && *q <= '9'; q++) if (e < 100000) e = e * 10 + (*q - '0');
            exp10 += eneg ? -e : e;
            p = q;
        }
    }
    if (endp) *endp = (char *)p;
    v = (long double)mant;
    if (mant && exp10) {
        if (exp10 < -4950) v = 0;
        else if (exp10 > 4950) v = __builtin_infl();
        else if (exp10 < -4000) v = v * pow10l_int(-4000) * pow10l_int(exp10 + 4000);
        else v *= pow10l_int(exp10);
    }
    return neg ? -v : v;
}

double strtod(const char *s, char **endp) {
    long double v = strtold(s, endp);
    if (v == __builtin_infl() || v == -__builtin_infl()) {
        if (endp && *endp != s && !lc_eq(s + strspn(s, " \t+-"), "inf")) errno = ERANGE;
    }
    return (double)v;
}
float strtof(const char *s, char **endp) { return (float)strtold(s, endp); }

void *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
              int (*compar)(const void *, const void *)) {
    const char *b = base;
    while (nmemb) {
        const char *mid = b + (nmemb / 2) * size;
        int c = compar(key, mid);
        if (!c) return (void *)mid;
        if (c > 0) {
            b = mid + size;
            nmemb -= nmemb / 2 + 1;
        } else nmemb /= 2;
    }
    return 0;
}

static void swap_bytes(char *a, char *b, size_t n) {
    while (n--) {
        char t = *a;
        *a++ = *b;
        *b++ = t;
    }
}

static void msort(char *b, size_t n, size_t size, char *tmp,
                  int (*cmp)(const void *, const void *)) {
    if (n < 8) {                    /* insertion sort */
        for (size_t i = 1; i < n; i++)
            for (size_t j = i; j && cmp(b + (j - 1) * size, b + j * size) > 0; j--)
                swap_bytes(b + (j - 1) * size, b + j * size, size);
        return;
    }
    size_t h = n / 2, i = 0, j = h, k = 0;
    msort(b, h, size, tmp, cmp);
    msort(b + h * size, n - h, size, tmp, cmp);
    while (i < h && j < n) {
        if (cmp(b + j * size, b + i * size) < 0) memcpy(tmp + k++ * size, b + j++ * size, size);
        else memcpy(tmp + k++ * size, b + i++ * size, size);
    }
    if (i < h) memcpy(tmp + k * size, b + i * size, (h - i) * size), k += h - i;
    memcpy(b, tmp, k * size);
}

/* A stable merge sort; heapsort in place when there is no memory for it. */
void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *)) {
    char *b = base;
    if (nmemb < 2 || !size) return;
    char *tmp = malloc(nmemb * size);
    if (tmp) {
        msort(b, nmemb, size, tmp, compar);
        free(tmp);
        return;
    }
    for (size_t start = nmemb / 2, end = nmemb;;) {
        size_t root;
        if (start) root = --start;
        else if (--end) { swap_bytes(b, b + end * size, size); root = 0; }
        else break;
        for (size_t child; (child = 2 * root + 1) < end; root = child) {
            if (child + 1 < end && compar(b + child * size, b + (child + 1) * size) < 0) child++;
            if (compar(b + root * size, b + child * size) >= 0) break;
            swap_bytes(b + root * size, b + child * size, size);
        }
    }
}

static unsigned long long rand_state = 1;
long random(void) {
    rand_state = rand_state * 6364136223846793005ULL + 1442695040888963407ULL;
    return (long)(rand_state >> 33);
}
void srandom(unsigned int seed) { rand_state = seed; random(); }
int rand(void) { return (int)random(); }
void srand(unsigned int seed) { srandom(seed); }

void abort(void) {
    raise(SIGABRT);
    signal(SIGABRT, SIG_DFL);
    raise(SIGABRT);
    _exit(127);
}

/* ── string ─────────────────────────────────────────────────────────────── */

void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = s;
    for (; n--; p++) if (*p == (unsigned char)c) return (void *)p;
    return 0;
}
void *memrchr(const void *s, int c, size_t n) {
    const unsigned char *p = (const unsigned char *)s + n;
    while (n--) if (*--p == (unsigned char)c) return (void *)p;
    return 0;
}
void *memccpy(void *dst, const void *src, int c, size_t n) {
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--) if ((*d++ = *s++) == (unsigned char)c) return d;
    return 0;
}
void *mempcpy(void *dst, const void *src, size_t n) {
    return (char *)memcpy(dst, src, n) + n;
}
void *memmem(const void *hay, size_t hlen, const void *needle, size_t nlen) {
    const char *h = hay;
    if (!nlen) return (void *)h;
    for (; hlen >= nlen; h++, hlen--)
        if (*h == *(const char *)needle && !memcmp(h, needle, nlen)) return (void *)h;
    return 0;
}
size_t strnlen(const char *s, size_t maxlen) {
    size_t n = 0;
    while (n < maxlen && s[n]) n++;
    return n;
}
char *strcasestr(const char *hay, const char *needle) {
    size_t n = strlen(needle);
    for (; *hay; hay++) if (!strncasecmp(hay, needle, n)) return (char *)hay;
    return n ? 0 : (char *)hay;
}
char *strsep(char **stringp, const char *delim) {
    char *s = *stringp, *e;
    if (!s) return 0;
    e = s + strcspn(s, delim);
    if (*e) *e++ = 0;
    else e = 0;
    *stringp = e;
    return s;
}
char *strchrnul(const char *s, int c) {
    while (*s && *s != (char)c) s++;
    return (char *)s;
}
size_t strlcpy(char *dst, const char *src, size_t size) {
    size_t n = strlen(src);
    if (size) {
        size_t c = n < size - 1 ? n : size - 1;
        memcpy(dst, src, c);
        dst[c] = 0;
    }
    return n;
}
int strcoll(const char *a, const char *b) { return strcmp(a, b); }

char *strsignal(int sig) {
    static const char *const names[] = {
        "Unknown signal", "Hangup", "Interrupt", "Quit", "Illegal instruction",
        "Trace/breakpoint trap", "Aborted", "Bus error", "Floating point exception",
        "Killed", "User defined signal 1", "Segmentation fault",
        "User defined signal 2", "Broken pipe", "Alarm clock", "Terminated",
        "Stack fault", "Child exited", "Continued", "Stopped (signal)", "Stopped",
        "Stopped (tty input)", "Stopped (tty output)", "Urgent I/O condition",
        "CPU time limit exceeded", "File size limit exceeded",
        "Virtual timer expired", "Profiling timer expired", "Window changed",
        "I/O possible", "Power failure", "Bad system call",
    };
    return (char *)names[sig > 0 && sig < 32 ? sig : 0];
}

int ffs(int i) { return __builtin_ffs(i); }
int ffsl(long i) { return __builtin_ffsl(i); }
int ffsll(long long i) { return __builtin_ffsll(i); }

/* ── wide characters (the C locale: ASCII) ──────────────────────────────── */

wint_t towlower(wint_t wc) { return (wc >= 'A' && wc <= 'Z') ? wc + 32 : wc; }
wint_t towupper(wint_t wc) { return (wc >= 'a' && wc <= 'z') ? wc - 32 : wc; }
int iswspace(wint_t wc) { return wc == ' ' || (wc >= '\t' && wc <= '\r'); }
int iswupper(wint_t wc) { return wc >= 'A' && wc <= 'Z'; }
int iswlower(wint_t wc) { return wc >= 'a' && wc <= 'z'; }
int iswalpha(wint_t wc) { return iswupper(wc) || iswlower(wc) || wc > 127; }
int iswdigit(wint_t wc) { return wc >= '0' && wc <= '9'; }
int iswalnum(wint_t wc) { return iswalpha(wc) || iswdigit(wc); }
int iswxdigit(wint_t wc) { return iswdigit(wc) || ((wc | 32) >= 'a' && (wc | 32) <= 'f'); }
int iswblank(wint_t wc) { return wc == ' ' || wc == '\t'; }
int iswcntrl(wint_t wc) { return (wc >= 0 && wc < 32) || wc == 127; }
int iswprint(wint_t wc) { return wc >= 32 && wc != 127; }
int iswgraph(wint_t wc) { return iswprint(wc) && wc != ' '; }
int iswpunct(wint_t wc) { return iswgraph(wc) && !iswalnum(wc); }

/* ── file systems ───────────────────────────────────────────────────────── */

/* The kernel's struct statfs64 (i386, 84 bytes). */
struct kstatfs64 {
    uint32_t f_type, f_bsize;
    uint64_t f_blocks, f_bfree, f_bavail, f_files, f_ffree;
    uint32_t f_fsid[2];
    uint32_t f_namelen, f_frsize, f_flags, f_spare[4];
} __attribute__((packed));

static unsigned long clamp32(uint64_t v) { return v > 0xffffffffULL ? 0xffffffffUL : (unsigned long)v; }

static void from_k64(const struct kstatfs64 *k, struct statfs *buf) {
    buf->f_type = (long)k->f_type;
    buf->f_bsize = (long)k->f_bsize;
    buf->f_blocks = (long)clamp32(k->f_blocks);
    buf->f_bfree = (long)clamp32(k->f_bfree);
    buf->f_bavail = (long)clamp32(k->f_bavail);
    buf->f_files = (long)clamp32(k->f_files);
    buf->f_ffree = (long)clamp32(k->f_ffree);
    buf->f_fsid = (long)k->f_fsid[0];
    buf->f_namelen = (long)k->f_namelen;
    buf->f_frsize = (long)(k->f_frsize ? k->f_frsize : k->f_bsize);
    buf->f_flags = (long)k->f_flags;
    memset(buf->f_spare, 0, sizeof(buf->f_spare));
}

int statfs(const char *path, struct statfs *buf) {
    struct kstatfs64 k;
    if (chkerr(syscall3(268, (int)path, sizeof(k), (int)&k)) < 0) return -1;
    from_k64(&k, buf);
    return 0;
}
int fstatfs(int fd, struct statfs *buf) {
    struct kstatfs64 k;
    if (chkerr(syscall3(269, fd, sizeof(k), (int)&k)) < 0) return -1;
    from_k64(&k, buf);
    return 0;
}

static void vfs_from_statfs(const struct statfs *s, struct statvfs *buf) {
    buf->f_bsize = (unsigned long)s->f_bsize;
    buf->f_frsize = (unsigned long)s->f_frsize;
    buf->f_blocks = (unsigned long)s->f_blocks;
    buf->f_bfree = (unsigned long)s->f_bfree;
    buf->f_bavail = (unsigned long)s->f_bavail;
    buf->f_files = (unsigned long)s->f_files;
    buf->f_ffree = (unsigned long)s->f_ffree;
    buf->f_favail = (unsigned long)s->f_ffree;
    buf->f_fsid = (unsigned long)s->f_fsid;
    buf->f_flag = (unsigned long)s->f_flags;
    buf->f_namemax = (unsigned long)s->f_namelen;
}
int statvfs(const char *path, struct statvfs *buf) {
    struct statfs s;
    if (statfs(path, &s)) return -1;
    vfs_from_statfs(&s, buf);
    return 0;
}
int fstatvfs(int fd, struct statvfs *buf) {
    struct statfs s;
    if (fstatfs(fd, &s)) return -1;
    vfs_from_statfs(&s, buf);
    return 0;
}

/* ── shell commands ─────────────────────────────────────────────────────── */

/* /bin/sh when a disk provides one, else the native shell in /. */
static const char *shell_path(void) {
    return access("/bin/sh", X_OK) ? "/shell" : "/bin/sh";
}

int system(const char *command) {
    if (!command) return !access(shell_path(), X_OK);
    int pid = fork(), status;
    if (pid < 0) return -1;
    if (!pid) {
        const char *sh = shell_path();
        execve(sh, (char *[]){ (char *)sh, "-c", (char *)command, 0 }, environ);
        _exit(127);
    }
    while (waitpid(pid, &status, 0) < 0) if (errno != EINTR) return -1;
    return status;
}

static struct { FILE *f; int pid; } popen_tab[16];

FILE *popen(const char *command, const char *mode) {
    int fds[2], rd = *mode == 'r', slot;
    for (slot = 0; slot < 16 && popen_tab[slot].f; slot++);
    if (slot == 16 || (*mode != 'r' && *mode != 'w')) { errno = EINVAL; return 0; }
    if (pipe(fds)) return 0;
    int pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return 0; }
    if (!pid) {
        const char *sh = shell_path();
        /* POSIX: the child must not keep the parent's ends of earlier popen()
         * pipes, or a reader of one of them never sees EOF. */
        for (int i = 0; i < 16; i++) if (popen_tab[i].f) close(fileno(popen_tab[i].f));
        dup2(fds[rd ? 1 : 0], rd ? 1 : 0);
        close(fds[0]);
        close(fds[1]);
        execve(sh, (char *[]){ (char *)sh, "-c", (char *)command, 0 }, environ);
        _exit(127);
    }
    close(fds[rd ? 1 : 0]);
    FILE *f = fdopen(fds[rd ? 0 : 1], rd ? "r" : "w");
    if (!f) { close(fds[rd ? 0 : 1]); return 0; }
    popen_tab[slot].f = f;
    popen_tab[slot].pid = pid;
    return f;
}

int pclose(FILE *stream) {
    int status, pid = -1;
    for (int i = 0; i < 16; i++) if (popen_tab[i].f == stream) {
        pid = popen_tab[i].pid;
        popen_tab[i].f = 0;
    }
    if (pid < 0) { errno = ECHILD; return -1; }
    fclose(stream);
    while (waitpid(pid, &status, 0) < 0) if (errno != EINTR) return -1;
    return status;
}

/* ── netdb / syslog odds and ends ───────────────────────────────────────── */

int h_errno;
const char *hstrerror(int err) {
    switch (err) {
    case HOST_NOT_FOUND: return "Unknown host";
    case TRY_AGAIN:      return "Host name lookup failure";
    case NO_RECOVERY:    return "Unknown server error";
    case NO_DATA:        return "No address associated with name";
    }
    return "Resolver error";
}
int setlogmask(int mask) { (void)mask; return 0xff; }
