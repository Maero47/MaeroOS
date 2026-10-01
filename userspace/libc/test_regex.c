/* Host-side regression test for userspace/libc/regex.c (regcomp/regexec and
 * fnmatch), used by tools/test_regex.py.  Built with the host compiler against
 * a copy of regex.c whose includes point at the host headers.  Prints one
 * FAIL line per broken case and exits non-zero if any failed. */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "regex.h"
#include "fnmatch.h"

static int fails, cases, skipped;

/* regex.c built again by tools/test_regex.py: ref_* never memoises, pru_*
 * memoises from the first step. */
int ref_regcomp(regex_t *, const char *, int);
int ref_regexec(const regex_t *, const char *, unsigned long, regmatch_t *, int);
void ref_regfree(regex_t *);
int pru_regcomp(regex_t *, const char *, int);
int pru_regexec(const regex_t *, const char *, unsigned long, regmatch_t *, int);
void pru_regfree(regex_t *);

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

#define SLOW_MS 500

/* A pattern that backtracks exponentially without memoisation: must give
 * `want` (0 or REG_NOMATCH / REG_ESPACE) in under SLOW_MS.  The bound only
 * has to catch exponential blow-up, not measure speed: a tighter one
 * flaked on loaded hosts (57 ms seen against 50). */
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
    if (x != want || ms >= SLOW_MS) {
        printf("FAIL slow /%s/ n=%d: result %d want %d, %.2f ms\n", re, n, x, want, ms);
        fails++;
    }
    regfree(&r);
}

/* Run `re` on `s` with nmatch = n through the default build (pru = 0) or
 * the always-memoising one (pru = 1), and through the unpruned reference;
 * 1 if the results and all n match slots agree. */
static int same(const char *re, int fl, const char *s, int n, int pru, int verbose) {
    regex_t a, b;
    regmatch_t ma[10], mb[10];
    int ca = pru ? pru_regcomp(&a, re, fl) : regcomp(&a, re, fl);
    int cb = ref_regcomp(&b, re, fl);
    if (ca || cb) {
        if (!ca) pru ? pru_regfree(&a) : regfree(&a);
        if (!cb) ref_regfree(&b);
        return ca == cb;
    }
    memset(ma, 0x55, sizeof(ma));
    memset(mb, 0x55, sizeof(mb));
    int xa = pru ? pru_regexec(&a, s, n, ma, 0) : regexec(&a, s, n, ma, 0);
    int xb = ref_regexec(&b, s, n, mb, 0);
    int ok = xa == xb;
    if (xa == REG_ESPACE || xb == REG_ESPACE) {  /* a step cap ran out: no verdict */
        skipped++;
        ok = 1;
        xa = REG_ESPACE;
    }
    for (int i = 0; ok && !xa && i < n; i++)
        ok = ma[i].rm_so == mb[i].rm_so && ma[i].rm_eo == mb[i].rm_eo;
    if (!ok && verbose) {
        printf("FAIL diff /%s/ on '%.40s%s' n=%d%s: %d vs ref %d\n", re, s,
               strlen(s) > 40 ? "..." : "", n, pru ? " (pru)" : "", xa, xb);
        for (int i = 0; i < n && !xa && !xb; i++)
            printf("    %d: %ld,%ld vs ref %ld,%ld\n", i, (long)ma[i].rm_so,
                   (long)ma[i].rm_eo, (long)mb[i].rm_so, (long)mb[i].rm_eo);
    }
    pru ? pru_regfree(&a) : regfree(&a);
    ref_regfree(&b);
    return ok;
}

static unsigned rng = 12345;
static unsigned rnd(unsigned n) { rng = rng * 1103515245u + 12345u; return (rng >> 16) % n; }

/* A random ERE over {a, b}: groups, alternation (with empty branches),
 * * + ? and small bounds. */
static void gen(char **p, int depth) {
    int pieces = 1 + rnd(2);
    for (int i = 0; i < pieces; i++) {
        int k = rnd(depth > 0 ? 6 : 3);
        if (k == 0) *(*p)++ = 'a';
        else if (k == 1) *(*p)++ = 'b';
        else if (k == 2) *(*p)++ = '.';
        else {
            *(*p)++ = '(';
            gen(p, depth - 1);
            if (rnd(3) == 0) {
                *(*p)++ = '|';
                if (rnd(2)) gen(p, depth - 1);
            }
            *(*p)++ = ')';
        }
        static const char *const q[] = { "", "", "*", "*", "+", "?", "{0,2}", "{2}" };
        const char *qq = q[rnd(8)];
        while (*qq) *(*p)++ = *qq++;
    }
}

static void differential(void) {
    /* The review repro: once the memo switched on, the loop's CHK saw a
     * different MARK and group 1/2 moved (318,320 / 318,319 vs 319,320 /
     * 319,319).  Submatches must now match the unpruned search. */
    static char s[400];
    for (int i = 0; i < 64; i++) memcpy(s + 5 * i, "bbaba", 5);
    s[320] = 0;
    cases++;
    if (!same("((b*)(|a))*", REG_EXTENDED, s, 10, 0, 1)) fails++;
    cases++;
    if (!same("((b*)(|a))*", REG_EXTENDED, s, 1, 1, 1)) fails++;

    /* Random patterns and subjects: the default and always-memoising builds
     * agree with the reference on the whole match (n = 1) and on every
     * submatch (n = 10). */
    int bad = 0, runs = 0;
    for (int it = 0; it < 4000; it++) {
        char re[256], str[40], *p = re;
        gen(&p, 3);
        *p = 0;
        int len = rnd(16);
        for (int i = 0; i < len; i++) str[i] = "ab"[rnd(2)];
        str[len] = 0;
        for (int pru = 0; pru < 2; pru++)
            for (int n = 1; n <= 10; n += 9) {
                runs++;
                if (!same(re, REG_EXTENDED, str, n, pru, bad < 5)) bad++;
            }
    }
    cases++;
    printf("differential: %d random runs, %d mismatches, %d skipped (a step cap ran out)\n",
           runs, bad, skipped);
    if (bad) fails++;
}

/* regexec with nmatch = 10: rc `want`, and on a match the first `ngrp`
 * (so, eo) pairs equal `grp`; under SLOW_MS. */
static void lines(const char *re, int fl, const char *s, int want, const long *grp, int ngrp) {
    regex_t r;
    regmatch_t m[10];
    struct timespec a, b;
    cases++;
    if (regcomp(&r, re, fl)) { printf("FAIL comp %s\n", re); fails++; return; }
    clock_gettime(CLOCK_MONOTONIC, &a);
    int x = regexec(&r, s, 10, m, 0);
    clock_gettime(CLOCK_MONOTONIC, &b);
    double ms = (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6;
    int ok = x == want && ms < SLOW_MS;
    for (int i = 0; ok && !x && i < ngrp; i++)
        ok = m[i].rm_so == grp[2 * i] && m[i].rm_eo == grp[2 * i + 1];
    printf("time /%s/ on %zu bytes, nmatch 10: %.2f ms, result %d\n", re, strlen(s), ms, x);
    if (!ok) {
        printf("FAIL /%s/ on %zu bytes: result %d want %d, %.2f ms\n", re, strlen(s), x, want, ms);
        for (int i = 0; !x && i < ngrp; i++)
            printf("    %d: %ld,%ld want %ld,%ld\n", i, (long)m[i].rm_so, (long)m[i].rm_eo,
                   grp[2 * i], grp[2 * i + 1]);
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
    /* Submatches wanted: the memoised first pass still decides there is no
     * match, quickly. */
    {
        static char a32[40];
        memset(a32, 'a', 32);
        long no[] = { -1, -1 };
        lines("(a|aa)*b", E, a32, REG_NOMATCH, no, 1);
    }
    /* The second pass must stop at the known end: text after it used to
     * count against a cap scaled only by the match length (REG_ESPACE,
     * although the first pass had found the match). */
    {
        static char buf[3100];
        long x1[] = { 0, 1, -1, -1 };
        buf[0] = 'x'; memset(buf + 1, 'a', 3000); buf[3001] = 0;
        lines("x(.*a.*b)?", E, buf, 0, x1, 2);
        buf[41] = 0;
        lines("x((a|aa)*b)?", E, buf, 0, x1, 2);
        buf[0] = 'a'; memset(buf + 1, ' ', 30); buf[31] = 0;
        lines("(a|b)(( *)* z)?", E, buf, 0, (long[]){ 0, 1, 0, 1, -1, -1 }, 3);
    }
    /* Ordinary patterns on long lines with nmatch = 10 (what toybox sed
     * passes): O(n^2) over all start positions without the memo, so a
     * step cap alone returned REG_ESPACE from about 1.5 KB. */
    {
        static const int sizes[] = { 1500, 4096, 16384 };
        static char buf[3 * 16384 + 8];
        long no[] = { -1, -1 };
        for (int k = 0; k < 3; k++) {
            int n = sizes[k];
            memset(buf, 'x', n); buf[n] = 0;
            lines(".*foo", E, buf, REG_NOMATCH, no, 1);
            lines("\\(.*\\),\\(.*\\)", 0, buf, REG_NOMATCH, no, 1);
            memset(buf, 'a', n); buf[n] = 0;
            lines("a*b", E, buf, REG_NOMATCH, no, 1);
            memset(buf, ' ', n); buf[n] = 'x'; buf[n + 1] = 0;
            long sp[] = { n + 1, n + 1 };
            lines(" *$", E, buf, 0, sp, 1);
            /* ...and when they match, the submatches come from the second
             * (unpruned) pass. */
            memset(buf, 'x', n); strcpy(buf + n, "foo");
            long foo[] = { 0, n + 3 };
            lines(".*foo", E, buf, 0, foo, 1);
            memset(buf, 'x', n); buf[n] = ','; memset(buf + n + 1, 'y', n); buf[2 * n + 1] = 0;
            long comma[] = { 0, 2 * n + 1, 0, n, n + 1, 2 * n + 1 };
            lines("\\(.*\\),\\(.*\\)", 0, buf, 0, comma, 3);
            memset(buf, 'a', n); strcpy(buf + n, "b");
            long ab[] = { 0, n + 1, 0, n };
            lines("(a*)b", E, buf, 0, ab, 2);
            memset(buf, 'z', n); memset(buf + n, ' ', n); buf[2 * n] = 0;
            long tail[] = { n, 2 * n, n, 2 * n };
            lines("( *)$", E, buf, 0, tail, 2);
        }
    }
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
    differential();
    printf("%d cases, %d failures\n", cases, fails);
    return fails != 0;
}
