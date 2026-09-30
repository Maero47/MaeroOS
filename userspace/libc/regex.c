/* POSIX regular expressions (basic and extended, plus the common GNU escapes
 * \< \> \b \B \w \W \s \S and BRE \+ \? \|) and fnmatch().
 *
 * regcomp() compiles to a small program that regexec() runs with a
 * backtracking matcher.  Matching is byte-oriented.  POSIX wants the longest
 * of the leftmost matches, so after the first match is found the matcher keeps
 * exploring the remaining alternatives for a longer one.
 *
 * Without back-references, the set of match ends reachable from a SPLIT at
 * (pc, pos) does not depend on the path that reached it, so once a call has
 * spent MEMO_AFTER steps the matcher records visited SPLIT states in a bitmap
 * and never explores one twice: nested loops like (a*)*b stay
 * O(len * pattern) instead of going exponential.  The bitmap is shared by
 * every start position (a state that failed from one start fails from all).
 * With back-references, or when the bitmap would be too large, a per-call
 * step cap applies instead, and a search that exceeds it returns REG_ESPACE
 * (or the longest match found so far).
 *
 * Pruning keeps the whole match (leftmost start, longest end) exact, but not
 * which path reaches it first: a loop's CHK reads its MARK register, so the
 * submatches could differ from an unpruned search.  So when the memo was used
 * and the caller wants submatches, a second pass reruns the unpruned matcher
 * from the known start, accepting only the known end: the first path to reach
 * it carries the same submatches the unpruned search would have reported.
 * That pass has its own cap, scaled by the match length. */
#include "../include/regex.h"
#include "../include/fnmatch.h"
#include "../include/ctype.h"
#include "../include/limits.h"
#include "../include/stdlib.h"
#include "../include/string.h"

enum {
    OP_CHAR, OP_ANY, OP_ANYNL, OP_SET, OP_BOL, OP_EOL, OP_WORDB, OP_NWORDB,
    OP_WBEG, OP_WEND, OP_SPLIT, OP_JMP, OP_SAVE, OP_BREF, OP_MARK, OP_CHK,
    OP_MATCH,
};

/* SPLIT: try pc+x first, then pc+y.  JMP: pc+x.  SAVE/BREF/MARK/CHK: slot x. */
typedef struct { unsigned char op; unsigned char c; short pad; int x, y; } inst_t;

struct re_prog {
    inst_t *code;
    int len, cap;
    unsigned char (*sets)[32];
    int nsets;
    int nmarks;
    int nsub;
    int cflags;
    int anchored;       /* starts with ^ outside REG_NEWLINE: try offset 0 only */
};

typedef struct {
    const char *p;
    struct re_prog *prog;
    int cflags;
    int err;
    int depth;
} parser_t;

static int emit(parser_t *ps, int op, int c, int x, int y) {
    struct re_prog *g = ps->prog;
    if (g->len == g->cap) {
        int ncap = g->cap ? g->cap * 2 : 64;
        inst_t *n = realloc(g->code, ncap * sizeof(inst_t));
        if (!n) { ps->err = REG_ESPACE; return -1; }
        g->code = n;
        g->cap = ncap;
    }
    inst_t *i = &g->code[g->len];
    i->op = (unsigned char)op;
    i->c = (unsigned char)c;
    i->pad = 0;
    i->x = x;
    i->y = y;
    return g->len++;
}

/* Insert n blank instructions at `at`.  Jumps are pc-relative, so code that
 * moves as a block keeps working. */
static int insert(parser_t *ps, int at, int n) {
    for (int i = 0; i < n; i++) if (emit(ps, OP_JMP, 0, 1, 0) < 0) return -1;
    struct re_prog *g = ps->prog;
    memmove(&g->code[at + n], &g->code[at], (g->len - n - at) * sizeof(inst_t));
    return 0;
}

static int fold(int c) { return tolower(c); }

static void set_add(unsigned char *set, int c, int icase) {
    set[c >> 3] |= 1 << (c & 7);
    if (icase) {
        int o = isupper(c) ? tolower(c) : toupper(c);
        set[o >> 3] |= 1 << (o & 7);
    }
}

static int class_match(const char *name, int len, int c) {
#define CL(s, e) if (len == (int)sizeof(s) - 1 && !memcmp(name, s, len)) return (e);
    CL("alpha", isalpha(c)) CL("digit", isdigit(c)) CL("alnum", isalnum(c))
    CL("upper", isupper(c)) CL("lower", islower(c)) CL("space", isspace(c))
    CL("blank", c == ' ' || c == '\t') CL("punct", ispunct(c))
    CL("print", isprint(c)) CL("graph", isprint(c) && c != ' ')
    CL("cntrl", iscntrl(c)) CL("xdigit", isxdigit(c))
#undef CL
    return -1;
}

static int new_set(parser_t *ps) {
    struct re_prog *g = ps->prog;
    void *n = realloc(g->sets, (g->nsets + 1) * 32);
    if (!n) { ps->err = REG_ESPACE; return -1; }
    g->sets = n;
    memset(g->sets[g->nsets], 0, 32);
    return g->nsets++;
}

static int parse_bracket(parser_t *ps) {
    int idx = new_set(ps), neg = 0, first = 1, icase = ps->cflags & REG_ICASE;
    if (idx < 0) return -1;
    unsigned char set[32];
    memset(set, 0, 32);
    if (*ps->p == '^') { neg = 1; ps->p++; }
    for (;;) {
        int c = (unsigned char)*ps->p, lo;
        if (!c) { ps->err = REG_EBRACK; return -1; }
        if (c == ']' && !first) { ps->p++; break; }
        first = 0;
        if (c == '[' && (ps->p[1] == ':' || ps->p[1] == '=' || ps->p[1] == '.')) {
            char kind = ps->p[1];
            const char *s = ps->p + 2, *e = s;
            while (*e && !(e[0] == kind && e[1] == ']')) e++;
            if (!*e) { ps->err = REG_EBRACK; return -1; }
            ps->p = e + 2;
            if (kind == ':') {
                if (class_match(s, (int)(e - s), 'a') < 0) { ps->err = REG_ECTYPE; return -1; }
                for (int ch = 1; ch < 256; ch++)
                    if (class_match(s, (int)(e - s), ch) > 0) set_add(set, ch, icase);
                continue;
            }
            if (e - s != 1) { ps->err = REG_ECOLLATE; return -1; }
            lo = (unsigned char)*s;
        } else {
            lo = c;
            ps->p++;
        }
        if (ps->p[0] == '-' && ps->p[1] && ps->p[1] != ']') {
            int hi = (unsigned char)ps->p[1];
            ps->p += 2;
            if (hi == '[' && ps->p[0] == '.' ) {
                const char *s = ps->p + 1;
                if (s[0] && s[1] == '.' && s[2] == ']') { hi = (unsigned char)s[0]; ps->p = s + 3; }
            }
            if (hi < lo) { ps->err = REG_ERANGE; return -1; }
            for (int ch = lo; ch <= hi; ch++) set_add(set, ch, icase);
        } else set_add(set, lo, icase);
    }
    if (neg) {
        for (int i = 0; i < 32; i++) set[i] = (unsigned char)~set[i];
        set[0] &= ~1;                                /* never NUL */
        if (ps->cflags & REG_NEWLINE) set['\n' >> 3] &= ~(1 << ('\n' & 7));
    }
    memcpy(ps->prog->sets[idx], set, 32);
    return emit(ps, OP_SET, 0, idx, 0);
}

static int parse_alt(parser_t *ps);

/* An escape that stands for a set: \w \W \s \S. */
static int class_escape(parser_t *ps, int c) {
    int idx = new_set(ps);
    if (idx < 0) return -1;
    for (int ch = 1; ch < 256; ch++) {
        int in = (c | 32) == 'w' ? (isalnum(ch) || ch == '_') : isspace(ch);
        if (isupper(c)) in = !in;
        if (in) ps->prog->sets[idx][ch >> 3] |= 1 << (ch & 7);
    }
    return emit(ps, OP_SET, 0, idx, 0);
}

/* Parse one atom; returns 1 if one was parsed, 0 at the end of a branch. */
static int parse_atom(parser_t *ps, int at_start) {
    int ere = ps->cflags & REG_EXTENDED, icase = ps->cflags & REG_ICASE;
    int c = (unsigned char)*ps->p;

    if (!c) return 0;
    if (ere) {
        if (c == '|' || (c == ')' && ps->depth)) return 0;
        if (c == '*' || c == '+' || c == '?') {
            if (at_start) { ps->p++; return emit(ps, OP_CHAR, c, 0, 0) < 0 ? -1 : 1; }
            ps->err = REG_BADRPT;
            return -1;
        }
        if (c == '{' && at_start) { ps->err = REG_BADRPT; return -1; }
    } else {
        if (c == '\\' && (ps->p[1] == '|' || (ps->p[1] == ')' && ps->depth))) return 0;
        if (c == '*' && at_start) { ps->p++; return emit(ps, OP_CHAR, '*', 0, 0) < 0 ? -1 : 1; }
    }
    ps->p++;
    switch (c) {
    case '.':
        return emit(ps, (ps->cflags & REG_NEWLINE) ? OP_ANYNL : OP_ANY, 0, 0, 0) < 0 ? -1 : 1;
    case '[':
        return parse_bracket(ps) < 0 ? -1 : 1;
    case '^':
        if (ere || at_start) return emit(ps, OP_BOL, 0, 0, 0) < 0 ? -1 : 1;
        break;
    case '$':
        if (ere || !*ps->p || (ps->p[0] == '\\' && (ps->p[1] == ')' || ps->p[1] == '|')))
            return emit(ps, OP_EOL, 0, 0, 0) < 0 ? -1 : 1;
        break;
    case '(':
        if (!ere) break;
    group: {
        int n = ++ps->prog->nsub;
        if (emit(ps, OP_SAVE, 0, 2 * n, 0) < 0) return -1;
        ps->depth++;
        if (parse_alt(ps) < 0) return -1;
        ps->depth--;
        if (ere ? *ps->p != ')' : (ps->p[0] != '\\' || ps->p[1] != ')')) {
            ps->err = REG_EPAREN;
            return -1;
        }
        ps->p += ere ? 1 : 2;
        return emit(ps, OP_SAVE, 0, 2 * n + 1, 0) < 0 ? -1 : 1;
    }
    case '\\':
        c = (unsigned char)*ps->p++;
        if (!c) { ps->err = REG_EESCAPE; return -1; }
        if (!ere && c == '(') goto group;
        if (!ere && (c == '{' || c == '}' || c == ')')) {
            if (c == '{' && !at_start) { ps->err = REG_BADRPT; return -1; }
            if (c == ')') { ps->err = REG_EPAREN; return -1; }
            break;
        }
        if (c >= '1' && c <= '9') {
            if (c - '0' > ps->prog->nsub) { ps->err = REG_ESUBREG; return -1; }
            return emit(ps, OP_BREF, 0, c - '0', 0) < 0 ? -1 : 1;
        }
        switch (c) {
        case '<': return emit(ps, OP_WBEG, 0, 0, 0) < 0 ? -1 : 1;
        case '>': return emit(ps, OP_WEND, 0, 0, 0) < 0 ? -1 : 1;
        case 'b': return emit(ps, OP_WORDB, 0, 0, 0) < 0 ? -1 : 1;
        case 'B': return emit(ps, OP_NWORDB, 0, 0, 0) < 0 ? -1 : 1;
        case 'w': case 'W': case 's': case 'S':
            return class_escape(ps, c) < 0 ? -1 : 1;
        case 'n': c = '\n'; break;
        case 't': c = '\t'; break;
        }
        break;
    }
    if (icase && isalpha(c)) {
        int idx = new_set(ps);
        if (idx < 0) return -1;
        set_add(ps->prog->sets[idx], c, 1);
        return emit(ps, OP_SET, 0, idx, 0) < 0 ? -1 : 1;
    }
    return emit(ps, OP_CHAR, c, 0, 0) < 0 ? -1 : 1;
}

/* Wrap [a, end) in a loop: L: SPLIT body,out; MARK k; body; CHK k; JMP L. */
static int make_star(parser_t *ps, int a) {
    int k = ps->prog->nmarks++;
    if (insert(ps, a, 2) < 0) return -1;
    int body_end = ps->prog->len;
    if (emit(ps, OP_CHK, 0, k, 0) < 0 || emit(ps, OP_JMP, 0, a - (body_end + 1), 0) < 0)
        return -1;
    inst_t *s = &ps->prog->code[a];
    s[0].op = OP_SPLIT; s[0].x = 1; s[0].y = ps->prog->len - a;
    s[1].op = OP_MARK;  s[1].x = k;
    return 0;
}

static int make_opt(parser_t *ps, int a) {
    if (insert(ps, a, 1) < 0) return -1;
    inst_t *s = &ps->prog->code[a];
    s->op = OP_SPLIT; s->x = 1; s->y = ps->prog->len - a;
    return 0;
}

/* Append a copy of body[0..blen); with `remap`, its loops get fresh MARK
 * slots (lo..hi become nmarks..). */
static int emit_copy(parser_t *ps, const inst_t *body, int blen, int remap, int lo, int span) {
    int base = ps->prog->nmarks;
    for (int j = 0; j < blen; j++) {
        int x = body[j].x;
        if (remap && span && (body[j].op == OP_MARK || body[j].op == OP_CHK)) x = x - lo + base;
        if (emit(ps, body[j].op, body[j].c, x, body[j].y) < 0) return -1;
    }
    if (remap) ps->prog->nmarks += span;
    return 0;
}

/* Apply `{m,n}` (n < 0: unbounded) to the atom at [a, end). */
static int make_count(parser_t *ps, int a, int m, int n) {
    int end = ps->prog->len, blen = end - a, lo = 1 << 30, hi = -1, r = 0, copies = 0;
    inst_t *body = malloc(blen * sizeof(inst_t) + 1);
    if (!body) { ps->err = REG_ESPACE; return -1; }
    memcpy(body, &ps->prog->code[a], blen * sizeof(inst_t));
    for (int j = 0; j < blen; j++) if (body[j].op == OP_MARK) {
        if (body[j].x < lo) lo = body[j].x;
        if (body[j].x > hi) hi = body[j].x;
    }
    int span = hi >= lo ? hi - lo + 1 : 0;
    ps->prog->len = a;
    for (int i = 0; i < m && !r; i++)
        r = emit_copy(ps, body, blen, copies++ > 0, lo, span) < 0;
    if (n < 0 && !r) {
        int at = ps->prog->len;
        r = emit_copy(ps, body, blen, copies++ > 0, lo, span) < 0;
        if (!r) r = make_star(ps, at) < 0;
    } else for (int i = m; i < n && !r; i++) {
        int at = ps->prog->len;
        r = emit_copy(ps, body, blen, copies++ > 0, lo, span) < 0;
        if (!r) r = make_opt(ps, at) < 0;
    }
    free(body);
    return r ? -1 : 0;
}

static int parse_brace(parser_t *ps, int *m, int *n) {
    int ere = ps->cflags & REG_EXTENDED;
    const char *p = ps->p;
    if (!isdigit((unsigned char)*p)) { ps->err = REG_BADBR; return -1; }
    *m = 0;
    while (isdigit((unsigned char)*p)) *m = *m * 10 + (*p++ - '0');
    *n = *m;
    if (*p == ',') {
        p++;
        if (isdigit((unsigned char)*p)) {
            *n = 0;
            while (isdigit((unsigned char)*p)) *n = *n * 10 + (*p++ - '0');
        } else *n = -1;
    }
    if (ere ? *p != '}' : (p[0] != '\\' || p[1] != '}')) { ps->err = REG_EBRACE; return -1; }
    ps->p = p + (ere ? 1 : 2);
    if (*m > RE_DUP_MAX || *n > RE_DUP_MAX || (*n >= 0 && *n < *m)) {
        ps->err = REG_BADBR;
        return -1;
    }
    return 0;
}

static int parse_branch(parser_t *ps) {
    int ere = ps->cflags & REG_EXTENDED, at_start = 1;
    for (;;) {
        int a = ps->prog->len, r = parse_atom(ps, at_start);
        if (r <= 0) return r;
        /* A leading ^ in a BRE leaves `*` right after it literal too. */
        at_start = ps->prog->code[a].op == OP_BOL && ps->prog->len == a + 1;
        for (; !(at_start && !ere);) {
            const char *p = ps->p;
            int m, n;
            if (*p == '*') { ps->p++; if (make_star(ps, a) < 0) return -1; }
            else if (ere && *p == '+') { ps->p++; m = 1; n = -1; goto count; }
            else if (ere && *p == '?') { ps->p++; if (make_opt(ps, a) < 0) return -1; }
            else if (!ere && p[0] == '\\' && p[1] == '+') { ps->p += 2; m = 1; n = -1; goto count; }
            else if (!ere && p[0] == '\\' && p[1] == '?') { ps->p += 2; if (make_opt(ps, a) < 0) return -1; }
            else if ((ere && *p == '{' && isdigit((unsigned char)p[1])) ||
                     (!ere && p[0] == '\\' && p[1] == '{')) {
                ps->p += ere ? 1 : 2;
                if (parse_brace(ps, &m, &n) < 0) return -1;
            count:
                if (make_count(ps, a, m, n) < 0) return -1;
            } else break;
        }
    }
}

static int parse_alt(parser_t *ps) {
    int ere = ps->cflags & REG_EXTENDED;
    int start = ps->prog->len, prev_jmp = -1;
    if (parse_branch(ps) < 0) return -1;
    while (ere ? *ps->p == '|' : (ps->p[0] == '\\' && ps->p[1] == '|')) {
        ps->p += ere ? 1 : 2;
        /* start: SPLIT +1, next ; branch ; JMP end ; next: ... */
        if (insert(ps, start, 1) < 0) return -1;
        int j = emit(ps, OP_JMP, 0, 0, 0);
        if (j < 0) return -1;
        inst_t *s = &ps->prog->code[start];
        s->op = OP_SPLIT; s->x = 1; s->y = ps->prog->len - start;
        /* Chain the previous branch's end jump to this one; patched below. */
        ps->prog->code[j].y = prev_jmp;
        prev_jmp = j;
        start = ps->prog->len;
        if (parse_branch(ps) < 0) return -1;
    }
    /* Every branch's trailing JMP goes to the end of the alternation. */
    for (int j = prev_jmp; j >= 0;) {
        int next = ps->prog->code[j].y;
        ps->prog->code[j].x = ps->prog->len - j;
        ps->prog->code[j].y = 0;
        j = next;
    }
    return 0;
}

int regcomp(regex_t *preg, const char *regex, int cflags) {
    struct re_prog *g = calloc(1, sizeof(*g));
    parser_t ps = { regex, g, cflags, 0, 0 };

    preg->__prog = 0;
    preg->re_nsub = 0;
    if (!g) return REG_ESPACE;
    g->cflags = cflags;
    if (emit(&ps, OP_SAVE, 0, 0, 0) >= 0 && parse_alt(&ps) >= 0 && !ps.err) {
        if (*ps.p) ps.err = REG_EPAREN;
        else {
            emit(&ps, OP_SAVE, 0, 1, 0);
            emit(&ps, OP_MATCH, 0, 0, 0);
        }
    }
    if (!ps.err) {
        g->anchored = !(cflags & REG_NEWLINE) && g->len > 1 && g->code[1].op == OP_BOL;
        preg->__prog = g;
        preg->re_nsub = g->nsub;
        return 0;
    }
    if (!ps.err) ps.err = REG_BADPAT;
    free(g->code);
    free(g->sets);
    free(g);
    return ps.err;
}

void regfree(regex_t *preg) {
    struct re_prog *g = preg->__prog;
    if (!g) return;
    free(g->code);
    free(g->sets);
    free(g);
    preg->__prog = 0;
}

/* ── matcher ────────────────────────────────────────────────────────────── */

typedef struct { int pc; int kind; int slot; long val; } bt_t;   /* kind 0: branch */

typedef struct {
    const char *s;
    long start, end;        /* the searchable range (REG_STARTEND) */
    int eflags;
    struct re_prog *g;
    long *regs;             /* capture slots, then MARK slots */
    long *best;
    int have_best;
    bt_t *stk;
    int sp, cap;
    long steps;             /* over the whole regexec() call */
    unsigned char *seen;    /* visited SPLIT states, or 0 */
    int *split_ix;          /* pc -> SPLIT number */
    int memo;               /* 1: may use `seen`; 0: never; -1: over step cap */
    long want_end;          /* >= 0: second pass, accept only this end */
    long cap_steps;
} matcher_t;

/* Overridable so tools/test_regex.py can build unpruned and always-pruned
 * variants to compare against. */
#ifndef MEMO_AFTER
#define MEMO_AFTER     4096L
#endif
#define MEMO_MAX_BITS  (32L << 20)
#ifndef STEP_CAP
#define STEP_CAP       (4L << 20)    /* plus 16 steps per (pc, pos) pair */
#endif

/* Allocate the visited bitmap; 0 if memoisation is not possible. */
static int memo_init(matcher_t *m) {
    struct re_prog *g = m->g;
    long nsplit = 0, span = m->end - m->start + 1;
    for (int i = 0; i < g->len; i++) {
        if (g->code[i].op == OP_BREF) return 0;
        if (g->code[i].op == OP_SPLIT) nsplit++;
    }
    if (!nsplit || nsplit > MEMO_MAX_BITS / span) return 0;
    m->split_ix = malloc(g->len * sizeof(int));
    m->seen = calloc((nsplit * span + 7) / 8, 1);
    if (!m->split_ix || !m->seen) {
        free(m->split_ix);
        free(m->seen);
        m->split_ix = 0;
        m->seen = 0;
        return 0;
    }
    for (int i = 0, k = 0; i < g->len; i++) m->split_ix[i] = g->code[i].op == OP_SPLIT ? k++ : -1;
    return 1;
}

static int is_word(int c) { return isalnum(c) || c == '_'; }

static int push(matcher_t *m, int pc, int kind, int slot, long val) {
    if (m->sp == m->cap) {
        int ncap = m->cap ? m->cap * 2 : 256;
        bt_t *n = realloc(m->stk, ncap * sizeof(bt_t));
        if (!n) return -1;
        m->stk = n;
        m->cap = ncap;
    }
    bt_t *b = &m->stk[m->sp++];
    b->pc = pc;
    b->kind = kind;
    b->slot = slot;
    b->val = val;
    return 0;
}

/* Run from `pos`; 1 = some match recorded in m->best. */
static int run(matcher_t *m, long pos) {
    struct re_prog *g = m->g;
    int nslots = 2 * (g->nsub + 1) + g->nmarks, pc = 0;

    for (int i = 0; i < nslots; i++) m->regs[i] = -1;
    m->sp = 0;
    m->have_best = 0;
    for (;;) {
        inst_t *in = &g->code[pc];
        int ok = 1;
        long c = pos < m->end ? (unsigned char)m->s[pos] : -1;
        long pc_prev = pos > 0 ? (unsigned char)m->s[pos - 1] : -1;
        int bol = (pos == m->start && !(m->eflags & REG_NOTBOL)) ||
                  ((g->cflags & REG_NEWLINE) && pc_prev == '\n' && pos > m->start);
        int eol = (pos == m->end && !(m->eflags & REG_NOTEOL)) ||
                  ((g->cflags & REG_NEWLINE) && c == '\n');

        if (++m->steps == MEMO_AFTER && m->memo > 0) {
            m->memo = memo_init(m);
        }
        if (!m->seen && m->steps > m->cap_steps) { m->memo = -1; return m->have_best; }
        switch (in->op) {
        case OP_CHAR:  ok = c == in->c; if (ok) pos++; break;
        case OP_ANY:   ok = c > 0; if (ok) pos++; break;
        case OP_ANYNL: ok = c > 0 && c != '\n'; if (ok) pos++; break;
        case OP_SET:   ok = c > 0 && (g->sets[in->x][c >> 3] & (1 << (c & 7))); if (ok) pos++; break;
        case OP_BOL:   ok = bol; break;
        case OP_EOL:   ok = eol; break;
        case OP_WORDB: case OP_NWORDB: case OP_WBEG: case OP_WEND: {
            int a = pos > m->start && is_word((int)pc_prev), b = c >= 0 && is_word((int)c);
            ok = in->op == OP_WORDB ? a != b : in->op == OP_NWORDB ? a == b
               : in->op == OP_WBEG ? !a && b : a && !b;
            break;
        }
        case OP_SPLIT:
            if (m->seen) {
                long bit = (long)m->split_ix[pc] * (m->end - m->start + 1) + (pos - m->start);
                if (m->seen[bit >> 3] & (1 << (bit & 7))) { ok = 0; break; }
                m->seen[bit >> 3] |= 1 << (bit & 7);
            }
            if (push(m, pc + in->y, 0, 0, pos) < 0) return -1;
            pc += in->x;
            continue;
        case OP_JMP:
            pc += in->x;
            continue;
        case OP_SAVE: case OP_MARK: {
            int slot = in->op == OP_SAVE ? in->x : 2 * (g->nsub + 1) + in->x;
            if (push(m, 0, 1, slot, m->regs[slot]) < 0) return -1;
            m->regs[slot] = pos;
            break;
        }
        case OP_CHK:
            /* A loop body that matched nothing must not go round again. */
            ok = m->regs[2 * (g->nsub + 1) + in->x] != pos;
            break;
        case OP_BREF: {
            long so = m->regs[2 * in->x], eo = m->regs[2 * in->x + 1];
            if (so < 0 || eo < 0) { ok = 0; break; }
            if (pos + (eo - so) > m->end) { ok = 0; break; }
            for (long i = 0; i < eo - so && ok; i++) {
                int x = (unsigned char)m->s[so + i], y = (unsigned char)m->s[pos + i];
                ok = (g->cflags & REG_ICASE) ? fold(x) == fold(y) : x == y;
            }
            if (ok) pos += eo - so;
            break;
        }
        case OP_MATCH:
            if (m->want_end >= 0) {
                if (pos != m->want_end) { ok = 0; break; }
                memcpy(m->best, m->regs, 2 * (g->nsub + 1) * sizeof(long));
                m->best[1] = pos;
                return m->have_best = 1;
            }
            if (!m->have_best || pos > m->best[1]) {
                memcpy(m->best, m->regs, 2 * (g->nsub + 1) * sizeof(long));
                m->best[1] = pos;
                m->have_best = 1;
            }
            if (pos == m->end) return 1;        /* nothing can be longer */
            ok = 0;
            break;
        }
        if (ok) { pc++; continue; }
        /* Backtrack: undo slot writes until a pending branch. */
        for (;;) {
            if (!m->sp) return m->have_best;
            bt_t *b = &m->stk[--m->sp];
            if (b->kind) { m->regs[b->slot] = b->val; continue; }
            pc = b->pc;
            pos = b->val;
            break;
        }
    }
    return m->have_best;
}

/* Steps a search over `pairs` positions may take without the memo. */
static long step_cap(long pairs, int len) {
    return pairs > (LONG_MAX / 32) / len ? LONG_MAX : STEP_CAP + 16 * pairs * len;
}

int regexec(const regex_t *preg, const char *string, unsigned long nmatch,
            regmatch_t pmatch[], int eflags) {
    struct re_prog *g = preg->__prog;
    matcher_t m;
    int r = REG_NOMATCH;

    if (!g) return REG_BADPAT;
    memset(&m, 0, sizeof(m));
    m.s = string;
    m.g = g;
    m.eflags = eflags;
    m.memo = 1;
    m.want_end = -1;
    if ((eflags & REG_STARTEND) && pmatch) {
        m.start = pmatch[0].rm_so;
        m.end = pmatch[0].rm_eo;
    } else m.end = (long)strlen(string);
    m.cap_steps = step_cap(m.end - m.start + 1, g->len);
    int nslots = 2 * (g->nsub + 1) + g->nmarks;
    m.regs = malloc(nslots * sizeof(long));
    m.best = malloc(2 * (g->nsub + 1) * sizeof(long));
    if (!m.regs || !m.best) { r = REG_ESPACE; goto out; }

    /* The first instruction after SAVE 0: a literal lets us skip ahead. */
    int lit = g->len > 1 && g->code[1].op == OP_CHAR ? g->code[1].c : -1;
    for (long pos = m.start; pos <= m.end; pos++) {
        if (lit >= 0) {
            const char *f = memchr(string + pos, lit, m.end - pos);
            if (!f) break;
            pos = f - string;
        }
        int got = run(&m, pos);
        if (got < 0) { r = REG_ESPACE; break; }
        if (!got && m.memo < 0) { r = REG_ESPACE; break; }
        if (got && m.seen && nmatch > 1 && pmatch && !(g->cflags & REG_NOSUB)) {
            /* Second pass for the submatches: unpruned, from `pos`, and
             * ending exactly where the pruned search found the match. */
            long end = m.best[1];
            free(m.seen);
            m.seen = 0;
            m.memo = 0;
            m.want_end = end;
            m.steps = 0;
            m.cap_steps = step_cap(end - pos + 1, g->len);
            got = run(&m, pos);
            if (got <= 0) { r = REG_ESPACE; break; }
        }
        if (got) {
            r = 0;
            if (!(g->cflags & REG_NOSUB) && pmatch) {
                for (unsigned long i = 0; i < nmatch; i++) {
                    if (i <= (unsigned long)g->nsub && m.best[2 * i] >= 0 && m.best[2 * i + 1] >= 0) {
                        pmatch[i].rm_so = m.best[2 * i];
                        pmatch[i].rm_eo = m.best[2 * i + 1];
                    } else pmatch[i].rm_so = pmatch[i].rm_eo = -1;
                }
            }
            break;
        }
        if (g->anchored) break;
    }
out:
    free(m.regs);
    free(m.best);
    free(m.stk);
    free(m.seen);
    free(m.split_ix);
    return r;
}

unsigned long regerror(int errcode, const regex_t *preg, char *errbuf,
                       unsigned long errbuf_size) {
    static const char *const msgs[] = {
        "Success", "No match", "Invalid regular expression",
        "Invalid collation character", "Invalid character class name",
        "Trailing backslash", "Invalid back reference", "Unmatched [ or [^",
        "Unmatched ( or \\(", "Unmatched \\{", "Invalid content of \\{\\}",
        "Invalid range end", "Out of memory",
        "Invalid preceding regular expression",
    };
    (void)preg;
    const char *msg = errcode >= 0 && errcode < (int)(sizeof(msgs) / sizeof(*msgs))
                      ? msgs[errcode] : "Unknown error";
    unsigned long len = strlen(msg) + 1;
    if (errbuf && errbuf_size) {
        unsigned long n = len < errbuf_size ? len : errbuf_size;
        memcpy(errbuf, msg, n - 1);
        errbuf[n - 1] = 0;
    }
    return len;
}

/* ── fnmatch ────────────────────────────────────────────────────────────── */

static int fn_fold(int c, int flags) { return (flags & FNM_CASEFOLD) ? tolower(c) : c; }

/* Match a [...] expression at *pp against c; advances *pp past it.
 * Returns 1/0, or -1 if it is not a valid bracket (then '[' is literal). */
static int fn_bracket(const char **pp, int c, int flags) {
    const char *p = *pp + 1;
    int neg = 0, hit = 0, first = 1;
    if (*p == '!' || *p == '^') { neg = 1; p++; }
    for (;; first = 0) {
        int lo = (unsigned char)*p;
        if (!lo) return -1;
        if (lo == ']' && !first) { p++; break; }
        if (lo == '[' && p[1] == ':') {
            const char *e = strstr(p + 2, ":]");
            if (!e) return -1;
            if (class_match(p + 2, (int)(e - p - 2), c) > 0) hit = 1;
            p = e + 2;
            continue;
        }
        if (lo == '\\' && !(flags & FNM_NOESCAPE) && p[1]) lo = (unsigned char)*++p;
        p++;
        if (p[0] == '-' && p[1] && p[1] != ']') {
            int hi = (unsigned char)p[1];
            if (hi == '\\' && !(flags & FNM_NOESCAPE) && p[2]) hi = (unsigned char)*++p;
            p += 2;
            if (fn_fold(c, flags) >= fn_fold(lo, flags) && fn_fold(c, flags) <= fn_fold(hi, flags)) hit = 1;
            if (c >= lo && c <= hi) hit = 1;
        } else if (fn_fold(c, flags) == fn_fold(lo, flags)) hit = 1;
    }
    *pp = p;
    return hit != neg;
}

static int fn_match(const char *p, const char *s, const char *s0, int flags) {
    for (;;) {
        int c = (unsigned char)*p;
        if (!c) {
            if (!*s) return 0;
            return ((flags & FNM_LEADING_DIR) && *s == '/') ? 0 : FNM_NOMATCH;
        }
        int leading = *s == '.' && (flags & FNM_PERIOD) &&
                      (s == s0 || ((flags & FNM_PATHNAME) && s[-1] == '/'));
        if (c == '*') {
            while (*p == '*') p++;
            if (leading) return FNM_NOMATCH;
            if (!*p) {
                if (!(flags & FNM_PATHNAME) || (flags & FNM_LEADING_DIR)) return 0;
                return strchr(s, '/') ? FNM_NOMATCH : 0;
            }
            for (const char *t = s; ; t++) {
                if (!fn_match(p, t, s0, flags)) return 0;
                if (!*t || ((flags & FNM_PATHNAME) && *t == '/')) return FNM_NOMATCH;
            }
        }
        if (!*s) return FNM_NOMATCH;
        if (c == '?') {
            if (((flags & FNM_PATHNAME) && *s == '/') || leading) return FNM_NOMATCH;
            p++;
            s++;
            continue;
        }
        if (c == '[') {
            const char *q = p;
            if (((flags & FNM_PATHNAME) && *s == '/') || leading) return FNM_NOMATCH;
            int r = fn_bracket(&q, (unsigned char)*s, flags);
            if (r >= 0) {
                if (!r) return FNM_NOMATCH;
                p = q;
                s++;
                continue;
            }
        }
        if (c == '\\' && !(flags & FNM_NOESCAPE) && p[1]) c = (unsigned char)*++p;
        if (fn_fold(c, flags) != fn_fold((unsigned char)*s, flags)) return FNM_NOMATCH;
        p++;
        s++;
    }
}

int fnmatch(const char *pattern, const char *string, int flags) {
    return fn_match(pattern, string, string, flags);
}
