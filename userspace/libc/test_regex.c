/* Host-side regression test for userspace/libc/regex.c (regcomp/regexec and
 * fnmatch), used by tools/test_regex.py.  Built with the host compiler against
 * a copy of regex.c whose includes point at the host headers.  Prints one
 * FAIL line per broken case and exits non-zero if any failed. */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "regex.h"
#include "fnmatch.h"

static int fails, cases;

static void t(const char *re, int fl, const char *s, int want_so, int want_eo) {
    regex_t r;
    regmatch_t m[10];
    cases++;
    int e = regcomp(&r, re, fl);
    if (e) {
        if (want_so != -2) { printf("FAIL comp %s -> %d\n", re, e); fails++; }
        return;
    }
    if (want_so == -2) { printf("FAIL expected comp error %s\n", re); fails++; regfree(&r); return; }
    int x = regexec(&r, s, 10, m, 0);
    int so = x ? -1 : (int)m[0].rm_so, eo = x ? -1 : (int)m[0].rm_eo;
    if (so != want_so || eo != want_eo) {
        printf("FAIL /%s/ on '%s': got %d,%d want %d,%d\n", re, s, so, eo, want_so, want_eo);
        fails++;
    }
    regfree(&r);
}

static void sub(const char *re, int fl, const char *s, int n, int so, int eo) {
    regex_t r;
    regmatch_t m[10];
    cases++;
    regcomp(&r, re, fl);
    if (regexec(&r, s, 10, m, 0) || m[n].rm_so != so || m[n].rm_eo != eo) {
        printf("FAIL sub%d /%s/ '%s' got %d,%d\n", n, re, s, (int)m[n].rm_so, (int)m[n].rm_eo);
        fails++;
    }
    regfree(&r);
}

static void fn(int ok, const char *what) {
    cases++;
    if (!ok) { printf("FAIL fnmatch %s\n", what); fails++; }
}

/* A pattern that backtracks exponentially without memoisation: must give
 * `want` (0 or REG_NOMATCH / REG_ESPACE) in under 50 ms. */
static void slow(const char *re, int fl, char c, int n, const char *tail, int want) {
    char s[4096];
    regex_t r;
    struct timespec a, b;
    cases++;
    memset(s, c, n);
    strcpy(s + n, tail);
    if (regcomp(&r, re, fl)) { printf("FAIL comp %s\n", re); fails++; return; }
    clock_gettime(CLOCK_MONOTONIC, &a);
    int x = regexec(&r, s, 0, 0, 0);
    clock_gettime(CLOCK_MONOTONIC, &b);
    double ms = (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6;
    printf("time /%s/ on %d x '%c'%s: %.2f ms, result %d\n", re, n, c, tail, ms, x);
    if (x != want || ms >= 50) {
        printf("FAIL slow /%s/ n=%d: result %d want %d, %.2f ms\n", re, n, x, want, ms);
        fails++;
    }
    regfree(&r);
}

int main(void) {
    int E = REG_EXTENDED;
    t("abc", 0, "xxabcxx", 2, 5);
    t("a.c", 0, "abc", 0, 3);
    t("^abc", 0, "xabc", -1, -1);
    t("^abc$", 0, "abc", 0, 3);
    t("a*", 0, "aaab", 0, 3);
    t("ba*", 0, "xbaaa", 1, 5);
    t("a\\{2,3\\}", 0, "aaaa", 0, 3);
    t("a{2,3}", E, "aaaa", 0, 3);
    t("a{2}", E, "a aa", 2, 4);
    t("a{2,}", E, "aaaaa", 0, 5);
    t("(ab|abcd)", E, "abcd", 0, 4);
    t("ab|abcd", E, "xabcd", 1, 5);
    t("x(a|b)+y", E, "xababy", 0, 6);
    t("[0-9]+", E, "ab123c", 2, 5);
    t("[[:digit:]]\\+", 0, "ab123c", 2, 5);
    t("[^a-z]", 0, "abC", 2, 3);
    t("\\(a\\)\\1", 0, "xaa", 1, 3);
    t("(a+)b\\1", E, "aaabaa", 1, 6);
    t("\\<foo\\>", 0, "afoo foo", 5, 8);
    t("\\bfoo", 0, "afoo foo", 5, 8);
    t("\\w+", E, "  hi_2 ", 2, 6);
    t("HeLLo", REG_ICASE, "say hello", 4, 9);
    t("(a*)*", E, "b", 0, 0);
    t("(a*)+", E, "aa", 0, 2);
    t("(a|)*b", E, "aab", 0, 3);
    t("a?ab?", 0, "ab", -1, -1);
    t("a\\?ab\\?", 0, "ab", 0, 2);
    t("a?(ab)?", E, "ab", 0, 2);
    t("*a", 0, "*a", 0, 2);
    t("a**", 0, "aa", 0, 2);
    t("^*", 0, "*", 0, 1);
    t("a|b", 0, "a|b", 0, 3);
    t("a\\|b", 0, "b", 0, 1);
    t("$a", 0, "$a", 0, 2);
    t("a$b", 0, "a$b", 0, 3);
    t("a$", E, "ba", 1, 2);
    t("x[]a]y", 0, "x]y", 0, 3);
    t("[a-]", 0, "-", 0, 1);
    t("(", E, "", -2, 0);
    t("\\(", 0, "", -2, 0);
    t("[a", 0, "", -2, 0);
    t("a{3,1}", E, "", -2, 0);
    t(".*", 0, "", 0, 0);
    t("", 0, "abc", 0, 0);
    t("^$", 0, "", 0, 0);
    t("(^a|b)", E, "ca", -1, -1);
    t("(^a|b)", E, "ab", 0, 1);
    t("[.]", 0, "a.", 1, 2);
    t("a.c", REG_NEWLINE, "a\nc", -1, -1);
    t("^b", REG_NEWLINE, "a\nb", 2, 3);
    t("x(ab){2}y", E, "xababy", 0, 6);
    t("((a)|b)*c", E, "abac", 0, 4);
    sub("\\(a*\\)\\(b*\\)", 0, "aabbb", 2, 2, 5);
    sub("(a|b)*", E, "ab", 1, 1, 2);
    sub("x(y)?z", E, "xz", 1, -1, -1);
    t(".*foo.*bar", 0, "aaaaaaaaaaaaaaaaaaaaaaaaaaaafooaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaabar", 0, 87);
    fn(!fnmatch("*.c", "foo.c", 0), "fn1");
    fn(fnmatch("*.c", ".foo.c", FNM_PERIOD) != 0, "fn2");
    fn(!fnmatch("f[a-z]o?", "fooX", 0), "fn3");
    fn(fnmatch("a/*", "a/b/c", FNM_PATHNAME) != 0, "fn4");
    fn(!fnmatch("[!a]x", "bx", 0), "fn5");
    fn(!fnmatch("FOO", "foo", FNM_CASEFOLD), "fn6");
    fn(!fnmatch("\\*", "*", 0), "fn7");

    /* Pathological backtracking (fixed: memoised SPLIT states). */
    slow("(a|aa)*b", E, 'a', 32, "", REG_NOMATCH);
    slow("(a*)*b", E, 'a', 24, "", REG_NOMATCH);
    slow("(a*)*b", E, 'a', 4000, "", REG_NOMATCH);
    slow("(a|aa)*b", E, 'a', 4000, "", REG_NOMATCH);
    slow("(a+)+$", E, 'a', 3000, "!", REG_NOMATCH);
    slow("\\(a*\\)*b", 0, 'a', 30, "", REG_NOMATCH);
    slow("(x+x+)+y", E, 'x', 100, "", REG_NOMATCH);
    slow("(a|aa)*b", E, 'a', 32, "b", 0);
    slow("(a*)*b", E, 'a', 4000, "b", 0);
    /* With a back-reference there is no memo: the step cap stops it. */
    slow("\\(a*\\)*\\1b", 0, 'a', 30, "", REG_ESPACE);
    /* ...and ordinary back-reference patterns still match. */
    t("\\(a*\\)*\\1b", 0, "aab", 0, 3);
    /* Matches after the memo switches on stay leftmost-longest. */
    {
        static char big[3000];
        memset(big, 'x', 2000);
        strcpy(big + 2000, "abcd");
        t("(ab|abcd)", E, big, 2000, 2004);
        t("x*(a|ab)*c", E, "xxxxababababc", 0, 13);
    }
    printf("%d cases, %d failures\n", cases, fails);
    return fails != 0;
}
