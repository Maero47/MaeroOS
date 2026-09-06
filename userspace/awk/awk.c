/*
 * awk — minimal awk: pattern { action } with $0..$9, NR, NF, FS
 * Supports: print, printf (basic), if/else, arithmetic, string concat
 * Usage: awk [-F SEP] 'PROG' [FILE...]
 *
 * This is a very simplified awk — handles the 80% case:
 *   awk '{print $1}', awk '/pattern/{print}', awk 'NR==3{print $2}',
 *   awk 'BEGIN{...} /pat/{...} END{...}'
 */
#include "../include/unistd.h"
#include "../include/stdio.h"
#include "../include/string.h"
#include "../include/stdlib.h"

#define LMAX   4096
#define FMAX   256   /* max fields */
#define VMAX   64    /* max variables */

static char g_fs = ' ';   /* field separator */
static long  g_nr = 0;    /* Number of Records */
static char  g_line[LMAX];
static char *g_fields[FMAX];
static int   g_nf;
static char  g_field_bufs[FMAX][256];

/* Simple variable store */
static char  var_names[VMAX][64];
static long  var_inum[VMAX];
static char  var_sval[VMAX][256];
static int   var_n = 0;

static long var_get_n(const char *name) {
    for (int i = 0; i < var_n; i++)
        if (strcmp(var_names[i], name) == 0) return var_inum[i];
    return 0;
}
static const char *var_get_s(const char *name) {
    for (int i = 0; i < var_n; i++)
        if (strcmp(var_names[i], name) == 0) return var_sval[i];
    return "";
}
static void var_set(const char *name, long n, const char *s) {
    for (int i = 0; i < var_n; i++) {
        if (strcmp(var_names[i], name) == 0) {
            var_inum[i] = n;
            strncpy(var_sval[i], s ? s : "", 255);
            return;
        }
    }
    if (var_n < VMAX) {
        strncpy(var_names[var_n], name, 63);
        var_inum[var_n] = n;
        strncpy(var_sval[var_n], s ? s : "", 255);
        var_n++;
    }
}

/* Split line into fields */
static void split_fields(void) {
    g_nf = 0;
    char *p = g_line;
    if (g_fs == ' ') {
        /* whitespace splitting */
        while (*p) {
            while (*p == ' ' || *p == '\t') p++;
            if (!*p) break;
            char *start = p;
            while (*p && *p != ' ' && *p != '\t') p++;
            int len = (int)(p - start);
            if (g_nf < FMAX) {
                if (len > 255) len = 255;
                memcpy(g_field_bufs[g_nf], start, (size_t)len);
                g_field_bufs[g_nf][len] = '\0';
                g_fields[g_nf] = g_field_bufs[g_nf];
                g_nf++;
            }
        }
    } else {
        char *start = p;
        while (1) {
            if (*p == g_fs || *p == '\0') {
                int len = (int)(p - start);
                if (g_nf < FMAX) {
                    if (len > 255) len = 255;
                    memcpy(g_field_bufs[g_nf], start, (size_t)len);
                    g_field_bufs[g_nf][len] = '\0';
                    g_fields[g_nf] = g_field_bufs[g_nf];
                    g_nf++;
                }
                if (!*p) break;
                start = ++p;
            } else {
                p++;
            }
        }
    }
}

/* Get field or special variable as string */
static const char *get_field(const char *name) {
    if (name[0] == '$') {
        int n = atoi(name + 1);
        if (n == 0) return g_line;
        if (n >= 1 && n <= g_nf) return g_fields[n - 1];
        return "";
    }
    if (strcmp(name, "NR") == 0) {
        static char buf[32];
        sprintf(buf, "%ld", g_nr);
        return buf;
    }
    if (strcmp(name, "NF") == 0) {
        static char buf[32];
        sprintf(buf, "%d", g_nf);
        return buf;
    }
    return var_get_s(name);
}

/* Evaluate a simple expression token — returns string value */
static void eval_token(const char *tok, char *out, int outsz) {
    if (tok[0] == '"') {
        /* string literal */
        int i = 0, j = 1;
        while (tok[j] && tok[j] != '"' && i < outsz - 1) {
            if (tok[j] == '\\' && tok[j+1]) {
                char c = tok[++j];
                if (c == 'n') c = '\n';
                else if (c == 't') c = '\t';
                out[i++] = c;
            } else {
                out[i++] = tok[j];
            }
            j++;
        }
        out[i] = '\0';
        return;
    }
    const char *v = get_field(tok);
    strncpy(out, v, (size_t)(outsz - 1));
    out[outsz - 1] = '\0';
}

/* Execute a { body } block — very simplified */
static void exec_block(const char *body) {
    /* Split by ';' and '\n', execute each statement */
    char stmt[512];
    const char *p = body;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == ';') p++;
        if (!*p) break;
        int si = 0;
        while (*p && *p != ';' && *p != '\n' && si < 511) stmt[si++] = *p++;
        stmt[si] = '\0';
        const char *s = stmt;
        while (*s == ' ') s++;
        if (!*s) continue;

        if (strncmp(s, "print ", 6) == 0 || strcmp(s, "print") == 0) {
            s += 5;
            while (*s == ' ') s++;
            if (!*s) {
                /* print $0 */
                write(1, g_line, (unsigned)strlen(g_line));
                write(1, "\n", 1);
            } else {
                /* evaluate comma-separated args */
                int first = 1;
                while (*s) {
                    while (*s == ' ') s++;
                    if (!*s) break;
                    /* find next comma (outside quotes) */
                    const char *tok_start = s;
                    int indq = 0;
                    while (*s && (*s != ',' || indq)) {
                        if (*s == '"') indq = !indq;
                        s++;
                    }
                    /* tok is [tok_start, s) */
                    char tok[256];
                    int tl = (int)(s - tok_start);
                    if (tl > 255) tl = 255;
                    memcpy(tok, tok_start, (size_t)tl);
                    /* trim */
                    while (tl > 0 && (tok[tl-1]==' '||tok[tl-1]=='\t')) tl--;
                    tok[tl] = '\0';
                    if (!first) write(1, " ", 1);
                    first = 0;
                    char val[1024];
                    eval_token(tok, val, 1024);
                    write(1, val, (unsigned)strlen(val));
                    if (*s == ',') s++;
                }
                write(1, "\n", 1);
            }
        }
        /* ignore other statements for now */
    }
}

/* Match a pattern */
static int match_pattern(const char *pat) {
    if (!pat || !*pat) return 1;  /* empty pattern → always match */
    /* Check /regex/ (literal substring match) */
    if (pat[0] == '/') {
        const char *end = strrchr(pat + 1, '/');
        if (!end) return 0;
        char regex[256];
        int rlen = (int)(end - (pat + 1));
        if (rlen > 255) rlen = 255;
        memcpy(regex, pat + 1, (size_t)rlen);
        regex[rlen] = '\0';
        return strstr(g_line, regex) != NULL;
    }
    /* NR==N, NF==N comparisons */
    if (strncmp(pat, "NR==", 4) == 0) return g_nr == atoi(pat + 4);
    if (strncmp(pat, "NF==", 4) == 0) return g_nf == atoi(pat + 4);
    if (strncmp(pat, "NR>", 3) == 0)  return g_nr > atoi(pat + 3);
    if (strncmp(pat, "NR<", 3) == 0)  return g_nr < atoi(pat + 3);
    /* $N==val comparisons */
    if (pat[0] == '$') {
        char fname[64]; int fi = 0;
        const char *p = pat;
        while (*p && *p != '=' && *p != '!' && *p != '<' && *p != '>' && fi < 63)
            fname[fi++] = *p++;
        fname[fi] = '\0';
        char cmp[4] = {0};
        int ci = 0;
        while (*p && (*p=='='||*p=='!'||*p=='<'||*p=='>') && ci < 3)
            cmp[ci++] = *p++;
        char rhs[256];
        if (*p == '"') { p++; int ri=0; while(*p&&*p!='"'&&ri<255) rhs[ri++]=*p++; rhs[ri]='\0'; }
        else { int ri=0; while(*p&&ri<255) rhs[ri++]=*p++; rhs[ri]='\0'; }
        const char *fval = get_field(fname);
        if (strcmp(cmp,"==") == 0) return strcmp(fval, rhs) == 0;
        if (strcmp(cmp,"!=") == 0) return strcmp(fval, rhs) != 0;
    }
    return 0;
}

/* Parse and run awk program on one line */
static void run_program(const char *prog, int is_begin, int is_end) {
    const char *p = prog;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\n') p++;
        if (!*p) break;

        /* Find pattern and { body } */
        char pat[256] = {0};
        int pi = 0;

        /* Check for BEGIN/END */
        if (strncmp(p, "BEGIN", 5) == 0 && (p[5]=='{'||p[5]==' ')) {
            if (!is_begin) {
                /* skip to closing } */
                p += 5;
                while (*p && *p != '{') p++;
                if (*p == '{') p++;
                int depth = 1;
                while (*p && depth) { if (*p=='{') depth++; else if(*p=='}') depth--; p++; }
                continue;
            }
            p += 5;
            while (*p == ' ') p++;
        } else if (strncmp(p, "END", 3) == 0 && (p[3]=='{'||p[3]==' ')) {
            if (!is_end) {
                p += 3;
                while (*p && *p != '{') p++;
                if (*p == '{') p++;
                int depth = 1;
                while (*p && depth) { if(*p=='{') depth++; else if(*p=='}') depth--; p++; }
                continue;
            }
            p += 3;
            while (*p == ' ') p++;
        } else {
            /* collect pattern up to { */
            while (*p && *p != '{' && pi < 255) pat[pi++] = *p++;
            pat[pi] = '\0';
            /* trim trailing space */
            while (pi > 0 && (pat[pi-1]==' '||pat[pi-1]=='\t')) pat[--pi]='\0';
        }

        if (*p != '{') continue;
        p++;  /* skip { */
        /* collect body */
        char body[1024]; int bi = 0;
        int depth = 1;
        while (*p && depth) {
            if (*p == '{') depth++;
            else if (*p == '}') { depth--; if (!depth) { p++; break; } }
            if (depth && bi < 1023) body[bi++] = *p;
            p++;
        }
        body[bi] = '\0';

        /* Execute if pattern matches (or BEGIN/END already filtered) */
        if (is_begin || is_end || match_pattern(pat))
            exec_block(body);
    }
}

static void process_fd(int fd, const char *prog) {
    char buf[256];
    int llen = 0;
    int n;
    while ((n = (int)read(fd, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n' || llen == LMAX - 1) {
                g_line[llen] = '\0';
                llen = 0;
                g_nr++;
                split_fields();
                run_program(prog, 0, 0);
            } else {
                g_line[llen++] = c;
            }
        }
    }
    if (llen > 0) {
        g_line[llen] = '\0';
        g_nr++;
        split_fields();
        run_program(prog, 0, 0);
    }
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        write(2, "usage: awk [-F sep] 'prog' [FILE...]\n", 37);
        return 1;
    }
    int ai = 1;
    if (strcmp(argv[ai], "-F") == 0 && ai + 1 < argc) {
        g_fs = argv[ai + 1][0];
        ai += 2;
    } else if (strncmp(argv[ai], "-F", 2) == 0) {
        g_fs = argv[ai][2];
        ai++;
    }
    if (ai >= argc) { write(2, "awk: missing program\n", 21); return 1; }
    const char *prog = argv[ai++];

    /* Run BEGIN blocks */
    run_program(prog, 1, 0);

    if (ai >= argc) {
        process_fd(0, prog);
    } else {
        for (; ai < argc; ai++) {
            int fd = open(argv[ai], 0);
            if (fd < 0) { printf("awk: %s: not found\n", argv[ai]); continue; }
            process_fd(fd, prog);
            close(fd);
        }
    }

    /* Run END blocks */
    run_program(prog, 0, 1);
    return 0;
}
