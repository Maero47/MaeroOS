/* libctest — regression checks for libc behaviour that other programs rely
 * on.  Prints one line per check and "LIBCTEST PASS" at the end when all of
 * them held (tools/smoke_toybox.py looks for that line). */
#include "../include/errno.h"
#include "../include/fcntl.h"
#include "../include/pwd.h"
#include "../include/stdio.h"
#include "../include/stdlib.h"
#include "../include/string.h"
#include "../include/sys/stat.h"
#include "../include/unistd.h"
#include "../include/syscall.h"

static int failures;

static void check(int ok, const char *what) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

static int file_mode(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 ? (int)(st.st_mode & 07777) : -1;
}

static void test_snprintf(void) {
    char buf[8];
    memset(buf, 'Z', sizeof(buf));
    int n = snprintf(buf, 0, "hello %d", 42);
    check(n == 8 && buf[0] == 'Z' && buf[7] == 'Z', "snprintf(buf, 0) returns length, writes nothing");
    check(snprintf((char *)0, 0, "%s-%d", "abc", 12345) == 9, "snprintf(NULL, 0) returns length");
    n = snprintf(buf, sizeof(buf), "%s", "0123456789");
    check(n == 10 && !strcmp(buf, "0123456"), "snprintf truncates and terminates");
}

static void test_malloc(void) {
    check(malloc((size_t)-1 - 8) == 0, "malloc(SIZE_MAX-8) returns NULL");
    check(malloc(0x80000000u) == 0, "malloc(2 GiB) returns NULL");
    check(calloc(0x10000, 0x10001) == 0, "calloc overflowing product returns NULL");
    check(calloc((size_t)-1 / 2 + 1, 2) == 0, "calloc wrapping to 0 returns NULL");
    unsigned char *p = calloc(100, 3);
    int zero = p != 0;
    for (int i = 0; p && i < 300; i++) if (p[i]) zero = 0;
    check(zero, "calloc(100, 3) returns zeroed memory");
    free(p);
}

static void test_asprintf(void) {
    char *big = malloc(1501), *out = 0;
    memset(big, 'a', 1500);
    big[1500] = 0;
    int n = asprintf(&out, "%s-%d|", big, 7);
    int ok = n == 1503 && out && strlen(out) == 1503 &&
             !strncmp(out, big, 1500) && !strcmp(out + 1500, "-7|");
    check(ok, "asprintf of 1503 bytes is complete");
    free(out);
    free(big);
}

static void test_printf_ll(void) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%lld", 1LL << 40);
    check(!strcmp(buf, "1099511627776"), "printf %lld 1<<40");
    snprintf(buf, sizeof(buf), "%lld|%s", -(1LL << 40) - 5, "next");
    check(!strcmp(buf, "-1099511627781|next"), "printf %lld negative, next arg intact");
    snprintf(buf, sizeof(buf), "%llu %llx", 18446744073709551615ULL, 0x123456789abcULL);
    check(!strcmp(buf, "18446744073709551615 123456789abc"), "printf %llu / %llx");
    snprintf(buf, sizeof(buf), "%ld %d %lu", -7L, 3, 4000000000UL);
    check(!strcmp(buf, "-7 3 4000000000"), "printf %ld / %lu unchanged");
}

static void test_sscanf(void) {
    char s[8];
    memset(s, 'Z', sizeof(s));
    int n = sscanf("abcdefgh", "%3s", s);
    check(n == 1 && !strcmp(s, "abc") && s[4] == 'Z', "sscanf %3s stops at width");
    int a = 0, b = 0;
    n = sscanf("12345", "%2d%3d", &a, &b);
    check(n == 2 && a == 12 && b == 345, "sscanf %2d%3d honours widths");
    long long ll = 0;
    n = sscanf("x=1099511627776", "x=%lld", &ll);
    check(n == 1 && ll == (1LL << 40), "sscanf %lld");
    long l = 0;
    n = sscanf("-42 rest", "%ld", &l);
    check(n == 1 && l == -42, "sscanf %ld");
}

static void test_open_mode(void) {
    const char *p = "/tmp/libctest.mode";
    int old = umask(022);
    int fd;

    check(umask(022) == 022, "umask returns the previous mask");
    unlink(p);
    fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    close(fd);
    check(fd >= 0 && file_mode(p) == 0600, "open(O_CREAT, 0600) gives 0600");
    unlink(p);
    fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    close(fd);
    check(fd >= 0 && file_mode(p) == 0755, "open(O_CREAT, 0755) gives 0755");
    unlink(p);
    umask(077);
    fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    close(fd);
    check(fd >= 0 && file_mode(p) == 0600, "open(O_CREAT, 0666) under umask 077 gives 0600");
    umask(022);
    errno = 0;
    fd = open(p, O_WRONLY | O_CREAT | O_EXCL, 0600);
    check(fd < 0 && errno == EEXIST, "open(O_CREAT|O_EXCL) on an existing file fails EEXIST");
    if (fd >= 0) close(fd);
    unlink(p);
    umask(old);
}

static void test_mkstemp(void) {
    char a[] = "/tmp/lcXXXXXX", b[] = "/tmp/lcXXXXXX", bad[] = "/tmp/lcXXXX";
    int fa = mkstemp(a), fb = mkstemp(b);
    check(fa >= 0 && fb >= 0 && fa != fb, "mkstemp opens two files");
    check(strcmp(a, "/tmp/lcXXXXXX") && strcmp(a, b), "mkstemp names are filled in and unique");
    check(file_mode(a) == 0600 && file_mode(b) == 0600, "mkstemp files are 0600");
    if (fa >= 0) close(fa);
    if (fb >= 0) close(fb);
    unlink(a);
    unlink(b);
    errno = 0;
    check(mkstemp(bad) < 0 && errno == EINVAL, "mkstemp rejects a template without XXXXXX");
}

static void test_stdio_errors(void) {
    const char *p = "/tmp/libctest.ro";
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) close(fd);
    fd = open(p, O_RDONLY);
    FILE *f = fdopen(fd, "w");   /* stream over a read-only descriptor */
    int r1 = f ? fputs("data that cannot be written\n", f) : 0;
    check(f && r1 == EOF && ferror(f), "fputs reports a failed write");
    check(f && fclose(f) == EOF, "fclose reports the failed write");
    unlink(p);

    f = fopen(p, "w");
    check(f && fputs("fine\n", f) >= 0 && fflush(f) == 0 && !ferror(f) && fclose(f) == 0,
          "a good write still succeeds");
    unlink(p);
}

static void test_passwd(void) {
    struct passwd *pw = getpwuid(0);
    check(pw && !strcmp(pw->pw_name, "root"), "getpwuid(0)->pw_name == root");
    pw = getpwnam("root");
    check(pw && pw->pw_uid == 0 && pw->pw_dir && pw->pw_dir[0] == '/', "getpwnam(root)");
    pw = getpwnam("user");
    check(pw && pw->pw_uid == 1000 && pw->pw_gid == 100, "getpwnam(user) reads /etc/passwd");
    pw = getpwuid(1000);
    check(pw && !strcmp(pw->pw_name, "user"), "getpwuid(1000)->pw_name == user");
    check(getpwnam("nosuchuser") == 0 && getpwuid(4242) == 0, "unknown users are not found");
}

/* i386 numbers: 158 sched_yield, 159/160 sched_get_priority_max/min.  159
 * used to be sched_yield in both the kernel and this libc. */
static void test_sched(void) {
    check(sched_yield() == 0, "sched_yield returns 0");
    check(syscall1(159, 1) == 99 && syscall1(160, 1) == 1,
          "sched_get_priority_max/min(SCHED_FIFO) are 99/1");
    check(syscall1(159, 0) == 0 && syscall1(160, 0) == 0,
          "sched_get_priority_max/min(SCHED_OTHER) are 0");
    check(syscall1(159, 42) == -EINVAL, "sched_get_priority_max(bad policy) is EINVAL");
}

int main(void) {
    test_sched();
    test_snprintf();
    test_malloc();
    test_asprintf();
    test_printf_ll();
    test_sscanf();
    test_open_mode();
    test_mkstemp();
    test_stdio_errors();
    test_passwd();
    if (failures) printf("LIBCTEST FAILED %d\n", failures);
    else printf("LIBCTEST PASS\n");
    return failures ? 1 : 0;
}
