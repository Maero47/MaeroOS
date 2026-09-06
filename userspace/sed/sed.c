/*
 * sed — stream editor: s/pat/rep/[g][I], d (delete), p (print), -n (silent)
 * Usage: sed [-n] 'SCRIPT' [FILE...]
 */
#include "../include/unistd.h"
#include "../include/stdio.h"
#include "../include/string.h"
#include "../include/stdlib.h"

#define LMAX 4096

/* Simple literal string search/replace (no regex) */
static int do_subst(char *line, const char *pat, const char *rep, int global,
                    char *out, int outsz) {
    int patlen = (int)strlen(pat);
    int replen = (int)strlen(rep);
    int matches = 0;
    int oi = 0;
    const char *p = line;
    while (*p) {
        if (strncmp(p, pat, (size_t)patlen) == 0) {
            if (oi + replen >= outsz - 1) break;
            memcpy(out + oi, rep, (size_t)replen);
            oi += replen;
            p += patlen;
            matches++;
            if (!global) {
                /* copy rest */
                while (*p && oi < outsz - 1) out[oi++] = *p++;
                break;
            }
        } else {
            if (oi < outsz - 1) out[oi++] = *p;
            p++;
        }
    }
    out[oi] = '\0';
    return matches;
}

/* Parse a s/pat/rep/flags command. Returns 1 on success. */
typedef struct {
    char  pat[512];
    char  rep[512];
    int   global;
    int   icase;     /* not implemented, just parsed */
} subst_t;

static int parse_subst(const char *expr, subst_t *s) {
    if (*expr != 's') return 0;
    expr++;
    char delim = *expr++;
    if (!delim) return 0;

    int i = 0;
    while (*expr && *expr != delim && i < 511) {
        if (*expr == '\\' && *(expr+1)) { s->pat[i++] = *(++expr); expr++; }
        else s->pat[i++] = *expr++;
    }
    s->pat[i] = '\0';
    if (*expr == delim) expr++;

    i = 0;
    while (*expr && *expr != delim && i < 511) {
        if (*expr == '\\' && *(expr+1)) { s->rep[i++] = *(++expr); expr++; }
        else s->rep[i++] = *expr++;
    }
    s->rep[i] = '\0';
    if (*expr == delim) expr++;

    s->global = 0; s->icase = 0;
    while (*expr) {
        if (*expr == 'g') s->global = 1;
        if (*expr == 'I' || *expr == 'i') s->icase = 1;
        expr++;
    }
    return 1;
}

static void process_stream(int fd, const char *script, int silent) {
    char line[LMAX], out[LMAX];
    int llen = 0;
    char buf[256];
    int n;

    while ((n = (int)read(fd, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n' || llen == LMAX - 1) {
                line[llen] = '\0';
                llen = 0;

                int print = !silent;
                int del = 0;

                const char *sc = script;
                while (*sc == ' ') sc++;

                /* parse each semicolon-separated command */
                char cmdbuf[256];
                while (*sc) {
                    int cl = 0;
                    while (*sc && *sc != ';' && cl < 255)
                        cmdbuf[cl++] = *sc++;
                    cmdbuf[cl] = '\0';
                    if (*sc == ';') sc++;
                    const char *cmd = cmdbuf;
                    while (*cmd == ' ') cmd++;
                    if (!*cmd) continue;

                    if (*cmd == 'd') {
                        del = 1;
                    } else if (*cmd == 'p') {
                        write(1, line, (unsigned)strlen(line));
                        write(1, "\n", 1);
                    } else if (*cmd == 's') {
                        subst_t s;
                        if (parse_subst(cmd, &s)) {
                            if (do_subst(line, s.pat, s.rep, s.global, out, LMAX) > 0)
                                strncpy(line, out, LMAX - 1);
                        }
                    }
                }

                if (!del && print) {
                    write(1, line, (unsigned)strlen(line));
                    write(1, "\n", 1);
                }
            } else {
                line[llen++] = c;
            }
        }
    }
    /* flush partial line */
    if (llen > 0) {
        line[llen] = '\0';
        if (!silent) {
            write(1, line, (unsigned)strlen(line));
            write(1, "\n", 1);
        }
    }
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        write(2, "usage: sed [-n] 'SCRIPT' [FILE...]\n", 35);
        return 1;
    }
    int silent = 0;
    int ai = 1;
    if (strcmp(argv[ai], "-n") == 0) { silent = 1; ai++; }
    if (ai >= argc) { write(2, "sed: missing script\n", 20); return 1; }
    const char *script = argv[ai++];

    if (ai >= argc) {
        process_stream(0, script, silent);
    } else {
        for (; ai < argc; ai++) {
            int fd = open(argv[ai], 0);
            if (fd < 0) { printf("sed: %s: no such file\n", argv[ai]); continue; }
            process_stream(fd, script, silent);
            close(fd);
        }
    }
    return 0;
}
