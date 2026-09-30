#include "../include/errno.h"
#include "../include/dirent.h"
#include "../include/fcntl.h"
#include "../include/grp.h"
#include "../include/langinfo.h"
#include "../include/locale.h"
#include "../include/mntent.h"
#include "../include/netdb.h"
#include "../include/poll.h"
#include "../include/pwd.h"
#include "../include/regex.h"
#include "../include/setjmp.h"
#include "../include/signal.h"
#include "../include/stdio.h"
#include "../include/stdlib.h"
#include "../include/string.h"
#include "../include/sys/inotify.h"
#include "../include/sys/mman.h"
#include "../include/sys/socket.h"
#include "../include/sys/stat.h"
#include "../include/sys/statvfs.h"
#include "../include/sys/times.h"
#include "../include/sys/uio.h"
#include "../include/sys/xattr.h"
#include "../include/termios.h"
#include "../include/time.h"
#include "../include/unistd.h"
#include "../include/wchar.h"
#include "../include/wctype.h"
#include "../include/syscall.h"

#define AT_FDCWD (-100)

static int nosys(void) {
    errno = ENOSYS;
    return -1;
}

static int chkerr(int ret) {
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    errno = 0;
    return ret;
}

void _exit(int status) { exit(status); }
int vfork(void) { return fork(); }
int setuid(int uid) { return chkerr(syscall1(213, uid)); }  /* setuid32 */
int setgid(int gid) { return chkerr(syscall1(214, gid)); }  /* setgid32 */
/* seteuid(u) is setresuid(-1, u, -1), as in musl; 138 is setfsuid. */
int seteuid(int uid) { return chkerr(syscall3(208, -1, uid, -1)); }  /* setresuid32 */
int setegid(int gid) { return chkerr(syscall3(210, -1, gid, -1)); }  /* setresgid32 */
int setreuid(int ruid, int euid) { return chkerr(syscall2(203, ruid, euid)); }
int setregid(int rgid, int egid) { return chkerr(syscall2(204, rgid, egid)); }
int setresuid(int ruid, int euid, int suid) {
    return chkerr(syscall3(208, ruid, euid, suid));
}
int setresgid(int rgid, int egid, int sgid) {
    return chkerr(syscall3(210, rgid, egid, sgid));
}
int getresuid(int *ruid, int *euid, int *suid) {
    return chkerr(syscall3(209, (int)ruid, (int)euid, (int)suid));
}
int getresgid(int *rgid, int *egid, int *sgid) {
    return chkerr(syscall3(211, (int)rgid, (int)egid, (int)sgid));
}
int chroot(const char *path) { (void)path; return nosys(); }
int link(const char *oldpath, const char *newpath) { (void)oldpath; (void)newpath; return nosys(); }
int fsync(int fd) { (void)fd; return 0; }
int fchmod(int fd, int mode) { return chkerr(syscall2(94, fd, mode)); }
int chmod(const char *path, int mode) { return chkerr(syscall2(15, (int)path, mode)); }
int fchown(int fd, int owner, int group) {
    return chkerr(syscall3(207, fd, owner, group));
}
int fchmodat(int dirfd, const char *path, int mode, int flags) {
    return chkerr(syscall4(306, dirfd, (int)path, mode, flags));
}
int umask(int mask) { return syscall1(60, mask & 0777); }
int isatty(int fd) { return fd >= 0 && fd <= 2; }

int fstatat(int dirfd, const char *path, struct stat *buf, int flags) {
    return chkerr(syscall4(300, dirfd, (int)path, (int)buf, flags));
}

DIR *fdopendir(int fd) {
    DIR *d = malloc(sizeof(DIR));
    if (!d) return 0;
    d->fd = fd;
    d->pos = 0;
    d->buf_pos = 0;
    d->buf_len = 0;
    return d;
}

int mkdirat(int dirfd, const char *path, int mode) {
    return chkerr(syscall3(296, dirfd, (int)path, mode));
}

int readlinkat(int dirfd, const char *path, char *buf, int bufsiz) {
    return chkerr(syscall4(305, dirfd, (int)path, (int)buf, bufsiz));
}

int faccessat(int dirfd, const char *path, int mode, int flags) {
    return chkerr(syscall4(307, dirfd, (int)path, mode, flags));
}

int unlinkat(int dirfd, const char *path, int flags) {
    return chkerr(syscall3(301, dirfd, (int)path, flags));
}

int rmdir(const char *path) {
    return unlink(path);
}

char *getcwd(char *buf, int size) {
    if (!buf) {
        size = size > 0 ? size : 4096;
        buf = malloc((size_t)size);
        if (!buf) return 0;
    }
    return getcwd_syscall(buf, size) < 0 ? 0 : buf;
}

int execv(const char *path, char *const argv[]) { return execve(path, argv, environ); }
int execvp(const char *file, char *const argv[]) { return execve(file, argv, environ); }

char *stpcpy(char *dst, const char *src) {
    while ((*dst = *src)) { dst++; src++; }
    return dst;
}

char *strerror(int errnum) {
    (void)errnum;
    return "error";
}

int strcasecmp(const char *a, const char *b) {
    while (*a && *b) {
        char ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (ca != cb) return (unsigned char)ca - (unsigned char)cb;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncasecmp(const char *a, const char *b, size_t n) {
    while (n-- && *a && *b) {
        char ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (ca != cb) return (unsigned char)ca - (unsigned char)cb;
        a++; b++;
    }
    return n == (size_t)-1 ? 0 : (unsigned char)*a - (unsigned char)*b;
}

long long strtoll(const char *s, char **endp, int base) {
    return (long long)strtol(s, endp, base);
}

long long atoll(const char *s) {
    return strtoll(s, 0, 10);
}

double strtod(const char *s, char **endp) {
    long v = strtol(s, endp, 10);
    return (double)v;
}

long double strtold(const char *s, char **endp) {
    return (long double)strtod(s, endp);
}

int setenv(const char *name, const char *value, int overwrite) {
    (void)name; (void)value; (void)overwrite;
    return 0;
}
int unsetenv(const char *name) { (void)name; return 0; }
int mkstemp(char *template) {
    static const char chars[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    size_t len = template ? strlen(template) : 0;
    char *x = template + len - 6;

    if (len < 6 || memcmp(x, "XXXXXX", 6)) { errno = EINVAL; return -1; }
    for (int tries = 0; tries < 100; tries++) {
        unsigned char rnd[6];
        if (getrandom(rnd, sizeof(rnd), 0) != (int)sizeof(rnd)) {
            /* No entropy source: fall back to something that still differs
             * per call so the O_EXCL retry loop can make progress. */
            struct timeval tv;
            gettimeofday(&tv, 0);
            unsigned v = (unsigned)tv.tv_usec ^ ((unsigned)getpid() << 16)
                       ^ (unsigned)tries * 2654435761u;
            for (int i = 0; i < 6; i++) { rnd[i] = (unsigned char)v; v = v * 1103515245u + 12345u; }
        }
        for (int i = 0; i < 6; i++) x[i] = chars[rnd[i] % (sizeof(chars) - 1)];
        int fd = open(template, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0 || errno != EEXIST) return fd;
    }
    errno = EEXIST;
    return -1;
}

void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *)) {
    char *b = base;
    char tmp[64];
    if (size > sizeof(tmp)) return;
    for (size_t i = 0; i < nmemb; i++) {
        for (size_t j = i + 1; j < nmemb; j++) {
            char *a = b + i * size;
            char *c = b + j * size;
            if (compar(a, c) > 0) {
                memcpy(tmp, a, size);
                memcpy(a, c, size);
                memcpy(c, tmp, size);
            }
        }
    }
}

int sigfillset(sigset_t *set) { if (set) *set = ~0UL; return 0; }
int sigprocmask(int how, const sigset_t *set, sigset_t *oldset) {
    (void)how; (void)set; if (oldset) *oldset = 0; return 0;
}
int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact) {
    (void)act; if (oldact) oldact->sa_handler = SIG_DFL; signal(signum, act ? act->sa_handler : SIG_DFL); return 0;
}
int sigsetjmp(sigjmp_buf env, int savesigs) { (void)env; (void)savesigs; return 0; }
void siglongjmp(sigjmp_buf env, int val) { (void)env; exit(val); }

int tcflush(int fd, int queue_selector) { (void)fd; (void)queue_selector; return 0; }
int cfsetspeed(struct termios *t, speed_t speed) { (void)t; (void)speed; return 0; }

int setvbuf(FILE *stream, char *buf, int mode, size_t size) {
    (void)stream; (void)buf; (void)mode; (void)size; return 0;
}
long getdelim(char **lineptr, size_t *n, int delim, FILE *stream) {
    if (!lineptr || !n || !stream) {
        errno = EINVAL;
        return -1;
    }
    if (!*lineptr || !*n) {
        *n = 128;
        *lineptr = malloc(*n);
        if (!*lineptr) return -1;
    }

    size_t len = 0;
    for (;;) {
        int c = fgetc(stream);
        if (c == EOF) break;
        if (len + 1 >= *n) {
            size_t new_n = *n * 2;
            char *new_line = realloc(*lineptr, new_n);
            if (!new_line) return -1;
            *lineptr = new_line;
            *n = new_n;
        }
        (*lineptr)[len++] = (char)c;
        if (c == delim) break;
    }

    if (!len) return -1;
    (*lineptr)[len] = 0;
    return (long)len;
}
long getline(char **lineptr, size_t *n, FILE *stream) {
    return getdelim(lineptr, n, '\n', stream);
}
int dprintf(int fd, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) write(fd, buf, n < (int)sizeof(buf) ? n : (int)sizeof(buf));
    return n;
}

char *basename(char *path) {
    char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}
char *dirname(char *path) {
    char *s = strrchr(path, '/');
    if (!s) return ".";
    if (s == path) return "/";
    *s = 0;
    return path;
}

/* ── /etc/passwd ────────────────────────────────────────────────────────────
 * name:passwd:uid:gid:gecos:dir:shell.  Fields may be empty ("user::1000:..."
 * or an empty gecos), so the line is split on every ':' — a strtok-style
 * splitter would merge "::" and shift every later field. */

/* Split `line` in place on ':' into exactly `want` fields; -1 otherwise. */
static int pw_split(char *line, char **f, int want) {
    int n = 0;
    f[n++] = line;
    for (char *p = line; *p; p++) {
        if (*p != ':') continue;
        if (n == want) return -1;
        *p = 0;
        f[n++] = p + 1;
    }
    return n == want ? 0 : -1;
}

static int pw_num(const char *s, unsigned *out) {
    unsigned v = 0;
    if (!*s) return -1;
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || v > 429496728U) return -1;
        v = v * 10 + (unsigned)(*s - '0');
    }
    *out = v;
    return 0;
}

static FILE *pw_open(void) {
    FILE *f = fopen("/etc/passwd", "r");
    if (!f) f = fopen("/disk/etc/passwd", "r");   /* same fallback as login */
    return f;
}

/* Find the entry for `name` (or `uid` when name is NULL), copying the line
 * into buf so the returned strings live there.  0 = found, ENOENT = no such
 * entry, ERANGE = buf too small, EIO = no passwd file. */
static int pw_lookup(const char *name, uid_t uid, struct passwd *pw,
                     char *buf, size_t buflen) {
    char line[512];
    FILE *f = pw_open();
    int rc = ENOENT;

    if (!f) return EIO;
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        char *fld[7], tmp[512];
        unsigned u, g;

        if (len && line[len - 1] != '\n' && !feof(f)) {
            int c;   /* over-long line: skip the rest of it */
            while ((c = fgetc(f)) != EOF && c != '\n') {}
            continue;
        }
        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = 0;
        if (!len || line[0] == '#') continue;
        memcpy(tmp, line, len + 1);
        if (pw_split(tmp, fld, 7) || pw_num(fld[2], &u) || pw_num(fld[3], &g))
            continue;
        if (name ? strcmp(fld[0], name) : u != (unsigned)uid) continue;

        if (len + 1 > buflen) { rc = ERANGE; break; }
        memcpy(buf, line, len + 1);
        pw_split(buf, fld, 7);
        pw->pw_name   = fld[0];
        pw->pw_passwd = fld[1];
        pw->pw_uid    = (uid_t)u;
        pw->pw_gid    = (gid_t)g;
        pw->pw_gecos  = fld[4];
        pw->pw_dir    = fld[5];
        pw->pw_shell  = fld[6];
        rc = 0;
        break;
    }
    fclose(f);
    return rc;
}

/* With no passwd file at all (a bare initrd), root still has a name. */
static int pw_builtin_root(const char *name, uid_t uid, struct passwd *pw,
                           char *buf, size_t buflen) {
    static const char line[] = "root:x:0:0:root:/:/shell";
    char *fld[7];
    if (name ? strcmp(name, "root") : uid != 0) return ENOENT;
    if (sizeof(line) > buflen) return ERANGE;
    memcpy(buf, line, sizeof(line));
    pw_split(buf, fld, 7);
    pw->pw_name = fld[0]; pw->pw_passwd = fld[1];
    pw->pw_uid = 0; pw->pw_gid = 0;
    pw->pw_gecos = fld[4]; pw->pw_dir = fld[5]; pw->pw_shell = fld[6];
    return 0;
}

static int pw_get_r(const char *name, uid_t uid, struct passwd *pwd, char *buf,
                    size_t buflen, struct passwd **result) {
    int rc = pw_lookup(name, uid, pwd, buf, buflen);
    if (rc == EIO) rc = pw_builtin_root(name, uid, pwd, buf, buflen);
    *result = rc ? (struct passwd *)0 : pwd;
    return rc == ENOENT ? 0 : rc;   /* "not found" is not an error */
}

int getpwuid_r(uid_t uid, struct passwd *pwd, char *buf, size_t buflen, struct passwd **result) {
    return pw_get_r((const char *)0, uid, pwd, buf, buflen, result);
}
int getpwnam_r(const char *name, struct passwd *pwd, char *buf, size_t buflen, struct passwd **result) {
    if (!name) { *result = 0; return 0; }
    return pw_get_r(name, 0, pwd, buf, buflen, result);
}

static struct passwd pw_static;
static char pw_static_buf[512];
struct passwd *getpwuid(uid_t uid) {
    struct passwd *r;
    getpwuid_r(uid, &pw_static, pw_static_buf, sizeof(pw_static_buf), &r);
    return r;
}
struct passwd *getpwnam(const char *name) {
    struct passwd *r;
    getpwnam_r(name, &pw_static, pw_static_buf, sizeof(pw_static_buf), &r);
    return r;
}

static struct group root_gr = {"root", "x", 0, 0};
struct group *getgrgid(gid_t gid) { return gid ? 0 : &root_gr; }
struct group *getgrnam(const char *name) { return !strcmp(name, "root") ? &root_gr : 0; }
int getgrgid_r(gid_t gid, struct group *grp, char *buf, size_t buflen, struct group **result) {
    (void)buf; (void)buflen; *result = getgrgid(gid); if (*result && grp) *grp = **result; return *result ? 0 : ENOENT;
}
int getgrnam_r(const char *name, struct group *grp, char *buf, size_t buflen, struct group **result) {
    (void)buf; (void)buflen; *result = getgrnam(name); if (*result && grp) *grp = **result; return *result ? 0 : ENOENT;
}
int initgroups(const char *user, gid_t group) { (void)user; (void)group; return 0; }
int getgroups(int size, gid_t list[]) {
    if (size > 0 && list) list[0] = 0;
    return 1;
}
int getgrouplist(const char *user, gid_t group, gid_t *groups, int *ngroups) {
    (void)user;
    if (groups && ngroups && *ngroups > 0) groups[0] = group;
    if (ngroups) *ngroups = 1;
    return 1;
}

int regcomp(regex_t *preg, const char *regex, int cflags) { (void)preg; (void)regex; (void)cflags; return REG_NOMATCH; }
int regexec(const regex_t *preg, const char *string, unsigned long nmatch, regmatch_t pmatch[], int eflags) {
    (void)preg; (void)string; (void)nmatch; (void)pmatch; (void)eflags; return REG_NOMATCH;
}
unsigned long regerror(int errcode, const regex_t *preg, char *errbuf, unsigned long errbuf_size) {
    (void)errcode; (void)preg; if (errbuf && errbuf_size) *errbuf = 0; return 0;
}
void regfree(regex_t *preg) { (void)preg; }

int poll(struct pollfd *fds, unsigned long nfds, int timeout) {
    int ret = syscall3(168, (int)fds, (int)nfds, timeout);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    errno = 0;
    return ret;
}
int setsockopt(int fd, int level, int optname, const void *optval, socklen_t optlen) {
    (void)fd; (void)level; (void)optname; (void)optval; (void)optlen; return 0;
}
int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res) {
    (void)node; (void)service; (void)hints; if (res) *res = 0; return EAI_FAIL;
}
void freeaddrinfo(struct addrinfo *res) { (void)res; }
const char *gai_strerror(int errcode) { (void)errcode; return "addrinfo"; }
const char *inet_ntop(int af, const void *src, char *dst, socklen_t size) {
    (void)af; (void)src; if (size) *dst = 0; return dst;
}

time_t time(time_t *tloc) { struct timeval tv; gettimeofday(&tv, 0); if (tloc) *tloc = tv.tv_sec; return tv.tv_sec; }
struct tm *localtime(const time_t *timep) { static struct tm tm; return localtime_r(timep, &tm); }
struct tm *localtime_r(const time_t *timep, struct tm *result) {
    (void)timep;
    memset(result, 0, sizeof(*result));
    result->tm_mday = 1;
    result->tm_year = 70;
    result->tm_wday = 4;
    return result;
}
struct tm *gmtime(const time_t *timep) { return localtime(timep); }
char *ctime(const time_t *timep) { (void)timep; return "Thu Jan  1 00:00:00 1970\n"; }
unsigned long strftime(char *s, unsigned long max, const char *format, const struct tm *tm) {
    static const char *days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    (void)format;
    if (!s || !max || !tm) return 0;

    int wday = (tm->tm_wday >= 0 && tm->tm_wday < 7) ? tm->tm_wday : 4;
    int mon = (tm->tm_mon >= 0 && tm->tm_mon < 12) ? tm->tm_mon : 0;
    int mday = tm->tm_mday > 0 ? tm->tm_mday : 1;
    int year = tm->tm_year + 1900;
    if (year < 1900) year = 1970;

    int n = snprintf(s, max, "%s %s %2d %02d:%02d:%02d UTC %d",
                     days[wday], months[mon], mday, tm->tm_hour, tm->tm_min,
                     tm->tm_sec, year);
    if (n < 0 || (unsigned long)n >= max) {
        if (max) *s = 0;
        return 0;
    }
    return (unsigned long)n;
}
char *strptime(const char *buf, const char *format, struct tm *tm) { (void)format; (void)tm; return (char *)buf; }
time_t mktime(struct tm *tm) { (void)tm; return 0; }
void tzset(void) {}
int clock_gettime(clockid_t clk_id, struct timespec *tp) {
    (void)clk_id; struct timeval tv; int rc = gettimeofday(&tv, 0); if (!rc) { tp->tv_sec = tv.tv_sec; tp->tv_nsec = tv.tv_usec * 1000; } return rc;
}
int settimeofday(const struct timeval *tv, const void *tz) {
    (void)tv; (void)tz; return 0;
}
int utimensat(int dirfd, const char *path, const struct timespec times[2], int flags) {
    (void)dirfd; (void)times; (void)flags;
    if (access(path, F_OK)) return -1;
    return 0;
}
int futimens(int fd, const struct timespec times[2]) {
    (void)fd; (void)times; return 0;
}

char *setlocale(int category, const char *locale) { (void)category; (void)locale; return "C"; }
locale_t newlocale(int mask, const char *locale, locale_t base) { (void)mask; (void)locale; return base ? base : (locale_t)1; }
locale_t uselocale(locale_t locale) { return locale; }
char *nl_langinfo(nl_item item) { (void)item; return "UTF-8"; }
int wcwidth(wchar_t wc) { (void)wc; return 1; }
int wcrtomb(char *s, wchar_t wc, void *ps) { (void)ps; if (s) *s = (char)wc; return 1; }
wint_t towlower(wint_t wc) { return (wc >= 'A' && wc <= 'Z') ? wc + 32 : wc; }
int iswspace(wint_t wc) { return wc == ' ' || wc == '\n' || wc == '\t'; }

int statvfs(const char *path, struct statvfs *buf) { (void)path; memset(buf, 0, sizeof(*buf)); return 0; }
int fstatvfs(int fd, struct statvfs *buf) { (void)fd; memset(buf, 0, sizeof(*buf)); return 0; }
void *setmntent(const char *filename, const char *type) { (void)filename; (void)type; return 0; }
struct mntent *getmntent(void *stream) { (void)stream; return 0; }
int endmntent(void *stream) { (void)stream; return 0; }

int inotify_init(void) { return nosys(); }
int inotify_add_watch(int fd, const char *pathname, uint32_t mask) { (void)fd; (void)pathname; (void)mask; return nosys(); }
int inotify_rm_watch(int fd, int wd) { (void)fd; (void)wd; return nosys(); }

ssize_t getxattr(const char *p, const char *n, void *v, size_t s) { (void)p; (void)n; (void)v; (void)s; return nosys(); }
ssize_t lgetxattr(const char *p, const char *n, void *v, size_t s) { return getxattr(p, n, v, s); }
ssize_t fgetxattr(int fd, const char *n, void *v, size_t s) { (void)fd; return getxattr(0, n, v, s); }
ssize_t listxattr(const char *p, char *l, size_t s) { (void)p; (void)l; (void)s; return nosys(); }
ssize_t llistxattr(const char *p, char *l, size_t s) { return listxattr(p, l, s); }
ssize_t flistxattr(int fd, char *l, size_t s) { (void)fd; return listxattr(0, l, s); }
ssize_t setxattr(const char *p, const char *n, const void *v, size_t s, int f) { (void)p; (void)n; (void)v; (void)s; (void)f; return nosys(); }
ssize_t lsetxattr(const char *p, const char *n, const void *v, size_t s, int f) { return setxattr(p, n, v, s, f); }
ssize_t fsetxattr(int fd, const char *n, const void *v, size_t s, int f) { (void)fd; return setxattr(0, n, v, s, f); }

void openlog(const char *ident, int option, int facility) { (void)ident; (void)option; (void)facility; }
void vsyslog(int priority, const char *format, va_list ap) { (void)priority; vfprintf(stderr, format, ap); }
void syslog(int priority, const char *format, ...) {
    va_list ap; va_start(ap, format); vsyslog(priority, format, ap); va_end(ap);
}
void closelog(void) {}

long syscall(long num, ...) { (void)num; return -ENOSYS; }
void *mmap(void *addr, size_t length, int prot, int flags, int fd, int offset) {
    (void)addr; (void)prot; (void)flags; (void)fd; (void)offset; return malloc(length);
}
int munmap(void *addr, size_t length) { (void)length; free(addr); return 0; }
