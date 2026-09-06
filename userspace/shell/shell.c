/*
 * MaeroOS shell — quoting, ;/&&/||/&, $VAR, if/while/for, pipelines, redirections
 */
#include "../include/unistd.h"
#include "../include/signal.h"
#include "../include/stdio.h"
#include "../include/string.h"
#include "../include/stdlib.h"
#include "../include/termios.h"

#define MAX_LINE    512
#define MAX_ARGS    64
#define MAX_CMDS    16
#define MAX_REDIRS  8
#define MAX_SHVARS  64
#define MAX_JOBS    16
#define MAX_SCRIPT  8192

#define O_RDONLY  0
#define O_WRONLY  1
#define O_CREAT   0x40
#define O_TRUNC   0x200
#define O_APPEND  0x400

static const char *preferred_shell_path(void) {
    return access("/disk/shell", X_OK) == 0 ? "/disk/shell" : "/shell";
}

/* ── Redirection ──────────────────────────────────────────────────────────── */
typedef enum { REDIR_IN, REDIR_OUT, REDIR_APPEND, REDIR_ERR, REDIR_2TO1, REDIR_HEREDOC } redir_type_t;
#define HEREDOC_MAX 4096
typedef struct {
    redir_type_t type;
    int          target_fd;
    int          open_flags;
    char         file[MAX_LINE];        /* filename, or heredoc delimiter */
    char         heredoc_buf[HEREDOC_MAX]; /* content (REDIR_HEREDOC only) */
} redir_t;

/* Current script position — used by collect_heredocs() for script-mode heredocs */
static const char *g_script_heredoc_pos = NULL;

/* ── Shell variable store ─────────────────────────────────────────────────── */
static char sv_str[MAX_SHVARS][MAX_LINE];
static int  sv_exp[MAX_SHVARS];
static int  sv_n = 0;

static void sv_set(const char *name, const char *val) {
    int nl = (int)strlen(name);
    for (int i = 0; i < sv_n; i++) {
        int eq = 0;
        while (sv_str[i][eq] && sv_str[i][eq] != '=') eq++;
        if (eq == nl && strncmp(sv_str[i], name, (size_t)nl) == 0) {
            int o = 0;
            for (int k = 0; name[k] && o < MAX_LINE-2; k++) sv_str[i][o++] = name[k];
            sv_str[i][o++] = '=';
            for (int k = 0; val[k]  && o < MAX_LINE-1; k++) sv_str[i][o++] = val[k];
            sv_str[i][o] = '\0';
            return;
        }
    }
    if (sv_n < MAX_SHVARS) {
        int o = 0;
        for (int k = 0; name[k] && o < MAX_LINE-2; k++) sv_str[sv_n][o++] = name[k];
        sv_str[sv_n][o++] = '=';
        for (int k = 0; val[k]  && o < MAX_LINE-1; k++) sv_str[sv_n][o++] = val[k];
        sv_str[sv_n][o] = '\0';
        sv_exp[sv_n] = 0;
        sv_n++;
    }
}

static const char *sv_get(const char *name) {
    int nl = (int)strlen(name);
    for (int i = 0; i < sv_n; i++) {
        int eq = 0;
        while (sv_str[i][eq] && sv_str[i][eq] != '=') eq++;
        if (eq == nl && strncmp(sv_str[i], name, (size_t)nl) == 0)
            return sv_str[i] + eq + 1;
    }
    return (char *)0;
}

static void sv_export_name(const char *name) {
    int nl = (int)strlen(name);
    for (int i = 0; i < sv_n; i++) {
        int eq = 0;
        while (sv_str[i][eq] && sv_str[i][eq] != '=') eq++;
        if (eq == nl && strncmp(sv_str[i], name, (size_t)nl) == 0) {
            sv_exp[i] = 1; return;
        }
    }
}

static void sv_unset(const char *name) {
    int nl = (int)strlen(name);
    for (int i = 0; i < sv_n; i++) {
        int eq = 0;
        while (sv_str[i][eq] && sv_str[i][eq] != '=') eq++;
        if (eq == nl && strncmp(sv_str[i], name, (size_t)nl) == 0) {
            for (int j = i; j < sv_n-1; j++) {
                strcpy(sv_str[j], sv_str[j+1]);
                sv_exp[j] = sv_exp[j+1];
            }
            sv_n--;
            return;
        }
    }
}

/* ── Global state ─────────────────────────────────────────────────────────── */
static char **g_envp   = (char **)0;
static int    g_status = 0;
static int    shell_pgrp = 0;
static int    job_pgrp = 0;

/* Background/stopped jobs */
typedef struct { int pid; int stopped; } job_t;
static job_t jobs_list[MAX_JOBS];
static int   jobs_n = 0;

static void shell_take_terminal(void) {
    if (shell_pgrp > 0)
        tcsetpgrp(0, shell_pgrp);
}

static void job_take_terminal(int pgid) {
    if (pgid > 0)
        tcsetpgrp(0, pgid);
}

/* ── Build envp for child: exported shell vars override inherited ──────────── */
#define MAX_CENV 128
static char  cenv_bufs[MAX_CENV][MAX_LINE];
static char *cenv_ptrs[MAX_CENV + 1];

static char **make_envp(void) {
    int n = 0;
    /* exported shell vars */
    for (int i = 0; i < sv_n && n < MAX_CENV; i++) {
        if (!sv_exp[i]) continue;
        strncpy(cenv_bufs[n], sv_str[i], MAX_LINE-1);
        cenv_bufs[n][MAX_LINE-1] = '\0';
        cenv_ptrs[n] = cenv_bufs[n];
        n++;
    }
    /* inherited envp, skip vars already exported */
    if (g_envp) {
        for (int i = 0; g_envp[i] && n < MAX_CENV; i++) {
            int nl = 0;
            while (g_envp[i][nl] && g_envp[i][nl] != '=') nl++;
            int dup = 0;
            for (int j = 0; j < n; j++) {
                int el = 0;
                while (cenv_bufs[j][el] && cenv_bufs[j][el] != '=') el++;
                if (el == nl && strncmp(cenv_bufs[j], g_envp[i], (size_t)nl) == 0) {
                    dup = 1; break;
                }
            }
            if (!dup) {
                strncpy(cenv_bufs[n], g_envp[i], MAX_LINE-1);
                cenv_bufs[n][MAX_LINE-1] = '\0';
                cenv_ptrs[n] = cenv_bufs[n];
                n++;
            }
        }
    }
    cenv_ptrs[n] = (char *)0;
    return cenv_ptrs;
}

/* ── Variable lookup: shell vars first, then g_envp ──────────────────────── */
static const char *var_lookup(const char *name) {
    const char *v = sv_get(name);
    if (v) return v;
    if (!g_envp) return (char *)0;
    int nl = (int)strlen(name);
    for (int i = 0; g_envp[i]; i++) {
        int eq = 0;
        while (g_envp[i][eq] && g_envp[i][eq] != '=') eq++;
        if (eq == nl && strncmp(g_envp[i], name, (size_t)nl) == 0)
            return g_envp[i] + eq + 1;
    }
    return (char *)0;
}

/* ── History ring buffer ──────────────────────────────────────────────────── */
#define HIST_MAX 20
static char hist[HIST_MAX][MAX_LINE];
static int  hist_n    = 0;  /* entries stored */
static int  hist_head = 0;  /* index where next entry goes (wraps) */

static void hist_save(const char *line) {
    if (!line[0]) return;
    strncpy(hist[hist_head % HIST_MAX], line, MAX_LINE - 1);
    hist[hist_head % HIST_MAX][MAX_LINE - 1] = '\0';
    hist_head = (hist_head + 1) % HIST_MAX;
    if (hist_n < HIST_MAX) hist_n++;
}

/* pos=1 → most recent, pos=2 → one before, etc. Returns NULL if out of range. */
static const char *hist_get(int pos) {
    if (pos < 1 || pos > hist_n) return (void*)0;
    int idx = ((hist_head - pos) % HIST_MAX + HIST_MAX) % HIST_MAX;
    return hist[idx];
}

/* ── Line editor ──────────────────────────────────────────────────────────── */
static int readline_raw(char *buf, int max) {
    int i = 0;
    int hist_pos = 0; /* 0 = current input, 1 = most recent, etc. */
    char saved[MAX_LINE] = ""; /* saves current draft when browsing history */

    while (i < max - 1) {
        int c = getchar();
        if (c < 0) return -1;  /* EOF */
        if (c == 4)  return i; /* ^D */
        if (c == 3) {
            /* ^C: clear line */
            for (int j = 0; j < i; j++) write(1, "\b \b", 3);
            i = 0; hist_pos = 0; continue;
        }
        if (c == '\r' || c == '\n') break;
        if ((c == '\b' || c == 127) && i > 0) {
            i--; write(1, "\b \b", 3); continue;
        }
        /* ESC sequence: arrows */
        if (c == 0x1B) {
            int c2 = getchar();
            if (c2 == '[') {
                int c3 = getchar();
                /* tty_read echoed '[' and the direction key — erase them */
                write(1, "\b \b\b \b", 6);
                const char *entry = (void*)0;
                if (c3 == 'A') { /* up */
                    if (hist_pos == 0)
                        strncpy(saved, buf, max - 1);
                    int next = hist_pos + 1;
                    entry = hist_get(next);
                    if (entry) hist_pos = next;
                } else if (c3 == 'B') { /* down */
                    if (hist_pos > 1) {
                        hist_pos--;
                        entry = hist_get(hist_pos);
                    } else if (hist_pos == 1) {
                        hist_pos = 0;
                        entry = saved;
                    }
                }
                if (entry) {
                    /* Erase current input, then print history entry */
                    for (int j = 0; j < i; j++) write(1, "\b \b", 3);
                    strncpy(buf, entry, max - 1);
                    buf[max - 1] = '\0';
                    i = (int)strlen(buf);
                    write(1, buf, i);
                }
            }
            continue;
        }
        if (c < 0x20) continue;
        buf[i++] = (char)c;
    }
    buf[i] = '\0';
    hist_save(buf);
    return i;
}

/* ── Quote-aware expander helpers ────────────────────────────────────────── */
static int int_to_str(int v, char *buf) {
    if (v == 0) { buf[0]='0'; buf[1]='\0'; return 1; }
    char tmp[16]; int tl=0, neg=(v<0);
    unsigned uv = neg ? (unsigned)(-v) : (unsigned)v;
    while (uv) { tmp[tl++]='0'+(int)(uv%10); uv/=10; }
    if (neg) tmp[tl++]='-';
    for (int a=0,b=tl-1; a<b; a++,b--) { char t=tmp[a]; tmp[a]=tmp[b]; tmp[b]=t; }
    tmp[tl]='\0';
    strcpy(buf, tmp);
    return tl;
}

/* Append variable value for name[] to out[o] within [o..outsz-2]. Returns new o. */
static int append_var(const char *name, char *out, int o, int outsz) {
    const char *val = var_lookup(name);
    if (val) {
        while (*val && o < outsz-1) out[o++] = *val++;
    }
    return o;
}

/* Command substitution: capture output of cmd into out[*oo..outsz-1].
   Advances *pp past the closing ')'. */
static void expand_cmdsub(const char **pp, char *out, int *oo, int outsz) {
    /* Collect the command string up to the matching ')' */
    const char *p = *pp;
    char cmd[MAX_LINE]; int cl = 0;
    int depth = 1;
    while (*p && cl < MAX_LINE-1) {
        if (*p == '(') depth++;
        else if (*p == ')') { if (--depth == 0) { p++; break; } }
        cmd[cl++] = *p++;
    }
    cmd[cl] = '\0';
    *pp = p;

    /* Create a pipe */
    int pfd[2];
    if (pipe(pfd) < 0) return;

    int pid = fork();
    if (pid == 0) {
        /* Child: redirect stdout to pipe write end */
        close(pfd[0]);
        dup2(pfd[1], 1);
        close(pfd[1]);
        /* Execute the command via shell's own eval */
        const char *shell_path = preferred_shell_path();
        execve(shell_path, (char *[]){(char *)shell_path, "-c", cmd, (char *)0},
               (char *[]){(char *)0});
        /* If execve fails, use a simpler approach: write nothing */
        exit(1);
    }
    /* Parent: read from pipe */
    close(pfd[1]);
    char buf[MAX_LINE]; int n;
    while ((n = read(pfd[0], buf, sizeof(buf)-1)) > 0) {
        buf[n] = '\0';
        for (int i = 0; i < n && *oo < outsz-1; i++) out[(*oo)++] = buf[i];
    }
    close(pfd[0]);
    waitpid(pid, (int *)0, 0);
    /* Trim trailing newlines */
    while (*oo > 0 && out[*oo-1] == '\n') (*oo)--;
}

/* Expand one $... reference starting after the '$'.
   Advances *pp. Returns the number of chars appended (writes to out+*oo). */
static void expand_dollar(const char **pp, char *out, int *oo, int outsz) {
    const char *p = *pp;
    if (*p == '(') {
        /* Command substitution: $(...) */
        p++;  /* skip '(' */
        expand_cmdsub(&p, out, oo, outsz);
        *pp = p;
        return;
    }
    if (*p == '?') {
        char tmp[16]; int_to_str(g_status, tmp);
        for (int k=0; tmp[k] && *oo < outsz-1; k++) out[(*oo)++] = tmp[k];
        *pp = p + 1;
        return;
    }
    if (*p == '$') {
        char tmp[16]; int_to_str(getpid(), tmp);
        for (int k=0; tmp[k] && *oo < outsz-1; k++) out[(*oo)++] = tmp[k];
        *pp = p + 1;
        return;
    }
    char name[64]; int nl=0;
    while ((*p>='A'&&*p<='Z')||(*p>='a'&&*p<='z')||(*p>='0'&&*p<='9')||*p=='_') {
        if (nl<63) name[nl++]=*p;
        p++;
    }
    name[nl]='\0';
    if (!nl) { if (*oo<outsz-1) out[(*oo)++]='$'; *pp=p; return; }
    *oo = append_var(name, out, *oo, outsz);
    *pp = p;
}

/* ── Read one token from *pp with quoting; writes to buf[0..bufsz-1].
   Returns token length (0 = no token at current position / separator). ── */
static int read_token(const char **pp, char *buf, int bufsz) {
    const char *p = *pp;
    while (*p==' '||*p=='\t') p++;
    /* stop at unquoted special chars */
    if (!*p || *p=='|' || *p==';' || *p=='&' || *p=='\n') { *pp=p; return 0; }
    /* redirection operators are not word characters */
    if (*p=='<' || *p=='>') { *pp=p; return 0; }

    int o = 0;
    while (*p && o < bufsz-1) {
        if (*p=='\'') {
            p++;
            while (*p && *p!='\'' && o<bufsz-1) buf[o++]=*p++;
            if (*p=='\'') p++;
        } else if (*p=='"') {
            p++;
            while (*p && *p!='"') {
                if (*p=='$') { p++; expand_dollar(&p, buf, &o, bufsz); }
                else if (*p=='\\'&&(p[1]=='"'||p[1]=='\\'||p[1]=='$'||p[1]=='\n')) {
                    p++; if (*p&&o<bufsz-1) buf[o++]=*p++;
                } else { if (o<bufsz-1) buf[o++]=*p++; }
            }
            if (*p=='"') p++;
        } else if (*p=='\\') {
            p++; if (*p&&o<bufsz-1) buf[o++]=*p++;
        } else if (*p=='$') {
            p++; expand_dollar(&p, buf, &o, bufsz);
        } else if (*p==' '||*p=='\t'||*p=='|'||*p==';'||*p=='&'||*p=='<'||*p=='>') {
            break;
        } else {
            buf[o++]=*p++;
        }
    }
    buf[o]='\0';
    *pp=p;
    return o;
}

/* ── Tokenize a command string with quoting. Returns argc. ───────────────── */
/* We need per-token storage that persists for the lifetime of the invocation. */
static char tok_store[MAX_ARGS][MAX_LINE];

static int tokenize_q(const char *line, char *argv[], int max_args) {
    const char *p = line;
    int argc = 0;
    while (argc < max_args-1) {
        while (*p==' '||*p=='\t') p++;
        if (!*p || *p=='|' || *p==';' || *p=='&') break;
        /* skip redirection operators and their filename */
        if (*p=='>' || (*p=='2' && p[1]=='>')) {
            if (*p=='2') p++;
            p++;
            if (*p=='>') p++;
            if (*p=='&' && p[1]=='1') { p+=2; continue; }
            while (*p==' '||*p=='\t') p++;
            /* skip filename token */
            const char *dummy = p;
            char tmp[MAX_LINE];
            read_token(&dummy, tmp, MAX_LINE);
            p = dummy;
            continue;
        }
        if (*p=='<') {
            p++;
            while (*p==' '||*p=='\t') p++;
            const char *dummy = p;
            char tmp[MAX_LINE];
            read_token(&dummy, tmp, MAX_LINE);
            p = dummy;
            continue;
        }
        int n = read_token(&p, tok_store[argc], MAX_LINE);
        if (!n) break;
        argv[argc] = tok_store[argc];
        argc++;
    }
    argv[argc] = (char *)0;
    return argc;
}

/* ── Redirection parser (quote-aware) ────────────────────────────────────── */
static void parse_redirs(const char *cmd, redir_t *redirs, int *nredirs, char *clean, int cleansz) {
    *nredirs = 0;
    const char *p = cmd;
    int o = 0;
    int in_sq=0, in_dq=0;

    while (*p && o < cleansz-1) {
        /* track quote context */
        if (*p=='\'' && !in_dq) { in_sq=!in_sq; clean[o++]=*p++; continue; }
        if (*p=='"'  && !in_sq) { in_dq=!in_dq; clean[o++]=*p++; continue; }
        if (*p=='\\' && !in_sq) { if (o<cleansz-2) { clean[o++]=*p++; } if (*p) clean[o++]=*p++; continue; }
        if (in_sq || in_dq)     { clean[o++]=*p++; continue; }

        redir_type_t type; int op_len; int has_file=1;
        if (p[0]=='2'&&p[1]=='>'&&p[2]=='&'&&p[3]=='1') { type=REDIR_2TO1;   op_len=4; has_file=0; }
        else if (p[0]=='>'&&p[1]=='>')                   { type=REDIR_APPEND; op_len=2; }
        else if (p[0]=='2'&&p[1]=='>')                   { type=REDIR_ERR;    op_len=2; }
        else if (p[0]=='>')                              { type=REDIR_OUT;    op_len=1; }
        else if (p[0]=='<'&&p[1]=='<')                  { type=REDIR_HEREDOC;op_len=2; }
        else if (p[0]=='<')                              { type=REDIR_IN;     op_len=1; }
        else { clean[o++]=*p++; continue; }

        if (*nredirs >= MAX_REDIRS) { p+=op_len; continue; }

        redir_t *r = &redirs[*nredirs];
        r->type = type;
        r->file[0] = '\0';
        p += op_len;

        if (!has_file) {
            r->target_fd=2; r->open_flags=0; (*nredirs)++; continue;
        }
        while (*p==' ') p++;
        /* read filename (quote-aware) */
        read_token(&p, r->file, MAX_LINE);
        switch (type) {
        case REDIR_IN:      r->target_fd=0; r->open_flags=O_RDONLY; break;
        case REDIR_OUT:     r->target_fd=1; r->open_flags=O_WRONLY|O_CREAT|O_TRUNC; break;
        case REDIR_APPEND:  r->target_fd=1; r->open_flags=O_WRONLY|O_CREAT|O_APPEND; break;
        case REDIR_ERR:     r->target_fd=2; r->open_flags=O_WRONLY|O_CREAT|O_TRUNC; break;
        case REDIR_HEREDOC: r->target_fd=0; r->heredoc_buf[0]='\0'; break;
        default: break;
        }
        (*nredirs)++;
    }
    clean[o]='\0';
}

/*
 * collect_heredocs — called BEFORE fork to fill heredoc_buf for each REDIR_HEREDOC.
 * In interactive mode: prompts for lines via readline_raw.
 * In script mode: reads from g_script_heredoc_pos.
 */
static void collect_heredocs(redir_t *redirs, int nredirs) {
    for (int i = 0; i < nredirs; i++) {
        redir_t *r = &redirs[i];
        if (r->type != REDIR_HEREDOC) continue;

        int boff = 0;
        char line[MAX_LINE];
        for (;;) {
            if (g_script_heredoc_pos) {
                /* Script mode: consume lines from g_script_heredoc_pos */
                if (!*g_script_heredoc_pos) break;
                int ll = 0;
                while (*g_script_heredoc_pos && *g_script_heredoc_pos != '\n' && ll < MAX_LINE-1)
                    line[ll++] = *g_script_heredoc_pos++;
                if (*g_script_heredoc_pos == '\n') g_script_heredoc_pos++;
                line[ll] = '\0';
            } else {
                /* Interactive mode */
                write(1, "> ", 2);
                int n = readline_raw(line, MAX_LINE);
                write(1, "\n", 1);
                if (n < 0) break;  /* EOF */
            }
            /* Stop when we see the delimiter line */
            if (strcmp(line, r->file) == 0) break;
            int ll = (int)strlen(line);
            if (boff + ll + 1 < HEREDOC_MAX) {
                memcpy(r->heredoc_buf + boff, line, (size_t)ll);
                boff += ll;
                r->heredoc_buf[boff++] = '\n';
            }
        }
        r->heredoc_buf[boff] = '\0';
    }
}

static void apply_redirs(redir_t *redirs, int nredirs) {
    for (int i = 0; i < nredirs; i++) {
        redir_t *r = &redirs[i];
        if (r->type == REDIR_2TO1) { dup2(1, 2); continue; }
        if (r->type == REDIR_HEREDOC) {
            /* Create a pipe, write heredoc content to write end, use read end as stdin */
            int pfd[2];
            if (pipe(pfd) < 0) { exit(1); }
            int len = (int)strlen(r->heredoc_buf);
            write(pfd[1], r->heredoc_buf, (unsigned)len);
            close(pfd[1]);
            dup2(pfd[0], r->target_fd);  /* usually fd 0 */
            close(pfd[0]);
            continue;
        }
        int fd = open(r->file, r->open_flags);
        if (fd < 0) { printf("shell: cannot open %s\n", r->file); exit(1); }
        dup2(fd, r->target_fd);
        close(fd);
    }
}

/* ── PATH-based exec ──────────────────────────────────────────────────────── */
static void exec_with_path(char *argv[], char **envp) {
    if (!argv[0]) return;
    if (argv[0][0]=='/' || argv[0][0]=='.') {
        execve(argv[0], argv, (char *const *)envp);
        return;
    }
    const char *path_env = var_lookup("PATH");
    if (!path_env) path_env = "/";
    const char *p = path_env;
    while (*p) {
        char dir[MAX_LINE]; int dl=0;
        while (*p && *p!=':' && dl<MAX_LINE-1) dir[dl++]=*p++;
        if (*p==':') p++;
        dir[dl]='\0';
        if (!dl) { dir[0]='.'; dir[1]='\0'; dl=1; }
        char full[MAX_LINE];
        int al=(int)strlen(argv[0]);
        if (dl+1+al+1>MAX_LINE) continue;
        strncpy(full, dir, MAX_LINE-1);
        if (dir[dl-1]!='/') strncat(full, "/", MAX_LINE-strlen(full)-1);
        strncat(full, argv[0], MAX_LINE-strlen(full)-1);
        char *saved=argv[0]; argv[0]=full;
        execve(full, argv, (char *const *)envp);
        argv[0]=saved;
    }
}

/* ── Forward declaration ──────────────────────────────────────────────────── */
static int exec_script(const char *buf);
static int exec_line(const char *line);

/* Source a startup script (e.g. /etc/profile) if it exists. */
static void source_file_if_present(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return;
    static char sbuf[MAX_SCRIPT];
    int total = 0, n;
    while ((n = read(fd, sbuf + total, MAX_SCRIPT - total - 1)) > 0)
        total += n;
    sbuf[total] = '\0';
    close(fd);
    exec_script(sbuf);
}

/* ── Builtins ─────────────────────────────────────────────────────────────── */
static int run_builtin(char *argv[], int argc) {
    if (!argv[0]) return 0;

    if (strcmp(argv[0],"exit")==0) {
        exit(argc>1 ? atoi(argv[1]) : g_status);
    }
    if (strcmp(argv[0],"true")==0)  { g_status=0; return 1; }
    if (strcmp(argv[0],"false")==0) { g_status=1; return 1; }
    if (strcmp(argv[0],"clear")==0) {
        /* ANSI erase-display + cursor-home; understood by the desktop
         * terminal and any real terminal on the serial console. */
        write(1, "\033[2J\033[H", 7);
        g_status=0; return 1;
    }

    if (strcmp(argv[0],"cd")==0) {
        const char *dir = argc>1 ? argv[1] : var_lookup("HOME");
        if (!dir) dir="/";
        if (chdir(dir)<0) printf("cd: %s: no such directory\n", dir);
        return 1;
    }

    if (strcmp(argv[0],"echo")==0) {
        int nl=1, esc=0, start=1;
        /* Parse flags: -n, -e, -ne, -en */
        while (start < argc && argv[start][0] == '-' && argv[start][1]) {
            const char *fl = argv[start]+1;
            int valid = 1;
            for (int fi = 0; fl[fi]; fi++) {
                if (fl[fi]=='n') nl=0;
                else if (fl[fi]=='e') esc=1;
                else { valid=0; break; }
            }
            if (!valid) break;
            start++;
        }
        for (int i=start; i<argc; i++) {
            if (i>start) write(1," ",1);
            if (!esc) {
                write(1, argv[i], strlen(argv[i]));
            } else {
                /* Interpret \n \t \r \\ \a \b \e \0NNN etc. */
                for (const char *p = argv[i]; *p; p++) {
                    if (*p == '\\' && p[1]) {
                        char out; p++;
                        switch (*p) {
                        case 'n': out='\n'; break;
                        case 't': out='\t'; break;
                        case 'r': out='\r'; break;
                        case '\\': out='\\'; break;
                        case 'a': out='\a'; break;
                        case 'b': out='\b'; break;
                        case 'e': out='\033'; break;
                        case '0': {
                            /* \0NNN octal */
                            int v=0, digits=0;
                            while (digits<3 && p[1]>='0'&&p[1]<='7') { p++; v=v*8+(*p-'0'); digits++; }
                            out=(char)v;
                            break;
                        }
                        default: write(1,"\\",1); out=*p; break;
                        }
                        write(1, &out, 1);
                    } else {
                        write(1, p, 1);
                    }
                }
            }
        }
        if (nl) write(1,"\n",1);
        g_status=0; return 1;
    }

    if (strcmp(argv[0],"export")==0) {
        for (int i=1; i<argc; i++) {
            /* export NAME or export NAME=VALUE */
            char *eq = strchr(argv[i], '=');
            if (eq) {
                *eq = '\0';
                sv_set(argv[i], eq+1);
                *eq = '=';
            }
            sv_export_name(argv[i]);
        }
        g_status=0; return 1;
    }

    if (strcmp(argv[0],"unset")==0) {
        for (int i=1; i<argc; i++) sv_unset(argv[i]);
        g_status=0; return 1;
    }

    if (strcmp(argv[0],"read")==0) {
        const char *prompt = NULL;
        int silent = 0;
        int ai = 1;
        /* Parse options: -p PROMPT, -s */
        while (ai < argc && argv[ai][0] == '-' && argv[ai][1]) {
            if (strcmp(argv[ai],"-p")==0 && ai+1<argc) { prompt=argv[++ai]; ai++; }
            else if (strcmp(argv[ai],"-s")==0) { silent=1; ai++; }
            else break;
        }
        const char *varname = (ai < argc) ? argv[ai] : NULL;
        if (prompt) { write(2, prompt, strlen(prompt)); }
        char buf[MAX_LINE];
        int n=0;
        /* If stdin is a tty and silent requested, put it in raw/noecho mode */
        struct termios saved, raw;
        int was_canon = 0;
        if (silent) {
            if (tcgetattr(0, &saved) == 0) {
                raw = saved;
                raw.c_lflag &= ~(0x0008 /* ECHO */);
                tcsetattr(0, 0, &raw);
                was_canon = 1;
            }
        }
        char ch;
        while (n<MAX_LINE-1) {
            int r=read(0,&ch,1);
            if (r<=0) break;
            if (ch=='\n') break;
            buf[n++]=ch;
        }
        buf[n]='\0';
        if (was_canon) {
            tcsetattr(0, 0, &saved);
            write(2, "\n", 1);
        }
        if (varname) sv_set(varname, buf);
        g_status = 0;
        return 1;
    }

    if (strcmp(argv[0],".")==0 || strcmp(argv[0],"source")==0) {
        if (argc<2) { g_status=1; return 1; }
        /* Read and execute file */
        int fd=open(argv[1],O_RDONLY);
        if (fd<0) { printf(".: %s: not found\n", argv[1]); g_status=1; return 1; }
        char sbuf[MAX_SCRIPT];
        int total=0,n;
        while ((n=read(fd,sbuf+total,MAX_SCRIPT-total-1))>0) total+=n;
        sbuf[total]='\0';
        close(fd);
        exec_script(sbuf);
        return 1;
    }

    if (strcmp(argv[0],"jobs")==0) {
        int new_n=0;
        for (int i=0; i<jobs_n; i++) {
            int st=0;
            int r=waitpid(jobs_list[i].pid, &st, 3); /* WNOHANG|WUNTRACED */
            if (r==0) {
                /* still running or stopped */
                printf("[%d] %s  %d\n", new_n+1,
                       jobs_list[i].stopped ? "Stopped " : "Running ",
                       jobs_list[i].pid);
                jobs_list[new_n++]=jobs_list[i];
            } else if (r>0 && (st&0xff)==0x7f) {
                /* just became stopped */
                jobs_list[i].stopped=1;
                printf("[%d] Stopped   %d\n", new_n+1, jobs_list[i].pid);
                jobs_list[new_n++]=jobs_list[i];
            }
            /* else: exited — drop from list */
        }
        jobs_n=new_n;
        g_status=0; return 1;
    }

    if (strcmp(argv[0],"fg")==0) {
        int idx = (argc>1) ? atoi(argv[1])-1 : jobs_n-1;
        if (idx<0 || idx>=jobs_n) { printf("fg: no such job\n"); g_status=1; return 1; }
        int pid = jobs_list[idx].pid;
        jobs_list[idx].stopped = 0;
        job_take_terminal(pid);
        kill(-pid, 18); /* SIGCONT=18 */
        /* Wait for it to finish or stop again */
        int st=0;
        waitpid(pid, &st, 2); /* WUNTRACED=2 */
        shell_take_terminal();
        g_status = st;
        if ((st & 0xff) == 0x7f) {
            jobs_list[idx].stopped = 1; /* stopped again */
        } else {
            /* remove from list */
            for (int i=idx; i<jobs_n-1; i++) jobs_list[i]=jobs_list[i+1];
            jobs_n--;
        }
        return 1;
    }

    if (strcmp(argv[0],"bg")==0) {
        int idx = (argc>1) ? atoi(argv[1])-1 : jobs_n-1;
        if (idx<0 || idx>=jobs_n) { printf("bg: no such job\n"); g_status=1; return 1; }
        jobs_list[idx].stopped = 0;
        kill(-jobs_list[idx].pid, 18); /* SIGCONT=18 */
        g_status=0; return 1;
    }

    if (strcmp(argv[0],"pwd")==0) {
        char buf[256]; getcwd_syscall(buf,256);
        write(1,buf,strlen(buf)); write(1,"\n",1);
        g_status=0; return 1;
    }

    if (strcmp(argv[0],"wait")==0) {
        /* wait [PID] — wait for background job or all bg jobs */
        if (argc > 1) {
            int pid = atoi(argv[1]);
            int st = 0;
            waitpid(pid, &st, 0);
            g_status = (st >> 8) & 0xff;
        } else {
            /* Wait for all background jobs */
            for (int j = 0; j < jobs_n; j++) {
                int st = 0;
                waitpid(jobs_list[j].pid, &st, 0);
            }
            jobs_n = 0;
            g_status = 0;
        }
        return 1;
    }

    if (strcmp(argv[0],"trap")==0) {
        /* Minimal trap: just acknowledge — full trap needs async signal handling */
        /* trap CMD SIGNAL... or trap - SIGNAL... or trap */
        if (argc < 2) { /* print traps — not implemented */ g_status=0; return 1; }
        /* For now, handle: trap '' INT (ignore INT) and trap - INT (reset) */
        if (argc >= 3) {
            const char *action = argv[1];
            for (int i = 2; i < argc; i++) {
                int sig = atoi(argv[i]);
                if (sig == 0) {
                    /* Named signal */
                    if (strcmp(argv[i],"INT")==0||strcmp(argv[i],"SIGINT")==0)   sig=2;
                    else if (strcmp(argv[i],"TERM")==0||strcmp(argv[i],"SIGTERM")==0) sig=15;
                    else if (strcmp(argv[i],"HUP")==0||strcmp(argv[i],"SIGHUP")==0)  sig=1;
                    else if (strcmp(argv[i],"QUIT")==0||strcmp(argv[i],"SIGQUIT")==0) sig=3;
                    else if (strcmp(argv[i],"CHLD")==0||strcmp(argv[i],"SIGCHLD")==0) sig=17;
                    else if (strcmp(argv[i],"EXIT")==0) sig=0; /* EXIT pseudo-signal */
                }
                if (sig > 0 && sig < 32) {
                    if (strcmp(action,"")==0) {
                        signal(sig, (sighandler_t)1); /* SIG_IGN */
                    } else if (strcmp(action,"-")==0) {
                        signal(sig, (sighandler_t)0); /* SIG_DFL */
                    }
                    /* Non-empty action string: not fully implemented */
                }
            }
        }
        g_status=0; return 1;
    }

    if (strcmp(argv[0],"type")==0||strcmp(argv[0],"which")==0) {
        /* basic: just echo the path found via PATH */
        if (argc<2) { g_status=1; return 1; }
        const char *path_env=var_lookup("PATH");
        if (!path_env) path_env="/";
        const char *p=path_env; int found=0;
        while (*p && !found) {
            char dir[MAX_LINE]; int dl=0;
            while (*p&&*p!=':'&&dl<MAX_LINE-1) dir[dl++]=*p++;
            if (*p==':') p++;
            dir[dl]='\0';
            if (!dl){dir[0]='.';dir[1]='\0';}
            char full[MAX_LINE];
            strncpy(full,dir,MAX_LINE-1);
            full[MAX_LINE-1]='\0';
            /* Only add the separator when dir doesn't already end in '/'. */
            if (dl>0 && dir[dl-1]!='/')
                strncat(full,"/",MAX_LINE-strlen(full)-1);
            strncat(full,argv[1],MAX_LINE-strlen(full)-1);
            if (access(full,0)==0) { printf("%s\n",full); found=1; }
        }
        if (!found) printf("%s: not found\n",argv[1]);
        g_status=found?0:1; return 1;
    }

    return 0;
}

/* ── Execute one simple command (after pipeline split) ───────────────────── */
/* in_fd/out_fd: pipe fds (-1 if none). background: don't wait. */
/* Returns pid if forked, 0 if builtin or empty, -1 on error. */
static int run_simple(const char *cmdstr, int in_fd, int out_fd, int background) {
    /* Parse redirections */
    redir_t redirs[MAX_REDIRS]; int nredirs=0;
    char clean[MAX_LINE];
    parse_redirs(cmdstr, redirs, &nredirs, clean, MAX_LINE);

    /* Tokenize */
    char *argv[MAX_ARGS];
    int argc = tokenize_q(clean, argv, MAX_ARGS);
    if (!argc) return 0;

    /* Variable assignment: NAME=VALUE with no slashes and starts with alpha */
    if (in_fd<0 && out_fd<0 && !background) {
        /* Check if ALL args are assignments */
        int all_assign=1;
        for (int i=0; i<argc; i++) {
            char *eq=strchr(argv[i],'=');
            if (!eq || eq==argv[i] ||
                !((argv[i][0]>='A'&&argv[i][0]<='Z')||(argv[i][0]>='a'&&argv[i][0]<='z')||argv[i][0]=='_')) {
                all_assign=0; break;
            }
        }
        if (all_assign && argc>0) {
            for (int i=0; i<argc; i++) {
                char *eq=strchr(argv[i],'=');
                *eq='\0'; sv_set(argv[i], eq+1); *eq='=';
            }
            g_status=0; return 0;
        }
        /* Check if first arg is assignment and rest is command */
        int first_assign=0;
        char *first_eq=strchr(argv[0],'=');
        if (first_eq && first_eq!=argv[0] &&
            ((argv[0][0]>='A'&&argv[0][0]<='Z')||(argv[0][0]>='a'&&argv[0][0]<='z')||argv[0][0]=='_')) {
            first_assign=1;
        }
        if (first_assign && argc==1) {
            *first_eq='\0'; sv_set(argv[0], first_eq+1); *first_eq='=';
            g_status=0; return 0;
        }
    }

    /* Collect here-documents before fork (needs stdin from parent) */
    collect_heredocs(redirs, nredirs);

    /* Builtin (only when not piped) */
    if (in_fd<0 && out_fd<0 && !background && nredirs==0 && run_builtin(argv, argc))
        return 0;

    /* Fork */
    int pid=fork();
    if (pid<0) { printf("shell: fork failed\n"); return -1; }
    if (pid==0) {
        int pg = job_pgrp ? job_pgrp : getpid();
        setpgid(0, pg);
        signal(SIGINT,  SIG_DFL);
        signal(SIGPIPE, SIG_DFL);
        if (in_fd>=0)  { dup2(in_fd,0);  close(in_fd); }
        if (out_fd>=0) { dup2(out_fd,1); close(out_fd); }
        apply_redirs(redirs, nredirs);

        /* If first token is assignment, set it in env before exec */
        char *first_eq2=strchr(argv[0],'=');
        if (first_eq2 && first_eq2!=argv[0] &&
            ((argv[0][0]>='A'&&argv[0][0]<='Z')||(argv[0][0]>='a'&&argv[0][0]<='z')||argv[0][0]=='_')) {
            /* skip leading assignments, find real command */
            int cmd_i=0;
            while (cmd_i<argc) {
                char *eq=strchr(argv[cmd_i],'=');
                if (!eq||eq==argv[cmd_i]) break;
                cmd_i++;
            }
            if (cmd_i<argc) {
                char **envp=make_envp();
                exec_with_path(argv+cmd_i, envp);
                printf("shell: %s: not found\n", argv[cmd_i]);
            }
            exit(127);
        }

        char **envp=make_envp();
        /* Builtin in pipe context */
        if (run_builtin(argv, argc)) exit(g_status);
        exec_with_path(argv, envp);
        printf("shell: %s: not found\n", argv[0]);
        exit(127);
    }
    if (!job_pgrp) job_pgrp = pid;
    setpgid(pid, job_pgrp);
    return pid;
}

/* ── Quote-aware pipeline split ───────────────────────────────────────────── */
static int split_pipe_q(char *line, char *cmds[], int max) {
    int n=0; char *p=line;
    cmds[n++]=p;
    int sq=0, dq=0;
    while (*p && n<max) {
        if (*p=='\'' && !dq) sq=!sq;
        else if (*p=='"' && !sq) dq=!dq;
        else if (*p=='\\' && !sq) { p++; if (*p) p++; continue; }
        else if (*p=='|' && !sq && !dq && p[1]!='|') {
            *p++='\0';
            while (*p==' ') p++;
            cmds[n++]=p; continue;
        }
        p++;
    }
    return n;
}

/* ── Execute a pipeline string ────────────────────────────────────────────── */
static int run_pipeline(const char *cmdstr, int background) {
    char buf[MAX_LINE];
    strncpy(buf, cmdstr, MAX_LINE-1); buf[MAX_LINE-1]='\0';

    char *cmds[MAX_CMDS]; int ncmds=split_pipe_q(buf, cmds, MAX_CMDS);

    if (ncmds==1 && !background) {
        /* single command — check builtins and assignments */
        redir_t redirs[MAX_REDIRS]; int nredirs=0;
        char clean[MAX_LINE];
        parse_redirs(cmds[0], redirs, &nredirs, clean, MAX_LINE);
        char *argv[MAX_ARGS]; int argc=tokenize_q(clean, argv, MAX_ARGS);
        if (!argc) return 0;

        /* Collect heredocs before any fork */
        collect_heredocs(redirs, nredirs);

        /* All-assignment case */
        if (nredirs==0) {
            int all=1;
            for (int i=0;i<argc;i++) {
                char *eq=strchr(argv[i],'=');
                if (!eq||eq==argv[i]||
                    !((argv[i][0]>='A'&&argv[i][0]<='Z')||(argv[i][0]>='a'&&argv[i][0]<='z')||argv[i][0]=='_'))
                    { all=0; break; }
            }
            if (all) {
                for (int i=0;i<argc;i++) {
                    char *eq=strchr(argv[i],'='); *eq='\0'; sv_set(argv[i],eq+1); *eq='=';
                }
                g_status=0; return 0;
            }
            if (run_builtin(argv,argc)) return g_status;
        }
        /* Fall through to fork */
        int pid=fork();
        if (pid<0) { printf("shell: fork failed\n"); return -1; }
        if (pid==0) {
            setpgid(0, getpid());
            signal(SIGINT,SIG_DFL); signal(SIGPIPE,SIG_DFL);
            apply_redirs(redirs,nredirs);
            char **envp=make_envp();
            if (run_builtin(argv,argc)) exit(g_status);
            exec_with_path(argv,envp);
            printf("shell: %s: not found\n",argv[0]);
            exit(127);
        }
        setpgid(pid, pid);
        job_take_terminal(pid);
        int st=0; waitpid(pid,&st,2); g_status=st;
        shell_take_terminal();
        if ((st & 0xff) == 0x7f && jobs_n < MAX_JOBS) {
            jobs_list[jobs_n].pid = pid;
            jobs_list[jobs_n].stopped = 1;
            jobs_n++;
        }
        return g_status;
    }

    /* Multi-stage pipeline */
    int pids[MAX_CMDS]; int prev=-1;
    int pgid = 0;
    job_pgrp = 0;
    for (int i=0;i<ncmds;i++) {
        int pfd[2]={-1,-1};
        if (i<ncmds-1) pipe(pfd);
        pids[i]=run_simple(cmds[i], prev, (i<ncmds-1)?pfd[1]:-1, background);
        if (pids[i] > 0 && !pgid) {
            pgid = pids[i];
            job_pgrp = pgid;
        }
        if (prev>=0) close(prev);
        if (i<ncmds-1) close(pfd[1]);
        prev=pfd[0];
    }
    job_pgrp = 0;
    if (prev>=0) close(prev);

    if (!background) {
        job_take_terminal(pgid);
        int st=0;
        for (int i=0;i<ncmds;i++)
            if (pids[i]>0) { int s; waitpid(pids[i],&s,2); st=s; }
        shell_take_terminal();
        g_status=st;
        if ((st & 0xff) == 0x7f && pgid > 0 && jobs_n < MAX_JOBS) {
            jobs_list[jobs_n].pid = pgid;
            jobs_list[jobs_n].stopped = 1;
            jobs_n++;
        }
    } else {
        /* background: record last pid */
        if (pgid>0 && jobs_n<MAX_JOBS) {
            jobs_list[jobs_n].pid     = pgid;
            jobs_list[jobs_n].stopped = 0;
            jobs_n++;
        }
    }
    return g_status;
}

/* ── Compound command splitting: ; && || & ────────────────────────────────── */
typedef enum { SEP_NONE=0, SEP_SEMI, SEP_AND, SEP_OR } sep_t;
typedef struct { char cmd[MAX_LINE]; sep_t sep; int bg; } cpd_t;

static int split_cpd(const char *line, cpd_t *out, int max) {
    int n=0; const char *p=line;
    int sq=0,dq=0;
    while (n<max) {
        int ci=0; out[n].sep=SEP_NONE; out[n].bg=0;
        while (*p && ci<MAX_LINE-1) {
            if (*p=='\'' && !dq) { sq=!sq; out[n].cmd[ci++]=*p++; continue; }
            if (*p=='"'  && !sq) { dq=!dq; out[n].cmd[ci++]=*p++; continue; }
            if (*p=='\\' && !sq) {
                if (ci<MAX_LINE-2) out[n].cmd[ci++]=*p++;
                if (*p && ci<MAX_LINE-1) out[n].cmd[ci++]=*p++;
                continue;
            }
            if (!sq && !dq) {
                if (p[0]=='&'&&p[1]=='&') { out[n].sep=SEP_AND; p+=2; break; }
                if (p[0]=='|'&&p[1]=='|') { out[n].sep=SEP_OR;  p+=2; break; }
                if (p[0]==';')            { out[n].sep=SEP_SEMI; p++;  break; }
                if (p[0]=='&'&&p[1]!='&') { out[n].bg=1; out[n].sep=SEP_SEMI; p++; break; }
            }
            out[n].cmd[ci++]=*p++;
        }
        out[n].cmd[ci]='\0';
        /* trim */
        int l=(int)strlen(out[n].cmd);
        while (l>0&&(out[n].cmd[l-1]==' '||out[n].cmd[l-1]=='\t')) l--;
        out[n].cmd[l]='\0';
        if (l>0) n++;
        if (!*p) break;
        while (*p==' '||*p=='\t') p++;
    }
    return n;
}

/* ── Collect block from a buffer pointer (for if/while/for inside scripts) ── */
static int next_bline(const char **pp, char *out, int outsz) {
    const char *p=*pp;
    if (!*p) return 0;
    int i=0;
    while (*p && *p!='\n' && i<outsz-1) out[i++]=*p++;
    if (*p=='\n') p++;
    out[i]='\0';
    *pp=p;
    return 1;
}

/* Trim leading whitespace (returns pointer into s) */
static const char *ltrim(const char *s) {
    while (*s==' '||*s=='\t') s++;
    return s;
}

/* Does line start with keyword kw followed by space/tab/semicolon/end? */
static int kw_match(const char *line, const char *kw) {
    const char *l=ltrim(line);
    int kl=(int)strlen(kw);
    if (strncmp(l,kw,(size_t)kl)!=0) return 0;
    char nc=l[kl];
    return !nc||nc==' '||nc=='\t'||nc==';'||nc=='\n';
}

/* Extract block from buffer *pp until close_kw at depth 0.
   open_kw increments depth. Appends everything including close_kw line to out. */
static int extract_block(const char **pp, const char *open_kw, const char *close_kw,
                          char *out, int outsz) {
    int off=0, depth=1;
    char line[MAX_LINE];
    while (depth>0 && next_bline(pp, line, MAX_LINE)) {
        if (kw_match(line, open_kw)) depth++;
        if (kw_match(line, close_kw)) depth--;
        int ll=(int)strlen(line);
        if (off+ll+2<outsz) {
            memcpy(out+off, line, (size_t)ll);
            off+=ll; out[off++]='\n';
        }
    }
    out[off]='\0';
    return (depth==0)?0:-1;
}

/* ── if/elif/else/fi executor (buf = full block including "if ...\n...\nfi\n") ─ */
static int exec_if_block(const char *buf) {
    const char *p=buf;
    char line[MAX_LINE];

    /* Parse "if CMD; then" or "if CMD\nthen" */
    if (!next_bline(&p, line, MAX_LINE)) return 1;
    /* Strip "if " prefix, find "; then" or " then" */
    const char *cond_start = ltrim(line)+3; /* skip "if " */
    /* Remove trailing "; then" or " then" */
    char cond[MAX_LINE]; strncpy(cond, cond_start, MAX_LINE-1); cond[MAX_LINE-1]='\0';
    /* Trim "; then" suffix */
    int cl=(int)strlen(cond);
    while (cl>0 && (cond[cl-1]==' '||cond[cl-1]=='\t')) cl--;
    /* Check for "then" at end */
    if (cl>=4 && strncmp(cond+cl-4,"then",4)==0) {
        cl-=4;
        while (cl>0 && (cond[cl-1]==' '||cond[cl-1]=='\t'||cond[cl-1]==';')) cl--;
    }
    cond[cl]='\0';

    /* If cond is empty, read next line */
    if (!cl) next_bline(&p, cond, MAX_LINE); /* should be the command */

    /* Execute condition */
    exec_line(cond);
    int cond_true=(g_status==0);

    /* Scan body: collect lines for true/elif/else/fi sections */
    int in_true=cond_true, found_fi=0;
    while (!found_fi && next_bline(&p, line, MAX_LINE)) {
        const char *tl=ltrim(line);
        if (kw_match(tl,"fi"))   { found_fi=1; break; }
        if (kw_match(tl,"else")) {
            if (!cond_true && in_true) in_true=1; /* else branch active */
            else in_true=0;
            continue;
        }
        if (kw_match(tl,"elif")) {
            if (cond_true) { in_true=0; continue; } /* already found true branch */
            /* evaluate elif condition */
            const char *ec=ltrim(tl)+5;
            char econd[MAX_LINE]; strncpy(econd,ec,MAX_LINE-1); econd[MAX_LINE-1]='\0';
            int el=(int)strlen(econd);
            while (el>0&&(econd[el-1]==' '||econd[el-1]=='\t')) el--;
            if (el>=4&&strncmp(econd+el-4,"then",4)==0) {
                el-=4;
                while (el>0&&(econd[el-1]==' '||econd[el-1]=='\t'||econd[el-1]==';')) el--;
            }
            econd[el]='\0';
            if (el>0) {
                exec_line(econd);
                cond_true=(g_status==0);
                in_true=cond_true;
            }
            continue;
        }
        if (in_true) {
            /* Handle nested if/while/for in body */
            if (kw_match(tl,"if")) {
                char nbuf[MAX_SCRIPT]; int nboff=0;
                int nl=(int)strlen(line);
                memcpy(nbuf, line, (size_t)nl); nboff+=nl; nbuf[nboff++]='\n';
                extract_block(&p, "if", "fi", nbuf+nboff, MAX_SCRIPT-nboff);
                exec_if_block(nbuf);
            } else if (kw_match(tl,"while")||kw_match(tl,"for")) {
                const char *ow=kw_match(tl,"while")?"while":"for";
                char nbuf[MAX_SCRIPT]; int nboff=0;
                int nl2=(int)strlen(line);
                memcpy(nbuf, line, (size_t)nl2); nboff+=nl2; nbuf[nboff++]='\n';
                extract_block(&p, ow, "done", nbuf+nboff, MAX_SCRIPT-nboff);
                exec_script(nbuf);
            } else {
                exec_line(line);
            }
        }
    }
    return g_status;
}

/* ── while executor ───────────────────────────────────────────────────────── */
static int exec_while_block(const char *buf) {
    const char *orig=buf;
    char line[MAX_LINE];
    const char *p=orig;
    if (!next_bline(&p, line, MAX_LINE)) return 1;

    /* Extract "while COND; do" */
    const char *cs=ltrim(line)+6; /* skip "while " */
    char cond[MAX_LINE]; strncpy(cond,cs,MAX_LINE-1); cond[MAX_LINE-1]='\0';
    int cl=(int)strlen(cond);
    while (cl>0&&(cond[cl-1]==' '||cond[cl-1]=='\t')) cl--;
    if (cl>=2&&strncmp(cond+cl-2,"do",2)==0) {
        cl-=2;
        while (cl>0&&(cond[cl-1]==' '||cond[cl-1]=='\t'||cond[cl-1]==';')) cl--;
    }
    cond[cl]='\0';

    /* Collect body (remaining lines until "done") into body_buf */
    char body[MAX_SCRIPT]; body[0]='\0';
    int bo=0;
    while (next_bline(&p, line, MAX_LINE)) {
        const char *tl=ltrim(line);
        if (kw_match(tl,"done")) break;
        int ll=(int)strlen(line);
        if (bo+ll+2<MAX_SCRIPT) { memcpy(body+bo,line,(size_t)ll); bo+=ll; body[bo++]='\n'; }
    }
    body[bo]='\0';

    /* Execute: while cond; do body; done */
    for (int iter=0; iter<10000; iter++) {
        exec_line(cond);
        if (g_status!=0) break;
        exec_script(body);
    }
    return g_status;
}

/* ── for executor ─────────────────────────────────────────────────────────── */
static int exec_for_block(const char *buf) {
    const char *p=buf;
    char line[MAX_LINE];
    if (!next_bline(&p, line, MAX_LINE)) return 1;

    /* Parse "for VAR in WORD1 WORD2 ...; do" */
    const char *cs=ltrim(line)+4; /* skip "for " */
    /* extract VAR */
    char varname[64]; int vl=0;
    const char *cp=ltrim(cs);
    while (*cp && *cp!=' ' && *cp!='\t' && vl<63) varname[vl++]=*cp++;
    varname[vl]='\0';
    /* skip "in" */
    while (*cp==' '||*cp=='\t') cp++;
    if (strncmp(cp,"in",2)==0) cp+=2;
    while (*cp==' '||*cp=='\t') cp++;
    /* collect words until "; do" or "do" or end of line */
    char words_str[MAX_LINE]; strncpy(words_str,cp,MAX_LINE-1); words_str[MAX_LINE-1]='\0';
    /* strip "; do" suffix */
    int wl=(int)strlen(words_str);
    while (wl>0&&(words_str[wl-1]==' '||words_str[wl-1]=='\t')) wl--;
    if (wl>=2&&strncmp(words_str+wl-2,"do",2)==0) {
        wl-=2;
        while (wl>0&&(words_str[wl-1]==' '||words_str[wl-1]=='\t'||words_str[wl-1]==';')) wl--;
    }
    words_str[wl]='\0';

    /* Collect body */
    char body[MAX_SCRIPT]; int bo=0;
    while (next_bline(&p, line, MAX_LINE)) {
        if (kw_match(ltrim(line),"done")) break;
        int ll=(int)strlen(line);
        if (bo+ll+2<MAX_SCRIPT) { memcpy(body+bo,line,(size_t)ll); bo+=ll; body[bo++]='\n'; }
    }
    body[bo]='\0';

    /* Tokenize the word list (handles quoting + $VAR) */
    char *wargv[MAX_ARGS]; int wargc=tokenize_q(words_str, wargv, MAX_ARGS);

    for (int i=0; i<wargc; i++) {
        sv_set(varname, wargv[i]);
        exec_script(body);
    }
    return g_status;
}

/* ── exec_script: execute a buffer line by line ───────────────────────────── */
static int exec_script(const char *buf) {
    const char *p=buf;
    char line[MAX_LINE];
    const char *saved_heredoc_pos = g_script_heredoc_pos;  /* save for nesting */
    while (next_bline(&p, line, MAX_LINE)) {
        /* Point g_script_heredoc_pos at remaining buffer so collect_heredocs
         * can consume heredoc lines from it and update p accordingly. */
        g_script_heredoc_pos = p;
        const char *tl=ltrim(line);
        if (!*tl || *tl=='#') { p = g_script_heredoc_pos; continue; }
        if (kw_match(tl,"if")) {
            char nbuf[MAX_SCRIPT]; int nb=0;
            int ll=(int)strlen(line);
            memcpy(nbuf,line,(size_t)ll); nb+=ll; nbuf[nb++]='\n';
            extract_block(&p,"if","fi",nbuf+nb,MAX_SCRIPT-nb);
            g_script_heredoc_pos = p;
            exec_if_block(nbuf);
            p = g_script_heredoc_pos;
        } else if (kw_match(tl,"while")) {
            char nbuf[MAX_SCRIPT]; int nb=0;
            int ll=(int)strlen(line);
            memcpy(nbuf,line,(size_t)ll); nb+=ll; nbuf[nb++]='\n';
            extract_block(&p,"while","done",nbuf+nb,MAX_SCRIPT-nb);
            g_script_heredoc_pos = p;
            exec_while_block(nbuf);
            p = g_script_heredoc_pos;
        } else if (kw_match(tl,"for")) {
            char nbuf[MAX_SCRIPT]; int nb=0;
            int ll=(int)strlen(line);
            memcpy(nbuf,line,(size_t)ll); nb+=ll; nbuf[nb++]='\n';
            extract_block(&p,"for","done",nbuf+nb,MAX_SCRIPT-nb);
            g_script_heredoc_pos = p;
            exec_for_block(nbuf);
            p = g_script_heredoc_pos;
        } else {
            exec_line(line);
            /* Sync p: collect_heredocs may have advanced g_script_heredoc_pos */
            p = g_script_heredoc_pos;
        }
    }
    g_script_heredoc_pos = saved_heredoc_pos;
    return g_status;
}

/* ── exec_line: execute one logical line (compound commands) ─────────────── */
static int exec_line(const char *line) {
    const char *tl=ltrim(line);
    if (!*tl || *tl=='#') return g_status;

    cpd_t cpds[MAX_CMDS]; int nc=split_cpd(tl, cpds, MAX_CMDS);
    if (!nc) return g_status;

    int skip=0; /* 0=run, 1=skip */
    for (int i=0; i<nc; i++) {
        if (skip) {
            /* After &&: skip if last failed; after ||: skip if last succeeded */
            /* The skip flag is set based on the previous separator */
        } else {
            if (cpds[i].cmd[0]) {
                run_pipeline(cpds[i].cmd, cpds[i].bg);
            }
        }
        /* Determine skip for next */
        if (i<nc-1) {
            if (cpds[i].sep==SEP_AND)  skip=(g_status!=0);
            else if (cpds[i].sep==SEP_OR) skip=(g_status==0);
            else skip=0;
        }
    }
    return g_status;
}

/* ── Interactive: collect if/while/for block from stdin ─────────────────── */
static int collect_stdin_block(const char *first_line, const char *open_kw,
                                const char *close_kw, char *buf, int bufsz) {
    int off=0, depth=1;
    int fl=(int)strlen(first_line);
    if (off+fl+2>=bufsz) return -1;
    memcpy(buf+off,first_line,(size_t)fl); off+=fl; buf[off++]='\n';

    char line[MAX_LINE];
    while (depth>0) {
        write(1,"> ",2);
        int n=readline_raw(line,MAX_LINE);
        write(1,"\n",1);
        if (n<0) return -1;
        const char *tl=ltrim(line);
        if (kw_match(tl,open_kw))  depth++;
        if (kw_match(tl,close_kw)) depth--;
        int ll=(int)strlen(line);
        if (off+ll+2>=bufsz) return -1;
        memcpy(buf+off,line,(size_t)ll); off+=ll; buf[off++]='\n';
    }
    buf[off]='\0';
    return 0;
}

/* ── Main ─────────────────────────────────────────────────────────────────── */
int main(int argc, char *argv[], char *envp[]) {
    g_envp = envp;

    signal(SIGINT,  SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);

    setsid();
    shell_pgrp = getpid();
    setpgid(0, shell_pgrp);
    ioctl(0, TIOCSCTTY, 0);
    shell_take_terminal();

    /* -c CMD: execute one command and exit */
    if (argc >= 3 && strcmp(argv[1], "-c") == 0) {
        exec_script(argv[2]);
        exit(g_status);
    }

    /* Interactive login shell: run the system-wide profile. */
    source_file_if_present("/etc/profile");

    char line[MAX_LINE];
    for (;;) {
        /* Reap any finished background jobs silently; track stopped ones */
        {
            int new_n=0;
            for (int i=0; i<jobs_n; i++) {
                int st=0;
                int r=waitpid(jobs_list[i].pid, &st, 3); /* WNOHANG|WUNTRACED */
                if (r==0) {
                    jobs_list[new_n++]=jobs_list[i]; /* still running */
                } else if (r>0 && (st&0xff)==0x7f) {
                    /* stopped */
                    jobs_list[i].stopped=1;
                    printf("\n[%d]+ Stopped  %d\n", new_n+1, jobs_list[i].pid);
                    jobs_list[new_n++]=jobs_list[i];
                }
                /* else: exited — drop */
            }
            jobs_n=new_n;
        }

        write(1,"MaeroOS$ ",9);
        int n=readline_raw(line,MAX_LINE);
        write(1,"\n",1);
        if (n<0) { write(1,"exit\n",5); break; } /* EOF */
        if (!n) continue;

        const char *tl=ltrim(line);
        if (!*tl||*tl=='#') continue;

        /* Collect multi-line control structures */
        if (kw_match(tl,"if")) {
            char buf[MAX_SCRIPT];
            if (collect_stdin_block(line,"if","fi",buf,MAX_SCRIPT)==0)
                exec_if_block(buf);
        } else if (kw_match(tl,"while")) {
            char buf[MAX_SCRIPT];
            if (collect_stdin_block(line,"while","done",buf,MAX_SCRIPT)==0)
                exec_while_block(buf);
        } else if (kw_match(tl,"for")) {
            char buf[MAX_SCRIPT];
            if (collect_stdin_block(line,"for","done",buf,MAX_SCRIPT)==0)
                exec_for_block(buf);
        } else {
            exec_line(line);
        }
    }
    return 0;
}
